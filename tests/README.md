# The suite

One suite, written once against `ub::` only, run against every JavaScript
backend the tree can build. A test that names an engine type has failed at its
job.

**The CPython backend does not run it.** Its scripts are Python, and these cases
are JavaScript source as much as C++, so it has a suite of its own in
[`python/`](python/README.md), written the same way. `tests/CMakeLists.txt`
builds that one instead when `UNIBIND_BACKEND=python`, and the parity comparison
below leaves the python backend out.

```powershell
cmake --preset v8
cmake --build build/v8 --config Release --parallel 1
ctest --test-dir build/v8 -C Release              # every case, one CTest test each
cmake --build build/v8 --config Release --target parity   # every backend, side by side
```

## Running the parity comparison

`parity` is the one command. It builds every backend under `src/backends/` that
this tree can build, runs the suite through each, and prints one row per test
case with one column per backend:

```powershell
cmake --build build/v8 --config Release --parallel 1 --target parity
```

It never rebuilds the backend it was invoked from, and it reuses a build tree
that already exists for another backend (`build/<backend>`, which is what the
presets make) rather than making a second one. The full matrix is written to
`<tree>/tests/parity/parity-report.md`; the console shows only the rows that
differ. The command fails if any case passes on one backend and fails on
another - which is the failure it exists to find.

`ctest` also carries a `<backend>.parity` test. That one compares the trees that
are *already* built and never compiles anything, so a plain `ctest` run stays a
few seconds rather than an hour.

A backend joins the comparison by existing: nothing lists them - except
`python`, which `cmake/RunParity.cmake` skips by name, because its cases are not
these cases.

## What a CTest run looks like

Every doctest case is registered with CTest under its own name
(`v8.handles: a Global outlives every handle scope`), so a failure names the
case rather than the binary. Seven tests stand apart from the per-case ones on
every backend, an eighth on V8, and two more in a checked build:

| test | what it is |
|---|---|
| `<backend>.whole-suite-in-one-process` | the whole suite in one process, which is the only thing that catches a case quietly depending on another |
| `<backend>.benchmark` | the hot-operation benchmark, at a small scale |
| `<backend>.parity` | the comparison described above, over already-built trees |
| `<backend>.global-identity-without-a-scope` | decision 16's load-bearing clause: two `Global`s compared with no `HandleScope` open at all. A backend that gets it wrong ends the process, so it runs alone rather than taking the matrix with it. |
| `<backend>.platform-is-the-gate-on-an-isolate` | the platform lifecycle - before one exists, while one does, after it has gone - which needs a process with no `Platform` in it, and the suite always has one |
| `<backend>.worker-threads-belong-to-one-platform` | the same, with a worker-thread count asked for, so the cached figure is observable |
| `<backend>.fatal-reaches-the-fault-handler` | decision 28's `EngineFault::Fatal`: the engine's own fatal path, provoked in a process of its own, passing on the report the handler printed on the way out rather than on an exit code |
| `v8.interop` | `unibind/interop/v8.h`, which only the V8 backend defines, so it is built and run only there |
| `<backend>.checked.using-a-handle-after-its-scope-closed-is-diagnosed` | `docs/lifetimes.md` section 9: a checked build must diagnose a stale handle. Diagnosing it means dying, so it runs in a process of its own and is expected to fail. Debug configurations only - Release compiles the assertion out. |
| `<backend>.checked.a-global-outliving-its-isolate-is-diagnosed` | decision 27: a `Context`, `Script` or `Global<T>` still alive when its isolate goes. Same shape and same reason as the one above, and registered the same way. |

## Areas a backend has not implemented yet

An operation a backend *declares* but does not *define* is a link error at the
call site, so a case that called one could not be built, let alone skipped. The
suite therefore decides at build time what to compile:

1. `cmake/ProbeCapabilities.cmake` reads the built backend library's archive
   symbol index and writes `unibind_test_capabilities.h`, a header of `0`/`1`
   macros - one per row of `cmake/Capabilities.cmake`. It reads the symbol
   *index* rather than the whole file on purpose: the rest of an archive names
   every declaration, defined or not, so scanning it would say every backend
   implements everything.
2. A case declared with `UNIBIND_TEST_CASE(AREA, "name")` compiles either to an
   ordinary `TEST_CASE`, or - where the area is absent - to a `TEST_CASE` of the
   same name that reports a skip, followed by an uninstantiated function
   template that swallows the body. The body is still parsed and type-checked;
   no code is emitted for it, so it references nothing the backend lacks.

The case therefore exists under the same name on every backend, CTest reports it
as skipped rather than passed, and the parity matrix has a `SKIPPED` cell rather
than a missing row. `harness: a case whose area the backend lacks is skipped,
not missing` is gated on a capability no backend can have, so that path is
exercised on every run rather than only on the day a backend falls behind.

Adding an area means adding a row to `cmake/Capabilities.cmake` and using its
name in `UNIBIND_TEST_CASE`. Nothing else knows the list.

## Layout

```
python/             the CPython backend's suite - see python/README.md
main.cpp            the runner, plus the checks that need a process of their
                    own: the two deliberately-fatal checked-build ones,
                    comparing `Global`s with no scope open, and the two about
                    a process before and after it has a `Platform`
fatal_v8.cpp        EngineFault::Fatal provoked through each engine's own
spidermonkey/fatal.cpp    fatal path, one process per backend
interop_v8.cpp      unibind/interop/v8.h, built only for V8
support/harness.h   the fixture, the gating macros, and the small helpers
support/allocations.cpp   global operator new/delete, so a leak test has
                          something to measure that no engine reports
support/parity_reporter.cpp   a doctest reporter that writes STATE|name
cases/              one file per area
bench/              the hot-operation benchmark
cmake/              the capability probe, the engine-allocator probe, and the
                    parity runner
```

## Writing a case

- Never name an engine type, and never include an engine header.
  `BackendName()` and `BackendVersion()` may be printed; branching on either is
  a finding, not a fix - and `BackendVersion()`'s text is the engine's own, so
  it may not be asserted on at all.
- Where the engines genuinely differ, assert the shape and write the difference
  down in `docs/testing.md`.
- Keep case names stable and identical across backends: the parity matrix joins
  on them.
- `|` is the parity reporter's separator, so it must not appear in a case name.
- Nor may `;`: CTest registers each case by name, CMake splits a name at `;`,
  and the halves are filters that match nothing - which passes.
