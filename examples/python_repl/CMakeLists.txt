# An interactive Python prompt over the CPython backend, and the demo that
# exercises every binding it registers. See README.md in this directory.
#
# Unlike examples/embed, this one is part of the library's own build tree: it
# needs the python backend, and it doubles as a test of it (the "example" CTest
# label below). The root CMakeLists.txt adds it only when UNIBIND_BACKEND is
# python and UNIBIND_BUILD_EXAMPLES is on.
#
#   cmake --preset python-x64
#   cmake --build build/python-x64 --config Release --target unibind_python_repl

add_executable(unibind_python_repl main.cpp host.cpp host.h demo.py)
target_link_libraries(unibind_python_repl PRIVATE unibind::unibind)

# Large-address-aware, a 32-bit process gets 4 GB of address space on 64-bit
# Windows instead of 2 GB, for nothing. It is no longer a necessity: CPython 3.12
# kept every isolate's memory for good, and a few hundred isolates filled 2 GB;
# 3.14 gives it back (docs/python.md, section 11).
if(UNIBIND_ARCH STREQUAL "x86")
    target_link_options(unibind_python_repl PRIVATE "/LARGEADDRESSAWARE")
endif()
target_compile_definitions(unibind_python_repl PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    # Where --demo looks when there is no demo.py next to the program.
    "UNIBIND_REPL_DEMO_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/demo.py\"")
# An example is documentation, and documentation that warns is wrong.
target_compile_options(unibind_python_repl PRIVATE /W4 /WX)
set_source_files_properties(demo.py PROPERTIES HEADER_FILE_ONLY ON)

# The demo goes next to the program, so `--demo` works from a copied build.
add_custom_command(TARGET unibind_python_repl POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/demo.py" "$<TARGET_FILE_DIR:unibind_python_repl>/demo.py"
    VERBATIM)

if(NOT UNIBIND_BUILD_TESTS)
    return()
endif()

# --- CTest: the example is a test of the backend too -----------------------------

set(repl "$<TARGET_FILE:unibind_python_repl>")
set(data "${CMAKE_CURRENT_SOURCE_DIR}/tests")

# The demo asserts what it prints; any failed assert is exit code 1.
add_test(NAME example.python_repl.demo COMMAND ${repl} --demo)
set_tests_properties(example.python_repl.demo PROPERTIES
    LABELS "example" TIMEOUT 120
    PASS_REGULAR_EXPRESSION "demo: all sections passed"
    FAIL_REGULAR_EXPRESSION "Traceback")

# A script file with arguments, which also exercises sys.argv and SystemExit.
add_test(NAME example.python_repl.script COMMAND ${repl} "${data}/script.py" one two)
set_tests_properties(example.python_repl.script PROPERTIES
    LABELS "example" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "argv: [[]'[^']*script[.]py', 'one', 'two'[]].*counted to 3.*vector length 5")

# A script that raises: its traceback on stderr, and exit code 1.
add_test(NAME example.python_repl.script-that-raises COMMAND ${repl} "${data}/raises.py")
set_tests_properties(example.python_repl.script-that-raises PROPERTIES
    LABELS "example" TIMEOUT 60 WILL_FAIL TRUE)
add_test(NAME example.python_repl.script-that-raises-prints-a-traceback
         COMMAND "${CMAKE_COMMAND}" -DREPL=${repl} -DARGS=${data}/raises.py
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.cmake")
set_tests_properties(example.python_repl.script-that-raises-prints-a-traceback PROPERTIES
    LABELS "example" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "Traceback [(]most recent call last[)]:.*raises[.]py.*in inner.*ValueError: raised on purpose.*exit code: 1")

# The watchdog: an endless loop stopped by --timeout, exit code 1.
add_test(NAME example.python_repl.timeout-stops-a-runaway-loop
         COMMAND "${CMAKE_COMMAND}" -DREPL=${repl} "-DARGS=--timeout|1|-c|while True: pass"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.cmake")
set_tests_properties(example.python_repl.timeout-stops-a-runaway-loop PROPERTIES
    # A one-second watchdog: if the stop does not land, fail in seconds, not a minute.
    LABELS "example" TIMEOUT 15
    PASS_REGULAR_EXPRESSION "TimeoutError: stopped by the watchdog after 1 s.*exit code: 1")

# An interactive session with its input piped in (tests/session.txt):
# expressions, `_`, a multi-line def ended by a blank line, a traceback, a
# class, an interceptor, a greeter, a command and :quit. Piped input is echoed
# after the prompt, so the output reads as the session would on a console.
#
# These are the lines that must come out, in this order, with anything
# between them. Metacharacters are bracket classes - `[*]` rather than `\*` -
# and the joint is `.*` rather than a newline, because neither a backslash nor
# a control character survives the trip into the generated CTest file.
set(session_lines
    ">>> 6 [*] 7"   "42"
    ">>> _ [+] 1"   "43"
    ">>> def fib[(]n[)]:"   "[.][.][.]     return n if n < 2 else"   "[.][.][.] "
    ">>> [[]fib[(]i[)] for i in range[(]10[)][]]"   "[[]0, 1, 1, 2, 3, 5, 8, 13, 21, 34[]]"
    ">>> 1 / 0"   "Traceback [(]most recent call last[)]:"   "ZeroDivisionError: division by zero"
    ">>> Counter[(]5[)][.]increment[(][)]"   "6"
    ">>> host[.]env[.]mode"   "'interactive'"
    ">>> greet[(].REPL.[)]"   "'Hi, REPL!'"
    ">>> :heap"   "used [0-9]+ B"
    ">>> :quit"   "exit code: 0")
list(JOIN session_lines ".*" session_pattern)

add_test(NAME example.python_repl.session
         COMMAND "${CMAKE_COMMAND}" -DREPL=${repl} "-DINPUT=${data}/session.txt"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.cmake")
set_tests_properties(example.python_repl.session PROPERTIES
    LABELS "example" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "${session_pattern}"
    # :quit ends the session; the line after it must never be evaluated.
    FAIL_REGULAR_EXPRESSION "never evaluated")

# The standard library is embedded (UNIBIND_PYTHON_EMBED_STDLIB): the REPL,
# copied alone into an empty directory with UNIBIND_PYTHON_HOME unset, still has
# all of it - served from the program itself, with nothing on sys.path.
if(UNIBIND_PYTHON_EMBED_STDLIB)
    add_test(NAME example.python_repl.runs-alone-from-an-empty-directory
             COMMAND "${CMAKE_COMMAND}"
                     "-DEXE=$<TARGET_FILE:unibind_python_repl>"
                     "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/standalone/$<CONFIG>"
                     "-DARGS=${data}/standalone.py"
                     -P "${PROJECT_SOURCE_DIR}/tests/python/standalone.cmake")
    set_tests_properties(example.python_repl.runs-alone-from-an-empty-directory PROPERTIES
        LABELS "example" TIMEOUT 60
        PASS_REGULAR_EXPRESSION "stdlib: frozen [[][]] ran standalone.*exit code: 0")
endif()
