# CPython, and what a third engine in another language cost

Notes from writing the third backend, against CPython 3.14.7 built as a static
library (it was written against 3.12.13; section 13 says what the move changed).
`docs/spidermonkey.md` asked whether `docs/lifetimes.md` was a design or a
description of V8; this backend asks a harder question - whether the API is a
design or a description of *JavaScript* - and this document is the answer,
written the same way: the model first, then every place the engine and the API
disagreed and what the backend did about it.

The short answer, in three parts:

- **The C++ held.** No public header changed to add this backend. Not a
  signature, not a storage budget (`UNIBIND_FRAME_STORAGE_SIZE` and the rest
  were already big enough), not a rule. A program compiled against
  `include/unibind/` links against `unibind_backend_python.lib` exactly as it
  links against the other two, and `Platform::BackendName()` is how it finds out
  which it got.
- **The scripts did not, and could not.** CPython runs Python. `ub::Evaluate`
  hands the engine source text, and that text is Python here and JavaScript
  there. So the binding code an embedder writes - classes, templates,
  interceptors, callbacks, the pump - carries over unchanged, and the scripts
  that call it do not. This backend is not a drop-in replacement for a
  JavaScript one, and nothing below should be read as claiming it is.
- **Where the two languages disagree, one was picked, per operation, and
  written down.** JavaScript's semantics where the API promises them to C++;
  Python's where a Python programmer is writing Python. Section 3 is the list.

The shared suite in `tests/cases/` is written in JavaScript source as much as in
C++, so it does not run here and the parity comparison does not include this
backend. It has a suite of its own - `tests/python/`, 271 cases - written the
same way: against `ub::` only, with Python as the script language. Every
behaviour below that a test pins names the test.

## Contents

