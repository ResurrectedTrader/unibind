# What is implemented

The suite in `tests/` is written once, against the public API only, and runs
against every built JavaScript backend - `ctest -R parity` prints one row per
case and a row that differs is the thing to look at. The CPython backend runs
Python, so that suite's scripts mean nothing to it; it has `tests/python/`
instead, and decision 30 says why that is the right shape rather than a gap.

An operation that is declared but not defined is a **link error at the call
site**, not a silent no-op or a runtime abort. That is now load-bearing rather
than incidental: it is how this API says "this engine cannot do that" - see
decision 19, where the compiled-code cache is three entry points precisely so a
backend without one can define none of them.

**Where the two JavaScript backends are.** Both implement everything the headers declare
that their engines can do, decisions 1-29, and the suite agrees case for case:
413 cases compared, **no divergences**. The single `SKIPPED | SKIPPED` row is the
harness's own test of the skip path, which exists so that the machinery for
reporting a missing area is exercised on every backend rather than only on the
day one falls behind.

Twenty-five rows are `SKIPPED | PASSED`, and none of them is a backend falling
behind. Two are `HEAP_LIMIT`, decision 28's near-heap-limit hook: SpiderMonkey
has no such thing to implement, so it defines nothing, a call does not link, and
the two cases report a skip. That is the shape decision 19 designed for an
absent operation, working as designed - it is a gap in the *engines*, and the
report saying so is the point rather than a defect in it. Nineteen are cases that
drive the inspector's protocol (decision 29), which ask `Inspector::Supported()`
and report a skip where it says no - the same gap, reported the way decision 29
says this one is. Two are views over a `SharedArrayBuffer`, which a SpiderMonkey
realm made here does not define, so each case asks script whether the
constructor exists and reports a skip when it does not (`docs/testing.md`). One
posts work until the queue cannot grow, and reports a skip where no allocation
under it ever failed; the last makes a realm under a failing allocator, and
reports a skip where making one asked the allocator for nothing it could refuse.

- V8 15.6: `src/backends/v8/`.
- SpiderMonkey 153.3.0esr: `src/backends/spidermonkey/`. Its own notes - what
  had to bend, and what it measured - are in `docs/spidermonkey.md`, and they
  are the better read for anyone writing a JavaScript backend.
- CPython 3.12.13: `src/backends/python/`, and `docs/python.md` for the notes.
  It defines every entry point decisions 1-29 declare, `SetHeapLimitCallback`
  included; `Inspector::Supported()` is false there, as on SpiderMonkey, and
  `unibind/interop/v8.h` is V8's alone. Its own suite is 269 cases, all green.
  Decisions 30-37 are what a second *language* behind the API needed, and not
  one of them changed a public header.

**What compiles the public headers: clang, including clang-cl, and not MSVC's
`cl.exe`.** That is a compiler bug rather than an API problem and it is written
up at the top of `unibind/unibind.h`, with the workaround and what it would cost. The
build pins ClangCL.

## Decisions a backend author has to know

These are the places where the two engines wanted different things and the
answer had to be picked rather than discovered. Each one is normative in a
public header; this is the index, not the text. Decisions 30 on are the third
engine's, where what disagreed was not two engines but two languages; those are
normative in the CPython backend and in `docs/python.md`, since no header
changed for them.

### 1. The callback return sink (`unibind/function.h`)

V8 types a callback's return slot by the hook that owns it -
`PropertyCallbackInfo<Value>` for a getter, `<Integer>` for a query,
`<Boolean>` for a setter or deleter, `<Array>` for an enumerator - and the
setters differ between them: a `ReturnValue<Integer>` will not take a `double`
and says so at compile time. The six `SetReturn*` entry points are one set for
every hook, so they cannot name one V8 type.

The V8 backend erases that with a `ReturnSink`: six function pointers, one
static table per hook shape, chosen where the `CallbackState` is built - the
one place that statically knows the shape. SpiderMonkey needs no erasure at
all; every native there writes into a `JS::Value`, so its `CallbackState`
carries one `JS::Value*` and the six entry points are six stores.

That difference is the point: **nothing public names the sink.**
`CallbackState` is each backend's own type. What is shared is behaviour, and it
is written at the top of `unibind/function.h`:

- One `CallbackState` serves every shape of call. The public wrapper decides
  what may be asked of it, so a property hook is never asked for an argument
  list or for `IsConstructCall`.
- A hook whose answer is its C++ return value - an interceptor setter, query,
  deleter or enumerator - **discards** writes through `GetReturnValue()`.
  Legal, does nothing, must not corrupt the hook's protocol. Script sees this:
  an intercepted assignment still evaluates to the assigned value.
- `Holder()` is the object carrying the handler, `This()` the receiver. Where
  an engine hands a property hook only one of the pair - V8 15.6 gives an
  interceptor the holder and nothing else - they are the same object.

### 2. A frame that cannot grow yields an empty handle (`unibind/handle.h`, rule 9)

Adding a slot is fallible: SpiderMonkey's `RootedVector::append` reports OOM,
and V8's overflow vector throws. A slot-allocating operation that cannot grow
the frame therefore hands back an **empty** `Slot` - an empty optional where the
signature has one - and reports the condition the way the engine reports
running out of memory.

It must never hand back a slot that reads as `undefined`. That was the
SpiderMonkey backend's original behaviour and it is the one failure this API
exists to prevent: an allocation failure arriving as a plausible value.
`std::optional<Slot>` on every maker was the alternative and is worse - it would
put an unwrap on `Undefined()`, `GlobalObject()`, `Global::Get()` and
`info.This()` at every call site forever. `Local` already had `IsEmpty()`; this
rule is what it is for. The reasoning is in `docs/lifetimes.md` under "Running
out of slots".

### 3. `~TryCatch` consumes what it caught (`unibind/exception.h`)

Unless `ReThrow` was called. A handler is a `catch` block, not an observer.
Both backends already did this; the header used to say the opposite. The other
rule - propagate unless explicitly consumed - is worse: a forgotten call leaks
a pending exception into code that never went near the throw, and it surfaces
somewhere else entirely.

### 4. A value may be used with any realm of its isolate (`unibind/context.h`)

A handle belongs to an isolate and a thread, not a realm, so an object made in
one realm works in another - which is what makes a sandbox useful at all. V8
needs nothing for this; SpiderMonkey must wrap into the target compartment
(`JS_WrapValue`). Guaranteeing it costs one backend a call; the alternative
would have cost the API its most useful realm feature.

What is **not** guaranteed is identity as seen from a foreign realm: where an
engine wraps, the wrapper is a distinct object. Compare within one realm.

Bounded again by what the sandbox composition found: **enter a realm before
reading through an object that belongs to it.** A value crosses freely, but a
realm's *global object* is access-checked, and reading a property of one while a
different realm is current fails - on V8 with a `TypeError: no access`, before
any embedder code runs. So a property hook that answers by reading an inner
realm's `globalThis` opens a `ContextScope` on that realm first. One line, both
backends, and it is a rule rather than tidiness.

There is deliberately no way to say "these two realms trust each other". V8
spells that as a shared security token; SpiderMonkey has no token, only
compartments and principals, and the two do not describe the same thing closely
enough to promise one.

### 5. Interceptors are three-state because of proxies, not resolve hooks

`Intercepted::No` has to be distinguishable from "handled, and the answer is
undefined". An engine with no interceptor of its own implements the hooks as
proxy traps - the only construct that runs on every access *and* can hand the
access back to the ordinary object. A resolve-style hook cannot: it fires only
on a miss, what it defines sticks, and it has no setter.

### 6. Accessors are ECMAScript accessor properties, not native data properties

V8 15.6's `PropertyCallbackInfo` reports `Holder()` and not `This()`. For an
interceptor that is a documented limit (see 1). For an accessor it would have
been fatal: `Class<T>::Accessor` installs on the *prototype*, and the
trampolines in `unibind/class.h` unwrap `info.This()`, which under a native data
property would be the prototype, carrying no native. So `TemplateSetAccessor`
installs a real accessor property backed by two function templates. A function
call has a receiver, so the accessor has one.

Consequence: `PropertyAttribute::ReadOnly` is meaningless on an accessor - a
getter with no setter *is* the read-only form - and both backends drop it.

`Object::SetAccessor` - V8's, for one object rather than every instance of a
template - installs exactly the same property: two real functions, so
`Object.getOwnPropertyDescriptor` reports `get` and `set` on both engines, a
receiver that is whatever the read went through, and the same dropped
`ReadOnly`. Its callback record is the isolate's for the isolate's life, as a
template's is (decision 13), which is the one thing an embedder has to know
about it: it is for an object made a bounded number of times.

### 7. Boxes and natives are destroyed exactly once, finalizer or not

This was one invariant until decision 14, because under exclusive ownership
there was one thing to destroy. A share separates the wrapper's cell from the
thing it points at, so it is now two - and the old single sentence is not merely
imprecise, it is *false* of the interesting case, since a native the embedder
co-owns deliberately outlives the isolate.

1. **Every box is destroyed exactly once, and all of them by the time the
   isolate is gone.** A box is the per-wrapper cell holding the share. This is
   the half a backend works for: V8 does not promise to run a weak callback
   before an isolate goes away, so its isolate keeps a record of every live
   instance and destroys the survivors in `~Isolate`; SpiderMonkey does run
   finalizers at `JS_DestroyContext` and keeps the same list anyway. Without the
   guarantee there is no destruction parity test to write, which is why it is a
   rule and not an implementation detail.
2. **Every native is destroyed exactly once, when its last share goes** - which
   may be *after* the isolate, because the last share may be the embedder's.
   Not a leak and not a violation of (1): the engine gave back everything it
   took, and what remains is something the embedder still holds and still owns.

