// The `unibind` module: what every isolate's interpreter imports before it runs
// anything, and where the backend's own types live.
//
// It is a multi-phase module (PEP 489) declared safe for a per-interpreter GIL,
// and every type in it is a heap type made by its exec slot: each isolate gets
// its own copy of every type object, because a static type is shared by the
// whole process and an interpreter with a GIL of its own may not touch one
// another interpreter can.
//
// A little of it is Python - the exception classes and the helpers for
// compiling a script and describing an exception - because that is the
// language those are naturally written in. It runs from a code object named
// "<unibind>", and frames from it are left out of every stack the backend
// reports.

#include <cstring>

#include "internal.h"

namespace ub::detail {

namespace {

// ---------------------------------------------------------------------------
// The Python half
// ---------------------------------------------------------------------------

constexpr const char* SUPPORT_SOURCE = R"PY(
import ast as _ast
import linecache as _linecache
import traceback as _traceback

class Error(Exception):
    """What `ub::ErrorKind::Error` throws."""
    __module__ = "unibind"

class RangeError(ValueError):
    """What `ub::ErrorKind::RangeError` throws. A ValueError, which is the
    Python name for the same complaint."""
    __module__ = "unibind"

class Thrown(Exception):
    """A value thrown by `ub::Throw` that is not an exception. `value` is the
    value; a `TryCatch` hands back the value, not this."""
    __module__ = "unibind"

    def __init__(self, value):
        super().__init__(value)
        self.value = value

