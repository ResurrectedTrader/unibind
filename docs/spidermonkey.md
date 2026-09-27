# SpiderMonkey, and where the abstraction bent

Notes from writing the second backend against SpiderMonkey 153.3.0esr. The
purpose of a second engine is to find out whether `docs/lifetimes.md` is a
design or a description of V8, so this document starts with the one claim
everything else rests on, then lists every place the two engines genuinely
disagree and what the backend did about it.

The short answer: the design held. The suite in `tests/` runs unchanged on both
backends with no divergence - `ctest -R parity` prints one row per case and
every row reads `PASSED | PASSED` - and no public header needed a change to get
there. What did have to bend is listed in section 2, and none of it is the
handle model.

## 1. The frame is a real stack root

`docs/lifetimes.md` section 7 says a frame on this engine is a
`JS::RootedVector<JS::Value>` constructed inside the `HandleScope`. That is
exactly what it is:

```cpp
struct Frame {
    JS::RootedVector<JS::Value> slots;   // the root
    Isolate* owner; Frame* parent; uint32_t epoch;
    const JS::CallArgs* args; uint32_t argCount;
};
```

`OpenFrame` placement-constructs one into `HandleScope::storage_`, which is a
member of a stack-only object (`operator new` is deleted, copy and move are
deleted), so the storage is the caller's stack frame and nothing else.
`JS::Rooted`'s constructor pushes onto `JSContext::stackRoots_`; its destructor
pops. There is no persistent root anywhere in the handle path, no heap node
standing in for a stack one, and no trace hook of our own.

The LIFO rule is therefore not two rules that happen to agree. `~Rooted` is

```cpp
~Rooted() { MOZ_ASSERT(*this->stack == this); *this->stack = this->prev; }
```

so a debug build asserts the ordering and a release build corrupts the root
list if it is violated. The API's rule 1 ("scopes close in reverse order of
opening") is the engine's own precondition, restated. A checked build of `unibind`
additionally asserts it before the engine gets a chance to.

Measured on this build (x86, release, `UNIBIND_HANDLE_CHECKS=1`):

| | bytes |
|---|---|
| `JS::RootedVector<JS::Value>` | 104 |
| `detail::Frame` | 120 |
| `detail::TryCatchState` | 88 |
| `detail::ContextScopeState` | 8 |

against budgets of 256 / 128 / 64 in `CMakeLists.txt`. No number had to be
raised for this backend. The 104 bytes include an inline capacity of eight
`JS::Value`s, which is where `UNIBIND_FRAME_INLINE_SLOTS = 8` came from, so a frame
of up to eight handles touches no allocator at all.

### Running out of slots, and why the spill buffer is not the engine's

`RootedVector::append` is fallible, so rule 9 has real work to do here.
`Frame::Push` answers `NO_SLOT`, every slot-allocating path turns that into an
**empty** slot - an empty `Maybe` where the signature has one - and
`JS_ReportOutOfMemory` puts the condition where script will see it. Nothing
hands back a slot that reads as `undefined`, which is the failure the rule
exists to prevent.

Getting that *tested* took one more step, and it is the sharpest engine-specific
trap in this backend. The suite provokes exhaustion by replacing global
`operator new`. Two separate things hid the frame from it:

1. `JS::RootedVector` defaults to `js::TempAllocPolicy`, which allocates through
   `js_malloc`. So the frame's spill buffer is invisible to a C++ allocator
   hook. The frame now uses `JS::Rooted<JS::StackGCVector<JS::Value,
   FrameAllocPolicy>>` instead - still the engine's own stack root, still LIFO,
   still eight slots inline; only where the spill buffer comes from changes.
2. **That was not enough, and the reason is worth knowing.**
   `mozilla/cxxalloc.h`, which `jsapi.h` pulls in transitively, defines
   `operator new` and `operator delete` as `MOZ_ALWAYS_INLINE_EVEN_DEBUG`
   forwards to `moz_xmalloc`. A `::operator new` written in *any* translation
   unit that has seen a SpiderMonkey header is therefore not the program's
   replaceable `operator new`; it is `moz_xmalloc`, inlined, and an embedder or
   a test that replaced `operator new` never sees it. (The `LNK4217` warnings
   about `moz_xmalloc` in the build log are this, showing through.)

   So `frame_alloc.cpp` is the one file in this backend that includes no
   SpiderMonkey header, and it exists solely to own the two lines that call
   `::operator new` and `::operator delete`. Adding an engine include to it
   would silently redirect frame allocation back to `moz_xmalloc` and quietly
   un-test rule 9 - hence the comment at the top of it.

The payoff is that the suite needs no new API to reach this: the lever it
already has works on both engines, and the two frame-exhaustion cases assert
here instead of skipping. It also makes "frames give back everything they took"
measure something on this backend, since that test counts outstanding
`operator new`s.

### Called or constructed

A `FunctionTemplate`'s function is callable as well as constructable
(decision 12), and one trampoline serves both: `JSFUN_CONSTRUCTOR` on the
function object makes `new` legal, and `args.isConstructing()` decides which
half runs. A plain call runs the template's callback with `IsConstructCall()`
false and hands back whatever the callback wrote; `new` builds the instance
first and runs the callback against it.

Only a `Class<T>` refuses a plain call, with a `TypeError`, because there is a
native to make and nowhere to put it. The refusal therefore sits *below* the
look at `tpl->ownerClass`, not above it - above it, every template refuses, the
middle row of the grid is unreachable, and a template becomes a strictly worse
class.

The one corner left open is a template with no callback: still constructable,
but a plain call to it promises nothing, because there is nothing to run. The
backend answers `undefined` and the suite asserts nothing about it.

### One isolate per thread

SpiderMonkey permits exactly one `JSContext` per thread, and a `ub::Isolate`
is a `JSContext` here. `js/Context.h` is explicit: *"JSContext represents a
thread: there must be exactly one JSContext for each thread running JS/Wasm"*,
and `JS_NewContext` is *"create a new context (and runtime) for this thread"*.
A debug engine build asserts on the second call; the release build shipped here
walks into undefined behaviour and dies.

This is a genuine engine constraint, not something the backend does at init
that is not re-entrant - there is nothing to fix on our side of it. What the
backend does is refuse rather than die: a thread-local records the isolate that
holds the thread, a second `Isolate::New` on that thread answers empty (which
`unibind/isolate.h` already documents as what happens when a heap cannot be made),
and the slot is released when the isolate goes. A different thread gets its own,
and the first isolate keeps working after the refusal.

