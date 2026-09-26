# The CPython backend. This is the only directory in the tree that may include
# a CPython header; `unibind_headers_only` exists to keep it that way.
#
# The engine is a static /MT CPython from vcpkg - see _unibind_provide_python in
# cmake/UnibindEngines.cmake - so, like the other two, it is a link input. With
# UNIBIND_PYTHON_EMBED_STDLIB on, so is the pure-Python standard library, and
# there is nothing to deploy beside the program at all.

add_library(unibind_backend_python STATIC
    core.cpp
    values.cpp
    objects.cpp
    bindings.cpp
    runtime.cpp
    data.cpp
    module.cpp
    inspector.cpp
    stdlib_frozen.h)
add_library(unibind::backend_python ALIAS unibind_backend_python)

target_link_libraries(unibind_backend_python PUBLIC unibind_headers)
target_include_directories(unibind_backend_python SYSTEM PRIVATE "${UNIBIND_PYTHON_INCLUDE_DIR}")
target_include_directories(unibind_backend_python PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Py_NO_LINK_LIB: vcpkg's pyconfig.h otherwise names the import library in a
# `#pragma comment(lib)`, and the static library is named here instead.
target_compile_definitions(unibind_backend_python PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN Py_NO_LINK_LIB)

target_link_libraries(unibind_backend_python PUBLIC ${UNIBIND_PYTHON_LIBS} ${UNIBIND_PYTHON_SYSTEM_LIBS})

if(NOT UNIBIND_PYTHON_EMBED_STDLIB)
    # UNIBIND_PYTHON_DEFAULT_STDLIB is where the standard library was when this
    # was built: the last place the backend looks, after UNIBIND_PYTHON_HOME and
    # a `python-stdlib` directory beside the program. It is what makes a program
    # work on the machine that built it, and only there.
    target_compile_definitions(unibind_backend_python PRIVATE
        "UNIBIND_PYTHON_DEFAULT_STDLIB=\"${UNIBIND_PYTHON_STDLIB}\"")
    message(STATUS "unibind: CPython ${UNIBIND_PYTHON_VERSION}, standard library read from disk (${UNIBIND_PYTHON_STDLIB} when built)")
    return()
endif()

# --- The standard library, embedded ------------------------------------------------
#
# freeze_stdlib.py compiles every module of `Lib` the exclusion list leaves in,
# with the prefix's own interpreter - the one tools/python3 holds, which is this
# library's exact version and so writes the bytecode and the marshal format it
# reads - and generates a source that `#embed`s the result and indexes it as
# CPython's frozen-module table. Debug and Release use the same output: a
# Py_DEBUG CPython reads the same bytecode (its .pyc files are a release
# build's), and both run at optimization level 0.
#
# The interpreter is the target architecture's: on an x86 build it is the x86
# python.exe, which runs on an x64 host.

set(freezePython "${UNIBIND_PYTHON_DIR}/tools/python3/python.exe")
if(NOT EXISTS "${freezePython}")
    message(FATAL_ERROR
        "unibind: UNIBIND_PYTHON_EMBED_STDLIB needs the prefix's own interpreter to compile the standard library, "
        "and there is no ${freezePython}. Turn it off (-DUNIBIND_PYTHON_EMBED_STDLIB=OFF) to read Lib/ from disk instead.")
endif()
execute_process(
    COMMAND "${freezePython}" -I -c "import sys; print('%d.%d' % sys.version_info[:2])"
    OUTPUT_VARIABLE freezeVersion OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE freezeResult)
if(NOT freezeResult EQUAL 0 OR NOT freezeVersion STREQUAL UNIBIND_PYTHON_VERSION)
    message(FATAL_ERROR
        "unibind: ${freezePython} is CPython '${freezeVersion}' (${freezeResult}), and the library is "
        "${UNIBIND_PYTHON_VERSION}. The embedded standard library must be compiled by the same version.")
endif()

set(frozenDir "${CMAKE_CURRENT_BINARY_DIR}/stdlib")
set(frozenBlob "${frozenDir}/python_stdlib.bin")
set(frozenSource "${frozenDir}/python_stdlib.cpp")

# Everything that decides the output and is not a file under Lib: a change to any
# of it rewrites this file, and that reruns the freeze.
file(CONFIGURE OUTPUT "${frozenDir}/freeze.options" CONTENT
    "python=${freezePython}\nlib=${UNIBIND_PYTHON_STDLIB}\nexclude=${UNIBIND_PYTHON_EMBED_STDLIB_EXCLUDE}\noptimize=${UNIBIND_PYTHON_EMBED_STDLIB_OPTIMIZE}\n")
file(GLOB_RECURSE stdlibFiles CONFIGURE_DEPENDS "${UNIBIND_PYTHON_STDLIB}/*.py")

string(REPLACE ";" "," freezeExclude "${UNIBIND_PYTHON_EMBED_STDLIB_EXCLUDE}")
add_custom_command(
    OUTPUT "${frozenBlob}" "${frozenSource}"
    COMMAND "${freezePython}" -I "${CMAKE_CURRENT_SOURCE_DIR}/freeze_stdlib.py"
            --lib "${UNIBIND_PYTHON_STDLIB}"
            --blob "${frozenBlob}"
            --source "${frozenSource}"
            "--exclude=${freezeExclude}"
            --optimize "${UNIBIND_PYTHON_EMBED_STDLIB_OPTIMIZE}"
            --expect-version "${UNIBIND_PYTHON_VERSION}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/freeze_stdlib.py" "${frozenDir}/freeze.options" "${freezePython}"
            ${stdlibFiles}
    COMMENT "Freezing the CPython ${UNIBIND_PYTHON_VERSION} standard library into the backend"
    VERBATIM)

target_sources(unibind_backend_python PRIVATE "${frozenSource}" freeze_stdlib.py)
set_source_files_properties("${frozenSource}" PROPERTIES OBJECT_DEPENDS "${frozenBlob}")
set_source_files_properties(freeze_stdlib.py PROPERTIES HEADER_FILE_ONLY ON)
target_compile_definitions(unibind_backend_python PRIVATE UNIBIND_PYTHON_EMBED_STDLIB=1)

message(STATUS "unibind: CPython ${UNIBIND_PYTHON_VERSION}, standard library embedded from ${UNIBIND_PYTHON_STDLIB} "
               "(excluding ${UNIBIND_PYTHON_EMBED_STDLIB_EXCLUDE}; optimize=${UNIBIND_PYTHON_EMBED_STDLIB_OPTIMIZE})")
