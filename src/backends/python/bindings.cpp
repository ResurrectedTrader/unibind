// Bindings: native functions and the call trampoline, the hooks a property
// access runs (accessors and interceptors), templates, and `Class<T>` with
// the native state its instances carry.
//
// The shapes, in the order a reader meets them:
//
//   * **A native function is a `unibind.NativeFunction`**: a vectorcall object
//     carrying the callback, its `CallbackData` or its script value, and
//     nothing the isolate has to keep - so a function made on every call costs
//     the isolate nothing once it is collected. Read as an attribute of a
//     `unibind.Object` it comes back bound to that object (a second small
//     type), which is how `o.method()` hands the callback `This()`.
//   * **A call is a frame whose first slots borrow the vectorcall arguments**
//     (docs/lifetimes.md section 8): no copying, no reference counting per
//     argument. The trampoline raises the isolate's native depth for the
//     length of the call and turns anything C++ throws into a Python
//     exception, because nothing may unwind through CPython's frames.
//   * **A template is an isolate-owned descriptor** (decision 13). What it
//     makes in a realm is a *type*: `FunctionTemplate::GetFunction` answers a
//     heap type derived from `unibind.Object`, per realm, whose instances are
//     what `new` - calling the type, from Python - makes. Its prototype object
//     is the type's `prototype` attribute and every instance's [[Prototype]];
//     its statics are type attributes. The per-realm cache is a dict *inside*
//     the realm's globals, so that the collector sees it: see `RealmCache`.
//   * **A class is a template whose instances carry a `NativeBox`**, recorded
//     in `Isolate::Impl::liveNatives` so that each box is given back exactly
//     once - by the instance's deallocation, or by `~Isolate` for survivors.

#include "bindings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ub::detail {

// ---------------------------------------------------------------------------
// Per-isolate state
// ---------------------------------------------------------------------------

struct BindingsState {
    /// Every template and class the isolate declared, for its whole life. The
    /// public handles are raw pointers into these.
    std::vector<std::unique_ptr<TemplateRec>> templates;
    std::vector<std::unique_ptr<ClassRec>> classes;
    /// Accessor records - a template's and `Object::SetAccessor`'s alike. A
    /// deque, because a property holds a pointer to its record and a vector's
    /// growth would move them.
    std::deque<CallbackRecord> records;

    /// The metaclass of every template's type, and the iterator adapter.
    /// Strong until `IsolateBindingsTeardown`, and null after it: a type or an
    /// adapter still alive then holds its own reference to these.
    PyTypeObject* templateMeta = nullptr;
    PyTypeObject* iteratorAdapter = nullptr;

    /// The innermost backend operation that was given a realm.
    RealmScope* realmScope = nullptr;
};

void DestroyBindingsState(BindingsState* state) noexcept {
    // After the interpreter is gone: nothing here may touch a Python object,
    // and nothing here holds one - the two type pointers were dropped at
    // teardown.
    delete state;
}

namespace {

[[nodiscard]] Isolate* IsolateOrRaise() noexcept {
    Isolate* isolate = CurrentIsolate();
    if (isolate == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "unibind: no isolate on this thread");
    }
    return isolate;
}

[[nodiscard]] BindingsState* StateOf(Isolate& isolate) noexcept {
    return isolate.impl().bindings.get();
}

/// Whether a value is a primitive - what a constructor may not answer with.
[[nodiscard]] bool IsPrimitiveValue(Isolate& isolate, PyObject* value) noexcept {
    return value == Py_None || IsNull(isolate, value) || PyBool_Check(value) || PyLong_Check(value) ||
           PyFloat_Check(value) || PyUnicode_Check(value) || IsSymbol(isolate, value);
}

/// A Number as `MakeNumber` makes one: an int when it is integral and safe,
/// so script can index and `range` with it, a float otherwise.
[[nodiscard]] PyObject* NumberObject(double value) noexcept {
    if (std::isfinite(value) && std::trunc(value) == value && std::fabs(value) <= 9007199254740992.0 &&
        !(value == 0 && std::signbit(value))) {
        return PyLong_FromDouble(value);
    }
    return PyFloat_FromDouble(value);
}

}  // namespace

// ---------------------------------------------------------------------------
// Realms
// ---------------------------------------------------------------------------

RealmScope::RealmScope(Isolate& isolate, ContextRec* rec) noexcept
    : state(StateOf(isolate)), rec(rec), frame(PyEval_GetFrame()) {
    if (state != nullptr) {
        previous = state->realmScope;
        state->realmScope = this;
    }
}

RealmScope::~RealmScope() {
    if (state != nullptr) {
        state->realmScope = previous;
    }
}

ContextRec* CallingRealm(Isolate& isolate, ContextRec* fallback) noexcept {
    BindingsState* state = StateOf(isolate);
    RealmScope* scope = state != nullptr ? state->realmScope : nullptr;
    PyFrameObject* frame = PyEval_GetFrame();
    // No Python code since the operation began: the operation's realm.
    if (scope != nullptr && scope->frame == frame) {
        return scope->rec;
    }
    // Python code called us: the realm whose globals it runs in.
    if (frame != nullptr) {
        PyObject* globals = PyFrame_GetGlobals(frame);
        ContextRec* rec = RealmOfGlobals(isolate, globals);
        Py_XDECREF(globals);
        if (rec != nullptr) {
            return rec;
        }
    }
    if (isolate.impl().entered != nullptr) {
        return isolate.impl().entered;
    }
    if (scope != nullptr) {
        return scope->rec;
    }
    if (fallback != nullptr) {
        return fallback;
    }
    // Code with no realm of its own at all - a job run with nothing entered,
    // say. Any realm is better than none: a callback's `GetContext()` is not
    // allowed to be empty.
    const auto& realms = isolate.impl().realms;
    return realms.empty() ? nullptr : realms.begin()->second;
}

// ---------------------------------------------------------------------------
// The call state
// ---------------------------------------------------------------------------

NativeCall::NativeCall(Isolate& isolate, PyObject* const* args, std::uint32_t argc, ContextRec* realm) noexcept
    : impl_(isolate.impl()), saved_(isolate.impl().current), frame_(isolate, saved_, args, argc) {
    impl_.current = &frame_;
    ++impl_.nativeDepth;
    state_.owner = &isolate;
    state_.frame = &frame_;
    state_.argc = argc;
    if (realm != nullptr) {
        state_.context = Context::FromRec(realm);
    }
}

NativeCall::~NativeCall() {
    Py_CLEAR(state_.result);
    assert(impl_.current == &frame_ && "a HandleScope opened in a callback outlived the callback");
    --impl_.nativeDepth;
    impl_.current = saved_;
}

PyObject* NativeCall::TakeResult() noexcept {
    return std::exchange(state_.result, nullptr);
}

namespace {

[[nodiscard]] Slot UndefinedSlot(const CallbackState& state) noexcept {
    if (state.undefinedSlot == Frame::NO_SLOT) {
        state.undefinedSlot = state.frame->Push(Py_NewRef(Py_None));
        if (state.undefinedSlot == Frame::NO_SLOT) {
            return NoSlot();
        }
    }
    return MakeSlot(*state.frame, state.undefinedSlot);
}

void StoreResult(const CallbackState& state, PyObject* value) noexcept {
    if (value == nullptr) {
        return;  // could not be made; the exception says why
    }
    Py_XSETREF(state.result, value);
}

}  // namespace

Isolate& CallbackIsolate(const CallbackState& state) noexcept {
    return *state.owner;
}

const Context& CallbackContext(const CallbackState& state) noexcept {
    return state.context;
}

std::uint32_t CallbackArgumentCount(const CallbackState& state) noexcept {
    return state.argc;
}

Slot CallbackArgument(const CallbackState& state, std::uint32_t index) noexcept {
    if (index < state.argc) {
        return MakeSlot(*state.frame, index);
    }
    return UndefinedSlot(state);
}

Slot CallbackThis(const CallbackState& state) noexcept {
    return SlotOrEmpty(*state.frame, state.thisSlot);
}

Slot CallbackHolder(const CallbackState& state) noexcept {
    return SlotOrEmpty(*state.frame, state.holderSlot);
}

bool CallbackIsConstruct(const CallbackState& state) noexcept {
    return state.isConstruct;
}

CallbackData CallbackDataOf(const CallbackState& state) noexcept {
    return state.data;
}

Slot CallbackValueData(const CallbackState& state) noexcept {
    if (!state.hasValue) {
        return UndefinedSlot(state);
    }
    return SlotOrEmpty(*state.frame, state.valueSlot);
}

void SetReturnSlot(const CallbackState& state, Slot value) noexcept {
    if (value.IsEmpty()) {
        return;
    }
    StoreResult(state, Py_NewRef(Resolve(value)));
}

void SetReturnUndefined(const CallbackState& state) noexcept {
    StoreResult(state, Py_NewRef(Py_None));
}

void SetReturnNull(const CallbackState& state) noexcept {
    StoreResult(state, Py_NewRef(state.owner->impl().types.nullValue));
}

void SetReturnBoolean(const CallbackState& state, bool value) noexcept {
    StoreResult(state, Py_NewRef(value ? Py_True : Py_False));
}

void SetReturnNumber(const CallbackState& state, double value) noexcept {
    StoreResult(state, NumberObject(value));
}

void SetReturnInteger(const CallbackState& state, std::int32_t value) noexcept {
    StoreResult(state, PyLong_FromLong(value));
}

// ---------------------------------------------------------------------------
// unibind.NativeFunction
// ---------------------------------------------------------------------------