So the sentence to hold a backend to is "the engine gives back every share it
took, exactly once, by the time its isolate is gone".

**And it gives them back on the isolate's own thread.** Never a background
collector or helper thread: `destroy` drops a `shared_ptr` whose other holders
are the embedder's, so running it off-thread races them. This looks like a
tidiness choice right up until someone removes it, and the failure does not
reproduce - so where an engine offers the choice, SpiderMonkey's
`JSCLASS_FOREGROUND_FINALIZE`, the foreground one is **required** and the code
says so in a comment. That backend verified all 200 of its finalizers run on the
JS thread.

Shared ownership cost that backend nothing else at all, which is the second
piece of evidence that decision 14 put the boundary in the right place - the
first being that no backend signature changed.

### 8. `ValueKind` answers what you can do with a value (`unibind/types.h`)

A Date, a RegExp, a Proxy and a typed array are all `Object`, because `Object`
is exactly the set of operations the API offers for them. There is no exotic
kind: the list differs by engine and by version, and it is a *negative* answer
no caller can act on. `Proxy` settles it - an engine with no interceptor of its
own builds ours out of proxies, so a kind that singled proxies out would make a
sandbox object report differently on two backends purely because of how we
built it. `ValueKind::Other` is for a value a backend cannot classify at all,
which today is no value on either engine.

The other half of the same rule: **an `External` is not an `Object`**, to
`Is<Object>()` or to `IsObject()`, on either backend. It is a value with no
properties, which is what `Object` would promise. V8 15.6 counts its own
External as an object, so that backend excludes it by hand; SpiderMonkey's is an
object of a private class, excluded the same way (`docs/spidermonkey.md`).

### 9. A native function is callable; a constructor is asked for (`unibind/value.h`)

`Function::New` produces something `new` refuses - a TypeError from script, a
failure from `NewInstance` - and so does a template method and either half of an
accessor. `FunctionTemplate` and `Class<T>` are how an embedder asks for a
constructor, and they still are one.

A function that can be `new`-ed without anyone asking hands script a fresh empty
object instead of the callback's result, and the callback cannot tell unless it
thought to check `IsConstructCall()`. The engines defaulted opposite ways; this
is the stricter one, and the one an ordinary JavaScript method already follows.

### 10. A script sees the globals of the realm it runs in (`unibind/script.h`)

Not the realm it was compiled in. `unibind/script.h` already argued that compiled
source is an artefact rather than a member of a realm - that is why `Script` is
not a handle - and this is the rest of that sentence. Compile once, run in every
sandbox is the job a compiled script is cached for; the alternative makes
`Run`'s context parameter a lie. V8 pays one `BindToCurrentContext` per run,
which is its own facility for exactly this.

### 11. One isolate per thread at a time (`unibind/isolate.h`)

A second isolate while the first is alive on that thread is refused -
`Isolate::New` returns empty - and one after another is fine. The restriction is
SpiderMonkey's: it keeps the running `JSContext` in a single thread-local slot,
so a second one on a thread is not a heap it can make. V8 would allow it and
refuses anyway, because a program that works on one backend and not the other is
the failure this library exists to prevent. Empty was already the documented
answer for a heap that could not be made, so no signature changed. A crash never
was one of the available answers.

Two heaps at once is two threads, which costs nothing this design had not
already charged: handles, scopes and contexts are thread-bound anyway.

**And an isolate does not move**, which is why `New` hands back a pointer rather
than a value the way `Global<T>` does. It is asked about often enough to be
worth an entry: the *address* of the `Isolate` is what everything remembers it
by - the engine's own embedder slot, so a callback handed only an engine context
can get home; the thread-local that enforces the rule above; and an `Isolate*`
on every frame, context, script, template, class, root and handler. Storing the
implementation's address instead would work, at a load per recovery, but
immovability is worth having anyway: `HandleScope`, `ContextScope` and
`TryCatch` are stack objects pointing at the isolate, and a movable one could be
moved out from under an open scope with nothing to catch it.

The only shape that removes the dereference is a plain `Isolate` with its own
empty state, and that trades a null pointer - which cannot be used by accident -
for a valid-looking object whose methods are undefined. `Local` makes exactly
that trade and says so, because handles are made constantly; an isolate is made
once, so the check is free and the trade is not worth making twice.

**`Platform` has the opposite shape and the same question**, and the answer was
already in the API: a constructor cannot report failure, but `IsInitialized()`
can, and `Isolate::New` already refuses when it is false. What was missing was a
backend honouring it - V8's `Initialize` and SpiderMonkey's `JS_Init` both report
failure, and both were being ignored. A `Platform` that fails to bring the engine
up is now an object that exists and did nothing, rather than one that did half.
Worth separating from decision 20 while they are next to each other: the
`workerThreads` hint was *not* caused by the missing channel. That was argued on
observable-semantics grounds and would have come out the same with a way to
refuse.

### 12. A `FunctionTemplate` is a function as well as a constructor

Its function may be *called* as well as constructed, and the callback tells the
two apart with `IsConstructCall()`. A `Class<T>` constructor still requires
`new`. Together with decision 9 that is a grid with no redundant row and no
unreachable one:

| made by | `f()` | `new f()` |
|---|---|---|
| `Function::New` | yes | TypeError |
| `FunctionTemplate` | yes | yes |
| `Class<T>` | TypeError | yes |

A `FunctionTemplate` that refused a plain call would be a strictly worse
`Class<T>`, and nothing would be left in the API to express a built-in like
`Error` that means something both ways - which is also the only thing that makes
`IsConstructCall()` worth having. A template given *no* callback is still
constructable; what a plain call to one does is left unspecified, because there
is nothing to run.

### 13. Templates outlive nothing

`TemplateRec`, `ClassRec` and the accessor and interceptor records - including
the record `Object::SetAccessor` makes for a single object - are
isolate-owned and isolate-lifetime by design (see the header comment in
`unibind/template.h`). They are raw pointers into containers the isolate owns, and
they must not become refcounted. Everything holding an engine root is released
before the isolate is torn down.

A template's *shape* - class name, parent, handlers - is fixed at its first
instantiation, and a later `SetClassName`, `Inherit` or `SetHandler` is ignored.
That is V8's rule, where changing any of them afterwards is a fatal error inside
the engine; SpiderMonkey would have taken the change and applied it to the next
realm, and follows V8 so the two agree. Each backend records which templates
have been instantiated - directly, or along with one that instantiates them - in
its own records, and consults that before passing the call on.

### 14. A wrapper owns a *share* of its native (`unibind/class.h`)

`Class<T>` used to own its native exclusively, which made two ordinary things
inexpressible: two wrappers naming one native (a collection hands out a child,
an enumeration of the same collection hands out the same child), and a native
co-owned with the embedder. Under exclusive ownership one wrapper frees a native
the other still reads, and nothing at the call site says which.

`NativeHolder<T>` now holds a `std::shared_ptr<T>`, so destroying a wrapper
gives back a share and the native goes when the last holder does - which may be
the embedder's. `Wrap` takes a share (a `unique_ptr` converts), `Unwrap` still
hands back a bare `T*`, and `UnwrapShared` is how a native outlives the wrapper
it came from. A constructor may return either shape too: `Construct` and
`ConstructOrCall` take a callback returning a `std::unique_ptr<T>` or one
returning a `std::shared_ptr<T>`, so a native with a deleter of its own, or one
the embedder already holds, is made the way any other is. The trampoline turns
either into the wrapper's share, so that cost no backend anything either.

Rejected, and why, in the header: a **borrowed wrapper** (the bug this
prevents), a **per-class dial** (a type parameter on the most-used type in the
API, or a runtime flag that is this design with a branch in front), an
**intrusive count** (cannot wrap a type the embedder did not write, and cannot
be co-owned with an embedder holding a `shared_ptr`).

Cost: one atomic pair per wrapper, nothing per `Unwrap`, and nothing per native
if it is built with `make_shared`. **No backend signature changed** - `NativeBox`
is untouched - which is the clearest evidence the boundary was in the right
place.

**This splits decision 7 in two**, and decision 7 now carries both halves: a
*box* is destroyed exactly once and all of them before the isolate goes, while a
*native* is destroyed exactly once when its last share goes, which may be after
the isolate. The old single sentence - "every native is destroyed by the time
its isolate is gone" - is not merely imprecise now, it forbids in plain terms
the case this decision exists for.

### 15. Stopping a script from another thread (`unibind/isolate.h`)

`TerminateExecution` is callable from a thread other than the isolate's, as are
`RequestInterrupt` (decision 24), `PostJob` and `PostDelayedJob` (decision 23)
and the inspector dispatcher's `RequestDispatch` (decision 29); nothing else is. Not
catchable from script, not consumed by a `TryCatch`
(`HasTerminated` is how a caller tells it from a throw), remembered if nothing
is running yet, and **it does not interrupt a native callback** on either
engine - a blocking native runs to completion and the unwind happens at the next
JavaScript checkpoint.

That last one is stated bluntly at both `TerminateExecution` and
`RequestInterrupt`, because it is what an embedder needs *before* building a
timeout rather than after: **these reach script, not your own C++, and a watchdog
cannot save you from your own blocking callback.** Every checkpoint either engine
has is inside script; a native that spins, blocks on a socket or waits on a lock
is not one, and has to poll `IsExecutionTerminating()` itself.

Two things a backend must do rather than delegate:

