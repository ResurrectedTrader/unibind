# Run the one suite against every backend this tree can build, and put the
# results side by side.
#
# Parity is the point of the suite, and parity is not something a
# single build tree can observe: the engine is a compile-time choice, so "the
# same test on both engines" means two build trees. This script owns that. It
# builds the trees it needs, runs each suite through the `unibind-parity` reporter,
# and prints one row per test case with one column per backend.
#
# It never rebuilds the backend it was invoked from - that binary is already
# built, and building it twice would double the cost of the command that
# matters.
#
#   cmake -DUNIBIND_SOURCE_DIR=... -DUNIBIND_CURRENT_BACKEND=v8 -DUNIBIND_CURRENT_EXE=...
#         -DUNIBIND_WORK_DIR=... [-DUNIBIND_PARITY_BUILD=ON] -P RunParity.cmake

cmake_minimum_required(VERSION 3.25)

foreach(required UNIBIND_SOURCE_DIR UNIBIND_CURRENT_BACKEND UNIBIND_CURRENT_EXE UNIBIND_WORK_DIR UNIBIND_ARCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "RunParity: ${required} is not set")
    endif()
endforeach()

if(NOT DEFINED UNIBIND_CONFIG OR UNIBIND_CONFIG STREQUAL "")
    set(UNIBIND_CONFIG "Release")
endif()
if(NOT DEFINED UNIBIND_PARITY_BUILD)
    set(UNIBIND_PARITY_BUILD OFF)
endif()
if(NOT DEFINED UNIBIND_REPORT)
    set(UNIBIND_REPORT "${UNIBIND_WORK_DIR}/parity-report.md")
endif()

file(MAKE_DIRECTORY "${UNIBIND_WORK_DIR}")

# Passed as one argument with `|` between entries, because a `;` would have
# been split by the shell that started this script.
if(DEFINED UNIBIND_CACHE_ARGS)
    string(REPLACE "|" ";" UNIBIND_CACHE_ARGS "${UNIBIND_CACHE_ARGS}")
endif()

# ---------------------------------------------------------------------------
# Which backends exist at all. Discovered, not listed: a third backend joins
# the comparison by existing.
# ---------------------------------------------------------------------------
file(GLOB backendDirs "${UNIBIND_SOURCE_DIR}/src/backends/*")
set(backends "")
foreach(dir IN LISTS backendDirs)
    get_filename_component(name "${dir}" NAME)
    # The python backend's scripts are Python, so the JavaScript suite this
    # compares means nothing to it; it has its own (tests/python).
    if(EXISTS "${dir}/CMakeLists.txt" AND NOT name STREQUAL "python")
        list(APPEND backends "${name}")
    endif()
endforeach()
list(SORT backends)
if(NOT backends)
    message(FATAL_ERROR "RunParity: no backend under ${UNIBIND_SOURCE_DIR}/src/backends")
endif()

