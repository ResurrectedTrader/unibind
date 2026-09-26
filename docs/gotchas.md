# Gotchas

Everything in this file cost somebody a day, or would have. It is collected
here so that you can read it in ten minutes instead - before you lose the day,
not while you are losing it.

The ones worth fearing are not the crashes. A crash tells you where it happened.
These are the ones where **the code compiles, links, runs, and gives you the
wrong answer**: a `TypeError` that arrives as a `SyntaxError`, a cached blob
that runs a different script than the one you asked for, an allocator hook that
silently stops hooking, a promise continuation that simply never happens. Every
entry below says whether it is silent.

If you are here because something is already wrong, the index is the fastest
way in.

## The ones you will not diagnose from the symptom

Eighteen of these give a wrong answer and no error at all. The nineteenth gives
an error that blames something else entirely, which is the same problem wearing
a disguise.

| | |
|---|---|
| Every engine symbol undefined, as if the archive were missing | [A 32-bit `lld-link` loses the engine without saying so](#a-32-bit-lld-link-silently-fails-to-link-the-engine-archive) |
| Asking for a `TypeError` and getting a `SyntaxError` | [`js-config.h` is a record of the build, and it is wrong](#js-configh-does-not-describe-the-build-it-shipped-with) |
| A leak counter or failure injector that stopped working | [An engine header replaces your `operator new`](#an-engine-header-replaces-your-operator-new) |
| A cached compile that runs the wrong source | [A code-cache blob is keyed to the engine, not to your source](#a-code-cache-blob-says-which-engine-built-it-not-which-source-it-came-from) |
| `async` code that never continues | [Nothing drains promises but `PumpJobs`](#if-you-never-call-pumpjobs-promise-continuations-never-run) |
| A registry lookup that never finds the callback it was given | [Comparing two `Global`s compares roots, not values](#comparing-two-globals-as-handles-asks-the-wrong-question) |
| An interceptor setter whose result is ignored | [Four of the hooks discard their return value](#an-interceptor-setter-query-deleter-or-enumerator-discards-what-you-write-to-the-return-value) |
| `Constant("name")` arriving as `true` | [Pointer-to-bool beats every user-defined conversion](#constantliteral-is-constanttrue) |
| A read-only accessor that is not read-only | [`ReadOnly` is meaningless on an accessor](#propertyattributereadonly-is-ignored-on-an-accessor) |
| A `switch` on `Kind()` that never sees your typed array | [`ValueKind` answers what you can *do*, not what it *is*](#valuekind-collapses-date-regexp-proxy-and-typed-arrays-into-object) |
| A copy that copies nothing | [`CopyElements<T>` refuses a type mismatch by writing zero](#copyelementst-with-the-wrong-t-copies-nothing-and-says-0) |
| A stale handle that reads a plausible value | [`UNIBIND_HANDLE_CHECKS` is an ABI flag, not a debug flag](#unibind_handle_checks-is-an-abi-flag-and-it-changes-behaviour) |
| Every operation on an isolate doing nothing, over and over | [A stopped isolate stays stopped](#a-stopped-isolate-stays-stopped-and-does-nothing-quietly) |
| An empty list that a callback treats as false (CPython) | [`ToBoolean`, `ToString` and `LooseEquals` are Python's](#toboolean-tostring-and-looseequals-are-pythons) |
| A native's `SetNull()` that a script's `is None` misses (CPython) | [`None` is `undefined`, and `null` is something else](#none-is-undefined-and-null-is-something-else) |
| One realm's monkey-patch showing up in another (CPython) | [Realms of one isolate share their modules](#realms-of-one-isolate-share-their-modules) |
| A script's background thread that gets nothing done (CPython) | [A thread the script started dies with its isolate, and runs only while script does](#a-thread-the-script-started-dies-with-its-isolate-and-runs-only-while-script-does) |
| Standard-library modules from another version, on one machine only (CPython) | [A standard library directory on the machine replaces the embedded one](#a-standard-library-directory-on-the-machine-replaces-the-embedded-one) |
| A test case that passes without running | [A test case's name may not contain `;`](#a-test-cases-name-may-not-contain-) |

---

## Building and linking against an engine

### A 32-bit `lld-link` silently fails to link the engine archive

**Loud, and it blames the wrong thing entirely.** An x86 MSBuild project takes
the 32-bit host toolchain by default (`VC\Tools\Llvm\bin`, not
`...\Llvm\x64\bin`), and a 32-bit `lld-link` cannot get through an archive the
size of an engine - V8's monolith is 1.2 GB. It does not say so. It opens the
archive, loads no member from it, and reports *every* engine symbol as
undefined, exactly as though the library had not been passed at all.

So what a reader sees is hundreds of undefined V8 symbols, and what a reader
concludes is that the archive is missing, corrupt, or the wrong architecture.
Nothing in the output points at the linker's own bitness.

Set `<PreferredToolArchitecture>x64</PreferredToolArchitecture>` in the
project's `Globals` group, above the import of `Microsoft.Cpp.Default.props`.
`unibind.props` checks for this and stops the build with a sentence, but it cannot
fix it: by the time a property sheet is imported, the toolchain has been chosen.

CMake happens to pick the 64-bit tools already, which is why this went
unnoticed - and why it now lands on the most likely newcomer of all, the one who
cloned, had the engine fetched for them, and then could not link.

### An engine header replaces your `operator new`

**Silent, except for a warning nobody reads.** `mozilla/cxxalloc.h`, which
`jsapi.h` pulls in transitively, defines `operator new` and `operator delete` as
always-inline forwards to `moz_xmalloc`. So a `::operator new` written in *any*
translation unit that has seen a SpiderMonkey header is not the program's
replaceable `operator new`. A leak counter, an allocation-failure injector or an
arena stops working in that TU and nothing says so; the only outward sign is
`LNK4217` warnings about `moz_xmalloc` in the build log.

Put such code in a file that includes no engine header. This repository does
exactly that: `src/backends/spidermonkey/frame_alloc.cpp` exists solely to own
the two lines that call `::operator new` and `::operator delete`, and adding an
engine include to it would redirect frame allocation back to `moz_xmalloc` and
quietly un-test the frame-exhaustion rule.

### The x64 V8 monolith replaces your `operator new` too, and it is a link error

**Loud, and only on x64.** The published x64 V8 build carries PartitionAlloc's
Windows allocator shim - `allocator_shim_win_static.obj` - which defines
`malloc`, `free`, `_aligned_malloc` **and twelve of the twenty allocation
operators**: nothrow `new`, aligned `new`, sized `delete`, aligned `delete`. Any
link mentions `malloc`, so that object is always pulled in, so those definitions
are always present. A program that replaces them itself gets LNK2005 on all
twelve and does not build.

It is the same trap as SpiderMonkey's above with the polarity reversed: that one
is silent and this one is loud, and neither leaves you owning your allocator. The
x86 monolith has no shim at all - PartitionAlloc is not used as malloc on 32-bit
Windows - which is why an x86 build of the same tree links and an x64 build does
not.

There is no way to take only part of that object, so a program that must own
those operators links with `/FORCE:MULTIPLE` and relies on the order: an object
named on the command line is seen before an archive scanned after it, so its
definitions win and the shim's are dropped. Memory stays consistent, because the
operators still call the shim's `malloc`. This tree's test suite does exactly
that, only in the configuration that needs it, and only after probing the
archive's symbol index to find out - `tests/cmake/EngineAllocator.cmake`.

### An `operator new` that throws is survivable only where unibind allocates

**Silent on a release V8, an abort on a debug one.** Replacing `operator new`
with one that throws `std::bad_alloc` - the standard one does, when memory runs
out - is supported where the *library* allocates: its own allocations are
caught and answered with the empty result the header documents, and nothing is
handed over twice or not at all. It is not supported where the *engine*
allocates through the same operator. V8 calls `operator new` from inside itself
(a template instantiation's cache, a global handle's young-object list) and is
compiled without exceptions, so a throw from there unwinds through frames that
clean nothing up: a handle scope left open, a cache half-written, a root the
collector no longer tracks. A debug V8 notices the first of those and aborts; a
release V8 goes on with it, and what goes wrong later is somewhere else. Where
the library's own entry point is `noexcept`, such a throw ends at
`std::terminate`, deliberately, rather than being caught and carried on from.

Nothing an embedder or a backend can do makes the engine's allocations
recoverable. An embedder that injects allocation failures to test itself - as
this repository's suite does - aims them at the library's allocations and keeps
them out of the engine's; `docs/testing.md` has how the suite does that.

### `js-config.h` does not describe the build it shipped with

**Silent, and the most expensive one here.** `js-config.h` is the header
bundle's own record of what the library was compiled with, and the bundle used
here omits `ENABLE_EXPLICIT_RESOURCE_MANAGEMENT` although the library was
plainly built with it. That macro gates `JSEXN_SUPPRESSEDERR` in the **middle**
of `JSExnType`, and entries in `JSProtoKey` and `JS::SymbolCode`, so every
enumerator after it means one thing inside the library and another in your
translation unit.

It compiles. It links. It runs. And then asking for a `TypeError` hands back a
`SyntaxError`, and asking for a `SyntaxError` hands back a `SuppressedError`.
Three test cases found it, off by exactly one in exactly the predicted
direction, which is what made the diagnosis certain rather than plausible.

If an enum-valued answer from an engine is off by one position, suspect a build
flag missing from its config header before you suspect your own code. Assume
`js-config.h` is incomplete rather than authoritative, and define the missing
macro yourself to match how the library was actually built.

### The engine's headers lay out different objects without its own defines

**Silent, and it is an ABI mismatch rather than a compile error.** SpiderMonkey
needs `STATIC_JS_API`, `MOZ_STATIC_JS` and `XP_WIN`; V8 needs `V8_GN_HEADER`,
which makes `v8config.h` pull in the bundled `v8-gn.h` carrying the exact define
set the monolith was built with. Without them the public headers fall back to
their defaults and lay objects out differently from the library, and that
"mostly does not show up as a compile error".

Both backends set these in their `CMakeLists.txt` with a comment saying why.
Do not tidy them away.

### The toolset is a floor, and it is a link error a long way from its cause

**Loud, eventually.** Both prebuilt engines were compiled against the MSVC 14.44
STL and call helpers that ship only in that toolset's own `libcpmt.lib`. An
older toolset fails at link with undefined `__std_*` symbols, which names
nothing useful. `CMakeLists.txt` refuses at configure time instead, with the
reason.

The other half of the same rule is the CRT, and it fails at *link* rather than
at compile, which is a long way from the flag that caused it. The engines link
the static CRT, so a `/MD` consumer gets LNK2038 - the MSVC STL stamps
`RuntimeLibrary` into every object with the same `#pragma detect_mismatch`
mechanism `unibind/config.h` uses for its own ABI tag, so at least the error names
the CRT. A Debug build of this tree wants `/MTd` and therefore a *debug* engine
tree; that is the same LNK2038 for the same reason.

### The public headers need clang-cl

**Loud.** MSVC's own `cl.exe` does not accept them today: at `/std:c++latest` it
instantiates `std::optional<Local<T>>` while `Local<T>` is still being defined
and reports C7637. clang-cl accepts them, and clang-cl is what the whole tree is
built with. The CI job pins the ClangCL toolset for this reason rather than
hiding it.

### mozglue defines `DllMain`

**Loud, and only if you are building a DLL.** `WindowsDllMain.obj` inside
mozglue defines `DllMain`, so a host DLL that defines its own fails to link with
a duplicate symbol. Irrelevant for a static library or an executable. If it
comes up: either let mozglue's run, or exclude that object.

### `UNIBIND_HANDLE_CHECKS` is an ABI flag, and it changes behaviour

**Silent in both directions.** It puts a frame epoch in every `Local<T>`, which
changes `sizeof(Local<T>)` from 8 to 12 on x86 - on x64 it changes nothing, the
epoch fits in padding - *and* decides whether using a handle after its scope
closed is diagnosed or silently reads a stale slot. A frame's storage is the
caller's stack, so a dead frame's address is routinely reused by a live one -
without the epoch, a stale handle reads a plausible value belonging to
something else. **The flag being free on x64 does not make it the same ABI**:
an object compiled with it and one compiled without it still disagree about
whether the epoch is checked, and the tag below is what keeps them apart.

It is on by default in *every* configuration, and that is the half people get
backwards in the other direction: the comparison is an assertion, so a Release
build carries the epoch and never tests it. Diagnosing a stale handle needs the
flag **and** a build with assertions live. `docs/testing.md` says which CTest
test that is and why it is expected to die.

This is why it lives in a **generated** `unibind/config.h` rather than in a
per-target `-D`: every translation unit of a build tree must agree, and so must
every consumer of an installed library. The same goes for the inline storage
sizes, which `TryCatch` and `ContextScope` `reinterpret_cast` their raw byte
arrays to - a mismatch there is memory corruption, not a link error. The
generated header carries a `#pragma detect_mismatch` so that disagreeing objects
fail to link (LNK2038) rather than agreeing to differ.

---

## Handles, scopes, and what "empty" means

### An empty handle is not `undefined`

**By construction, not silent - and that is the point.** Growing a frame is
fallible: SpiderMonkey's `RootedVector::append` reports OOM and nothing portable
makes it infallible. A slot-allocating operation that cannot grow the frame
therefore yields an **empty** handle and reports the condition the way the
engine reports allocation failure. It must never yield a slot that reads as
`undefined`, because then running out of memory would be indistinguishable from
a property that was not there.

Reading an empty handle is a programming error: diagnosed in checked builds, a
hard failure otherwise. If you care about surviving OOM, `IsEmpty()` once after
a batch of allocations.

### An empty `std::optional` never means "the value was `undefined`"

**Silent if you conflate them.** `undefined` is a value and arrives as one. An
empty optional means *the operation did not produce a value*, and there are
three different reasons: a script threw (exception pending, `TryCatch::HasCaught`),
the engine failed (`Isolate::HasPendingException`, and there may be no
JavaScript exception at all), or execution was terminated from another thread
(`TryCatch::HasTerminated`, and there is no value, no message and no stack).
Handling the wrong one means either calling back into an engine with an
exception pending or looping forever on a terminated isolate.

Note the two shapes side by side: handles report failure by being empty,
everything else by an empty optional. So `if (maybe)` on an
`std::optional<Local<T>>` is true even when the `Local` inside it is empty.

### Scopes are stack-only and close in reverse order

**Silent in a release build, which is the worst of both.** A `HandleScope` owns
its frame in place, because on SpiderMonkey the frame *is* a `JS::Rooted` and
`JS::Rooted` is stack-only and strictly LIFO. `~Rooted` asserts the ordering in
a debug engine build and **corrupts the root list** in a release one.
`operator new` is deleted on the scope type, so it cannot be heap-allocated, but
nothing stops you from destroying two scopes out of order.

The API's rule and the engine's precondition are the same rule, restated. A
checked build of `unibind` asserts it before the engine gets a chance to.

### To return a handle you must escape it, and escapability is decided when the scope opens

**Silent at the point of the mistake.** A bare `return local;` out of a function
that opened its own `HandleScope` returns a dangling handle, and there is no way
to make that a compile error; it is caught at the point of *use*, in a checked
build, and not at all in a release one.

`Escape` is a member of `EscapableHandleScope` rather than a free function for a
reason that is not stylistic: it copies the value into the parent frame while
both frames are still live roots, so the value is never momentarily unrooted.
And escapability has to be chosen when the scope is opened because V8 must
reserve the parent's slot up front. On V8 the first escape from a scope is free
and each later one costs a global root; on SpiderMonkey escaping is a `JS::Value`
copy and is free and unbounded. That is a cost difference, not a rule you have
to remember.

### Comparing two `Global`s as handles asks the wrong question

**Silent.** `Duplicate()`, or rooting the same function twice, makes two roots
over one object. Comparing the roots answers "are these the same root", and the
canonical way to get bitten is a registry of callbacks kept as `Global`s: the
caller hands back the same function through a different handle, the removal
compares roots, finds nothing, and removes nothing.

Use `StrictEquals` or `SameValue` (both spellings exist because they differ on
`NaN` and `-0`; there is deliberately no `operator==`). They need no open
`HandleScope`, on purpose - so a caller with no scope open gets an answer rather
than a failure that would read as "not equal". An empty `Global` is equal to
nothing at all, **including another empty one**, which reads like "two different
objects" if you were using it as a null check; ask `IsEmpty()`.

### A handle belongs to one isolate and one thread

And nothing releases handles individually: both engines are traced, there is no
per-value release call, and a frame gives back its slots wholesale when it
closes. If a value must outlive every frame, move it into a `Global<T>`; nothing
else does.

---

## Values and properties

### `Constant("literal")` is `Constant(true)`

**Silent, and no warning.** Pointer-to-bool is a standard conversion and beats
the user-defined one to `std::string_view`. `ub::Constant` and
`ReturnValue::Set` each carry an explicit `const char*` overload for exactly
this reason. Any new sink you add that takes both a `bool` and a `string_view`
needs the same guard, and will not warn you that it does not have one.

### `ValueKind` collapses Date, RegExp, Proxy and typed arrays into `Object`

**Silent if you `switch` on it.** `ValueKind` answers what you can *do* with a
value, and `Object` is exactly the set of property operations offered for all of
them. There is no exotic kind, deliberately: the list differs by engine and by
version, and an engine with no interceptor of its own builds ours out of
proxies, so a kind that singled proxies out would make a sandbox object report
differently on two backends purely because of how it was built.

Ask `Is<ArrayBuffer>()` or `To<TypedArray>()` - or `IsArrayBuffer()`,
`IsTypedArray()`, `IsDataView()` - which answer without a kind of their own. `ValueKind::Other` is not the exotic-object answer - it means the
backend could not classify the value at all, which today happens for no value on
either engine.

### `Integer` is a question about the value, not about the type

`Is<Integer>()` asks whether this number is an exact int32 with `-0` excluded,
not how the engine happens to be storing it. So it is true for `1.0` and false
for `1.5`, and the same JavaScript expression can narrow or fail to narrow
depending on what it evaluated to. That is deliberate: SpiderMonkey has one
number type, and asking V8's storage question would have given two backends two
answers. `IsInt32()` is the same question, and `IsUint32()` its unsigned twin,
which also says no to `-0`.

### An `External` is not an `Object`, though V8 says it is

`IsObject()`, `Is<Object>()` and `To<Object>()` all say no to an `External`, on
both backends: it is a value with no properties, and `Object` promises property
operations. Code ported from V8 may expect the opposite, because V8 15.6's own
`External::IsObject()` is true. Ask `IsExternal()` first where the difference
matters.

### `Object::Set` is not `DefineOwnProperty`

**Silent.** `Set` performs an ordinary assignment, so a setter anywhere on the
prototype chain runs and your property may never be installed.
`DefineOwnProperty` installs a data property with explicit attributes and
ignores setters. If you mean "put this property here", say the second one.

### `PropertyAttribute::ReadOnly` is ignored on an accessor

**Silent.** An accessor is an ECMAScript accessor property, so it has no
`[[Writable]]`: a getter with no setter *is* the read-only form. Both backends
drop the flag. Asking for it and getting a writable accessor is not a bug in the
backend.

Accessors are installed as real accessor properties rather than native data
properties for a reason worth knowing: a `Class<T>::Accessor` lives on the
*prototype*, and V8's `PropertyCallbackInfo` reports the holder, which under a
native data property would be the prototype - carrying no native at all. A
function call has a receiver; that is what buys the accessor one.

### `GetPropertyAttributes` has to ask whether the property exists first

V8 reports `None` for an absent property, and `None` is also what an ordinary
writable/enumerable/configurable property reports. The extra lookup is what lets
an absent property answer empty instead of lying. A backend that skips it is
wrong in a way no caller can detect.

### Recovering embedder data is exact-type, and a mismatch is null

**Silent.** `External::As<D>()`, `info.Data<D>()`, `ExternalValue<D>` and
`Isolate::GetEmbedderData<D>` all yield null unless `D` is exactly the type that
was stored - cv-qualifiers aside. A base class, a derived class or a typedef of
a different type gets you null, which is also what "nothing was stored" looks
like. And the pointee is yours: it must outlive every callback that can see it.

`info.Data()` with no type is a different thing altogether: the *script value*
a function was made with (`Function::New` with a `Local<T>`), which is
`undefined` for every callback made any other way. A function has one kind of
data or the other, so in a function made with a value `info.Data<D>()` is null,
and in one made with a pointer `info.Data()` is `undefined`.

---

## Callbacks, interceptors and accessors

### An interceptor setter, query, deleter or enumerator discards what you write to the return value

**Silent, and maximally so.** One `CallbackState` serves every shape of call, so
every hook has the `SetReturn*` entry points whether or not the language reads
its result. A hook whose answer is its **C++ return value** - a setter, a query,
a deleter, an enumerator - discards anything written through
`GetReturnValue()`. Writing there is legal and does nothing.

The answer these hooks give is the returned `Intercepted`,
`std::optional<PropertyAttribute>`, `std::optional<bool>` or
`std::optional<Local<Array>>`. Script still sees the ordinary thing: an
intercepted assignment evaluates to the assigned value.

### `Intercepted` has three states and the third is the whole point

**Silent if you collapse it.** `Intercepted::No` means "carry on with the
ordinary lookup", which is *not* the same as "handled, and the answer is
`undefined`". An engine with no interceptor of its own implements these hooks as
proxy traps, which are the only construct that runs on every access *and* can
hand the access back to the ordinary object. A resolve-style hook cannot: it
fires only on a miss, what it defines sticks, and it has no setter.

### An enumerator's empty answer means "no own keys", not "I decline"

V8's enumerator has no decline path, so an engine that has one may read an empty
answer as declining. Do not depend on the difference.

### `This()` and `Holder()` may be the same object, and which you get is the engine's business

**Silent.** `Holder()` is the object carrying the handler that ran; `This()` is
the receiver of the access. Where an engine hands a property hook only one of
the pair - V8 15.6 gives an interceptor the holder and nothing else - the two
name the same object. Code that unwraps one expecting the other gets the other
one, with no diagnostic, on one backend only.

For an inherited accessor the holder is the prototype. `Class<T>`'s own
trampolines unwrap `This()` and throw a `TypeError` if it is not a `T`, so a
class method borrowed through `.call()` fails loudly, which is the safe half.

### Use the context the callback was handed, not one you captured

**Silent.** `info.GetContext()` is the realm the call is actually happening in.
Making values in, or reading properties through, a context captured at
declaration time puts them in the wrong realm.

### Reading past the end of the argument list is `undefined`, not an error

As in JavaScript. If "not passed" has to differ from "passed `undefined`", ask
`Length()`.

### `Object::SetAccessor` keeps its callbacks for the life of the isolate

**Silent, and it grows.** An accessor on one object records its getter, setter
and data the way a template's accessor does - in the isolate, until the isolate
goes - because the property's functions may be called for as long as anything
can reach the object, and nothing tells the library when that stops. Once per
realm or per long-lived object is what it is for. Calling it on an object made
for every call adds a record per call that is never given back; use an
`ObjectTemplate` with the accessor on it instead, which records it once.

### A template's class name, parent and handlers are fixed when it is first used

**Silent.** `SetClassName`, `Inherit` and `SetHandler` - on a template or on a
`Class<T>` - are ignored once the template has been instantiated: after
`GetFunction`, `NewInstance`, `GetConstructor` or `Wrap`, or after a template
that instantiates it along with itself was (see `unibind/template.h` for the
whole list). V8 bakes them into the constructor it builds at that moment and
ends the process if they change afterwards, so the library stops the call
rather than passing it on, and does the same on SpiderMonkey. Constants,
methods and accessors may still be added later, and reach the realms the
template is instantiated in from then on.

### Throwing does not stop your C++

**Silent if ignored.** `Throw` marks an exception pending; it takes effect when
control returns to the engine. The caller should return promptly and not call
further into the engine - on SpiderMonkey, calling back in with an exception
pending is a crash, not an error (stringifying a pending exception with
`JS::ToString` before taking it off the context is the measured example).

Related: `ReturnValue::Set(std::string_view)` can fail, and then nothing was
set, and the callback returns `undefined`. It is `[[nodiscard]]` for that
reason.

### An engine-fault handler may not allocate, and the commonest fault is that allocation is failing

**Silent until it is not, and then it is a crash inside your own reporting
code.** `PlatformOptions::onEngineFault` is called at the moment the engine has
decided it is in trouble, and the kind that arrives most often is
`EngineFault::OutOfMemory`. A handler that builds a `std::string`, formats into
a stream, pushes onto a vector or opens a file is allocating in order to say
that allocation failed. Reserve the buffer, open the log, and size the strings
before you need them; the report itself is borrowed views and costs nothing to
deliver.

The rest of the contract is `RequestInterrupt`'s, minus handles: embedder state
and `TerminateExecution` yes, a handle or a value or a call into the engine no.
It may also be called **on any thread**, including one that never had an
isolate, so `EngineFaultReport::isolate` can be null and whatever the handler
touches has to be safe for that.

### `EngineFault` is not `ErrorKind`, and they mean nearly opposite things

`ErrorKind` names a JavaScript `Error` constructor - what native code *throws*,
a value, catchable by script, part of the program working. `EngineFault` is the
engine reporting that *it* is failing: not a value, not catchable, and not
something a script did. They are spelled with no word in common on purpose.

The one place they touch is the one to be careful about: a script's own `throw`
must never arrive on the fault channel, and does not.

---

## Realms

### Enter a realm before reading through an object that belongs to it

**Loud, but it looks like your hook never ran.** A value made in one realm is a
value in another - that is guaranteed, and it is what makes a sandbox useful at
all. But a realm's **global object** is access-checked: reading a property of
one while a different realm is current fails, on V8 with `TypeError: no access`,
*before any embedder code runs*.

So the obvious sandbox - hold the inner realm's global and read properties of it
from an interceptor - throws on V8 and works on SpiderMonkey. The portable
answer is one line per hook: open a `ContextScope` on the realm that owns the
object first. It costs nothing on the engine that did not need it.

### Identity across a realm boundary is not guaranteed

**Silent.** Where an engine wraps a value into a compartment, the wrapper is a
distinct object, so "is this the same object" asked across a realm boundary can
answer differently on two backends. Compare within one realm, or compare
something the values carry. A portable program does not ask.

There is deliberately no way to say "these two realms trust each other": V8
spells that as a shared security token, SpiderMonkey has no token at all, and
the two do not describe the same thing closely enough to promise one.

### `instanceof` does not cross a realm, and for a sandbox that is the normal case

An error thrown inside a sandbox was built by the inner realm, so its
constructor is the inner realm's `SyntaxError` and `e instanceof SyntaxError` in
the outer realm is false. Ask `e.constructor.name`.

### A compiled script sees the globals of the realm it runs in

Not the realm it was compiled in. That is what makes "compile once, run in every
sandbox" work, and the alternative would make `Run`'s context parameter a lie -
but if you assumed a script stayed bound to where it was compiled, it does not.

---

## Exceptions

### Closing a `TryCatch` consumes what it caught

**Silent if you expected the other rule.** A handler is a `catch` block, not an
observer. Call `ReThrow` to send the exception on to the enclosing handler.

The opposite rule - propagate unless explicitly consumed - was rejected because
its failure is worse: a forgotten call leaks a pending exception into code that
never went near the throw, and it surfaces somewhere else entirely. A swallow at
least happens where the handler is.

`Reset()` consumes now, so the rest of the scope can call into the engine again;
after it there is nothing left for `ReThrow` to send on, so `Reset` then
`ReThrow` is a silent no-op.

### A termination is the one thing a handler does not consume

`HasCaught()` is true, `HasTerminated()` is true, and closing the handler lets
the unwind continue whether or not `ReThrow` was called. There is no exception
value, no message and no stack; `Reset` and `ReThrow` mean nothing. Only
`CancelTerminateExecution`, on the isolate's own thread, clears it.

### A stack comes off the Error object, never off the engine's Message

**Silent, and it took a round trip to find.** V8 will happily capture a trace
for `throw 1` if you ask it that way - and a stack invented for a value that
never carried one is exactly the plausible wrong answer this API exists to
prevent. A thrown non-Error answers empty. That is the right answer, not a
missing feature.

### The stack *text* is for a human; a short stack is not evidence

`TryCatch::StackTrace` returns the engine's own format, which differs between
engines and is not parseable across them - it will parse on one backend and
mis-parse on the other. To read a stack, ask `StackFrames`.

And engines cap captured frames at their own configured depth (V8 at
`Error.stackTraceLimit`), so a short trace is not evidence that there were no
more frames. In a `StackFrame`, an empty function name means top-level or
anonymous, an empty script name means compiled with no origin, and a line or
column of 0 means the engine did not say - lines and columns are 1-based.

### Error wording is the engine's

Both engines put your text in the message and wrap it in their own decoration.
Assert that your text survived, never the whole string; and where the *kind*
matters, ask in script (`e instanceof TypeError`, `err.constructor.name`) rather
than parsing a message.

---

## Stopping a script, interrupts, and the job queue

### A blocking native cannot be interrupted, and a running script cannot be made to yield

**Silent: the watchdog appears to do nothing.** Every checkpoint either engine
has is inside JavaScript. A native that spins, blocks on a socket or waits on a
lock is not one: it runs to completion, and the unwind happens at the next
JavaScript checkpoint after it returns. Neither `TerminateExecution` nor
`RequestInterrupt` reaches your own C++.

This is the bound worth knowing *before* you build a timeout rather than after:
**a running script cannot be made to give the thread back, only terminated**,
and there is no yield, suspend or resume anywhere in this API. Keep scripts
short and pump between them, or terminate and restart. A native that wants to
cooperate polls `IsExecutionTerminating()` itself.

### A stopped isolate stays stopped, and does nothing quietly

**Silent.** Everything that would run script keeps failing until
`CancelTerminateExecution`, so an operation whose result is checked but whose
failure is ignored will silently do nothing, over and over.

That rule is the library's rather than either engine's. SpiderMonkey's context
is usable the instant the unwind finishes, with nothing to reset; V8 keeps its
termination pending. Taking the permissive answer would let a stop requested
from another thread race the very next `Script::Run` and sometimes lose,
silently, on one backend only. So `IsExecutionTerminating()` here answers a flag
the library owns - which also means that reaching around this API to the engine
while a stop is pending gets you an engine that will happily run script.

One more edge: cancel only after every native frame the termination unwound has
returned. Cancelling from inside one resumes a script that was told to stop.

### An interrupt callback may not run script, even where the engine allows it

**Silent, and the nastiest entry on this page.** A staged probe inside a live
SpiderMonkey interrupt, fired from another thread into a `while (true)` loop,
got all the way through: made an object, set a property, called a JavaScript
function that really ran, compiled and ran a fresh script, captured a stack -
after which the loop terminated normally and the isolate was fine. V8 refuses
the same thing outright.

The stricter rule stands, and not for symmetry. An interrupt fires *between two
bytecodes of whatever was running*, so script run there runs at an arbitrary
point inside unrelated code, and anything it leaves pending - an exception above
all - is left for the interrupted frame to trip over. That probe survived only
because it cleared its own exceptions at every stage; a callback that threw and
returned normally would hand the interrupted code an exception it never threw.

Allowed: make and read handles, inspect values, read and write embedder state,
set a flag, queue a job. Not allowed: call a JavaScript function, run a script,
throw.

### An interrupt never fires while the thread is idle

**Silent: it just never runs.** There is no checkpoint between scripts, so an
interrupt requested while nothing is running waits for the next script.

### An interrupt cannot make posted work run sooner

**Silent, and it had to be traced rather than assumed.** If the script thread is
inside a long-running script, the interrupt fires, the callback cannot run the
work and cannot make the script yield, so the script resumes, runs to
completion, and the thread pumps afterwards - which is what would have happened
with no interrupt at all. If the thread is idle, no script is running, no
checkpoint is reached, and the interrupt never fires. Neither state moves
forward.

### If you never call `PumpJobs`, promise continuations never run

**Silent in the worst way.** Script containing `async`/`await` compiles and
runs, and its continuations never execute: no error, no exception, the work
simply does not happen. `Isolate::PumpJobs` is the one drain, for engine jobs
and for posted work alike.

The engines' own automatic draining is deliberately turned **off**: V8's default
policy drains when a call returns and SpiderMonkey's never does, so left alone
the same script would run its continuations after every `Script::Run` on one
backend and never on the other. When a continuation runs is something a script
can observe, so it is made uniform rather than left as a hint.

Call it with no native frame on the stack. It catches and **discards** anything
a job throws - a pump is not a call and has nowhere to put an exception - and it
does nothing at all while a termination is pending, though the queues survive
that. A stop that lands *during* a pump is different, and silent too: the posted
work behind it waits for the cancel, but the promise continuations queued behind
a stopped one are **discarded** - V8 empties its queue then and nothing can keep
it, so both backends do.

### Posting does not wake anything, and what is queued at teardown is dropped

**Silent.** `PostJob` queues work from any thread; the work runs when the script
thread next pumps, and nothing accelerates that short of terminating what is
running. Jobs are ordered and never coalesced, so posting the same callback
twice runs it twice.

Whatever is still queued when the isolate is destroyed is **dropped, not run** -
said out loud because "it will run eventually" is what a caller would otherwise
assume. Unfired interrupts go the same way.

A posted callback is not inside a call: no realm is current and no handle scope
is open. Open your own `HandleScope` and `ContextScope`, let nothing escape, and
wrap engine calls in a `TryCatch`.

`PostDelayedJob` is not a timer. Its delay is a floor: once it has passed the
job is ordinary posted work, and nothing wakes the thread to run it, so a job due
in ten milliseconds on a thread that pumps once a second runs up to a second
late. The same goes for `InspectorDispatcher::RequestDispatch` on an idle
isolate - it runs at the next pump - and a request still waiting when the
`Inspector` goes is dropped with it.

### A stack quota larger than the thread's real stack is not a limit

**Fatal, and it is the failure the knob exists to prevent.** `stackLimitBytes`
turns runaway recursion into an exception a script can catch - but only if the
figure is below the stack the thread actually has. Set it above and the engine
recurses happily past it until the *thread* runs out, which no engine can turn
into an exception. The suite measures at 128 KiB and 512 KiB on a 1 MiB thread.

*Which* error you get is the engine's business - `RangeError` on one,
`InternalError` on the other - so a portable script catches it rather than
asking what it is.

### One isolate per thread, and the second one comes back empty

**Loud if you check, a crash if you do not.** SpiderMonkey keeps the running
`JSContext` in a single thread-local slot: *"there must be exactly one JSContext
for each thread running JS/Wasm"*. A debug engine build asserts on the second
call; the release build walks into undefined behaviour and dies.

So `Isolate::New` **returns empty** rather than throwing, which is what it
already did for a heap that could not be made. V8 would allow a second one and
refuses anyway, because a program that works on one backend and not the other is
the failure this library exists to prevent. Two heaps at once is two threads.

### `workerThreads` is a hint at every value, including zero

**Silent on one backend, by choice.** SpiderMonkey builds its helper pool in
`JS_Init` for the whole process, ignores `JSGC_MAX_HELPER_THREADS = 0` outright,
and the one hook that replaces the pool accepts your callback and then segfaults
on the first collection - it fails late and it fails looking like it worked.

Of "ignore silently", "crash later" and "refuse", only the first survives, so
the option is a hint - and `Platform::WorkerThreads()` reports the count actually
in effect so that the hint is observable. **An empty answer is not zero**: it
means the engine would not say, so assume it has threads.

`engineFlags` is on `Platform` and not on an isolate for a related reason: a
flag string set per isolate would silently apply to every isolate made before
it.

---

## Ownership and finalizers

### A native may outlive its isolate, and that is not a leak

There are two invariants, not one, and conflating them forbids the case shared
ownership exists for:

1. **Every box** - the per-wrapper cell holding the share - is destroyed exactly
   once, and all of them by the time the isolate is gone. V8 does not promise to
   run a weak callback before an isolate goes away, so the backend keeps a
   record of every live instance and destroys the survivors itself.
2. **Every native** is destroyed exactly once, when its last share goes, which
   may be **after** the isolate, because the last share may be the embedder's.

The sentence to hold a backend to is "the engine gives back every share it took,
exactly once, by the time its isolate is gone".

### A box must be given back on the isolate's own thread

**Silent, and it does not reproduce.** Dropping a box drops a `std::shared_ptr`
whose other holders are the embedder's, so running it on a background collector
or helper thread races them - and a race in a finalizer is exactly the failure
that a test counting destructions will never see. Where an engine offers the
choice, SpiderMonkey's `JSCLASS_FOREGROUND_FINALIZE`, the foreground one is
**required**, and the code says so in a comment because nothing else about it
would.

This looks like tidiness right up until somebody removes it.

### A finalizer cannot call back into the engine

By design: `JSFinalizeOp` is handed a `JS::GCContext*`, not a `JSContext*`. A
native whose destructor touches JavaScript was always forbidden; shared
ownership does not widen that, it only makes it likelier that somebody tries,
because the destructor may now run at a moment nobody chose.

### `Unwrap` hands back a borrowed pointer

**Silent.** The `T*` from `Unwrap` is valid only as long as the wrapper you
unwrapped. To keep it longer, take a share with `UnwrapShared` - one atomic
increment. And unwrapping is exact-type: a related class or a derived native
unwraps to null, which is also what "not an instance" looks like.

---

## Compiled code and serialized values

### A code-cache blob says which engine built it, not which source it came from

**Silent, and it cost two bugs in one afternoon.** Both engines stamp a blob
with an engine-build identity and refuse one from another build, and that check
is easy to mistake for the whole of it. Neither checks that the blob came from
the source you are offering it for: a SpiderMonkey stencil encoded from `"a"`
decodes cleanly when offered for `"b"`, reports success, and **runs `a`**. You
asked to run one script and got another, with nothing in the result to suggest
anything happened.

It is one safety property at three scales: *these bytes came from this engine*
(the engines check it), *these bytes came from this source* (neither does), and
*these bytes are a blob at all* (neither reliably does). `unibind/script.h` frames
every blob it emits - magic and format, a hash of the source with its origin and
the backend name, the payload's length and hash - and checks it before the
engine sees anything.

**A backend author porting this to a third engine must assume the keying is
theirs** until a test proves otherwise, and the test to write is the one that
encodes from one source, offers the blob for another, and then checks *what
actually ran* rather than what the API reported.

### `UsedCodeCache()` answers about your blob, and an engine will lie by accident

**Silent.** An engine may answer a repeat compile out of its own in-isolate
compilation cache without parsing and without looking at your blob at all - it
is then in no position to say whether the blob was good, which is how
`UsedCodeCache()` came to report true for a payload that had been bit-flipped or
cut in half. V8 only checksums a blob on consume when
`--verify-snapshot-checksum` is on, which it is not in a release build. The
payload length and hash settle it without asking the engine anything.

The other direction is true too: `UsedCodeCache()` can be false for a compile
that was instant anyway, because that is a different question.

### A blob made after an ordinary compile covers only what had run

**Silent, and it looks like a working cache.** Both engines compile a function's
body on its first call, so a blob made straight after `Script::Compile` holds
the top level and nothing inside a function. Made after a run, V8's holds what
that run happened to call and SpiderMonkey's still holds only the compile as it
was - measured, not assumed, by the suite's eager-compile cases. The next
start consumes it, `UsedCodeCache()` says true, and every function still
compiles on first use. Compile with
`CompileOptions::EagerCompile` when the point is the blob, and pass it to
`CompileWithCache` as well, so that a stale blob is replaced by an eager compile
rather than a lazy one. V8 would otherwise answer an eager compile of source it
had already compiled lazily in the same isolate with the lazy result; its
backend files the two apart, so you do not have to avoid that.

### `JS::EncodeStencil` dereferences a null function pointer

**A crash, not a failure code**, if `JS::SetProcessBuildIdOp` was never called.
It does not return `Failure_BadBuildId` and it does not throw. That is
process-wide state, so the backend installs a build-id op during `Platform`
construction, before an embedder can reach a cache API at all.

### An absent operation is a link error at your call site, on purpose

**Loud, and it is the design.** Caching is three separate entry points rather
than a parameter on `Compile` precisely so that a backend whose engine has no
code cache can define none of them: calling one is then a link error where you
called it. A parameter a backend quietly ignored would have been the silent
answer, and there is no way to ask a linker about half a function. Backends are
told, in the header: do not define a stub.

### A serialized value is opaque bytes belonging to one engine build

`Serialize`/`Deserialize` move a value between isolates of one engine. A blob is
not an interchange format and not a storage format; keeping one past the process
is outside what it promises, and an engine upgrade is where that goes wrong.
`Deserialize` refuses a blob it did not write rather than misreading it.

A value that will not clone fails the **whole** call rather than becoming
`undefined` somewhere inside the result. Neither engine can substitute below the
top level without reimplementing the algorithm, and a rule that held for a
top-level function but not for one two properties down would be worse than no
rule. Serialize per value if you need per-value decisions.

---

## Strings and binary data

### `Utf8Length()` excludes the terminator and `WriteUtf8` does not write one

**Silent.** Size a buffer by `Utf8Length()`, treat it as a C string, and you read
past the end. `WriteUtf8` truncates at a code point boundary if it does not fit
and returns how many bytes it wrote - a short buffer gives you a valid,
truncated string rather than an error, so compare the return against
`Utf8Length()`.

The same trap exists the other way round in the engine's own API:
`JS_EncodeStringToUTF8` hands back a NUL-terminated buffer, which silently
truncates any JavaScript string containing a NUL. Everything that reads a string
out of the engine here takes the deflated length first and copies exactly that
many bytes.

### Strings are copied, and `String::New` can come back empty for two reasons

There is no borrowing a string's bytes on a moving collector, so `Utf8Value()`
allocates and `String::New` copies out of the view you hand it. An empty result
means the bytes were not valid UTF-8 **or** the engine could not allocate, and
the two are indistinguishable. The same applies to the `std::string_view`
overloads of `Get`/`Set`: a key that could not be built and a property access
that failed both arrive as an empty optional.

`String::NewFromUtf8` never refuses bytes - it repairs them, one U+FFFD per
maximal invalid sequence - so an empty result from it means only that the
engine could not allocate. Reach for it when repair is what you want, and not to
make an encoding error go away. **The conveniences are lossy, and `String::New`
is not**: `ReturnValue::Set` with text and every way of throwing a fresh error
(`ub::Throw`, `info.Throw`, `Isolate::ThrowError`, `MakeError`) repair as
`NewFromUtf8` does, as V8's own do, because that text is usually read from
outside. So the same bytes are a string with a U+FFFD in it when a callback
returns them and no string at all when handed to `String::New` - both on
purpose, and silent either way.

### Binary data is copied in both directions, and there is no borrowing

A buffer backed by embedder memory was rejected on three counts, any one
sufficient: script can **detach** a buffer, so ownership moves on a schedule
script controls; neither collector offers a stable interior pointer, so a
borrowed span would be a handle with none of the rules handles have here; and
the engines' free-callback contracts do not match.

`ByteLength` is zero for a buffer script has detached, which is indistinguishable
from an empty one - and so is a view over one: its length *and its offset* are
zero and a copy writes nothing. `CopyBytes` and `CopyElements` truncate silently
and return how much they wrote; ask `ByteLength` first if you mean to take all of
it. `GetBuffer` on a view over a `SharedArrayBuffer` is empty, because this API
has no type for one; `CopyBytes` still reads it.

### `CopyElements<T>` with the wrong `T` copies nothing and says 0

**Silent, and 0 is also a legitimate answer for an empty view.** This does not
convert: a `Float64Array` read as `std::int32_t` is a mistake, not a rounding,
and silently obliging would be exactly the "looks like it succeeded" failure the
API exists to prevent.

---

## Running Python (the CPython backend)

Everything above applies to this backend too, except where it names the other
two engines. These are its own, and most are the two languages disagreeing
somewhere a JavaScript embedder would not think to look. `docs/python.md` has
the reasoning behind each; this is the list of what bites.

### Your scripts do not carry over, and nothing tells you until they run

**Loud, eventually.** The C++ compiles once and links against any backend; the
source text you hand `Evaluate` is the engine's language. A JavaScript script
on the CPython backend is a `SyntaxError` - or, for a one-liner like `"1 + 1"`,
quietly the same answer, which is how a test suite of small expressions can pass
on both and prove nothing. Choosing this backend is choosing Python.

### `None` is `undefined`, and `null` is something else

**Silent.** `undefined` is `None`, because that is what a Python function with
no `return` answers and what a missing value is in every Python API. `null` is
`unibind.null`, a singleton of its own. So `ReturnValue::SetNull()` hands script
an object that is falsy but **is not `None`** - `x is None` is false for it - and
a Python function returning `None` reaches C++ as `undefined`, where
`IsNull()` is false and `IsNullOrUndefined()` is true. Code that means "nothing"
from C++ should say `SetUndefined()`; `null` is for when the distinction is the
point. `json.dumps` does not know `unibind.null`.

### A whole-number `double` arrives in Python as an `int`

**Silent, and deliberate.** `Number::New(isolate, 3.0)` and
`GetReturnValue().Set(3.0)` reach Python as the `int` 3, so a script can index,
slice and `range()` with what a native returned. An integral value within ±2^53
is an `int`; everything else - and `-0` - is a `float`. A script that tests
`type(x) is float` on a native's answer is asking the wrong question. The other
way round, a Python `2.0` stays a `float`, and `ToString` of it is `"2.0"`, not
JavaScript's `"2"`.

### `ToBoolean`, `ToString` and `LooseEquals` are Python's

**Silent: a plausible wrong answer.** On this backend `ToBoolean` is `bool(x)`:
**an empty list, an empty dict, `""` and `0.0` are false**, where JavaScript
calls every object true. `ToString` is `str(x)`: `undefined` is `"None"`, `true`
is `"True"`, a list is `"[1, 2]"`. `LooseEquals` is Python's `==`, so `1 == "1"`
is false, with one JavaScript rule kept: `null == undefined`. A callback that
coerces its arguments to decide something is deciding it by Python's rules. The
one exception runs the other way: `bool()` of a `unibind.Object` is always true,
as every object is in JavaScript.

### A missing property is an error in Python and `undefined` in C++

**Loud, and only on one side.** `o.nope` raises `AttributeError` and `o["nope"]`
raises `KeyError`, as a Python programmer expects; `Get(context, "nope")` from
C++ answers `undefined`, as V8's does. Likewise a write to a read-only property
is a `TypeError` from Python and a silent no-op that answers true from C++ - V8's
sloppy `Set` - and `del` of a missing name raises from Python while `Delete`
answers true. A binding tested only from C++ has not seen what its script sees.

### Realms of one isolate share their modules

**Silent.** A realm here is a globals dictionary in one interpreter, so `sys`,
`sys.modules`, `builtins` and every imported module are shared by every realm of
an isolate. A script that patches `json.dumps` or sets `builtins.x` has done it
for its neighbours. Two sandboxes that must not see each other's module state are
two isolates, on two threads.

### A standard library directory on the machine replaces the embedded one

**Silent until it is the wrong one.** The pure-Python standard library is
compiled into the backend (`UNIBIND_PYTHON_EMBED_STDLIB`, on by default), and a
program needs nothing beside it. But a directory still wins when there is one:
`UNIBIND_PYTHON_HOME`, then a `python-stdlib` directory beside the executable -
any directory holding an `os.py`. That is there so a developer can run against
sources, and it is taken on a user's machine just the same: an
`UNIBIND_PYTHON_HOME` left pointing at another CPython's `Lib` gives the program
modules written for another version, with errors far from their cause. A module's
`__spec__.origin` says which one it got: `"frozen"` for the embedded one, a path
for a directory ([`docs/python.md`](python.md) section 10.3).

Two smaller things follow from the modules being frozen. A traceback into the
standard library shows `File "<frozen json.decoder>", line 354` without the
line's text, and `inspect.getsource` of a standard-library function raises
`OSError`; point `UNIBIND_PYTHON_HOME` at `Lib` while debugging to get both back.

### Built without the embedded standard library, the program runs only where it was built

**Loud, but a long way from its cause.** With `UNIBIND_PYTHON_EMBED_STDLIB`
off, the standard library is read from disk when the `Platform` is made, from
`UNIBIND_PYTHON_HOME`, then a `python-stdlib` directory beside the executable,
then the path the build found it at - which is inside the build tree. The last
one is why everything works until the program is copied somewhere else, where
`Platform` reports `EngineFault::Fatal` ("the Python standard library could not be
found"), `IsInitialized()` is false and every `Isolate::New` is null. Ship
`tools/python3/Lib` beside the program as `python-stdlib` - or leave the option
on.

### Three standard-library modules are refused in an isolate

**Loud.** An isolate is a sub-interpreter with a GIL of its own, and a module
that keeps state in C globals refuses to load in one, with `ImportError: module
X does not support loading in subinterpreters`. With CPython 3.14 that is only
`_wmi` (which `platform` does without), `_tracemalloc` - so there is no
`tracemalloc` - and `_suggestions` (which `traceback` does without). `ctypes`,
XML parsing, and the C `decimal` and `datetime`, which 3.12 refused, all work.
The list is in `cmake/vcpkg-ports/README.md`.

### A package with a C extension cannot be loaded, and there is no `site-packages`

**Loud.** The engine is a static library, and a `.pyd` links `python3X.dll`,
which does not exist in the process. Pure-Python packages work from wherever the
embedder puts them on `sys.path` - which is empty to begin with when the standard
library is embedded, and that one directory when it is read from disk: no
`site-packages`, no `PYTHONPATH`, no current directory.

### A long-running builtin cannot be stopped

**Silent: the watchdog appears to do nothing.** A stop lands between two
bytecodes, and a builtin is one bytecode. `sum(range(10**9))` ran for twenty
seconds under a one-second watchdog, and `time.sleep(6)` for six; the statement
after each did not run. The same goes for a blocking `socket.recv`, a lock, a
subprocess wait. It is decision 15's rule about blocking natives, reached from
the Python side.

### A thread the script started dies with its isolate, and runs only while script does

**Silent: work that simply stops happening.** `threading` works in an isolate,
within limits that are easy to miss:

- **It runs only while the isolate's thread lets it.** The two share a GIL, so a
  script's thread runs while the isolate's thread is running Python or blocked
  in a call - and not at all between scripts, however long the embedder waits
  before the next `Evaluate` or `PumpJobs`. "Background" work happens only in the
  foreground.
- **It cannot touch anything of `unibind`'s.** A bound function, a class, a
  `unibind.Object` attribute: each raises `RuntimeError: unibind: no isolate on
  this thread` there. Hand results back through plain Python objects.
- **A stop reaches it.** `TerminateExecution` stops every thread of the isolate's
  interpreter, uncatchably, with no `finally` run.
- **`~Isolate` ends it.** Teardown stops a script's threads and waits up to two
  seconds for them. Plain CPython would run them to completion; here they are cut
  off, so work a script wants finished it must `join()` before the script ends.
- **One stuck in a call that never returns is left behind, not waited for.** The
  isolate abandons its interpreter with the thread in it rather than hang or
  crash, and if that thread has still not finished by `~Platform`, CPython is
  never finalized - the process ends with it initialized.

### A stop leaves Python's own cleanup undone

**Silent.** No Python runs while a stop is in force, anywhere in the interpreter,
and that includes code nobody thinks of as the script's. A weakref callback does
not run, so a `WeakSet` whose members went during a stop goes on counting them.
An `except` or `finally` that would have released a lock does not run, so a lock
can stay held: a stop inside `Thread.join()` leaves the thread's bookkeeping
unfinished, and `is_alive()` answers true for a thread that has ended. After a
cancel, do not trust state that a stopped script was in the middle of changing.

### No daemon threads, so some of the standard library fails

**Loud.** Daemon threads are refused in an isolate, because one would outlive the
interpreter it runs in. `subprocess.run(..., capture_output=True)` is the one
people meet: on Windows it reads the pipes on daemon threads, and raises
`RuntimeError: daemon threads are disabled in this interpreter` after the child
has started. Without capturing it works, and so does `os.system`.

### `asyncio.run` runs on a loop of its own, not the isolate's

**Loud, in one case.** `asyncio.run()` works, on a fresh loop it makes and closes
- which is fine, and afterwards `asyncio.get_event_loop()`, `asyncio.Future()` and
`asyncio.ensure_future()` find the isolate's loop again. What does not work is
`asyncio.run()` inside a script that uses top-level `await`: the whole script is
then a task on the isolate's loop, and it is `RuntimeError: asyncio.run() cannot
be called from a running event loop`, as in plain Python. Top-level `await` is the
better tool in an embedding anyway: it runs on the isolate's loop, which
`PumpJobs` drives, so its promises are ones C++ can see.

### The isolate's loop cannot run subprocesses

**Loud.** It is a `SelectorEventLoop`, which on Windows has no subprocess or pipe
support: `await asyncio.create_subprocess_exec(...)` at the top level is
`NotImplementedError`. Inside `asyncio.run()`, which makes a Proactor loop of its
own, it works.

### `Symbol.asyncIterator` and `Symbol.hasInstance` do nothing from Python

**Silent until used.** Well-known symbols that stand for a Python protocol are
stored under the protocol's name, and `Symbol.iterator` is honoured: `iter()` of a
`unibind.Object` calls it. But Python looks `__aiter__` and `__instancecheck__` up
on the *type*, never on an instance or its prototype, so an `asyncIterator`
method does not make `async for` work (`TypeError`) and a `hasInstance` method is
ignored by `isinstance`.

### A debug CPython runs out of stack after about fifteen levels

**Loud.** A debug build's evaluation loop is unoptimised, and every level of
recursion through C - an import inside an import, a native calling back into
Python - costs it about 50 KB of stack, twenty times what a release build
spends. On a thread with Windows' default 1 MB that is some fifteen levels
before `RecursionError`, which a release build does not reach for hundreds. Run
debug builds on threads with a few megabytes of stack.

### The heap limit counts what the script allocates on its own thread

**Silent.** `heapLimitBytes` is kept by allocator hooks that charge each block to
the allocating thread's isolate. What the interpreter allocates while it comes up
is not counted, nor its raw allocations, nor anything a `threading.Thread` the
script started allocates - that is charged to no isolate at all. A script that
wants to get round the limit can.

### Python is not a sandbox

**Silent in the worst way.** A JavaScript engine can reach nothing the embedder
did not bind. CPython can reach the file system, the network, other processes
and the environment the moment a script says `import os`, and removing things from
`builtins` does not take any of it away. Run only Python you would run as the
host process, or contain the process with the operating system.

---

## Working on the library itself

### A test case's name may not contain `;`

**Silent: the case passes without running.** `doctest_discover_tests` registers
each case with CTest by name, and CMake splits a name at `;` as it splits any
list. The case becomes two CTest tests, each asking the runner for a name that
matches nothing, and a filter that matches nothing passes. The whole-suite-in-one-
process test still runs it, which is the only reason anyone would notice - and how
two cases in the python suite were found not to have been running under `ctest`.
The parity reporter's `|` is the other character a name may not hold.

### Add a row to `tests/cmake/Capabilities.cmake` when you add an operation

Otherwise the suite never gates on it, and a backend that has not implemented it
yet **fails to link** instead of reporting a skip. The skip is the useful
answer.

### The frame-exhaustion lever only reaches a frame that grows through the C++ heap

**Silently proves nothing otherwise.** The suite provokes OOM by replacing
global `operator new`. A backend whose frame spills through the *engine's*
allocator is invisible to that - `JS::RootedVector` defaults to
`js::TempAllocPolicy`, which allocates through `js_malloc`, so the SpiderMonkey
frame had to be given an allocation policy of its own before the test could see
it (and then see the `operator new` trap at the top of this page).

The case checks whether an injected failure actually fired and reports a skip if
none did, rather than claiming coverage it does not have.

### A case that encodes a promise is left red, never weakened

Weakening it turns a bug into coverage nobody has, which is the one thing the
suite exists to prevent. The two cases that were ever in that state found the
two code-cache bugs above, and both were found the same way: **assert on what
ran, not on what the API reported.**

### The storage sizes are a compile error, never a heap fallback

`UNIBIND_FRAME_STORAGE_SIZE` and friends are checked by `static_assert` in each
backend. If a backend type grows past one, raise the number - a frame on the
heap is not a `JS::Rooted` on SpiderMonkey, and falling back to one would quietly
give up the property the whole handle model is built on.

### Two small things that will bite exactly once

- **V8 deletes `HandleScope::operator new`**, which also hides the *global
  placement* new, so every in-place construction of one spells `::new`.
- **`JS_DisableInterruptCallback` / `JS_ResetInterruptCallback`** are a
  save/restore pair with a confusing spelling: `Disable` returns the *previous*
  state and `Reset` takes a parameter named `enable`. Feeding the first straight
  into the second is the round trip that works. Do not try to reason about the
  parameter's name.