- **The "stop requested" bit is the library's, on both engines.** Neither has
  one. V8's `IsExecutionTerminating` is true only while the termination
  exception is pending, which is essentially never while a native is running, so
  the native that unibind tells to poll would poll false forever; SpiderMonkey has
  nothing sticky at all. An atomic flag set on request and cleared on cancel is
  what makes the promise keepable - and makes it *ours to keep*.
- **A handler re-arms the stop, it does not re-throw it.** `Reset` takes the
  engine's pending termination away with everything else, so both `Reset` and
  the close re-request termination while the flag is set.

And one thing settled rather than inherited, because the engines answer
differently: **a stopped isolate stays stopped until it is cancelled**, whatever
the engine would allow. SpiderMonkey's context is usable the instant the unwind
finishes, with nothing to reset; V8 keeps its termination pending. Taking the
permissive answer would let a stop requested from another thread race the very
next `Script::Run` and sometimes lose, silently, on one backend only.
`CancelTerminateExecution` is not book-keeping - it is how an isolate is made
usable again.

### 16. `Global<T>` compares values, not roots (`unibind/handle.h`)

Two `Global`s can be two roots over one object - `Duplicate` makes exactly that,
and so does rooting the same function twice - so comparing the nodes answers the
wrong question and answers it silently. The shape that gets bitten is a registry
of callbacks kept as `Global`s: the caller hands back the same function through a
different handle and the removal finds nothing.

`StrictEquals` and `SameValue`, against another `Global` or against a `Local`.
Both spellings for the same reason `Local` has both - they differ on `NaN` and
`-0` - and no `operator==`, because a language with two defensible equalities
should not spell one of them with the punctuation that hides which.

**No open `HandleScope` is required**, and that is the load-bearing part: the
backend roots what it needs for the duration of the comparison, so there is no
failure that a caller could mistake for "not equal". An empty `Global` is equal
to nothing at all, including another empty one.

### 17. A class can be callable as well as constructable (`unibind/class.h`)

`Class<T>::ConstructOrCall` opts a class into the middle row of decision 12's
grid; `Construct` stays construct-only and stays the default. The callback is the
same one and tells the two apart with `IsConstructCall()`, and **a plain call
yields an instance**, as `Error()` does.

Letting a plain call return something that is *not* an instance was rejected:
that is a function with a constructor bolted on, which is what `FunctionTemplate`
already is, and it would need a second callback with a different signature - at
which point the one thing the typed layer guarantees, that what comes out of a
`Class<T>` carries a `T`, stops being true.

| made by | `f()` | `new f()` |
|---|---|---|
| `Function::New` | yes | TypeError |
| `FunctionTemplate` | yes | yes |
| `Class<T>` + `Construct` | TypeError | yes |
| `Class<T>` + `ConstructOrCall` | yes, and it makes an instance | yes |

### 18. A stack is frames, and the text is for a human (`unibind/exception.h`)

`StackFrame` carries the three things both engines will name - function name,
script name, line - plus a column where the engine gives one.
`TryCatch::StackFrames` reads one off a caught exception and
`CaptureStackFrames` asks where a native was called from. `TryCatch::StackTrace`
still returns the engine's own text, which is what you show a human and is not
parseable across engines.

The rule that cost a round trip to find: **a stack comes off the Error object,
never off the engine's Message.** V8 will happily capture a trace for `throw 1`
if asked, and a stack invented for a value that never carried one is the
plausible wrong answer this whole API exists to prevent. A thrown non-Error
answers empty.

`TryCatch::Location` is the other half: V8's `v8::Message` - script, line,
column and the text of the line - in one call, and the one thing to print a
syntax error with, because a script that never compiled has no frame. It places an
exception where it was *thrown*, as V8's Message does - an Error made on one
line and thrown on another at the `throw`. SpiderMonkey's error report names
where an Error was made, so that backend reads the stack captured at the throw
and keeps the report for the one thing it is right about, a syntax error. Where the
engine has no position at all - a throw from native code with no script under
it - the answer is empty. Both engines answer that case with a location that
names no line and differ in what they put in the rest, so the header settles it
rather than either backend: no line is no location.

### 19. Compiled-code caching, and how "this engine cannot" is said (`unibind/script.h`)

`CompileWithCache` / `UsedCodeCache` / `CreateCodeCache`. A blob is opaque and
keyed on source *and* engine build *and* flags; a blob that does not match is
rejected and the source compiled normally, so a stale blob costs a wasted compile
rather than a crash - and the key is in the blob, so an embedder keeping blobs
on disk does not have to invent one.

**The engine half of that key is the engine's build identity, not its name**,
and that is the correction this decision needed. Keying on
`Platform::BackendName()` alone is a cache that stops working silently: upgrade
the engine and every stored blob still keys identically, while the engine's own
build-id check refuses all of them, so `UsedCodeCache()` answers false on every
compile for ever and nothing says why. So `detail::BackendBuildId()` - V8's
`ScriptCompiler::CachedDataVersionTag()` behind the backend's name,
SpiderMonkey's process build id, which is the same string the engine is handed
through `SetProcessBuildIdOp` - is what goes into the key. A stale blob is then
rejected here, cheaply, by the check that can also say so, rather than
underneath by the one that cannot.

`Platform::BackendVersion()` is the other half of the same gap and deliberately
*not* the key: it is what the engine calls its own build, in the engine's own
words, for a log line or a bug report. **Diagnostic only and not parseable** -
the two engines' schemes have nothing in common to promise, and a version can
move without the build identity moving or the other way round. An embedder who
needs to branch on an engine version is asking for something this API does not
do.

Caching is **three separate entry points rather than a parameter on `Compile`**,
and that is the decision: a backend whose engine has no code cache defines none
of them, so a caller that reaches for one gets a link error at its own call
site - this library's existing answer for an absent operation. A parameter a
backend quietly ignored would have been the silent answer, and there is no way
to ask a linker about half a function.

**A blob is keyed to its source here, once, and not in a backend.** This is the
part that had to be discovered, and it cost two bugs in one afternoon to find:
neither engine checks that a blob came from the source you are offering it for.
Both stamp a blob with an engine-build identity and refuse one from another
build, and that check is easy to mistake for the whole of it - a SpiderMonkey
stencil encoded from `"a"` decodes cleanly when offered for `"b"`, reports
success, and runs `a`.

It is one safety property at three scales: *these bytes came from this engine*
(the engines check it), *these bytes came from this source* (neither does), and
*these bytes are a blob at all* (neither reliably does). So `unibind/script.h` frames
every blob it emits - magic and format, a hash of the source with its origin and
the backend name, and the payload's length and hash - and checks it before the
engine sees anything. A blob that does not match is never offered.

Scale 3 is the one that cannot be delegated, and it is what was wrong on V8: an
engine may answer a repeat compile out of its own in-isolate compilation cache
without looking at your blob, and is then in no position to say whether the blob
was good - which is how `UsedCodeCache()` came to report true for a payload that
had been bit-flipped or cut in half. The payload length and hash settle it
without asking the engine anything.

**This is not a security property.** A caller that lets someone else choose its
cache file has already lost. It catches the case the promise is about: source
that moved on without its cache. And the only test that sees the failure
**checks what actually ran, not what the API reported** - encode from one
source, offer the blob for another, and run the result.

Two engine traps: V8 only checksums a blob on consume when
`--verify-snapshot-checksum` is on, which it is not in a release build; and
SpiderMonkey's `JS::EncodeStencil` dereferences a null function pointer, rather
than failing, if `JS::SetProcessBuildIdOp` was never called - process-wide state
that belongs in `Platform`'s constructor, so that an embedder cannot reach a
cache API before it exists.

**`CompileOptions::EagerCompile` rides on `Compile` and `CompileWithCache` as a
parameter**, which is not a contradiction of the paragraph above: it is not a
capability an engine may lack - both compile lazily by default and both can be
told not to - so there is no absence for a link error to report. It exists for
the cache. A blob covers what had been compiled when it was made, and on both
engines that is the top level and whatever functions had run, so an embedder
that keeps blobs wants the compile they are made from to be eager.

Two things had to be decided. **V8 will not consume a cache and compile eagerly
in one call**, so `CompileWithCache` with `EagerCompile` means: the blob decides
when it is used - it is what was compiled, and a blob made eagerly is already
eager - and the option decides when the blob is absent or refused. The refused
case is the one an embedder is relying on, because it is the one about to make a
fresh blob, and it is the one V8 gets wrong by default: a blob it refuses falls
back to an ordinary lazy compile. Its backend therefore asks
`CachedData::CompatibilityCheck` first when eager was asked for, and never
offers a blob that would be refused. And **V8 answers a repeat compile out of
its in-isolate cache with whatever it compiled first**, so source compiled
lazily once and then eagerly came back lazy, with nothing to say so. That cache
keys on host-defined options, which this library otherwise never sets, so an
eager compile carries a mark there and is filed apart. SpiderMonkey has no such
cache and needs neither; its eager compile is `ParseEverythingEagerly`.

The suite holds this by the one thing about a blob that is portable to look at,
its size - an eager blob is larger than a lazy one of the same source, and no
smaller than a lazy one made after every function has run, on both engines and
in every build of them (`docs/testing.md` says why not by a fixed factor) -
including for an eager compile of source the same isolate had
just compiled lazily, and one that followed a refused blob.

### 20. What belongs to an isolate, what belongs to the process (`unibind/isolate.h`)

`IsolateOptions` gains `stackLimitBytes`, which turns runaway recursion into an
exception a script can catch instead of a stack overflow in the host process:
per-isolate on both engines, measured the same way, and preventing a failure an
embedder cannot catch by other means - though *which* error is the engine's
business and the two do not agree (`RangeError` on one, `InternalError` on the
other), so a portable script catches it rather than asking what it is.