namespace {

struct FunctionObject {
    PyObject_HEAD vectorcallfunc vectorcall;
    FunctionCallback callback;
    CallbackData data;
    /// `Function::New` with a script value: held here and nowhere else, and
    /// visited by the collector through this object, so it lives exactly as
    /// long as the function does (backend.h, `MakeFunctionWithValue`).
    PyObject* value;
    PyObject* name;  ///< str, or null for an anonymous function
    PyObject* weaklist;
    /// The realm the function was made in, for a call that arrives with no
    /// realm of its own. Not a reference - a function stored in its realm's
    /// globals would otherwise keep that realm alive through a cycle the
    /// collector cannot see (see `RealmCache`) - and so checked against the
    /// isolate's live realms, by its globals, before it is used.
    ContextRec* realm;
    PyObject* realmGlobals;
};

struct BoundObject {
    PyObject_HEAD vectorcallfunc vectorcall;
    PyObject* function;  ///< a FunctionObject
    PyObject* receiver;
    PyObject* weaklist;
};

[[nodiscard]] FunctionObject* AsFunction(PyObject* object) noexcept {
    return reinterpret_cast<FunctionObject*>(object);
}

[[nodiscard]] BoundObject* AsBound(PyObject* object) noexcept {
    return reinterpret_cast<BoundObject*>(object);
}

/// The receiver a callback sees as `This()`: always an object, as
/// `unibind/function.h` promises - a call with no receiver, or with None or
/// null, gets the realm's global object, which is its globals dict. Any
/// other value is already an object in Python and is passed as itself.
[[nodiscard]] PyObject* ReceiverFor(Isolate& isolate, PyObject* receiver, ContextRec* realm) noexcept {
    if (receiver == nullptr || receiver == Py_None || IsNull(isolate, receiver)) {
        return Py_NewRef(realm != nullptr ? realm->globals : Py_None);
    }
    return Py_NewRef(receiver);
}

/// The trampoline every native function call goes through.
[[nodiscard]] PyObject* CallNative(Isolate& isolate, FunctionObject* function, PyObject* receiver,
                                   PyObject* const* args, std::size_t argc) noexcept {
    ContextRec* own = function->realm != nullptr && RealmOfGlobals(isolate, function->realmGlobals) == function->realm
                          ? function->realm
                          : nullptr;
    ContextRec* realm = CallingRealm(isolate, own);
    // Copied out before the call: the callback may drop the last reference to
    // the function it is running as.
    const FunctionCallback callback = function->callback;
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return nullptr;
    }
    PyObject* value = Py_XNewRef(function->value);

    NativeCall call(isolate, args, static_cast<std::uint32_t>(argc), realm);
    call.state().thisSlot = call.Push(ReceiverFor(isolate, receiver, realm));
    call.state().holderSlot = call.state().thisSlot;
    call.state().data = function->data;
    if (value != nullptr) {
        call.state().valueSlot = call.Push(value);
        call.state().hasValue = true;
    }
    Shielded([&] { callback(CallbackInfo(call.state())); });
    // A stop requested while the native ran - which it may have seen and
    // returned for - is enforced here, on the way back into script. The eval
    // breaker would get there too, but only once the requesting thread has
    // queued the pending call, and a native that returns the instant it sees
    // the flag can beat that (unibind/isolate.h: the script stops when it
    // returns).
    if (PyErr_Occurred() == nullptr && Terminating(isolate)) {
        RaiseStop(isolate);
    }
    if (PyErr_Occurred() != nullptr) {
        return nullptr;
    }
    PyObject* result = call.TakeResult();
    return result != nullptr ? result : Py_NewRef(Py_None);
}

[[nodiscard]] bool RefuseKeywords(PyObject* kwnames) noexcept {
    if (kwnames != nullptr && PyTuple_GET_SIZE(kwnames) > 0) {
        PyErr_SetString(PyExc_TypeError, "a native function takes no keyword arguments");
        return false;
    }
    return true;
}

PyObject* FunctionVectorcall(PyObject* callable, PyObject* const* args, std::size_t nargsf, PyObject* kwnames) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr || !RefuseKeywords(kwnames)) {
        return nullptr;
    }
    return CallNative(*isolate, AsFunction(callable), nullptr, args,
                      static_cast<std::size_t>(PyVectorcall_NARGS(nargsf)));
}

PyObject* BoundVectorcall(PyObject* callable, PyObject* const* args, std::size_t nargsf, PyObject* kwnames) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr || !RefuseKeywords(kwnames)) {
        return nullptr;
    }
    BoundObject* bound = AsBound(callable);
    // Held across the call: the callback may rebind the attribute it was read
    // from, and this object with it.
    PyObject* function = Py_NewRef(bound->function);
    PyObject* receiver = Py_NewRef(bound->receiver);
    PyObject* result = CallNative(*isolate, AsFunction(function), receiver, args,
                                  static_cast<std::size_t>(PyVectorcall_NARGS(nargsf)));
    Py_DECREF(function);
    Py_DECREF(receiver);
    return result;
}

void FunctionDealloc(PyObject* self) {
    FunctionObject* function = AsFunction(self);
    PyTypeObject* type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    if (function->weaklist != nullptr) {
        PyObject_ClearWeakRefs(self);
    }
    Py_CLEAR(function->value);
    Py_CLEAR(function->name);
    type->tp_free(self);
    Py_DECREF(type);
}

int FunctionTraverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(AsFunction(self)->value);
    return 0;
}

int FunctionClear(PyObject* self) {
    Py_CLEAR(AsFunction(self)->value);
    return 0;
}

PyObject* FunctionRepr(PyObject* self) {
    PyObject* name = AsFunction(self)->name;
    if (name == nullptr || PyUnicode_GET_LENGTH(name) == 0) {
        return PyUnicode_FromString("<native function>");
    }
    return PyUnicode_FromFormat("<native function %U>", name);
}

/// A native function stored on a class is a method of its instances, as a
/// JavaScript function on a prototype is: `instance.f()` hands it `This()`.
PyObject* FunctionDescrGet(PyObject* self, PyObject* object, PyObject* /*type*/) {
    if (object == nullptr || object == Py_None) {
        return Py_NewRef(self);
    }
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    return BindFunction(*isolate, self, object);
}

PyObject* FunctionGetName(PyObject* self, void* /*closure*/) {
    PyObject* name = AsFunction(self)->name;
    return name != nullptr ? Py_NewRef(name) : PyUnicode_FromString("");
}

PyGetSetDef functionGetSet[] = {
    {"__name__", &FunctionGetName, nullptr, "The name the function was declared with, or ''.", nullptr},
    {"__qualname__", &FunctionGetName, nullptr, "As __name__.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMemberDef functionMembers[] = {
    {"__vectorcalloffset__", Py_T_PYSSIZET, offsetof(FunctionObject, vectorcall), Py_READONLY, nullptr},
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(FunctionObject, weaklist), Py_READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr},
};

PyType_Slot functionSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(&FunctionDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&FunctionTraverse)},
    {Py_tp_clear, reinterpret_cast<void*>(&FunctionClear)},
    {Py_tp_call, reinterpret_cast<void*>(&PyVectorcall_Call)},
    {Py_tp_repr, reinterpret_cast<void*>(&FunctionRepr)},
    {Py_tp_descr_get, reinterpret_cast<void*>(&FunctionDescrGet)},
    {Py_tp_getset, functionGetSet},
    {Py_tp_members, functionMembers},
    {Py_tp_doc, const_cast<char*>("A function whose body is native: ub::Function::New, or a template's method. "
                                  "Callable; not a constructor.")},
    {0, nullptr},
};

PyType_Spec functionSpec = {
    "unibind.NativeFunction",
    sizeof(FunctionObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_DISALLOW_INSTANTIATION |
        Py_TPFLAGS_IMMUTABLETYPE,
    functionSlots,
};

void BoundDealloc(PyObject* self) {
    BoundObject* bound = AsBound(self);
    PyTypeObject* type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    if (bound->weaklist != nullptr) {
        PyObject_ClearWeakRefs(self);
    }
    Py_CLEAR(bound->function);
    Py_CLEAR(bound->receiver);
    type->tp_free(self);
    Py_DECREF(type);
}

int BoundTraverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(AsBound(self)->function);
    Py_VISIT(AsBound(self)->receiver);
    return 0;
}

PyObject* BoundRepr(PyObject* self) {
    PyObject* name = AsFunction(AsBound(self)->function)->name;
    const char* type = Py_TYPE(AsBound(self)->receiver)->tp_name;
    if (name == nullptr) {
        return PyUnicode_FromFormat("<bound native function of %s>", type);
    }
    return PyUnicode_FromFormat("<bound native function %U of %s>", name, type);
}

PyObject* BoundGetSelf(PyObject* self, void* /*closure*/) {
    return Py_NewRef(AsBound(self)->receiver);
}

PyObject* BoundGetFunc(PyObject* self, void* /*closure*/) {
    return Py_NewRef(AsBound(self)->function);
}

PyObject* BoundGetName(PyObject* self, void* closure) {
    return FunctionGetName(AsBound(self)->function, closure);
}

/// Equal when they bind one function to one receiver, as a Python bound
/// method is - `o.m == o.m` holds although each read makes a new one.
PyObject* BoundCompare(PyObject* self, PyObject* other, int op) {
    if ((op != Py_EQ && op != Py_NE) || Py_TYPE(other) != Py_TYPE(self)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    const bool same =
        AsBound(self)->function == AsBound(other)->function && AsBound(self)->receiver == AsBound(other)->receiver;
    return PyBool_FromLong((op == Py_EQ) == same ? 1 : 0);
}

Py_hash_t BoundHash(PyObject* self) {
    const Py_hash_t function = PyObject_Hash(AsBound(self)->function);
    const Py_hash_t receiver = Py_HashPointer(AsBound(self)->receiver);
    const Py_hash_t combined = function ^ (receiver * 31);
    return combined == -1 ? -2 : combined;
}

PyGetSetDef boundGetSet[] = {
    {"__self__", &BoundGetSelf, nullptr, "The receiver: This() in the callback.", nullptr},
    {"__func__", &BoundGetFunc, nullptr, "The native function.", nullptr},
    {"__name__", &BoundGetName, nullptr, "The function's name.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMemberDef boundMembers[] = {
    {"__vectorcalloffset__", Py_T_PYSSIZET, offsetof(BoundObject, vectorcall), Py_READONLY, nullptr},
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(BoundObject, weaklist), Py_READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr},
};

PyType_Slot boundSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(&BoundDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&BoundTraverse)},
    {Py_tp_call, reinterpret_cast<void*>(&PyVectorcall_Call)},
    {Py_tp_repr, reinterpret_cast<void*>(&BoundRepr)},
    {Py_tp_richcompare, reinterpret_cast<void*>(&BoundCompare)},
    {Py_tp_hash, reinterpret_cast<void*>(&BoundHash)},
    {Py_tp_getset, boundGetSet},
    {Py_tp_members, boundMembers},
    {Py_tp_doc, const_cast<char*>("A native function bound to a receiver: what `o.method` reads.")},
    {0, nullptr},
};

