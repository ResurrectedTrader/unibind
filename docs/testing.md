# Testing, and where the engines differ

How the suite is built and run is in [`tests/README.md`](../tests/README.md).
This file is the other half: the places where the two engines do not agree, and
what the suite asserts instead.

That suite is the JavaScript backends'. The CPython backend runs Python, so it
has a suite of its own, and [the last section](#the-cpython-backends-suite) is
its account: why parity does not apply to it, what it covers, and what it does
not.

One rule governs the whole of it: **where the engines genuinely differ, the test
asserts the shape and the difference is written down here.** A test that
branches on `Platform::BackendName()` would be a bug, so none does - a
difference either becomes a portable assertion, or it becomes an entry below.
Nothing is allowed to be neither. `Platform::BackendVersion()` is under the
same rule and one step stronger: its *shape* is the engine's own and is not
promised at all, so the suite asserts only that there is one and prints what it
says.

## Running clang-tidy

Reading the tree takes about a minute; the awkward part is that there is nothing
to read it *with* until you make one.

**The Visual Studio generator writes no `compile_commands.json`**, so clang-tidy
has no idea what defines, include paths and standard a file is compiled with -
and an engine header without its defines does not even parse. The `tidy-v8`,
`tidy-spidermonkey` and `tidy-python` presets exist for that and nothing else
(the last builds CPython through vcpkg on its first configure, as the `python`
presets do): Ninja, the same
ClangCL toolchain, the same flags, and nothing linked. They are x64, because
Ninja has no `-A` and clang-cl targets the host; nothing clang-tidy reports
depends on the architecture.

```powershell
$llvm = '<VS install>\VC\Tools\Llvm\x64\bin'
$env:VCPKG_ROOT = '<your vcpkg>'
cmake --preset tidy-v8
# The suite includes a header the build generates by asking the backend library
# what it defines, so that header has to exist before any test file will parse.
cmake --build build/tidy-v8 --target tests/generated/unibind_test_capabilities.h
python "$llvm\run-clang-tidy" -clang-tidy-binary "$llvm\clang-tidy.exe" -p build/tidy-v8 -quiet
```

`run-clang-tidy` is LLVM's own parallel driver and lives beside `clang-tidy.exe`
in the ClangCL toolset. It uses every core by default, which is what takes the
whole tree - both backends, the public headers, the suite, the benchmark and
the headers-only tool - from a single-threaded afternoon to about a minute.

Three things to get right, and **every one of them fails quietly rather than
loudly** - this tooling's characteristic shape is a run that exits zero having
done less than you think, or that reports something a colleague's run does not:

- **Name the binary.** `run-clang-tidy` spawns whichever `clang-tidy` `PATH`
  finds first, and a machine may well have two - the ClangCL toolset's, and a
  standalone LLVM. Different releases have different check sets, so which one
  ran decides what the lint *means*. `-clang-tidy-binary` is not optional here,
  and printing `--version` of the binary you *found* proves nothing about the
  one that *ran*.
- **Run it from the repository root.** clang-tidy decides whether a diagnostic
  is in one of *our* headers by matching `HeaderFilterRegex` against a path it
  resolves relative to the working directory. Started somewhere else it still
  exits zero, having reported a fraction of what it should.
- **Do not lint the engines.** Two independent things stop it, and either alone
  is enough: the backends mark the engine include directory `SYSTEM` (and
  clang-tidy's `SystemHeaders` defaults to false), and `HeaderFilterRegex` names
  our own include roots. `ExcludeHeaderFilterRegex` is a third layer that
  carries nothing the first two do not - which is just as well, because it is
  new enough (clang-tidy 19) that an older binary drops the key **without
  saying so**. `lint.yml` refuses such a binary rather than letting it read
  less; if you add a setting to `.clang-tidy`, check it survives
  `clang-tidy --dump-config`.

Which checks are on, and why each disabled one is disabled, is in `.clang-tidy`
itself. Two directories narrow it further and say why in the same way:
`tests/`, because doctest's `REQUIRE` defeats the optional-access check, and
`src/backends/spidermonkey/`, because two path-sensitive analyzer checks end up
pointing inside SpiderMonkey's own crash macro.

## What the suite covers

| area | file | notes |
|---|---|---|
| handle lifetimes | `cases/handles_test.cpp` | `docs/lifetimes.md` section 5, one case per rule: LIFO nesting, copying and storing within a frame, escaping once, sixteen times, and through two frames, `Global` across frames, and frame spill past the inline slots |
| frame exhaustion | `cases/handles_test.cpp` | rule 9: a frame that cannot grow yields an *empty* handle and reports out of memory - see below for where it can be provoked |
| roots given back | `cases/handles_test.cpp` | frames and globals, measured through replaced `operator new` |
| values | `cases/values_test.cpp` | kinds, the narrowing lattice, V8's type predicates, strict / same-value / loose equality, every coercion, UTF-8 round-trip and truncation, `NewFromUtf8`'s repair of every kind of malformed sequence, externals |
| objects | `cases/objects_test.cpp` | get / set / delete / has / hasOwn, own-key filters, prototypes, attributes, arrays, and an accessor on one object - native on every read and write, a real accessor to `getOwnPropertyDescriptor`, read-only without a setter |
| functions | `cases/functions_test.cpp` | arguments, receiver, every `ReturnValue` setter and integer width, typed callback data, a script value as a function's data (one callback behind many functions, any kind of value, collected with its function, crossing a realm), text returned or thrown with bad bytes arriving repaired, calling and constructing both directions, reentrancy |
| exceptions | `cases/exceptions_test.cpp` | throwing values and errors, every `ErrorKind`, nesting, re-throw, reset, a handler that does nothing consuming what it caught, stack shape, and `Location`: a runtime error, a syntax error that has no frame, a thrown non-error, and nothing caught |
| scripts | `cases/scripts_test.cpp` | compile once run many, a script outliving its scope, origins in diagnostics |
| realms | `cases/realms_test.cpp` | separate globals, values crossing, `instanceof` not crossing, nesting, refcounting. A value *works* in any realm of its isolate and that is asserted; its *identity* seen from a foreign realm is not guaranteed, because an engine that wraps hands out a distinct object, so nothing there compares objects across a realm boundary |
| symbols | `cases/symbols_test.cpp` | identity, description, the registry, the well-known five, symbols as keys |
| the isolate | `cases/isolate_test.cpp` | the typed embedder pointer, throwing from outside a callback, heap figures and their consistency with each other, collection requests, one isolate after another, and a second one at once being refused rather than made |
| templates | `cases/templates_test.cpp` | constants, methods, accessors, nesting, inheritance, `HasInstance`, instantiation into several realms, and the call/construct grid: a template given a callback answers to both and tells them apart, while a method and both halves of an accessor answer to neither `new` |
| interceptors | `cases/interceptors_test.cpp` | all five named and all five indexed hooks, driven through `in`, `Object.keys`, `getOwnPropertyDescriptor` and `delete` |
| classes | `cases/classes_test.cpp` | construction, a constructor that hands back a share with its own deleter, statics, accessors, one instance carrying an accessor its class does not, `Wrap`, checked unwrapping, and finalizers |
| ownership and destruction | `cases/ownership_test.cpp` | who owns a native and when it is destroyed: exactly once per native and never twice, a native still whole after a collection that spared it, two wrappers over one native, co-ownership with the embedder in both directions, the last reference winning whichever one it is, the no-op-deleter escape hatch, the failure paths that must not leak, an allocation failure walked through every step of the hand-over the backend makes itself (which is where the double free lived) and kept out of the engine's own, what a function, a function carrying a script value that points back at it, and an external cost their isolate once they are collected, and that finalizing one instance costs the same however many the isolate has made |
| a sandbox | `cases/sandbox_test.cpp` | the one composition: a second realm, an interceptor over every property, a `Global` held across calls, and a prototype method that has to survive the interceptor |
| termination | `cases/termination_test.cpp` | stopping a running script from another thread, telling a stop from a throw, script not being able to catch one, a remembered stop, repeatability, and a blocking native running to completion |
| stacks | `cases/stacks_test.cpp` | frames off a caught exception and off the running stack, the origin's line offset reaching a diagnostic, and the empty answers |
| compiled-code caching | `cases/codecache_test.cpp` | a blob accepted, and - the half that matters - a damaged, truncated, foreign or empty one producing an ordinary compile with identical behaviour; an eager compile's blob covering functions that never ran, including after a lazy compile of the same source and after a refused blob |
| binary data and serialization | `cases/binary_test.cpp` | a span in and the same bytes at the same width out, views over part of a buffer, a `DataView` and a typed array asked the same byte-level questions, a view whose buffer script detached, and a value moved to another isolate with equal contents and an identity of its own |
| roots that name one value | `cases/handles_test.cpp` | `Global` identity: two roots over one object, a duplicate, a root against a `Local`, an empty root equal to nothing including another empty one, and the registry lookup the comparison exists for. The clause that needs **no scope open at all** runs in a process of its own - see below |
| work that is not a call | `cases/jobs_test.cpp` | promise continuations waiting for `PumpJobs` and for nothing else, `async`/`await`, a chain draining in one pump, posted work ordered and never coalesced, work dropped when the isolate goes, a job's throw stopping at the pump, an interrupt reaching a running script while the job it announced waits for the drain, an interrupt callback making a handle, and a promise the embedder makes, settles, and has settled from a job, and delayed work waiting for its delay and then for a pump, in the order it fell due |
| the engine in trouble | `cases/faults_test.cpp` | decision 28: running out of memory reaching the embedder's handler with the right kind, the right isolate and its typed data; nothing reported while nothing is wrong (a script's own `throw` included); and, where the engine has the hook, a heap about to hit its ceiling being raised and the script that filled it stopped. See below for the kind that cannot be reached at all |
| the inspector | `cases/inspector_test.cpp` | decision 29: that an inspector exists exactly where `Supported()` says, one per isolate; and where it does, `Runtime.evaluate`, a `debugger` statement pausing into the client's loop, a dispatch requested from another thread reaching a busy isolate and an idle one, a dispatcher hammered from another thread while its inspector is destroyed, a burst of requests sharing one wake-up while each still runs once, `New` and `Connect` null only for want of memory, `Resume` / `Stop` / destruction in every order (inside a pause included), a script's URL, and realms and sessions coming and going without growth |
| the edges | `cases/coverage_test.cpp` | what the cases beside each feature added since the first release leave out: the predicates on boxed primitives, externals and the ends of the integer ranges; each integer width either side of `int32`; a share-returning constructor called both ways; an accessor's attributes, receiver and throw, and one pair of callbacks behind two names; a location's line offset, its CRLF and the handlers that have none; delayed work from another thread and dropped at teardown; a view over shared memory; eager compiles with a good blob and with none; dispatch order, dispatches dropped with their inspector, and a stopped session |
| bugs that were found | `cases/regressions_test.cpp` | one case per bug, each of which failed - or ended the process - on the code before its fix, grouped by area. Locations: quoting another script of the same name, lines cut at LF only, a cached script unquoted, a column offset dropped, an error placed where it was made rather than thrown, a script named in bytes that are not UTF-8 whose errors could not be caught. Stacks: self-hosted built-ins named as frames, an anonymous function named by the engine's guess, a capture limited to no frames capturing all of them. Messages: running script to read one, or having none for a symbol; throwing or catching with no realm entered ending the process. Objects and arrays: a refused definition or accessor throwing, or answering true on a proxy it never reached; an array longer than an int ending the process or refused. Binary data: a misaligned or detached typed array, a buffer too large to allocate, BigInt64, BigUint64 and Float16 arrays read as doubles. Names and text: a non-ASCII or malformed template or class name, a member named like an array index crashing, malformed text leaving an exception pending. Classes: a quiet refusal handing script an instance with no native, an out-of-memory unwinding the engine. Jobs and promises: a delay too long for the clock running at once, a settled promise settled again, a promise from another realm unsettleable. Inspector: a realm announced twice left behind. Realms: a value made for one realm made in whichever was entered |
| bugs in the native bindings | `cases/regressions_bindings_test.cpp` | the same, for classes, templates, interceptors and callbacks. Classes: a constructor's return slot replacing the instance, a plain call constructing on the global object, `new` on a class with no constructor a plain `Error`, a new.target with no object prototype falling back to the class's. Templates: an object a function template's constructor answers with ignored, a primitive receiver dropped or handed over raw, a nested template losing its handler, `HasInstance` reading the prototype chain, a shape changed after instantiation ending the process, a read-only `prototype`, constructors chained by `Inherit`, accessor halves named. Interceptors: an index above 2^31 reaching the named half, repeated enumerator keys, the hidden target as receiver, a prototype swap not reaching lookups, an intercepted plain-template object with no prototype. Stacks: native recursion through the engine, and script recursion under the default limit, overflowing the thread's stack |
| the harness itself | `cases/harness_test.cpp` | that a skipped area is visible rather than absent |

## What it does not cover

- **Threads.** An isolate is single-threaded by contract and nothing tests what
  happens if that is violated; there is no portable way to observe it. Two heaps
  at once is two threads (decision 11), so that is untested for the same reason.
- **A plain call to a template given no callback.** Deliberately unspecified -
  there is nothing to run - so nothing asserts it either way.
- **A real heap exhaustion, and `EngineFault::Fatal`.** Both are covered under
  "What decision 28 can and cannot be asked" below. `heapLimitBytes` is
  exercised now, but only on the backend that can be asked about it first.
- **A bring-up that fails.** `Platform::IsInitialized()` is the failure channel
  and the suite pins that it is *read*, but neither engine can be made to fail
  bring-up from a test - one segfaults, one dies, and on one the injector never
  fires at all. Measured, with the numbers, under "A bring-up that fails" below.
- **Making a `BigInt`.** The tag and the `TypeCode` exist and are asserted
  against one script made, but no factory does, so native cannot make one.
- **Stack trace contents beyond frame names.** Format is the engine's; see below.
- **A `Local` used after its frame closed**, in Release. It is undefined
  behaviour, so it cannot be a test case among others. A checked (Debug) build
  diagnoses it, and there is a CTest test expecting exactly that death; Release
  compiles the assertion out and the test is not registered.
- **A `Context`, `Script` or `Global<T>` outliving its isolate**, in Release,
  for the same reason and with the same shape of test in a checked build. See
  decision 27.
- **A refcounted backend.** There is one now - CPython - and this suite does not
  run on it, because its scripts are JavaScript. Nothing here assumes a tracing
  collector except the finalizer timing case, which already reports rather than
  asserts; `tests/python/` pins the refcounted behaviour itself (the last
  section).
- **A template cache that stored one half of a pair.** SpiderMonkey materialises
  a template into a realm as a constructor and a prototype, cached together; if
  the second store failed the first would still hit, and every later instance in
  that realm would come out with a null prototype while everything reported
  success. The fix - a lookup that finds one half and not the other is a cache
  that cannot be trusted - rests on review, because the store that would have to
  fail is a `JS_SetProperty` through the *engine's* allocator, which the suite's
  injector does not reach, and the cache lives in a hidden slot script cannot
  touch.
- **An engine that refuses to shut down.** `v8::V8::Dispose()` returns a `bool`
  that is now checked; nothing can make it answer false from a test.
- **A native call whose exception could not be taken or wrapped.** Two failures
  inside SpiderMonkey's `FinishNativeCall`, both fixed and neither reachable:
  `JS_WrapValue` failing (which used to install the *unwrapped*
  cross-compartment value as the pending exception, where the catching realm
  cannot name it) and `StealPendingExceptionStack` failing (which used to
  return false with nothing pending - this engine's *uncatchable* idiom, the
  one termination uses, so an ordinary throw became an unwind no `try`/`catch`
  could see and `HasTerminated()` could not explain). Both fail only by running
  out of memory inside the engine's own allocator, which the suite's injector
  does not reach.
- **An interceptor hook that throws *and* declines**, on the enforcement side.
  `unibind/template.h` makes it normative that the throw wins, and both
  backends now answer intercepted when a hook leaves an exception pending -
  but the case that pins it (`interceptors: a hook that throws has intercepted
  the access whatever it returned`) passed before that change as well as after,
  because each engine's own dispatch already stops at the pending exception.
  The rule is written down and asserted for the backend that does not yet
  exist; the code that enforces it is belt and braces on the two that do.
- **An instance that cannot carry a native.** Both backends check that the
  object a native is about to be attached to has somewhere to put it, and fail
  the construction rather than writing past the field count (which on V8 is an
  `ApiCheck`, i.e. an abort). The false branch is unreachable from script: every
  way script can reach a class constructor - `new Foo()`, `class Sub extends
  Foo`, `Reflect.construct(Foo, [], Array)` - was measured, and the receiver
  carries the field in all of them. The check stays because the cost of being
  wrong about that is the process.

## Where the engines differ

### Error message wording

Both engines put the embedder's own text in the message and wrap it in their
own decoration. The suite asserts that the text survives
(`message->find("expected one argument")`), never the whole string.

Where the *kind* of an error matters, the portable question is asked in script -
`e instanceof TypeError`, `err.constructor.name` - rather than by parsing a
message.

### Which column an error is placed at

`TryCatch::Location` and `StackFrame` agree on the script and the line, and the
engines choose the column differently: for `null.x` V8 names the column of the
property it could not read and SpiderMonkey the start of the expression. The
suite asserts columns only relative to one another - a column offset moving a
first line's columns by exactly itself - never as a number.

### Stack trace format

`TryCatch::StackTrace` returns the engine's own format, and the two are not the
same text. What both do is name the functions on the stack and the script's
resource name, so `cases/exceptions_test.cpp` asserts that `innerMost`,
`outerMost` and `trace-test.js` appear in it, and nothing about the layout.

### `This()` and `Holder()` on a property hook

V8 15.6 hands an interceptor only the holder, so on that backend `This()` and
`Holder()` are the same object inside an interceptor. `unibind/function.h` makes
that the contract: where an engine offers only one of the pair, the two name the
same object. The suite asserts the holder is the object carrying the handler,
and only that the receiver is an object.

An *accessor* is different and is specified: it is installed as a real
ECMAScript accessor property precisely so that it has a receiver, and
`templates: an accessor sees the instance as its receiver` asserts that a
prototype accessor read through an instance finds the instance.

### A key an enumerator lists that no hook answers for

An enumerator is expected to list the keys the query or the getter answers
for. One that lists a key neither answers makes the engines disagree, and
neither can be made to agree with the other without breaking something else:
V8 reports such a key from `Object.keys` (though not from `for...in`, which
asks whether it is there), while SpiderMonkey's interceptor is a proxy whose
`Object.keys` asks each listed key for its descriptor and drops one nobody
describes. The suite lists only answered keys; `unibind/template.h` says to.

### Finalizer timing

Neither engine promises to collect on request, and they do not collect the same
things at the same time. What is portable, and what the backends were built to
guarantee, is the invariant across the whole lifetime:

> the engine gives back every share it took, exactly once, by the time its
> isolate is gone.

That is deliberately about **shares**, not about natives, and the distinction is
the point rather than pedantry. Decision 14 separated the wrapper's cell from
the native it holds a share of, so the guarantee splits in two:

1. Every cell is destroyed exactly once, and all of them by teardown - the half
   a backend works for, and why an isolate keeps a list of live instances and
   finishes the survivors as it goes away.
2. Every native is destroyed exactly once **when its last share goes**, which
   may be *after* the isolate, because the last share may be the embedder's.

The older single sentence - every native destroyed by the time the isolate is
gone - forbade in plain terms the case shared ownership exists for, which is why
the case that used to carry it as a name is now
`classes: the engine gives back every share it took by the time the isolate is
gone`. It asserts (1), with half the instances deliberately still rooted at
teardown; the number destroyed *before* teardown is reported as a message, not
asserted, because that is the collector's business. `ownership: a native the
embedder co-owns outlives its wrapper` asserts (2).

### Where frame exhaustion can be provoked from a test

`docs/lifetimes.md` rule 9 - a frame that cannot grow yields an empty handle and
reports out of memory, never a slot that reads as `undefined` - is asserted by
`handles: a frame that cannot grow yields an empty handle, not undefined`. The
suite provokes it by replacing global `operator new` and making exactly one
allocation fail (`tests/support/allocations.cpp`).

That lever reaches a backend whose frame overflow is an ordinary C++ container
and does not reach one whose frame grows through the *engine's* allocator. The
case therefore checks whether an injected failure actually fired: if none did,
the frame never touched the C++ heap, the case has proved nothing, and it
reports a skip rather than asserting something it did not observe.

So the rule is asserted where it can be, and the gap is visible where it cannot
be, instead of the suite quietly claiming coverage it does not have. Closing the
gap needs a lever the engine's own allocator honours - which is a backend's to
offer, not a test's to invent.

**On x64 V8 the lever has to be taken rather than asked for.** That monolith
carries PartitionAlloc's allocator shim, which defines twelve of the twenty
allocation operators in an object every link pulls in, so the suite's own
definitions are a duplicate symbol. `tests/cmake/EngineAllocator.cmake` reads
the archive's symbol index, and where the operators are in it the suite links
with `/FORCE:MULTIPLE` - its objects are on the command line and the monolith is
an archive scanned after them, so its definitions win. Nothing is assumed about
whether that worked: the fired/not-fired check above is exactly the check for
it, so a configuration where the engine's operators won instead reports skips
rather than passing quietly.

### Heap statistics

`HeapStatistics` measures different things on the two engines and only
`usedBytes` is comparable, and only as a trend. Nothing asserts a figure; the
leak tests count C++ allocations instead, which is a question both engines
answer the same way because neither of them is the one answering it.

What is asserted is that the figures agree with *each other*: something is in
use, what is reserved holds it, and the ceiling is above both. The six optional
figures are empty on SpiderMonkey and filled on V8, so the case asserts only
that the pairs come together - both malloced figures or neither, both
global-handle figures or neither, and a pool at least as large as its use - and
reports which it was given. It does not assert that the malloced peak is at
least the current figure: V8 raises its peak only at certain points and reports
a current figure above it in between.

### The inspector, and shared memory

Two things one backend has and the other does not, and both are reported as
skips rather than branched on. The inspector's protocol cases ask
`Inspector::Supported()` and skip where it says no, which is every one of them
on SpiderMonkey (decision 29); the case that asks only whether an inspector
exists runs on both. And `coverage: a view over shared memory ...` needs a
`SharedArrayBuffer`, which a SpiderMonkey realm made here does not define - the
engine exposes it only where shared memory is enabled at realm creation, and the
backend does not ask - so it asks script whether the constructor exists and
skips if not.

### Eager compiles are seen through the blob

Nothing outside an engine can see whether a function body was compiled, so the
`EAGER_COMPILE` cases look at the one thing that is portable: the size of the
code-cache blob. Not against a fixed factor, though. How much a *lazy* blob
holds is the engine build's business - for the suite's source a debug V8's is
more than three times a release V8's, and its eager blob is only about a
quarter larger than its lazy one where a release V8's is more than three times
it - so "more than one and a half times", which is what the cases asserted
first, passed on every release engine and failed on the debug one. What they
assert now holds on every engine and every build of one, and they report all
three sizes:

- an eager blob is **larger than a lazy one** of the same source; and
- it is **at least as large as a lazy one made after every function has run**,
  inner ones included - the blob an embedder would otherwise have to warm up to
  get. V8's blob takes in what has run since the compile, so there the two are
  the same size, to the byte, in both builds; SpiderMonkey's is the compile as
  it was, so there the second is no larger than the first lazy one.

That covers an eager compile after a lazy one of the same source in the same
isolate - which V8 would otherwise answer from its own cache with the lazy
result, a blob no larger than the lazy one - and after a refused blob.

## What the new areas assert, and what they cannot

### Ownership: the questions a passing count would hide

A wrapper owns a *share* of its native (`unibind/class.h`), so a native can be
named by two wrappers, or by a wrapper and the embedder at once. The failure
that model makes possible is a use-after-free, and the thing about a
use-after-free is that a suite counting destructions will happily agree that
the total came out right - one destroyed twice and one leaked adds up the same
way. So `tests/support/ownership.h` keeps a registry keyed by an id each native
carries, and every case asks **per native**: `Deaths(id)`, never just a total.

The cases are deterministic where the collector's are not, and the reason is
worth copying: the embedder holds a share of its own, so "has the engine given
its share back yet" is a `use_count()` the test can read rather than a
destructor it has to wait for. Where a case does depend on the collector - has
the unreachable wrapper been taken? - it **reports** rather than asserts, and
says in its message that it proved the weaker thing.

**Which thread a destructor runs on is asserted, not assumed.** Foreground
finalization is a requirement rather than tidiness: a box's `destroy` drops a
`std::shared_ptr` whose other holders are the embedder's, so an engine
finalizing on a helper thread would race them - and a race in a finalizer is
exactly the failure that does not reproduce, so counting destructions would
never see it. `ownership: a native is destroyed on the thread that owned its
isolate` makes the isolate on a thread of its own and records where each
destructor ran, which is what makes the question sharp: a death on the main
thread is then as visibly wrong as a death on an engine's helper thread.

What is still not assertable: that the engine releases a share *promptly*.
Neither engine promises to collect, so "destroyed after both wrappers are gone"
is asserted as "not destroyed while one remains, and destroyed exactly once by
the time the isolate is" - which is the strongest portable form of it.

**An injected allocation failure is kept out of the engine.** `ownership: a
hand-over that runs out of memory gives the native back exactly once` walks one
failure through every allocation `Wrap` makes, and the walk used to be a fixed
eight stops - which on V8 reached the engine's own `operator new` calls, from
`ObjectTemplate::NewInstance` and from the global handle behind the weak root.
V8 is compiled without exceptions, so a `std::bad_alloc` thrown there unwinds
through frames that clean nothing up: a debug V8 aborts on the handle-scope
level the unwound `NewInstance` left behind, and a release V8 carries on with
its handle scopes, its instantiation cache and its young-handle list quietly
wrong. That is not a question about ownership and no backend can answer it, so
the case now measures how many allocations a complete hand-over asks for - the
fewest over a few, after one that lets the engine do its first-instance work -
and walks only those. They are the backend's: V8's backend makes every
allocation of its own before it asks the engine for anything (the record that
files the box, and the frame slot the handle will need), and SpiderMonkey's
engine allocates through its own allocator and asks for none of them.

### Termination

The contract these hold the backends to is in `unibind/isolate.h`. Two things about
it are worth writing down here.

**Only one engine has a terminating state of its own.** On the other, the
promise that "everything that would run script fails until the termination is
cancelled" is the *backend's* bookkeeping over an engine whose context is live
again the moment the unwind finishes. Nothing in that engine objects if the
bookkeeping drifts, so `termination: a handler does not swallow a stop the way
it swallows an exception` is what enforces it - and it asserts the half that
both accounts of the feature agree on: while the handler is open, the isolate is
still terminating and runs nothing. Whether *closing* the handler also ends the
stop is the open question listed at the end of this file, so that case reports
it rather than asserting it, and every other case cancels (a no-op when nothing
is armed) before carrying on. A decision either way touches one case.

**And the other engine's state is not the stop either.** V8 is terminating
only while its termination is pending: it forgets once the unwind reaches the
top, and a native whose own `TryCatch` caught the stop is not terminating to it
at all. So a stopped isolate on V8 ran the next getter, proxy trap, conversion
or call, and a native that called its callback again after catching the stop
ran a loop there for ever. V8's backend now keeps the stop itself at every
entry that can reach script, as SpiderMonkey's does, and arms the engine again
whenever it refuses; `regressions: a stopped isolate runs no getter, trap or
conversion either` walks the entries.

**A compile during a stop fails without asking the engine.** V8 asserts, in a
debug build, that nothing enters its compiler while a termination is unwinding;
a release V8 compiles anyway. Decision 15 says a stopped isolate may be asked
to compile and run and that nothing will run, so the backend answers a compile
itself while V8 is terminating - it fails, as the run it would lead to would -
rather than make a call the engine forbids.

**A native that does not re-enter the engine cannot be interrupted**, on either
engine: a stop takes effect where the *engine* checks for one, so the spinning
native finishes. That half is portable and is asserted. *Where* the next check
is, is not: one engine unwinds at the next statement, and V8 has no check
between two top-level calls, so the script runs to the end and the stop stays
armed for whatever runs next. The case reports which happened and asserts only
that the isolate is usable afterwards.

Not covered: what a stop does to a native callback that is *itself* several
frames into script when it lands, beyond `IsExecutionTerminating()` being
readable from one.

### Stacks

Both engines name functions, scripts and lines; nothing else about a stack is
portable. So the cases assert the frames they *caused* - `innerMost`,
`outerMost`, the resource name, a line number above zero - and report the rest,
including how many frames there were and what the outermost one is called.
`TryCatch::StackTrace` is the engine's own text and is only ever searched for a
substring.

A thrown value that carries no stack (a string, on both engines) has to answer
empty rather than hand back a plausible stack belonging to somewhere else; the
case accepts either an empty optional or an empty vector and reports which,
because the two are the same thing to a caller.

### Compiled-code caching

Nothing here looks inside a blob, and no blob crosses a backend: a blob is
opaque bytes belonging to one engine build. What is asserted is the pair of
outcomes - accepted, or an ordinary compile - and that `UsedCodeCache()` tells
them apart. The damaged / truncated / foreign / empty cases all assert
*identical script behaviour*, because that is the failure an embedder never
sees: blobs silently stop being accepted after an upgrade and the only symptom
is a slower start.

An engine that declines to produce a blob for the test source reports a skip
rather than asserting something it did not observe.

### Serialization

A round trip is asserted **within** one backend only, never across, and no case
compares blob bytes: `unibind/value.h` makes a blob opaque and build-specific, so a
test that compared two engines' bytes would be asserting something the API
explicitly does not promise. What crosses backends is the *behaviour*: equal
contents, distinct identity, refusal for a value that cannot be cloned, and
refusal for a damaged blob rather than a misreading.

The cross-isolate case is the only place in the suite that makes a second
isolate, and it does so on a second thread because one isolate per thread is a
rule (decision 11). The worker thread asserts nothing; it collects plain values
that the main thread checks after the join, so doctest is never used from two
threads at once.

### `IsolateOptions`

`stackLimitBytes` is asserted twice over: that runaway recursion becomes a
`RangeError` rather than a dead process, and - the part that makes it an option
rather than a comment - that a smaller limit reaches a smaller depth. No
particular depth is asserted; that is engine tuning.

`heapLimitBytes` used to be unexercised, and the reason was the right one at the
time: what an engine does at its ceiling is not comparable, and both are allowed
to abort rather than report - which is not something a case can survive. What
changed is decision 28, not the argument. `Isolate::SetHeapLimitCallback` gives
one of the two engines an answer other than aborting, so on that backend the
option is now asserted for real - a 32 MiB ceiling, a script that fills it, and
a callback that raises the ceiling and stops the script - and the case ends with
the isolate evaluating `1 + 1`, which is the assertion that the process is still
here. On the other backend there is no hook, the case does not link, and it
reports a skip. Nothing about the ceiling itself is asserted on either: the
figure the callback is handed is checked only for being non-zero, because V8
rounds a request of 32 MiB to 26 MiB and that is engine tuning.

### What decision 28 can and cannot be asked

**Out of memory is asserted, and not faked.** The lever is the one rule 9
already uses - a replaced `operator new` that fails exactly once, at the moment
a handle frame has to grow - so what the case reports is a real allocation
failure reaching the backend, not a call to something that pretends. Both
backends raise `EngineFault::OutOfMemory` there and the report names the right
isolate, which is why this is a parity row rather than a V8 row. It skips
wherever that lever does not reach, exactly as the rule 9 case does and for the
same reason; see "Where frame exhaustion can be provoked from a test" above.

Getting there by two different routes is worth recording, because it is the
reason the two agree without either backend being bent to it. SpiderMonkey's
`JS::SetOutOfMemoryCallback` sits under every internal out-of-memory the engine
has, and the backend was already reporting frame exhaustion through
`JS_ReportOutOfMemory`, so the fault arrives without the frame code knowing
anything about faults. V8 has a hook for its *own* heap and nothing that covers
ours, so that backend raises this one by hand where it already throws. Neither
is a workaround: each is the natural place in that engine, and the observable
behaviour is identical.

**A genuine heap exhaustion is still out of reach** and the suite does not
pretend otherwise. Filling a real heap takes as long as it takes, the result
differs by engine, and on V8 with no heap-limit callback installed it ends the
process. What is asserted instead is the thing an embedder can actually do about
it, which is the ceiling case above.

**`EngineFault::Fatal` is tested outside the suite, one process per backend**
(`<backend>.fatal-reaches-the-fault-handler`). The kind ends the process by
design (decision 28), so it cannot be a case among others; each backend gets an
executable that installs a handler, provokes the engine's own fatal path, and
prints the report, and CTest passes on that line rather than on an exit code.
Neither is faked, and neither is reachable through the public API without
undefined behaviour, so each reaches past it: the V8 one asks the native isolate
(through `unibind/interop/v8.h`) for a handle with no `v8::HandleScope` open,
which V8 refuses through its fatal-error hook in a release build; the
SpiderMonkey one expands the engine's own `MOZ_CRASH` macro, compiled the way
the engine compiles it. A `DCHECK` or `MOZ_ASSERT` needs a debug engine and a
real engine bug to fire, so that route shares the wiring but not the test. What
*is* pinned inside the suite is
the other half, and it is the half that would bite: `faults: nothing is reported
while nothing is going wrong` fails if either backend's fatal wiring fired on
ordinary work, and a script's own `throw` is in that case on purpose, because a
JavaScript exception arriving on this channel would be the easiest mistake to
make and the hardest to notice.

**The handler's own rules are held by construction rather than by assertion.**
It may not allocate, and `ub_test::RecordEngineFault` does not - fixed buffer,
atomic counter, no `std::string` anywhere near it - which is why the
out-of-memory case can arm an allocation failure and record the report it
provokes in the same breath. A handler that allocated would not fail an
assertion here; it would fail somewhere else, later, which is exactly why the
rule is written into the header rather than left to be discovered.

## What the undecided areas need before they can be asserted

Written down here rather than left in a commit message, because each one is a
requirement on the API rather than a test that has not been written yet.

- **`Context` has no control over cross-realm access.** The sandbox works
  around V8's access check on a foreign global by entering the inner realm in
  every hook, which is fine - but an embedder who wants two realms to be
  *mutually* readable (V8's shared security token) cannot ask for it, and an
  embedder who wants a realm walled off from another cannot ask for that either.
  Whichever way it is decided, it is currently unexpressible rather than
  decided, and the workaround only looks like a style choice.

Two entries that used to be here are now answered, and are recorded because the
answers are what the cases are written against:

- **`workerThreads` is observable.** `Platform::WorkerThreads()` reports the
  count in effect, so `isolate: what the engine did with the worker-thread hint
  can be read back` asserts the shape - readable, self-consistent, unchanged by
  running script - and never a number. Empty means the engine would not say,
  which is not zero.
- **A stopped isolate stays stopped.** The two accounts of termination are
  reconciled: the sticky bit is the *library's*, on both engines, because
  neither has one that survives the unwind. `termination: a handler does not
  swallow a stop the way it swallows an exception` now asserts it rather than
  reporting it, through `Reset`, through the handler closing, and up to
  `CancelTerminateExecution` - which makes that case the only thing holding
  either backend to the promise.

### Four things that cannot be cases among others

Two of them would take the process down rather than fail. The other two are
about the *process* itself: the suite runs under a `Platform` that is already
up, a second one is a precondition violation rather than a failure, and every
interesting question about bring-up is about a process that has no platform yet
or has finished with the one it had. Each gets a process of its own and CTest
carries it.

| test | what it is | expected |
|---|---|---|
| `<backend>.checked.using-a-handle-after-its-scope-closed-is-diagnosed` | a stale handle, deliberately: diagnosing it means dying. Debug only. | **fail** |
| `<backend>.global-identity-without-a-scope` | decision 16's load-bearing clause - comparing two `Global`s with **no `HandleScope` open at all**. A backend that gets it wrong makes a handle with no frame to make it in, and the engine ends the process, which is why it cannot sit in the suite: it would take every case after it down and the parity matrix with them. | pass |
| `<backend>.platform-is-the-gate-on-an-isolate` | the whole platform lifecycle - before one exists, while one does, and after it has gone - holding `IsInitialized()`, `Isolate::New` and `WorkerThreads()` to the same story at each point. | pass |
| `<backend>.worker-threads-belong-to-one-platform` | the same lifecycle with a worker-thread count actually asked for, which is what makes the cache observable. | pass |

The in-suite case next to the second asserts as much as can safely be asserted
there - every frame that ever held the value closed, and a collection through
since. The last two are the subject of the next section.

## A bring-up that fails: what can be asked, and what cannot

Four bugs of one shape were fixed in the backends at once - **an engine call
that reported failure, whose result was discarded, so the library reported
success and handed out something unusable**. V8's `Initialize` and a null
`NewDefaultPlatform`; SpiderMonkey's `JS_Init`, checked with an `assert` that
compiles out of exactly the build where the failure matters;
`JS_AddInterruptCallback`, which returns `bool` and was ignored; and a cached
helper-thread count that outlived the `JS_Init` it belonged to. None was found
by a failing test. So the reachable half is pinned now, and the unreachable half
is written down here rather than assumed.

The behaviour they were fixed to, which is what the cases are written against: a
failed bring-up **unwinds what came up**, leaving an object that exists and did
*nothing* rather than one that did half; `IsInitialized()` is false; the
destructor does not shut down what was never started; and a *second* `Platform`
while one is alive stays a precondition violation - an assert - deliberately not
conflated with a failure.

### What is pinned

**That the flag is read.** `unibind/isolate.h` says `IsInitialized()` is the failure
channel and that `Isolate::New` refuses when it is false, and that is the half of
items 1 and 2 a test can reach even where bring-up cannot be made to fail:
`platform-is-the-gate-on-an-isolate` asks before any `Platform` exists (nothing
up, so `Isolate::New` answers empty), while one does (an isolate, and script
running in it), and after it has gone (empty again). A backend that set the flag
unconditionally, or a destructor that left it set, fails the first or the last.

**That a figure does not outlive the platform it came from.** That is item 4, and
it is the cheapest real coverage of the set because it needs no injection at all.
`Platform::WorkerThreads()` is cached on both backends, and the same case asserts
the cache is empty on either side of a platform's life. SpiderMonkey reports 4
while up and nothing either side, so the assertion has something to bite on
there; V8 with no explicit request reports nothing throughout, so it pins the
shape rather than a value.

### Where the backends disagree, and what is reported instead

`worker-threads-belong-to-one-platform` asks for a count, which is what makes the
cache visible, and asserts only what both engines promise - no figure before
there is a platform, and the same answer twice while there is one. What happens
to an explicitly requested figure *after* the platform has gone is **reported**,
because the two do not agree:

- SpiderMonkey clears it in `~Platform` (that is the fix for item 4) and answers
  empty. It also ignores the request itself - it asks for 1 and reports 4 -
  which is the documented hint behaviour, not a failure.
- V8 keeps the figure. It is the number that was *asked for* rather than one read
  out of the engine, and nothing clears it, so after the platform is gone
  `WorkerThreads()` still reports it. That is a figure about a platform that no
  longer exists, and the header says the answer is the count "actually in
  effect".

So the case prints both and asserts neither. Asserting the SpiderMonkey answer
would leave the suite red on V8 for a backend bug rather than a promise, which
is the one thing a case may not be weakened into; asserting the V8 answer would
be writing the bug down as the contract.

### What is not pinned, and why

**Making a bring-up actually fail.** The suite's one lever is
`FailNextAllocations` in `tests/support/allocations.cpp`, and pointed at
`Platform` construction or `Isolate::New` it does not produce a refusal on either
engine. Measured rather than assumed, because the trap below makes a quiet pass
worthless:

- **V8, `Platform` construction:** one injected failure segfaults, at every count
  tried. V8's bring-up is not written to survive an allocation failure from
  outside its own allocator.
- **V8, `Isolate::New`:** the injector fires exactly once and `std::bad_alloc`
  comes back out of `Isolate::New`, which is *not* the documented empty answer -
  though the thread is not poisoned by it and a later `Isolate::New` succeeds.
  The failure lands on the library's own `make_unique`, before the engine is
  reached at all, so it says nothing about what V8 would do.
- **SpiderMonkey, `Platform` construction:** the injector fires **zero** times.
  That is the `mozilla/cxxalloc.h` trap in `docs/gotchas.md` doing exactly what
  it does: a replaced `operator new` is not the program's `operator new` in any
  translation unit that has seen a SpiderMonkey header, so nothing was injected
  and a pass here would have meant nothing. The fired count is what says so.
- **SpiderMonkey, `Isolate::New`:** the process dies with
  `STATUS_STACK_BUFFER_OVERRUN`, which is how an infallible allocation or a
  `std::bad_alloc` crossing a frame built without exceptions ends up. Not a
  refusal, and fatal, so it cannot be a case: it would take the rest of the run
  with it.

Two of the four outcomes are fatal and one is a silent non-event, so there is no
shape of this that could be gated into the suite. Closing it needs a lever the
*engines'* own allocators honour, which is a backend's to offer rather than a
test's to invent - the same conclusion frame exhaustion reached, and for the same
reason.

**A second `Platform` after the first has gone.** Item 4's literal scenario - a
second platform with a different pool reading the first one's figure back - is
unreachable, and not for the reason one would guess. SpiderMonkey survives it
perfectly well: `JS_Init` after `JS_ShutDown` comes back up, reports its pool and
makes an isolate. **V8 does not**: `V8::Initialize` after `DisposePlatform` is a
fatal V8 check (`current_state != V8StartupState::kPlatformDisposed`) and ends
the process. A case that made one would be green on one backend and dead on the
other, and choosing by backend is the thing a case may not do. What is asserted
instead is the same property one step earlier - the figure is gone when the
platform is - which is what makes the second platform's figure *its own*
whenever an engine allows a second one at all.

**The interrupt hook.** Item 3 is the most valuable of the four and the only one
with no test at all. `JS_AddInterruptCallback` appends into a vector with two
inline slots, allocated through `js_malloc` inside a translation unit where
`operator new` is already `moz_xmalloc` - so it is out of reach of the injector
three times over, and V8 has no equivalent registration to fail (its interrupt is
per-request and `RequestInterrupt` returns nothing). **That `Isolate::New` comes
back empty rather than handing out an isolate that cannot be terminated rests on
review, not on a test, and should be read that way.**

What *is* covered is the consequence, which is worth knowing because it is the
symptom an embedder would actually meet: on SpiderMonkey `TerminateExecution` is
`JS_RequestInterruptCallback` and nothing else, so an isolate that lost the hook
is an isolate whose runaway scripts never stop - and `termination: a script that
will not return is stopped from another thread` asserts that they do. A hook
that silently failed to install would fail that case. What would not be caught is
the isolate being handed out in the first place.

**An allocation failure in `Isolate::New` itself.** A fifth of the same shape,
found by reading rather than by a test and fixed the same way. SpiderMonkey's
`Isolate::New` built its `Impl` with `make_unique` and its `Isolate` with a
throwing `new`, so two things were wrong at once: `std::bad_alloc` escaped a
function whose header promises an *empty* answer, and if it escaped from the
second of them the `JSContext` was already made and owned by nothing -
`~Isolate` is what destroys one and there was no `Isolate` - so an entire
JavaScript heap leaked on the way out. V8's already used `new (std::nothrow)` and
unwound by hand; SpiderMonkey's now matches, and the
sequencing it relies on is standard (the allocation function runs before the
new-initializer, so a null result leaves the half-built `Impl` untouched and
ours to unwind).

**It rests on review**, exactly like the interrupt hook above and for the same
reason: the allocations that would have to fail go through `moz_xmalloc`, so
the suite's `FailNextAllocations` lever does not reach them - the
`mozilla/cxxalloc.h` trap in `docs/gotchas.md`, again. The measurements under
"Making a bring-up actually fail" are what say so: pointed at SpiderMonkey's
`Isolate::New` the injector does not produce a refusal, it produces
`STATUS_STACK_BUFFER_OVERRUN` and takes the run with it.

**A `Context`, `Script` or `Global<T>` outliving its isolate** is the other
lifetime finding of that pass, and it is *not* in this list: decision 27 made
it a rule and a checked build counts and diagnoses it, with a CTest test in a
process of its own expecting exactly that death. Like the use-after-scope check
it exists only where it can hold - `CONFIGURATIONS Debug`, and only with
`UNIBIND_HANDLE_CHECKS` - so a Release run neither checks it nor tolerates it.

## A Debug build is green, and what it took

CI runs the suite in Release on both engines and both architectures, which is
what the engine archives are built for, and in Debug against each engine's
debug build as well. A debug engine compiles in its own assertions - V8's
`DCHECK`s, SpiderMonkey's `MOZ_ASSERT`s - and the rule here is that one firing
is a bug until proven otherwise. Every one so far has been, and none of them
was visible in a release build, which is the reason the Debug run is not
optional:

- **An allocation failure injected into V8.** The ownership sweep walked its
  failure into V8's own `operator new` calls, and a `std::bad_alloc` thrown
  through an engine built without exceptions left V8's handle-scope level one
  deeper than its API check expects (`scope_level_ == ...
  handle_scope_data()->level`). The release engine carried on with the same
  corruption. The backend now makes its own allocations for a hand-over before
  it enters the engine and the sweep stays in front of the engine's - see
  "Ownership" above, and `docs/gotchas.md` for what that means for an
  embedder's own allocator.
- **An empty `std::vector` that could not fail politely.** Under the MSVC STL's
  iterator debugging even an empty vector allocates - its container proxy - in
  a default constructor that is `noexcept`, so a frame's first overflow slot
  running out of memory was `std::terminate` instead of the empty handle
  `docs/lifetimes.md` rule 9 promises. The V8 backend makes those vectors with a
  constructor that may throw.
- **A compile while a termination is unwinding**, which V8 asserts against and
  decision 15 asks for: see "Termination" above.
- **Eager blobs held to a release engine's proportions.** A debug V8's lazy
  blob is several times a release one's; "Eager compiles are seen through the
  blob" above has what the cases compare instead.
- **The use-after-scope check's provocation.** Its epoch mismatched only if a
  new frame landed on the storage the closed one had, and an unoptimised build
  gives two scopes different stack addresses, so the stale handle resolved
  through the dead frame's memory, which still read its old epoch - and the
  `WILL_FAIL` test passed its handle and exited cleanly. A frame now writes an
  epoch no handle carries as it closes, on both backends, so the check fires
  whether or not the storage was reused (`docs/lifetimes.md` section 9).
- **A debug SpiderMonkey that did not link.** Its library is built with
  `MOZ_DIAGNOSTIC_ASSERT_ENABLED`, which `js-config.h` does not record, and
  `JS::AutoAssertNoGC` is laid out by it - a context pointer and out-of-line
  members with it, an empty class with inline ones without. The link said only
  "duplicate symbol"; the object size was wrong too. The backend now defines it
  for a debug engine, from the one list of engine defines in
  `cmake/UnibindEngines.cmake`.
- **A script run in a realm that did not instantiate it.** A `JSScript` belongs
  to one realm, and "compile once, run in any realm" (decision 10) executed it
  in whichever was entered - a realm mismatch a debug engine asserts. A run in
  another realm now instantiates the stencil there.
- **A cross-compartment wrapper's realm, entered.** A wrapper has no realm, only
  a compartment; the backend now enters a global of that compartment, asks
  questions about the unwrapped object in its own realm, and wraps an
  interceptor's key list into the current compartment rather than reading it
  out of its own.
- **Strings read across zones.** Every realm was its own zone, and a string -
  which belongs to the isolate here - made in one was read in another: an atom
  marked white for the zone reading it, which a release engine's collector could
  later free under it. Every realm of an isolate is now in one zone, each still
  in a compartment of its own.
- **A stack quota under the engine's minimum margin.** `ThreadStackQuotaForSize`
  takes a tenth of the stack as its margin, which below 320 KiB is less than the
  32 KiB the engine needs to throw its "too much recursion"; small stacks now
  keep the whole minimum. `docs/spidermonkey.md` section 5.10 has all five.

## What building the sandbox found

The composition was written because each piece of it passed on its own and
nobody had put them together. Four things came out of doing so, and none of
them would have been found by another single-operation case.

### A foreign realm's global object is access-checked

**The finding, and the one that changes how a sandbox has to be written.** The
obvious sandbox holds the inner realm's global object and reads properties of
it from the interceptor - and on V8 that throws `TypeError: no access` before
any embedder code runs, because a global proxy belonging to another realm is
access-checked. An embedder using V8 directly answers this by giving the two
contexts the same security token; `ub::Context` has no such control, and
adding one would be a decision, not a fix.

The portable answer, and what `cases/sandbox_test.cpp` does, is that **every
hook enters the realm that owns the object before touching it**. That is one
line per hook and it costs nothing on the engine that did not need it.

The same check bounds decision 4 in a way the decision does not say: a value
made in one realm *is* a value in the other, but the inner realm's
`globalThis`, handed out, is an object the outer realm's script cannot read
through. An ordinary object made inside is fine. The suite asserts that the
global crosses as a value and deliberately asserts nothing about reading it.

### An error thrown inside a sandbox is caught by name, not by `instanceof`

The error object is built by the inner realm, so its constructor is the inner
realm's `SyntaxError`, and `e instanceof SyntaxError` in the outer realm is
false. `realms: instanceof does not cross a realm` already said this; what the
sandbox adds is that it is the *normal* case for a host that runs untrusted
source, not an exotic one. The portable question is
`e.constructor.name === 'SyntaxError'`, and that is what the case asks.

### Keys and identity agreed

The two things the proxy-backed interceptor was most likely to show through on -
object identity across repeated reads, and the set of keys an enumeration
produces - agree on both backends. `Object.keys(sandbox)` reports the same three
entries either way, so the case asserts containment (what the test put in is
there, and the prototype's `evaluate` is not) and reports the count, which is
what it would have to do anyway for a realm global whose built-ins are the
engine's business.

### Runaway recursion is not the same error

`isolate: runaway recursion is a RangeError, not a dead process` asks in script
what it caught: V8 says `RangeError`, SpiderMonkey says `InternalError`. The
portable property - and the one an embedder needs - is that script catches it at
all rather than the process dying, so that is asserted and the name is reported.

Also worth knowing, because it is a way to make a test crash rather than fail:
`stackLimitBytes` above the thread's own stack is not a limit. The engine will
happily recurse past it and the *thread* runs out first, which no engine can
turn into an exception. The cases measure at 128 KiB and 512 KiB on a 1 MiB
thread.

## A case that encodes a promise is left red, never weakened

A case written against what a header promises, run against a backend that does
not yet keep the promise, stays **red rather than weakened**: weakening it turns
a bug into coverage nobody has, which is the one thing this suite exists to
prevent. Nothing is in that state today - both backends pass every case - and
the two that were are worth recording, because the same technique found both and
neither would have been found by asking the API what it thought had happened.

- **A damaged blob reported as used** (V8). `UsedCodeCache()` came back true for
  a blob that had been bit-flipped, cut in half, or was never a blob at all. The
  script itself was fine, so the only thing wrong was the answer to "did this
  compile pay full price" - exactly the question the accessor exists to answer,
  and exactly the failure an embedder cannot otherwise see.
- **A blob for one source accepted for another** (SpiderMonkey). A stencil
  encoded from source A decoded cleanly when offered for source B, reported
  success, and **ran A**. The case caught it only because it checked what came
  out of `Run` rather than what `UsedCodeCache()` said; had it asserted the
  report alone it would have agreed with the bug.

That is the technique, not just the case: for anything the engine is trusted to
validate, assert on what *ran*. `code cache: a blob for other source is declined
rather than believed` and `code cache: a blob is keyed to its origin as well as
its source` both do, and the second reads the origin back out of a stack.

## The CPython backend's suite

`tests/python/` is the CPython backend's suite, and
[`tests/python/README.md`](../tests/python/README.md) is how to build and run it.
This is the account of what it asserts.

### Why parity does not apply

The shared suite is JavaScript source as much as C++: nearly every case hands a
script to `Evaluate` and reads back what it did. On a backend whose scripts are
Python those cases are syntax errors, and translating them would give a second
suite that only *looked* like the first - the case names would match and the
scripts would not, so a parity row would compare two different tests and call
them one. So the parity comparison leaves this backend out
(`tests/cmake/RunParity.cmake` skips it by name), `tests/CMakeLists.txt` builds
`tests/python/` instead when `UNIBIND_BACKEND=python`, and nothing pretends the
two suites are one.

What carries over is the discipline, unchanged: C++ against `ub::` only, no
CPython header and no CPython type anywhere in a case, and a case written against
what a header promises stays red rather than weakened. The difference is that a
case here may assert Python's behaviour - that a missing attribute is an
`AttributeError`, that `len()` of an object counts its keys - because those are
decisions this backend made (`docs/status.md` decisions 30-37) rather than
differences between engines to be written around.

### What it covers

271 cases, about 120700 assertions in a Release x64 run, and one stress case
that runs only when asked for (`-tc="stress:*" --no-skip`): it makes and
destroys a thousand isolates and prints the process's memory as it goes, which
must level off rather than climb. The
suite links `tests/support/allocations.cpp`, the shared suite's replacement of
the global allocation operators, so that the lifetime cases can count the
backend's own C++ heap exactly and make its allocations fail on purpose.

| area | file | notes |
|---|---|---|
| bring-up and scripts | `smoke_test.cpp` | the backend's name and version, a trailing expression as the completion value, statements before it running in the realm's globals, a script with none being `undefined`, a throw caught with its message |
| objects | `objects_test.cpp` | `unibind.Object` from both sides: one property whatever spells its key, a missing property `undefined` to C++ and an error to Python, `in` along the chain, delete, key order and filters, attributes from Python (strict) and from C++ (sloppy), an inherited read-only property refusing a shadowing write, accessors (This and Holder, a throw, a repr that does not run them), prototypes and their cycles, construction and iteration as a mapping, symbols and the protocol a well-known one names, Python subclasses and their descriptors, weak references and cycles; and the other shapes - a list grown by index with `None` holes and capped at 2^26, a tuple read-only, a dict as its items (the realm's globals among them), any other object as its attributes, and `dir` |
| functions | `functions_test.cpp` | borrowed arguments, every `ReturnValue` setter reaching Python as the value it names (an integral `double` as an `int`), `This()` as the receiver or the realm's globals, typed data, callable and not a constructor, calling and constructing what script wrote, a native throw caught in script, a C++ exception stopped at the boundary, nested handlers each owning its depth, a script value as data and its lifetime, a realm found after its `Context` was released, a function freed with its last reference |
| templates | `templates_test.cpp` | constants and `ReadOnly`, methods, accessors and inherited ones, nesting, one type per realm, statics on the type (and `ReadOnly`/`DontDelete` holding there), a callback's `IsConstructCall`, the plain call from C++, a constructor answering with an object, `HasInstance`, `Inherit` chaining prototypes and types, the sealed shape, a JavaScript-style iterator iterated from Python, a Python subclass constructing through the template |
| interceptors | `interceptors_test.cpp` | named and indexed hooks, declining, query/delete/enumerate, the two halves kept apart, a getter alone as a whole handler, a setter's discarded return, an enumerator with nothing to say, This and Holder, a throw that intercepts, dunder lookups kept away from a catch-all, a symbol key |
| classes | `classes_test.cpp` | construction with native state, the type with statics and a prototype, the plain call refused unless opted in, a class that can only be wrapped, a constructor that throws or declines, instance-template accessors, a Python subclass keeping and overriding methods (and `super()`), checked unwrapping and a wrong receiver, a prototype swap fooling nothing, several realms, an interceptor over every instance, `Symbol.iterator`, and ownership: a share handed back, the native going with the last reference exactly once, a cycle collected, survivors given back by `~Isolate`, the no-op deleter, a share outliving the isolate |
| termination, interrupts, jobs | `runtime_test.cpp` | a tight loop stopped from another thread, and every way a script might swallow the stop - `except BaseException`, a `finally` with its own loop and its other lines, nested calls, recursion, a generator, a builtin in an `except` - plus a stop remembered while idle, landing in the compiler, repeated, cancelled with nothing run; interrupts in order, from a callback, waiting out a stop, sampling before a stop, their throws cleared, many from many threads; posted and delayed work in every order the contract names |
| heap, stack, concurrency, lifetime | `runtime_test.cpp` | statistics that follow what a script allocates, a limit reported as the limit, `MemoryError` and a fault at the limit, the rescue callback, a declining one asked once per crossing, a limit belonging to its isolate; recursion on threads of several sizes, `stackLimitBytes` held to the thread's stack, native recursion and recursion through native and Python as `RecursionError`; isolates on several threads stopping, pumping and allocating at once, and running in parallel; teardown with work queued, while stopped, and racing a stop; and many isolates leaking nothing of the backend's own |
| natives and stops | `runtime_test.cpp` | a native stopping its own isolate, a native polling `IsExecutionTerminating` and the script stopping when it returns, a native called from a stopped script's `finally` not running, a stopped isolate refusing a native's call back into Python |
| promises | `promises_test.cpp` | asyncio in an isolate and the isolate's loop being current, a native promise made, settled once and read, rejections with and without an exception, continuations waiting for the pump, top-level `await`, script awaiting a native promise, rejection through a chain, following another promise, resolving with itself, futures and tasks as promises, continuations queued by continuations, a timer not due, a job settling one before the next job, stops in a continuation and while stopped, a stopped isolate refusing, teardown with tasks pending, isolates on several threads |
| binary data | `binary_test.cpp` | buffers copied and zero-filled, a buffer too large to allocate, `bytearray` and `bytes` as buffers, every element type both ways and every conversion at its boundaries, views that do not fit refused, views sharing a buffer, the script-side `TypedArray` and `DataView`, a buffer shrunk under a view, a conversion that shrinks it mid-write, an export pinning its size, a view keeping its buffer alive |
| structured clone | `serialization_test.cpp` | every primitive exactly, NaN and `-0` to the bit, a lone surrogate, containers and their keys, shared references and cycles, views sharing their buffer, what cannot be cloned failing the whole call, a plain object, blobs crossing isolates and threads, damaged and well-framed-but-foreign blobs refused, deep nesting on the heap, a graph owing nothing to the realm that wrote it |
| code cache | `codecache_test.cpp` | the build id, a round trip, scripts with and without a tail, no blob and bad blobs compiling the source, a blob for other source, a payload re-framed for other source, tracebacks from a cached script, a blob used in another isolate on another thread, top-level `await`, a script outliving its realm, a syntax error beside a foreign blob |
| the embedded standard library | `stdlib_embedded_test.cpp` | modules served by CPython's `FrozenImporter` with `sys.path` empty and no module having a file, packages with an empty `__path__`, what the build left out absent; `asyncio`, `json`, `re`, `ssl`, `sqlite3`, `decimal`, `collections`, `dataclasses`, `typing`, `pathlib` and `email` each doing something; isolates on six threads unmarshalling the one table at once; a traceback into it naming `<frozen json.decoder>` and the line; isolate start-up time, measured. Every case asks which standard library the run should see and checks that one, and CTest runs them twice more out of process: from a copy of the suite alone in an empty directory, and with `UNIBIND_PYTHON_HOME` set, which must win |
| the standard library | `stdlib_test.cpp` | every extension module built in and none loaded from a `.pyd`; the modules that refuse an isolate refusing cleanly, and their pure-Python fallbacks; `ssl`/`hashlib`, `sqlite3`, the three compressors, `socket`/`select`, `unicodedata`, `queue`, `uuid`, `zoneinfo`, `multiprocessing`, `winsound`; `asyncio.run`, a TCP echo over streams, and event loops in two isolates at once |
| handles, roots and realms | `lifetimes_test.cpp` | every handle one reference given back when its scope closes, ten thousand in one frame, escapes through frames, a handle as the only owner; frame and root exhaustion yielding empty handles with `MemoryError` and nothing leaked; `Global`s moved, duplicated, compared with no scope open, and reset; a realm surviving script that empties its globals four ways, and a copied globals dict confusing no later realm; a released realm brought back whole by a callback; `Context` reference counting, nested `ContextScope`s; ten thousand realms and many scripts leaving the C++ heap where it was; templates going with their realm; a `bytearray` too large to allocate refused without the `SystemError` CPython (3.12, and still 3.14) would print |
| natives and their calls | `lifetimes_natives_test.cpp` | a native in a cycle through containers and closures, one resurrected by `__del__`, one whose destructor gives back its own realm and root, destructors at teardown that run script reaching other natives or make new ones, one shared native in two isolates on two threads, an instance dying on a script's thread (given back on the isolate's), a receiver dropped by script mid-call or kept in a `Global`, a throw after building values, fifty levels of native into Python into native, interceptor hooks that let go of their object |
| teardown and threads | `teardown_test.cpp` | a second isolate refused and then allowed, isolates in sequence and on many threads giving back all they took (one small allocation per isolate kept, and no more), ten thousand rounds of churn in one isolate; a `threading.Thread` and raw `_thread` threads stopped quietly at teardown, a short blocking call waited for and a long one left behind with the isolate's thread free at once, `TerminateExecution` reaching a script's thread while an interrupt waits for the isolate's; stops inside a native holding handles, a class constructor, a destructor during collection and an asyncio task; the isolate's loop current again after `asyncio.run`, and `asyncio.run` refused inside top-level `await` |

### What it does not cover

- **The coercions.** `ToBoolean`, `ToString`, `ToNumber`, `ToInt32`,
  `LooseEquals`, `StrictEquals` and `SameValue` are decision 32's, and no case
  asserts them directly: they are exercised only where another case coerces an
  argument. Python's truthiness in `ToBoolean` and `str()` in `ToString` are the
  two a case should pin first.
- **`Symbol.asyncIterator` and `Symbol.hasInstance`.** Stored under their Python
  protocol names; what Python does with them on an instance - nothing - is not
  asserted (`docs/python.md` section 2).
- **A bring-up that fails.** A `Platform` is made once per process, before the
  suite, so a missing standard library cannot be provoked in it - and with the
  standard library embedded there is none to miss.
- **Any regular run.** The suite passes Release and Debug on x64 and Release on
  x86, run by hand; no CI job runs it.

### No case name has a `;`

`doctest_discover_tests` registers each case with CTest by name, and CMake
splits a name at `;` - the case becomes two CTest tests whose `--test-case`
filter matches nothing, and a filter that matches nothing passes. Two cases in
`runtime_test.cpp` had one and so never ran under `ctest`; they have been
renamed. `docs/gotchas.md` has the rule.