Refused, in writing and with reasons: interpreter-only /
jitless, collector tuning, `eval` control (per *realm*, so it would go on
`Context`), microtask policy (`PumpJobs` *is* the policy, and fixed on purpose),
locale and random seed.

`PlatformOptions` is new and holds what is the process's on both engines:
`engineFlags`, which moved off `IsolateOptions` because a flag string set per
isolate would silently apply to every isolate before it, and `workerThreads`.

`workerThreads` is a **hint at every value, with an observable answer** -
`Platform::WorkerThreads()`, which reports the count actually in effect. A bool
would not do: honoured, clamped and ignored are the three outcomes an embedder
has to tell apart, and an empty answer means the engine would not say, which is
not the same as zero. SpiderMonkey cannot comply: it builds its
pool in `JS_Init`, ignores `JSGC_MAX_HELPER_THREADS = 0`, and the one hook that
replaces the pool accepts the callback and then crashes on the first collection.
Of "ignore silently", "crash later" and "refuse", only the first survives - and a
hint nobody can observe is the mistake `UsedCodeCache` avoids.

Why this is a hint while a second isolate is refused outright (decision 11),
which looks like the opposite policy: **that rule is about semantics a program
observes**, and this is not. Worker threads change timing and where work happens,
never what a script sees. An ignored hint is not a portability cliff; making V8
refuse a mode it supports perfectly well would throw a real capability away to
buy symmetry.

### 21. Binary data is copied, in both directions (`unibind/value.h`)

`ArrayBuffer` and `TypedArray`, so an embedder can hand over a
`std::span<const T>` in one crossing instead of N `Array::Set` calls building the
type script did not want. A buffer backed by embedder memory was rejected on
three counts, any one sufficient: **detachment** (script can detach a buffer, so
ownership moves on a schedule script controls, and the engines hand it back on
different terms), **no stable interior pointer** (both collectors move, so a
borrowed span would be a handle with none of the rules handles have here), and
**free-callback contracts that do not match**.

This does not disturb decision 8. A typed array still reports `ValueKind::Object`
because `Object` is still exactly the set of *property* operations offered for
it; the new operations are reached through `Is<ArrayBuffer>()` and
`To<TypedArray>()`, which answer without a kind of their own - which is what the
argument for collapsing exotic objects into `Object` predicted.