PyType_Spec boundSpec = {
    "unibind.BoundNativeFunction",
    sizeof(BoundObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_DISALLOW_INSTANTIATION |
        Py_TPFLAGS_IMMUTABLETYPE,
    boundSlots,
};

/// A new native function. `value` is borrowed; `name` may be empty.
[[nodiscard]] PyObject* NewFunction(Isolate& isolate, ContextRec* realm, FunctionCallback callback, CallbackData data,
                                    PyObject* value, std::string_view name) noexcept {
    PyTypeObject* type = isolate.impl().types.function;
    PyObject* raw = type->tp_alloc(type, 0);
    if (raw == nullptr) {
        return nullptr;
    }
    FunctionObject* function = AsFunction(raw);
    function->vectorcall = &FunctionVectorcall;
    function->callback = callback;
    function->data = data;
    function->value = Py_XNewRef(value);
    function->realm = realm;
    function->realmGlobals = realm != nullptr ? realm->globals : nullptr;
    if (!name.empty()) {
        function->name = TextString(name);
        if (function->name == nullptr) {
            Py_DECREF(raw);
            return nullptr;
        }
    }
    return raw;
}

}  // namespace

bool IsNativeFunction(Isolate& isolate, PyObject* value) noexcept {
    return Py_TYPE(value) == isolate.impl().types.function;
}

PyObject* BindFunction(Isolate& isolate, PyObject* function, PyObject* receiver) noexcept {
    PyTypeObject* type = isolate.impl().types.boundFunction;
    PyObject* raw = type->tp_alloc(type, 0);
    if (raw == nullptr) {
        return nullptr;
    }
    BoundObject* bound = AsBound(raw);
    bound->vectorcall = &BoundVectorcall;
    bound->function = Py_NewRef(function);
    bound->receiver = Py_NewRef(receiver);
    return raw;
}

PyObject* CallWithReceiver(Isolate& isolate, PyObject* callable, PyObject* receiver, PyObject* const* args,
                           std::size_t argc) noexcept {
    if (IsNativeFunction(isolate, callable)) {
        return CallNative(isolate, AsFunction(callable), receiver, args, argc);
    }
    return PyObject_Vectorcall(callable, args, argc, nullptr);
}

// ---------------------------------------------------------------------------
// Accessors and interceptors
//
// Each hook runs as a native call of its own - a frame, the native depth, a
// callback state - with the receiver as `This()` and the object carrying the
// hook as `Holder()`. CPython gives a lookup both, so both are real: an
// accessor inherited from a prototype sees the instance it was read through
// and the prototype it was found on.
// ---------------------------------------------------------------------------

namespace {

/// The key as a callback's `Local<Name>`: a string or a symbol. An index key
/// reaching a named hook (an accessor's name, say) is the digits it spells.
[[nodiscard]] PyObject* NameOf(Isolate& isolate, PyObject* key) noexcept {
    if (PyUnicode_Check(key) || IsSymbol(isolate, key)) {
        return Py_NewRef(key);
    }
    return PyObject_Str(key);
}

/// A hook call's frame and state, with `This()` and `Holder()` filled in.
class HookCall {
   public:
    HookCall(Isolate& isolate, PyObject* receiver, PyObject* holder, CallbackData data) noexcept
        : call_(isolate, nullptr, 0, CallingRealm(isolate)) {
        call_.state().thisSlot = call_.Push(Py_NewRef(receiver));
        call_.state().holderSlot = holder == receiver ? call_.state().thisSlot : call_.Push(Py_NewRef(holder));
        call_.state().data = data;
    }

    /// A value as a handle in the hook's frame: empty if the frame could not
    /// grow, never a stand-in value.
    template <class T>
    [[nodiscard]] Local<T> Handle(PyObject* value) noexcept {
        if (value == nullptr) {
            return Local<T>::FromSlot(Slot{});
        }
        return Local<T>::FromSlot(SlotOrEmpty(call_.frame(), call_.Push(value)));
    }

    [[nodiscard]] PropertyCallbackInfo Info() const noexcept { return PropertyCallbackInfo(call_.state()); }
    [[nodiscard]] PyObject* Result() noexcept {
        PyObject* result = call_.TakeResult();
        return result != nullptr ? result : Py_NewRef(Py_None);
    }

   private:
    NativeCall call_;
};

[[nodiscard]] bool AsIndex(PyObject* key, std::uint32_t* index) noexcept {
    // A normalized key is an int only when it is a canonical array index.
    if (!PyLong_Check(key)) {
        return false;
    }
    *index = static_cast<std::uint32_t>(PyLong_AsUnsignedLong(key));
    return true;
}

}  // namespace

CallbackRecord* StoreAccessorRecord(Isolate& isolate, AccessorGetterCallback getter, AccessorSetterCallback setter,
                                    CallbackData data) noexcept {
    try {
        return &StateOf(isolate)->records.emplace_back(CallbackRecord{getter, setter, data});
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
        return nullptr;
    }
}

PyObject* RunAccessorGetter(Isolate& isolate, const CallbackRecord& record, PyObject* key, PyObject* receiver,
                            PyObject* holder) noexcept {
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return nullptr;
    }
    HookCall call(isolate, receiver, holder, record.data);
    const Local<Name> name = call.Handle<Name>(NameOf(isolate, key));
    if (PyErr_Occurred() != nullptr) {
        return nullptr;
    }
    Shielded([&] { record.getter(name, call.Info()); });
    if (PyErr_Occurred() != nullptr) {
        return nullptr;
    }
    return call.Result();
}

bool RunAccessorSetter(Isolate& isolate, const CallbackRecord& record, PyObject* key, PyObject* value,
                       PyObject* receiver, PyObject* holder) noexcept {
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return false;
    }
    HookCall call(isolate, receiver, holder, record.data);
    const Local<Name> name = call.Handle<Name>(NameOf(isolate, key));
    const Local<Value> incoming = call.Handle<Value>(Py_NewRef(value));
    if (PyErr_Occurred() != nullptr) {
        return false;
    }
    // A setter's result is not read: the language does not have one.
    Shielded([&] { record.setter(name, incoming, call.Info()); });
    return PyErr_Occurred() == nullptr;
}

bool HasHandlerFor(const TemplateRec* shape, PyObject* key) noexcept {
    return PyLong_Check(key) ? shape->hasIndexed : shape->hasNamed;
}

Hook InterceptGet(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* receiver, PyObject* holder,
                  PyObject** out) noexcept {
    *out = nullptr;
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(key, &index);
    if (isIndex ? shape->indexed.getter == nullptr : shape->named.getter == nullptr) {
        return Hook::Declined;
    }
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return Hook::Failed;
    }
    HookCall call(isolate, receiver, holder, isIndex ? shape->indexed.data : shape->named.data);
    Intercepted answer = Intercepted::No;
    if (isIndex) {
        Shielded([&] { answer = shape->indexed.getter(index, call.Info()); });
    } else {
        const Local<Name> name = call.Handle<Name>(NameOf(isolate, key));
        if (PyErr_Occurred() != nullptr) {
            return Hook::Failed;
        }
        Shielded([&] { answer = shape->named.getter(name, call.Info()); });
    }
    // A hook that throws has intercepted the access, whatever it answered
    // (unibind/template.h): the ordinary property underneath does not answer.
    if (PyErr_Occurred() != nullptr) {
        return Hook::Failed;
    }
    if (answer == Intercepted::No) {
        return Hook::Declined;
    }
    *out = call.Result();
    return Hook::Handled;
}

Hook InterceptSet(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* value, PyObject* receiver) noexcept {
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(key, &index);
    if (isIndex ? shape->indexed.setter == nullptr : shape->named.setter == nullptr) {
        return Hook::Declined;
    }
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return Hook::Failed;
    }
    HookCall call(isolate, receiver, receiver, isIndex ? shape->indexed.data : shape->named.data);
    const Local<Value> incoming = call.Handle<Value>(Py_NewRef(value));
    Intercepted answer = Intercepted::No;
    if (isIndex) {
        Shielded([&] { answer = shape->indexed.setter(index, incoming, call.Info()); });
    } else {
        const Local<Name> name = call.Handle<Name>(NameOf(isolate, key));
        if (PyErr_Occurred() != nullptr) {
            return Hook::Failed;
        }
        Shielded([&] { answer = shape->named.setter(name, incoming, call.Info()); });
    }
    // Whatever the setter wrote through its return value is discarded with
    // the call: its answer is `answer` (unibind/function.h).
    if (PyErr_Occurred() != nullptr) {
        return Hook::Failed;
    }
    return answer == Intercepted::Yes ? Hook::Handled : Hook::Declined;
}