    def __str__(self):
        return repr(self.value)

class Terminated(BaseException):
    """`ub::Isolate::TerminateExecution` unwinding the script. A BaseException,
    so `except Exception` does not see it - and re-raised at the next line if
    something catches it anyway."""
    __module__ = "unibind"

_FLAGS = _ast.PyCF_ALLOW_TOP_LEVEL_AWAIT
_CO_COROUTINE = 0x80

def compile_script(source, filename, line_offset):
    lines = source.splitlines(True)
    if line_offset > 0:
        lines = ["\n"] * line_offset + lines
    # Registered first, so that even a SyntaxError's traceback can quote it.
    _linecache.cache[filename] = (len(source), None, lines, filename)
    tree = compile(source, filename, "exec", _ast.PyCF_ONLY_AST | _FLAGS, dont_inherit=True)
    if line_offset:
        _ast.increment_lineno(tree, line_offset)
    tail = None
    if tree.body and isinstance(tree.body[-1], _ast.Expr):
        last = tree.body.pop()
        tail = compile(_ast.Expression(last.value), filename, "eval", _FLAGS, dont_inherit=True)
    body = compile(tree, filename, "exec", _FLAGS, dont_inherit=True)
    return body, tail

async def run_async_script(body, tail, g):
    ran = eval(body, g)
    if body.co_flags & _CO_COROUTINE:
        await ran
    if tail is None:
        return None
    value = eval(tail, g)
    if tail.co_flags & _CO_COROUTINE:
        value = await value
    return value

def _frames(e):
    return [f for f in _traceback.extract_tb(e.__traceback__) if f.filename != "<unibind>"]

def exception_message(e):
    return "".join(_traceback.format_exception_only(e)).strip().splitlines()[-1]

def exception_stack(e):
    te = _traceback.TracebackException.from_exception(e)
    te.stack = _traceback.StackSummary.from_list(
        [f for f in te.stack if f.filename != "<unibind>"])
    return "".join(te.format())

def exception_location(e):
    if isinstance(e, SyntaxError) and e.lineno:
        text = e.text.rstrip("\r\n") if e.text else None
        return (e.filename or "", e.lineno, e.offset or 0, text)
    frames = _frames(e)
    if not frames:
        return ("", 0, 0, None)
    f = frames[-1]
    line = _linecache.getline(f.filename, f.lineno)
    column = f.colno + 1 if getattr(f, "colno", None) is not None else 0
    return (f.filename, f.lineno, column, line.rstrip("\r\n") if line else None)
)PY";

// ---------------------------------------------------------------------------
// unibind.null
// ---------------------------------------------------------------------------

PyObject* NullRepr(PyObject* /*self*/) {
    return PyUnicode_FromString("null");
}

int NullBool(PyObject* /*self*/) {
    return 0;
}

PyType_Slot nullSlots[] = {
    {Py_tp_repr, reinterpret_cast<void*>(&NullRepr)},
    {Py_nb_bool, reinterpret_cast<void*>(&NullBool)},
    {Py_tp_doc,
     const_cast<char*>("The type of `unibind.null`, ub::Null: a value distinct from None, which is undefined.")},
    {0, nullptr},
};

PyType_Spec nullSpec = {
    "unibind.NullType",
    sizeof(PyObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE,
    nullSlots,
};

// ---------------------------------------------------------------------------
// unibind.Symbol
// ---------------------------------------------------------------------------

PyObject* SymbolNew(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static const char* keywords[] = {"description", nullptr};
    PyObject* description = Py_None;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "|O:Symbol", const_cast<char**>(keywords), &description) == 0) {
        return nullptr;
    }
    if (description != Py_None && !PyUnicode_Check(description)) {
        description = PyObject_Str(description);
        if (description == nullptr) {
            return nullptr;
        }
    } else {
        Py_INCREF(description);
    }
    auto* self = reinterpret_cast<SymbolObject*>(type->tp_alloc(type, 0));
    if (self == nullptr) {
        Py_DECREF(description);
        return nullptr;
    }
    self->description = description;
    self->dunder = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

/// A symbol is a GC object for one reason: the well-known ones live in the
/// `Symbol` type's own dictionary (`Symbol.iterator`), and each holds that
/// type. Without `tp_traverse` the collector cannot see the edge back to the
/// type, so the cycle outlives the interpreter - and a sub-interpreter that
/// ends with any block still allocated keeps every one of its arenas, some
/// megabytes, for the life of the process.
int SymbolTraverse(PyObject* object, visitproc visit, void* arg) {
    auto* self = reinterpret_cast<SymbolObject*>(object);
    Py_VISIT(Py_TYPE(object));
    Py_VISIT(self->description);
    Py_VISIT(self->dunder);
    return 0;
}

int SymbolClear(PyObject* object) {
    auto* self = reinterpret_cast<SymbolObject*>(object);
    Py_CLEAR(self->description);
    Py_CLEAR(self->dunder);
    return 0;
}

void SymbolDealloc(PyObject* object) {
    PyTypeObject* type = Py_TYPE(object);
    PyObject_GC_UnTrack(object);
    (void)SymbolClear(object);
    type->tp_free(object);
    Py_DECREF(type);
}

PyObject* SymbolRepr(PyObject* object) {
    auto* self = reinterpret_cast<SymbolObject*>(object);
    if (self->description == nullptr || self->description == Py_None) {
        return PyUnicode_FromString("Symbol()");
    }
    return PyUnicode_FromFormat("Symbol(%U)", self->description);
}

PyObject* SymbolGetDescription(PyObject* object, void* /*closure*/) {
    auto* self = reinterpret_cast<SymbolObject*>(object);
    return Py_NewRef(self->description != nullptr ? self->description : Py_None);
}

PyGetSetDef symbolGetSet[] = {
    {"description", &SymbolGetDescription, nullptr, "The symbol's description, or None.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyType_Slot symbolSlots[] = {
    {Py_tp_new, reinterpret_cast<void*>(&SymbolNew)},
    {Py_tp_dealloc, reinterpret_cast<void*>(&SymbolDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&SymbolTraverse)},
    {Py_tp_clear, reinterpret_cast<void*>(&SymbolClear)},
    {Py_tp_repr, reinterpret_cast<void*>(&SymbolRepr)},
    {Py_tp_getset, symbolGetSet},
    {Py_tp_doc, const_cast<char*>("A unique property key: ub::Symbol. Symbol(description=None).")},
    {0, nullptr},
};

PyType_Spec symbolSpec = {
    "unibind.Symbol", sizeof(SymbolObject), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, symbolSlots,
};

// ---------------------------------------------------------------------------
// unibind.External
// ---------------------------------------------------------------------------

PyObject* ExternalRepr(PyObject* /*self*/) {
    return PyUnicode_FromString("<unibind.External>");
}

PyType_Slot externalSlots[] = {
    {Py_tp_repr, reinterpret_cast<void*>(&ExternalRepr)},
    {Py_tp_doc, const_cast<char*>("An embedder pointer carried through script: ub::External. Opaque.")},
    {0, nullptr},
};

PyType_Spec externalSpec = {
    "unibind.External",
    sizeof(ExternalObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE,
    externalSlots,
};

// ---------------------------------------------------------------------------
// The module
// ---------------------------------------------------------------------------

PyObject* SymbolFor(PyObject* /*module*/, PyObject* key) {
    Isolate* isolate = CurrentIsolate();
    if (isolate == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "unibind: no isolate on this thread");
        return nullptr;
    }
    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "symbol_for() takes a str");
        return nullptr;
    }
    Types& types = isolate->impl().types;
    PyObject* existing = PyDict_GetItemWithError(types.symbolRegistry, key);
    if (existing != nullptr) {
        return Py_NewRef(existing);
    }
    if (PyErr_Occurred() != nullptr) {
        return nullptr;
    }
    PyObject* symbol = PyObject_CallOneArg(reinterpret_cast<PyObject*>(types.symbol), key);
    if (symbol == nullptr || PyDict_SetItem(types.symbolRegistry, key, symbol) != 0) {
        Py_XDECREF(symbol);
        return nullptr;
    }
    return symbol;
}

