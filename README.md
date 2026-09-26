# unibind (`ub::`)

One embedding API over several script engines, chosen at the link. You write
against `ub::` and compile once; whether V8, SpiderMonkey or CPython is
underneath is decided by which library you link, and nothing in your code - or
in what it compiled to - says which.

**The scripts are the engine's language, and that is not abstracted.** V8 and
SpiderMonkey run JavaScript; CPython runs Python. Your C++ - the classes,
templates, interceptors, callbacks and pumps you bind - compiles once and links
against any of the three. The *source text* you hand `ub::Evaluate` does not
carry over: a JavaScript program's scripts run on either JavaScript backend
unchanged, and on the CPython backend they are a `SyntaxError`. Choosing
CPython is choosing Python as your scripting language, not a faster way to run
the scripts you have.

```cpp
const ub::Platform platform;                 // process-wide, once
const auto isolate = ub::Isolate::New();     // one heap, one thread
const ub::HandleScope scope(*isolate);       // handles live here
const auto context = ub::Context::New(*isolate);
const ub::ContextScope entered(*context);

const auto result = ub::Evaluate(*context, "1 + 1");   // JavaScript or Python: both say 2
const int sum = result->To<ub::Integer>()->Int32Value();   // 2
```

| | |
|---|---|
| Public API | complete: values, objects, accessors, interceptors, symbols, classes with native state, exceptions, realms, promises and jobs, termination, binary data, structured clone, compiled-code caching, engine-fault reporting, a Chrome DevTools inspector |
| V8 15.6 | JavaScript. Implements all of it |
| SpiderMonkey 153.3.0esr | JavaScript. Implements all of it except two things its engine does not have: the near-heap-limit hook - a call to that one does not link, on purpose - and the inspector, which links and answers `Supported()` with false |
| CPython 3.12.13 | **Python.** Implements all of it except the inspector, which links and answers `Supported()` with false; its engine-fault reporting covers running out of heap and a failed bring-up, not a crash inside CPython. Where Python and JavaScript disagree - `None` is `undefined`, a missing attribute is an error to Python - the choice is written down in [`docs/python.md`](docs/python.md) |
| Tests | the JavaScript suite, written once against `ub::`: 413 cases, green on both JavaScript backends, every case compared backend against backend with no divergences. The CPython backend has a suite of its own, written the same way with Python as the script language: 269 cases, plus one opt-in stress case |
| Not here | cross-realm access control - see [Limits](#limits) |

> **Read [`docs/gotchas.md`](docs/gotchas.md) before you lose a day to one of
> them.** It is ninety-odd traps indexed by what you were doing when it bit you,
> and it opens with nineteen you will not diagnose from the symptom: eighteen
> give a *wrong answer and no error at all* - a `TypeError` that arrives as a
> `SyntaxError`, a cached blob that runs a different script than the one you
> asked for, a promise continuation that simply never happens, an empty list
> that a Python callback treats as false - and the
> nineteenth gives a loud error that blames something else entirely. Ten
> minutes there is the best-value reading in this repository.

**The JavaScript engines do not have the same rules, and the stricter one is
what this API is shaped by.** SpiderMonkey roots a GC value through `JS::Rooted`, which
must live on the stack, must be destroyed in reverse order of construction, and
cannot be moved or copied or put in a container - and its collector *moves* what
it roots, so a value copied out of a root is not merely possibly-freed but
possibly-stale. V8 has none of those rules. An abstraction only V8 could
implement honestly would be worth nothing, so what is here is what both can
implement honestly rather than what either would have designed alone - and the
handle model is where that bites hardest. CPython, the third, refcounts and
never moves anything, which is the other end of the same scale; the model took
it without a change to a public header, and a frame there is simply an array of
strong references.

The rest of [`docs/`](docs/) is the design: [`lifetimes.md`](docs/lifetimes.md)
for the handle model everything else follows from,
[`status.md`](docs/status.md) for the thirty-seven decisions a backend author has
to know, [`testing.md`](docs/testing.md) for where the engines differ and
what the suites assert instead, [`spidermonkey.md`](docs/spidermonkey.md) for
what writing the second backend cost, [`python.md`](docs/python.md) for the
third - a different language behind the same API, and everything that decided -
and [`licensing.md`](docs/licensing.md) for what you owe whom when you ship this.

---

## Contents

- [What you have to supply](#what-you-have-to-supply)
- [Building the library](#building-the-library)
- [Using it from your project](#using-it-from-your-project)
- [The API, from nothing to a working embedding](#the-api-from-nothing-to-a-working-embedding)
  - [1. Platform, isolate, context](#1-platform-isolate-context)
  - [2. Handles and scopes](#2-handles-and-scopes)
  - [3. Values and objects](#3-values-and-objects)
  - [4. Native functions](#4-native-functions)
  - [5. Binding a native class](#5-binding-a-native-class)
  - [6. Calling script, and catching what it throws](#6-calling-script-and-catching-what-it-throws)
  - [7. Interceptors: an object that answers for every property](#7-interceptors-an-object-that-answers-for-every-property)
  - [8. Promises, jobs, and the drain](#8-promises-jobs-and-the-drain)
  - [9. Stopping a runaway script](#9-stopping-a-runaway-script)
  - [10. When the engine itself is in trouble](#10-when-the-engine-itself-is-in-trouble)
  - [11. Chrome DevTools](#11-chrome-devtools)
- [Rules an embedder must know](#rules-an-embedder-must-know)
- [What it costs](#what-it-costs)
- [Gotchas worth knowing before you start](#gotchas-worth-knowing-before-you-start)
- [What the name claims](#what-the-name-claims)
- [Limits](#limits)
- [Layout](#layout)

---

## What you have to supply

unibind is the abstraction, not the engine. A consumer brings:

| | |
|---|---|
| **The engine** | a prebuilt static V8 (`include/` + `v8_monolith.lib`) or SpiderMonkey (`include/` + `spidermonkey.lib`), of the architecture you are building, which **you link yourself**. Building *this tree* no longer needs one - the build fetches the pinned version (see below) - but an installed prefix ships unibind's library and not the engine's, so a consumer supplies and links it. `unibind.props` and the CMake package already know the path the prefix was built against and the system libraries that go with it. See [`dependencies/README.md`](dependencies/README.md). CPython is the exception to where it comes from: vcpkg builds it, static and `/MT`, and it needs its pure-Python standard library on disk at run time ([`docs/python.md`](docs/python.md) section 10). |
| **x86 (Win32) or x64** | both, built and tested. A prefix is installed for one of them: the library, the engine and the generated `config.h` all have to agree, and `config.h` names the architecture in the ABI tag so that mixing them is LNK2038 rather than corruption. The *engine* is the choice that is not baked in - your objects link against either backend. |
| **The static CRT** | `/MT`, or `/MTd` against a debug engine tree. The engines link it; a `/MD` consumer fails at link with the MSVC STL's own `RuntimeLibrary` mismatch. |
| **MSVC toolset 14.44 or newer** | SpiderMonkey's floor, not a preference: its STL headers call helpers that ship in that toolset's `libcpmt.lib`, and an older one fails with undefined `__std_*`. The V8 tree here is built the same way. |
| **C++23, compiled by clang-cl** | `/std:c++latest` on MSBuild, `cxx_std_23` from CMake - and the **ClangCL toolset**, not MSVC's `cl.exe`. This is a constraint, not a preference: `cl.exe` instantiates `std::optional<Local<T>>` while `Local<T>` is still being defined and fails with C7637 and a cascade behind it. clang-cl accepts the headers, and the whole tree, its CI and the example are built with it. |
| **64-bit host tools** | `PreferredToolArchitecture=x64`. This is not a performance preference; see [Gotchas](#gotchas-worth-knowing-before-you-start). |

The engine's *headers* are not among them: your code includes no engine header,
which is the whole point, so the engine's own required defines
(`V8_GN_HEADER`, `STATIC_JS_API`, `XP_WIN`) applied when the backend was
compiled and do not apply to you.

## Building the library

```powershell
cmake --preset v8                                  # or --preset spidermonkey
cmake --build build/v8 --config Release --parallel 1
ctest --preset v8
```

Six presets, one per engine and architecture: `v8`, `spidermonkey`, `python`,
`v8-x64`, `spidermonkey-x64` and `python-x64`. The unsuffixed ones are x86,
which is what everything here was first measured in.

A fresh clone builds with nothing placed by hand. Configuring fetches the
pinned engine build into `dependencies/` when one is not already there - a
little under 300 MB for V8 and a little under 150 MB for SpiderMonkey, the exact
figure depending on the architecture, and
[`dependencies/README.md`](dependencies/README.md) lists all four. It happens
once per version *and* architecture, unpacked into a staging sibling and renamed
into place in one move, so an interrupted download cannot be mistaken for a
complete one.

Point it at an engine you already have and nothing is downloaded:
`-DUNIBIND_V8_DIR=...` / `-DUNIBIND_SPIDERMONKEY_DIR=...`, or `-DUNIBIND_FETCH_ENGINES=OFF`
to refuse the fetch outright and be told what to unpack where.
`UNIBIND_V8_VERSION` / `UNIBIND_SPIDERMONKEY_VERSION` pick the version, and a version
bump repoints the tag, the asset and the directory together rather than
silently reusing the old library.

### The CPython backend

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset python-x64                          # or --preset python
cmake --build build/python-x64 --config Release --parallel 1
ctest --preset python-x64
```

Nothing is downloaded into `dependencies/` for this one. CPython comes from
vcpkg - the `python` feature of `vcpkg.json`, which the root `CMakeLists.txt`
turns on only for this backend, through an overlay port in `cmake/vcpkg-ports/`
that builds it as a static `/MT` library with the standard library's extension
modules compiled in. **The first configure of a triplet builds CPython from
source**, and with it OpenSSL, libffi, SQLite, expat, liblzma and bzip2: about
twenty minutes on a 32-thread machine, nearly all of it OpenSSL and libffi, and
longer on a smaller one. After that vcpkg's binary cache makes it seconds.
`-DUNIBIND_PYTHON_DIR=...` points at a static CPython you already have, with
vcpkg's installed layout, and vcpkg is then not asked for one.

CPython's pure-Python standard library (`Lib/`) is compiled into the backend as
well: the build byte-compiles it with vcpkg's own interpreter for the triplet and
embeds it as CPython's frozen-module table, so **the built program is one
executable with nothing to ship beside it**. A directory still wins when there is
one, so that a developer can run against sources on disk: `UNIBIND_PYTHON_HOME`,
or a `python-stdlib` directory beside the executable.
`-DUNIBIND_PYTHON_EMBED_STDLIB=OFF` reads `Lib/` from disk instead ([`docs/python.md`](docs/python.md) section 10.3).

This preset also builds [`examples/python_repl`](examples/python_repl/README.md),
an interactive Python prompt whose host bindings exercise most of the binding
API; `ctest -L example` runs its checks.

The number `ctest` prints is a little larger than 413 and depends on the tree,
because it registers the suite's cases *and* a few things that cannot be cases
among others: the whole suite again in one process, four checks that each need
a process of their own (five on V8, which adds `unibind/interop/v8.h`'s) (plus two more in a Debug build, which are the two
checked-build deaths), the benchmark, and the
cross-backend `parity` comparison (which only compares what has actually been
built). **413 cases is the figure that means the same thing everywhere** - it is
what the test binary itself reports, on either backend. The assertion count is not: a case may assert a
different number of times on each engine, so V8 counts 10999 and SpiderMonkey
10472, and neither number is the one to compare a run against.

The CPython backend does not run that suite: its cases are JavaScript source as
much as C++, and the parity comparison leaves this backend out. It runs
`tests/python/` instead - **269 cases**, the figure `unibind_python_tests.exe`
reports, registered with CTest one per case under `python.` plus the whole suite
in one process and two runs of the embedded standard library's cases out of
process, one opt-in stress case that runs only when asked for by name, and seven
more tests for the example REPL under the label `example`. [`tests/python/README.md`](tests/python/README.md) says what it covers.

CI pins `windows-2022` and MSVC **14.44** on purpose: that is the toolset both
engine archives were built with, and therefore the one a consumer links
against. Following `windows-latest` would test a toolchain nobody chose. A
non-blocking canary does build on `windows-latest` - Visual Studio 2026, MSVC
14.51 - and currently passes, which is how the pin will eventually be moved.

Each JavaScript engine's workflow runs the suite three times, as separate jobs: x86 and x64
in Release, because the handle is a different size in the two, the backends'
frames are different sizes, and the x64 V8 archive brings a different
allocator - so one of them passing says nothing about the other; and x86 in
Debug against the engine's debug build, whose assertions have caught backend
bugs no release engine reports (`docs/testing.md`).

The CPython backend has no workflow yet: its suite and the example's checks are
run locally - Release and Debug on x64, Release on x86 - before a change lands.

Then install a prefix for consumers:

```powershell
cmake --install build/v8 --config Release --prefix C:\unibind
```

```
C:\unibind\
  include\unibind\*.h          the public API
  include\unibind\config.h     generated, and the same for every backend
  lib\unibind_backend_v8.lib   one engine, as a link input
  msbuild\unibind.props        for a .vcxproj
  lib\cmake\unibind\           for find_package(unibind)
```

**The backend is chosen at the link, not at the compile.** Nothing a consumer
compiles differs between engines: no public header names one, and the generated
`config.h` describes the architecture and the handle layout and says nothing
about an engine. So installing the other backend into the **same prefix** adds
one library and rewrites an identical header, and one set of your object files
links against either. `examples/embed` does exactly that - one object library,
two executables - and both run.

Installing the other *architecture* into the same prefix is not: one library
name per backend, and nothing in the layout to tell an x86 one from an x64 one.
Install a prefix per architecture. Reaching for the wrong one is caught rather
than documented - `unibind.props` and `find_package(unibind)` both check, and the
generated `config.h` carries the architecture in its ABI tag, so an object that
got past both fails to link.

A python prefix installs the same way (`cmake --install build/python-x64 ...`)
and adds `lib\unibind_backend_python.lib`. Like the other two it does not copy
the engine: its package file names the vcpkg prefix the build used - by default
`build/python-x64/vcpkg_installed/x64-windows-static`, inside the build tree - for
`python312.lib` and the six libraries beside it, and `UNIBIND_PYTHON_DIR`
relocates it. The pure-Python standard library is in
`unibind_backend_python.lib` itself, so a program linked against the prefix needs
no `Lib/` either.

## Using it from your project

You do not have to build this tree to use it. Every `vX.Y.Z` tag publishes a
prefix per architecture and flavor on the
[releases page](https://github.com/ResurrectedTrader/unibind/releases), named
`unibind-X.Y.Z-<x86|x64>-<release|debug>-msvc14.44.zip`, with a `.sha256` next
to each. One archive holds the headers, `config.h`, and **both** backend
libraries, so the same objects link either engine. `engines.txt` in the archive
names the engine versions those libraries were compiled against. The engines are
not included: fetch the matching ones from the releases that
`cmake/UnibindEngines.cmake` names, and point `UnibindV8Dir` /
`UnibindSpiderMonkeyDir` (or `UNIBIND_V8_DIR` / `UNIBIND_SPIDERMONKEY_DIR`) at
them. The `debug` flavor links the engines' debug builds and `/MTd`. The
published archives hold the two JavaScript backends; a prefix with the CPython
backend is built from this tree.

### MSBuild (`.vcxproj`)

One line, in the property-sheet slot every C++ project already has:

```xml
<ImportGroup Label="PropertySheets">
  <Import Project="C:\unibind\msbuild\unibind.props" />
</ImportGroup>
```

That sets the include directory (the public headers and the generated
`config.h` beside them), and the link inputs - `unibind_backend_<engine>.lib`,
the engine's own library, and the fourteen-odd system libraries it was built
against, which is the list nobody should be assembling by hand. Switching
`UnibindBackend` changes only the second list.

Properties you may set before the import:

| | |
|---|---|
| `UnibindBackend` | `v8`, `spidermonkey` or `python`. Defaults to whichever the prefix holds, preferring them in that order. |
| `UnibindRoot` | the prefix. Defaults to the props file's own parent, so normally unset. |
| `UnibindV8Dir`, `UnibindSpiderMonkeyDir`, `UnibindPythonDir` | where your engine lives - for CPython, vcpkg's installed triplet directory. Defaults to what the prefix was built against. |

Your project still has to say three things for itself, because they are decided
before any property sheet is imported: the `Platform` the prefix was installed
for, `MultiThreaded` (or `MultiThreadedDebug`), and - on `Win32` -
`PreferredToolArchitecture=x64`. The props file checks all three and fails with
a sentence rather than letting the link fail with something unreadable. [`examples/embed/embed.vcxproj`](examples/embed/embed.vcxproj)
is a complete, working consumer: under a hundred lines, of which one is unibind.
[`examples/README.md`](examples/README.md) is the command that builds it, both
ways, against a prefix you installed.

### CMake

```cmake
find_package(unibind REQUIRED)            # or COMPONENTS spidermonkey, or python
target_link_libraries(app PRIVATE unibind::unibind)
```

`UNIBIND_V8_DIR` / `UNIBIND_SPIDERMONKEY_DIR` / `UNIBIND_PYTHON_DIR` relocate the
engine if the prefix was moved to a machine where it lives elsewhere.

Three targets, and the split is what makes one compile serve every engine:

| | |
|---|---|
| `unibind::headers` | the public headers and `config.h`. Names no engine. |
| `unibind::backend_v8`, `unibind::backend_spidermonkey`, `unibind::backend_python` | one engine, as a link input. One per backend the prefix holds. |
| `unibind::unibind` | the headers plus one backend - what a program that links one engine wants, and the only one most consumers name. |

```cmake
find_package(unibind REQUIRED COMPONENTS v8 spidermonkey)

add_library(app_objects OBJECT app.cpp)
target_link_libraries(app_objects PRIVATE unibind::headers)   # compiled once

add_executable(app_v8 $<TARGET_OBJECTS:app_objects>)
target_link_libraries(app_v8 PRIVATE unibind::backend_v8)

add_executable(app_sm $<TARGET_OBJECTS:app_objects>)
target_link_libraries(app_sm PRIVATE unibind::backend_spidermonkey)
```

One *program* still links one engine - two of them in one executable is two
copies of every engine symbol - but building both from one set of objects is an
ordinary thing to want, and it is how `examples/embed` proves the promise.

### The config.h that goes with the library

`include/unibind/config.h` is **generated at configure time** and carries the
handle layout and the storage sizes of `HandleScope`, `TryCatch` and
`ContextScope`. A consumer compiled against a copy that disagreed with the
library it calls would lay those types out one way while the library reads them
another: no compile error, no link error, just corruption.

What prevents it is a `#pragma detect_mismatch` naming every value in the
header, so an object compiled against a different one is **LNK2038 at link
time**, which is the failure you want.

**What the tag does *not* carry is the backend**, and that is deliberate rather
than an omission. An object compiled against these headers is the same object
whichever engine it is linked with, so a tag naming the engine would refuse a
legitimate cross-backend link while catching nothing a real mismatch would not
also trip. What is left is the set a difference in which really is corruption:
the architecture, the handle layout, and the storage budgets.

The rule the tag therefore stops enforcing is enforced in the build instead: no
public header may name the backend. `tools/headers_only/backend_neutral.cmake`
reads them and fails the build if one does - it cannot be a compile error,
because `#if UNIBIND_BACKEND_V8` against a macro nobody defines is silently
false rather than wrong, which is exactly the failure worth catching. The
storage budgets carry a rule of their own for the same reason: **each is the
maximum across every backend for that architecture**, not a per-backend figure,
so a future backend with a bigger frame raises the number for everyone rather
than getting one of its own.

---

## The API, from nothing to a working embedding

Everything below compiles, and
[`examples/embed/main.cpp`](examples/embed/main.cpp) assembles most of it into
one runnable program: the platform, the isolate, the realm, a bound class, a
native function, a script, a call back into script, an exception caught, and the
pump. Escaping a handle (section 2), interceptors (section 7) and termination
(section 9) are not in it - `tests/cases/` is where those run.

```cpp
#include "unibind/unibind.h"
```

There is one header. Everything is in `namespace ub`.

The script source below is JavaScript, because two of the three engines run
it. On the CPython backend every line of C++ here is unchanged and the strings
handed to `Evaluate` are Python - `"40 + 2"` happens to be both.
[`examples/python_repl/host.cpp`](examples/python_repl/host.cpp) is most of this
tour bound for Python scripts, and [`docs/python.md`](docs/python.md) is where
the two languages' semantics meet: what `undefined`, a prototype, a promise and
`new` turn into there.

Section 1 makes an isolate and a realm, so it spells them `*isolate` and
`*context` - the one is a `std::unique_ptr`, the other a `std::optional`. Every
section after it is written as the inside of a function that was handed
`ub::Isolate& isolate` and `const ub::Context& context`, which is what your own
code will look like.

### 1. Platform, isolate, context

```cpp
const ub::Platform platform;               // before the first isolate, after the last
```

One per process. It is an object rather than a pair of free functions because
that is the only shape an embedder cannot get wrong.

`Platform::BackendName()` says which engine you linked and
`Platform::BackendVersion()` which build of it - for a log line or a bug
report. Both are runtime questions because the engine is a link-time choice,
and neither is for branching on: the version string's shape is the engine's own
and this API promises nothing about it.

```cpp
const auto isolate = ub::Isolate::New();   // std::unique_ptr<Isolate>, may be null
if (isolate == nullptr) {
    return 1;                               // no heap - including: this thread already has one
}
```

An isolate is one JavaScript heap, bound to the thread that made it. **At most
one is alive per thread**; one after another is fine. That is SpiderMonkey's
rule, kept on both backends so a program cannot work on one and not the other.

```cpp
const ub::HandleScope scope(*isolate);     // see the next section
const auto context = ub::Context::New(*isolate);   // std::optional<Context>
const ub::ContextScope entered(*context);  // this realm is current for the block
```

A `Context` is a realm: its own global object, its own built-ins. It is not a
handle and does not live in a scope - it is reference counted and outlives the
call that made it, which is the point of a realm. An isolate can hold any
number.

```cpp
const auto result = ub::Evaluate(*context, "40 + 2", {.resourceName = "example.js"});
if (!result) {
    // it threw, or the source did not compile: see section 6
}
const auto asInteger = result->To<ub::Integer>();
if (asInteger) {
    std::printf("%d\n", asInteger->Int32Value());
}
```

`Evaluate` compiles and runs. To run the same source many times, keep the
compiled form:

```cpp
const auto script = ub::Script::Compile(*context, source, {.resourceName = "loop.js"});
if (script) {                                   // empty if the source did not compile
    for (int i = 0; i < 10; ++i) {
        const auto value = script->Run(*context);   // empty if it threw
    }
}
```

A `Script` is an artefact, not a handle: it holds its own root, it outlives any
scope, and it may be run **in a realm other than the one it was compiled in** -
where it sees that realm's globals. Compile once, run in every sandbox.

Both engines compile a function's body on its first call. To keep the compiled
form across runs, compile everything up front and keep the blob:

```cpp
const auto eager = ub::Script::Compile(*context, source, {.resourceName = "app.js"},
                                       ub::CompileOptions::EagerCompile);
const auto blob = eager->CreateCodeCache();          // covers every function, not only those that ran
// next run:
const auto cached = ub::Script::CompileWithCache(*context, source, *blob, {.resourceName = "app.js"},
                                                 ub::CompileOptions::EagerCompile);
```

A blob made after a lazy compile covers the top level and whatever had run.
With `EagerCompile`, `CompileWithCache` uses a good blob as it is and compiles
eagerly when the blob is stale - which is the moment a fresh one is worth
making.

### 2. Handles and scopes

This is the part that will bite first, so it is the part to read twice. The
whole model is in [`docs/lifetimes.md`](docs/lifetimes.md); the rules are these.

**A `Local<T>` names a slot in the innermost open frame.** A `HandleScope` opens
a frame. Every handle made while it is open dies when it closes.

```cpp
{
    const ub::HandleScope inner(*isolate);
    const auto text = ub::String::New(*isolate, "hello");   // lives in `inner`
}   // `text` is gone. Reading it now is undefined behaviour.
```

**A scope is a stack object and closes in reverse order of opening.** `operator
new` is deleted and copy and move are deleted, so it cannot be heap-allocated,
returned, or made a member of something that is. That is not caution: on
SpiderMonkey the frame *is* a `JS::Rooted`, which has exactly those rules.

**Within its frame, a `Local` is an ordinary value.** Copy it, pass it, put it in
a `std::vector`. It is trivially copyable, and small: a frame pointer and a
32-bit ordinal, so 8 bytes on x86 and 16 on x64.

**To return a handle, escape it.** A `HandleScope` cannot hand anything to its
caller; an `EscapableHandleScope` can:

```cpp
ub::Local<ub::Object> MakePoint(ub::Isolate& isolate, const ub::Context& context, int x, int y) {
    ub::EscapableHandleScope scope(isolate);          // NOT const: Escape is non-const

    const auto point = ub::Object::New(context);
    if (!point) {
        return {};                                     // an empty handle, not undefined
    }
    (void)point->Set(context, "x", ub::Integer::New(isolate, x));
    (void)point->Set(context, "y", ub::Integer::New(isolate, y));

    return scope.Escape(*point);                       // into the caller's frame
}
```

Whether a scope can escape is decided when it is **opened**, not when `Escape` is
called - which is why there are two types. V8 has to reserve the parent's slot up
front; SpiderMonkey does not care. Escaping more than one handle from a frame is
allowed (the second and later ones cost a global root on V8, and nothing on
SpiderMonkey).

A bare `return local;` out of a function that opened its own scope returns a
dangling handle. Nothing makes that a compile error; a checked build (`Debug`,
`UNIBIND_HANDLE_CHECKS`) diagnoses it at the point of *use*.

**To outlive every frame, use a `Global<T>`.** A cached constructor, a callback
you were handed, a value a C++ object has to remember:

```cpp
class Handler {
   public:
    Handler(ub::Isolate& isolate, const ub::Local<ub::Function>& fn) : fn_(isolate, fn) {}

    void Fire(const ub::Context& context) {
        const ub::HandleScope scope(context.GetIsolate());
        const auto fn = fn_.Get(context.GetIsolate());       // back into the current frame
        (void)fn.Call(context, context.GlobalObject());
    }

   private:
    ub::Global<ub::Function> fn_;                          // move-only, one engine root
};
```

A `Global` is move-only; copying one is spelled `Duplicate()`, because a root
costs something and that should be visible. Two `Global`s can be two roots over
one value, so compare them with `StrictEquals` / `SameValue`, never by comparing
handles - and those need no open scope.

**An empty handle is not `undefined`.** A value-making operation that could not
grow the frame hands back an empty `Local`; `IsEmpty()` asks. It is the one
failure this model cannot make impossible, and it is deliberately not
`undefined`, because running out of memory must not arrive as a plausible value.

### 3. Values and objects

Everything that can fail returns `std::optional`, and an empty one always means
the same thing: *no value was produced*. Why is a second question -
`TryCatch::HasCaught()` for a script throw, `Isolate::HasPendingException()` for
an engine failure, `TryCatch::HasTerminated()` for a stop from another thread.

```cpp
const auto object = ub::Object::New(context);                    // optional<Local<Object>>
const auto name = ub::String::New(isolate, "widget");            // optional<Local<String>>, strict UTF-8
const auto line = ub::String::NewFromUtf8(isolate, fromSocket);  // lossy: bad bytes become U+FFFD
const auto ok = object->Set(context, "name", *name);              // optional<bool>
const auto back = object->Get(context, "name");                   // optional<Local<Value>>
const std::string text = back->ToString(context)->Utf8Value();    // "widget"
```

`String::New` is strict: bytes that are not UTF-8 make no string, rather than a
row of replacement characters where the caller thought it had text.
`String::NewFromUtf8` is V8's lossy decode, for bytes that are meant to be
repaired - each maximal invalid sequence becomes one U+FFFD, by the WHATWG rule
V8 follows, and the same string comes out of both engines because the repair is
done in the header. The conveniences that take text an embedder usually did not
write - `ReturnValue::Set(std::string_view)` and every way of throwing a fresh
error (`ub::Throw`, `CallbackInfo::Throw`, `Isolate::ThrowError`, `MakeError`) -
decode the same lossy way, as V8's do, so a stray byte in a file name costs a
U+FFFD and not the whole return value or error.

Widening is implicit and narrowing is checked:

```cpp
ub::Local<ub::Value> value = *name;             // Local<String> -> Local<Value>, free
const auto asString = value.To<ub::String>();    // optional: empty if it is not one
if (value.Is<ub::Function>()) { /* ... */ }
switch (value.Kind()) { case ub::ValueKind::Object: break; default: break; }
```

`ValueKind` answers *what can I do with this*, so a Date, a RegExp, a Proxy and a
typed array all report `Object`; arrays and functions have kinds of their own.
The more specific questions are `Is<T>()` / `To<T>()`.

V8's type predicates are there as well, spelled as V8 spells them -
`IsUndefined()`, `IsNullOrUndefined()`, `IsTrue()`, `IsInt32()`, `IsUint32()`,
`IsObject()`, `IsArrayBufferView()`, `IsPromise()` and the rest - because a
binding asks those questions constantly and ported code already asks them that
way. Each is a `Kind()` or an `Is<T>()` underneath, and they answer as V8's do:
`IsObject()` is true for an array or a function and false for `null`, `IsTrue()`
is false for `new Boolean(true)`, and neither integer question counts `-0`. The
one place they part from V8 is an `External`, which V8 counts as an object and
this API, on both backends, does not.

Arrays, keys, prototypes and attributes are where you would expect:

```cpp
const auto array = ub::Array::New(context, 3);
(void)array->Set(context, 0u, ub::Integer::New(isolate, 7));
const std::uint32_t length = array->Length();

const auto keys = object->GetOwnPropertyNames(context, {.includeSymbols = true});
(void)object->DefineOwnProperty(context, *name, *name, ub::PropertyAttribute::ReadOnly);
const auto prototype = object->GetPrototype(context);

// A property that runs native code on every read (and write, given a setter) -
// on this one object, where a template would put it on every instance. The
// callbacks are kept for the life of the isolate, as a template's are, so this
// is for an object made a bounded number of times, not one made on every call.
(void)object->SetAccessor(context, "level", &ReadLevel, &WriteLevel, ub::CallbackData::For(state));
```

Bulk numeric data goes across in one crossing rather than N:

```cpp
const std::array<float, 4> samples{1.0F, 2.0F, 3.0F, 4.0F};
const auto view = ub::TypedArray::New<float>(context, samples);   // a Float32Array
```

The bytes are copied, in both directions, and `unibind/value.h` says at length why
there is no borrowing alternative.

A typed array and a `DataView` are both an `ArrayBufferView`, as in V8, and code
that only moves bytes can take either:

```cpp
const auto header = ub::DataView::New(context, *buffer, 4, 12);   // no element width of its own
if (const auto any = value.To<ub::ArrayBufferView>()) {
    std::vector<std::byte> bytes(ub::ByteLength(*any));
    (void)ub::CopyBytes(*any, bytes);                              // its own range, not the buffer's
}
```

### 4. Native functions

A callback is a plain function pointer - there is no `std::function` anywhere in
a call path. Embedder state arrives through `CallbackData`, which is typed:

```cpp
struct Counters { int calls = 0; };

void Bump(const ub::CallbackInfo& info) {
    auto* counters = info.Data<Counters>();        // null if it was declared with another type
    if (counters == nullptr) {
        info.ThrowTypeError("no counters");
        return;                                    // return promptly after throwing
    }
    ++counters->calls;

    std::int32_t by = 1;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return;                                // the coercion ran script and it threw
        }
        by = *asInt;
    }
    info.GetReturnValue().Set(counters->calls * by);
}

Counters counters;                                 // must outlive every call
const auto fn = ub::Function::New(context, &Bump, ub::CallbackData::For(counters));
(void)context.GlobalObject().Set(context, "bump", *fn);
```

`info[i]` past the end is `undefined`, as script would see. A callback that
writes nothing to `GetReturnValue()` returns `undefined`.

`ReturnValue` has V8's full set of setters: a `Local`, a `Global` (returned as it
is now, and `undefined` if it is empty), `bool`, `double`, text, `SetNull`,
`SetUndefined`, `SetFalse`, `SetEmptyString`, and every integer width from
`int16_t` to `uint64_t`. An integer is returned as that integer when it fits in
an `int32_t` and as a Number otherwise - the nearest double, past 2^53, as V8
answers - so `Set(std::uint32_t{...})` is never ambiguous and never negative.

A function's data can instead be a *script value*, read back as `info.Data()` -
one native callback behind many functions, each closing over its own value, and
nothing for the embedder to keep alive: the value lives exactly as long as the
function.

```cpp
void Greet(const ub::CallbackInfo& info) {
    const auto greeting = info.Data().ToString(info.GetContext());  // this function's own string
    if (greeting) {
        (void)info.GetReturnValue().Set(greeting->Utf8Value() + ", world");
    }
}

for (const char* word : {"hello", "goodbye"}) {
    const auto fn = ub::Function::New(context, &Greet, *ub::String::New(isolate, word));
    (void)context.GlobalObject().Set(context, word, *fn);
}
```

**A function made this way is callable, not constructable.** `new bump()` is a
TypeError before the callback runs. Something `new`-able is asked for on purpose,
with a `FunctionTemplate` (callable *and* constructable) or a `Class<T>`
(constructable only, unless you opt into both).

### 5. Binding a native class

```cpp
struct Counter {
    explicit Counter(std::int32_t start) : value(start) {}
    std::int32_t value = 0;
};

std::unique_ptr<Counter> MakeCounter(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;
        }
        start = *asInt;
    }
    if (start < 0) {
        info.ThrowTypeError("a counter starts at zero or above");
        return nullptr;              // null after a throw declines the construction
    }
    return std::make_unique<Counter>(start);
}

void Increment(Counter& self, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(++self.value);
}

void ReadValue(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void WriteValue(Counter& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    if (const auto asInt = value.ToInt32(info.GetContext())) {
        self.value = *asInt;
    }
}

void HowMany(const ub::CallbackInfo& info) { info.GetReturnValue().Set(0); }

// Declared once per isolate; instantiated into any number of realms.
const auto counterClass = ub::Class<Counter>::New(isolate, "Counter");
counterClass.Construct<&MakeCounter>();
counterClass.Method<&Increment>("increment");
counterClass.Accessor<&ReadValue, &WriteValue>("value");
counterClass.StaticMethod("howMany", &HowMany);

const auto constructor = counterClass.GetConstructor(context);
(void)context.GlobalObject().Set(context, "Counter", *constructor);
```

Callbacks are **template** arguments, not runtime ones, so a method is one
indirect call - the trampoline that unwraps `this` - with nothing per method to
store or keep alive. A method whose receiver is not a `Counter` throws a
TypeError before your code runs.

`Method` and `Accessor` put their members on the prototype, once. Script that
copies an object property by property - `for...in` with `hasOwnProperty`,
`Object.keys`, `JSON.stringify` - sees only *own* properties, so a member it
has to see goes on the instance template instead, as V8's
`FunctionTemplate::InstanceTemplate()` does it:

```cpp
counterClass.InstanceTemplate().SetAccessor("value", &ReadValueUntyped, &WriteValueUntyped);
```

That costs per instance on SpiderMonkey, which defines the member on each
object as it is made.

Recovering the native is checked, and cannot lie:

```cpp
Counter* self = ub::Class<Counter>::Unwrap(someValue);   // null unless it is exactly a Counter
```

**A wrapper owns a *share* of its native**, not the native. So an object the
embedder also holds is ordinary rather than a lifetime puzzle:

```cpp
const auto shared = std::make_shared<Counter>(7);
const auto wrapper = counterClass.Wrap(context, shared);        // script gets one; you keep one
const auto another = counterClass.Wrap(context, shared);        // two wrappers, one native
auto share = ub::Class<Counter>::UnwrapShared(*wrapper);       // outlive the wrapper
```

The native goes when the last share does, whoever holds it - which may be you,
after the isolate is gone. A native that must not be destroyed by the engine is
handed over with a no-op deleter, said once at the call site:
`std::shared_ptr<Counter>(&mine, [](Counter*) {})`.

The same holds for what a constructor makes. `Construct` and `ConstructOrCall`
take a callback that returns a `std::unique_ptr<T>`, or one that returns a
`std::shared_ptr<T>` - for a native released through a deleter of its own, one
that counts live instances say, or one the embedder already holds and hands
script a share of. Either way the wrapper holds a share, and null after a throw
declines the construction.

### 6. Calling script, and catching what it throws

Calling a JavaScript function from native:

```cpp
const auto described = context.GlobalObject().Get(context, "describe");
const auto fn = described ? described->To<ub::Function>() : std::nullopt;
if (fn) {
    const auto argument = ub::String::New(isolate, "the answer");
    const std::array<ub::Local<ub::Value>, 1> arguments{*argument};
    const auto answer = fn->Call(context, context.GlobalObject(), arguments);
    if (!answer) {
        // it threw; see below
    }
}
```

Catching:

```cpp
{
    const ub::TryCatch caught(isolate);

    const auto result = ub::Evaluate(context, "throw new TypeError('nope')");
    if (!result && caught.HasCaught()) {
        if (caught.HasTerminated()) {
            return;                                   // a stop, not a throw: section 9
        }
        const std::string message = caught.Message(context).value_or("<none>");
        const auto where = caught.Location(context);       // script, line, column, the line's text
        const auto frames = caught.StackFrames(context);   // function, script, line
        const auto human = caught.StackTrace(context);     // the engine's own text
        const ub::Local<ub::Value> thrown = caught.Exception();
    }
}   // closing consumes what it caught
```

**Closing a `TryCatch` consumes the exception** unless `ReThrow()` was called. A
handler is a `catch` block, not an observer. The one thing it does not consume is
a termination: that continues outwards whatever you do, which is the whole point
of the facility.

Throwing from native is `info.Throw(kind, message)` inside a callback, or
`ub::Throw(isolate, kind, message)` outside one. Either way the throw takes
effect when you return to the engine, so return promptly and call nothing else
into the engine on the way out.

Two portability notes the suite had to learn: engines word their built-in
messages differently, so ask script `e.constructor.name` rather than parsing a
message; and `StackTrace` text is not parseable across engines, which is what
`StackFrames` is for. `Location` is the one to print a syntax error with: a
script that never compiled has no frame, but it does have a line.

### 7. Interceptors: an object that answers for every property

An interceptor is a catch-all for *any* property of an object - the thing a
sandbox or a scope object is made of. Each hook says whether it handled the
access; `Intercepted::No` means "carry on with the ordinary lookup", and that
third state is what keeps the object's own methods reachable.

```cpp
struct Scope {
    std::map<std::string, std::string, std::less<>> entries;
};

Scope* ScopeOf(const ub::PropertyCallbackInfo& info) {
    return ub::Class<Scope>::Unwrap(info.Holder());
}

ub::Intercepted ScopeGet(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    const auto asString = property.To<ub::String>();
    if (self == nullptr || !asString) {
        return ub::Intercepted::No;
    }
    const auto found = self->entries.find(asString->Utf8Value());
    if (found == self->entries.end()) {
        return ub::Intercepted::No;           // declining is what keeps `keys()` reachable
    }
    (void)info.GetReturnValue().Set(found->second);
    return ub::Intercepted::Yes;
}

ub::Intercepted ScopeSet(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                          const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    const auto asString = property.To<ub::String>();
    const auto asText = value.ToString(info.GetContext());
    if (self == nullptr || !asString || !asText) {
        return ub::Intercepted::No;
    }
    self->entries[asString->Utf8Value()] = asText->Utf8Value();
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> ScopeQuery(const ub::Local<ub::Name>& property,
                                                 const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    const auto asString = property.To<ub::String>();
    if (self == nullptr || !asString || !self->entries.contains(asString->Utf8Value())) {
        return std::nullopt;                   // empty means: not intercepted
    }
    return ub::PropertyAttribute::None;
}

std::optional<ub::Local<ub::Array>> ScopeEnumerate(const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    if (self == nullptr) {
        return std::nullopt;
    }
    const auto keys = ub::Array::New(info.GetContext(), static_cast<std::uint32_t>(self->entries.size()));
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t index = 0;
    for (const auto& [key, unused] : self->entries) {
        const auto name = ub::String::New(info.GetIsolate(), key);
        if (!name) {
            return std::nullopt;
        }
        (void)keys->Set(info.GetContext(), index++, *name);
    }
    return keys;
}

const auto scopeClass = ub::Class<Scope>::New(isolate, "Scope");
scopeClass.Construct<&MakeScope>();
scopeClass.Method<&ScopeKeys>("keys");         // reachable because the getter declines
scopeClass.SetHandler(ub::NamedPropertyHandler{.getter = &ScopeGet,
                                                .setter = &ScopeSet,
                                                .query = &ScopeQuery,
                                                .enumerator = &ScopeEnumerate});
```

Script then sees an object whose properties are yours: `s.answer = 42` lands in
the map, `'answer' in s` asks the query hook, `Object.keys(s)` asks the
enumerator, and `s.keys()` still finds the prototype's method because the getter
declined. An `IndexedPropertyHandler` is the same five hooks over `uint32_t`.

**A real sandbox adds a realm**, and one rule with it. Make a `Context` of its
own, hold its global object in a `Global<Object>`, and answer each hook out of
that realm - but **enter the realm before reading through an object that belongs
to it**:

```cpp
const auto scope = self->scope.Get(info.GetIsolate());
const ub::ContextScope inside(self->realm);       // required, not tidiness
const auto value = scope.Get(self->realm, property);
```

A value crosses realms freely, but a realm's *global object* is access-checked,
and reading a property of one while another realm is current fails - on V8 with
`TypeError: no access`, before any of your code runs. One line per hook, both
backends. [`tests/cases/sandbox_test.cpp`](tests/cases/sandbox_test.cpp) is the
whole composition: a second realm, an interceptor over every property, a `Global`
held across calls, and a prototype method that has to survive the interceptor.

### 8. Promises, jobs, and the drain

```cpp
(void)ub::Evaluate(context, "globalThis.ran = false; Promise.resolve().then(() => { ran = true });");
// ran === false
isolate.PumpJobs();
// ran === true
```

> **If you never call `PumpJobs`, promise continuations never run.** No error, no
> exception, no warning - `async` / `await` and `.then` compile and run and the
> continuation simply does not happen.

That is a documented requirement rather than a bug, and this sentence is the
difference between the two. Both engines *can* drain at moments of their own
choosing - V8's default policy does it after every call, SpiderMonkey's never
does - and unibind turns that off on both, so that a continuation runs at the same
observable moment whichever backend you linked. Call it on the isolate's own
thread with no native frame on the stack: after a `Script::Run`, at the bottom of
your loop.

An embedder settles its own promises:

```cpp
const auto promise = ub::Promise::New(context);      // hand this to script
// ... later, on the isolate's thread ...
(void)ub::Resolve(context, *promise, ub::Integer::New(isolate, 42));
isolate.PumpJobs();                                   // and its continuations run
```

`PostJob` queues work from any thread, and the same pump runs it - engine jobs
first, then one piece of posted work, then round again, so work that settles a
promise sees its continuations in the same pump, before the next piece of work.
Posting does not wake anything: **posted work runs when the script thread next
pumps, and nothing accelerates that.** Whatever is still queued when the isolate
is destroyed is dropped, not run.

`PostDelayedJob(callback, data, delayInSeconds)` is the same with a floor on
when: V8's `PostDelayedTask`, for the timer you would otherwise keep beside the
isolate. It still runs only at a pump, so an embedder that sleeps between pumps
decides how late it can be. Delayed jobs run in the order they fall due and join
the back of the queue at the first pump after that; a delay of zero, a negative
one or a NaN is no delay at all.

### 9. Stopping a runaway script

```cpp
std::thread watchdog([&isolate] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    isolate.TerminateExecution();            // the one call that may come from another thread
});

{
    const ub::TryCatch caught(isolate);
    const auto result = ub::Evaluate(context, "for (;;) {}");
    if (!result && caught.HasTerminated()) {
        // stopped. There is no exception value, no message, no stack.
    }
}

watchdog.join();
isolate.CancelTerminateExecution();          // on the isolate's own thread: this is the way back
```

The unwind is not catchable from script - `try`/`finally` does not stop it - and
a `TryCatch` does not swallow it. Until it is cancelled, every operation that
would run script fails, which is settled rather than inherited: one engine would
happily run the next script.

`RequestInterrupt` is the other way to reach a running script from another
thread, and is narrow on purpose. It runs your callback on the isolate's thread
while script is running, where it may make handles, read values and set flags -
and **may not call a function, run a script, or throw**. It is for a profiler
tick, or for a watchdog that has to read something on-thread before deciding to
terminate.

> **Both of these reach script, not your own C++. A watchdog cannot save you
> from your own blocking callback.**

Every checkpoint either engine has is inside script. A native that spins, blocks
on a socket or waits on a lock runs to completion first; if it can block for long
enough to matter, it polls `isolate.IsExecutionTerminating()` itself and returns.

### 10. When the engine itself is in trouble

Everything above reports a failure to whoever asked for something. This is the
other direction: the engine noticing that *it* is failing, at a moment nobody
asked it anything. Without it, a long-running embedder finds out by dying.

```cpp
void OnFault(const ub::EngineFaultReport& report, ub::CallbackData data) {
    auto* log = data.As<MyLog>();                 // reserved before it was needed
    if (log != nullptr) {
        log->Record(report.fault, report.isolate, report.message);
    }
}

const ub::Platform platform({.onEngineFault = &OnFault,
                             .engineFaultData = ub::CallbackData::For(myLog)});
```

One callback with a kind, not one per kind. It lives on the `Platform` because
that is the only place that exists before the first isolate and after the last -
a failure inside `Isolate::New` has no isolate to have registered on - and
`report.isolate` names the heap when there was one. It is fixed for the life of
the `Platform` rather than settable, because it can arrive on any thread.

**What a handler may do is narrower than anywhere else in this API**: embedder
state and `TerminateExecution` yes; a handle, a value, a call into the engine,
script, a throw, and above all **an allocation** - no. The report that arrives
most often says allocation is failing, so allocating to report it is a bug in
every case and a crash in the interesting one. Reserve the buffer and open the
log file first.

Both kinds come from both engines. `EngineFault::Fatal` covers a failed engine
check of every sort - V8's `CHECK` and API misuse, SpiderMonkey's `MOZ_CRASH`
and `MOZ_RELEASE_ASSERT`, and the `DCHECK` / `MOZ_ASSERT` a debug engine adds -
and the process ends after your handler returns. It is the one hook to install
instead of each engine's own OOM, fatal and assertion handlers.

```cpp
std::size_t Rescue(ub::Isolate& isolate, std::size_t current, std::size_t initial,
                   ub::CallbackData data) {
    isolate.TerminateExecution();                 // stop the script that filled it
    return initial * 2;                           // and leave room to unwind in
}

isolate.SetHeapLimitCallback(&Rescue, ub::CallbackData::For(policy));
```

A heap about to hit `heapLimitBytes` is a *decision*, not a report, which is why
it is not one of the kinds: nothing has failed yet, and what happens next is
what you return. Raise the ceiling and terminate together - a bigger heap alone
just feeds the runaway script, and a stop alone has no room to unwind in - and
one bad script stops instead of the process ending.

**V8 has this hook and SpiderMonkey does not, so a SpiderMonkey build does not
link a call to it.** That is this library's standing answer for an operation an
engine cannot do: a build error at your call site, not a field that compiles
everywhere and fires in half the builds. The CPython backend has it too - built
on allocator hooks rather than found in the engine, since CPython has no heap
limit of its own - and there the refused allocation is a `MemoryError` the
script can catch.

### 11. Chrome DevTools

```cpp
class DevTools final : public ub::InspectorClient {
    void SendProtocolMessage(std::string_view message) override { socket.Send(message); }
    void RunMessageLoopOnPause() override {
        paused = true;
        while (paused) {                                        // script is stopped until this returns
            session->DispatchProtocolMessage(inbox.Wait());     // what the socket thread queued
        }
    }
    void QuitMessageLoopOnPause() override { paused = false; }  // a Debugger.resume came through
    // ...
};

std::unique_ptr<ub::Inspector> inspector;                       // after the isolate, before the realm goes
std::unique_ptr<ub::InspectorSession> session;
if (ub::Inspector::Supported()) {                               // false on SpiderMonkey, and it links anyway
    inspector = ub::Inspector::New(isolate, devTools);          // null if this isolate already has one
    inspector->ContextCreated(context, "main");
    session = inspector->Connect();
}
// ... and on the way out: session.reset(); inspector->ContextDestroyed(context); inspector.reset();
```

DevTools talks to a session in protocol messages - JSON, UTF-8 -
`DispatchProtocolMessage` takes them in, and answers come back through the
client, usually before the dispatch returns. A pause runs inside whatever call
was running script, and it is yours to run: `RunMessageLoopOnPause` feeds the
session until DevTools resumes.

A socket is read on a thread of its own, and that thread holds the inspector's
`Dispatcher()` - a `std::shared_ptr<ub::InspectorDispatcher>`, taken on the
isolate's thread and handed over - never the `Inspector` itself. It queues what
it reads and asks the isolate to come and take it:

```cpp
std::thread reader([dispatcher = inspector->Dispatcher(), &socket, &devTools] {
    while (auto message = socket.Receive()) {
        devTools.inbox.Push(std::move(*message));
        // false once the inspector has gone; the callback then never runs
        dispatcher->RequestDispatch(&DispatchQueued, ub::CallbackData::For(devTools));
    }
});
// DispatchQueued, on the isolate's thread: take what is in the inbox without
// waiting, and hand each message to the session.
```

`RequestDispatch` runs your callback on the isolate's thread at the next safe
point - inside a running script, which is how a busy isolate still answers, or
at the next `PumpJobs` if it is idle - and that callback may dispatch. **A
pause is neither**, which is why the pause loop above reads the inbox itself: a
request made while script is paused waits for the pause to be over - the engine
holds interrupts off for the whole of it, evaluations made in it included - so a
`Debugger.resume` that only reached the isolate through one would never arrive.
The callback finds the inbox empty afterwards, which is what one queue read by
both costs. `RequestDispatch` is safe from any thread at any time, including
while the isolate's thread is destroying the inspector and after: no mutex of
yours around it. Each request runs exactly once, in the order they were made; a
burst made before the isolate gets to them shares one wake-up. Any still
waiting when the `Inspector` is destroyed are dropped, not run - so what a
request's data points at has to live until it runs or the inspector goes.

A session goes before its inspector and an inspector before its isolate, all on
the isolate's thread, and a realm is withdrawn with `ContextDestroyed` before
the `Context` is let go. A connection that closes during a pause can simply be
destroyed there, inside `RunMessageLoopOnPause`: that ends the pause as `Stop`
does, and the client hears nothing more from the session. `Resume` does nothing
outside a pause or after `Stop`. `Connect` is null only when there is not the
memory to make a session.

---

## Rules an embedder must know

Each of these is a decision written down in a header, and each fails *silently*
if ignored - no compile error, no exception, no log line.

1. **One isolate alive per thread.** A second while the first lives is refused
   (`Isolate::New` returns null). One after another is fine. Two heaps at once is
   two threads.
2. **Scopes close in reverse order of opening**, and live on the stack.
   `HandleScope`, `EscapableHandleScope`, `ContextScope` and `TryCatch` are all
   stack-only and LIFO.
3. **A `Local` dies with its frame.** To return one, `Escape` it from an
   `EscapableHandleScope`; to keep one longer, move it into a `Global<T>`.
4. **An empty handle is not `undefined`**, and an empty `std::optional` is not a
   value. Neither is a result.
5. **If you never call `PumpJobs`, promise continuations never run.** Nothing
   else drains them.
6. **Posted work runs when the script thread next pumps**, and nothing
   accelerates that. An interrupt does not: it cannot make a running script
   yield, and it never fires while the thread is idle.
7. **A running script cannot be made to yield, only terminated.** There is no
   suspend and no resume anywhere in this API.
8. **A native that blocks cannot be interrupted.** Termination and interrupts
   land at the engine's checkpoints, all of which are inside script.
9. **A stopped isolate stays stopped until `CancelTerminateExecution`.** Every
   operation that would run script fails until then, and the failure is not an
   exception.
10. **Closing a `TryCatch` consumes what it caught** unless you called
    `ReThrow`. A termination is the exception: it continues regardless.
11. **Enter a realm before reading through an object that belongs to it.** A
    value crosses realms freely; a realm's global object is access-checked.
12. **A cache blob is opaque and belongs to the source and the engine build that
    made it.** Offer a stale one and it is refused, the source compiles normally,
    and `UsedCodeCache()` says false. The blob is keyed on the engine's own
    *build identity*, not on its name, so upgrading the engine invalidates
    every blob you kept - which is the point: a key that did not notice would
    leave you paying a full compile every run, for ever, with `UsedCodeCache()`
    quietly answering false. The same goes for a `Serialize` blob: valid for
    that engine build, in that process, and not an interchange format.
13. **A callback returns promptly after throwing** and calls nothing further into
    the engine.
14. **Whatever is still queued when an isolate is destroyed is dropped**, not
    run.
15. **Nothing an isolate handed out may outlive it.** A `Context`, a `Script`
    and a `Global<T>` are the three things that outlive a handle frame, and all
    three name that isolate's engine state: let them go before it does.
    Declaring the isolate first and everything else after it is enough. A
    checked build diagnoses the violation in `~Isolate`; a release build does
    neither check nor tolerate it.

## What it costs

Against V8's own API, the handle model costs **one extra load per value access
and one extra pointer-sized word per handle**; against SpiderMonkey's it costs
nothing, because `JS::Rooted` cannot express the operation at all. No allocation
per handle, no virtual call, no `std::function`, no RTTI. The table is in
[`docs/lifetimes.md`](docs/lifetimes.md) §6.

That table assumes link-time optimization. Without it:

> **Every backend call is a real out-of-line call.** The public headers declare
> the backend's operations and the backend library defines them, because a public
> header may not include an engine header - that is the property that keeps a
> second engine possible. With LTO the definitions inline back into your code;
> without it, reading a property is a call.

`UNIBIND_LTO` is off in this tree, which means **the benchmarks
(`tests/bench/bench_main.cpp`) were measured in the slower configuration**. They
are a floor, not a ceiling. A consumer wanting the table's numbers builds the
library with `-DUNIBIND_LTO=ON` and compiles their own code with LTO too.

Mixing is fine in the ordinary case, and was checked rather than assumed: a
consumer compiled `-flto=thin` (bitcode objects) links against this
non-LTO-compiled static library and the engine's own archives under `lld-link`,
and the result runs. What you do not get from that is inlining *across* the
boundary, which is the whole point of turning it on.

## Gotchas worth knowing before you start

[`docs/gotchas.md`](docs/gotchas.md) is the collection - ninety-odd of them,
grouped by what you were doing, and opening with the nineteen you will not
diagnose from the symptom. Five belong here because they are about *getting the
build to work at all*, which is where a new consumer meets them.

**The 32-bit linker silently loses the engine.** An x86 MSBuild project takes the
32-bit host toolchain by default (`VC\Tools\Llvm\bin`, not `...\Llvm\x64\bin`),
and a 32-bit `lld-link` cannot get through an archive the size of an engine -
V8's monolith is 1.2 GB. It does not say so. It opens the archive, loads no
member from it, and reports *every* engine symbol as undefined, exactly as though
the library had not been passed at all. Set
`<PreferredToolArchitecture>x64</PreferredToolArchitecture>` in the project's
`Globals` group, above the import of `Microsoft.Cpp.Default.props`. `unibind.props`
checks for this and stops the build with a sentence, but it cannot fix it: by the
time a property sheet is imported the toolchain has been chosen. CMake happens to
pick the 64-bit tools already, which is why this only bites MSBuild consumers.

**A `/MD` project fails at link, not at compile.** The engines link the static
CRT. The MSVC STL stamps `RuntimeLibrary` into every object with the same
mechanism unibind's `config.h` uses for its own ABI tag, so the error is LNK2038 and
names the CRT.

**A Debug build needs a debug engine tree.** `/MTd` against a `/MT` engine is the
same LNK2038 for the same reason.

**The x64 V8 monolith replaces your `operator new`.** It carries PartitionAlloc's
Windows allocator shim, which defines `malloc`, `free` and twelve of the twenty
allocation operators, in an object that every link pulls in. A program that
replaces those operators itself - a leak counter, an allocation-failure injector,
an arena - gets a duplicate symbol and does not link. The x86 monolith has no
shim, and SpiderMonkey's problem is the mirror image: including one of *its*
headers replaces your `operator new` silently (`dependencies/README.md`). This
tree's own test suite hits this; `tests/CMakeLists.txt` says what it does about
it and why.

**A CPython program takes a standard library directory over its own.** The
pure-Python standard library is embedded in the backend, and a program needs
nothing beside it - but `UNIBIND_PYTHON_HOME`, or a `python-stdlib` directory
beside the executable, still wins, on a user's machine as on yours. One left
pointing at another CPython's `Lib` gives the program another version's modules.
Built with `UNIBIND_PYTHON_EMBED_STDLIB` off, the program instead reads `Lib/`
from disk, last from the build tree, and runs where it was built and nowhere
else until you ship `Lib/` beside it as `python-stdlib`.

## What the name claims

The name does not say JavaScript, and most of the binding surface really is not
JavaScript-shaped: a class with a constructor, methods, accessors, statics,
native state and a finalizer; calling native from script and script from
native; exceptions; the value set of scalars, strings, arrays and objects;
interceptors, which are `__getattr__`/`__setattr__` in Python and
`__index`/`__newindex` in Lua; iteration; and the handle model itself, since a
frame that gives back everything it took when it closes is exactly the shape a
refcounted runtime wants a borrow to have.

That was a prediction when it was written here, and the CPython backend is the
test of it. The binding layer carried whole: no public header changed, the
handle model became an array of strong references, and a class, a template, an
interceptor or a pump written for V8 binds the same way for Python.

What this section said would not carry is where the work went, and none of it
was free. Realms became globals dictionaries of one interpreter, so they share
its modules and a realm is a namespace rather than a world. Prototypes became a
`unibind.Object` that carries its own `[[Prototype]]` chain, and a template
became a Python *type* per realm, so that `isinstance`, subclassing and
`super()` work. Symbol keys stand for Python's protocols where one exists. The
promise model became an asyncio event loop that only `PumpJobs` drives. Each of
those is a decision, and [`docs/python.md`](docs/python.md) makes all of them in
the open - including the places where the two languages simply disagree and one
had to be picked. So the name's claim held: it is about the shape of the binding
layer. A non-JavaScript backend was not only a matter of writing one, and it
turned out to be possible anyway.

## Limits

**A debugger where the engine has the protocol, and not where it does not.**
[`unibind/inspector.h`](include/unibind/inspector.h) offers the Chrome DevTools
inspector - V8's inspector protocol, in this library's terms - and on
SpiderMonkey `Inspector::Supported()` says no. Its debugging surface is the
`Debugger` object, a JavaScript API installed into a debuggee realm, which
speaks no protocol and has no C++ session to drive; a DevTools server over it
would be a different and much larger thing than a backend. CPython's debugging
surface is `sys.monitoring` and the debuggers built on it, which speak the Debug
Adapter Protocol if they speak anything, and it says no too. A program links
either way and asks at run time (decision 29).

For anything else only V8 can do,
[`unibind/interop/v8.h`](include/unibind/interop/v8.h) hands out the engine
objects (`ub::interop::V8Isolate`, `ub::interop::V8Context`). It is the one
header whose functions only one backend defines: put the code that calls it in a
library linked only into the V8 build, and link something else in its place for
the others.

**Cross-realm access control is not expressible.** Two realms cannot be told to
trust each other (V8 spells that as a shared security token; SpiderMonkey has
compartments and principals, and the two do not describe the same thing), and a
realm cannot be walled off from another. The portable answer is the rule in
section 7: enter the realm that owns the object. It is a line per hook, and it is
not a workaround for something you could otherwise ask for - you cannot ask. On
the CPython backend a realm is walled off from even less: realms of one isolate
share an interpreter, and so share `sys.modules` and `builtins`.

**Python is not a sandbox.** A JavaScript engine starts with nothing but what
you bind; CPython starts with the file system, sockets, processes and the whole
standard library, and nothing an embedder binds or removes takes them away.
Run only Python you would run as the host process itself, or contain the
process with the operating system ([`docs/python.md`](docs/python.md) section 12).

**An isolate on CPython 3.12 costs memory the process never gets back** - about
9 MB per isolate made and destroyed, because 3.12 does not free a
sub-interpreter's arenas (3.13 does). Keep isolates for the life of a worker
thread rather than making one per request.

**A Python script's threads are second-class, and die with its isolate.** They
share the isolate's GIL, so they run only while the isolate's thread is running
Python or blocked in a call; they cannot call anything the embedder bound; a stop
reaches them; and `~Isolate` stops them rather than wait for them to finish. One
stuck in a call that never returns makes the isolate leave its interpreter behind
([`docs/python.md`](docs/python.md) section 6.4).

**No BigInt factory.** The type is recognised (`ValueKind::BigInt`,
`Is<BigInt>()`) but native cannot make one. A `BigInt64Array` or
`BigUint64Array` is still made and read in bulk, as `std::int64_t` and
`std::uint64_t`, because that goes through its bytes and not through a BigInt.

**A heap about to hit its ceiling cannot be *asked about* on SpiderMonkey.**
`Isolate::SetHeapLimitCallback` is V8's near-heap-limit hook and SpiderMonkey
has nothing of the kind - not a different shape, nothing - so a call to it does
not link there. The CPython backend defines it, over allocator hooks of its own.
All three still report the failure itself as `EngineFault::OutOfMemory`; what
differs is whether you get asked first. `EngineFault::Fatal` needs no such
caveat on the JavaScript engines: SpiderMonkey has no hook for its `MOZ_CRASH`,
so the backend recognises the crash itself. CPython's `Py_FatalError` is not
hooked, so on that backend `Fatal` arrives only for a bring-up that failed.

**Not thread-safe, by contract.** Everything but `TerminateExecution`,
`RequestInterrupt`, `PostJob`, `PostDelayedJob` and the inspector dispatcher's
`RequestDispatch` happens on the isolate's own thread.

**Windows only.** x86 and x64 are both built and tested on both JavaScript
backends, and `CMakeLists.txt` refuses anything else rather than letting it fail
later. The CPython backend has presets for both and its suite passes on both,
Release and Debug on x64 and Release on x86; no CI job runs it yet. Nothing in the *design* is Windows-specific; nothing
has been built anywhere else.

## Layout

```
include/unibind/    the public API. No engine header, transitively, and no
                    mention of a backend either - both enforced by the
                    unibind_headers_only target, not by review
src/backends/v8/    the V8 backend: one of the three places an engine header may appear
src/backends/spidermonkey/
src/backends/python/
cmake/              UnibindEngines.cmake: fetching a published engine build, once
                    per version, whole or not at all - or, for CPython, finding
                    what vcpkg built
cmake/vcpkg-ports/  the overlay python3 port: a static CPython with the
                    standard library's extension modules built in
tools/headers_only/ compiles every public header alone, and instantiates the
                    whole template surface, with no engine on the include path;
                    backend_neutral.cmake is the half that has to be read
                    rather than compiled
tests/              one suite, written against ub:: only, run against both
                    JavaScript backends and compared - see tests/README.md
tests/python/       the CPython backend's suite: the same discipline, Python
                    as the script language - see tests/python/README.md
examples/           embed: a consumer, built against an installed prefix;
                    python_repl: a Python prompt over the CPython backend
packaging/          unibind.props and the CMake package, for consumers
docs/               decisions, including the ones that were rejected, and
                    gotchas.md, which is the one to read first
```

## License

MIT - see [`LICENSE`](LICENSE). No engine is in this repository: V8 and
SpiderMonkey are paths you point the build at, and CPython is built by vcpkg -
what is here of it is a vcpkg port and patches against its source. The engine
you link has a license of its own. SpiderMonkey's MPL-2.0 is the one to read
closely: it permits linking into a proprietary product and asks that the
*engine's* source stay available. CPython's PSF license is permissive, but a
program linked with this backend also carries OpenSSL, libffi, SQLite, expat,
liblzma, bzip2 and zlib, and CPython's standard library compiled into it.
[`docs/licensing.md`](docs/licensing.md) says what all of that means in practice.