Hook InterceptQuery(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* receiver,
                    PropertyAttribute* out) noexcept {
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(key, &index);
    if (isIndex ? shape->indexed.query == nullptr : shape->named.query == nullptr) {
        return Hook::Declined;
    }
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return Hook::Failed;
    }
    HookCall call(isolate, receiver, receiver, isIndex ? shape->indexed.data : shape->named.data);
    std::optional<PropertyAttribute> answer;
    if (isIndex) {
        Shielded([&] { answer = shape->indexed.query(index, call.Info()); });
    } else {
        const Local<Name> name = call.Handle<Name>(NameOf(isolate, key));
        if (PyErr_Occurred() != nullptr) {
            return Hook::Failed;
        }
        Shielded([&] { answer = shape->named.query(name, call.Info()); });
    }
    if (PyErr_Occurred() != nullptr) {
        return Hook::Failed;
    }
    if (!answer) {
        return Hook::Declined;
    }
    *out = *answer;
    return Hook::Handled;
}

Hook InterceptDelete(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* receiver, bool* out) noexcept {
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(key, &index);
    if (isIndex ? shape->indexed.deleter == nullptr : shape->named.deleter == nullptr) {
        return Hook::Declined;
    }
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return Hook::Failed;
    }
    HookCall call(isolate, receiver, receiver, isIndex ? shape->indexed.data : shape->named.data);
    std::optional<bool> answer;
    if (isIndex) {
        Shielded([&] { answer = shape->indexed.deleter(index, call.Info()); });
    } else {
        const Local<Name> name = call.Handle<Name>(NameOf(isolate, key));
        if (PyErr_Occurred() != nullptr) {
            return Hook::Failed;
        }
        Shielded([&] { answer = shape->named.deleter(name, call.Info()); });
    }
    if (PyErr_Occurred() != nullptr) {
        return Hook::Failed;
    }
    if (!answer) {
        return Hook::Declined;
    }
    *out = *answer;
    return Hook::Handled;
}

bool InterceptEnumerate(Isolate& isolate, TemplateRec* shape, PyObject* receiver, PyObject* keys) noexcept {
    for (int half = 0; half < 2; ++half) {
        const bool named = half == 0;
        if (named ? !(shape->hasNamed && shape->named.enumerator != nullptr)
                  : !(shape->hasIndexed && shape->indexed.enumerator != nullptr)) {
            continue;
        }
        // Native recursion never passes CPython's own stack check; the
        // stack itself is the limit (IsolateOptions::stackLimitBytes).
        if (StackExhausted(isolate)) {
            return false;
        }
        HookCall call(isolate, receiver, receiver, named ? shape->named.data : shape->indexed.data);
        std::optional<Local<Array>> listed;
        Shielded(
            [&] { listed = named ? shape->named.enumerator(call.Info()) : shape->indexed.enumerator(call.Info()); });
        if (PyErr_Occurred() != nullptr) {
            return false;
        }
        // Empty, or an optional holding an empty handle - an array the hook
        // could not make - both mean "no own keys here" (unibind/template.h).
        // Read while the hook's frame, which roots the array, is still open.
        if (!listed || listed->IsEmpty()) {
            continue;
        }
        PyObject* array = PySequence_Fast(Resolve(listed->slot()), "an enumerator answers with an array");
        if (array == nullptr) {
            return false;
        }
        for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(array); ++i) {
            PyObject* key = NormalizeKey(isolate, PySequence_Fast_GET_ITEM(array, i));
            if (key == nullptr || PyList_Append(keys, key) != 0) {
                Py_XDECREF(key);
                Py_DECREF(array);
                return false;
            }
            Py_DECREF(key);
        }
        Py_DECREF(array);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The iterator adapter
//
// An embedder's `Symbol.iterator` method answers what it would answer in
// JavaScript: an object with a `next()` method that answers `{done, value}`.
// Python iterates with `__next__` and StopIteration. This type is the
// translation, so that `for x in instance` and `list(instance)` work on a
// class whose iterator was written for the other language.
// ---------------------------------------------------------------------------

namespace {

struct AdapterObject {
    PyObject_HEAD PyObject* iterator;
};

void AdapterDealloc(PyObject* self) {
    PyTypeObject* type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    Py_CLEAR(reinterpret_cast<AdapterObject*>(self)->iterator);
    type->tp_free(self);
    Py_DECREF(type);
}

int AdapterTraverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(reinterpret_cast<AdapterObject*>(self)->iterator);
    return 0;
}

int AdapterClear(PyObject* self) {
    Py_CLEAR(reinterpret_cast<AdapterObject*>(self)->iterator);
    return 0;
}

[[nodiscard]] PyObject* Field(Isolate& isolate, PyObject* object, const char* name, bool* found) noexcept {
    PyObject* key = PyUnicode_InternFromString(name);
    if (key == nullptr) {
        return nullptr;
    }
    PyObject* value = GetAny(isolate, object, key, found);
    Py_DECREF(key);
    return value;
}

PyObject* AdapterNext(PyObject* self) {
    Isolate* isolate = IsolateOrRaise();
    PyObject* iterator = reinterpret_cast<AdapterObject*>(self)->iterator;
    if (isolate == nullptr || iterator == nullptr) {
        return nullptr;
    }
    Py_INCREF(iterator);
    bool found = false;
    PyObject* next = Field(*isolate, iterator, "next", &found);
    PyObject* step = nullptr;
    if (next != nullptr && (!found || PyCallable_Check(next) == 0)) {
        PyErr_SetString(PyExc_TypeError, "the iterator has no next() method");
    } else if (next != nullptr) {
        step = CallWithReceiver(*isolate, next, iterator, nullptr, 0);
    }
    Py_XDECREF(next);
    Py_DECREF(iterator);
    if (step == nullptr) {
        return nullptr;
    }
    PyObject* done = Field(*isolate, step, "done", &found);
    const int finished = done != nullptr ? PyObject_IsTrue(done) : -1;
    Py_XDECREF(done);
    PyObject* value = nullptr;
    if (finished == 0) {
        value = Field(*isolate, step, "value", &found);
    } else if (finished > 0) {
        // Exhausted: drop the JavaScript iterator, so that a second `next()`
        // answers StopIteration again without asking it.
        Py_CLEAR(reinterpret_cast<AdapterObject*>(self)->iterator);
    }
    Py_DECREF(step);
    return value;  // null with nothing pending is StopIteration
}

PyType_Slot adapterSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(&AdapterDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&AdapterTraverse)},
    {Py_tp_clear, reinterpret_cast<void*>(&AdapterClear)},
    {Py_tp_iter, reinterpret_cast<void*>(&PyObject_SelfIter)},
    {Py_tp_iternext, reinterpret_cast<void*>(&AdapterNext)},
    {Py_tp_doc, const_cast<char*>("A Python iterator over a JavaScript-style one: next() answering {done, value}.")},
    {0, nullptr},
};

PyType_Spec adapterSpec = {
    "unibind.IteratorAdapter",
    sizeof(AdapterObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE,
    adapterSlots,
};

}  // namespace

PyObject* AdaptIterator(Isolate& isolate, PyObject* produced) noexcept {
    if (PyIter_Check(produced) != 0) {
        return produced;
    }
    bool found = false;
    PyObject* next = Field(isolate, produced, "next", &found);
    if (next == nullptr) {
        Py_DECREF(produced);
        return nullptr;
    }
    const bool javascriptStyle = found && PyCallable_Check(next) != 0;
    Py_DECREF(next);
    PyTypeObject* type = StateOf(isolate) != nullptr ? StateOf(isolate)->iteratorAdapter : nullptr;
    if (!javascriptStyle || type == nullptr) {
        PyObject* iterator = PyObject_GetIter(produced);
        Py_DECREF(produced);
        return iterator;
    }
    PyObject* adapter = type->tp_alloc(type, 0);
    if (adapter == nullptr) {
        Py_DECREF(produced);
        return nullptr;
    }
    reinterpret_cast<AdapterObject*>(adapter)->iterator = produced;
    return adapter;
}

// ---------------------------------------------------------------------------
// Template types
//
// What a function template materialises into is a heap type whose metaclass -
// `unibind.TemplateType`, a subclass of `type` - carries one extra field: the
// template it was made from. A Python subclass of such a type is made by the
// same metaclass with that field empty, which is how `TemplateOfType` finds
// the nearest template along a subclass's bases.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* CACHE_KEY = "__unibind_templates__";

struct TemplateTypeObject {
    PyHeapTypeObject heap;
    TemplateRec* tpl;
};

/// The declaration a static of `type`'s own template makes under `name`, if
/// any: what decides whether assigning or deleting it is allowed.
[[nodiscard]] const TemplateEntry* StaticNamed(PyObject* type, PyObject* name) noexcept {
    TemplateRec* tpl = reinterpret_cast<TemplateTypeObject*>(type)->tpl;
    if (tpl == nullptr || !PyUnicode_Check(name)) {
        return nullptr;
    }
    Py_ssize_t size = 0;
    const char* text = PyUnicode_AsUTF8AndSize(name, &size);
    if (text == nullptr) {
        PyErr_Clear();
        return nullptr;
    }
    const std::string_view wanted(text, static_cast<std::size_t>(size));
    const TemplateEntry* found = nullptr;
    for (const TemplateEntry& entry : tpl->entries) {
        if (entry.kind != TemplateEntry::Kind::SymbolMethod && entry.name == wanted) {
            found = &entry;  // the last declaration of a name is the one installed
        }
    }
    return found;
}

/// A static declared ReadOnly - `Class<T>::StaticValue`'s default - refuses an
/// assignment from Python, and a DontDelete one a `del`, as the properties of
/// a `unibind.Object` do. Everything else on the type is Python's to change.
int TemplateTypeSetAttr(PyObject* type, PyObject* name, PyObject* value) {
    if (const TemplateEntry* entry = StaticNamed(type, name); entry != nullptr) {
        const PropertyAttribute refused =
            value != nullptr ? PropertyAttribute::ReadOnly : PropertyAttribute::DontDelete;
        if (HasAttribute(entry->attributes, refused)) {
            PyErr_Format(PyExc_TypeError,
                         value != nullptr ? "Cannot assign to read only property '%U' of %s"
                                          : "Cannot delete property '%U' of %s",
                         name, reinterpret_cast<PyTypeObject*>(type)->tp_name);
            return -1;
        }
    }
    return PyType_Type.tp_setattro(type, name, value);
}

