# The CPython backend's suite

The CPython backend runs Python, so the shared suite in `tests/cases/` - whose
cases are JavaScript source as much as C++ - means nothing to it, and the parity
comparison leaves it out. This is the suite that stands in its place, written the
same way: **C++ against the public `ub::` API only, with Python as the script
language.** Nothing here names a CPython type or includes a CPython header; a
case that did would be testing the backend's insides rather than what an
embedder can see. `docs/testing.md`, "The CPython backend's suite", is the
account of what it asserts and what it does not.

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset python-x64
cmake --build build/python-x64 --config Release --parallel 1
ctest --preset python-x64                                   # every case, one CTest test each
build\python-x64\tests\python\Release\unibind_python_tests.exe   # the whole suite, one process
```

The first configure of a triplet builds CPython through vcpkg; see the root
README. The test binary finds the standard library where the build left it, so
nothing has to be copied to run it from the build tree.

## What a run looks like

**261 cases** - the figure the test binary reports, and the one to compare a run
against (120490 assertions in a Release x64 run, most of them the lifetime and
teardown cases counting references in loops; it differs by build and
architecture and is not the number to compare). `-ltc` lists them,
`-tc="objects:*"` runs one area.

One more case is registered but skipped unless asked for by name:
`stress: making isolates until memory runs out fails cleanly`, which makes
isolates until `Isolate::New` answers empty. It is too slow and too hard on the
machine for every run - it runs until memory does - so run it deliberately: `unibind_python_tests -tc="stress:*" --no-skip`. It is
not among the 261, and CTest does not register it.

Under CTest each case is a test of its own, named `python.<area>: <what it
pins>`, with the label `suite`, plus one more:

| test | what it is |
|---|---|
| `python.whole-suite-in-one-process` | every case in one process, which is the only thing that catches a case quietly depending on another |

The example REPL registers six more, under the label `example`:
`ctest -C Release -L example` runs its `--demo`, a script with arguments, a
script that raises and the traceback it prints, a runaway loop stopped by
`--timeout`, and a piped interactive session
([`examples/python_repl/README.md`](../../examples/python_repl/README.md)). A
plain `ctest` runs both labels: 268 tests.

A full run takes about two and a half minutes (Release x64), most of it cases
that stop scripts from other threads, make and tear down many isolates, or wait
for timers and thread grace periods on purpose.

## Areas

Each file is one area, and every case name starts with its area:

| file | areas |
|---|---|
| `smoke_test.cpp` | `smoke`: the platform, the completion value, a throw |
| `objects_test.cpp` | `objects`: `unibind.Object`, and property operations on dicts, lists, tuples and other objects |
| `functions_test.cpp` | `functions`: native functions, receivers, return values, data, calls both ways, exceptions across the boundary |
| `templates_test.cpp` | `templates`: object and function templates as per-realm types, statics, inheritance, iteration |
| `interceptors_test.cpp` | `interceptors`: named and indexed handlers |
| `classes_test.cpp` | `classes`: `Class<T>`, Python subclasses of one, and when a native is given back |
| `runtime_test.cpp` | `termination`, `interrupts`, `jobs`, `heap`, `stack`, `concurrency`, `lifetime` |
| `promises_test.cpp` | `promises`: asyncio futures as `ub::Promise`, and the loop `PumpJobs` drives |
| `binary_test.cpp` | `binary`: `bytearray` buffers, `unibind.TypedArray` and `unibind.DataView` |
| `serialization_test.cpp` | `clone`: structured clone |
| `codecache_test.cpp` | `codecache`: the compiled-code cache |
| `stdlib_test.cpp` | `stdlib`: the extension modules built into the static CPython, and the ones an isolate refuses |
| `lifetimes_test.cpp` | `lifetimes`: handles, frames, `Global`s and realms counted exactly - references taken and given back, frame and root exhaustion, a realm surviving `globals().clear()` and a copied dict, many realms and scripts leaving the C++ heap where it was, and the `bytearray` that could not be allocated |
| `lifetimes_natives_test.cpp` | `lifetimes`: when a `Class<T>` native is destroyed - cycles, resurrection, destructors that run script or make natives at teardown, an instance dying on a script's thread - and how long a callback's values live |
| `teardown_test.cpp` | `teardown`: isolates in sequence and in parallel giving back all they took, threads a script started (stopped by `TerminateExecution`, stopped and waited for by `~Isolate`, left behind when stuck), stops that land on code holding things, and asyncio's current loop after `asyncio.run` |
| `stress_test.cpp` | `stress`: skipped unless asked for - making isolates until memory runs out |

and the rest of the directory:

```
main.cpp            the runner: one Platform for the whole run, with an
                    engine-fault handler that counts what arrives
support.h/.cpp      the fixture (an isolate, a scope, a realm, entered) and the
                    Eval helpers: Eval, EvalInt, EvalText, EvalTruth, EvalError
data_support.h      what the binary-data, clone and code-cache cases share
lifetimes_support.h what the lifetime and teardown cases share
../support/allocations.cpp   the shared suite's replacement of the global
                    allocation operators, linked in here too: the lifetime
                    cases count the backend's own C++ heap with it and make
                    its allocations fail on purpose. CPython allocates through
                    malloc, which it does not count
```

## Writing a case

- **Never name a CPython type and never include a CPython header.** Everything
  goes through `ub::` and through Python source.
- **Assert Python's behaviour only where it is a decision.** A case may pin that
  a missing attribute raises `AttributeError` or that `len()` of an object counts
  its keys, because `docs/python.md` decided those and says why. It may not pin
  CPython's own wording of a message it did not choose, or a traceback's layout.
- **Assert on what ran, not on what the API reported** - the rule the shared
  suite learned from two code-cache bugs, and the reason `codecache` cases run the
  script they compiled.
- **A case that stops a script waits until the script is running** before it
  stops it - `Stopper` in `runtime_test.cpp` learns that from an interrupt, which
  fires only inside running Python - or it will be testing the compiler.
- **Keep `;` and `|` out of case names.** CMake splits a name at `;` when CTest
  registers it, and each half is a filter that matches nothing, which passes. Two cases
  in `runtime_test.cpp` had one, and ran only in the whole-suite test until
  they were renamed.
- **Run concurrent cases at full thread count in Debug too.** A debug CPython
  is where a race inside CPython shows up: `concurrency_test.cpp` found the one
  patch 0103 fixes (`docs/python.md` section 11), and would not have at two
  threads.
- **Size isolate counts for x86**, where every isolate keeps about 9.5 MB of a
  32-bit address space for good; the cases that make many scale down there.
- **A case written against a promise stays red rather than weakened**, as in the
  shared suite.