1. [The model](#1-the-model)
2. [Values](#2-values)
3. [`unibind.Object`, and property operations on everything else](#3-unibindobject-and-property-operations-on-everything-else)
4. [Functions, templates and classes](#4-functions-templates-and-classes)
5. [Exceptions](#5-exceptions)
6. [Stopping a script, interrupts, jobs and promises](#6-stopping-a-script-interrupts-jobs-and-promises)
7. [Heap and stack](#7-heap-and-stack)
8. [Binary data, structured clone, and the code cache](#8-binary-data-structured-clone-and-the-code-cache)
9. [What is not here](#9-what-is-not-here)
10. [The standard library, and shipping a program](#10-the-standard-library-and-shipping-a-program)
11. [What CPython itself costs](#11-what-cpython-itself-costs)
12. [Python is not a sandbox](#12-python-is-not-a-sandbox)
13. [Build notes, and what an upgrade has to recheck](#13-build-notes-and-what-an-upgrade-has-to-recheck)

## 1. The model

Four lines, and `src/backends/python/internal.h` opens with the same four:

| API | CPython |
|---|---|
| an `Isolate` | a sub-interpreter with a GIL of its own (PEP 684), whose one thread state stays attached to the thread that made it |
| a `Local` | a strong reference, held by the frame it lives in |
| the pending exception | CPython's own error indicator |
| a `Context` (realm) | a globals dictionary |

### 1.1 The platform and the isolate

`Platform` initialises CPython once, in the *main* interpreter, with
`PyConfig_InitIsolatedConfig`: no `site`, no user site directory, no signal
handlers (the host owns Ctrl+C), no `.pyc` files written, and nothing read from
the environment or the command line - `PYTHONPATH` and friends are ignored, and
`sys.flags.isolated` is 1. The main interpreter then detaches its thread state
and **never runs a script**. It owns the process-wide machinery - the runtime,
the allocators - and nothing else.

`Isolate::New` makes a sub-interpreter with `Py_NewInterpreterFromConfig`:

| `PyInterpreterConfig` | | why |
|---|---|---|
| `gil` | `OWN_GIL` | isolates on different threads run Python in parallel (`concurrency: isolates on different threads run Python in parallel`) |
| `use_main_obmalloc` | 0 | required by an own GIL |
| `check_multi_interp_extensions` | 1 | an extension module that keeps state in C globals is refused rather than shared between interpreters - section 10 |
| `allow_fork`, `allow_exec` | 0 | an embedded engine does not replace its host process: `os.execv` raises `RuntimeError` |
| `allow_threads` | 1 | `threading` works - within the limits in section 6.4 |
| `allow_daemon_threads` | 0 | a daemon thread would outlive the interpreter it runs in |

The thread state the new interpreter comes with is **never detached**. It stays
attached to the thread that called `Isolate::New` until `~Isolate`, so no
operation ever takes a GIL on the way in: the only thread allowed to call one
already holds it. That is decision 11 - one isolate per thread - arrived at from
the other side. SpiderMonkey imposed that rule because a thread can have only
one `JSContext`; here it falls out of a thread state being attached for life,
and the backend refuses a second `Isolate::New` on the thread for the same
reason the other two do.

The rest of the platform, briefly:

- `BackendName()` is `"python"` and `BackendVersion()` is `PY_VERSION`
  (`"3.14.7"`).
- `detail::BackendBuildId()` is `python-<version>-<magic>`, the magic being the
  bytecode magic number: exactly what a `.pyc` is refused over, and so exactly
  what a code-cache blob has to be keyed on (decision 19).
- `WorkerThreads()` answers 0, whatever `workerThreads` asked for. CPython starts
  no threads of its own; collection and compilation happen on the thread that
  asked.
- `PlatformOptions::engineFlags` is ignored. There is no string of engine flags
  to hand CPython; its equivalents are `PyConfig` fields, and the ones that
  matter are fixed above.

### 1.2 A frame is an array of strong references

CPython never moves an object, so the whole of `docs/lifetimes.md` section 3
point 3 - *nothing portable gives you a stable address* - is moot here, and a
frame is the simplest thing it could be:

```cpp
struct Frame {
    Isolate* owner; Frame* parent; std::uint32_t epoch;
    PyObject* const* args; std::uint32_t argCount;   // borrowed from the vectorcall
    std::uint32_t count, spillCapacity;
    PyObject* inlineSlots[UNIBIND_FRAME_INLINE_SLOTS];
    PyObject** spill;                                // ::operator new(std::nothrow)
};
```

Pushing a handle takes a reference; closing the frame releases every reference
it took, in reverse. Escaping is `Py_NewRef` into the parent - free and
unbounded, as on SpiderMonkey. A `Global<T>` is one more strong reference. Slots
`[0, argc)` of a callback's frame are the vectorcall argument array, which
CPython holds for the length of the call, so reading an argument touches no
reference count (section 8 of `docs/lifetimes.md`, which this backend follows
exactly).

The spill buffer comes from `::operator new(std::nothrow)`, not from
`PyMem_Malloc`, so that rule 9 - a frame that cannot grow yields an **empty**
handle - is reachable by the same lever the shared suite uses. A frame that
cannot grow releases the value it was handed and reports `MemoryError`
(`PyErr_NoMemory` allocates nothing: the instance is preallocated).

`docs/lifetimes.md` section 12 described this backend before it existed; it
now says what was built.

### 1.3 The pending exception is the error indicator

CPython has no `v8::TryCatch`. As on SpiderMonkey, an exception is simply
pending - here, in the thread state's error indicator - until something takes
it, so `TryCatch` is a stack of handlers the backend keeps itself
(`TryCatchState`), and "catching" is `PyErr_GetRaisedException`.

Ownership is by native depth, the same rule SpiderMonkey's backend uses: a
handler takes an exception only if it was opened at the depth of the operation
that raised it. An exception a native leaves behind with no handler of its own
belongs to the Python code that called the native, and propagates into it when
the native returns - that is what makes `info.ThrowTypeError("...")` a
`TypeError` a script can `except` (`functions: a native throw is a Python
exception, catchable in script`). An exception with no handler and no native on
the stack is dropped, as V8 drops an uncaught one. Anything already pending when
a handler opens is parked and put back when it closes.

`Isolate::HasPendingException()` is `PyErr_Occurred()`.

### 1.4 A realm is a globals dictionary

`Context::New` makes a `dict` holding `__builtins__` (the `builtins` module),
and `__name__ == "__main__"`. `Context::GlobalObject()` **is that dictionary**,
and every script run in the realm runs with it as both its globals and its
locals. So what native sets on the global object is a global to script, and
`globals()` in script is what native reads (`objects: a dict is its items, the
realm's globals among them`). The backend's own entries - anything named
`__unibind_*`, which today is the template cache (section 4.2) - are left out of
every key listing. The global object's prototype is `null`: a dict has no
`[[Prototype]]`.

**Lifetime.** Two things keep a realm alive: the embedder's `Context`
references, and the dictionary itself. While a `Context` exists the realm's
record holds a strong reference to the dictionary. Once none does, the record
lives exactly as long as the dictionary: a function defined in a realm whose last
`Context` the embedder has released still finds its realm when it calls a native,
through its `__globals__`, for as long as something keeps those alive
(`functions: a native called from a realm whose Context was released still has
its realm`, `lifetimes: a callback that keeps its Context brings a released realm
back, whole`).

What tells the record its dictionary has gone is a **dictionary watcher**
(`PyDict_AddWatcher`, one per isolate), which reports the dictionary's
deallocation, and nothing else the backend acts on. The record is deleted then,
and its entry in the isolate's dictionary-to-realm map goes with it. Nothing of
the backend's is stored *in* the dictionary to keep the record, and that is the
point: the dictionary is script's. An earlier version hung the record on a
capsule under one of its keys, and script broke it both ways - `globals().clear()`
freed the record while a `Context` still named the realm, a use-after-free; and
a copy, `dict(globals())`, kept the capsule alive after the realm's own dictionary
had gone, so the map pointed at a dead address that the next dictionary allocated
there inherited, and a native called from that one found the wrong realm
(`lifetimes: script that empties its own globals does not take the realm's record
with it`, `lifetimes: a copy of a realm's globals that outlives the realm confuses
no later realm`). No reference cycle runs through C++ either way, which is what
lets the collector free a realm at all; the template cache lives *inside* the
globals for the same reason.

A native's `GetContext()` is, in order: the realm of the innermost operation that
was handed a `Context`, if no Python code has run since it began; the realm whose
globals the calling Python frame runs in; the entered `ContextScope`'s; the realm
the function was made in, if it is still alive; and, for code with no realm at
all - a job run with nothing entered - any live realm of the isolate, because
`GetContext()` is not allowed to be empty.

**What realms of one isolate share is the interpreter.** `sys`, `sys.modules`,
the `builtins` module and every module imported in one realm are the same
objects in every other. A realm here is a separate global namespace, not a
separate world: a script that sets `builtins.x` or patches `json.dumps` has done
it for every realm of its isolate. On V8 and SpiderMonkey a realm has its own
built-ins; on this backend it cannot, because what CPython isolates is an
*interpreter*, and one of those is an isolate. Two sandboxes that must not see
each other's module state are two isolates, on two threads.

### 1.5 Scripts

A script is compiled in two pieces, by `compile_script` in the backend's own
Python (module.cpp): everything up to a trailing expression statement, in
`exec` mode, and that expression, in `eval` mode. Running the script runs the
first and evaluates the second, and **its value is the completion value**, which
is what `Evaluate("1 + 1")` answering 2 requires (`smoke: an expression
statement is the completion value`). A script with no trailing expression
evaluates to `None`, which is `undefined` (`smoke: a script with no trailing
expression is undefined`).

- The source is decoded as UTF-8 with replacement, so bad bytes become U+FFFD
  rather than refusing the script.
- `ScriptOrigin::resourceName` is the code's filename (cut at its first NUL, as
  `unibind/types.h` says), and `lineOffset` shifts every line number.
  `columnOffset` is ignored: CPython has nothing to shift columns with.
- The source is registered with `linecache` under that name before it is
  compiled, so a traceback - even a `SyntaxError`'s - quotes the line.
- **Top-level `await` is allowed** (`PyCF_ALLOW_TOP_LEVEL_AWAIT`). Such a script
  compiles to coroutine code, and running it runs nothing: it evaluates to a
  `Promise` - an asyncio `Task` - of its completion value, which `PumpJobs`
  drives (`promises: a script with top-level await evaluates to a promise the
  pump settles`). That is the shape a JavaScript module with top-level await
  has, and the only one that fits a synchronous `Script::Run`.
- `CompileOptions::EagerCompile` changes nothing: CPython compiles a module in
  full or not at all, so every compile is the eager one.
- A compiled `Script` is two code objects, which belong to no realm, so decision
  10 - a script sees the globals of the realm it runs in - costs nothing here.
- The `unibind` module is **not** put in a realm's globals. A script that wants
  `unibind.null` or `unibind.Object` imports it: `import unibind`.

## 2. Values

| `ub::` | Python | notes |
|---|---|---|
| `undefined` | `None` | |
| `null` | `unibind.null` | a singleton of its own; falsy; `repr` is `null` |
| `Boolean` | `bool` | |
| `Number` | `int` with \|n\| ≤ 2^53, or `float` | an integral Number made in C++ arrives as an `int` - see below |
| `BigInt` | `int` beyond ±2^53 | recognised, as on the other backends; there is still no factory |
| `String` | `str` | a lone surrogate, which a `str` may hold, reads out as U+FFFD |
| `Symbol` | `unibind.Symbol` | `Symbol.for` is `unibind.symbol_for(key)`; the well-known five are `unibind.Symbol.iterator` and so on |
| `Array` | `list`; a `tuple` is a read-only one | |
| `Function` | anything callable that is not a primitive | a Python function, a bound method, a class, a `unibind.NativeFunction` |
| `Object` | everything else | `unibind.Object`, a `dict`, a module, an instance of any Python class |
| `External` | `unibind.External` | opaque; not an `Object` (decision 8) |
| `ArrayBuffer` | `bytearray`; a `bytes` is a read-only one | |
| `TypedArray`, `DataView` | `unibind.TypedArray`, `unibind.DataView` | typed windows onto a `bytearray` - section 8 |
| `Promise` | `asyncio.Future`, and so any `asyncio.Task` | section 6.3 |

**Numbers.** JavaScript has one number type and Python has two, and the backend
decides by value, not by C++ type:

- `Number::New(isolate, 2.0)`, `ReturnValue::Set(2.0)` and a `double` constant
  all arrive in Python as the `int` 2 when the value is integral and within
  ±2^53 - so script can index, slice and `range()` with what a native returned
  (`functions: every way of answering a call reaches Python as the value it
  names`). Anything else is a `float`, and so is `-0`, which an `int` cannot
  hold. `Integer::New` is always an `int`.
- In the other direction a Python `2.0` stays a `float`: `Kind()` is `Number`,
  `Is<Integer>()` is true (it asks about the value, as it does on
  SpiderMonkey), and `ToString` of it is `"2.0"`, not JavaScript's `"2"`.
- An `int` beyond ±2^53 is a `BigInt`, because a Number cannot hold it exactly.
  `NumberValue()` of one is the nearest double.

**Symbols and the Python protocols.** A property key is a `str`, a
`unibind.Symbol`, or an `int` for a canonical array index - `"3"` and `3` are one
property, `"03"` is not an index, as in JavaScript. Three of the well-known
symbols stand for a Python protocol, and a property stored under one is stored
under the protocol's name: `Symbol.iterator` is `__iter__`,
`Symbol.asyncIterator` is `__aiter__`, `Symbol.hasInstance` is
`__instancecheck__`. Key listings hand the symbol back, so C++ sees what it
declared. **Only `Symbol.iterator` is wired to anything**: `iter()` of a
`unibind.Object` looks up `__iter__` along its prototype chain (section 3). The
other two are stored and read back under their dunder names, but Python looks
`__aiter__` and `__instancecheck__` up on the *type*, never on an instance or
its prototype, so `async for` over an object with a `Symbol.asyncIterator`
method raises `TypeError`, and `isinstance` ignores a `Symbol.hasInstance`
method. `Symbol.toPrimitive` and `Symbol.toStringTag` have no Python
counterpart and are ordinary symbol keys.

**Conversions are Python's where Python has one.** Nothing in the shared suite
could pin these, and the Python suite does not assert them directly; the code
is `src/backends/python/values.cpp`:

| operation | answer |
|---|---|
| `ToBoolean` | Python truth - `bool(x)` - except that `null` is false. **An empty list, dict, string and `0.0` are false**, and so is any object whose `__bool__` or `__len__` says so. A `unibind.Object` is always true (section 3). |
| `ToString` | `str(x)`: `None` is `"None"`, `True` is `"True"`, a list is `"[1, 2]"`, a `unibind.Object` is its literal-shaped repr |
| `ToNumber` | `None` is NaN, `null` is 0, a `bool` 0 or 1, a string is parsed as `float()` parses it, then as a `0x`/`0o`/`0b` literal, and is NaN if neither works; a Symbol is a `TypeError`; anything else is asked for `__float__` or `__index__`, and is NaN if it has neither |
| `ToInt32`, `ToUint32` | an `int` of any size is reduced modulo 2^32 exactly; everything else goes through `ToNumber` |
| `ToObject` | the value itself - everything in Python already is one - except `None` and `null`, which are a `TypeError` |
| `StrictEquals` | two numbers compare by value, `int` against `float` included (`1 === 1.0`); strings by content; everything else by identity. Runs no Python code. |
| `SameValue` | the same, with NaN equal to itself and `-0` unequal to `+0` |
| `LooseEquals` | `null == undefined`, and otherwise Python's `==`, which runs `__eq__`. `1 == "1"` is **false**. |

**Strings.** `String::New` is strict, as everywhere: bytes that are not UTF-8
make no string, and nothing is left pending. Text an embedder hands over in any
other form - a name, an error message - is decoded with replacement. A Python
`str` may contain a lone surrogate, which UTF-8 cannot carry, so reading one out
writes U+FFFD in its place, as V8 does for an unpaired surrogate.

## 3. `unibind.Object`, and property operations on everything else

A Python object already has properties - attributes, or items - so the question
the backend has to answer for every `ub::Object` operation is *which* of them it
means. The answer is by type:

| the object is | a property is | notes |
|---|---|---|
| a `unibind.Object` | an own property or one found along its `[[Prototype]]` chain, with the template's interceptor consulted first | the full model below |
| a `dict` | an item, keyed as every key here is keyed | the realm's global object is one |
| a `list` | an index, and `length` | writing past the end grows it, the gap filled with `None`, which is what a hole reads as; writing `length` truncates or pads |
| a `tuple` | the same, read-only | `Set` answers false, `Delete` false |
| anything else | an attribute: `getattr`, `setattr`, `hasattr`, `delattr` | an index key is spelled as its digits; a symbol names no attribute |

(`objects: a dict is its items...`, `objects: an array is a list, grown by index,
with holes that read as None`, `objects: a tuple is a read-only array`,
`objects: any other object is its attributes`.)

What an ordinary object cannot carry is refused rather than faked: a
`DefineOwnProperty` with attributes other than `None` on a dict or an ordinary
object answers false, and so does `SetAccessor`, because a dict item cannot be
read-only and an attribute cannot run native code on read. An ordinary object's
attribute whose name starts with an underscore reports `DontEnum` and is left
out of an enumerable key listing - the only convention Python has for "not for
listing". `GetPrototype` of anything but a `unibind.Object` is `null`: its
Python type is a different thing, and nothing here walks it as a prototype.

### What `unibind.Object` is

`unibind.Object` is a JavaScript-shaped property bag: an ordered own-property
table, per-property attributes (`ReadOnly`, `DontEnum`, `DontDelete`), accessor
properties that run native code, a `[[Prototype]]` chain that lookup walks, and
the interceptor a template may put on it. `Object::New` makes one, and every
object a template or a class makes is one - their types derive from it. It is
garbage-collectable, weakly referenceable, and collected in a cycle
(`objects: an object is weakly referenceable and collected in a cycle`).

Own keys come in the language's order: array indices ascending, then strings in
insertion order, then symbols in insertion order.

From Python it should read naturally, and does: `o.x`, `o["x"]`, `o[0]`,
`o[sym]`, `"x" in o`, `del o.x`, `for k in o`, `len(o)`, `dir(o)`, and
`o.__proto__` to read or replace the prototype (a cycle is refused with
`TypeError: Cyclic __proto__ value`). `unibind.Object(mapping_or_pairs=None,
**properties)` builds one as an object literal would, and copying another
`unibind.Object` copies its enumerable own properties, getters run
(`objects: Python builds, copies and iterates an object as it would a
mapping`).

### Where it is not JavaScript, and which way each went

The rule: **operations the API promises to C++ behave as V8's do; Python
spellings behave as Python's do.** Each of these is deliberate and each has a
case.

- **A missing property is `undefined` to C++ and an error to Python.**
  `Get` answers `undefined`; `o.nope` raises `AttributeError` and `o["nope"]`
  `KeyError`, which is what `getattr` defaults, `hasattr`, `dict.get`-style code
  and every Python programmer expect (`objects: a missing property is undefined
  to native and an error to Python`).
- **A refused write is sloppy from C++ and strict from Python.** `Set` on a
  read-only property, or on an accessor with no setter, answers **true** and
  changes nothing, as V8's `Object::Set` does in the sloppy mode it runs in.
  From Python there is no sloppy mode to drop the write silently in, so it is
  the `TypeError` strict-mode JavaScript throws: `Cannot assign to read only
  property 'x' of object`, `Cannot set property x of object which has only a
  getter`, `Cannot delete property 'x' of object` (`objects: a defined property
  honours its attributes, from Python and from native`). An inherited read-only
  property refuses the write that would shadow it, as the language says.
- **`del` of something that is not there raises**, `AttributeError` or
  `KeyError`. `Delete` from C++ answers true, as JavaScript's `delete` does.
  `del` of a missing name is a mistake in Python code and nothing else would
  say so.
- **`len(o)` is the number of enumerable own keys** - `Object.keys(o).length` -
  and **`bool(o)` is always true**, as every object is in JavaScript. Without
  the second, an object with no enumerable keys would be falsy through `len`,
  and `if o:` would mean something no reader of the object expects.
- **Iterating an object yields its enumerable own keys**, as a dict's does -
  unless it has a `Symbol.iterator` method (one a template or class declared,
  or one stored under `unibind.Symbol.iterator`), which is called with the
  object as its receiver. What it returns is adapted: a Python iterator is used
  as it is, and a **JavaScript-style iterator** - an object whose `next()`
  answers `{done, value}` - is wrapped by `unibind.IteratorAdapter`, so a class
  whose iterator was written for JavaScript iterates from Python unchanged
  (`templates: a Symbol.iterator method written for JavaScript iterates from
  Python`, `classes: a Symbol.iterator method makes instances iterable from
  Python`).
- **`repr` is an object literal**: `{x: 1, y: 'two'}`, `Widget {name: 'w'}` for
  an instance of a template's type, keys spelled as a literal would spell them,
  and cycles as `{...}`. Enumerable own properties only, and **accessors are
  shown as `[Getter/Setter]`, never run**: a repr that ran native code would make
  a debugger's variable view do things.
- **A native function read as an attribute comes back bound** to the object it
  was read from - `o.method()` hands the callback `This()` as `o`, as the same
  call does in JavaScript. Read by subscript, `o["method"]`, it comes back as
  stored, unbound, as `o.__dict__["m"]` would. A Python function stored as a
  property is never bound: it is a value in a table.
- **An interceptor is not asked about dunder attribute names.** Python's own
  machinery looks names like `__len__`, `__deepcopy__` and `__getstate__` up on
  instances constantly - `copy`, `pickle`, `inspect`, a bare `hasattr` - and a
  catch-all interceptor, which is what interceptors exist to be, would answer
  every one of them and break all of it. The same name by subscript,
  `o["__len__"]`, still reaches the hook (`interceptors: Python's own dunder
  lookups do not reach a catch-all`).
- **Python's data descriptors come first.** `__class__`, `__proto__`, and a
  `property` a Python subclass declares are found before the JavaScript lookup,
  as they are for any Python object (`objects: a Python subclass keeps Python's
  descriptors and methods`).
- **An array is dense, and `Array::New` is capped at 2^26 elements.** A
  JavaScript array may be holes and cost nothing; a Python list may not - every
  element is a pointer - so `new Array(2**32 - 1)` would be 32 GiB of `None`.
  Past 2^26 (64 Mi elements, half a gigabyte of pointers) `Array::New`, an index
  write and a `length` write are refused with a `RangeError` rather than
  attempted, because attempting one can *succeed* on a machine with enough swap
  and then page the process to death (`objects: an array too long to be dense
  is refused, not attempted`). Deleting an element leaves `None` in its place,
  as `delete a[i]` leaves a hole, and does not shift the rest.

## 4. Functions, templates and classes

### 4.1 Native functions

`Function::New` makes a `unibind.NativeFunction`: a vectorcall object carrying
the callback and its `CallbackData` or script value, and nothing the isolate has
to keep, so a function made on every call is freed with its last reference
(`functions: a native function made on every call is freed with its last
reference`). A script value given as its data is held by the function and
visited by the collector through it, so it lives exactly as long as the function
does, cycles included (`functions: the value lives exactly as long as the
function, cycles included`).

- It is callable and **not a constructor** (decision 9): it is not a type, and
  Python constructs only types. `NewInstance` on one is a `TypeError`.
- It takes **no keyword arguments**, since the API has no way to hand a callback
  one, and refuses them with a `TypeError` rather than dropping them.
- Its `repr` is `<native function name>`, and its `__name__` is the name it was
  declared with - empty for `Function::New`, as V8 leaves it.
- `This()` for a plain call from Python, and for a call from C++ with no
  receiver or with `undefined` or `null`, is **the realm's global object** -
  its globals dict - because `unibind/function.h` promises `This()` is always an
  object. Any other receiver is passed as it is (`functions: This is the
  receiver - the object a method was read from, or the realm's globals`).
- A C++ exception out of a callback does not unwind through CPython's frames:
  it becomes a `SystemError` (a `std::bad_alloc`, a `MemoryError`) in the
  script that called it (`functions: a C++ exception does not unwind through the
  interpreter`).

`Function::Call` from C++ on a **Python** callable drops the receiver. A Python
function has no `this` to be given: a bound method is already bound to its own,
and a plain function takes what it is passed. Slipping the receiver in as a first
argument the callable did not ask for would be worse than dropping it. A bound
native function keeps the receiver it was bound to, as a JavaScript bound
function does.

`NewInstance` from C++ takes a **type** - a template's, a class's, or one script
declared, `dict` included - and anything else is `... is not a constructor`
(`functions: native calls a function script wrote, and constructs a class script
wrote`).

### 4.2 Templates are types

There is no engine-side template in CPython, so - as on SpiderMonkey - a
`TemplateRec` is a descriptor the backend replays. What it replays *into* is the
difference: **a function template materialises, per realm, as a Python heap
type** derived from `unibind.Object`, whose metaclass is `unibind.TemplateType`
(a subclass of `type` with one field, the template it was made from).

| JavaScript | here |
|---|---|
| the constructor function | the type: `unibind.<ClassName>` |
| `F.prototype` | the type's `prototype` attribute, a `unibind.Object`, and every instance's `[[Prototype]]` |
| `F.prototype.constructor` | the type, `DontEnum` |
| statics - `FunctionTemplate::Set`, `Class<T>::StaticMethod`/`StaticValue` | type attributes: `Widget.KIND` |
| instance-template members | own properties stamped on each instance |
| `new F(...)` | **calling the type**: `F(...)` |

The type is made the first time a realm asks for it and cached **in that
realm's globals**, under a key of the backend's own. The obvious place, the
realm's C++ record, would have been a reference nothing can see: record → cache
→ type → prototype → a function script stored there → its `__globals__` would
keep the dictionary, and so the record, alive through a cycle no collection can
break, and the realm would have lived until its isolate did. Inside the globals
every one of those references is visible, and the cache goes with its realm like
everything else in it (`lifetimes: what a template made in a realm goes with the
realm`). One
template instantiated into three realms is three types (`templates: one template
instantiates into several realms, as a type of each`).

Decisions this needed on top of the other two backends':

- **Calling a type from Python is `new`.** Python has one spelling for both, so
  `Counter(3)` constructs, `IsConstructCall()` is true, and a `Class<T>` made
  with `Construct` - construct-only - is still callable from Python, because
  that call *is* the construction. The plain call of decision 12's grid is
  reachable only from C++: `Function::Call` on a template's type runs a
  `FunctionTemplate`'s callback with `IsConstructCall()` false and answers what
  it wrote, refuses a `Class<T>` with a `TypeError`, and makes an instance of a
  `ConstructOrCall` class (`templates: a function template is a function as well
  as a constructor`, `classes: a plain call from native is refused, unless the
  class opted in`).
- **A constructor answering with an object replaces the instance**; a primitive
  answer is ignored, and a `Class<T>`'s answer always is - what comes out of one
  must carry a `T`.
- **`Inherit` chains the types as well as the prototypes.** V8 chains only the
  prototypes. A Python type that did not also derive from its parent's would
  answer `isinstance(child, Parent)` wrongly, so here it does derive - and the
  one visible difference is that **a static declared on the parent is visible
  on the child**, as a Python class attribute is. `unibind/template.h` says it is
  not; on this backend it is (`templates: inheritance puts the parent's prototype
  behind the child's, and its type behind the child's`).
- **A static declared `ReadOnly` or `DontDelete` holds on the type**: assigning
  or deleting it from Python is the same `TypeError` a `unibind.Object`
  property gives. `Class<T>::StaticValue` is read-only by default. Everything
  else on the type is Python's to change.
- `HasInstance` asks what made the object - this template or one inheriting from
  it - and never the prototype chain, which script can rewrite; the record is on
  the object, so the answer is the same from every realm.
- The shape is sealed at first instantiation, as on the other two (decision
  13): a later `SetClassName`, `Inherit` or `SetHandler` is ignored, and members
  may still be added (`templates: the shape is fixed at the first instantiation,
  and members may still be added`).

### 4.3 Python subclasses of a template's type

A script may subclass a template's or a class's type, and the subclass
constructs through the template - a `Class<T>` subclass instance still carries
its native (`classes: a Python subclass keeps its own methods and still carries
the native`). What that needed:

- **A member a Python subclass declares overrides the prototype**, as a
  subclass's method overrides its base's in Python. Lookup is: the instance's
  own properties, then members declared by a Python class between the instance's
  type and the first template-made type in its MRO, then the prototype chain.
  Without that, `class Loud(Counter): def increment(self): ...` would never run
  its own `increment` (`classes: a Python subclass overrides a class method, and
  can call the base's`).
- **`super()` works, by a mirror.** `super().increment(by)` searches class
  dictionaries along the MRO and never looks at a prototype. So a template's type
  also carries its prototype's native methods as class attributes, and records
  which ones are mirrors: ordinary lookup ignores a mirror (the prototype chain
  decides), so an instance whose prototype was swapped away does not keep the old
  methods through the back door. Dunder names are not mirrored - the type slot
  for `__iter__` would otherwise call the native directly and skip the iterator
  adapter.

### 4.4 Interceptors

A template's handler answers for every instance's properties, consulted before
the object's own properties and the prototype chain, and `Intercepted::No` falls
through to that ordinary lookup (decision 5). Behaviour worth knowing, all held
by `tests/python/interceptors_test.cpp`:

- An `int` key - a canonical array index - goes to the indexed half; every other
  key, symbols included, to the named half.
- `has` asks the query hook, then the getter: a getter on its own is enough to
  make a key look present, as on SpiderMonkey.
- A **setter hook is asked only about a write made on the object itself**, as V8
  asks it; a write through something inheriting from an intercepted object is the
  ordinary set.
- The enumerator's keys are merged with the object's own; a key it lists is
  enumerable unless the query hook says `DontEnum`.
- A hook that throws has intercepted the access, whatever it returned.
- Dunder attribute names are not put to the hooks (section 3).
- A well-known symbol that stands for a protocol reaches a named hook as its
  dunder name, because that is the key it is stored under.
- `This()` is the object the access was made through and `Holder()` the object
  carrying the hook. CPython gives a lookup both, so both are real - unlike V8
  15.6's interceptors, which see only the holder.

### 4.5 `Class<T>`, and when a native is given back

A class instance is a `unibind.Object` whose `box` field holds the `NativeBox`,
recorded in the isolate's `liveNatives` so that every box is given back exactly
once (decision 7). There is no finalizer to wait for: **a reference count that
reaches zero deallocates the instance there and then, and the box goes with it**
(`classes: a native goes with its instance's last reference, exactly once`).
Only instances in a reference cycle wait for the collector - `gc.collect()`,
`Isolate::RequestGarbageCollection()`, or the isolate going (`classes: instances
in a cycle are collected by gc.collect(), natives with them`).

**A native is destroyed on the isolate's thread, always** (decision 7). An
instance can die elsewhere: a `threading.Thread` the script started can drop the
last reference to one (section 6.4). Its native is then *not* destroyed there -
a native is the embedder's object, and is never destroyed on a thread the
embedder did not make - but left in `liveNatives`, and given back at the latest
by `~Isolate`, on the isolate's thread (`lifetimes: an instance that dies on a
script's thread gives its native back on the isolate's`).

`~Isolate` gives back the rest in this order, while the interpreter is still
alive, because a native's destructor is the embedder's code and may release a
`Context` or a `Global` of its own - or run script:

1. stop the threads the script started, and wait for them (section 6.4);
2. stop accepting work, drop queued jobs and interrupts, close the event loop
   (section 6);
3. drop every realm's template cache, so the types, their prototypes and the
   instances only they kept become unreachable;
4. `gc.collect()`, so garbage gives its natives back through the ordinary path;
5. give back every box still alive, **one at a time**, each taken out of
   `liveNatives` before it is destroyed. A native's destructor can run script -
   dropping its last reference to a Python object runs that object's `__del__` -
   and that script can reach another instance still waiting its turn, or the one
   going now. While this runs, `Unwrap` answers only for a native still waiting,
   so it never hands out one already destroyed, and a native made meanwhile
   joins the set and goes too (`lifetimes: natives destroyed at teardown whose
   destructors run script that reaches other natives`, `lifetimes: a native whose
   destructor makes another native at teardown - that one goes too`);
6. check, in a checked build, that the embedder holds no `Context`, `Script` or
   `Global` (decision 27) - here, after the natives, because a native that owns
   a realm and a root gives them back by being destroyed;
7. drop the backend's types, stop any thread a finalizer started meanwhile, and
   `Py_EndInterpreter` - or leave the interpreter behind (section 6.4).

An instance deallocated after step 5 - during `Py_EndInterpreter`, say, by a
module being torn down - still carries its box pointer, which is gone; from then
on `Unwrap` answers null for every instance, and a method called on one is the
wrong-receiver `TypeError` (`classes: the isolate gives back every share it
still holds when it goes`).

## 5. Exceptions

`ErrorKind` maps onto Python's classes where Python has one, and onto a class of
the `unibind` module where it does not:

| `ErrorKind` | Python |
|---|---|
| `Error` | `unibind.Error(Exception)` |
| `TypeError` | `TypeError` |
| `RangeError` | `unibind.RangeError(ValueError)` - the Python name for the same complaint |
| `ReferenceError` | `NameError` |
| `SyntaxError` | `SyntaxError` |

Two more exception classes are the backend's own:

- **`unibind.Thrown(Exception)`** carries a thrown value that is not an
  exception. `ub::Throw(isolate, value)` with a string, a number or an object -
  legal in JavaScript, impossible to `raise` in Python - raises
  `unibind.Thrown(value)`, and `TryCatch::Exception()` hands back the value, not
  the wrapper. A script's own `raise` can only raise an exception, and that is
  what `Exception()` then answers.
- **`unibind.Terminated(BaseException)`** is `TerminateExecution` unwinding the
  script. A `BaseException`, so `except Exception` does not see it; section 6.1
  says what happens to code that catches it anyway.

What a `TryCatch` reports, in Python's formats - neither is parseable across
engines, and the header already says so:

| | format |
|---|---|
| `Message()` | the last line of `traceback.format_exception_only`: `TypeError: bang`, `KeyError: 'nope'`, `unibind.Error: nope` |
| `StackTrace()` | Python's traceback text, `Traceback (most recent call last): ...`, with the backend's own frames left out |
| `StackFrames()` | innermost first. `functionName` is the code object's name, empty at module level; `scriptName` the resource name; `lineNumber` and a 1-based `columnNumber` from the code's position table |
| `Location()` | a `SyntaxError`'s own file, line, offset and text - a script that never compiled has no frame, and this is how it is still placed - and otherwise the innermost frame's, with the line quoted from `linecache` |

`CaptureStackFrames` walks the running Python frames, innermost first; from C++
with no Python below it, it is empty. Every frame of the backend's own Python -
the support code, compiled under the name `<unibind>` - is left out of every
stack the backend reports.

A termination has no message, no stack and no location, and `Exception()` is
empty for one.

## 6. Stopping a script, interrupts, jobs and promises

### 6.1 Termination

CPython has no terminate and no interrupt. What it has is the **pending call**:
a per-interpreter queue any thread may add to, which whichever of the
interpreter's threads checks first drains at its next *eval-breaker* check -
every loop back-edge, every function entry, most calls. A pending call that
returns -1 with an exception set raises that exception there.

`TerminateExecution` sets the library's own flag first - so every gate on the
isolate's thread refuses from that instant, the rule decision 15 makes ours to
keep on every engine - and then queues one such call, `Service`, which raises
`unibind.Terminated`. Two things make the stop stick, because a single
exception is easy to swallow:

1. **The pending call re-queues itself before it raises**, for as long as the
   flag is set. The eval breaker stays tripped, and the next check - the loop's
   back-edge, the next function entered - raises `Terminated` again. A script
   that writes `except BaseException: pass` inside a `while True` is stopped
   anyway (`termination: a loop that swallows BaseException is stopped anyway`),
   and so are nested calls, recursion and generators that catch everything.
2. **`sys.monitoring` LINE and CALL events are switched on while the stop is in
   force** (PEP 669; tool id 4, else 3 - the two CPython leaves unassigned).
   Between two eval-breaker checks there can be straight-line code, and a call
   to a builtin or a native is checked only *after* it returns. A `finally:`
   block of two plain statements would run both, and a builtin called in an
   `except` block would run - where V8 runs no `finally` at all. A LINE event and
   a CALL event raise `Terminated` again at each new line and before each call,
   so neither runs (`termination: a loop in a finally block is stopped, and the
   finally's other lines do not run`, `termination: a builtin called in an
   except block does not run`). PEP 669 instruments code only while events are
   set, so this costs nothing until a stop, and the cancel switches it off. If a
   script's own debugger holds both tool ids, the stop still works through the
   pending call alone, with those two gaps.

`Terminated` reaching the top of a finalizer - a `__del__`, a coroutine being
closed - or of a thread the script started, while a stop is in force, is the stop
working, not an error, so the isolate's `sys.unraisablehook` says nothing about
it and passes everything else to the default hook.

**The stop reaches every thread of the isolate's interpreter**, not only the
isolate's own: the pending call and the monitoring events fire on whichever
thread checks, so a `threading.Thread` the script started is stopped with it
(`teardown: TerminateExecution stops a script's threads too, and an interrupt
waits for the isolate's thread`). Such a thread is simply ended: the stop is
uncatchable there as everywhere, so its `finally` blocks do not run.

Two consequences of "no Python runs while a stop is in force" are worth knowing,
because they reach past the script that was stopped:

- **Python-level cleanup does not happen.** A weakref callback is Python, so
  while a stop is in force none runs: an object that goes then is gone, but a
  `weakref.WeakSet` or `WeakValueDictionary` that would have forgotten it from its
  callback goes on counting it. The same goes for an `except` or `finally` that
  would have released something.
- **A standard-library lock can be left held.** A stop that lands between a
  lock's `acquire()` and its `release()` leaves it acquired, because the
  `except` block the standard library has there - written for Ctrl-C - does not
  run. Under 3.12 `Thread.join()` was the case that showed it: stopped inside it,
  the thread's bookkeeping was never finished, `Thread.is_alive()` answered true
  for a thread that had ended, and `~Isolate` had to drop the lock so as not to
  wait on it. 3.13 moved joining into C thread handles, which a stop does not
  leave half-done (`teardown: TerminateExecution stops a script's threads too,
  and an interrupt waits for the isolate's thread`). A lock a script's own code
  held is still the script's to lose.

A native that returns the instant it sees `IsExecutionTerminating()` could beat
the stopping thread's pending call to the next check and let the script run on,
so the trampoline raises the stop itself on the way back into Python (`termination:
a native is not interrupted, sees the stop, and the script stops when it
returns`).

**What a stop does not reach**, measured with the example REPL's `--timeout 1`:

- **A single long-running builtin.** `sum(range(10**9))` ran for about twenty
  seconds and `time.sleep(6)` for six before the stop landed; the statement
  after each did not run. They are native code, like an embedder's own callback,
  and decision 15 already says a stop does not reach native code. The same goes
  for a blocking `socket.recv`, a lock, a `subprocess` wait - on the isolate's
  thread or on one the script started.

### 6.2 Interrupts

`RequestInterrupt` rides the same pending call. Requests run in the order they
were made, once each, on the isolate's thread between two bytecodes of running
Python - never while the thread is idle, because nothing checks then (`interrupts:
requests run in order, once each, at the next script`). Each callback gets a
`HandleScope` of its own and runs with no `TryCatch` in reach, and whatever it
leaves pending is cleared: an exception left there would be raised by code that
never threw it (decision 24). One requested from inside a callback runs in the
same pass. While a stop is in force interrupts wait, and run after the cancel.

The interpreter's pending-call queue holds 32 calls; `Service` is queued at most
once at a time, so any number of requests from any number of threads share one
slot (`interrupts: many requested from many threads while a script runs all run,
once each`).

An interrupt belongs to the isolate's thread even when a thread the script
started is the one running Python: the pending call leaves it queued there and
re-queues itself until the isolate's thread next checks.

### 6.3 Jobs, promises, and the event loop

**A promise is an `asyncio.Future`, and the engine's job queue is an asyncio
event loop** - one per isolate, a `SelectorEventLoop` made when the isolate is
and set as its thread's current loop, so `asyncio.get_event_loop()` at the top of
a script finds it rather than making one nobody drains. A task is a future, so a
coroutine script drives is a `ub::Promise` too (`promises: a future and a task
script made are promises too`).

The loop is **never run forever**. `PumpJobs` iterates it - `loop._run_once`
with `_stopping` set, so every iteration polls with a zero timeout - until
nothing is ready, then runs one piece of posted work, then drains again, exactly
the alternation decision 23 describes. Ready work runs, timers that have fallen
due join it, and a timer not yet due never blocks a pump (`promises: a timer that
is not due does not block the pump`). The running-loop state `run_forever` would
set up is set up in C around the drain, with no Python frame in between, so that a
stop cannot land half-way through it and leave the loop believing it is still
running. Callbacks see the loop running, so `asyncio.get_running_loop()` works in
a continuation.

- **Settling from C++.** `Promise::New` is `loop.create_future()`. `Resolve` with
  an awaitable follows it, as a JavaScript promise resolved with a thenable does;
  resolving a promise with itself rejects it with a `TypeError`; a second settle
  answers false. `Reject` with an exception sets it; with anything else it sets
  `unibind.Thrown(value)`, which `await` raises and a `TryCatch` unwraps back
  into the value. A cancelled future reads as rejected.
- **Nothing a pump runs reaches a `TryCatch`.** A pump is not a call; what a job
  or a callback raises is dropped, and asyncio's own "Task exception was never
  retrieved" is silenced by the loop's exception handler, because an unhandled
  rejection is not reported on either JavaScript engine either.
- **A stop in a pump** discards what is ready - V8 empties its microtask queue
  when a stop lands in a checkpoint - and leaves timers and posted work for after
  the cancel.
- **An isolate destroyed with tasks pending** closes each coroutine, so an
  unstarted one does not warn that it was never awaited and a suspended one gets
  the `GeneratorExit` collection would have given it, and closes the loop
  (`promises: an isolate destroyed with pending tasks, timers and futures goes
  quietly`).

Scripts use asyncio as they would anywhere else, with two consequences of the
loop being the isolate's rather than theirs:

- **`asyncio.run()` works in a script that has no top-level `await`** - it makes
  a loop of its own (a `ProactorEventLoop`, which the overlay port's patch 0102
  lets a sub-interpreter make), runs it to completion inside the call, and
  closes it (`stdlib: asyncio - run, sleep and gather`). Closing it sets the
  thread's current loop to none, which would leave the next script's
  `asyncio.get_event_loop()`, `asyncio.Future()` and `asyncio.ensure_future()`
  with nothing to find. So the isolate installs an event-loop policy that falls
  back to the isolate's own loop, on the isolate's thread, whenever no loop is
  set: after `asyncio.run()` those three find the isolate's loop again, and a
  script that sets a loop of its own still gets that one (`teardown: after
  asyncio.run the isolate's loop is the current one again`). In a script that
  *does* use top-level `await`, `asyncio.run()` is `RuntimeError: asyncio.run()
  cannot be called from a running event loop`, as it is in plain Python, because
  the whole script is then a task on the isolate's loop.
  Measured with the example REPL; no case pins either.
- **The isolate's loop is a `SelectorEventLoop`**, which on Windows has no
  subprocess or pipe support: `await asyncio.create_subprocess_exec(...)` at the
  top level is `NotImplementedError`. Inside `asyncio.run()`, on its Proactor
  loop, the same call works.

Posted work (`PostJob`, `PostDelayedJob`) is the other two backends' contract,
unchanged: ordered, never coalesced, run only by `PumpJobs` with no realm entered
and no handle scope open, dropped at teardown, and waiting out a stop.

### 6.4 Threads a script starts

`allow_threads` is on: asyncio resolves host names on a worker thread, and a
standard library without `threading` is not the standard library. So a script can
start threads in its isolate's interpreter, and what they may do and when they
end are the backend's to decide.

**They share the isolate's GIL, so they run only while the isolate's thread lets
them**: while it is running Python (CPython switches threads every few
milliseconds) or blocked in a call that releases the GIL - `time.sleep`, a
socket read, `Thread.join()`. Between scripts the isolate's thread holds the GIL
and a script's threads wait, however long the embedder takes before its next
`Evaluate` or `PumpJobs`. A thread started to do work "in the background" does
it only while script is running.

**They cannot use the `unibind` module or anything the embedder bound.** A native
function, a class constructor, a template's type, an interceptor and every
attribute of a `unibind.Object` find their isolate through the calling thread,
and a thread the script started has none: each raises `RuntimeError: unibind: no
isolate on this thread` (`lifetimes: an instance that dies on a script's thread
gives its native back on the isolate's`; measured with the example REPL for
`unibind.Object` and a bound function). Plain Python - lists, dicts, the standard
library - works on them as anywhere. A native whose instance happens to die on
one is not destroyed there (section 4.5).

**`TerminateExecution` stops them** along with the isolate's thread (section 6.1).

**`~Isolate` stops them, and waits up to two seconds.** `Py_EndInterpreter` may
not run while another thread is alive in the interpreter - it is a fatal error -
and for a `threading.Thread` CPython would first join it, for ever if it never
ends. So before anything else, `~Isolate` puts a stop on every thread of the
interpreter but its own, and releases the GIL in short slices so that they can
meet it and finish. Running Python meets it at its next check; a thread blocked
in a call meets it when the call returns. A thread still there after two seconds
is inside a call that has not returned - a sleep longer than that, a lock nobody
will release, a read nothing will answer:

- **The interpreter is left behind rather than ended under it.** The isolate
  unhooks everything that leads back to it, gives back its natives as usual, lets
  go of the interpreter and its GIL, and records it with the platform. The
  isolate's thread is free for a new isolate at once. The stop stays on the
  abandoned thread, so when its call returns it ends too (`teardown: a thread in
  a short blocking call is waited for, and one that sleeps past the grace is left
  behind`).
- **`~Platform` ends such an interpreter if its threads have finished by then.**
  If one still has not, CPython cannot be finalized at all - `Py_FinalizeEx` with a
  sub-interpreter left is a fatal error - so `~Platform` skips `Py_FinalizeEx` and
  leaves the process to end with CPython still initialized.

**The trade-off is deliberate: a script's threads die with its isolate.** Plain
CPython runs non-daemon threads to completion at exit; here an embedder
destroying an isolate is the one deciding when that isolate's work ends, and an
isolate whose destruction waited on a script's thread for ever would be a hang the
embedder could do nothing about. Work a script wants finished, it joins before the
script ends. A stop that lands on a thread is silent - nothing is printed for it
(`teardown: a threading.Thread still running Python when the isolate goes is
stopped, quietly`, `teardown: a raw _thread thread still running when the isolate
goes is stopped, not fatal`).

## 7. Heap and stack

### 7.1 A heap limit, from allocator hooks

CPython has no per-interpreter heap limit and no statistics to speak of. What it
has is `PyMem_SetAllocator`, which accepts hooks before the interpreter comes
up - and that is enough for both.

`Platform` wraps the **MEM** and **OBJ** domains - where every Python object and
most of the interpreter's working memory come from - with hooks that put a
16-byte header in front of every block: the account it is charged to, and its
size. The account is the allocating thread's isolate (one isolate per thread, so
a thread-local is the whole attribution), and the header is what lets a free, on
any thread and at any time, credit the right one. 16 bytes keeps every block
16-aligned, which is what both allocators underneath promise. An account is never
freed, because a block charged to an isolate can be freed after the isolate has
gone; one whose count has fallen back to zero is reused by the next isolate.
CPython 3.14 gives every block of a sub-interpreter back when it ends, so an
account is reused, and **the backend keeps nothing per isolate**: the pool holds
as many accounts as there were ever isolates alive at once, and the suite
asserts that (`lifetime: many isolates in sequence and on many threads leak
nothing of this area's`). Under 3.12, which never freed a sub-interpreter's
arenas, one account - tens of bytes - stayed per isolate.

What is and is not counted:

- The **RAW** domain is not wrapped. It is what CPython uses for thread and
  interpreter states and for everything it allocates without the GIL, and a
  refusal there is one CPython treats as fatal rather than as a `MemoryError`.
- What the interpreter allocates **while it comes up** is not counted - it is not
  the script's, and importing asyncio for the loop is several megabytes of it.
- An allocation made **on another thread** - a `threading.Thread` the script
  started - is charged to no isolate, and so is outside the budget.

With `heapLimitBytes` set, an allocation that would cross it is refused, which
CPython turns into a **`MemoryError` the script can catch**, and
`EngineFault::OutOfMemory` is reported to the platform's handler - once per
crossing, not once per refused allocation, so a script failing allocation after
allocation in one `except MemoryError` loop is one report (`heap: a script that
reaches the limit gets a MemoryError it can catch, and a fault is reported`). A
crossing ends when usage falls an eighth of the limit below it, or the limit is
raised. Nothing is collected first: CPython collects only at points where every
object is consistent, and inside an allocation is not one.

**`Isolate::SetHeapLimitCallback` is defined on this backend**, as on V8 and
unlike SpiderMonkey: it is asked once per crossing, before the allocation is
refused, and the ceiling it answers is adopted - clamped up, never down. The
policy `unibind/isolate.h` recommends, raise *and* terminate, works as written:
the raised ceiling gives the unwind room and the stop ends the script (`heap: the
limit callback raises the ceiling and stops the script - the rescue`). A limit
belongs to its isolate, not to the thread or the process.

`GetHeapStatistics`:

| field | here |
|---|---|
| `usedBytes` | what is charged to the isolate now, headers included |
| `totalBytes` | the same. CPython's object allocator reserves arenas per interpreter with no public way to ask for the figure, so what is charged is also what is held |
| `limitBytes` | `heapLimitBytes` (or what the callback raised it to), else the system commit limit |
| `mallocedBytes`, `peakMallocedBytes` | exact: everything charged came through malloc-shaped hooks |
| the physical, external and global-handle figures | empty |

`RequestGarbageCollection` is `gc.collect()`, keeping whatever exception is
pending.

### 7.2 Stack

`stackLimitBytes` is measured from `Isolate::New`, as on the other two, and **held
to the thread's real stack**: the budget is `stackLimitBytes` or what is left of
the thread's stack below that point less a headroom, whichever is smaller. The
headroom - 128 KiB, 192 KiB against a debug CPython - is CPython's own margins
below its limit (below) and room past them for whatever catches the error. A
limit past the end of the stack is no limit at all, so it is clamped rather than
believed (`stack: a stackLimitBytes past the end of the thread's stack is held to
the stack`). 0 means the whole stack less the headroom.

That floor is one address, enforced twice:

- **CPython's own check.** CPython 3.14 guards recursion through C with the
  stack pointer: every entry into its evaluation loop, and every C-level
  recursion (a `repr` of a nested object, a call through `tp_call`), compares it
  with the thread state's *soft limit* and raises `RecursionError` below it
  ("Stack overflow (used N kB)"). One *margin* below that is a hard limit, past
  which it gives up with a fatal error. `PyUnstable_ThreadState_SetStackProtection`
  places the soft limit on the isolate's floor. Python-to-Python calls cost no C
  stack and are held by `sys.getrecursionlimit()` (1000), which is left alone.
- **Every native entry** - a native function, an accessor, an interceptor hook, a
  template constructor - compares the stack pointer against the floor, and fails
  the call with `RecursionError` below it. A native calling a native never
  enters the evaluation loop, so CPython's check never sees it (`stack: native
  recursion that never enters Python is a RecursionError, not a crash`, `stack:
  recursion bouncing between a native and Python is a RecursionError, not a
  crash`).

The margin has to be more than CPython spends between two checks. A release
build spends 2.5 KB a level through `map` and 7 KB through `sorted(key=...)`,
well inside its 16 KB (x64). A debug build, whose evaluation loop MSVC leaves
unoptimised, spends 52 KB and 58 KB - more than upstream's 32 KB, so a runaway
recursion could step over the soft limit into `Fatal Python error:
Unrecoverable stack overflow`; the overlay port's patch 0105 makes the debug
margin 64 KB. The same figure is the other thing a debug build changes: a 1 MB
thread holds only about fifteen levels of recursion through C, where a release
build holds hundreds.

Under 3.12 CPython counted instead of measuring (`c_recursion_remaining`, which
the backend set from the budget), and **a builtin with a large frame** outran
the count: `sorted` keeps a 2 KB merge buffer on the stack, and a recursion
through its `key=` overflowed the stack - a stock `python.exe` 3.12's too. It is
a `RecursionError` now (`stack: runaway recursion is a RecursionError, not a
crash, on threads of several sizes`).

## 8. Binary data, structured clone, and the code cache

**Binary data.** An `ArrayBuffer` is a `bytearray` - Python's own mutable byte
string, which every Python library already takes - and a `bytes` is a read-only
one. What Python has no word for is a *typed window* onto one, so the `unibind`
module has two: `unibind.TypedArray` (`TypedArray('int32', buffer_or_length_or_
iterable, byteOffset, length)`; JavaScript's names, `'Int32Array'`, work too) and
`unibind.DataView` (`getInt8` ... `setBigUint64`, big-endian unless
`littleEndian=True`). Both hold a reference to their buffer and read and write its
storage directly, with JavaScript's element conversions: integer types wrap,
`uint8clamped` clamps and rounds half to even, `float32` and `float16` round to
nearest, the BigInt types take `int`s only (`binary: element conversions follow
JavaScript at every boundary`). A `TypedArray` is a sequence to Python, exports
the buffer protocol, and a slice of it is a copy while `subarray()` shares.

Python has no detaching, but it has something with the same consequence: script
can **resize** a `bytearray` under a view. A view therefore never keeps a
pointer; every access re-reads the buffer's storage and size, and a window that no
longer fits reads as a view over nothing - length and offset zero, what decision
21 asks of a detached buffer (`binary: a view whose buffer shrank reads as a view
over nothing`). A buffer exported through the buffer protocol cannot be resized
while the export lives, which is CPython's own rule.

**A buffer too large to allocate is made by growing an empty one**, never with
`PyByteArray_FromStringAndSize(NULL, n)`. In CPython - 3.12, and 3.14.7 still -
that call, when it cannot get the storage, frees the half-made `bytearray` before it has set the
field that counts buffer exports, and the deallocator reads whatever the
allocator left there: when that happened to be positive, CPython printed
`SystemError: deallocated bytearray object has exported buffers` as an
unraisable exception, after `ArrayBuffer::New` or `TypedArray(..., n)` had
correctly failed. An empty `bytearray` is made whole, and resizing it fails
cleanly with `MemoryError`, so an unallocatable buffer is now just empty
(`lifetimes: a bytearray too large to allocate is refused without a word about
exported buffers`). Anything else of the embedder's that calls that function
with a null pointer has the same bug.

**Structured clone** is `marshal` over a graph the backend builds: the value
becomes a flat list of nodes, each a tag and plain data, with children named by
their index in the list. Indices are what make shared references and cycles
survive, and flatness is what keeps both directions free of recursion - a list
nested a million deep costs heap, not stack (`clone: deep nesting costs heap, not
stack`). `pickle` was not an option: loading a pickle calls whatever the pickle
names. Loading this runs nothing, and every node is checked for exactly the shape
the writer gives it before anything is built from it.

What clones: `None`, `unibind.null`, `bool`, `int` of any size, `float` (NaN and
-0 to the bit), `str` (lone surrogates included), `bytes`, `bytearray`, `list`,
`tuple`, `dict` (with keys of any clonable kind), `TypedArray` and `DataView`
(sharing their buffer node), and a **plain** `unibind.Object`, as its enumerable
own string-keyed properties, getters run, into a fresh object with no prototype.
What does not - a function, a Symbol, an External, a promise, a template or class
instance, a `set`, an instance of any other class, and a subclass of a clonable
type, whose class could not be rebuilt from the data - fails the whole call with
`unibind.DataCloneError` pending (decision 22).

**The code cache** is `marshal` too, of the pair of code objects a `Script`
holds: CPython's own compiled-code format, the one a `.pyc` is, and exactly as
portable as one - one interpreter version, one bytecode magic number. A script
compiled from a blob still has its source registered with `linecache`, so its
tracebacks still quote lines (`codecache: a script compiled from a blob still
quotes its lines in a traceback`).

Both blobs sit behind a 32-byte header of the backend's own - magic, format,
marshal version, `PY_VERSION_HEX`, bytecode magic, payload length and FNV-1a
hash - which is checked in full **before `marshal` reads a byte**. That matters most
for code: `marshal.loads` trusts a code object's fields, and a damaged one can
take the process down. A blob from another interpreter version, a truncated one,
one with bytes appended and one with a bit flipped are all refused there
(`clone: a damaged blob is refused, never misread or crashed on`, `codecache: no
blob, garbage and damage all compile the source instead`). Under that, the
source-keyed frame `unibind/script.h` puts on every code-cache blob (decision 19)
still applies; this backend adds a check that the code was compiled under the
same script name, and nothing it could replace.

## 9. What is not here

- **The inspector.** CPython's debugging surface is `sys.monitoring` and the
  pdb/debugpy family built on it, which speak the Debug Adapter Protocol if they
  speak anything. A Chrome DevTools inspector is not something this backend can
  honestly offer, so, as on SpiderMonkey, `Inspector::Supported()` is false,
  `New` returns null, and every member links (decision 29).
- **`unibind/interop/v8.h`** is V8's alone; a call to it does not link.
- **`SharedArrayBuffer`**: nothing in Python is one, so no view is over one.
- **`EngineFault::Fatal` is raised only at bring-up** - the standard library not
  found (possible only with it not embedded, section 10.3), or
  `Py_InitializeFromConfig` failing - and there it does **not** end the
  process: the `Platform` is left existing and not initialised, and every
  `Isolate::New` answers null, as decision 11 says of a platform that failed to
  bring its engine up. A fatal error inside CPython later - `Py_FatalError` - is
  not hooked: it prints and aborts without reaching `onEngineFault`.
- **A published prefix.** The release workflow builds the two JavaScript
  backends; a python prefix is built from this tree.

## 10. The standard library, and shipping a program

### 10.1 A static CPython with its extension modules built in

The engine is vcpkg's `python3` port, built for the `*-windows-static` triplets:
a static library against the static CRT, the same shape as the other two
engines. The root `CMakeLists.txt` turns on the `python` manifest feature and adds
`cmake/vcpkg-ports` as an overlay when `UNIBIND_BACKEND=python`, so nothing is
fetched by hand. `UNIBIND_PYTHON_DIR` points at another prefix with the same
layout instead.

A static CPython on Windows cannot load an extension module: every `.pyd` links
`python3X.dll`, which does not exist here. So the overlay port compiles the
standard library's extension modules *into* the static library as built-in
modules - `_socket`, `select`, `_ssl`, `_asyncio`, `_sqlite3` and the rest - and
nothing ever looks for a `.pyd` (`stdlib: the extension modules are built in, and
nothing looks for a .pyd`). `cmake/vcpkg-ports/README.md` has the list, what each
needs, and the patches that make it work. The same fact means **a third-party
package with a C extension cannot be loaded** by this backend; a pure-Python one
can, from wherever the embedder puts it on `sys.path`.

### 10.2 What a sub-interpreter refuses

An isolate is an own-GIL sub-interpreter with `check_multi_interp_extensions` on,
and an extension module that keeps state in C globals cannot be shared between
interpreters with GILs of their own. CPython's answer is to refuse it, and so is
this backend's. `import X` fails with `ImportError: module X does not support
loading in subinterpreters` and leaves nothing behind (`stdlib: the few modules
not safe under a GIL of their own refuse an isolate`). In 3.14 that is three
modules:

| refused | why | what still works |
|---|---|---|
| `_wmi` | does not declare per-interpreter-GIL support | nothing (`platform` does without it) |
| `_tracemalloc` | single-phase init | nothing: no `tracemalloc` |
| `_suggestions` | declares it cannot be shared | `traceback` works out its "Did you mean" hints without it |

Every other built-in module loads, with state of its own per interpreter:
`ctypes`, `decimal` and `datetime` run on their C modules, `zoneinfo` on
`_zoneinfo`, and `xml.etree`, `xml.dom.minidom` and `xml.sax` parse with
`pyexpat` (`stdlib: ctypes, decimal, datetime, zoneinfo and XML run on their C
modules in an isolate`). Under 3.12 the list was much longer - `_ctypes`,
`_decimal`, `_msi` and the core's `_datetime` had single-phase init, and
`pyexpat` and `_elementtree` were not isolated yet (gh-103092) - so an isolate had
no ctypes and no XML parsing, and `decimal` and `datetime` ran on `_pydecimal`
and `_pydatetime`. 3.13 and 3.14 made them multi-phase, removed `_msi`, and made
the refusal itself uniform: a single-phase module's init now runs under the main
interpreter, and is refused in an isolated one afterwards, whether it is a `.pyd`
or built in. (3.12 checked a built-in only once some interpreter had it, which
the overlay port used to patch.) Everything works in the main interpreter, which
never runs a script.

`zoneinfo` works, but Windows has no tz database: `ZoneInfo('Europe/London')`
needs the `tzdata` package on `sys.path`. `ssl` finds the Windows certificate
stores by itself, from any number of isolates at once: `_ssl`'s
`certEncodingType` used to cache two strings in C `static` variables, made by
whichever interpreter got there first and reference-counted by all of them, and
isolates making default contexts on several threads crashed; the overlay port's
patch 0104 makes them per call (`stdlib: ssl - default contexts made in isolates
on many threads at once`).

### 10.3 Where the standard library comes from

The C half of CPython is linked in, and by default so is the pure-Python half -
`Lib/`, with `os.py`, `asyncio/`, `json/` and the rest. **A program ships as one
executable, with nothing beside it.**

**How it is embedded.** With `UNIBIND_PYTHON_EMBED_STDLIB` on (the default), the
build runs `src/backends/python/freeze_stdlib.py` with the engine prefix's own
interpreter, `tools/python3/python.exe` - the same CPython as the library, so the
bytecode and the marshal format are the ones it reads. Configure checks the
version, and the generated source `static_assert`s it. The script compiles every
module of `tools/python3/Lib` the exclusion list leaves in, `marshal`s each code
object, and writes the bytes end to end into one file, which a generated C++
source `#embed`s and indexes as CPython's own frozen-module table - one
`struct _frozen` per module: name, code, size, whether it is a package. That
source is part of `unibind_backend_python.lib`, so a program gets the standard
library by linking the backend, from the build tree or from an installed prefix
alike. It is regenerated when a file under `Lib`, the script, the interpreter or
one of the options below changes.

When the `Platform` is made with no directory given (below), it points
`PyImport_FrozenModules` at that table before `Py_InitializeFromConfig`.
CPython's `FrozenImporter` sits ahead of the path finder on `sys.meta_path` and
serves every standard-library `import` from it, including the ones CPython makes
while it starts (`encodings`, `io`, ...). CPython looks in the table after its
own bootstrap modules (`importlib._bootstrap`, `_bootstrap_external`,
`zipimport`) and before its own frozen copies of `os`, `codecs` and the other
start-up modules, so those come from the table too; `use_frozen_modules` gates
only CPython's copies. Each interpreter unmarshals a module the first time it
imports it, into code objects of its own, from the one read-only copy of the
bytes in the image - which is what makes it safe for own-GIL isolates on many
threads at once (`stdlib embedded: isolates on many threads unmarshal the same
table at once`).

**What a frozen module looks like.** `__spec__.origin` is `"frozen"` and
`__loader__` is `FrozenImporter`; there is no `__file__`, and a package's
`__path__` is `[]` - its submodules are found by name in the table, not by
searching a directory. This is what CPython's own frozen `os` has in any
embedding that does not tell it where its standard library is, and the standard
library is written for it: `logging`, `importlib`, `inspect` and the rest check
for a missing `__file__`. Every module in the table was imported in an isolate
both ways, embedded and from disk, and the same 36 failed both ways - the ones
section 10.2 lists and the ones that need a POSIX-only module (`curses`, `pty`,
`tty`, `dbm.gnu`, ...). `sys.path` is empty; `sys.prefix` and `sys.exec_prefix`
are the program's directory; nothing on disk is read for an `import`, and nothing
is written.

**A traceback** into the standard library names CPython's own file name for
frozen code, and the line, without the line's text - there is no source to read
it from (`stdlib embedded: a traceback into the standard library names the
module and the line`):

```
  File "<frozen json.decoder>", line 354, in raw_decode
json.decoder.JSONDecodeError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)
```

`inspect.getsource` of a standard-library function raises `OSError`, as it does
for CPython's own frozen modules. The sources would be about 9 MB more (2 MB
compressed) for something a developer can have by pointing `UNIBIND_PYTHON_HOME`
at `Lib`, so they are not embedded.

**What is left out**, by `UNIBIND_PYTHON_EMBED_STDLIB_EXCLUDE` - dotted module
names, each with everything under it: `test` (CPython's own test suite, 31 MB of
the 46 MB), `idlelib`, `tkinter`, `turtle` and `turtledemo` (no Tk here),
`ensurepip` and `venv` (nothing to install into), `lib2to3`, and `pydoc_data`
(the topic text behind `pydoc.help('if')`, which then says it is not available).
`site-packages` and `__pycache__` are never walked - neither is a package name -
and neither is a directory without an `__init__.py`. What remains is 514
modules, **9.1 MB of marshalled code** (CPython 3.12, x64).

`UNIBIND_PYTHON_EMBED_STDLIB_OPTIMIZE` is `compile()`'s `optimize`: 0, the
default, keeps asserts and docstrings, as the interpreter runs everything else;
2 drops both, for 7.7 MB. Debug and Release embed the same bytes: a `Py_DEBUG`
CPython reads a release build's bytecode (the two share `.pyc` files), and both
run at optimization level 0. On x86 the interpreter that compiles it is the x86
one, which runs on an x64 host.

**Data files.** Nothing the standard library imports needs one: outside the
excluded packages `Lib` holds no file but `.py` ones except four notes and
scripts in `ctypes/macholib` and `email`, and the modules
that use `__file__` or `importlib.resources` (`pydoc`, `doctest`, `unittest`'s
discovery, `trace`, `zoneinfo` for the separate `tzdata` package) do so for the
caller's files, not their own. `importlib.resources.files()` of a
standard-library package has nothing in it, and
`pkgutil.iter_modules(json.__path__)` lists nothing.

**What it costs and saves**, measured on x64 Release:

| | from disk | embedded |
|---|---|---|
| `unibind_python_repl.exe` | 16.4 MB | 26.0 MB (+9.6 MB) |
| a new isolate that imports `json`, `re`, `dataclasses` and `typing` (median; `stdlib embedded: isolate start-up, measured`) | about 300 ms | about 45 ms |
| the REPL running `-c pass`, from process start to exit | about 380 ms | about 130 ms |
| process memory kept per isolate made and destroyed (section 11) | 9.7 MB | 8.6 MB |

From disk, every isolate compiled every module it imported from source - no
`.pyc` comes with vcpkg's `Lib`, and none is written - and `asyncio`, which every
isolate imports for its loop, is the bulk of it. Embedded, it unmarshals
bytecode instead. The bytes themselves are in the image, shared by every isolate
and paged in as they are read.

**A directory still wins**, so a developer can run against sources on disk, with
line text in tracebacks. `FindStandardLibraryDirectory` in `core.cpp` takes the
first of these that holds `os.py`:

1. **`UNIBIND_PYTHON_HOME`**, an environment variable: that directory, or its
   `Lib` subdirectory;
2. **`python-stdlib`**, a directory beside the running executable;
3. **the embedded standard library** - no directory at all.

With a directory, the frozen table is not installed, `sys.path` is that one
directory and `sys.prefix` its parent: everything is as it was before the
standard library was embedded. `python.stdlib-embedded.unibind-python-home-still-wins`
runs the `stdlib embedded` cases that way. In either case there is no
`site-packages`, no current directory and nothing from the environment on
`sys.path`.

**Built with `UNIBIND_PYTHON_EMBED_STDLIB` off**, nothing is embedded and the
backend reads `Lib` from disk, with a third place to look, last: **the path the
build found it at** - `<build>/vcpkg_installed/<triplet>/tools/python3/Lib`,
compiled in as `UNIBIND_PYTHON_DEFAULT_STDLIB` - which is why such a program
works on the machine that built it and fails on the next one. If none of the
three holds it, the `Platform` reports `EngineFault::Fatal` ("the Python
standard library could not be found") and is not initialised. Shipping such a
program means copying `tools/python3/Lib` beside it as `python-stdlib`, or
pointing `UNIBIND_PYTHON_HOME` at a copy. A backend with the standard library
embedded never looks at the build-tree path: a program that works only on the
machine that built it is the failure embedding exists to end, and the embedded
copy is the same `Lib`.

Embedding needs the prefix's `tools/python3/python.exe`. A `UNIBIND_PYTHON_DIR`
without one stops at configure and says so, rather than compiling with whatever
other Python is at hand; turn the option off for such a prefix.

### 10.4 Linking

Beside `python314.lib` (`python314_d.lib` for Debug) a program links the
third-party libraries its built-in modules call, which stay vcpkg's own static
libraries rather than being merged in - so a program that uses OpenSSL or SQLite
itself links one copy of each: zlib, OpenSSL (`libssl`, `libcrypto`), libffi,
SQLite, expat, liblzma, bzip2, libmpdec and zstd. And from Windows: `version
ws2_32 shlwapi pathcch bcrypt advapi32 user32 kernel32 ole32 oleaut32 iphlpapi
rpcrt4 crypt32 winmm wbemuuid propsys`. `unibind::backend_python` and the install
tree's `unibind-backend-python.cmake` name all of them; nobody should be
assembling that list by hand.

An MSBuild project gets the same list from `msbuild\unibind.props` with
`UnibindBackend=python`: the install tree's `unibind-engine-python.props`
records the engine's libraries relative to `UnibindPythonDir` - vcpkg's
installed triplet directory, defaulted to the one this prefix was built against -
once for Release and once for Debug, and `unibind.props` picks by
`UseDebugLibraries`. `examples/embed/embed.vcxproj` links this way against a
python prefix in both configurations.

The CRT is `/MT` (`/MTd` in Debug), as for the other two, and a `/MD` consumer
fails at link with LNK2038. `Py_NO_LINK_LIB` keeps `pyconfig.h`'s `#pragma
comment(lib)` from naming the import library that does not exist - which 3.14's
`pyconfig.h` honours itself, where 3.12's needed a vcpkg patch.

## 11. What CPython itself costs

These are the engine's, not the backend's, and each is the kind of thing that
decides how an embedding is shaped:

- **An isolate gives its memory back - if nothing of it is left.** CPython 3.14
  frees a sub-interpreter's object arenas when it ends, but only when not one
  block of them is still allocated; one reference kept past the end - a cycle the
  collector cannot see, an object a native still holds - keeps all of them, a few
  megabytes. The backend itself had one such cycle until the move to 3.14, in its
  `Symbol` type, and it cost 3.5 MB an isolate; `lifetime: many isolates in
  sequence and on many threads leak nothing of this area's` now measures what an
  isolate made, used and destroyed costs the process - under 1 MB on average is
  the bound, and it measures about nothing - and `stress: process memory over
  many isolates made and destroyed` shows 1000 of them levelling off at about 8
  MB in all. **Under 3.12** a sub-interpreter's arenas were never freed: about
  9.5 MB an isolate, for good, and on x86 a few hundred isolates filled the 2 GB
  address space a 32-bit process gets by default. The suite and the REPL are
  still linked `/LARGEADDRESSAWARE` on x86 - 4 GB on 64-bit Windows, for
  nothing - but no longer need it. Making an isolate still costs tens of
  milliseconds (section 10.3), so long-lived isolates - one per worker thread -
  are still the shape to aim for.
- **An interpreter cannot be ended under a thread that is still in it.**
  `Py_EndInterpreter` with another thread alive is a fatal error, and it joins
  every non-daemon `threading.Thread` first, for ever if one never ends. Section
  6.4 is what the backend does about it: stop them, wait two seconds, and leave
  the interpreter behind - and, at `~Platform`, possibly CPython unfinalized -
  when one is stuck in a call that never returns.
- **No daemon threads** also means library code that makes one fails:
  `subprocess.run(..., capture_output=True)` raises `RuntimeError: daemon threads
  are disabled in this interpreter`, because its Windows implementation reads the
  pipes on daemon threads. `subprocess.run` without capturing works, as does
  `os.system`.
- **Each isolate's thread is its interpreter's main thread.** To `threading`, a
  sub-interpreter's first thread is `threading.main_thread()`, while signals are
  delivered only to the main interpreter: `signal.signal` raises `ValueError:
  signal only works in main thread of the main interpreter`, and asyncio's
  Proactor loop fails for the same reason without the overlay port's patch 0102
  (still so in 3.14).
- **A debug CPython needs big stacks.** Its evaluation loop, unoptimised, costs
  about 50 KB of stack for every level of recursion through C - an import inside
  an import, a callback into Python from a native - so a 1 MB thread holds about
  fifteen, where a release build holds hundreds (section 7.2).
- **A script can make interpreters of its own.** 3.14's
  `concurrent.interpreters` works inside an isolate: a script can create, run
  and close sub-interpreters on its own thread. They are not isolates - the
  backend knows nothing of them - and what they allocate is charged to the
  isolate whose thread made them.

Two costs of 3.12 are gone. Starting an interpreter no longer swaps the
process-wide RAW allocator: 3.12's `_Py_ClearStandardStreamEncoding()` did,
unlocked, under other own-GIL interpreters, and a debug CPython's heap caught
blocks going through the wrong allocator with more than a couple of isolates
starting at once - the overlay port patched that (0103) until 3.13 removed the
function. `concurrency_test.cpp`, eight threads making, using and destroying
isolates at once, is the repro, and runs clean in Debug without it. And a
recursion through a builtin with a large frame is a `RecursionError`, not a
crash (section 7.2).

## 12. Python is not a sandbox

A JavaScript engine starts with nothing: no file system, no network, no
processes, only what the embedder binds. CPython starts with everything. In an
isolate, script can `open()` any file the process can, `import socket` and
connect anywhere, `os.remove`, `os.system`, `subprocess.run`, read the
environment, and `import` anything on `sys.path`. None of that goes through a
binding the embedder wrote, and none of it can be taken away by one: removing
`open` from `builtins` is undone by `import io`, and CPython's own documentation
is plain that restricted execution is not a thing it offers. Since 3.14 an
isolate has `ctypes` too (section 10.2), and with it any address in the process.

What the backend does refuse - `fork`, `exec`, daemon threads, signal handlers -
it refuses because an embedded engine must not do those things *to its host*, not
to contain a script. **Run only Python you would run as the host process
itself**, or put the process in a sandbox of the operating system's: a
restricted token, an AppContainer, a job object, a container. Nothing in this
library is a substitute.

## 13. Build notes, and what an upgrade has to recheck

- **The overlay port** (`cmake/vcpkg-ports/python3`) is vcpkg's own port for
  3.14.7 - vcpkg master's, since the manifest baseline still has 3.12 - with five
  patches of ours: 0100 makes `/GL` respect `WholeProgramOptimization=false`,
  because `lld-link` cannot read LTCG objects and `link.exe` reads only its own
  version's; 0101 builds the extension modules in; 0102 lets asyncio's Proactor
  loop be made in a sub-interpreter; 0104 stops `_ssl` sharing two strings between
  interpreters (section 10.2); 0105 widens a debug build's stack margin (section
  7.2). A second overlay, `mpdecimal`, supplies the libmpdec the 3.14 port builds
  `_decimal` against. Its README says what each patch is for, and how to
  regenerate them when the version moves.
- **The embedded standard library** (section 10.3) is compiled by the prefix's
  own `tools/python3/python.exe`, whatever version that is: nothing in
  `freeze_stdlib.py` or the CMake around it names one, configure refuses an
  interpreter whose X.Y is not the library's, and the generated source
  `static_assert`s the same. On an upgrade, recheck what it leans on: the
  fields of `struct _frozen` (`name`, `code`, `size`, `is_package`), which the
  generated table names; `look_up_frozen` in `Python/import.c` consulting
  `PyImport_FrozenModules` after the bootstrap modules and before CPython's own
  frozen copies, whatever `use_frozen_modules` says; and that a Debug CPython
  still reads a release build's bytecode. Two build details: clang 19 hands an
  `#embed`-ed byte to C++ as a signed `char`, so the generated source turns off
  the narrowing error it would otherwise raise into an `unsigned char` array;
  and the list of files under `Lib` is a `CONFIGURE_DEPENDS` glob, so a module
  added there is picked up by the next build.
- **The first configure builds CPython**, and with it OpenSSL, libffi, SQLite,
  expat, liblzma, bzip2, zstd and mpdecimal, for the triplet, plus a host
  CPython vcpkg needs to build the static one. The port's README measured about
  twenty minutes on a 32-thread machine, nearly all of it OpenSSL and libffi;
  after that it is binary-cached.
- **Reaching past the limited API.** The backend uses things CPython does not
  promise to keep, and each is a line to check on an upgrade:
  - `_PyEval_AddPendingCall` - the per-interpreter pending call, exported by the
    static library but declared only in internal headers, so its signature is
    declared by hand in `runtime.cpp`: 3.13's, with `flags` and a result of
    0 (queued) or -1 (full). The public `Py_AddPendingCall` goes to the main
    interpreter.
  - `PyUnstable_ThreadState_SetStackProtection`, and `_PyOS_STACK_MARGIN_BYTES`
    from an internal header, repeated in `runtime.cpp` - with patch 0105's debug
    value.
  - asyncio's internals: `loop._ready`, `loop._scheduled`, `loop._run_once`,
    `loop._stopping`, `loop._thread_id`, `asyncio.events._set_running_loop`, and
    a future's `_state` and `_exception`. `PumpJobs` is built on them. So is
    the event-loop policy that falls back to the isolate's loop, which reads the
    default policy's `_local._loop` and is installed with
    `asyncio.events._set_event_loop_policy`: 3.14 deprecates the public policy
    functions, and **3.16 removes the policy system** - the fallback will need
    another hook then.
  - `PyInterpreterState_ThreadHead` / `PyThreadState_Next`, to see a script's
    threads, and `PyDict_AddWatcher`, which ends a realm with its dictionary.
  - `PyCodeObject::co_flags`, `PyTracebackObject::tb_lasti`, `PyFrame_GetLasti`
    with `PyCode_Addr2Location`, `_PyType_Lookup`, `_PyType_Name`.
- **What the move from 3.12 to 3.14 changed**, for the record: the pending call's
  signature; `c_recursion_remaining` gave way to the stack protection above;
  `_Py_HashPointer` and `_PyLong_Sign` to `Py_HashPointer` and `PyLong_GetSign`;
  the marshal format (version 5) and the bytecode magic, which the code cache's
  header and `BackendBuildId` carry, so a 3.12 blob is refused and compiled
  afresh; asyncio's policy functions; `threading._shutdown_locks`, gone with
  3.13's C thread handles, which `~Isolate` used to empty; and the module list
  and refusals of section 10.2. No public header of unibind changed.