PyType_Slot templateTypeSlots[] = {
    {Py_tp_setattro, reinterpret_cast<void*>(&TemplateTypeSetAttr)},
    {Py_tp_doc, const_cast<char*>("The metaclass of every type a unibind template or class makes.")},
    {0, nullptr},
};

PyType_Spec templateTypeSpec = {
    "unibind.TemplateType",
    sizeof(TemplateTypeObject),
    0,
    // Collected as any type is: `type`'s traverse and clear, and its GC flag,
    // are inherited. The field added here is no reference.
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    templateTypeSlots,
};

/// Mark `rec`, and everything instantiating it instantiates with it, as
/// instantiated - the same closure the other two backends seal, so a late
/// shape call is ignored on all three at the same moment.
void Seal(TemplateRec* rec) noexcept {
    if (rec == nullptr || rec->sealed) {
        return;
    }
    rec->sealed = true;
    Seal(rec->instanceOf);
    Seal(rec->prototypeOf);
    Seal(rec->parent);
    Seal(rec->prototype);
    Seal(rec->instance);
    for (const TemplateEntry& entry : rec->entries) {
        if (entry.kind == TemplateEntry::Kind::Child) {
            Seal(entry.child);
        }
    }
}

/// The realm's template cache: a dict from `PyLong(TemplateRec*)` to the type
/// the template materialised into there. Borrowed; null with an exception
/// pending if it could not be made.
///
/// It lives in the realm's globals, under a key of the backend's own, rather
/// than in `ContextRec::templateCache`. The reason is the collector: a cache
/// the record holds is a reference nothing can see, and it closes a cycle -
/// globals, their realm capsule, the record, the cache, a type, its prototype,
/// a function script stored there, that function's `__globals__` - that no
/// collection can ever break, so the realm would live until its isolate did.
/// Inside the globals the same references are all visible, and the cache goes
/// with its realm like everything else in it.
[[nodiscard]] PyObject* RealmCache(ContextRec* realm) noexcept {
    PyObject* cache = PyDict_GetItemString(realm->globals, CACHE_KEY);  // borrowed
    if (cache != nullptr && PyDict_Check(cache)) {
        return cache;
    }
    cache = PyDict_New();
    if (cache == nullptr) {
        return nullptr;
    }
    const int stored = PyDict_SetItemString(realm->globals, CACHE_KEY, cache);
    Py_DECREF(cache);  // the globals hold it now
    return stored == 0 ? cache : nullptr;
}

[[nodiscard]] PyObject* ConstantValue(Isolate& isolate, const TemplateEntry& entry) noexcept {
    switch (entry.constant.GetKind()) {
        case Constant::Kind::Undefined:
            return Py_NewRef(Py_None);
        case Constant::Kind::Null:
            return Py_NewRef(isolate.impl().types.nullValue);
        case Constant::Kind::Boolean:
            return Py_NewRef(entry.constant.AsBoolean() ? Py_True : Py_False);
        case Constant::Kind::Number:
            return NumberObject(entry.constant.AsNumber());
        case Constant::Kind::Integer:
            return PyLong_FromLong(entry.constant.AsInteger());
        case Constant::Kind::String:
            return TextString(entry.text);
    }
    return Py_NewRef(Py_None);
}

[[nodiscard]] const char* SymbolMethodName(WellKnownSymbol key) noexcept {
    switch (key) {
        case WellKnownSymbol::Iterator:
            return "[Symbol.iterator]";
        case WellKnownSymbol::AsyncIterator:
            return "[Symbol.asyncIterator]";
        case WellKnownSymbol::HasInstance:
            return "[Symbol.hasInstance]";
        case WellKnownSymbol::ToPrimitive:
            return "[Symbol.toPrimitive]";
        case WellKnownSymbol::ToStringTag:
            return "[Symbol.toStringTag]";
    }
    return "";
}

[[nodiscard]] PyObject* Materialise(Isolate& isolate, ContextRec* realm, TemplateRec* tpl) noexcept;
[[nodiscard]] ObjectInstance* NewTemplateInstance(Isolate& isolate, ContextRec* realm, TemplateRec* tpl,
                                                  PyTypeObject* type) noexcept;

/// The value one declaration installs, and the key it installs it under -
/// both new references - or false with the exception pending. An accessor has
/// no value: its record is the entry's own.
[[nodiscard]] bool EntryValue(Isolate& isolate, ContextRec* realm, const TemplateEntry& entry, PyObject** key,
                              PyObject** value) noexcept {
    *key = nullptr;
    *value = nullptr;
    if (entry.kind == TemplateEntry::Kind::SymbolMethod) {
        PyObject* symbol = isolate.impl().types.wellKnown[static_cast<int>(entry.symbolKey)];
        *key = symbol != nullptr ? NormalizeKey(isolate, symbol) : nullptr;
    } else {
        PyObject* text = TextString(entry.name);
        *key = text != nullptr ? NormalizeKey(isolate, text) : nullptr;
        Py_XDECREF(text);
    }
    if (*key == nullptr) {
        return false;
    }
    switch (entry.kind) {
        case TemplateEntry::Kind::Constant:
            *value = ConstantValue(isolate, entry);
            break;
        case TemplateEntry::Kind::Method:
            *value = NewFunction(isolate, realm, entry.callback, entry.data, nullptr, entry.name);
            break;
        case TemplateEntry::Kind::SymbolMethod:
            *value =
                NewFunction(isolate, realm, entry.callback, entry.data, nullptr, SymbolMethodName(entry.symbolKey));
            break;
        case TemplateEntry::Kind::Accessor:
            return true;
        case TemplateEntry::Kind::Child:
            // A function template set as a property is its constructor in this
            // realm; an object template, a whole instance of it, handler and
            // all, as `NewInstance` would make it.
            *value = entry.child->isFunction
                         ? Materialise(isolate, realm, entry.child)
                         : reinterpret_cast<PyObject*>(NewTemplateInstance(isolate, realm, entry.child, nullptr));
            break;
    }
    if (*value == nullptr) {
        Py_CLEAR(*key);
        return false;
    }
    return true;
}

