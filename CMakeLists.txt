cmake_minimum_required(VERSION 3.25)

# The python backend's engine is CPython from vcpkg - the `python` manifest
# feature - so that nobody has to fetch it separately. It is asked for only when
# that backend is configured, because building CPython costs minutes that a V8
# or SpiderMonkey tree has no use for. This has to happen before `project()`,
# which is when the vcpkg toolchain installs the manifest.
#
# The port itself is overlaid: see cmake/vcpkg-ports/README.md for the one
# change, which is what makes vcpkg's static CPython linkable at all.
if(UNIBIND_BACKEND STREQUAL "python")
    if(NOT "python" IN_LIST VCPKG_MANIFEST_FEATURES)
        list(APPEND VCPKG_MANIFEST_FEATURES "python")
    endif()
    if(NOT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vcpkg-ports" IN_LIST VCPKG_OVERLAY_PORTS)
        list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vcpkg-ports")
    endif()
endif()

# vcpkg toolchain, if the caller did not already set one. UNIBIND_VCPKG_ROOT may be
# passed on the command line; otherwise VCPKG_ROOT from the environment is used.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED UNIBIND_VCPKG_ROOT)
        set(_unibind_vcpkg "${UNIBIND_VCPKG_ROOT}")
    elseif(DEFINED ENV{VCPKG_ROOT})
        set(_unibind_vcpkg "$ENV{VCPKG_ROOT}")
    endif()
    if(DEFINED _unibind_vcpkg AND EXISTS "${_unibind_vcpkg}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${_unibind_vcpkg}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    endif()
endif()

project(unibind LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
set(UNIBIND_BACKEND "v8" CACHE STRING "Engine to build against: v8 | spidermonkey | python")
set_property(CACHE UNIBIND_BACKEND PROPERTY STRINGS v8 spidermonkey python)

# The engine. Left empty, the build fetches the version below into
# dependencies/ and uses that; set either one and nothing is downloaded, which
# is how a machine that already has a build elsewhere points at it.
set(UNIBIND_V8_DIR "" CACHE PATH
    "Prebuilt V8: a directory holding include/ and v8_monolith.lib. Empty: fetch UNIBIND_V8_VERSION")
set(UNIBIND_SPIDERMONKEY_DIR "" CACHE PATH
    "Prebuilt SpiderMonkey: a directory holding include/ and spidermonkey.lib. Empty: fetch UNIBIND_SPIDERMONKEY_VERSION")
set(UNIBIND_PYTHON_DIR "" CACHE PATH
    "Static CPython in vcpkg's installed layout: include/python3.X/, lib/python3X.lib, debug/lib/python3X_d.lib, tools/python3/Lib/. Empty: vcpkg's python3 port for this triplet")

# The python backend's pure-Python standard library. Embedded, it is compiled
# into the backend library at build time by the prefix's own
# tools/python3/python.exe, and a program needs nothing beside it at run time.
# See docs/python.md, "Where the standard library comes from".
option(UNIBIND_PYTHON_EMBED_STDLIB
    "python backend: compile the pure-Python standard library into the backend, so a program ships as one executable" ON)
set(UNIBIND_PYTHON_EMBED_STDLIB_EXCLUDE
    "test;idlelib;tkinter;turtle;turtledemo;ensurepip;venv;lib2to3;pydoc_data" CACHE STRING
    "python backend: dotted module names the embedded standard library leaves out, each with everything under it")
set(UNIBIND_PYTHON_EMBED_STDLIB_OPTIMIZE "0" CACHE STRING
    "python backend: compile()'s optimize for the embedded standard library - 0 keeps asserts and docstrings, as the interpreter itself runs; 2 drops both and is about 15% smaller")
set_property(CACHE UNIBIND_PYTHON_EMBED_STDLIB_OPTIMIZE PROPERTY STRINGS 0 1 2)

set(UNIBIND_V8_VERSION "15.6.8" CACHE STRING
    "Which published V8 build to fetch: the release tag is v8-<version>")
set(UNIBIND_SPIDERMONKEY_VERSION "153.3.0esr" CACHE STRING
    "Which published SpiderMonkey build to fetch: the release tag is spidermonkey-<version>")
set(UNIBIND_ENGINE_FLAVOR "release" CACHE STRING
    "Which flavour of the published engine to fetch: release | debug. A Debug build of this tree needs debug - the static CRT must match")
set_property(CACHE UNIBIND_ENGINE_FLAVOR PROPERTY STRINGS release debug)

option(UNIBIND_FETCH_ENGINES "Fetch a published engine build when there is not one already" ON)

option(UNIBIND_BUILD_TESTS "Build the test suite" ON)
option(UNIBIND_BUILD_EXAMPLES "Build the examples that live in this tree (examples/python_repl, python backend only)" ON)
option(UNIBIND_HANDLE_CHECKS_ENABLED "Diagnose use of a handle after its scope closed (costs 4 bytes per handle)" ON)
option(UNIBIND_LTO "Link-time optimization, which is what inlines the backend into callers" OFF)

# --------------------------------------------------------------------------
# Environment checks. Both engine builds are /MT and need MSVC 14.44+ - the STL
# helpers they call live in that toolset's own libcpmt.lib, so an older one
# fails at link with undefined __std_* symbols. Fail here instead.
# --------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "unibind: the prebuilt engines are Windows static libraries; this tree is Windows-only.")
endif()

# The engines are MSVC-ABI libraries, so MinGW (an IDE's bundled default, often)
# cannot link them. clang-cl and cl both set MSVC; refuse anything else here
# rather than at a missing package or a link error much further on.
if(NOT MSVC)
    message(FATAL_ERROR
        "unibind: ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}) is not an MSVC-ABI compiler. "
        "The prebuilt engines link only with cl or clang-cl: use a Visual Studio toolchain.")
endif()

# x86 and x64 are both built and tested; anything else has no published engine
# to link and no backend that has ever been compiled for it.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(UNIBIND_ARCH "x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(UNIBIND_ARCH "x86")
else()
    message(FATAL_ERROR
        "unibind: the published engines are x86 (Win32) and x64. Configure with -A Win32 or -A x64 "
        "(sizeof(void*) is ${CMAKE_SIZEOF_VOID_P} here).")
endif()

# Toolset floor. Both engine builds were compiled against the MSVC 14.44 STL,
# whose helpers ship in that toolset's own libcpmt.lib; an older one fails at
# link with undefined __std_* symbols. This is a floor, not a preference.
#
# CMAKE_VS_PLATFORM_TOOLSET_VERSION is not set for the ClangCL toolset, which
# still uses the MSVC STL, so fall back to reading the VC tools tree that the
# compiler lives in.
set(UNIBIND_MSVC_TOOLSET_FLOOR "14.44")
set(unibind_toolset "${CMAKE_VS_PLATFORM_TOOLSET_VERSION}")
if(NOT unibind_toolset)
    string(REGEX REPLACE "/VC/Tools/.*$" "/VC/Tools/MSVC" unibind_vc_tools "${CMAKE_CXX_COMPILER}")
    file(GLOB unibind_toolsets RELATIVE "${unibind_vc_tools}" "${unibind_vc_tools}/*")
    foreach(candidate IN LISTS unibind_toolsets)
        if(IS_DIRECTORY "${unibind_vc_tools}/${candidate}" AND candidate VERSION_GREATER "${unibind_toolset}")
            set(unibind_toolset "${candidate}")
        endif()
    endforeach()
endif()
if(unibind_toolset)
    if(unibind_toolset VERSION_LESS UNIBIND_MSVC_TOOLSET_FLOOR)
        message(FATAL_ERROR
            "unibind: MSVC toolset ${unibind_toolset} is below the floor of ${UNIBIND_MSVC_TOOLSET_FLOOR}. "
            "The prebuilt engines call STL helpers that ship only in that toolset's libcpmt.lib.")
    endif()
    message(STATUS "unibind: MSVC toolset ${unibind_toolset} (floor ${UNIBIND_MSVC_TOOLSET_FLOOR})")
else()
    message(STATUS "unibind: MSVC toolset version unknown; the prebuilt engines need ${UNIBIND_MSVC_TOOLSET_FLOOR} or newer.")
endif()
# Both engines link the static CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --------------------------------------------------------------------------
# Generated configuration
#
# Handle size and the inline storage sizes change the layout of a public type,
# so they are generated once for the whole build tree rather than passed
# per-target. See include/unibind/config.h.in.
#
# What is deliberately *not* in there is which engine this tree builds. The
# generated header is the same file for every backend of an architecture, so a
# consumer compiles their own objects once and chooses an engine at the final
# link; `Platform::BackendName()` answers which one at runtime, which is where
# that question belongs. `UNIBIND_BACKEND` selects what this tree builds and
# installs, and nothing else.
# --------------------------------------------------------------------------
if(NOT UNIBIND_BACKEND MATCHES "^(v8|spidermonkey|python)$")
    message(FATAL_ERROR "unibind: unknown UNIBIND_BACKEND '${UNIBIND_BACKEND}' (expected v8, spidermonkey or python)")
endif()

# One value for the whole build tree: it changes sizeof(Local<T>), so a
# per-config or per-target answer would be an ODR trap. See docs/lifetimes.md.
if(UNIBIND_HANDLE_CHECKS_ENABLED)
    set(UNIBIND_HANDLE_CHECKS 1)
else()
    set(UNIBIND_HANDLE_CHECKS 0)
endif()

# A frame is a handle scope's inline storage. These are checked by a
# static_assert in the backend, so a too-small number is a build error.
#
# They are measured, not guessed. Each type here is mostly pointers, so every
# one of them grows on x64; what the two backends actually come out at, in
# bytes, with the storage sizes set to 1 so the asserts print the real figure:
#
#                          x86   x64
#   V8            Frame     84   160
#   SpiderMonkey  Frame    120   152
#   V8     TryCatchState    28    56
#   SM     TryCatchState    88   128
#   V8 ContextScopeState     4     8
#   SM ContextScopeState     8    16
#
# **Each number is the maximum across every backend for this architecture, and
# is not a per-backend figure.** That is the rule, and it is load-bearing
# rather than tidy: a consumer compiles these sizes into their own stack frames
# once and links whichever backend they like, so a number that varied by engine
# would make `HandleScope` a different type per engine and the single compile a
# lie. A backend whose frame is larger therefore raises the number **for
# everyone** rather than getting one of its own - the cost is stack bytes in a
# scope the caller already had, and the alternative is the silent mismatch the
# ABI tag below exists to prevent. Plus room for an engine version to add a
# field, since running out is a compile error in the backend and never a silent
# heap fallback.
#
# Architecture is the one thing they do vary by, because a build is one
# architecture and always was: an x86 prefix and an x64 prefix are separate
# installs, so x86 is not charged for x64's growth.
#
# Two of the three needed no split at all: the x86 numbers were generous enough
# that the larger x64 types still fit under them, and an alignment of 8 was
# always what a pointer wanted.
set(UNIBIND_FRAME_STORAGE_SIZE 256)
set(UNIBIND_FRAME_STORAGE_ALIGN 8)
set(UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE 64)
set(UNIBIND_CONTEXT_SCOPE_STORAGE_ALIGN 8)
set(UNIBIND_TRY_CATCH_STORAGE_ALIGN 8)

# TryCatch is the one that had to move. SpiderMonkey's x64 TryCatchState is 128
# bytes, which is exactly the x86 number: it fits, with nothing at all to spare,
# and a number with no headroom is one engine bump away from being a build
# failure someone has to come back and measure again.
if(UNIBIND_ARCH STREQUAL "x64")
    set(UNIBIND_TRY_CATCH_STORAGE_SIZE 192)
else()
    set(UNIBIND_TRY_CATCH_STORAGE_SIZE 128)
endif()

# Not architecture-dependent, and that is checked rather than assumed. It is
# SpiderMonkey's StackGCVector inline capacity, which GCVector.h fixes at a
# literal 8 elements and mozilla::Vector only reduces for an element type large
# enough to blow a 1 KB budget - JS::Value is 8 bytes on both architectures, so
# the answer is 8 on both. The backend static_asserts the two agree.
set(UNIBIND_FRAME_INLINE_SLOTS 8)

# The generated header carries one more thing than the template does: a linker
# directive naming every value above. Those macros set the layout of `Local<T>`,
# `HandleScope`, `TryCatch` and `ContextScope`, and a consumer compiled against
# a `config.h` that disagreed with the library it calls would lay those types
# out one way while the library reads them another. It is the one mismatch that
# produces neither a compile error nor a link error on its own: the sizes are
# the caller's, the code reading them is the library's, and the result is silent
# corruption.
#
# `#pragma detect_mismatch` puts the string into every object file's linker
# directives; two objects that disagree are LNK2038 at link time. It is what the
# MSVC STL uses for `_ITERATOR_DEBUG_LEVEL` and `RuntimeLibrary`, and clang-cl
# implements it. It lives here rather than in `config.h.in` because it is the
# install/export layer's guarantee, not the API's.
#
# **The tag names what is genuinely ABI and nothing else**, which is why the
# backend is not in it. An object compiled against these headers is the same
# object whichever backend it is linked with - no public header branches on the
# engine, and the storage budgets are the maximum across all of them - so a tag
# carrying the backend's name refuses a legitimate cross-backend link and
# catches nothing a real mismatch would not also trip. What is left is the set
# a difference in which really is corruption: the architecture, the handle
# layout, and the three storage budgets with the frame's slot count.
#
# The frame's inline slot count is in there as part of the frame's description,
# though only a backend ever reads it - what reaches a caller's stack is the
# total storage, not how the backend divides it up. It cannot differ for a
# legitimate link either way, because a `config.h` and the library built from
# it always come out of one tree.
#
# The architecture is in the tag for the same reason the sizes are. Nothing in
# an installed prefix's path says which architecture it holds, so an x86 header
# over an x64 library is reachable by accident - and it is the worst case of
# all, because `Local<T>`, `HandleScope` and every pointer in the backend change
# size together. MSVC already refuses to link objects of different machine
# types, but the header is read before that: this makes the mismatch LNK2038
# with a sentence naming it rather than a machine-type error naming an object
# file.
set(UNIBIND_ABI_TAG
    "@UNIBIND_ARCH@/hc@UNIBIND_HANDLE_CHECKS@/f@UNIBIND_FRAME_STORAGE_SIZE@:@UNIBIND_FRAME_STORAGE_ALIGN@/t@UNIBIND_TRY_CATCH_STORAGE_SIZE@:@UNIBIND_TRY_CATCH_STORAGE_ALIGN@/c@UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE@:@UNIBIND_CONTEXT_SCOPE_STORAGE_ALIGN@/s@UNIBIND_FRAME_INLINE_SLOTS@")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/unibind/config.h.in" unibindConfigTemplate)
string(APPEND unibindConfigTemplate "
// ---------------------------------------------------------------------------
// ABI guard - appended by CMakeLists.txt, not part of config.h.in.
//
// Everything above sets the layout of a public type. A translation unit that
// sees a different copy of this header is not a link error by itself: it would
// lay a HandleScope or a Local<T> out one way while the library it calls reads
// it another. So the whole set goes into the object file as a linker
// directive, and disagreeing objects fail to link (LNK2038) instead.
//
// It names no backend, on purpose. An object compiled against these headers is
// the same object whichever backend it is linked against, so the tag has to
// let a cross-backend link through - and it still refuses the mismatches that
// really are corruption.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#pragma detect_mismatch(\"unibind_abi\", \"${UNIBIND_ABI_TAG}\")
#endif
")

file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/unibind/config.h"
    CONTENT "${unibindConfigTemplate}"
    @ONLY)

# --------------------------------------------------------------------------
# The public API: headers only, no engine anywhere - and no backend either.
# This is the same target an installed prefix exports as `unibind::headers`,
# and it is what a consumer compiles against when their objects are going to be
# linked more than once.
# --------------------------------------------------------------------------
add_library(unibind_headers INTERFACE)
add_library(unibind::headers ALIAS unibind_headers)
target_include_directories(unibind_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(unibind_headers INTERFACE cxx_std_23)

# The enforcement that keeps a second engine possible, and the enforcement that
# keeps one compile serving both. The first compiles every public header on its
# own with no engine include directory on the command line, so a header that
# reaches an engine header - directly or transitively - fails here. The second
# reads them for any mention of the backend, which cannot be a compile error
# and so cannot be caught that way. Both at build time, not at review time.
add_subdirectory(tools/headers_only)

# --------------------------------------------------------------------------
# The backend, and the engine under it
#
# The engine is resolved after the environment checks above and before anything
# is compiled: there is no point fetching a quarter of a gigabyte for a tree
# that was going to be refused for being x64 anyway.
# --------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/UnibindEngines.cmake")
unibind_provide_engine(${UNIBIND_BACKEND})

add_subdirectory(src/backends/${UNIBIND_BACKEND})

add_library(unibind INTERFACE)
add_library(unibind::unibind ALIAS unibind)
target_link_libraries(unibind INTERFACE unibind_headers unibind_backend_${UNIBIND_BACKEND})

if(UNIBIND_LTO)
    set_property(TARGET unibind_backend_${UNIBIND_BACKEND} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(UNIBIND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# examples/embed is built against an *installed* prefix and is not part of this
# tree. The python REPL is: it needs the python backend and is registered as a
# test of it (label "example").
if(UNIBIND_BUILD_EXAMPLES AND UNIBIND_BACKEND STREQUAL "python")
    add_subdirectory(examples/python_repl)
endif()

# --------------------------------------------------------------------------
# Installing
#
# What a consumer needs is the public headers, the generated `config.h`, a
# backend library, and a list of link inputs nobody should assemble by hand.
#
# **One include tree, and the backend is a link input.** `config.h` sits beside
# the public headers because it describes the architecture and the handle
# layout and nothing about an engine, so it is the same file whichever backend
# wrote it. Installing a second backend into the same prefix therefore adds one
# library and rewrites an identical header: a consumer compiles their own
# objects once and picks an engine at the link.
#
# Two consumption paths, and the MSBuild one is the one that matters: a
# `.vcxproj` imports `msbuild/unibind.props`. `find_package(unibind)` is the same
# information for a CMake consumer.
# --------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

set(UNIBIND_INSTALL_INCLUDE_DIR "include")
set(UNIBIND_INSTALL_LIB_DIR "lib")
set(UNIBIND_INSTALL_CMAKE_DIR "lib/cmake/unibind")
set(UNIBIND_INSTALL_MSBUILD_DIR "msbuild")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/unibind"
        DESTINATION "${UNIBIND_INSTALL_INCLUDE_DIR}"
        FILES_MATCHING PATTERN "*.h")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/unibind/config.h"
        DESTINATION "${UNIBIND_INSTALL_INCLUDE_DIR}/unibind")

install(TARGETS unibind_backend_${UNIBIND_BACKEND}
        ARCHIVE DESTINATION "${UNIBIND_INSTALL_LIB_DIR}")

# The engine's own library and the system libraries it was built against. A
# consumer supplies the engine; this is the list they would otherwise transcribe
# from dependencies/README.md and get wrong. The engine directory is recorded as
# a default and can be overridden - `UnibindV8Dir` in MSBuild, `UNIBIND_V8_DIR` in CMake
# - because the prefix may be copied to a machine where the engine lives
# somewhere else.
set(UNIBIND_ENGINE_EXTRA_LIB_NAMES "")
# Extra properties for this backend's MSBuild engine fragment. Only CPython has
# any: its link inputs are more than one library, and differ by configuration.
set(UNIBIND_MSBUILD_ENGINE_EXTRA "")
if(UNIBIND_BACKEND STREQUAL "v8")
    set(UNIBIND_ENGINE_DIR "${UNIBIND_V8_DIR}")
    set(UNIBIND_ENGINE_LIB_NAME "v8_monolith.lib")
    set(UNIBIND_MSBUILD_ENGINE_DIR_PROP "UnibindV8Dir")
    set(UNIBIND_ENGINE_SYSTEM_LIBS
        winmm dbghelp advapi32 shlwapi ws2_32 user32 kernel32
        ole32 oleaut32 psapi version ntdll userenv bcrypt)
elseif(UNIBIND_BACKEND STREQUAL "python")
    # A static /MT CPython from vcpkg, like the other two engines a static
    # library. What it does need at run time is its standard library, the
    # `Lib/` directory - see docs/python.md.
    set(UNIBIND_ENGINE_DIR "${UNIBIND_PYTHON_DIR}")
    set(UNIBIND_ENGINE_LIB_NAME "lib/${UNIBIND_PYTHON_LIB_NAME}")
    set(UNIBIND_MSBUILD_ENGINE_DIR_PROP "UnibindPythonDir")
    set(UNIBIND_ENGINE_SYSTEM_LIBS ${UNIBIND_PYTHON_SYSTEM_LIBS})
    # The third-party libraries its built-in extension modules call (zlib,
    # openssl, ...), from the same prefix - see _unibind_provide_python.
    set(UNIBIND_ENGINE_EXTRA_LIB_NAMES ${UNIBIND_PYTHON_DEP_LIB_NAMES})
    # For MSBuild: the engine's libraries relative to UnibindPythonDir, one list
    # per CRT flavour - a Debug (/MTd) project links python3X_d.lib and the
    # debug builds of its dependencies from debug/lib.
    string(REPLACE ".lib" "_d.lib" unibindPythonDebugLib "debug/lib/${UNIBIND_PYTHON_LIB_NAME}")
    set(unibindPythonRelease "lib/${UNIBIND_PYTHON_LIB_NAME}" ${UNIBIND_PYTHON_DEP_LIB_NAMES})
    set(unibindPythonDebug "${unibindPythonDebugLib}" ${UNIBIND_PYTHON_DEP_DEBUG_LIB_NAMES})
    list(JOIN unibindPythonRelease ";" unibindPythonRelease)
    list(JOIN unibindPythonDebug ";" unibindPythonDebug)
    string(REPLACE "/" "\\" unibindPythonRelease "${unibindPythonRelease}")
    string(REPLACE "/" "\\" unibindPythonDebug "${unibindPythonDebug}")
    string(CONCAT UNIBIND_MSBUILD_ENGINE_EXTRA
        "    <UnibindPythonLibs Condition=\"'$(UnibindPythonLibs)' == ''\">${unibindPythonRelease}</UnibindPythonLibs>\n"
        "    <UnibindPythonDebugLibs Condition=\"'$(UnibindPythonDebugLibs)' == ''\">${unibindPythonDebug}</UnibindPythonDebugLibs>")
else()
    set(UNIBIND_ENGINE_DIR "${UNIBIND_SPIDERMONKEY_DIR}")
    set(UNIBIND_ENGINE_LIB_NAME "spidermonkey.lib")
    set(UNIBIND_MSBUILD_ENGINE_DIR_PROP "UnibindSpiderMonkeyDir")
    set(UNIBIND_ENGINE_SYSTEM_LIBS
        mincore ws2_32 advapi32 user32 ole32 oleaut32 shell32 userenv
        bcrypt ntdll dbghelp psapi winmm shlwapi)
endif()

# Two spellings of one list, because the two build systems want different
# separators and different suffixes.
set(UNIBIND_ENGINE_SYSTEM_LIBS_CMAKE "")
set(UNIBIND_ENGINE_SYSTEM_LIBS_MSBUILD "")
foreach(lib IN LISTS UNIBIND_ENGINE_SYSTEM_LIBS)
    string(APPEND UNIBIND_ENGINE_SYSTEM_LIBS_CMAKE "${lib};")
    string(APPEND UNIBIND_ENGINE_SYSTEM_LIBS_MSBUILD "${lib}.lib;")
endforeach()

set(UNIBIND_BACKEND_LIB_NAME "unibind_backend_${UNIBIND_BACKEND}.lib")
string(TOUPPER "${UNIBIND_BACKEND}" UNIBIND_BACKEND_UPPER)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/unibind-backend.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-backend-${UNIBIND_BACKEND}.cmake" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/unibind-config.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-config.cmake" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/unibind.props.in"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind.props" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/unibind-engine.props.in"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-engine-${UNIBIND_BACKEND}.props" @ONLY)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-config-version.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMinorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-backend-${UNIBIND_BACKEND}.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-config-version.cmake"
        DESTINATION "${UNIBIND_INSTALL_CMAKE_DIR}")

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind.props"
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/unibind-engine-${UNIBIND_BACKEND}.props"
        DESTINATION "${UNIBIND_INSTALL_MSBUILD_DIR}")

message(STATUS "unibind: backend=${UNIBIND_BACKEND} arch=${UNIBIND_ARCH} handle-checks=${UNIBIND_HANDLE_CHECKS} lto=${UNIBIND_LTO}")
