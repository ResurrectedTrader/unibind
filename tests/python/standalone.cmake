# Runs a program from a copy of it in an empty directory, with
# UNIBIND_PYTHON_HOME unset: what a program shipped as one executable meets on
# a machine that has never seen the build tree. Used by the CTest cases that
# prove the embedded standard library needs nothing on disk.
#
#   cmake -DEXE=<program> -DDIR=<scratch directory> [-DARGS=a|b|c] -P standalone.cmake
#
# DIR is emptied first, and nothing but the program is put in it. Prints the
# program's output then "exit code: N", and fails if the exit code is not 0.

if(NOT EXE OR NOT DIR)
    message(FATAL_ERROR "standalone.cmake: EXE and DIR are required")
endif()

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
get_filename_component(name "${EXE}" NAME)
file(COPY_FILE "${EXE}" "${DIR}/${name}")

unset(ENV{UNIBIND_PYTHON_HOME})
string(REPLACE "|" ";" args "${ARGS}")
execute_process(COMMAND "${DIR}/${name}" ${args}
                WORKING_DIRECTORY "${DIR}"
                OUTPUT_VARIABLE output ERROR_VARIABLE output
                RESULT_VARIABLE code)
message(NOTICE "${output}exit code: ${code}")
if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}, run alone from ${DIR}, failed")
endif()