/// Replay a template's declarations onto an object, as data and accessor
/// properties with the declared attributes. Index loop: a declaration made
/// from a callback this runs may add to the vector.
[[nodiscard]] bool ApplyEntries(Isolate& isolate, ContextRec* realm, ObjectInstance* target,
                                TemplateRec* shape) noexcept {
    if (shape == nullptr) {
        return true;
    }
    // NOLINTNEXTLINE(modernize-loop-convert): the vector may grow under the loop - see above
    for (std::size_t i = 0; i < shape->entries.size(); ++i) {
        const TemplateEntry& entry = shape->entries[i];
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        if (!EntryValue(isolate, realm, entry, &key, &value)) {
            return false;
        }
        bool ok = false;
        if (entry.kind == TemplateEntry::Kind::Accessor) {
            ok = DefineAccessorRaw(target, key, entry.record, entry.attributes);
        } else {
            const PropertyAttribute attributes =
                entry.kind == TemplateEntry::Kind::SymbolMethod ? PropertyAttribute::DontEnum : entry.attributes;
            ok = DefineRaw(target, key, value, static_cast<long>(attributes));
        }
        Py_XDECREF(value);
        Py_DECREF(key);
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// A function template's own declarations - its statics - onto its type, as
/// type attributes: what `Widget.KIND` reads from Python. An accessor declared
/// there is not installed - a type attribute that ran native code on read would
/// have to be a descriptor on the metaclass, shared by every template's type -
/// and neither is a symbol method with no Python protocol to stand for.
[[nodiscard]] bool ApplyStatics(Isolate& isolate, ContextRec* realm, PyObject* type, TemplateRec* tpl) noexcept {
    // NOLINTNEXTLINE(modernize-loop-convert): an index loop, as in ApplyEntries and for its reason
    for (std::size_t i = 0; i < tpl->entries.size(); ++i) {
        const TemplateEntry& entry = tpl->entries[i];
        if (entry.kind == TemplateEntry::Kind::Accessor) {
            continue;
        }
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        if (!EntryValue(isolate, realm, entry, &key, &value)) {
            return false;
        }
        PyObject* name = PyUnicode_Check(key) ? Py_NewRef(key) : nullptr;
        if (name == nullptr && entry.kind != TemplateEntry::Kind::SymbolMethod) {
            name = TextString(entry.name);  // an index-like name, as the text it was declared as
        }
        const bool ok = name == nullptr ? PyErr_Occurred() == nullptr : PyType_Type.tp_setattro(type, name, value) == 0;
        Py_XDECREF(name);
        Py_DECREF(value);
        Py_DECREF(key);
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// The prototype's native methods, mirrored onto the type as class attributes.
///
/// The prototype object is where instances find them, and stays the only
/// place ordinary lookup finds them. But `super()` - `super().increment(by)`
/// in a Python subclass that overrides `increment` - searches the class
/// dictionaries along the MRO and never looks at a prototype, so without this
/// a subclass could override a native method and never reach it again. A
/// native function binds like a method when it is found on a class, so the
/// mirrored entry gives `super()` exactly the bound method it expects.
///
/// What was mirrored is recorded under `MIRRORED_KEY`, so that the ordinary
/// lookup can tell a mirror from a real class attribute and ignore it: an
/// instance whose prototype was swapped away must not keep the old methods
/// through the back door (`MirroredOnly`). A static of the same name keeps
/// the class attribute, and dunder names are not mirrored at all - the type
/// slot for `__iter__` would otherwise call the native directly and skip the
/// adapter that lets Python iterate a JavaScript-style iterator.
[[nodiscard]] bool MirrorPrototypeMethods(Isolate& isolate, ObjectInstance* prototype, PyObject* type) noexcept {
    PyObject* dict = reinterpret_cast<PyTypeObject*>(type)->tp_dict;
    if (prototype->properties == nullptr || dict == nullptr) {
        return true;
    }
    PyObject* mirrored = PySet_New(nullptr);
    if (mirrored == nullptr) {
        return false;
    }
    Py_ssize_t position = 0;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    bool ok = true;
    while (ok && PyDict_Next(prototype->properties, &position, &key, &value) != 0) {
        if (!PyUnicode_Check(key) || !IsNativeFunction(isolate, value) ||
            PyUnicode_CompareWithASCIIString(key, "constructor") == 0 ||
            (PyUnicode_GET_LENGTH(key) > 4 && PyUnicode_READ_CHAR(key, 0) == '_' &&
             PyUnicode_READ_CHAR(key, 1) == '_')) {
            continue;
        }
        const int present = PyDict_Contains(dict, key);
        ok = present >= 0 &&
             (present == 1 || (PyType_Type.tp_setattro(type, key, value) == 0 && PySet_Add(mirrored, key) == 0));
    }
    PyObject* name = ok ? PyUnicode_InternFromString(MIRRORED_KEY) : nullptr;
    ok = ok && name != nullptr && PyType_Type.tp_setattro(type, name, mirrored) == 0;
    Py_XDECREF(name);
    Py_DECREF(mirrored);
    return ok;
}

/// The `prototype` a template's type hands its instances, or null if it has
/// been replaced with something that cannot be one.
[[nodiscard]] PyObject* PrototypeOf(Isolate& isolate, PyObject* type) noexcept {
    PyObject* prototype = PyObject_GetAttrString(type, "prototype");
    if (prototype == nullptr) {
        PyErr_Clear();
        return nullptr;
    }
    if (!IsObjectInstance(isolate, prototype)) {
        Py_DECREF(prototype);
        return nullptr;
    }
    return prototype;
}

/// The type `tpl` makes in `realm`, made the first time it is asked for. New
/// reference, or null with the exception pending.
PyObject* Materialise(Isolate& isolate, ContextRec* realm, TemplateRec* tpl) noexcept {
    Seal(tpl);
    Types& types = isolate.impl().types;
    BindingsState* state = StateOf(isolate);
    if (realm == nullptr || state == nullptr || state->templateMeta == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "unibind: a template can only be instantiated into a live realm");
        return nullptr;
    }
    PyObject* cache = RealmCache(realm);
    PyObject* key = cache != nullptr ? PyLong_FromVoidPtr(tpl) : nullptr;
    if (key == nullptr) {
        return nullptr;
    }
    if (PyObject* cached = PyDict_GetItemWithError(cache, key); cached != nullptr || PyErr_Occurred() != nullptr) {
        Py_DECREF(key);
        return Py_XNewRef(cached);
    }

    // The parent first, so this type can derive from its type and this
    // prototype inherit from its prototype. V8 chains only the prototypes; a
    // Python type that did not also derive from the parent's would answer
    // `isinstance(child, Parent)` wrongly, so here the types are chained too,
    // and a static declared on the parent is visible on the child as a Python
    // class attribute is - the one visible difference.
    PyObject* base = tpl->parent != nullptr ? Materialise(isolate, realm, tpl->parent)
                                            : Py_NewRef(reinterpret_cast<PyObject*>(types.object));
    if (base == nullptr) {
        Py_DECREF(key);
        return nullptr;
    }
    PyObject* parentPrototype = tpl->parent != nullptr ? PrototypeOf(isolate, base) : nullptr;

    // `unibind.<name>`: the type's `__module__` is `unibind` and its `__name__`
    // the class name - which an unnamed template leaves empty, as V8 leaves a
    // template function's `name`.
    PyObject* type = nullptr;
    try {
        const std::string qualified = "unibind." + tpl->className;
        PyType_Slot slots[] = {{0, nullptr}};
        // Layout, slots and collection all inherited from the base.
        PyType_Spec spec = {qualified.c_str(), 0, 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, slots};
        PyObject* bases = PyTuple_Pack(1, base);
        if (bases != nullptr) {
            type = PyType_FromMetaclass(state->templateMeta, types.module, &spec, bases);
            Py_DECREF(bases);
        }
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
    }
    Py_DECREF(base);
    if (type == nullptr) {
        Py_XDECREF(parentPrototype);
        Py_DECREF(key);
        return nullptr;
    }
    reinterpret_cast<TemplateTypeObject*>(type)->tpl = tpl;

    ObjectInstance* prototype = NewObjectInstance(isolate, types.object, parentPrototype);
    Py_XDECREF(parentPrototype);
    bool ok = prototype != nullptr;
    if (ok) {
        // The prototype template's handlers, if it declares any, answer for
        // the prototype object; `ShapeOf` reads it off this.
        prototype->tpl = tpl->prototype;
    }
    // Cached before the declarations are applied: a template that mentions
    // itself - a static holding its own constructor - would otherwise recurse.
    ok = ok && PyDict_SetItem(cache, key, type) == 0;
    PyObject* prototypeName = ok ? PyUnicode_InternFromString("prototype") : nullptr;
    PyObject* constructorName = ok ? PyUnicode_InternFromString("constructor") : nullptr;
    ok = ok && prototypeName != nullptr && constructorName != nullptr &&
         PyType_Type.tp_setattro(type, prototypeName, reinterpret_cast<PyObject*>(prototype)) == 0 &&
         DefineRaw(prototype, constructorName, type, static_cast<long>(PropertyAttribute::DontEnum)) &&
         ApplyEntries(isolate, realm, prototype, tpl->prototype) && ApplyStatics(isolate, realm, type, tpl) &&
         MirrorPrototypeMethods(isolate, prototype, type);
    Py_XDECREF(prototypeName);
    Py_XDECREF(constructorName);
    Py_XDECREF(prototype);
    if (!ok) {
        // A type half made is not left behind for the next caller to find.
        PyObject* pending = PyErr_GetRaisedException();
        if (PyDict_DelItem(cache, key) != 0) {
            PyErr_Clear();
        }
        PyErr_SetRaisedException(pending);
        Py_DECREF(type);
        Py_DECREF(key);
        return nullptr;
    }
    Py_DECREF(key);
    return type;
}

/// A fresh instance of what `tpl` describes, stamped with its declarations and
/// carrying no native yet: a function template's instance (with its instance
/// template's members, and its prototype), or an object template's plain
/// object. `type`, if given, is the type to make it as - a Python subclass of
/// the template's type, when script subclassed it.
ObjectInstance* NewTemplateInstance(Isolate& isolate, ContextRec* realm, TemplateRec* tpl,
                                    PyTypeObject* type) noexcept {
    Seal(tpl);
    TemplateRec* constructor = tpl->isFunction ? tpl : tpl->instanceOf;
    TemplateRec* shape = tpl->isFunction ? tpl->instance : tpl;

    PyObject* madeAs = nullptr;
    if (type != nullptr) {
        madeAs = Py_NewRef(reinterpret_cast<PyObject*>(type));
    } else if (constructor != nullptr) {
        madeAs = Materialise(isolate, realm, constructor);
    } else {
        madeAs = Py_NewRef(reinterpret_cast<PyObject*>(isolate.impl().types.object));
    }
    if (madeAs == nullptr) {
        return nullptr;
    }
    // The type's `prototype` as it is now - the language's rule for `new`,
    // and what makes a Python subclass's instances inherit what its class
    // inherits.
    PyObject* prototype = constructor != nullptr ? PrototypeOf(isolate, madeAs) : nullptr;
    ObjectInstance* instance = NewObjectInstance(isolate, reinterpret_cast<PyTypeObject*>(madeAs), prototype);
    Py_XDECREF(prototype);
    Py_DECREF(madeAs);
    if (instance == nullptr) {
        return nullptr;
    }
    instance->tpl = constructor != nullptr ? constructor : shape;
    if (!ApplyEntries(isolate, realm, instance, shape)) {
        Py_DECREF(instance);
        return nullptr;
    }
    return instance;
}

/// Give `box` to `instance`: recorded in the isolate's list first, so that a
/// failure publishes nothing and the caller still owns the box.
[[nodiscard]] bool AttachNative(Isolate& isolate, ObjectInstance* instance, NativeBox* box) noexcept {
    try {
        isolate.impl().liveNatives.insert(box);
    } catch (const std::bad_alloc&) {
        return false;
    }
    instance->box = box;
    return true;
}

void DestroyBox(NativeBox* box) noexcept {
    if (box != nullptr && box->destroy != nullptr) {
        box->destroy(box);
    }
}

/// A plain call of a template's type, from C++ - Python has no way to call a
/// type other than constructing. A `FunctionTemplate`'s callback runs with
/// `IsConstructCall()` false and its answer is the call's; a class refuses,
/// unless it opted in with `ConstructOrCall`, when it makes an instance.
[[nodiscard]] PyObject* CallTemplate(Isolate& isolate, PyTypeObject* type, TemplateRec* tpl, PyObject* receiver,
                                     PyObject* const* args, std::size_t argc) noexcept {
    if (ClassRec* owner = tpl->ownerClass; owner != nullptr) {
        if (!owner->callableWithoutNew) {
            PyErr_Format(PyExc_TypeError, "Class constructor %s cannot be invoked without 'new'", owner->name.c_str());
            return nullptr;
        }
        // The same path as `new`, and what comes out is an instance; only
        // `IsConstructCall()` tells the callback which spelling it was.
        return ConstructTemplate(isolate, type, tpl, args, argc, /*isConstruct=*/false);
    }
    if (tpl->callback == nullptr) {
        // What a plain call to a template with nothing to run does is left
        // unspecified (unibind/template.h); undefined is the least surprising.
        return Py_NewRef(Py_None);
    }
    ContextRec* realm = CallingRealm(isolate);
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return nullptr;
    }
    NativeCall call(isolate, args, static_cast<std::uint32_t>(argc), realm);
    call.state().thisSlot = call.Push(ReceiverFor(isolate, receiver, realm));
    call.state().holderSlot = call.state().thisSlot;
    call.state().data = tpl->callbackData;
    const FunctionCallback callback = tpl->callback;
    Shielded([&] { callback(CallbackInfo(call.state())); });
    // A stop requested while the native ran - which it may have seen and
    // returned for - is enforced here, on the way back into script. The eval
    // breaker would get there too, but only once the requesting thread has
    // queued the pending call, and a native that returns the instant it sees
    // the flag can beat that (unibind/isolate.h: the script stops when it
    // returns).
    if (PyErr_Occurred() == nullptr && Terminating(isolate)) {
        RaiseStop(isolate);
    }
    if (PyErr_Occurred() != nullptr) {
        return nullptr;
    }
    PyObject* result = call.TakeResult();
    return result != nullptr ? result : Py_NewRef(Py_None);
}

}  // namespace

TemplateRec* TemplateOfType(Isolate& isolate, PyTypeObject* type) noexcept {
    BindingsState* state = StateOf(isolate);
    PyTypeObject* meta = state != nullptr ? state->templateMeta : nullptr;
    if (meta == nullptr) {
        return nullptr;
    }
    for (PyTypeObject* walk = type; walk != nullptr; walk = walk->tp_base) {
        if (PyObject_TypeCheck(reinterpret_cast<PyObject*>(walk), meta)) {
            if (TemplateRec* tpl = reinterpret_cast<TemplateTypeObject*>(walk)->tpl; tpl != nullptr) {
                return tpl;
            }
        }
    }
    return nullptr;
}

bool IsTemplateMadeType(Isolate& isolate, PyTypeObject* type) noexcept {
    BindingsState* state = StateOf(isolate);
    PyTypeObject* meta = state != nullptr ? state->templateMeta : nullptr;
    return meta != nullptr && PyObject_TypeCheck(reinterpret_cast<PyObject*>(type), meta) &&
           reinterpret_cast<TemplateTypeObject*>(type)->tpl != nullptr;
}

/// `new Type(...)`, for a template's type or a Python subclass of one.
///
/// The instance is made first and the callback runs with it as `This()`, so a
/// constructor that writes to its receiver writes to what `new` returns. A
/// `FunctionTemplate` callback that answers with an object replaces the
/// instance, as a JavaScript constructor returning one does; a class's answer
/// is discarded, because what comes out of a `Class<T>` must carry a `T`.
PyObject* ConstructTemplate(Isolate& isolate, PyTypeObject* type, TemplateRec* tpl, PyObject* const* args,
                            std::size_t argc, bool isConstruct) noexcept {
    // Native recursion never passes CPython's own stack check; the
    // stack itself is the limit (IsolateOptions::stackLimitBytes).
    if (StackExhausted(isolate)) {
        return nullptr;
    }
    ClassRec* owner = tpl->ownerClass;
    if (owner != nullptr && owner->constructor == nullptr) {
        PyErr_Format(PyExc_TypeError, "%s cannot be constructed from script", owner->name.c_str());
        return nullptr;
    }
    ContextRec* realm = CallingRealm(isolate);
    ObjectInstance* instance = NewTemplateInstance(isolate, realm, tpl, type);
    if (instance == nullptr) {
        return nullptr;
    }
    auto* self = reinterpret_cast<PyObject*>(instance);

    if (owner != nullptr) {
        NativeBox* box = nullptr;
        {
            NativeCall call(isolate, args, static_cast<std::uint32_t>(argc), realm);
            call.state().thisSlot = call.Push(Py_NewRef(self));
            call.state().holderSlot = call.state().thisSlot;
            call.state().isConstruct = isConstruct;
            const NativeConstructor constructor = owner->constructor;
            Shielded([&] { box = constructor(CallbackInfo(call.state())); });
        }
        if (box == nullptr || PyErr_Occurred() != nullptr) {
            // A constructor that threw, or declined without saying why - which
            // gets a reason, because no instance without a native may reach
            // script (unibind/class.h).
            DestroyBox(box);
            if (PyErr_Occurred() == nullptr) {
                RaiseError(isolate, ErrorKind::Error, "constructor declined");
            }
            Py_DECREF(self);
            return nullptr;
        }
        if (!AttachNative(isolate, instance, box)) {
            DestroyBox(box);
            Py_DECREF(self);
            return PyErr_NoMemory();
        }
        return self;
    }

    if (tpl->callback != nullptr) {
        PyObject* answer = nullptr;
        {
            NativeCall call(isolate, args, static_cast<std::uint32_t>(argc), realm);
            call.state().thisSlot = call.Push(Py_NewRef(self));
            call.state().holderSlot = call.state().thisSlot;
            call.state().data = tpl->callbackData;
            call.state().isConstruct = isConstruct;
            const FunctionCallback callback = tpl->callback;
            Shielded([&] { callback(CallbackInfo(call.state())); });
            answer = call.TakeResult();
        }
        if (PyErr_Occurred() != nullptr) {
            Py_XDECREF(answer);
            Py_DECREF(self);
            return nullptr;
        }
        if (answer != nullptr && !IsPrimitiveValue(isolate, answer)) {
            Py_DECREF(self);
            return answer;
        }
        Py_XDECREF(answer);
    }
    return self;
}

// ---------------------------------------------------------------------------
// Functions and calls
// ---------------------------------------------------------------------------

std::optional<Slot> MakeFunction(const Context& context, FunctionCallback callback, CallbackData data) {
    Isolate& isolate = OwnerOf(context);
    return PushOrNothing(isolate, NewFunction(isolate, context.rec(), callback, data, nullptr, {}));
}

std::optional<Slot> MakeFunctionWithValue(const Context& context, FunctionCallback callback, Slot data) {
    Isolate& isolate = OwnerOf(context);
    return PushOrNothing(isolate, NewFunction(isolate, context.rec(), callback, CallbackData{}, Resolve(data), {}));
}

namespace {

/// The arguments of a C++ call as the vector a vectorcall takes. Borrowed:
/// every one is rooted by the caller's handle.
[[nodiscard]] std::vector<PyObject*> Arguments(std::span<const Slot> arguments) {
    std::vector<PyObject*> out;
    out.reserve(arguments.size());
    for (const Slot& argument : arguments) {
        out.push_back(Resolve(argument));
    }
    return out;
}

}  // namespace

std::optional<Slot> CallFunction(const Context& context, Slot function, Slot receiver,
                                 std::span<const Slot> arguments) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    const RealmScope realm(isolate, context.rec());
    std::vector<PyObject*> args;
    try {
        args = Arguments(arguments);
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
        return std::nullopt;
    }
    PyObject* callable = Resolve(function);
    PyObject* self = receiver.IsEmpty() ? nullptr : Resolve(receiver);
    PyObject* result = nullptr;
    if (IsNativeFunction(isolate, callable)) {
        result = CallNative(isolate, AsFunction(callable), self, args.data(), args.size());
    } else if (TemplateRec* tpl = PyType_Check(callable) != 0
                                      ? TemplateOfType(isolate, reinterpret_cast<PyTypeObject*>(callable))
                                      : nullptr;
               tpl != nullptr) {
        result = CallTemplate(isolate, reinterpret_cast<PyTypeObject*>(callable), tpl, self, args.data(), args.size());
    } else {
        // A Python callable has no receiver to be given: a method is already
        // bound to its own, and a function takes what it is passed. So the
        // receiver is dropped, rather than slipped in as a first argument the
        // callable did not ask for. A bound native function - which is not a
        // type, so never the branch above - is the same case: bound is bound,
        // and the receiver it carries wins over the one passed, as for a
        // JavaScript bound function.
        result = PyObject_Vectorcall(callable, args.data(), args.size(), nullptr);
    }
    return PushOrNothing(isolate, result);
}