PyMethodDef moduleMethods[] = {
    {"symbol_for", &SymbolFor, METH_O, "Symbol.for(key): the one symbol registered under `key` in this isolate."},
    {nullptr, nullptr, 0, nullptr},
};

[[nodiscard]] bool AddType(PyObject* module, PyObject* type, const char* name) noexcept {
    return PyModule_AddObjectRef(module, name, type) == 0;
}

/// The well-known symbols, and the Python protocol each one stands for where
/// Python has one: a template's `Symbol.iterator` method *is* `__iter__`.
struct WellKnownSpec {
    const char* attribute;
    const char* description;
    const char* dunder;
};
constexpr WellKnownSpec WELL_KNOWN[5] = {
    {"iterator", "Symbol.iterator", "__iter__"},
    {"asyncIterator", "Symbol.asyncIterator", "__aiter__"},
    {"hasInstance", "Symbol.hasInstance", "__instancecheck__"},
    {"toPrimitive", "Symbol.toPrimitive", nullptr},
    {"toStringTag", "Symbol.toStringTag", nullptr},
};

int ModuleExec(PyObject* module) {
    Isolate* isolate = CurrentIsolate();
    if (isolate == nullptr) {
        PyErr_SetString(PyExc_ImportError,
                        "unibind is imported by the interpreters a ub::Isolate makes, and only there");
        return -1;
    }
    Types& types = isolate->impl().types;
    if (types.module != nullptr) {
        PyErr_SetString(PyExc_ImportError, "unibind: imported twice into one isolate");
        return -1;
    }
    types.module = Py_NewRef(module);

    types.null = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &nullSpec, nullptr));
    if (types.null == nullptr) {
        return -1;
    }
    types.nullValue = types.null->tp_alloc(types.null, 0);
    types.symbol = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &symbolSpec, nullptr));
    types.external = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &externalSpec, nullptr));
    types.symbolRegistry = PyDict_New();
    if (types.nullValue == nullptr || types.symbol == nullptr || types.external == nullptr ||
        types.symbolRegistry == nullptr) {
        return -1;
    }
    if (PyModule_AddObjectRef(module, "null", types.nullValue) != 0 ||
        !AddType(module, reinterpret_cast<PyObject*>(types.null), "NullType") ||
        !AddType(module, reinterpret_cast<PyObject*>(types.symbol), "Symbol") ||
        !AddType(module, reinterpret_cast<PyObject*>(types.external), "External")) {
        return -1;
    }

    for (int i = 0; i < 5; ++i) {
        PyObject* description = PyUnicode_FromString(WELL_KNOWN[i].description);
        if (description == nullptr) {
            return -1;
        }
        PyObject* symbol = PyObject_CallOneArg(reinterpret_cast<PyObject*>(types.symbol), description);
        Py_DECREF(description);
        if (symbol == nullptr) {
            return -1;
        }
        if (WELL_KNOWN[i].dunder != nullptr) {
            reinterpret_cast<SymbolObject*>(symbol)->dunder = PyUnicode_InternFromString(WELL_KNOWN[i].dunder);
        }
        types.wellKnown[i] = symbol;
        if (PyObject_SetAttrString(reinterpret_cast<PyObject*>(types.symbol), WELL_KNOWN[i].attribute, symbol) != 0) {
            return -1;
        }
    }

    // The Python half, in a namespace of its own.
    types.support = PyDict_New();
    if (types.support == nullptr) {
        return -1;
    }
    PyObject* builtins = PyImport_ImportModule("builtins");
    if (builtins == nullptr || PyDict_SetItemString(types.support, "__builtins__", builtins) != 0) {
        Py_XDECREF(builtins);
        return -1;
    }
    Py_DECREF(builtins);
    PyObject* code = Py_CompileString(SUPPORT_SOURCE, "<unibind>", Py_file_input);
    if (code == nullptr) {
        return -1;
    }
    PyObject* ran = PyEval_EvalCode(code, types.support, types.support);
    Py_DECREF(code);
    if (ran == nullptr) {
        return -1;
    }
    Py_DECREF(ran);

    const auto fetch = [&types](const char* name) -> PyObject* {
        PyObject* value = PyDict_GetItemString(types.support, name);  // borrowed
        return value == nullptr ? nullptr : Py_NewRef(value);
    };
    types.error = fetch("Error");
    types.rangeError = fetch("RangeError");
    types.thrown = fetch("Thrown");
    types.terminated = fetch("Terminated");
    if (types.error == nullptr || types.rangeError == nullptr || types.thrown == nullptr ||
        types.terminated == nullptr) {
        PyErr_SetString(PyExc_ImportError, "unibind: support code incomplete");
        return -1;
    }
    if (!AddType(module, types.error, "Error") || !AddType(module, types.rangeError, "RangeError") ||
        !AddType(module, types.thrown, "Thrown") || !AddType(module, types.terminated, "Terminated")) {
        return -1;
    }

    if (!InitObjectTypes(*isolate, module) || !InitBindingTypes(*isolate, module) ||
        !InitRuntimeTypes(*isolate, module) || !InitDataTypes(*isolate, module)) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_ImportError, "unibind: a type could not be made");
        }
        return -1;
    }
    return 0;
}

