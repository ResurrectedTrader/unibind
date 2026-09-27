# Runs unibind_bench on V8, SpiderMonkey and CPython, one after another, and
# merges the three into the markdown tables README "What it costs" shows.
#
#     cmake -P tests/bench/Compare.cmake
#
# from the repository root, once the three x64 trees are built in Release
# (`cmake --build build/<preset> --config Release --target unibind_bench`).
# Each run's CSV and the merged tables are written to a new directory under
# build/bench-compare/, named for the time it was made, and the tables are
# printed as well. Each engine is run ROUNDS times, round-robin, and each figure
# is the median over its rounds of the medians each run reports. The V8 runs
# also carry each row's raw twin - the operation written against V8 directly -
# and a third table sets the two side by side: what the binding costs over the
# engine itself.
#
#   -DBUILD_ROOT=<dir>      where the build trees are (default: build)
#   -DPRESETS=a;b;c         the trees, in V8, SpiderMonkey, CPython order
#                           (default: v8-x64;spidermonkey-x64;python-x64)
#   -DCONFIG=<config>       (default: Release)
#   -DROUNDS=<n>            runs of each engine (default: 3)
#   -DBENCH_ARGS=<args>     passed to each run, `;`-separated (e.g. --target-ms;300)
#   -DCSV_DIR=<dir>         merge the <engine>-<round>.csv files already in <dir>
#                           (v8-1.csv, python-2.csv, ...) rather than running
#
# The runs are sequential on purpose. Run it on a machine that is otherwise
# idle: nothing here can tell a slow engine from a busy core.

cmake_minimum_required(VERSION 3.25)

set(unibindBackends v8 spidermonkey python)

if(NOT DEFINED BUILD_ROOT)
    set(BUILD_ROOT "build")
endif()
if(NOT DEFINED PRESETS)
    set(PRESETS v8-x64 spidermonkey-x64 python-x64)
endif()
if(NOT DEFINED CONFIG)
    set(CONFIG Release)
endif()
if(NOT DEFINED ROUNDS)
    set(ROUNDS 3)
endif()
if(NOT DEFINED BENCH_ARGS)
    set(BENCH_ARGS "")
endif()

# ---------------------------------------------------------------------------
# Numbers. CMake's arithmetic is integer only, so every figure is carried as
# thousandths of a nanosecond, which is what the CSV's three decimals give.
# ---------------------------------------------------------------------------