V8 can host several isolates on a thread and this engine cannot, so coexistence
could only ever have been a *documented capability* rather than a guarantee.
Decision 11 took the other road: the API promises at most one per thread and V8
refuses too, so the two backends behave the same rather than each allowing
whatever its engine happens to allow. That is the right call - a program that
works on one backend and not the other is the failure this library exists to
prevent - and it is worth recording that the constraint came from here.

### What the model got right

Three predictions held without argument:

- **Escaping is free and unbounded.** A `JS::Value` is 8 bytes of data, not a
  pointer into the closing frame's storage, so `EscapeSlot` is one `append` into
  the parent's vector while both frames are live roots. The "first escape is
  free, each later one costs a global root" wording in section 4 is a fact about
  V8, not about the API; here the `escapable` flag passed to `OpenFrame` is
  ignored entirely. That is the right place for the asymmetry to live - in a
  constructor flag one engine uses - rather than in a rule callers must
  remember.
- **Borrowed callback arguments cost nothing.** `JS::CallArgs` values live on
  the interpreter's stack and are already rooted for the duration of the call,
  so slots `[0, argc)` root nothing extra and reading one copies one value.
  Section 8 describes this engine accurately.
- **The index, not the address.** `RootedVector`'s buffer reallocates on growth,
  exactly as section 11.A predicted. A pointer-shaped `Local` would have been
  unimplementable without giving up stack rooting, and the test that pushes a
  hundred handles through one frame is the one that would have caught it.

### The one thing the model had to be told

`Frame::Push` can fail: `RootedVector::append` is fallible and reports OOM,
while `Slot MakeNumber(Isolate&, double)` is `noexcept` and returns a slot.
That looks at first like a hole, and the backend's first answer was a sentinel
index that read as `undefined` - which is precisely the failure the API exists
to prevent, since it makes running out of memory indistinguishable from a
property that was not there. The contract now says so in as many words
(`unibind/detail/backend.h`, decision 2): a slot-allocating function that
cannot grow the frame yields an **empty** `Slot`, and reports the condition the
way the engine reports running out of memory. `Frame::Push` answers `NO_SLOT`,
`Push` turns that into `Slot{}` after `JS_ReportOutOfMemory`, and the empty
handle reaches the caller as an empty `Local` rather than as a value. On V8 the
same append is infallible-ish (it aborts), so the rule costs that backend
nothing and is kept by this one.

## 2. Where the engines genuinely differ

### `TryCatch` does not exist

SpiderMonkey has no scoped exception handler. An exception is simply *pending*
on the `JSContext` until something takes it, and API calls signal failure by
returning `false`. So `TryCatch` is built here, not mapped: the isolate keeps a
stack of handlers, and "catching" is taking the pending exception off the
context - the first time anybody asks (`HasCaught`, `Exception`, `Message`,
`StackTrace`, or the destructor), and at the start of every operation that can
run script, because script that throws and catches its own exception clears the
context and would clear an embedder's along with it. That second take is only
into a handler opened at the current native-call depth: an exception a native
leaves behind without a handler of its own belongs to the script that called it.
Anything already pending when a handler opens is parked and put back when it
closes, because it was not thrown while the handler was in scope.

One behavioural question this backend raised and could not answer for itself:
what `~TryCatch` does with an exception nobody consumed. The header used to say
it was re-thrown to the enclosing handler unless `Reset` took it; V8's
`~TryCatch` does the opposite, and this backend followed V8 because parity with
the other backend is the point. That is now settled in the header rather than in
two backends - **closing a `TryCatch` consumes what it caught unless `ReThrow`
was called** (decision 3), with the exception of a termination, which continues
regardless.

`Message()` is the exception coerced to a string, so an `Error` yields
`"TypeError: ..."`; `StackTrace()` is `JS::BuildStackString` over the saved
frame captured with the exception. Neither format is parseable across engines,
which the header already says.

### The callback return slot needs no type erasure here

`docs/status.md` records that V8 types a callback's return slot by the hook it
belongs to (`PropertyCallbackInfo<Value>` for a getter, `<Integer>` for a
query, `<Boolean>` for a deleter, `<Array>` for an enumerator), and that the V8
backend needs a per-call function pointer to erase that.

SpiderMonkey has no such split. Every native - a function, a constructor, an
accessor, a proxy trap - writes its result into a `JS::Value`, so this
backend's `CallbackState` carries one `JS::Value* result` and the six
`SetReturn*` entry points are six stores. A hook whose answer is its C++ return
value is handed a discarded local to write into, which is the "legal, does
nothing" behaviour the header asks for, at the cost of one stack slot.

This is the clearest case in the whole exercise of the *abstraction* being
right and V8 being the odd engine. The public API was not bent to fit
SpiderMonkey here; SpiderMonkey just fits.

### An `External` is not a value kind

SpiderMonkey values are primitives or objects, with nothing in between. So
`External` is an object of a private `JSClass` holding the embedder pointer in
a reserved slot, and `KindOf` / `IsType` ask the class before they ask anything
else - which means `value.Is<Object>()` is deliberately **false** for an
External. That is this API's answer rather than either engine's: V8 15.6's
`External::IsObject()` is true, and the V8 backend excludes it by hand just as
this one does. The divergence is invisible from the API and is worth noting
only because the obvious implementation (a plain object) would have got it
wrong.

### There is no `Integer` type

V8 has `v8::Int32` as a distinct value class. SpiderMonkey has one number type
whose payload happens to be `int32` or `double`. `TypeCode::Integer` therefore
asks the *question* V8 asks - is this number an exact int32, `-0` excluded -
rather than asking how the engine happens to be storing it, so
`Local<Value>::To<Integer>()` gives the same answer on both backends for a
double that holds an integral value.

### Property attributes are inverted

V8 names what a property is **not** (`ReadOnly`, `DontEnum`, `DontDelete`);
SpiderMonkey names what it **is** (`JSPROP_ENUMERATE`, `JSPROP_PERMANENT`, with
`JSPROP_READONLY` the one that agrees). `ToNativeAttributes` is the whole of the
translation. `JSPROP_READONLY` is also rejected on an accessor, which is why the
accessor path strips it - the header already documents that `ReadOnly` is
meaningless on an ECMAScript accessor property and is ignored.

### A `Context` is a realm, and realms have to be entered