std::optional<Slot> ConstructObject(const Context& context, Slot function, std::span<const Slot> arguments) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    const RealmScope realm(isolate, context.rec());
    PyObject* callable = Resolve(function);
    // A type is what Python constructs with: a template's, a class's, or one
    // script declared. A native function is not a constructor (decision 9),
    // and neither is any other callable - a Python function has no `new`.
    if (PyType_Check(callable) == 0) {
        PyErr_Format(PyExc_TypeError, "%s is not a constructor", Py_TYPE(callable)->tp_name);
        return std::nullopt;
    }
    std::vector<PyObject*> args;
    try {
        args = Arguments(arguments);
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
        return std::nullopt;
    }
    return PushOrNothing(isolate, PyObject_Vectorcall(callable, args.data(), args.size(), nullptr));
}

// ---------------------------------------------------------------------------
// The template half of the backend interface
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] TemplateRec* NewTemplate(Isolate& isolate, bool isFunction) {
    BindingsState* state = StateOf(isolate);
    auto owned = std::make_unique<TemplateRec>();
    TemplateRec* raw = owned.get();
    raw->owner = &isolate;
    raw->isFunction = isFunction;
    state->templates.push_back(std::move(owned));
    return raw;
}

}  // namespace

TemplateRec* NewObjectTemplate(Isolate& isolate) {
    return NewTemplate(isolate, false);
}

TemplateRec* NewFunctionTemplate(Isolate& isolate, FunctionCallback callback, CallbackData data) {
    TemplateRec* tpl = NewTemplate(isolate, true);
    tpl->callback = callback;
    tpl->callbackData = data;
    return tpl;
}

// A declaration has nowhere to report a failure - these return void - so
// running out of memory while declaring is what it is on the other backends:
// `std::bad_alloc` out of the declaration, rather than a template that quietly
// does not carry what it was given.

void TemplateSetConstant(TemplateRec* tpl, std::string_view name, Constant value, PropertyAttribute attributes) {
    TemplateEntry entry{
        .kind = TemplateEntry::Kind::Constant, .name = std::string(name), .constant = value, .attributes = attributes};
    if (value.GetKind() == Constant::Kind::String) {
        entry.text = std::string(value.AsString());
    }
    tpl->entries.push_back(std::move(entry));
}