# "19.390" -> 19390
function(unibind_milli text out)
    if(NOT text MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?$")
        message(FATAL_ERROR "not a number: '${text}'")
    endif()
    set(sign "${CMAKE_MATCH_1}")
    set(whole "${CMAKE_MATCH_2}")
    string(SUBSTRING "${CMAKE_MATCH_4}000" 0 3 fraction)
    math(EXPR value "${whole} * 1000 + ${fraction}")
    if(sign STREQUAL "-")
        math(EXPR value "0 - ${value}")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# 1234567 -> "1,234,567"
function(unibind_group value out)
    set(text "${value}")
    set(grouped "")
    string(LENGTH "${text}" length)
    while(length GREATER 3)
        math(EXPR cut "${length} - 3")
        string(SUBSTRING "${text}" ${cut} 3 tail)
        string(SUBSTRING "${text}" 0 ${cut} text)
        set(grouped ",${tail}${grouped}")
        string(LENGTH "${text}" length)
    endwhile()
    set(${out} "${text}${grouped}" PARENT_SCOPE)
endfunction()

# Thousandths of a nanosecond, as a nanosecond figure with as many decimals as
# mean anything: two below 10 ns, one below 1000, none above.
function(unibind_format_ns milli out)
    if(milli LESS 10000)
        math(EXPR rounded "(${milli} + 5) / 10")
        math(EXPR whole "${rounded} / 100")
        math(EXPR fraction "${rounded} % 100")
        if(fraction LESS 10)
            set(fraction "0${fraction}")
        endif()
        set(text "${whole}.${fraction}")
    elseif(milli LESS 1000000)
        math(EXPR rounded "(${milli} + 50) / 100")
        math(EXPR whole "${rounded} / 10")
        math(EXPR fraction "${rounded} % 10")
        set(text "${whole}.${fraction}")
    else()
        math(EXPR rounded "(${milli} + 500) / 1000")
        unibind_group(${rounded} text)
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# A net figure. Below half a nanosecond it is inside the noise of subtracting
# one loop from another, and on a JIT it usually means the operation was hoisted
# out of the loop or folded away, so it is shown as that rather than as a number.
function(unibind_format_net milli out)
    if(milli STREQUAL "")
        set(${out} "" PARENT_SCOPE)
    elseif(milli LESS 500)
        set(${out} "<0.5" PARENT_SCOPE)
    else()
        unibind_format_ns(${milli} text)
        set(${out} "${text}" PARENT_SCOPE)
    endif()
endfunction()

# numerator / denominator as "12×", "1.4×" or, below a tenth, "0.002×".
function(unibind_ratio numerator denominator out)
    if(denominator LESS_EQUAL 0)
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    math(EXPR tenths "(${numerator} * 10 + ${denominator} / 2) / ${denominator}")
    math(EXPR thousandths "(${numerator} * 1000 + ${denominator} / 2) / ${denominator}")
    if(thousandths LESS 100)
        if(thousandths LESS 10)
            set(thousandths "00${thousandths}")
        else()
            set(thousandths "0${thousandths}")
        endif()
        set(text "0.${thousandths}")
    elseif(tenths GREATER_EQUAL 100)
        math(EXPR whole "(${tenths} + 5) / 10")
        unibind_group(${whole} text)
    else()
        math(EXPR whole "${tenths} / 10")
        math(EXPR fraction "${tenths} % 10")
        set(text "${whole}.${fraction}")
    endif()
    set(${out} "${text}×" PARENT_SCOPE)
endfunction()

# The median of a list of non-negative integers.
function(unibind_median values out)
    list(SORT values COMPARE NATURAL)
    list(LENGTH values count)
    math(EXPR middle "${count} / 2")
    list(GET values ${middle} upper)
    if(count GREATER 0 AND NOT count MATCHES "[13579]$")
        math(EXPR below "${middle} - 1")
        list(GET values ${below} lower)
        math(EXPR upper "(${lower} + ${upper}) / 2")
    endif()
    set(${out} "${upper}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Run, or take the CSVs given
#
# Round-robin: every engine once, then every engine again. Whatever else the
# machine is doing then falls on all three alike rather than on whichever one
# happened to be running when it started.
# ---------------------------------------------------------------------------

if(DEFINED CSV_DIR)
    set(outDir "${CSV_DIR}")
else()
    string(TIMESTAMP stamp "%Y%m%d-%H%M%S")
    set(outDir "${BUILD_ROOT}/bench-compare/${stamp}")
    if(EXISTS "${outDir}")
        message(FATAL_ERROR "${outDir} already exists; not writing over it")
    endif()
    file(MAKE_DIRECTORY "${outDir}")
    foreach(round RANGE 1 ${ROUNDS})
        foreach(backend preset IN ZIP_LISTS unibindBackends PRESETS)
            set(exe "${BUILD_ROOT}/${preset}/tests/${CONFIG}/unibind_bench.exe")
            if(NOT EXISTS "${exe}")
                message(FATAL_ERROR "no ${exe}; build the unibind_bench target of ${preset} in ${CONFIG} first")
            endif()
            message(STATUS "round ${round} of ${ROUNDS}: ${exe}")
            execute_process(COMMAND "${exe}" --csv ${BENCH_ARGS}
                            OUTPUT_FILE "${outDir}/${backend}-${round}.csv"
                            RESULT_VARIABLE status)
            if(NOT status EQUAL 0)
                message(FATAL_ERROR "${exe} failed: ${status}")
            endif()
        endforeach()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Merge: per engine and row, the median over the rounds. A net figure is the
# row's median less the empty loop's.
# ---------------------------------------------------------------------------

set(ids "")
foreach(backend IN LISTS unibindBackends)
    file(GLOB runs "${outDir}/${backend}-*.csv")
    if(NOT runs)
        message(FATAL_ERROR "no ${outDir}/${backend}-*.csv")
    endif()
    foreach(run IN LISTS runs)
        file(STRINGS "${run}" lines)
        foreach(line IN LISTS lines)
            # A run written on Windows ends its lines in CRLF.
            string(REPLACE "\r" "" line "${line}")
            if(line MATCHES "^backend,")
                continue()
            endif()
            # backend,side,id,label,ns,net_ns,iterations,spread[,raw_ns]
            #
            # raw_ns is the V8 build's raw twin of the row - the same operation
            # written against V8 directly - and empty everywhere else. A row
            # with the side `raw` has no unibind figure: it is measured against
            # V8 alone, and is only shown beside the others in the V8 table.
            # CMake keeps ten match groups, so the columns not read here are
            # not captured.
            if(NOT line MATCHES "^([^,]*),([^,]*),([^,]*),([^,]*),([^,]*),[^,]*,[^,]*,[^,]*(,([^,]*))?$")
                message(FATAL_ERROR "${run}: cannot read '${line}'")
            endif()
            set(side "${CMAKE_MATCH_2}")
            set(id "${CMAKE_MATCH_3}")
            set(label "${CMAKE_MATCH_4}")
            set(ns "${CMAKE_MATCH_5}")
            set(raw "${CMAKE_MATCH_7}")
            if(NOT id IN_LIST ids)
                list(APPEND ids "${id}")
                set(side_${id} "${side}")
                set(label_${id} "${label}")
            endif()
            if(NOT side STREQUAL "raw")
                unibind_milli("${ns}" milli)
                list(APPEND samples_${backend}_${id} "${milli}")
            endif()
            if(NOT raw STREQUAL "")
                unibind_milli("${raw}" milli)
                list(APPEND rawsamples_${backend}_${id} "${milli}")
            endif()
        endforeach()
    endforeach()
    foreach(id IN LISTS ids)
        if(DEFINED samples_${backend}_${id})
            unibind_median("${samples_${backend}_${id}}" median)
            set(ns_${backend}_${id} "${median}")
        endif()
        if(DEFINED rawsamples_${backend}_${id})
            unibind_median("${rawsamples_${backend}_${id}}" median)
            set(raw_${backend}_${id} "${median}")
        endif()
    endforeach()
    foreach(id IN LISTS ids)
        if(side_${id} STREQUAL "script" AND NOT id STREQUAL "empty-loop" AND DEFINED ns_${backend}_${id}
           AND DEFINED ns_${backend}_empty-loop)
            math(EXPR net "${ns_${backend}_${id}} - ${ns_${backend}_empty-loop}")
            set(net_${backend}_${id} "${net}")
        endif()
    endforeach()
    list(LENGTH runs rounds_${backend})
endforeach()

set(markdown "")
foreach(side script native)
    if(side STREQUAL "script")
        string(APPEND markdown
            "| script-side, ns per iteration | V8 | SpiderMonkey | CPython | V8 net | SpiderMonkey net | CPython net | CPython ÷ V8 | CPython ÷ SpiderMonkey |\n"
            "|---|--:|--:|--:|--:|--:|--:|--:|--:|\n")
    else()
        string(APPEND markdown
            "\n| C++-side, ns per operation | V8 | SpiderMonkey | CPython | CPython ÷ V8 | CPython ÷ SpiderMonkey |\n"
            "|---|--:|--:|--:|--:|--:|\n")
    endif()
    foreach(id IN LISTS ids)
        if(NOT side_${id} STREQUAL side)
            continue()
        endif()
        set(row "| ${label_${id}}")
        foreach(backend IN LISTS unibindBackends)
            if(DEFINED ns_${backend}_${id})
                unibind_format_ns(${ns_${backend}_${id}} text)
            else()
                set(text "")
            endif()
            string(APPEND row " | ${text}")
        endforeach()
        if(side STREQUAL "script")
            foreach(backend IN LISTS unibindBackends)
                if(DEFINED net_${backend}_${id})
                    unibind_format_net("${net_${backend}_${id}}" text)
                else()
                    set(text "")
                endif()
                string(APPEND row " | ${text}")
            endforeach()
        endif()
        foreach(other v8 spidermonkey)
            if(DEFINED ns_python_${id} AND DEFINED ns_${other}_${id})
                unibind_ratio(${ns_python_${id}} ${ns_${other}_${id}} text)
            else()
                set(text "")
            endif()
            string(APPEND row " | ${text}")
        endforeach()
        string(APPEND markdown "${row} |\n")
    endforeach()
endforeach()

# What unibind costs over V8 itself: each row beside its raw twin, from the same
# V8 runs. Written only when those runs carried twins.
set(anyRaw FALSE)
foreach(id IN LISTS ids)
    if(DEFINED raw_v8_${id})
        set(anyRaw TRUE)
    endif()
endforeach()
if(anyRaw)
    string(APPEND markdown
        "\n| V8: unibind against V8's own API, ns | ub:: | raw V8 | overhead | ub:: ÷ raw |\n"
        "|---|--:|--:|--:|--:|\n")
    foreach(side script native)
        foreach(id IN LISTS ids)
            if(NOT DEFINED raw_v8_${id})
                continue()
            endif()
            # A raw-only row sits with the script rows, where it belongs.
            if(side_${id} STREQUAL "raw")
                set(rowSide script)
            else()
                set(rowSide "${side_${id}}")
            endif()
            if(NOT rowSide STREQUAL side)
                continue()
            endif()
            unibind_format_ns(${raw_v8_${id}} rawText)
            if(DEFINED ns_v8_${id})
                unibind_format_ns(${ns_v8_${id}} ubText)
                math(EXPR overhead "${ns_v8_${id}} - ${raw_v8_${id}}")
                if(overhead LESS 0)
                    math(EXPR magnitude "0 - ${overhead}")
                    unibind_format_ns(${magnitude} overheadText)
                    set(overheadText "−${overheadText}")
                else()
                    unibind_format_ns(${overhead} overheadText)
                endif()
                unibind_ratio(${ns_v8_${id}} ${raw_v8_${id}} ratioText)
            else()
                set(ubText "—")
                set(overheadText "")
                set(ratioText "")
            endif()
            string(APPEND markdown "| ${label_${id}} | ${ubText} | ${rawText} | ${overheadText} | ${ratioText} |\n")
        endforeach()
    endforeach()
endif()

file(WRITE "${outDir}/tables.md" "${markdown}")
message("${markdown}")
message(STATUS "written to ${outDir}")
