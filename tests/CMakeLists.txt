# One suite, written against the public API only, built against whichever
# backend this tree configured - and comparable against every other backend
# through the `parity` target. See tests/README.md.

find_package(doctest CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# The benchmark
#
# The one program here every backend builds: its workloads are C++ against
# `ub::` and, for the script side, a JavaScript and a Python source for each,
# chosen at run time by `Platform::BackendName()`. Labelled `benchmark`, which
# CI does not run; `--quick` only checks that every workload runs and answers.
# `tests/bench/Compare.cmake` runs it on all three and merges the tables.
# ---------------------------------------------------------------------------
add_executable(unibind_bench bench/bench_main.cpp)
target_link_libraries(unibind_bench PRIVATE unibind::unibind)
add_test(NAME ${UNIBIND_BACKEND}.benchmark COMMAND unibind_bench --quick)
set_tests_properties(${UNIBIND_BACKEND}.benchmark PROPERTIES TIMEOUT 300 LABELS "benchmark")

# The python backend runs Python, not JavaScript, so this suite - and the parity
# comparison built on it - does not apply to it. It has a suite of its own.
if(UNIBIND_BACKEND STREQUAL "python")
    add_subdirectory(python)
    return()
endif()

# ---------------------------------------------------------------------------
# What this backend can answer
#
# An operation a backend declares but does not define is a link error at the
# call site, so a case that used one could not be built at all. The suite reads
# the built backend library and compiles each area in or out accordingly; an
# area that is out still contributes test cases, which report themselves
# skipped. See tests/cmake/ProbeCapabilities.cmake.
# ---------------------------------------------------------------------------
set(UNIBIND_TEST_CAPABILITIES_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/unibind_test_capabilities.h")

add_custom_command(
    OUTPUT "${UNIBIND_TEST_CAPABILITIES_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DUNIBIND_PROBE_LIB=$<TARGET_FILE:unibind_backend_${UNIBIND_BACKEND}>"
            "-DUNIBIND_PROBE_OUT=${UNIBIND_TEST_CAPABILITIES_HEADER}"
            "-DUNIBIND_PROBE_BACKEND=${UNIBIND_BACKEND}"
            "-DUNIBIND_PROBE_TABLE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/Capabilities.cmake"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProbeCapabilities.cmake"
    DEPENDS unibind_backend_${UNIBIND_BACKEND}
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Capabilities.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProbeCapabilities.cmake"
    COMMENT "unibind tests: asking the ${UNIBIND_BACKEND} backend what it defines"
    VERBATIM)

# ---------------------------------------------------------------------------
# The suite
# ---------------------------------------------------------------------------
add_executable(unibind_tests
    main.cpp
    "${UNIBIND_TEST_CAPABILITIES_HEADER}"
    support/allocations.cpp
    support/harness.cpp
    support/ownership.cpp
    support/parity_reporter.cpp
    cases/binary_test.cpp
    cases/classes_test.cpp
    cases/codecache_test.cpp
    cases/coverage_test.cpp
    cases/exceptions_test.cpp
    cases/faults_test.cpp
    cases/functions_test.cpp
    cases/handles_test.cpp
    cases/inspector_test.cpp
    cases/harness_test.cpp
    cases/interceptors_test.cpp
    cases/isolate_test.cpp
    cases/jobs_test.cpp
    cases/objects_test.cpp
    cases/ownership_test.cpp
    cases/realms_test.cpp
    cases/regressions_test.cpp
    cases/regressions_runtime_test.cpp
    cases/regressions_values_test.cpp
    cases/regressions_bindings_test.cpp
    cases/sandbox_test.cpp
    cases/scripts_test.cpp
    cases/stacks_test.cpp
    cases/symbols_test.cpp
    cases/templates_test.cpp
    cases/termination_test.cpp
    cases/values_test.cpp)

target_include_directories(unibind_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(unibind_tests PRIVATE unibind::unibind doctest::doctest)
set_property(SOURCE "${UNIBIND_TEST_CAPABILITIES_HEADER}" PROPERTY HEADER_FILE_ONLY ON)

# ---------------------------------------------------------------------------
# When the engine has already replaced operator new
#
# `support/allocations.cpp` replaces the global allocation operators, which is
# the only way the suite can ask whether a frame gave back what it took or what
# it does when an allocation fails. The x64 V8 monolith replaces twelve of them
# itself - PartitionAlloc's Windows allocator shim, pulled into every link that
# mentions malloc - so both definitions are present and the link fails.
#
# `/FORCE:MULTIPLE` resolves it the way we need rather than merely getting past
# it: the linker takes the first definition it saw, and `allocations.obj` is on
# the command line while the monolith is an archive scanned after it, so the
# suite's operators win and the engine's are dropped. The memory is the same
# either way - ours call `malloc`, which is the shim's - so nothing is freed by
# the wrong allocator.
#
# The suite proves the replacement took rather than assuming it: every case
# that arms an allocation failure checks whether it actually fired, and reports
# itself SKIPPED when it did not. So a configuration where the engine's
# operators won instead is visible in the run, not silently green.
#
# It is scoped to the one configuration that needs it, and off everywhere else:
# a duplicate symbol in the test binary is a real error and should stay one.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EngineAllocator.cmake")
if(UNIBIND_BACKEND STREQUAL "v8")
    set(unibindEngineArchive "${UNIBIND_V8_DIR}/v8_monolith.lib")
else()
    set(unibindEngineArchive "${UNIBIND_SPIDERMONKEY_DIR}/spidermonkey.lib")
endif()
unibind_engine_replaces_operators("${unibindEngineArchive}" unibindEngineOwnsOperators)
if(unibindEngineOwnsOperators)
    message(STATUS "unibind tests: the ${UNIBIND_BACKEND} library defines the allocation operators too; "
                   "linking the suite with /FORCE:MULTIPLE so its own win")
    target_link_options(unibind_tests PRIVATE "/FORCE:MULTIPLE")
endif()

# ---------------------------------------------------------------------------
# unibind/interop/v8.h - defined by one backend, so tested outside the suite
# ---------------------------------------------------------------------------
if(UNIBIND_BACKEND STREQUAL "v8")
    add_executable(unibind_interop_v8 interop_v8.cpp)
    target_link_libraries(unibind_interop_v8 PRIVATE unibind::unibind)
    target_include_directories(unibind_interop_v8 SYSTEM PRIVATE "${UNIBIND_V8_DIR}/include")
    target_compile_definitions(unibind_interop_v8 PRIVATE V8_GN_HEADER NOMINMAX WIN32_LEAN_AND_MEAN)
    if(unibindEngineOwnsOperators)
        target_link_options(unibind_interop_v8 PRIVATE "/FORCE:MULTIPLE")
    endif()
    add_test(NAME v8.interop COMMAND unibind_interop_v8)
    set_tests_properties(v8.interop PROPERTIES TIMEOUT 120 LABELS "suite")
endif()

# ---------------------------------------------------------------------------
# EngineFault::Fatal - ends the process by design, so each backend provokes its
# own in a process of its own, and the pass condition is the report the
# handler printed on the way out rather than an exit code.
# ---------------------------------------------------------------------------
if(UNIBIND_BACKEND STREQUAL "v8")
    add_executable(unibind_fatal fatal_v8.cpp)
    target_include_directories(unibind_fatal SYSTEM PRIVATE "${UNIBIND_V8_DIR}/include")
    target_compile_definitions(unibind_fatal PRIVATE V8_GN_HEADER NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_executable(unibind_fatal spidermonkey/fatal.cpp)
    target_include_directories(unibind_fatal SYSTEM PRIVATE "${UNIBIND_SPIDERMONKEY_DIR}/include")
    unibind_spidermonkey_definitions(unibind_fatal)
    target_compile_definitions(unibind_fatal PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(unibind_fatal PRIVATE unibind::unibind)
if(unibindEngineOwnsOperators)
    target_link_options(unibind_fatal PRIVATE "/FORCE:MULTIPLE")
endif()
add_test(NAME ${UNIBIND_BACKEND}.fatal-reaches-the-fault-handler COMMAND unibind_fatal)
set_tests_properties(${UNIBIND_BACKEND}.fatal-reaches-the-fault-handler PROPERTIES
    TIMEOUT 120 LABELS "suite" PASS_REGULAR_EXPRESSION "unibind-fault: Fatal isolate=yes message=.")

# ---------------------------------------------------------------------------
# CTest: one test per case, so a failure names the case and not the binary
# ---------------------------------------------------------------------------
if(EXISTS "${doctest_DIR}/doctest.cmake")
    include("${doctest_DIR}/doctest.cmake")
    doctest_discover_tests(unibind_tests
        TEST_PREFIX "${UNIBIND_BACKEND}."
        PROPERTIES
            TIMEOUT 120
            LABELS "suite"
            # A case whose area this backend cannot answer prints this and does
            # nothing, so CTest reports it as skipped rather than as a pass.
            SKIP_REGULAR_EXPRESSION "\\[unibind\\] SKIPPED:")
else()
    message(WARNING "unibind tests: doctest.cmake not found; registering the suite as one CTest test")
    add_test(NAME ${UNIBIND_BACKEND}.suite COMMAND unibind_tests)
    set_tests_properties(${UNIBIND_BACKEND}.suite PROPERTIES TIMEOUT 300 LABELS "suite")
endif()

# Everything in one process as well, which is the only way a case that quietly
# depends on another one shows up.
add_test(NAME ${UNIBIND_BACKEND}.whole-suite-in-one-process COMMAND unibind_tests)
set_tests_properties(${UNIBIND_BACKEND}.whole-suite-in-one-process PROPERTIES TIMEOUT 300 LABELS "suite")

# docs/status.md decision 16: comparing two `Global`s needs no open
# `HandleScope`. A backend that gets that wrong does not answer wrongly - it
# makes a handle with no frame and the engine ends the process - so the check
# runs in a process of its own rather than taking the suite down with it. It is
# expected to pass: this is a promise the headers make.
add_test(NAME ${UNIBIND_BACKEND}.global-identity-without-a-scope
         COMMAND unibind_tests --unibind-compare-globals-without-a-scope)
set_tests_properties(${UNIBIND_BACKEND}.global-identity-without-a-scope PROPERTIES TIMEOUT 120 LABELS "suite")

# `unibind/isolate.h`: `Platform::IsInitialized()` is the bring-up failure channel
# and `Isolate::New` consults it, so a platform that is not up hands out no
# isolates. Neither half is askable from inside the suite - it runs under a
# platform that is already up, and a second `Platform` is a precondition
# violation rather than a failure - so the whole lifecycle gets a process of its
# own. Both are expected to pass. See docs/testing.md.
add_test(NAME ${UNIBIND_BACKEND}.platform-is-the-gate-on-an-isolate
         COMMAND unibind_tests --unibind-platform-is-the-gate)
set_tests_properties(${UNIBIND_BACKEND}.platform-is-the-gate-on-an-isolate PROPERTIES TIMEOUT 120 LABELS "suite")

add_test(NAME ${UNIBIND_BACKEND}.worker-threads-belong-to-one-platform
         COMMAND unibind_tests --unibind-worker-threads-belong-to-one-platform)
set_tests_properties(${UNIBIND_BACKEND}.worker-threads-belong-to-one-platform PROPERTIES TIMEOUT 120 LABELS "suite")

# docs/lifetimes.md section 9: a checked build diagnoses a handle used after its
# frame closed. Diagnosing it means dying, so it runs in a process of its own
# and is expected to fail. Release compiles the assertion out, so the test only
# exists where it can hold.
if(UNIBIND_HANDLE_CHECKS_ENABLED)
    add_test(NAME ${UNIBIND_BACKEND}.checked.using-a-handle-after-its-scope-closed-is-diagnosed
             COMMAND unibind_tests --unibind-use-after-scope
             CONFIGURATIONS Debug)
    set_tests_properties(${UNIBIND_BACKEND}.checked.using-a-handle-after-its-scope-closed-is-diagnosed
                         PROPERTIES WILL_FAIL TRUE TIMEOUT 120 LABELS "checked")

    # docs/status.md decision 27: a Context, Script or Global still alive when
    # its isolate is destroyed. Same shape and same reason as the check above -
    # diagnosing it means dying - and registered the same way.
    add_test(NAME ${UNIBIND_BACKEND}.checked.a-global-outliving-its-isolate-is-diagnosed
             COMMAND unibind_tests --unibind-global-outlives-its-isolate
             CONFIGURATIONS Debug)
    set_tests_properties(${UNIBIND_BACKEND}.checked.a-global-outliving-its-isolate-is-diagnosed
                         PROPERTIES WILL_FAIL TRUE TIMEOUT 120 LABELS "checked")
endif()

# ---------------------------------------------------------------------------
# Parity
#
# The engine is a compile-time choice, so comparing two of them means two build
# trees. `cmake --build <tree> --target parity` builds whichever backends this
# source tree has and prints one row per test case with one column per backend.
# The CTest test does the same comparison over trees that are already built, so
# `ctest` never turns into an hour of compiling.
# ---------------------------------------------------------------------------
set(unibindParityCacheArgs "")
foreach(passthrough
        CMAKE_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET VCPKG_HOST_TRIPLET
        UNIBIND_VCPKG_ROOT UNIBIND_V8_DIR UNIBIND_SPIDERMONKEY_DIR
        UNIBIND_HANDLE_CHECKS_ENABLED UNIBIND_LTO)
    if(DEFINED ${passthrough})
        list(APPEND unibindParityCacheArgs "-D${passthrough}=${${passthrough}}")
    endif()
endforeach()
# `|` rather than `;`, so the list survives being one command-line argument.
list(JOIN unibindParityCacheArgs "|" unibindParityCacheArgs)

set(unibindParityCommand
    "${CMAKE_COMMAND}"
    "-DUNIBIND_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DUNIBIND_CURRENT_BACKEND=${UNIBIND_BACKEND}"
    "-DUNIBIND_CURRENT_EXE=$<TARGET_FILE:unibind_tests>"
    "-DUNIBIND_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/parity"
    "-DUNIBIND_CONFIG=$<CONFIG>"
    "-DUNIBIND_GENERATOR=${CMAKE_GENERATOR}"
    "-DUNIBIND_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DUNIBIND_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DUNIBIND_ARCH=${UNIBIND_ARCH}"
    "-DUNIBIND_CACHE_ARGS=${unibindParityCacheArgs}")

add_custom_target(parity
    COMMAND ${unibindParityCommand} -DUNIBIND_PARITY_BUILD=ON
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunParity.cmake"
    DEPENDS unibind_tests
    COMMENT "unibind: building every available backend and comparing the suite across them"
    USES_TERMINAL
    VERBATIM)

add_test(NAME ${UNIBIND_BACKEND}.parity
         COMMAND ${unibindParityCommand} -DUNIBIND_PARITY_BUILD=OFF
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunParity.cmake")
set_tests_properties(${UNIBIND_BACKEND}.parity PROPERTIES TIMEOUT 600 LABELS "parity")