void TemplateSetMethod(TemplateRec* tpl, std::string_view name, FunctionCallback callback, CallbackData data,
                       PropertyAttribute attributes) {
    tpl->entries.push_back(TemplateEntry{.kind = TemplateEntry::Kind::Method,
                                         .name = std::string(name),
                                         .callback = callback,
                                         .data = data,
                                         .attributes = attributes});
}

void TemplateSetSymbolMethod(TemplateRec* tpl, WellKnownSymbol key, FunctionCallback callback, CallbackData data) {
    tpl->entries.push_back(TemplateEntry{.kind = TemplateEntry::Kind::SymbolMethod,
                                         .symbolKey = key,
                                         .callback = callback,
                                         .data = data,
                                         .attributes = PropertyAttribute::DontEnum});
}

void TemplateSetAccessor(TemplateRec* tpl, std::string_view name, AccessorGetterCallback getter,
                         AccessorSetterCallback setter, CallbackData data, PropertyAttribute attributes) {
    CallbackRecord* record = &StateOf(*tpl->owner)->records.emplace_back(CallbackRecord{getter, setter, data});
    tpl->entries.push_back(TemplateEntry{
        .kind = TemplateEntry::Kind::Accessor, .name = std::string(name), .record = record, .attributes = attributes});
}

void TemplateSetTemplate(TemplateRec* tpl, std::string_view name, TemplateRec* value, PropertyAttribute attributes) {
    tpl->entries.push_back(TemplateEntry{
        .kind = TemplateEntry::Kind::Child, .name = std::string(name), .child = value, .attributes = attributes});
    if (tpl->sealed) {
        Seal(value);  // instantiated with `tpl` in every realm from now on
    }
}

// Class name, parent and handlers are fixed at the first instantiation - see
// unibind/template.h - so each of these four ignores a sealed template.

void TemplateSetNamedHandler(TemplateRec* tpl, const NamedPropertyHandler& handler) {
    if (tpl->sealed) {
        return;
    }
    tpl->named = handler;
    tpl->hasNamed = true;
}

void TemplateSetIndexedHandler(TemplateRec* tpl, const IndexedPropertyHandler& handler) {
    if (tpl->sealed) {
        return;
    }
    tpl->indexed = handler;
    tpl->hasIndexed = true;
}

void TemplateSetClassName(TemplateRec* tpl, std::string_view name) {
    if (tpl->sealed) {
        return;
    }
    tpl->className = std::string(name);
}

void TemplateInherit(TemplateRec* child, TemplateRec* parent) {
    if (child->sealed) {
        return;
    }
    child->parent = parent;
}

TemplateRec* TemplatePrototype(TemplateRec* tpl) {
    if (tpl->prototype == nullptr) {
        tpl->prototype = NewTemplate(*tpl->owner, false);
        tpl->prototype->prototypeOf = tpl;
        tpl->prototype->sealed = tpl->sealed;
    }
    return tpl->prototype;
}

TemplateRec* TemplateInstance(TemplateRec* tpl) {
    if (tpl->instance == nullptr) {
        tpl->instance = NewTemplate(*tpl->owner, false);
        tpl->instance->instanceOf = tpl;
        tpl->instance->sealed = tpl->sealed;
    }
    return tpl->instance;
}

std::optional<Slot> TemplateNewInstance(const Context& context, TemplateRec* tpl) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    const RealmScope realm(isolate, context.rec());
    return PushOrNothing(isolate,
                         reinterpret_cast<PyObject*>(NewTemplateInstance(isolate, context.rec(), tpl, nullptr)));
}

std::optional<Slot> TemplateGetFunction(const Context& context, TemplateRec* tpl) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    const RealmScope realm(isolate, context.rec());
    return PushOrNothing(isolate, Materialise(isolate, context.rec(), tpl));
}

std::optional<bool> TemplateHasInstance(const Context& context, TemplateRec* tpl, Slot value) {
    // What made the object - this template, or one inheriting from it - and
    // never its prototype chain, which script can rewrite. The record is on
    // the object, so the answer is the same from every realm.
    Isolate& isolate = OwnerOf(context);
    PyObject* object = Resolve(value);
    if (!IsObjectInstance(isolate, object)) {
        return false;
    }
    for (TemplateRec* maker = reinterpret_cast<ObjectInstance*>(object)->tpl; maker != nullptr; maker = maker->parent) {
        if (maker == tpl) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The class half of the backend interface
// ---------------------------------------------------------------------------

ClassRec* NewClass(Isolate& isolate, std::string_view name, TypeId nativeType) {
    auto owned = std::make_unique<ClassRec>();
    ClassRec* rec = owned.get();
    rec->owner = &isolate;
    rec->name = std::string(name);
    rec->nativeType = nativeType;
    rec->function = NewTemplate(isolate, true);
    rec->function->className = rec->name;
    rec->function->ownerClass = rec;
    StateOf(isolate)->classes.push_back(std::move(owned));
    return rec;
}

void ClassSetConstructor(ClassRec* rec, NativeConstructor constructor, bool callableWithoutNew) {
    rec->constructor = constructor;
    rec->callableWithoutNew = callableWithoutNew;
}

TemplateRec* ClassPrototypeTemplate(ClassRec* rec) {
    return TemplatePrototype(rec->function);
}

TemplateRec* ClassConstructorTemplate(ClassRec* rec) {
    return rec->function;
}

TemplateRec* ClassInstanceTemplate(ClassRec* rec) {
    return TemplateInstance(rec->function);
}

std::optional<Slot> ClassGetConstructor(const Context& context, ClassRec* rec) {
    return TemplateGetFunction(context, rec->function);
}

std::optional<Slot> ClassInstantiate(const Context& context, ClassRec* rec, NativeBox* native) noexcept {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        DestroyBox(native);  // ownership transferred at the call, whatever happens
        return std::nullopt;
    }
    const RealmScope realm(isolate, context.rec());
    ObjectInstance* instance = NewTemplateInstance(isolate, context.rec(), rec->function, nullptr);
    if (instance == nullptr || !AttachNative(isolate, instance, native)) {
        // Not published: the instance, if there is one, carries no box, and
        // the box is this function's to give back - once, here.
        DestroyBox(native);
        if (instance != nullptr) {
            Py_DECREF(instance);
            PyErr_NoMemory();
        }
        return std::nullopt;
    }
    // Past this point the instance owns the box: its deallocation, or
    // `~Isolate`, gives it back. The handle below can still fail to be made,
    // and then the instance goes - and gives the box back as it goes.
    return PushOrNothing(isolate, reinterpret_cast<PyObject*>(instance));
}

std::optional<bool> ClassHasInstance(const Context& context, ClassRec* rec, Slot value) {
    Isolate& isolate = OwnerOf(context);
    PyObject* object = Resolve(value);
    return IsObjectInstance(isolate, object) && reinterpret_cast<ObjectInstance*>(object)->tpl == rec->function;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool InitBindingTypes(Isolate& isolate, PyObject* module) noexcept {
    Isolate::Impl& impl = isolate.impl();
    impl.bindings.reset(new (std::nothrow) BindingsState());
    if (!impl.bindings) {
        PyErr_NoMemory();
        return false;
    }
    Types& types = impl.types;
    types.function = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &functionSpec, nullptr));
    types.boundFunction = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &boundSpec, nullptr));
    if (types.function == nullptr || types.boundFunction == nullptr) {
        return false;
    }
    BindingsState& state = *impl.bindings;
    state.templateMeta = reinterpret_cast<PyTypeObject*>(
        PyType_FromModuleAndSpec(module, &templateTypeSpec, reinterpret_cast<PyObject*>(&PyType_Type)));
    state.iteratorAdapter = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &adapterSpec, nullptr));
    if (state.templateMeta == nullptr || state.iteratorAdapter == nullptr) {
        return false;
    }
    return PyModule_AddObjectRef(module, "NativeFunction", reinterpret_cast<PyObject*>(types.function)) == 0 &&
           PyModule_AddObjectRef(module, "TemplateType", reinterpret_cast<PyObject*>(state.templateMeta)) == 0;
}

void IsolateBindingsTeardown(Isolate& isolate) noexcept {
    BindingsState* state = StateOf(isolate);
    if (state == nullptr) {
        return;
    }
    // Every realm's template cache, so that the collection `~Isolate` runs
    // next finds the types - and the prototypes, and the instances only they
    // kept - unreachable. The globals are held while this runs: dropping a
    // cache can run a `__del__`, and that can let a realm go.
    std::vector<PyObject*> globals;
    try {
        for (const auto& realm : isolate.impl().realms) {
            globals.push_back(Py_NewRef(realm.first));
        }
    } catch (const std::bad_alloc&) {
        // Whatever was not reached is collected with its interpreter.
    }
    for (PyObject* dict : globals) {
        if (PyDict_DelItemString(dict, CACHE_KEY) != 0) {
            PyErr_Clear();
        }
    }
    for (PyObject* dict : globals) {
        Py_DECREF(dict);
    }
    // The two types this state owns. A template type or an adapter still
    // alive holds its own reference; code that runs from here on finds no
    // template for any type, and makes plain objects.
    Py_CLEAR(state->templateMeta);
    Py_CLEAR(state->iteratorAdapter);
}

bool MirroredOnly(Isolate& isolate, PyTypeObject* type, PyObject* name) noexcept {
    PyObject* mro = type->tp_mro;
    if (mro == nullptr || !PyTuple_Check(mro)) {
        return false;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
        auto* entry = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro, i));
        if (entry->tp_dict == nullptr || PyDict_Contains(entry->tp_dict, name) != 1) {
            continue;
        }
        // The first class that has the name decides.
        if (!IsTemplateMadeType(isolate, entry)) {
            return false;
        }
        PyObject* mirrored = PyDict_GetItemString(entry->tp_dict, MIRRORED_KEY);  // borrowed
        return mirrored != nullptr && PySet_Check(mirrored) && PySet_Contains(mirrored, name) == 1;
    }
    return false;
}

}  // namespace ub::detail
