# Runs the REPL for a CTest case that needs more than a command line: input
# piped from a file, or an exit code to check alongside the output.
#
#   cmake -DREPL=<exe> [-DARGS=a|b|c] [-DINPUT=<file>] -P run.cmake
#
# Prints the program's stdout and stderr, merged in the order they were
# written, then "exit code: N" - the test's PASS_REGULAR_EXPRESSION reads both.

string(REPLACE "|" ";" args "${ARGS}")
if(DEFINED INPUT)
    execute_process(COMMAND "${REPL}" ${args}
                    INPUT_FILE "${INPUT}"
                    OUTPUT_VARIABLE output ERROR_VARIABLE output
                    RESULT_VARIABLE code)
else()
    execute_process(COMMAND "${REPL}" ${args}
                    OUTPUT_VARIABLE output ERROR_VARIABLE output
                    RESULT_VARIABLE code)
endif()
message(NOTICE "${output}exit code: ${code}")