Every operation that takes a `const Context&` enters that realm for its
duration (`JSAutoRealm`), which is what makes the context parameter mean the
same thing it means on V8, where passing a `Local<Context>` also selects the
realm. `ContextScope` is a `JSAutoRealm` and nothing else: stack-only and LIFO
natively, so the API is describing the engine rather than constraining it.

A value belongs to an isolate, not to a realm, and may be used with any realm of
its isolate. V8 needs nothing for that; SpiderMonkey compartmentalises, so every
operation here wraps its incoming handles into the realm it is about to work in
(`JS_WrapValue`, a no-op when the value is already there) and unwraps
(`js::UncheckedUnwrap`) before asking what an object *is* - its class, its
reserved slots - so that a question about a class instance is asked of the
instance rather than of whatever stands in for it in the current realm.

Operations given no `Context` at all - `ArrayLength`, the `Is<T>` questions,
strict and same-value equality - enter a realm the value is legal in instead,
because the realm that happens to be current need not be one of them. For
an object that is its own realm, except for a cross-compartment wrapper, which
has none - it belongs to a compartment, and the realm entered is one in that
compartment. A question asked of the object *behind* a wrapper is asked in that
object's realm, not the wrapper's.

**Realms get a compartment each, and share one zone.** Every global an isolate
makes is created with `setNewCompartmentInSystemZone`. The compartment is what
keeps realms apart and stays per realm; the zone is what strings and atoms
belong to, and since a handle belongs to the isolate, a string made in one realm
is read, keyed on and passed from every other. Across zones that is not
allowed - a string is its zone's cell, and an atom is only kept alive in a zone
that has marked it - so with a zone per realm every such read would have had to
copy the string or mark the atom first, and a missed one is a use-after-free a
release engine does not report. One zone per isolate makes it true by
construction. Equality
between two handles from different realms picks the left one's realm and wraps
the right into it.

What is deliberately *not* guaranteed is identity as seen from a foreign realm:
a wrapper is a distinct object, so nothing portable can be said about comparing
objects across a realm boundary, and `docs/testing.md` records that the suite
does not.

### Strings are counted, not terminated

`JS_EncodeStringToUTF8` hands back a NUL-terminated buffer, which is the
obvious way to implement `Utf8Value()` and silently truncates any JavaScript
string containing a NUL. Everything that reads a string out of the engine here
goes through one helper that takes the deflated UTF-8 length first and then
copies exactly that many bytes. V8's `WriteUtf8` returns a count and has the
same hazard only if you ignore it.

### Templates are ours, not the engine's