# Where this backend's build tree is, or should be: an existing tree that was
# configured for it - `build/<backend>` and `build/<backend>-x64` are what the
# presets make - and otherwise a tree of this command's own.
#
# The architecture has to match as well as the backend. A handle is a different
# size in the two and each links its own engine, so an x64 suite compared
# against an x86 one would be a comparison of two different libraries wearing
# one column heading.
#
# What a tree was configured for is asked of the tree itself: the architecture
# from the ABI tag in its generated `unibind/config.h` - the same string the
# linker uses to refuse a mismatched consumer - and the backend from its cache,
# which is where the choice is actually recorded. The tag deliberately does not
# name a backend any more, because an object compiled against these headers
# links against either one.
function(unibind_tree_for backend out)
    foreach(candidate
            "${UNIBIND_SOURCE_DIR}/build/${backend}"
            "${UNIBIND_SOURCE_DIR}/build/${backend}-${UNIBIND_ARCH}"
            "${UNIBIND_SOURCE_DIR}/build/parity-${backend}-${UNIBIND_ARCH}")
        set(config "${candidate}/generated/unibind/config.h")
        set(cache "${candidate}/CMakeCache.txt")
        if(EXISTS "${config}" AND EXISTS "${cache}")
            file(STRINGS "${config}" tag REGEX "unibind_abi")
            file(STRINGS "${cache}" configured REGEX "^UNIBIND_BACKEND:")
            if(tag MATCHES "\"${UNIBIND_ARCH}/" AND configured MATCHES "=${backend}$")
                set(${out} "${candidate}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
    set(${out} "${UNIBIND_SOURCE_DIR}/build/parity-${backend}-${UNIBIND_ARCH}" PARENT_SCOPE)
endfunction()

function(unibind_find_test_exe tree out)
    file(GLOB_RECURSE candidates "${tree}/unibind_tests.exe" "${tree}/unibind_tests")
    set(best "")
    foreach(candidate IN LISTS candidates)
        if(NOT IS_DIRECTORY "${candidate}")
            set(best "${candidate}")
        endif()
    endforeach()
    set(${out} "${best}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# One executable per backend: the current one as built, the others in trees of
# their own so nothing collides with the tree this was invoked from.
# ---------------------------------------------------------------------------
set(available "")
set(unavailable "")
set(exeList "")

foreach(backend IN LISTS backends)
    if(backend STREQUAL UNIBIND_CURRENT_BACKEND)
        list(APPEND available "${backend}")
        list(APPEND exeList "${UNIBIND_CURRENT_EXE}")
        continue()
    endif()

    unibind_tree_for("${backend}" tree)
    if(UNIBIND_PARITY_BUILD)
        message(STATUS "unibind parity: configuring ${backend} in ${tree}")
        set(configureArgs
            -S "${UNIBIND_SOURCE_DIR}" -B "${tree}"
            -DUNIBIND_BACKEND=${backend}
            -DUNIBIND_BUILD_TESTS=ON)
        if(UNIBIND_GENERATOR)
            list(APPEND configureArgs -G "${UNIBIND_GENERATOR}")
        endif()
        if(UNIBIND_GENERATOR_PLATFORM)
            list(APPEND configureArgs -A "${UNIBIND_GENERATOR_PLATFORM}")
        endif()
        if(UNIBIND_GENERATOR_TOOLSET)
            list(APPEND configureArgs -T "${UNIBIND_GENERATOR_TOOLSET}")
        endif()
        foreach(passthrough IN LISTS UNIBIND_CACHE_ARGS)
            list(APPEND configureArgs "${passthrough}")
        endforeach()

        execute_process(COMMAND "${CMAKE_COMMAND}" ${configureArgs}
                        RESULT_VARIABLE configured
                        OUTPUT_VARIABLE configureLog ERROR_VARIABLE configureLog)
        if(NOT configured EQUAL 0)
            file(WRITE "${UNIBIND_WORK_DIR}/parity-${backend}-configure.log" "${configureLog}")
            message(STATUS "unibind parity: ${backend} does not configure; see parity-${backend}-configure.log")
            list(APPEND unavailable "${backend}")
            continue()
        endif()

        # One compiler process: the machine this runs on is in use.
        message(STATUS "unibind parity: building ${backend} - this takes a while")
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${tree}"
                                --config "${UNIBIND_CONFIG}" --target unibind_tests --parallel 1
                        RESULT_VARIABLE built
                        OUTPUT_VARIABLE buildLog ERROR_VARIABLE buildLog)
        file(WRITE "${UNIBIND_WORK_DIR}/parity-${backend}-build.log" "${buildLog}")
        if(NOT built EQUAL 0)
            message(STATUS "unibind parity: ${backend} does not build; see parity-${backend}-build.log")
            list(APPEND unavailable "${backend}")
            continue()
        endif()
    endif()

    unibind_find_test_exe("${tree}" exe)
    if(exe STREQUAL "")
        list(APPEND unavailable "${backend}")
    else()
        list(APPEND available "${backend}")
        list(APPEND exeList "${exe}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Run each suite through the parity reporter
# ---------------------------------------------------------------------------
set(allCases "")

list(LENGTH available count)
math(EXPR lastIndex "${count} - 1")
foreach(index RANGE ${lastIndex})
    list(GET available ${index} backend)
    list(GET exeList ${index} exe)
    set(resultFile "${UNIBIND_WORK_DIR}/parity-${backend}.txt")
    file(REMOVE "${resultFile}")

    execute_process(COMMAND "${exe}" "--reporters=unibind-parity" "--out=${resultFile}"
                    RESULT_VARIABLE ran
                    OUTPUT_VARIABLE runLog ERROR_VARIABLE runLog)
    if(NOT EXISTS "${resultFile}")
        message(FATAL_ERROR "unibind parity: ${backend} produced no results (exit ${ran}):\n${runLog}")
    endif()

    file(STRINGS "${resultFile}" rows)
    set(names_${backend} "")
    set(states_${backend} "")
    foreach(row IN LISTS rows)
        if(row MATCHES "^(PASSED|FAILED|SKIPPED)[|](.+)$")
            list(APPEND states_${backend} "${CMAKE_MATCH_1}")
            list(APPEND names_${backend} "${CMAKE_MATCH_2}")
            list(APPEND allCases "${CMAKE_MATCH_2}")
        endif()
    endforeach()
endforeach()

list(REMOVE_DUPLICATES allCases)
list(SORT allCases)

# ---------------------------------------------------------------------------
# The matrix
# ---------------------------------------------------------------------------
function(unibind_state backend case out)
    list(FIND names_${backend} "${case}" at)
    if(at EQUAL -1)
        set(${out} "ABSENT" PARENT_SCOPE)
    else()
        list(GET states_${backend} ${at} value)
        set(${out} "${value}" PARENT_SCOPE)
    endif()
endfunction()

set(header "| test case |")
set(rule "|---|")
foreach(backend IN LISTS available)
    string(APPEND header " ${backend} |")
    string(APPEND rule "---|")
endforeach()

set(report "# Parity report\n\nOne row per test case, one column per backend.\n\n")
string(APPEND report "${header}\n${rule}\n")

set(divergences "")
set(gaps "")
set(failures "")

foreach(case IN LISTS allCases)
    set(row "| ${case} |")
    set(seen "")
    foreach(backend IN LISTS available)
        unibind_state("${backend}" "${case}" state)
        string(APPEND row " ${state} |")
        list(APPEND seen "${state}")
        if(state STREQUAL "FAILED")
            list(APPEND failures "${backend}: ${case}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES seen)
    list(LENGTH seen distinct)
    if(distinct GREATER 1)
        if("PASSED" IN_LIST seen AND "FAILED" IN_LIST seen)
            list(APPEND divergences "${case}")
        else()
            list(APPEND gaps "${case}")
        endif()
    endif()
    string(APPEND report "${row}\n")
endforeach()

if(unavailable)
    list(JOIN unavailable ", " unavailableText)
    string(APPEND report "\nNot compared: ${unavailableText} - no built test binary.\n")
endif()

file(WRITE "${UNIBIND_REPORT}" "${report}")

# ---------------------------------------------------------------------------
# What a human reads
# ---------------------------------------------------------------------------
list(LENGTH allCases caseCount)
list(JOIN available ", " availableText)
message(STATUS "")
message(STATUS "unibind parity: ${caseCount} cases; compared ${availableText}")
if(unavailable)
    list(JOIN unavailable ", " unavailableText)
    message(STATUS "unibind parity: not compared: ${unavailableText}")
endif()

foreach(case IN LISTS allCases)
    if(case IN_LIST divergences OR case IN_LIST gaps)
        set(line "")
        foreach(backend IN LISTS available)
            unibind_state("${backend}" "${case}" state)
            string(SUBSTRING "${state}         " 0 9 padded)
            string(APPEND line "${padded}")
        endforeach()
        if(case IN_LIST divergences)
            message(STATUS "  ${line} DIVERGES  ${case}")
        else()
            message(STATUS "  ${line} gap       ${case}")
        endif()
    endif()
endforeach()

message(STATUS "unibind parity: report written to ${UNIBIND_REPORT}")

if(divergences)
    list(LENGTH divergences divergenceCount)
    message(FATAL_ERROR
        "unibind parity: ${divergenceCount} case(s) pass on one backend and fail on another. "
        "That is the failure this command exists to find; see ${UNIBIND_REPORT}.")
endif()
if(failures)
    list(JOIN failures "\n  " failureText)
    message(FATAL_ERROR "unibind parity: failing cases:\n  ${failureText}")
endif()
if(count LESS 2)
    message(STATUS
        "unibind parity: only one backend was comparable, so this run says nothing about parity yet. "
        "A backend joins the comparison by existing under src/backends and building.")
endif()
