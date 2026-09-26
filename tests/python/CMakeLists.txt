# The CPython backend's suite.
#
# The shared suite in tests/cases is written in JavaScript source as much as in
# C++, so it has nothing to say to an engine whose scripts are Python - and the
# parity comparison, which lines one backend's results up against another's,
# has nothing to compare. This suite is the same kind of thing written for this
# engine: C++ against the public `ub::` API only, with Python as the script
# language. See tests/python/README.md.

add_executable(unibind_python_tests
    main.cpp
    support.cpp
    smoke_test.cpp
    runtime_test.cpp
    promises_test.cpp
    binary_test.cpp
    serialization_test.cpp
    codecache_test.cpp
    objects_test.cpp
    functions_test.cpp
    templates_test.cpp
    interceptors_test.cpp
    classes_test.cpp
    stress_test.cpp
    stdlib_test.cpp
    stdlib_embedded_test.cpp
    lifetimes_test.cpp
    lifetimes_natives_test.cpp
    teardown_test.cpp
    concurrency_test.cpp
    # The global allocation operators, replaced: the lifetime cases count the
    # backend's own C++ heap and make its allocations fail on purpose.
    "${PROJECT_SOURCE_DIR}/tests/support/allocations.cpp")

target_include_directories(unibind_python_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(unibind_python_tests PRIVATE unibind::unibind doctest::doctest)
# Which standard library the `stdlib embedded` cases expect when no directory
# is given: the embedded one, or (built without it) the build tree's Lib.
if(UNIBIND_PYTHON_EMBED_STDLIB)
    target_compile_definitions(unibind_python_tests PRIVATE UNIBIND_TEST_STDLIB_EMBEDDED=1)
else()
    target_compile_definitions(unibind_python_tests PRIVATE UNIBIND_TEST_STDLIB_EMBEDDED=0)
endif()

# Large-address-aware, a 32-bit process gets 4 GB of address space on 64-bit
# Windows instead of 2 GB, for nothing. It is no longer a necessity: CPython 3.12
# kept every isolate's memory for good, and a few hundred isolates filled 2 GB;
# 3.14 gives it back (docs/python.md, section 11).
if(UNIBIND_ARCH STREQUAL "x86")
    target_link_options(unibind_python_tests PRIVATE "/LARGEADDRESSAWARE")
endif()

if(EXISTS "${doctest_DIR}/doctest.cmake")
    include("${doctest_DIR}/doctest.cmake")
    doctest_discover_tests(unibind_python_tests
        TEST_PREFIX "python."
        PROPERTIES TIMEOUT 120 LABELS "suite")
else()
    add_test(NAME python.suite COMMAND unibind_python_tests)
    set_tests_properties(python.suite PROPERTIES TIMEOUT 300 LABELS "suite")
endif()

add_test(NAME python.whole-suite-in-one-process COMMAND unibind_python_tests)
set_tests_properties(python.whole-suite-in-one-process PROPERTIES TIMEOUT 900 LABELS "suite")

# The embedded standard library, out of process - see stdlib_embedded_test.cpp.
if(UNIBIND_PYTHON_EMBED_STDLIB)
    # A copy of the suite alone in an empty directory, UNIBIND_PYTHON_HOME
    # unset: nothing on disk to find, and the `stdlib embedded` cases pass from
    # the embedded table.
    add_test(NAME python.stdlib-embedded.runs-alone-from-an-empty-directory
             COMMAND "${CMAKE_COMMAND}"
                     "-DEXE=$<TARGET_FILE:unibind_python_tests>"
                     "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/standalone/$<CONFIG>"
                     "-DARGS=--test-case=stdlib embedded:*"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/standalone.cmake")
    set_tests_properties(python.stdlib-embedded.runs-alone-from-an-empty-directory PROPERTIES
        LABELS "suite" TIMEOUT 300
        PASS_REGULAR_EXPRESSION "standard library embedded: .*Status: SUCCESS!.*exit code: 0")

    # UNIBIND_PYTHON_HOME still wins: the same cases, which then expect the
    # directory's modules rather than the embedded ones.
    add_test(NAME python.stdlib-embedded.unibind-python-home-still-wins
             COMMAND unibind_python_tests "--test-case=stdlib embedded:*")
    set_tests_properties(python.stdlib-embedded.unibind-python-home-still-wins PROPERTIES
        LABELS "suite" TIMEOUT 300
        ENVIRONMENT "UNIBIND_PYTHON_HOME=${UNIBIND_PYTHON_STDLIB}"
        PASS_REGULAR_EXPRESSION "standard library from disk: .*Status: SUCCESS!")
endif()