There is no engine-side template on SpiderMonkey - no object that remembers a
shape and stamps it into a realm - so a `TemplateRec` is a descriptor and
`Materialise` replays it into one realm as a real constructor function and a
real prototype object. This is precisely the restriction `unibind/template.h`
already imposes ("what it can install directly is the closed set in
`ub::Constant`"), so the public API needed no change; the restriction exists
for engines like this one and it paid off exactly as intended.

Realms come and go while templates live as long as the isolate, so a
template's materialisation is cached **on the realm** - in a hidden reserved
slot of its global object holding a small plain object keyed by template id -
rather than in the template. The cache is traced by the engine and dies with
the realm, so there is no bookkeeping to get wrong when a context is released.

### Interceptors are a proxy, not a resolve hook

The interesting one. V8's interceptor runs on *every* named or indexed access,
ahead of the object's own properties, and may decline. SpiderMonkey's
`JSClassOps::resolve` is a different mechanism with a similar shape: it fires
only when a property is **missing**, what it defines then sticks, and there is
no setter hook at all. It cannot say "ask me again next time", which is the
entire content of `Intercepted::No`.

So an object whose template declares a handler is a `js::BaseProxyHandler`
proxy over the ordinary object that carries the template's declared properties
and, for a `Class<T>`, its native state. Proxy traps are the only construct on
this engine that run on every access and can fall through to an ordinary
lookup. The proxy has no prototype of its own - it is made with a *lazy* one,
so every read and write of its prototype goes to the handler and on to the
target. A proxy made with a concrete prototype keeps that one and answers
`setPrototypeOf` by changing it without asking the handler, while a declined
`get` forwarded to the target walks the target's chain, which then never
changes. With the lazy prototype there is one chain, the target's, and a
declined lookup walks the one the language expects - with the receiver the
access was made on, not the target, so a getter or setter up the chain sees the
proxy (or whatever inherits from it) as `this`. The target is the one object
script must never hold, since nothing on it goes through a hook. A write made on
an object that merely inherits from the proxy does not reach the setter hook -
V8's interceptor sees only writes made on its own object - and lands on the
object written to.

Consequences worth knowing:

- Only objects that declare a handler pay the indirection; everything else
  stays a plain native object.
- `GetNativeBox` and `ClassHasInstance` see through the proxy, so a class with
  an interceptor is still a class and `Unwrap` still works.
- `typeof` and ordinary property access are indistinguishable from V8's, but
  `Object.prototype.toString` and any engine-level "is this a proxy" question
  are not. Nothing in the public API exposes that.

The header's comment - that `Intercepted::No` is "the only answer a
SpiderMonkey resolve hook or a V8 interceptor can both give" - reached the
right three-state model for the wrong reason. A resolve hook cannot give that
answer at all. The model survives; the justification for it should be the
proxy, not resolve.

### A class instance carries its native in a reserved slot

`Class<T>` instances are objects of a per-class `JSClass` the `ClassRec` owns,
with three reserved slots: the `NativeBox`, the `ClassRec` (so `IsInstance` is
a pointer compare, not a prototype walk), and the template whose interceptor
answers for it. The class's finalizer destroys the box.

SpiderMonkey runs finalizers for everything at `JS_DestroyContext`, unlike V8,
which does not guarantee it. The isolate still keeps a list of live boxes and
destroys any survivors after the context is gone, because `docs/status.md` asks
for that guarantee so a parity test over destruction is writable - here it is
belt and braces rather than the mechanism.

### Isolate, and how a native call finds its way home

One `ub::Isolate` is one `JSContext`. A `JSNative` is handed only the context,
so `JS_SetContextPrivate` holds the owning `ub::Isolate*`; a callback's
`(callback, data)` pair lives in the function object's reserved slot, because
SpiderMonkey gives a native no closure pointer of its own. The realm's
`Context` is recovered from a reserved slot on the current global.

`GetHeapStatistics` reports `JSGC_BYTES` as `usedBytes` and, as `totalBytes`,
the chunks the collector holds for the heap (`JSGC_TOTAL_CHUNKS` less
`JSGC_UNUSED_CHUNKS`, times `JSGC_CHUNK_BYTES`): the heap is reserved in chunks,
so that is what has been reserved, used or not. The empty chunks the collector
keeps cached for reuse are left out, as V8 leaves its pooled pages out - with
several isolates collecting at once that cache alone outgrew the heap's ceiling.
The six figures V8 adds - physical, external, malloced and its peak, and the
global-handle pool - are left empty. The engine's only source for them is a
full memory report that walks the heap, which is not what a statistics call
should cost, and zero would be a claim. The header already warns that only
`usedBytes` is comparable, and even then only as a trend.

### A `PersistentRooted` you only assigned to is not a root

The trap in the same family as the four above, and silent in the same way. A
`JS::PersistentRooted<T>` registers itself with the runtime's root list in
`init(cx)` - or in the constructor that takes a `cx` - and **not** when you
assign to it. A default-constructed one that is only ever assigned holds the
pointer, traces nothing, and the collector is free to take what it names; it
looks exactly like a root, and the failure is a moved or freed object later,
somewhere else.

Every persistent in this backend is either constructed with a context
(`ContextRec::global`, `GlobalNode::value`, `ScriptRec::script`, the
`TryCatchState` pair) or `init`ed before its first assignment (the isolate's
own realm, which is made lazily and so cannot be constructed with one). The
matching rule at the other end: **`reset()` it before `JS_DestroyContext`**, or
the destructor unlinks from a runtime that no longer exists.

## 3. Cost, against the table in `docs/lifetimes.md` section 6

The SpiderMonkey column is accurate as written, with one addition and one
correction:

- **`escape a handle`** - the table says "1 `Value` copy into parent". True, and
  it is also unbounded: there is no per-scope limit and no global root, ever.
- **`create a handle`** - "append: bounds check + store" is right, but the
  append is *fallible*; see section 1.
- **`open a scope`** - "`RootedVector` ctor: 1 list link" is right. The vector's
  inline capacity means no allocation below eight handles, as the table says.

Nothing in the measured layout argues for revisiting §11.C (chunked frames) or
§11.F (one isolate-wide stack). The second load is one `Frame*` dereference to
reach a vector base that is almost always inline in the scope object itself.

## 4. Build notes

- `STATIC_JS_API` and `XP_WIN` are required, and so is `MOZ_STATIC_JS`; without
  them the headers lay types out differently from the library and the mismatch
  mostly does not show up as a compile error. This is the same trap as V8's
  `V8_GN_HEADER`.
- **`ENABLE_EXPLICIT_RESOURCE_MANAGEMENT` is required too, and `js-config.h`
  does not say so.** That header is the bundle's own record of what the build
  enabled, and this flag is missing from it while the library was plainly
  compiled with it. The flag gates `JSEXN_SUPPRESSEDERR` in the *middle* of
  `JSExnType`, and entries in `JSProtoKey` and `JS::SymbolCode`, so without it
  every enumerator after that point means something different on our side of
  the boundary from what it means inside the library. It compiles and links
  perfectly; it just produces a `SyntaxError` when you ask for a `TypeError`,
  and a `SuppressedError` when you ask for a `SyntaxError`. Three test cases -
  a native throw, `MakeError`, and a wrong-receiver method - found it, off by
  exactly one in exactly the predicted direction, which is what made the
  diagnosis certain. Anyone adding a define to this backend should assume
  `js-config.h` is incomplete rather than authoritative.
- **A debug engine needs two more: `DEBUG` and `MOZ_DIAGNOSTIC_ASSERT_ENABLED`.**
  `js-config.h` insists on the first and says nothing of the second, which lays
  `JS::AutoAssertNoGC` out differently (5.10). All of these live in one CMake
  function, `unibind_spidermonkey_definitions`, which every target that includes
  the engine's headers calls.
- **MSVC 14.44 is a floor, not a preference.** This library was compiled
  against that toolset's STL and calls helpers that ship only in its own
  `libcpmt.lib`, so an older one fails at link with undefined `__std_*`; the
  root `CMakeLists.txt` refuses at configure time instead. Building this backend
  against a *newer* local toolset was the thing to watch, and it never came up:
  the configure step reports 14.44.35207, so the two match exactly.
- `lld-link` emits `LNK4217` ("locally defined symbol imported") for
  `moz_xmalloc`, `moz_arena_malloc` and `InvalidArrayIndex_CRASH`. Harmless: the
  prebuilt headers declare a few mfbt helpers `dllimport` while the static
  library defines them locally. Nothing to fix on our side.
- `mozglue` defines `DllMain`, as `dependencies/README.md` warns. Irrelevant for
  a static library and the test executable.

## 5. What the engine can and cannot promise

Termination, shared ownership, stacks, code caching, off-thread compilation,
worker threads, interrupts, jobs and bring-up: the features whose shape the
engine decided rather than the design. Everything below was **measured** against
the shipped `153.3.0esr` library with a standalone probe - one translation unit,
the same defines the backend uses, no `unibind` - rather than read off a header
and believed, and it was measured before any of it was implemented, because on
these questions the measurement is worth more to the decision than the code is.
Where a claim comes from a header comment rather than a run, it says so.

### 5.1 Stopping a running script: what termination actually is here

SpiderMonkey has no "terminate" call. It has an **interrupt callback** and an
**uncatchable exception**, and termination is the two of them together:

```cpp
JS_AddInterruptCallback(cx, cb);      // once, at isolate setup
JS_RequestInterruptCallback(cx);      // from any thread
// ... cb returns false  =>  the script unwinds
```

`js/Interrupt.h` is explicit that the request may come from any thread: the
callback runs "from the JS thread some time after **any thread** triggered the
callback". Returning `false` from it is the engine's own idiom for termination -
`JS::ReportUncatchableException` in `js/ErrorReport.h` describes the same
mechanism as "mainly used to terminate JS execution from the interrupt handler".

Measured, all of it:

| question | answer |
|---|---|
| does a request from another thread stop `while(true){}`? | yes, one interrupt callback later |
| what does `JS::Evaluate` return? | `false`, with **no pending exception** |
| can script `catch` it? | no |
| does a `finally` block run? | **no** |
| is the context usable straight afterwards? | **yes, immediately** - no reset call |
| can the same context be terminated again? | yes, repeatedly |
| a request made while nothing is running? | parked; it kills the **next** script, then clears |
| does it interrupt a native callback? | **no** - see below |

So the shape the headers describe is honest on this engine, including the part
that is easiest to get wrong: `unibind/isolate.h` says a `try`/`finally` does not
stop the unwind, and on SpiderMonkey the `finally` block genuinely does not run.

#### Three things the engine does not do, that the backend must

1. **There is no sticky "terminating" state, so `IsExecutionTerminating()` on
   this backend answers `unibind` bookkeeping and not the engine.** After the
   unwind the `JSContext` is live again on the very next call; nothing in the
   engine remembers that a termination happened. `unibind/isolate.h` promises that
   every operation which would run script keeps failing until
   `CancelTerminateExecution`, so *that flag is ours* - held on the isolate,
   set by our interrupt callback, cleared only by `CancelTerminateExecution`,
   and checked at every entry point that would enter the engine. This is
   emulation of a policy rather than of a capability, and it is keepable; but
   anyone reading `IsExecutionTerminating()` here should know that no engine
   state backs it, and that a path into the engine which forgets to consult the
   flag will happily run script that was told to stop. On V8 the same question
   is the engine's own and needs no help.

2. **"Why did it stop" is not a question the engine answers.**
   `JS::ExceptionStatus` (`js/Exception.h`) has `None`, `ForcedReturn`,
   `Throwing`, `OutOfMemory` and `OverRecursed`, and carries a standing
   `// TODO: Track uncatchable exceptions explicitly.` - there is no enumerator
   for a termination. The only signal the engine offers is the pair *(the call
   returned `false`, and `JS_IsExceptionPending` is false)*, which is what the
   shell uses. It is a sound discriminator in practice - every ordinary failure
   measured here, including a syntax error, a runtime `TypeError`, an explicit
   `throw` and OOM, leaves a pending exception - but it is an inference, not a
   report. Combined with our own flag it becomes exact, which is the other
   reason the flag exists.

3. **Termination does not reach inside a native callback.** The interrupt fires
   at the engine's own checkpoints in *JavaScript* code. A native that spins for
   300ms without calling `JS_CheckForInterrupt` runs to completion; the unwind
   then happens at the next JavaScript checkpoint, and the statement after the
   call never runs. A native that *does* call `JS_CheckForInterrupt` gets
   `false` back and can return early - which is what `Isolate::IsExecutionTerminating()`
   is for. Neither engine can interrupt a blocking native, so this is not a
   divergence; it is the limit of what `TerminateExecution` buys, and it is
   better said outright than left as advice about returning promptly.

#### What is safe on a terminated context

Everything, once the unwind has finished - the context is not poisoned. The
sharp edge is elsewhere, and it bit the probe: **with an exception pending you
must not call back into the engine.** Stringifying a pending exception with
`JS::ToString` before taking it off the context crashes. The backend already
steals the exception first, which is why `TryCatch` here is a stack of handlers
over "take the pending exception the first time anybody asks".

`JS_DisableInterruptCallback` / `JS_ResetInterruptCallback` are a save/restore
pair with a confusing spelling: `Disable` returns the *previous* state as a
`bool` and `Reset` takes a parameter named `enable`. Feeding the first straight
into the second is the round trip that works - measured: interrupts were
suppressed in between and delivered again afterwards. Do not try to reason about
the parameter's name.

### 5.2 Shared ownership of a native

Nothing here resists it. `NativeHolder<T>` holding a `std::shared_ptr<T>`
instead of a `std::unique_ptr<T>` changes nothing this backend does: the box is
still one `NativeBox*` in a reserved slot, `GetNativeBox` is still one load, and
the finalizer still calls `box->destroy`. What changes is only what `destroy`
means - giving back a share rather than destroying the native - and the backend
never knew which it was.

Two engine facts make it safe, and both were measured rather than assumed:

- **Finalizers run on the isolate's own thread.** The instance class already
  carries `JSCLASS_FOREGROUND_FINALIZE`, and with it every finalizer ran on the
  JS thread: 199 of 200 instances during an ordinary `JS_GC`, and the survivor
  at `JS_DestroyContext`, none of them off-thread. That matters far more under
  shared ownership than under exclusive ownership: if the engine holds the last
  share, the native's destructor runs wherever the finalizer does. Without that
  flag SpiderMonkey finalizes on a background thread, and dropping the last
  share there would run an embedder's destructor on a thread it never heard of.
  **The flag is load-bearing for the ownership model, not just tidy.**
- **A finalizer cannot call back into the engine.** `JSFinalizeOp` is handed a
  `JS::GCContext*`, not a `JSContext*`, by design. A native whose destructor
  touches JavaScript was already forbidden and still is; shared ownership does
  not widen that, it only makes it likelier that somebody tries, because the
  destructor may now run at a moment nobody chose.

One consequence for `docs/status.md` decision 7. "Natives are destroyed exactly
once" needs splitting in two, because two wrappers over one native are now
ordinary: the **box** is destroyed exactly once per wrapper, by the finalizer or
by the isolate's survivor sweep; the **native** is destroyed exactly once
overall, when the last share goes, which may be neither of them. The isolate's
`liveNatives` list is keyed by box and needs no change.

### 5.3 Stack traces, including off a caught exception

Fully available, and structured - this is one of the areas where SpiderMonkey
gives *more* than the text-only `StackTrace()` the API had.

- `JS::CaptureCurrentStack(cx, &stackp, capture)` yields a chain of `SavedFrame`
  objects; `capture` is `AllFrames`, `MaxFrames(n)` or `FirstSubsumedFrame`, so
  `CaptureStackFrames`'s `limit` maps straight onto `MaxFrames`.
- `js/SavedFrameAPI.h` reads a frame: `GetSavedFrameFunctionDisplayName`,
  `GetSavedFrameSource`, `GetSavedFrameLine`, `GetSavedFrameColumn`,
  `GetSavedFrameParent` (and `AsyncParent`, `AsyncCause`, `SourceId`). Line and
  column are **1-origin**, which is what `StackFrame` asks for.
- Off a caught exception there are two doors and both work.
  `JS::ExceptionStackOrNull(errObj)` gives the chain captured with an `Error`
  object; `JS::GetPendingExceptionStack` gives the chain for *any* pending
  throw, including a thrown primitive that carries no `.stack` of its own.
  Measured: `throw 'plain'` from a function has no stack property and still
  produced `f@prim.js:1:15` through the second door.
- `JS::BuildStackString` renders the chain in `Error.prototype.stack` format,
  which is what `TryCatch::StackTrace()` already returns.

Two honest limits:

- **`CaptureCurrentStack` answers null when no JavaScript is on the stack** -
  measured from `main`. So `CaptureStackFrames` called from native code that is
  not inside a callback returns empty, which is the right answer and is what
  `unibind/exception.h` already says.
- **A termination carries no stack**, trivially: there is no exception object,
  and `GetPendingExceptionStack` asserts that one is pending. `TryCatch::StackFrames`
  must check `HasTerminated` before it asks.

The one piece of bookkeeping this backend owes: `~TryCatch` **consumes** what it
caught (decision 3), so by the time a caller asks for frames the pending
exception is gone. The backend already stashes the `SavedFrame` alongside the
exception value when it takes it off the context, and `StackFrames` must read
that stash rather than the context.

### 5.4 Compiled-code caching: yes, and stencils fit better than what is cached now

SpiderMonkey's unit is a **`JS::Stencil`** - the parser's output, before
anything is on the GC heap - and `js/experimental/JSStencil.h` has the whole
round trip:

```
CompileGlobalScriptToStencil -> EncodeStencil -> bytes
bytes -> DecodeStencil -> InstantiateGlobalStencil -> JSScript -> run
```

Measured end to end: a trivial script encodes to 464 bytes, decodes, instantiates
and runs correctly. `IsStencilCacheable` reports whether a stencil can be encoded
at all (it cannot if it contains asm.js), and `TranscodeResult` already
distinguishes the case the header cares about - `Failure_BadBuildId` is exactly
"a different engine build, rejected rather than trusted".

Better than that: **a stencil is realm-independent by construction**, which is
what `docs/status.md` decision 10 spent a paragraph arguing for. A `JSScript` is
a GC thing belonging to a realm; a stencil is not. See 5.4.1 for what switching
cost.

What the stencil route does **not** buy is any association between a blob and
the source it came from - see below. That was worth finding out, because it is
the obvious thing to assume.

`CompileOptions::EagerCompile` is one compile option here:
`setEagerDelazificationStrategy(ParseEverythingEagerly)`, which parses every
function body and gives it bytecode in the first pass, so the stencil - and so
the blob - holds all of it rather than a syntax-checked outline of each
function. Nothing else is needed, because this engine has no in-isolate
compilation cache that could answer an eager compile with an earlier lazy one;
that is V8's problem, and decision 19 says what its backend does about it.

#### The blob says which engine built it, and not which source it came from

**This is the most dangerous thing found in this backend, and the stencil route
does not fix it by construction.** It is now fixed in `unibind/script.h`, for every
backend at once (decision 19), and the reasoning is worth keeping here because
it is what put it there.

`JS::DecodeStencil` checks the build id and answers `Failure_BadBuildId` for a
blob from another engine build. It checks nothing about the *source*. A blob
encoded from `"a"` decodes cleanly when offered for `"b"`, reports success, and
runs `a`. The embedder asked to run one script and got a different one, with no
error, no exception and nothing in the result to suggest anything happened.

Every other failure mode in this project announces itself - a crash, a wrong
value, a leak. This one looks exactly like success.

`unibind/script.h` promises the opposite ("a different engine build, a different set
of flags, **source that has changed** - rejects it and compiles normally"). It
now keeps that promise itself: it frames every blob it emits with a stamp over
the source and drops one that does not match before a backend is ever called,
so what reaches `CompileScriptWithCache` here is either empty or a payload that
belongs to this source. **Do not add a second layer of framing in this backend,
and do not remove the reliance on the first.**

It is worth seeing the build id and the source stamp as the same safety property
at two scales - *these bytes came from this engine* and *these bytes came from
this source* - and noting that the engine only implements the first of them. A
backend author porting this to a third engine should assume the keying is theirs
to do until they have a test that proves otherwise; the test to write is the one
that encodes from one source and offers the blob for another, and then checks
what actually ran rather than only what the API reported.

Three further things that must be got right, one of which is a crash:

1. **`JS::EncodeStencil` segfaults if `JS::SetProcessBuildIdOp` was never
   called.** It does not return `Failure_BadBuildId` and it does not throw - it
   dereferences a null function pointer. This is process-wide state, so the
   backend installs a build-id op during `Platform` construction, before any
   embedder can reach a cache API. With one installed the round trip is clean.
2. The build id is what makes a stale blob safe, so it has to change whenever
   anything affecting the encoding does.
   `JS::GetOptimizedEncodingBuildId` folds in CPU features for the cases where
   that matters.
3. Encoding wants its buffer aligned: `JS::IsTranscodingBytecodeAligned` and
   `AlignTranscodingBytecodeOffset` exist for appending into an existing buffer.
   Starting from an empty `TranscodeBuffer` avoids the question.

`JS::StencilIsBorrowed` was false for a stencil decoded from a `TranscodeRange`
over our own buffer, so the caller's bytes need not outlive the decode. That
stops being true if `DecodeOptions::borrowBytecode` is ever set.

#### 5.4.1 What moving `ScriptRec` to a stencil cost, and what it did not buy

A backend-internal decision; no header depends on it, and it landed with the
caching entry points (decision 19).

**What shipped.** `ScriptRec` holds a `RefPtr<JS::Stencil>` *and* the
`JS::PersistentRooted<JSScript*>` that one realm made of it. Compiling is
`CompileGlobalScriptToStencil` followed by `InstantiateGlobalStencil` into the
realm that asked; `CompileWithCache` is `DecodeStencil` and then the same
instantiation, which skips the parse outright; and `CreateCodeCache` is
`EncodeStencil` on the stencil the rec is already holding, rather than the
re-compile it would otherwise have been. The stencil is the right unit for all
three because it is the only thing on this engine that can be turned into bytes
and back, and because it belongs to no realm - decision 10 is what the engine's
own unit already means here, rather than something the backend arranges.

A `RefPtr` is not a GC root, so the stencil needs nothing rooted for it. That
part was simpler than the `JSScript` it sits beside, not harder.

**What was considered and not done: the per-realm instantiation cache.** The
design sketched here was a stencil plus a small cache of instantiated
`JSScript*`s keyed by realm, in a hidden reserved slot on the realm's global -
the shape the template materialisation cache already uses, so it is traced by
the engine and dies with the realm, with no bookkeeping to get wrong when a
context is released. It was not built. The rec holds the one `JSScript` that was
instantiated into the realm which compiled it, and `RunScript` executes that
script only in that realm. Run in any other, it instantiates the stencil again,
*there*, and executes that - an instantiation per foreign run, not a parse, and
nothing kept, so a script run once in a sandbox does not keep the sandbox alive
for as long as the script. `scripts: a script sees the globals of the realm it
runs in` is what holds both backends to decision 10.

It used to execute the one `JSScript` in whatever realm it was handed, and a
release engine lets it: the result is even right, because a global script
resolves its globals through the environment it is run against. But a
`JSScript` belongs to its realm, and a debug engine stops at
`JS_ExecuteScript` with "Realm mismatch" - the release build was running on
invariants it had not been given. The debug suite is what found it (5.10).

What the cache would change is therefore what a run in a foreign realm costs,
not what it does. It is recorded because it is a real option with a shape the
realm-keyed template cache has already proven, and because a backend author
looking at the same `RefPtr` will have the same idea.

### 5.5 Off-thread compilation, which is real and does not fight one-isolate-per-thread

`JS::FrontendContext` (`js/experimental/CompileScript.h`) is a parser context
with **no `JSContext` and no heap**: `NewFrontendContext` on any thread,
`SetNativeStackQuota`, `CompileGlobalScriptToStencil(fc, ...)`, and the stencil
comes back to be instantiated on the isolate's thread. Measured: compiled on a
`FrontendContext`, instantiated on the `JSContext`, ran, right answer.

This is worth knowing because it is the one place decision 11 does *not* bite. A
`FrontendContext` is not a `JSContext`, so a worker thread compiling source is
not a second isolate on that thread, and "one isolate per thread" costs an
embedder nothing here. Errors are read off the `FrontendContext`
(`HadFrontendErrors`, `GetFrontendErrorReport`) and converted with
`ConvertFrontendErrorsToRuntimeErrors` when there is a `JSContext` to convert
them onto.

Nothing in the public API asks for this today. It is what to build on if
asynchronous compilation is ever wanted, and it needs no new lifetime rule.

### 5.6 Threads: the pool is the process's, and "none" is not on offer

**This is the one place in this whole section where SpiderMonkey cannot do what
the option originally meant, and the backend does not pretend otherwise.**

`PlatformOptions::workerThreads` is on the right object: SpiderMonkey's helper
threads are created by `JS_Init` for the process, not per context, so a
per-isolate figure would have been a promise the second isolate could not keep.

But `workerThreads == 0` - "everything the engine would have done in the
background it does on the calling thread instead" - has no implementation here:

- `JS::SetHelperThreadTaskCallback(cb, threadCount, stackSize)` is the only hook
  that replaces the pool. **Called before `JS_Init` it segfaults immediately.**
  Called after `JS_Init`, with a callback that runs the task inline on the
  calling thread, everything works - compiling, evaluating, terminating,
  encoding and decoding stencils - **until the first `JS_GC`, which segfaults**.
  The engine dispatches parallel marking tasks and then waits on them; running
  them synchronously inside the dispatch is a re-entrancy it does not support.
  It fails late and it fails looking like it worked, which is the worst shape of
  failure available.
- `JS_SetGCParameter(cx, JSGC_MAX_HELPER_THREADS, 0)` is silently ignored: the
  value stays at its default and `JSGC_HELPER_THREAD_COUNT`, which `js/GCAPI.h`
  documents as read-only and derived, does not move.

So a request for zero had three possible answers and two were unacceptable:
ignore it (the silent lie), honour it (crash on the first collection), or refuse
to construct the `Platform`. The resolution taken is that `workerThreads` is a
**hint** at every value including zero, and that the outcome is **observable** -
an embedder who wants fewer threads sets it and moves on, and one who genuinely
needs none can ask whether it got them and fail on its own terms.

That is not decision 11 in reverse. Decision 11 was about semantics a program
observes, and it refused on V8 something SpiderMonkey could not do because a
program that worked on one backend and not the other is the failure this library
exists to prevent. Worker threads change no observable behaviour of any script -
only timing, and where work happens - so an ignored hint is not a portability
cliff, and making V8 refuse a mode it supports perfectly well would have cost a
real capability for symmetry's sake.

`heapLimitBytes` and `stackLimitBytes` are both genuinely per-isolate here and
both mean what the header says: `JS_NewContext(maxBytes)` for the first, and
`JS_SetNativeStackQuota` - measured from wherever it is called, which is
`Isolate::New` - for the second. The figure passed is the stack less the margin
the engine needs in order to build and throw its error once it has decided it
is out: `JS::ThreadStackQuotaForSize` for a stack over 320 KiB, which takes a
tenth. Below that a tenth is less than `js::MinimumStackLimitMargin` (32 KiB),
which a debug engine asserts and a release one accepts with a margin too small
for its periodic recursion check to be sure of; so a smaller stack keeps the
32 KiB whole instead. The suite's 128 KiB case is one of those, and the debug
engine is what said so.

### 5.6.1 What is legal inside an interrupt callback, measured

The rule in `unibind/isolate.h` - a callback may make handles and read values, and
may **not** run script - was set from V8's source. SpiderMonkey is the looser
engine, and it is worth recording by how much, because the rule constrains this
backend more than the engine does.

A staged probe inside a live interrupt callback, fired from another thread into
a `while(true)` loop so that it certainly landed mid-script, recording the last
stage reached so a crash would name where it died. It reached the last stage.
In order: made a `JS::Rooted` handle, read a value out of it, made a plain
object, set a property on it, **called a JavaScript function** - which really
ran, its side-effect counter read 1 afterwards - **compiled and ran a fresh
script**, and captured a stack. The callback then returned false, the loop
terminated normally, and the isolate evaluated fine afterwards.

So obeying the stricter rule costs this backend nothing: the restriction falls
on the embedder, and there is no SpiderMonkey capability left unimplemented to
honour it. It should stay the rule anyway, and there is a reason from inside
this engine rather than a restatement of V8's. An interrupt fires between two
bytecodes of whatever happened to be running. Script run at that point runs in
the middle of unrelated code, and anything it leaves pending on the context - an
exception above all - is left for that code to trip over. The probe survived
only because it cleared its own exceptions at every stage. "You may, but you
must clean up perfectly afterwards" is better written as "you may not".

The limit that matters more in practice, and which is true on both engines:
**an interrupt reaches script, not the embedder's own C++.** A native that spins
without re-entering the engine runs to completion, and the unwind happens at the
next JavaScript checkpoint after it returns. A watchdog cannot save you from
your own blocking callback.

### 5.6.2 Jobs: there is no automatic drain to turn off

Decision 23 makes `PumpJobs` the single point where promise continuations and
posted work run, because when a continuation runs is script-observable. On V8
that means switching an automatic drain off. Here there is nothing to switch:
SpiderMonkey has no job queue of its own until an embedder gives it one.
`js::UseInternalJobQueues` in `Isolate::New` makes the engine queue promise jobs
rather than having nowhere to put them, and `js::RunJobs` - called only from
`PumpJobs` - is the only thing that runs them.

The pump alternates engine jobs and posted work until both are empty, so a
posted job that settles a promise sees its continuations in the same pump. The
posted queue is guarded by a mutex because posting is allowed from any thread,
and a job runs with the lock released, because a job may post more work and that
work is drained too.

### 5.6.3 Bring-up that half-succeeds

`JS_Init` returns a `bool` and can fail. It was being checked with an `assert`,
which is the same as not checking it: the assertion compiles out of exactly the
build where the failure matters, and then `IsInitialized()` answers true,
`Isolate::New` consults it and believes it, and isolates are handed out against
an engine that never came up. Same hole as the V8 backend had, same shape of
fix: a failed bring-up unwinds and leaves the object existing and having done
*nothing*, rather than existing and having done half, and the destructor does
not shut down what was never started.

A second `Platform` stays an `assert`, deliberately. It is a precondition
violation rather than a failure, and answering it through the same channel as
"the engine would not start" would make both unclear.

Two more of the same family were found by sweeping the rest of the bring-up for
results that are reported and discarded:

- **`JS_AddInterruptCallback` returns `bool` and it was being ignored.** It
  appends to a vector, so it can fail, and an isolate that lost it looks
  perfectly healthy while `TerminateExecution` quietly does nothing - which an
  embedder discovers when a runaway script does not stop, the moment it can
  least afford to. Now checked, with the context unwound.
- **The cached helper-thread count outlived the `JS_Init` it belonged to.** It
  is now cleared in `~Platform`, so a second `Platform` that gets a different
  pool does not read the first one's figure back.

The general shape is worth naming, because this backend has produced four
instances of it now: the engine's own contract is repeatedly stricter than its
documentation suggests, and the failures are silent rather than loud. One
`JSContext` per thread, the null dereference in `EncodeStencil` with no build-id
op, `operator new` being replaced out from under a translation unit by
`cxxalloc.h`, and a bring-up that reports success after failing. **Assume a
SpiderMonkey entry point that returns a `bool` means it**, and assume anything
reached through a header macro or a process-global hook is doing something the
header does not mention.

### 5.7 Construction without `new`

No engine constraint at all. `args.isConstructing()` already tells the two apart,
and the template trampoline already branches on it for `FunctionTemplate`
(section 1, "Called or constructed"). `Class<T>::ConstructOrCall` is the same
branch with the `TypeError` skipped, and `JSFUN_CONSTRUCTOR` is already on the
function object. The only thing to be careful of is that a plain call has no
`new.target` and therefore no prototype to derive from it, so the instance must
be built from the `ClassRec`'s own prototype - which is what the construct path
does anyway.

### 5.8 `Maybe` -> `std::optional`

Mechanical. `Maybe<T>` was already an alias for `std::optional<T>`, and this
backend's uses are spellings rather than semantics.

### 5.9 A DevTools inspector: not of this shape

SpiderMonkey's debugging surface is the `Debugger` object, a JavaScript API
installed into a debuggee realm. It speaks no protocol and has no C++ session to
drive, so a Chrome DevTools inspector over it would be a debugger server written
in script on top of that object - a different and much larger thing than a
backend, and not one this backend pretends to be.

So `src/backends/spidermonkey/inspector.cpp` defines every member of
`unibind/inspector.h` and makes nothing: `Inspector::Supported()` is false,
`New` returns null, and the rest can never be reached, because there is never an
object to call it on. That a program compiled once links against this backend
anyway, and is told no at run time, is the point (decision 29) - and it is why
this is not decision 19's link error, which would have made a program that
merely *offers* a DevTools port impossible to build against this engine.

### 5.10 What the debug engine found

The suite is run against the engine's debug build too (`UNIBIND_ENGINE_FLAVOR=debug`,
a Debug configuration; the spidermonkey workflow runs it). Every engine assertion it hit
was a backend bug that the release engine had been quietly tolerating, and none
of them was the engine's:

- **It did not link.** `JS::AutoAssertNoGC` is an empty class with inline members
  unless `MOZ_DIAGNOSTIC_ASSERT_ENABLED` is defined, in which case it carries the
  context it asserts against and its constructor and destructor are the
  library's. A debug engine is built with it, and `js-config.h` does not say so -
  `ENABLE_EXPLICIT_RESOURCE_MANAGEMENT`'s trap again - so the backend laid the
  class out at the wrong size and the link reported only "duplicate symbol".
  `unibind_spidermonkey_definitions` in `cmake/UnibindEngines.cmake` is now the
  one list of what a translation unit including these headers must define.
- **A script ran in a realm it did not belong to** ("Realm mismatch" at
  `JS_ExecuteScript`): 5.4.1.
- **A wrapper's realm was entered** (`!IsCrossCompartmentWrapper` at
  `JSAutoRealm`), and an unwrapped object was then used from the wrapper's
  compartment ("Compartment mismatch"): the no-`Context` questions in section 2.
  Answering them right needs a realm the object is legal in, and a wrapper has
  none.
- **An interceptor's key list was read across compartments.** An enumerator may
  make its array in another realm; it is wrapped into the current one now,
  rather than unwrapped out of its own.
- **Strings crossed zones** ("atom is marked white for zone"): section 2, and
  why realms now share one.
- **A 128 KiB stack left too small a margin** (`ThreadStackQuotaForSize`): 5.6.
- **The checked build's stale-handle diagnosis relied on the optimiser.** It
  compared the handle's epoch with whatever the dead frame's storage held, which
  diagnoses the mistake only if a later frame has reused that storage - which an
  optimised build does and an unoptimised one mostly does not. A closing frame
  now writes an epoch no handle of it carries, so the diagnosis holds in every
  build.