PyModuleDef_Slot moduleSlots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(&ModuleExec)},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {0, nullptr},
};

PyModuleDef moduleDef = {
    PyModuleDef_HEAD_INIT,
    "unibind",
    "The embedder's side of this interpreter: the types the ub:: API maps its values onto.",
    0,
    moduleMethods,
    moduleSlots,
    nullptr,
    nullptr,
    nullptr,
};

PyObject* InitModule() {
    return PyModuleDef_Init(&moduleDef);
}

}  // namespace

bool RegisterModule() noexcept {
    static bool registered = false;
    if (registered) {
        return true;
    }
    if (PyImport_AppendInittab("unibind", &InitModule) != 0) {
        return false;
    }
    registered = true;
    return true;
}

bool ImportModule(Isolate& isolate) noexcept {
    PyObject* module = PyImport_ImportModule("unibind");
    if (module == nullptr) {
        return false;
    }
    Py_DECREF(module);
    return isolate.impl().types.module != nullptr;
}

void ReleaseTypes(Isolate& isolate) noexcept {
    Types& types = isolate.impl().types;
    for (PyObject*& symbol : types.wellKnown) {
        Py_CLEAR(symbol);
    }
    Py_CLEAR(types.symbolRegistry);
    Py_CLEAR(types.support);
    Py_CLEAR(types.error);
    Py_CLEAR(types.rangeError);
    Py_CLEAR(types.thrown);
    Py_CLEAR(types.terminated);
    Py_CLEAR(types.nullValue);
    Py_CLEAR(types.null);
    Py_CLEAR(types.symbol);
    Py_CLEAR(types.external);
    Py_CLEAR(types.object);
    Py_CLEAR(types.function);
    Py_CLEAR(types.boundFunction);
    Py_CLEAR(types.promise);
    Py_CLEAR(types.typedArray);
    Py_CLEAR(types.dataView);
    Py_CLEAR(types.dataCloneError);
    Py_CLEAR(types.module);
}

}  // namespace ub::detail