`ArrayBufferView` and `DataView` follow V8's hierarchy - `TypedArray` and
`DataView` are both views - and the same two rules. A view's byte-level
questions (`ByteLength`, `ByteOffset`, `GetBuffer`, and `CopyBytes`, which is
V8's `CopyContents`) take any view and copy exactly its own range. Two answers
had to be made the same: **a view whose buffer script has detached answers zero
for its offset as well as its length**, which is V8's answer and not what
SpiderMonkey's raw accessors give, and **`GetBuffer` on a view over a
`SharedArrayBuffer` is empty** rather than a handle typed `ArrayBuffer` over
something that is not one - V8's `Buffer()` would have handed it back under that
type.

### 22. Moving a value between isolates is structured clone (`unibind/value.h`)

One isolate per thread (decision 11) means several scripts is several isolates,
and a handle belongs to one of them, so the portable answer is to write the value
down and build it again. `Serialize` / `Deserialize`.

**A blob is opaque bytes belonging to the engine build that wrote them**, and
that is the whole contract. Values move between isolates of *one* engine, which
is the stated scope rather than a limitation to route around: this library picks
an engine at build time, and a format two engines could both read is a different
and much larger thing to design. Keeping one past the process is outside what it
promises.

Two sub-decisions. **Owned bytes, not a span into engine storage** - a span
would have to survive operations that collect and be released on the isolate's
own thread, which is the thread the blob is being carried away from. And **a
value that will not clone fails the whole call** rather than becoming
`undefined`: neither engine can substitute below the top level without
reimplementing the algorithm, and a rule that held for a top-level function but
not for one two properties down is worse than no rule. A caller serialising a
list calls this once per value and decides for itself, which keeps the list's
shape and puts the decision where it can be seen.

### 23. One drain, for promises and for posted work (`unibind/isolate.h`)

The hole this closes was silent in the worst way: script containing
`async`/`await` compiled and ran and its continuations never executed - no error,
no exception, the work simply did not happen. `Isolate::PumpJobs` is the drain,
`unibind/value.h` says so where an embedder will read it, and the promise surface is
deliberately small (make a pending promise, hand it to script, settle it later),
with native `then`, unhandled-rejection reporting and native async functions
written down as out of scope rather than left to be discovered.

**The engine's own automatic draining is turned off.** V8's default policy drains
when a call returns and SpiderMonkey's never does, so left alone the same script
would run its continuations after every `Script::Run` on one backend and never on
the other. When a continuation runs is something a script can observe, so unlike
a worker-thread count it is made uniform rather than left as a hint - decision
11's principle, applied to the case it was written for.

`PostJob` queues work from any thread and `PumpJobs` runs it, engine jobs first,
then one piece of posted work, then round again, so work that settles a promise
sees its continuations in the same pump, before the next piece of work. Ordered,
never coalesced; whatever is queued when the isolate is destroyed is **dropped,
not run**, said out loud because "it will run eventually" is what a caller would
otherwise assume.

And posting does not wake anything: work runs when the script thread next pumps,
and **nothing accelerates that** short of terminating what is running. The
reasoning is under decision 24, because the thing an embedder reaches for first
is the interrupt and it does not help.

`PostDelayedJob` is `PostJob` with a floor on when - V8's `PostDelayedTask`,
for the timer an embedder would otherwise keep beside the isolate - and the
floor is all it adds. Measured on a steady clock from the call, a delayed job
becomes posted work at the first pump after it falls due and joins the back of
the queue there, so work posted before that pump runs first whenever it was
posted; delayed jobs among themselves run in the order they fall due, and in
posting order when they fall due together. **Nothing wakes the thread when one
falls due**, for the same reason nothing wakes it for posted work, so an
embedder that sleeps between pumps bounds the lateness by how long it sleeps.
A delay of zero, a negative one or a NaN is no delay, and one still waiting at
teardown is dropped like the rest.

### 24. What an interrupt callback may do (`unibind/isolate.h`)

`RequestInterrupt` is the other way to reach a running script from another
thread, and - the inspector dispatcher's `RequestDispatch` aside, which is built on it - the
only way to reach the isolate's thread without waiting for the script to finish. The contract is a
split, not a ban:

| inside an interrupt callback | |
|---|---|
| make and read handles, inspect values | yes |
| read and write embedder state, set a flag, queue a job | yes |
| call a JavaScript function, run a script, throw | **no** |

Handles are fine because the engine sets a scope up for exactly this - V8 opens a
fresh `HandleScope` and an external VM state around the call, deliberately
unsealing what the stack guard had sealed.

Running script is not, and the engines were measured rather than assumed: a
staged probe inside a live SpiderMonkey interrupt, fired from another thread into
a `while (true)` loop, got all the way through - made an object, set a property,
called a JavaScript function that really ran, compiled and ran a fresh script,
captured a stack - after which the loop terminated normally and the isolate was
fine. V8 refuses the same thing outright.

**The stricter rule stands, and not for symmetry.** An interrupt fires between
two bytecodes of whatever was running, so script run there runs at an arbitrary
point inside unrelated code and anything it leaves pending - an exception above
all - is left for the interrupted frame to trip over. That probe survived only
because it cleared its own exceptions at every stage; a callback that threw and
returned normally would hand the interrupted code an exception it never threw.
The identical hazard is visible in V8's dispatch, where a termination is also
processed earlier in the *same* interrupt pass - two engines, arrived at
independently, which is about as good as this kind of evidence gets.

*"You may, but you must clean up perfectly afterwards" is exactly the kind of
rule that is better written as "you may not."* Obeying it leaves no capability
unimplemented and makes nothing slower: the restriction falls on the embedder
rather than on a backend, which is the right place for it.

So **a posted job runs at the drain point, never inside the interrupt** - that
sentence is the difference between this working and failing under load.

What an interrupt *is* for is narrow, and both uses share the one property a
foreign thread cannot arrange any other way, being **on the isolate's thread
while script is running**: a profiler tick, and a watchdog that samples state
on-thread before deciding whether to terminate. Both read; neither runs. Note
that `TerminateExecution` is callable from any thread by itself, so the
interrupt earns its place in the second only when the decision needs to read
something first.

**It is not a way to make posted work run sooner, and that had to be traced
rather than assumed.** If the script thread is in a long-running script, the
interrupt fires, the callback cannot run the work and cannot make the script
yield - there is no yield, suspend or resume anywhere in this API - so the script
resumes, runs to completion, and the thread pumps afterwards, which is what
would have happened with no interrupt at all. If the thread is idle, no script is
running, no checkpoint is reached, and the interrupt never fires. Neither state
moves forward, so the pairing advice `PostJob` used to carry is gone.

That bounds what "promptly" can mean for an event-driven embedding, and the
bound is worth stating in one place: **a running script cannot be made to give
the thread back, only terminated**, and a native that blocks is not interruptible
either. Keep scripts short and pump between them, or terminate and restart.

### 25. `ub::Maybe` is gone (`unibind/types.h`)

It renamed `std::optional` and bought nothing. The part worth keeping was the
prose hanging off it - what an empty optional means, and where to ask *why* - and
that is now a section at the top of `unibind/types.h` covering all three causes: a
script throw (pending on the isolate, `TryCatch::HasCaught`), an engine failure
(`Isolate::HasPendingException`), and a termination (`TryCatch::HasTerminated`),
plus why a handle is the one deliberate exception to the shape.

Nothing under `include/unibind/`, `src/backends/v8/` or `tests/` names it. One
transitional typedef remains in `unibind/types.h` for
`src/backends/spidermonkey/`, which is the last user; it goes when that backend
is respelled.

### 26. The engine is chosen at the link, not at the compile (`include/unibind/config.h`)

A consumer compiles their own code **once**, against the public headers, and
picks `unibind_backend_v8` or `unibind_backend_spidermonkey` at the final link.
Before this, the generated `config.h` carried the backend's identity and the
`detect_mismatch` tag carried its name, so an object compiled for one engine
was refused by the other's link and a program that shipped both engines had to
compile everything twice.

Nothing was actually *making* that true - it was being enforced. No public
header branched on `UNIBIND_BACKEND_V8` or `UNIBIND_BACKEND_SPIDERMONKEY`, the
storage budgets were already one set of numbers rather than per-backend ones,
and the headers-only target was already compiling the whole surface with no
backend at all. All three were checked rather than assumed before the identity
came out.

What is in the tag now is what a difference in really is corruption: the
architecture, `UNIBIND_HANDLE_CHECKS`, and the three storage budgets with the
frame's slot count. The `config/<backend>/` install directory is gone with it -
`config.h` sits beside the public headers, because it is the same file for
every backend of an architecture - and the CMake package grew the split that
goes with it: `unibind::headers` (no engine), `unibind::backend_<name>` (one
engine, as a link input), and `unibind::unibind` for the ordinary consumer who
links one.

Two rules come out of the tag and have to be written down, because nothing
would catch breaking them otherwise:

- **No public header may name the backend.** It cannot be a compile error -
  `#if UNIBIND_BACKEND_V8` against a macro nobody defines is silently false in
  every build, including the V8 one - so it is read rather than compiled, by
  `tools/headers_only/backend_neutral.cmake`, which fails the build. That is
  the same directory and the same discipline as the rule about engine headers,
  which is compiled; the two halves sit together on purpose. Which backend is
  linked has a runtime answer, `Platform::BackendName()`, and `unibind/script.h`
  is the proof that runtime is early enough: it keys a cache blob on the engine
  and the same object gets the right key under either one.
- **Each storage budget is the maximum across every backend for that
  architecture**, not a per-backend figure. A consumer compiles those sizes
  into their own stack frames once, so a number that varied by engine would be
  a different `HandleScope` per engine and the single compile would be a lie. A
  future backend whose frame is larger raises the number **for everyone**; it
  does not get one of its own. The cost is stack bytes in a scope the caller
  already had.

Architecture stays in the tag, and stays per-architecture, because a build is
one architecture and always was: x86 and x64 are separate prefixes, so x86 is
not charged for x64's growth.

The proof is `examples/embed`, which compiles `main.cpp` into one object
library against `unibind::headers` and links it once per backend the prefix
holds. Two executables, one compile, and each prints its own engine.

### 27. Nothing an isolate handed out may outlive it (`unibind/isolate.h`)

A `Context`, a `Script` and a `Global<T>` are the three things an embedder can
hold across handle frames, and all three are `new`ed on demand and `delete`d by
the call that gives them back - which is the embedder's to make. Unlike
templates, classes and boxes, they are in no container the isolate can empty at
teardown, so one still alive when `~Isolate` runs is **two** faults at once:
its memory is never given back, and the release that eventually comes resets an
engine handle against a disposed isolate - on SpiderMonkey, unlinks a
`PersistentRooted` from a runtime that no longer exists.

The rule was true and unstated, which is the worst of the three available
states. Of the two ways to fix that - document it, or make teardown tolerant -
this is the first, because the second is not cheap: tolerance means every one
of those records carries an owner-validity check on every use, for a case that
is an embedder's ordering mistake rather than something the API asks for.
Declaring the isolate before everything that uses it is enough, and is what
every example here already does.

So it is written where an embedder reads it (the `Isolate` class comment, with
a line at `Context`, `Script` and `Global<T>`) and a checked build counts them
and asserts in `~Isolate`, where the mistake is, rather than leaving it to be
found at the crash. A release build does neither check nor tolerate it, which
is the same bargain `UNIBIND_HANDLE_CHECKS` already makes for a handle used
after its frame closed.

**The assert goes at the end of the teardown, not the start**, and that is not
a detail - it is the rule saying precisely what it means. A native the isolate
destroys may itself own a realm and a root: the sandbox in
`cases/sandbox_test.cpp` is a `Class<T>` whose native holds a `Context` and a
`Global<Object>`, and `~Isolate` destroying that box is what gives them back.
Those are the isolate's to release, at teardown, and counting them as a
violation would make the composition the suite is proudest of illegal. What is
outstanding *after* the isolate has emptied everything it owns is what nothing
but the embedder holds, and that is the thing the rule is about.

This does not disturb decision 13. Templates and their records outlive nothing
and are the isolate's own; these three are the embedder's, and that is exactly
why they need a rule rather than a container.

### 28. The engine reporting that *it* is in trouble (`unibind/isolate.h`)

Everything else in this API reports a failure to whoever asked for something.
There was no channel at all for the other direction - no out-of-memory
notification, no near-heap-limit hook, no fatal-error handler - so a
long-running embedder, which is this library's stated case, found out that its
engine was failing by dying.

`PlatformOptions::onEngineFault` is that channel: **one callback with a kind,
not four callbacks.** `EngineFault` is `OutOfMemory` and `Fatal` today, and
`EngineFaultReport` carries the kind, the isolate it happened in (or null), and
the engine's own words for where and what.

**It is `EngineFault`, not `EngineError`.** `ErrorKind` already exists and means
almost the opposite - a JavaScript `Error` constructor that native code
*throws*, a value, catchable by script - and `EngineError` beside `ErrorKind` is
two names sharing a word for two things sharing nothing. Renaming the existing
one instead (`ThrowKind`, say) was considered and dropped: it touches every
throwing call site in the tree and in an embedder's code, and it buys nothing
once the new name shares no word with it.

#### `NearHeapLimit` is not a kind, and the reason is a link error

V8's near-heap-limit callback **returns a new heap limit**: raise it, or hand
back the current one and let the isolate die. That does not fit a `void`
reporting callback, and it is the most valuable thing here for an embedder that
must not be killed by one runaway script. Two shapes were available and the
rejected one is worse for a reason that is not about types:

- *Rejected:* fold it in and let the reporting callback return something the
  other kinds ignore. **SpiderMonkey has no near-heap-limit hook at all** -
  checked before assuming, because this project has been wrong twice about
  whose shape is the portable one. Not a differently-shaped hook: nothing. Its
  memory callbacks are `SetOutOfMemoryCallback`, which fires *after* an
  allocation has already failed, and `SetProcessLargeAllocationFailureCallback`,
  which is process-wide, takes no context, and asks the embedder to *free*
  memory before a retry rather than asking it to decide a limit. So folding it
  in produces a `case NearHeapLimit: return raisedLimit;` that compiles on
  SpiderMonkey and never once runs - the silent half-portable surface this
  library exists to prevent.
- *Chosen:* `Isolate::SetHeapLimitCallback`, its own entry point with its own
  shape, **defined only by the backend that has it**. A SpiderMonkey build does
  not link a call to it. That is decision 19's answer applied unchanged - the
  compiled-code cache is three entry points for exactly this reason - and it is
  better than documentation, because it is a build error at the call site rather
  than a paragraph the embedder has to have read.

It is also not a *fault*, which is the second half of the same argument: nothing
has failed when it runs. The heap is near its ceiling, collection did not help,
and what happens next is decided by what the embedder returns. A report and a
decision are different things and reading one as the other is how an API grows a
return value that means nothing three quarters of the time.

The policy the header documents is raise **and** terminate: enough ceiling to
unwind in, plus a stop for the script that filled the heap. Either alone is
useless - a bigger heap for a runaway script, or an unwind with no room to
happen in - and together they turn "the process dies" into "that script
stopped".

#### `Platform`, with the isolate in the report

The kinds are not all at the same scope. On V8, out-of-memory and near-heap-limit
are per-isolate while a failed internal check is process-wide; on SpiderMonkey
the out-of-memory hook is per-context, which is per-isolate here.

*Rejected: on `Isolate`.* It reads right - the one kind both engines raise is
per-isolate on both - and it is deaf at the three moments this exists for. A
fault inside `Isolate::New` has no isolate to have been registered on, and that
is the single moment an embedder most wants to hear about; so is a failure on a
thread of the engine's own; so is V8's process-wide check, which names a file
and a line and no heap.

*Chosen: `PlatformOptions`, with `EngineFaultReport::isolate`.* A `Platform` is
the scope of the whole program's use of the engine, so a handler installed there
is armed before the first isolate and still armed after the last - and the
report gives back everything the per-isolate shape would have bought.
`workerThreads` is the precedent and decision 11 is what makes it cheap: one
isolate per thread means a fault that names no heap can still be attributed to
one, so `isolate` is populated far more often than the engines' own hooks would
manage, and null is a real answer rather than a shrug.

*And it is set at construction rather than by a setter*, which is the part that
would be easy to get wrong in the other direction. It can arrive **on any
thread**; a settable handler is a function pointer plus a data pointer that one
thread may be part-way through reading while another faults, and the obvious fix
is a lock taken at out-of-memory, possibly by the thread already holding it.
Fixed for the life of the `Platform` it needs neither, and it is what the
engines want anyway: V8's process-level handlers are state to install before the
engine comes up, and SpiderMonkey's process hook may be set at most once. An
embedder whose policy changes changes it behind the fixed handler, through
`engineFaultData` - the indirection this API asks for everywhere else.

#### Which kinds each backend can actually raise, and the one that is nobody's

| kind | V8 | SpiderMonkey | CPython |
|---|---|---|---|
| `OutOfMemory` | `Isolate::SetOOMErrorHandler`, plus the library's own frame exhaustion | `JS::SetOutOfMemoryCallback`, which every internal out-of-memory funnels through - frame exhaustion included, because the backend already reports that one through `JS_ReportOutOfMemory` | the backend's own allocator hooks refusing an allocation at `heapLimitBytes` (decision 37), once per crossing |
| `Fatal` | `Isolate::SetFatalErrorHandler`, `V8::SetFatalErrorHandler`, and `V8::SetDcheckErrorHandler` | `MOZ_CRASH` (so every `MOZ_RELEASE_ASSERT`, and a debug engine's `MOZ_ASSERT`), recognised by a vectored exception handler | a failed bring-up only - no standard library (possible only with it not embedded), or `Py_InitializeFromConfig` failing - and the process is not ended: the `Platform` is left uninitialised. `Py_FatalError` is not hooked |

SpiderMonkey's fatal path has no embedder hook, but it does not have to have one.
`mozilla/Assertions.h` fixes what `MOZ_CRASH` does on Windows - store the reason
in the exported `gMozCrashReason`, then `__debugbreak()`, then a null write - and
the engine's objects store that reason at every crash site. A vectored exception
handler that sees a breakpoint or access violation *with that reason set* is
therefore seeing the engine die and knows why, and nothing else in a process sets
the variable. It reports and then ends the process with the engine's own exit
code, so the embedder's unhandled-exception filter does not receive the same
death a second time with less to say about it. An embedder installs
`onEngineFault` instead of each engine's own hooks, which is what it is for.

**An engine assertion is a `Fatal`, not a kind of its own.** `DCHECK` and
`MOZ_ASSERT` compile out of a release engine, but debug engines are published
too and a consumer's Debug build links them, so an assertion does fire in a build
that ships. When it does, the engine is as finished as after any other failed
check and nothing an embedder would do differs, so it is routed into `Fatal`,
and `message` says which check it was.

`Fatal` **ends the process, and installing a handler does not change that.**
V8 with no handler prints and aborts; V8 *with* one would hand the failed check
back to whatever was running. A diagnostic hook that decided whether the program
survives would be the worst of both, so the backend reports first and ends the
process second, and `Fatal` means one thing rather than two.

`OutOfMemory` could not be made uniform in the other direction, and this is the
one place the decision settles for less than a promise. Whether anything
survives depends on *which* allocation failed and on which engine: the library's
own failures are recoverable on both - the operation answers empty, the isolate
carries on, and the suite asserts exactly that - while the engines' own are not
comparable, SpiderMonkey reporting an out-of-memory condition into the running
operation and carrying on where V8 treats its heap giving out as fatal and ends
the process once the callback returns. Nor does the report reliably say which
one this is.

Making it uniform would mean either making V8 survive its own fatal
out-of-memory, which is not in an embedder's gift, or making SpiderMonkey die
where it need not, which is decision 20's mistake in a worse place. So the
header gives one rule that is correct under all four combinations - **write a
handler that is correct if the next line never runs** - and tells an embedder
*not* to try to tell recovery from death, because the information to do it with
is not there.

#### What a handler may do, and the one rule that is not caution

The contract is `RequestInterrupt`'s (decision 24) minus handles, minus
allocation. Embedder state and `TerminateExecution` yes; a handle, a value, a
call into the engine, script, a throw - no.

**Allocating is not merely discouraged, it is the definition of the situation.**
The report that arrives most often says that allocation is failing, so
allocating in order to report it is a bug in every case and a crash in the
interesting one. That rule was already being kept inside the backends, in the
place this decision had to plug into: the message a frame that cannot grow
carries is a string literal *precisely so that saying it costs nothing*. So the
report is a stack aggregate of borrowed views, nothing in delivering it
allocates, and the header tells an embedder to reserve the buffer and open the
log file before it needs them.

Handles are refused for a second reason on top of that one: an interrupt may
make them because the engine sets a scope up for exactly that, and nothing sets
one up here.

### 29. The inspector is offered where the engine has one (`unibind/inspector.h`)

This reverses what used to stand here, which was that a debugger is out of
scope. The reasoning behind that was sound as far as it went: V8's debugging
surface is the inspector protocol - a C++ channel carrying CDP messages - while
SpiderMonkey's is the `Debugger` object, a JavaScript API installed into a
debuggee realm, and the two share no shape to abstract. What changed is the
requirement. **A consumer compiles against these headers and nothing else**:
anything it needs from V8 is abstracted here, and the one thing that still sent
a consumer to V8 directly - through `unibind/interop/v8.h` - was a Chrome
DevTools inspector. So there is now an inspector API, and it is V8's
`v8_inspector` in this library's terms: an embedder-written `InspectorClient`,
one `Inspector` per isolate, an `InspectorSession` per connection.

It does not pretend SpiderMonkey has one. **`Inspector::Supported()` answers at
run time, and every member is defined on every backend**, so a program links
either way; on SpiderMonkey `New` returns null and nothing else is reachable.
That is deliberately not decision 19's link error, and the difference is the
point: whether to open a DevTools port is decided when a program starts, not
when it is built, and one binary has to be able to make that decision against
either engine. What keeps it from being the silent answer decision 19 refuses is
that there is no parameter here a backend quietly ignores - only an object a
backend declines to make, and a null pointer is not used by accident.

These had to be decided on top of V8's shape:

- **One thread may be foreign, and it holds a dispatcher, not the inspector.**
  `InspectorDispatcher::RequestDispatch` is how a message read on a socket
  thread reaches the isolate, and it runs the embedder's callback at whichever
  comes first: an interrupt, which reaches a script that is running and never
  fires while the isolate is idle, or the next `PumpJobs`, which is the other
  way round. Unlike `RequestInterrupt`'s callback (decision 24) it may dispatch
  protocol messages, which run script - V8's inspector is designed to be driven
  from exactly that point, and it is how a busy isolate still answers DevTools.
  It used to be a member of `Inspector`, which made it unusable exactly where it
  is needed: the socket thread cannot know when the isolate's thread destroys
  the inspector, so every embedder had to put a mutex of its own around the call
  and the destruction. The dispatcher is V8's `TaskRunner` shape instead - a
  `std::shared_ptr` from `Inspector::Dispatcher()` that any thread may hold for
  as long as it likes, safe through the inspector's destruction and after it,
  where a request answers false and never runs. Its mutex is the one the
  embedder no longer writes: `~Inspector` takes it to mark the dispatcher
  orphaned, so a request either finishes asking the isolate for its wake-up
  first or finds it orphaned. A wake-up still in flight is handed the isolate,
  which outlives it, and finds the queue through the isolate's reference to the
  current inspector - none, once it has gone.
- **The wake-up is coalesced, the work is not.** Requests made before the
  isolate gets to the first of them share one interrupt and one posted job, and
  every one of them still runs, once, in order - the same callback and data
  asked for twice run twice. Coalescing belongs here rather than in the
  embedder, because only the backend knows whether a wake-up is already on its
  way; without it a burst of protocol messages costs an engine interrupt and a
  posted job each, which the suite measures. What was not done is merging
  requests themselves, because "each request runs exactly once" is a contract a
  caller can build on and "at least one of several runs" is not.
- **`New` and `Connect` are null only for want of memory**, besides `New` on a
  backend without an inspector or an isolate that already has one. The engine
  refuses no connection, so null from `Connect` is not a condition to wait out.
  The backend makes all of its own allocations before it asks the engine for
  anything, so that the null is a clean one.
- **`Resume`, `Stop` and destruction, in any order.** `Resume` leaves a pause
  (the engine calls `QuitMessageLoopOnPause` before it returns) and does nothing
  outside one or after `Stop`. `Stop` is final and idempotent, and during a
  pause it ends the pause unless another session still has the debugger on. A
  session may be destroyed anywhere on the isolate's thread, inside
  `RunMessageLoopOnPause` and inside a callback of its own dispatch included:
  that is `Stop`, and the client hears nothing more from it. V8 supports that
  by design - it reaches a session through weak pointers across exactly those
  calls - so nothing is deferred there. A notification is the exception: it is
  sent from inside the agent that raised it, which V8 goes on using, so a
  session destroyed or stopped inside one detaches at once and leaves the
  engine's half for the next call into the inspector, the next pause, or a
  wake-up. The suite holds each of these on every backend that has an
  inspector.
- **The default realm** - where an evaluation naming no context runs - is the
  one announced most recently and not yet withdrawn. The inspector holds realms
  weakly: DevTools seeing one is no reason for it to live.
- **Two names are not V8's.** `sendResponse` and `sendNotification` are one
  `SendProtocolMessage`, because an embedder forwards both to the same socket;
  and neither that nor `DispatchProtocolMessage` is spelled `SendMessage` or
  `DispatchMessage`, which `<windows.h>` defines as macros. A member with either
  name is silently renamed in whichever translation units included it first - an
  override that overrides nothing, or a call that links against a function
  nobody defined.

`unibind/interop/v8.h` stays, for whatever else only V8 can do, and it is still
the one header whose functions only one backend defines. The inspector no longer
needs it.

### 30. A backend's scripts are its engine's language (`docs/python.md`)

The CPython backend runs Python. What `ub::` abstracts is the *binding layer* -
values, handles, objects, classes, templates, interceptors, exceptions, realms,
jobs - and that compiles once and links against every backend. What it does not
abstract, and cannot, is the source text handed to `Evaluate`, `Script::Compile`
and `CompileWithCache`. So a program's C++ carries over and its scripts do not.

Three consequences, each decided rather than drifted into:

- **The shared suite does not run on this backend, and parity leaves it out.**
  `tests/cases/` is JavaScript source as much as C++; running it here would be
  a wall of syntax errors, and "translating" it would be a second suite
  pretending to be the first. `tests/python/` is written the same way - `ub::` only, no engine
  header, Python as the script language - and `tests/cmake/RunParity.cmake` skips
  the python directory by name.
- **No public header changed.** No signature, no rule and no storage budget:
  the frame, the `TryCatch` and the `ContextScope` all fit the sizes the two
  JavaScript engines had already set. The backend-neutrality check needed
  nothing either. That is the strongest evidence yet that the API is a design
  rather than a description of V8.
- **Where the two languages disagree, the rule is per spelling.** An operation
  the API promises to C++ keeps the promise as V8 keeps it; a Python spelling of
  the same thing - `o.x`, `del o[k]`, `len(o)` - keeps Python's meaning, because a
  Python programmer is the one reading it. Decision 33 is that rule applied.

### 31. An isolate is a sub-interpreter with its own GIL (`docs/python.md`)

`Isolate::New` is `Py_NewInterpreterFromConfig` with `OWN_GIL`, its own object
allocator, no fork or exec, threads but no daemon threads, and
`check_multi_interp_extensions` on; the thread state it comes with stays attached
to the calling thread until `~Isolate`. So no operation takes a GIL on the way in -
the only thread allowed to call one holds it already - and isolates on different
threads run Python in parallel. Decision 11, one isolate per thread, is what this
needed anyway, arrived at from the other side.

`Platform` initialises the **main** interpreter, isolated from the environment,
and then detaches it: it owns the process-wide machinery and never runs a
script. A realm is a globals dictionary in the isolate's interpreter, which is
what CPython can offer - and it means **realms of one isolate share `sys.modules`
and `builtins`**. On the JavaScript engines a realm has its own built-ins; here
two sandboxes that must not see each other's module state are two isolates.

**The dictionary is script's, so the backend keeps nothing in it.** A realm's
record is ended by a dictionary watcher when the dictionary is deallocated, not
by anything stored under one of its keys: the first version hung the record on a
capsule in the globals, and `globals().clear()` freed it while a `Context` still
named the realm, while `dict(globals())` kept it alive past its dictionary so
that the next dictionary allocated at that address inherited the wrong realm.

The standard library's extension modules that keep state in C globals are
refused in such an interpreter, by CPython's own check - `ctypes` and XML parsing
among them - and the pure-Python half of the library is read from disk when the
`Platform` is made. Both are the engine's terms for being embedded this way, and
`docs/python.md` section 10 is what they cost a program that ships.

### 32. `undefined` is `None`; `null` is `unibind.null`; a Number is decided by value (`docs/python.md`)

`undefined` had to be `None`: it is what a Python function with no `return`
answers, what a missing argument reads as, and what a hole in a list is. `null`
is then a singleton of the backend's own, falsy and distinct, because collapsing
the two would make `IsNull` and `IsUndefined` a coin toss.

A Number is an `int` when it is integral and within ±2^53 and a `float`
otherwise, **whatever C++ type it was made from** - so a script can index, slice
and `range()` with what a native returned - and `-0` stays a `float`. An `int`
beyond ±2^53 is a `BigInt`, because a Number cannot hold it exactly. The
alternative, a `float` for every `double`, would make every native that returns a
count or an index return something Python refuses to index with.

The coercions - `ToBoolean`, `ToString`, `ToNumber`, `LooseEquals` - are Python's
where Python has one: `bool()`, `str()`, `float()`, `==`. A backend that
reimplemented JavaScript's truthiness over Python values would disagree with every
`if` in the scripts it runs, which is a worse surprise than disagreeing with V8.

### 33. `unibind.Object`: JavaScript's semantics to C++, Python's to Python (`docs/python.md`)

A Python object has attributes and items but no `[[Prototype]]`, no per-property
attributes and nowhere for an accessor to run native code. `unibind.Object` is the
object that has all of them, and everything `Object::New`, a template or a class
makes is one. Property operations on anything else map onto what it already has:
a dict's items, a list's indices and `length`, any other object's attributes.
What those cannot carry - a `ReadOnly` item, an accessor on a dict - is refused,
not faked.

The rule from decision 30 decides every disagreement, and the list is in
`docs/python.md` section 3. The ones that show: a missing property is `undefined`
to C++ and `AttributeError`/`KeyError` to Python; a refused write answers true
from C++ (V8's sloppy `Set`) and is a `TypeError` from Python, where there is no
sloppy mode; `len()` is the enumerable own keys and `bool()` is always true;
iteration is over keys unless a `Symbol.iterator` method says otherwise, and a
JavaScript-style iterator is adapted; `repr` is an object literal that never runs
an accessor; and an interceptor is not asked about dunder attribute names, which
Python's own machinery looks up constantly. `Array::New` is capped at 2^26
elements, because a Python list is dense and a sparse JavaScript array is not.

### 34. A function template is a Python type per realm, and calling it is `new` (`docs/python.md`)

What a `FunctionTemplate` or a `Class<T>` materialises into is a heap type
derived from `unibind.Object`, one per realm, whose metaclass
`unibind.TemplateType` records the template. The prototype is its `prototype`
attribute, statics are type attributes, and a `ReadOnly` or `DontDelete` static
holds on the type. That is the only shape under which `isinstance`, subclassing
and `super()` - which a Python programmer will reach for immediately - mean
anything.

It changes three things decisions 12, 13 and 17 assumed:

- **Python has one spelling for call and construct**, so calling the type from
  Python is `new`, and a construct-only `Class<T>` is callable from Python
  because that call is the construction. The plain call of decision 12's grid
  is reachable only from C++, through `Function::Call`, where it behaves as the
  grid says.
- **`Inherit` chains the types, not only the prototypes**, so that
  `isinstance(child, Parent)` is right - and a static declared on the parent is
  therefore visible on the child, where `unibind/template.h` says V8's is not.
- **A Python subclass's own members come before the prototype chain**, as a
  subclass's methods override its base's in Python, and a template's type
  mirrors its prototype's native methods as class attributes, marked so that
  ordinary lookup ignores them, so that `super()` finds them.

The per-realm cache of types lives inside the realm's globals, not in the
realm's C++ record: there, the cycle through a type, its prototype, a function
stored on it and that function's `__globals__` is one the collector can see and
break. Held from C++, the realm would have lived until its isolate did.

### 35. Stopping CPython: a pending call that re-arms itself, and `sys.monitoring` (`docs/python.md`)

CPython has no terminate and no interrupt, only the per-interpreter pending call
that the eval breaker drains. `TerminateExecution` sets the library's flag
(decision 15) and queues one that raises `unibind.Terminated` - a
`BaseException`, so `except Exception` does not see it - and **re-queues itself
while the flag is set**, so that a script which catches `BaseException` meets it
again at the next back-edge or call. On top, `sys.monitoring` LINE and CALL events
are switched on while a stop is in force, which closes the two gaps a pending call
leaves: straight-line code between checks, and a call to a builtin, which is
checked only after it returns. The result is decision 15's promise kept to the
letter, including V8's: a stopped script's `finally` does not run. PEP 669 costs
nothing until events are set, so neither does this.

`RequestInterrupt` rides the same pending call, which is also why an interrupt
waits out a stop and runs after the cancel.

What it cannot reach is the same as everywhere - native code - with a
Python-shaped addition: **a single long-running builtin is native code**, so
`sum(range(10**9))` or `time.sleep(60)` runs to its end before the stop lands.

**The stop is the interpreter's, so it reaches every thread of it** - a
`threading.Thread` the script started is stopped with the script. And
**`~Isolate` stops those threads itself** and waits up to two seconds for them,
because `Py_EndInterpreter` under a live thread is a fatal error and CPython's
own answer - join it - can wait for ever. A thread still inside a call that has
not returned after that is not waited for: the isolate leaves its interpreter
behind, the isolate's thread is free at once, and `~Platform` ends the
interpreter later if the thread has finished by then - or, if it has not, skips
`Py_FinalizeEx`, which would be fatal with a sub-interpreter left. The cost is
chosen in the open: a script's threads die with its isolate, where plain CPython
would run them to completion. An embedder deciding an isolate's work is over is
the one that decides (`docs/python.md` section 6.4).

### 36. A promise is an `asyncio.Future`, and only `PumpJobs` runs the loop (`docs/python.md`)

Decision 23 needs one queue that only `PumpJobs` drains. CPython's queue of
continuations is an asyncio event loop, so each isolate gets one - a
`SelectorEventLoop`, set as its thread's current loop so that script finds it -
and **it is never run forever**. `PumpJobs` iterates it with a zero timeout until
nothing is ready, alternating with posted work exactly as on the other two, and
a stop in a pump discards what is ready. `Promise::New` is `create_future()`, a
task is a promise, a script with top-level `await` evaluates to one, and a
rejection with a value that is not an exception is carried in
`unibind.Thrown`.

The alternative - promises of the backend's own, with `then` - was rejected: every
Python library that does I/O speaks asyncio, and a promise type none of them can
`await` would have been a JavaScript island in a Python process.

### 37. A heap limit from allocator hooks, and the limit hook where the backend can keep it (`docs/python.md`)

CPython has no per-interpreter heap limit and no statistics worth the name.
`PyMem_SetAllocator` accepts hooks before the interpreter comes up, and a 16-byte
header on every block of the MEM and OBJ domains, naming the allocating thread's
isolate, is enough for both: a per-isolate byte count that follows each block to
whichever thread frees it, and a ceiling. An allocation that would cross
`heapLimitBytes` is refused - a `MemoryError` the script can catch - and
`EngineFault::OutOfMemory` is reported once per crossing.

That makes decision 28's `SetHeapLimitCallback` keepable here, so **it is
defined**: asked once per crossing, before the refusal, its answer adopted if it
raises the ceiling. Decision 19's rule is that an operation an engine cannot do
does not link; this one the *backend* can do, and so it links - SpiderMonkey
remains the only backend where it does not. What the hooks do not see is written
down rather than hidden: the interpreter's own start-up, the RAW domain, and
anything allocated on a thread that is not the isolate's.

## Smaller things worth knowing

- **`Frame` is placement-constructed into the caller's `HandleScope`.** V8
  deletes `HandleScope::operator new`, which hides the global placement new, so
  every in-place construction in that backend spells `::new`.
- **The storage sizes in `CMakeLists.txt`** (`UNIBIND_FRAME_STORAGE_SIZE` and
  friends) are checked by `static_assert` in each backend. If a backend type
  grows past one, raise the number - never fall back to the heap, because a
  frame on the heap is not a `JS::Rooted` on SpiderMonkey. Neither backend has
  needed a raise. Each is the maximum across *all* backends for the
  architecture, so raising one raises it for every backend; decision 26 says
  why that is the rule rather than a convenience.
- **`GetPropertyAttributes` asks whether the property exists first.** V8 reports
  `None` for a property that is absent, and `None` is also what an ordinary
  writable/enumerable/configurable property reports. The extra lookup is what
  lets an absent property answer empty instead of lying. Any backend should
  answer empty for an absent property.
- **An enumerator's empty answer means "no own keys", not "declined".** V8's
  enumerator has no decline path. `unibind/template.h` says so.
- **`ub::Constant("literal")` needs its own overload.** Without one it binds to
  `Constant(bool)`: pointer-to-bool is a standard conversion and beats the
  user-defined one to `string_view`, silently. `ReturnValue::Set` has the same
  guard for the same reason.
- **`String::NewFromUtf8` repairs bytes in the header, not in an engine.** V8's
  `NewFromUtf8` replaces each maximal invalid subsequence with one U+FFFD, the
  WHATWG rule; two engine decoders agree about valid UTF-8 and need not agree
  about how many replacements an overlong form or a cut-short sequence is
  worth. So the header validates first, copies only when something needs
  replacing, and hands the engine valid UTF-8 through the strict `String::New`
  - one scan and no copy for valid input, and the same string on every backend
  by construction. V8's backend checks `String::New`'s strictness with the same
  decoder, so the two cannot disagree about which bytes needed repair.
- **A function's data is an embedder pointer or a script value, never both.**
  `Function::New` with a `Local<T>` is V8's `Function::New` with a
  `Local<Value>` data, read back as `info.Data()`. The value must be reachable
  *through* the function and from no root of the backend's own - a strong root
  would keep a value that refers back to its function, and so the function,
  alive until the isolate goes. V8 carries it in an internal field of the
  function's data object, beside the callback; SpiderMonkey in the second
  reserved slot of the holder object its function already carries, because a
  `NewFunctionWithReserved` function has exactly two slots and both are taken.
  A `CallbackData` function pays nothing for it on V8, which has a trampoline
  for each kind, and one flag test on SpiderMonkey.
- **`HeapStatistics` has three figures every engine gives and six only V8
  does.** `usedBytes`, `totalBytes` and `limitBytes` are always filled; the
  physical, external, malloced and global-handle figures are `std::optional`
  and empty on SpiderMonkey, whose only source for them is a full memory report
  that walks the heap. Empty rather than zero, because zero would be a claim.
  SpiderMonkey's `totalBytes` is the chunks its collector has reserved for the
  heap, less the empty ones it keeps cached for reuse, so it is not a repeat of
  `usedBytes`; only `usedBytes` compares across engines, and
  only as a trend. The CPython backend fills `usedBytes`, `totalBytes` (the
  same figure: CPython will not say what it has reserved), `limitBytes` and the
  two malloced figures, which are exact there because every byte it counts came
  through its own allocator hooks.
- **`ReturnValue` and `Local`'s predicates are header-only.** V8's integer
  setters (`int16_t` to `uint64_t`, each the integer when it fits in an
  `int32_t` and a Number otherwise), `Set(const Global<T>&)`, `SetFalse`,
  `SetEmptyString`, and `IsUndefined` through `IsPromise` are all written over
  entry points every backend already had, so none of them has a capability row
  or a backend change.

## What the suite pins, and what it deliberately does not

Every decision above is held to by at least one assertion, on both backends.
That was not true twice before in this document's history, so it is worth saying
what changed rather than only that it did: the three that used to be listed here
as unpinned - a constructable template also being callable (12), one isolate per
thread (11), and a value crossing realms (4) - all have cases now, and the
decisions added since arrived with theirs.

Decisions 30-37 are held by `tests/python/` rather than by the shared suite, and
`tests/python/README.md` lists what each area pins. The coercions of decision 32
are the one part with no direct case yet.

What is *not* asserted is a separate list and a deliberate one: engine wording,
stack-trace layout, finalizer timing, identity seen from a foreign realm, and a
handful of others. `docs/testing.md` is its own account of those and is the file
to read before adding an assertion, because most of them are things a test
could assert on one backend and would then be wrong about the other.

**Adding an operation to the headers?** Add its row to
`tests/cmake/Capabilities.cmake` too, or the suite will never gate on it and an
unimplemented backend will fail to link instead of reporting a skip. The
decisions from 14 on, and the operations added beside them, need these rows,
and a backend that has not caught up will then report a skip rather than
failing to link:

```
"OWNERSHIP|ClassInstantiate,GetNativeBox"
"CALLABLE_CLASS|ClassSetConstructor,ClassInstantiate"
"GLOBAL_IDENTITY|GlobalStrictEquals,GlobalSameValue,GlobalStrictEqualsSlot,GlobalSameValueSlot"
"TERMINATION|TerminateExecution@Isolate,IsExecutionTerminating@Isolate,CancelTerminateExecution@Isolate,TryCatchHasTerminated"
"STACK_FRAMES|CaptureStack,TryCatchStackFrames"
"MESSAGE_LOCATION|TryCatchLocation"
"OBJECT_ACCESSORS|SetAccessorProperty"
"CODE_CACHE|CompileScriptWithCache,ScriptUsedCodeCache,ScriptCreateCodeCache"
"STACK_LIMIT|New@Isolate"
"BINARY_DATA|MakeArrayBuffer,ArrayBufferByteLength,ArrayBufferCopyOut,MakeTypedArray,TypedArrayElementType,TypedArrayLength,TypedArrayByteOffset,TypedArrayBuffer,TypedArrayCopyOut"
"SERIALIZATION|SerializeValue,DeserializeValue"
"PROMISES|MakePromise,ResolvePromise,RejectPromise,PromiseStateOf,PumpJobs@Isolate"
"JOBS|PostJob@Isolate,PumpJobs@Isolate"
"DELAYED_JOBS|PostDelayedJob@Isolate,PumpJobs@Isolate"
"INTERRUPTS|RequestInterrupt@Isolate"
"WORKER_THREADS|WorkerThreads@Platform"
"HEAP_LIMIT|SetHeapLimitCallback@Isolate"
"ARRAY_BUFFER_VIEWS|ArrayBufferViewByteLength,ArrayBufferViewByteOffset,ArrayBufferViewBuffer,ArrayBufferViewCopyOut,MakeDataView"
"FUNCTION_VALUE_DATA|MakeFunctionWithValue,CallbackValueData"
"EAGER_COMPILE|CompileScript,CompileScriptWithCache,ScriptCreateCodeCache"
"INSPECTOR|Supported@Inspector,New@Inspector,ContextCreated@Inspector,ContextDestroyed@Inspector,Connect@Inspector,Dispatcher@Inspector,RequestDispatch@InspectorDispatcher,DispatchProtocolMessage@InspectorSession,Resume@InspectorSession,Stop@InspectorSession"
```

`EAGER_COMPILE` names no new symbol, like `STACK_LIMIT`: the option rides on the
two compile entry points, and the row is there so the area is listed.
`String::NewFromUtf8` has no row at all, because it adds no entry point - it is
written in the header over `String::New`. `INSPECTOR` is present on every
backend by construction, since every backend defines all of it (decision 29); it
is a row so the area is listed, and its cases gate on `Supported()` inside.

Decision 28 is the clearest case that rule was written for, and it splits in
two: `HEAP_LIMIT` earns a row because one engine has the hook and the other has
nothing of the kind, so a SpiderMonkey build defines nothing and a call does not
link. The fault channel itself earns none - `PlatformOptions::onEngineFault` is
a field rather than an entry point, every backend honours it, and what differs
is which *kinds* arrive, which is written down in the header and in the table
above rather than gated.

`OWNERSHIP` and `CALLABLE_CLASS` name no new symbol, because neither decision
added one - shared ownership is entirely in the header templates, and
construct-without-`new` only widened `ClassSetConstructor`. They are there so
the areas are listed and skippable like every other.

Decisions 26 and 27 add no row either, and for the opposite reason: the two
symbols decision 26 brought - `Platform::BackendVersion` and
`detail::BackendBuildId` - are **not** optional. Every backend defines them,
including one with no code cache, because `Script::Compile` keys a script
whether a blob is ever asked for or not. An area only earns a row when a
backend is allowed to define nothing at all.
