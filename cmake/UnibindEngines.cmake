# Getting an engine.
#
# Neither engine is in this repository and neither can be, so a clone used to
# need a hand-placed directory before it could configure at all. Both are
# published as GitHub releases of prebuilt static libraries, which is enough for
# the build to fetch the one it was configured for.
#
# Three properties the implementation is shaped by:
#
#   * Only when absent. A directory that is already there is used as it stands
#     and never re-downloaded, whether it was unpacked here, junctioned in from
#     elsewhere, or pointed at with UNIBIND_V8_DIR / UNIBIND_SPIDERMONKEY_DIR.
#   * Whole or not there. The archive is unpacked into a staging sibling and
#     renamed into place in one move, so an interrupted or failed fetch leaves
#     no half-unpacked directory that a later configure would mistake for a
#     complete one.
#   * Version-derived. The release tag, the asset name and the directory all
#     come from UNIBIND_<ENGINE>_VERSION, so changing a version repoints every one
#     of them and fetches the new library rather than reusing the old one.

# Part of every published asset name, and the toolset floor the engines were
# built against - see the floor check in the root CMakeLists.txt.
set(UNIBIND_ENGINE_TOOLSET "msvc14.44")

# sha256 and size of each asset this tree knows how to ask for, so that a
# damaged or substituted download fails here rather than at link time, and so
# the message before a quarter-gigabyte download can say how big it is.
# `gh release view <tag> --repo <repo> --json assets` prints both. An asset with
# no entry here is still fetched; it just says that it could not be verified.
set(_unibind_asset_v8-15.6.8-x86-release-msvc14.44.zip
    "df7f4f1a6b21b7093fedf252016d1514170f010097b3d324407e9a98fa2c4fba;263")
set(_unibind_asset_v8-15.6.8-x86-debug-msvc14.44.zip
    "8a6d24b1542a9b4a038ce7c3c29e63295b7dd2df28ae1f11907e37590ee14471;418")
set(_unibind_asset_v8-15.6.8-x64-release-msvc14.44.zip
    "65822dd31235025d0f9eb518fc2595f6a36967f82aa5bffa867aff61a639d1a3;277")
set(_unibind_asset_v8-15.6.8-x64-debug-msvc14.44.zip
    "b7f02de590fb2f46dd03cf08bcae2fc0ca3014eb7df6d57fd3bca4a25b69e47b;462")
set(_unibind_asset_spidermonkey-153.3.0esr-x86-release-msvc14.44.zip
    "8fb82896f9c649aef1c42c73e891c597a49f1be62c43eb5664da9481c71844b8;141")
set(_unibind_asset_spidermonkey-153.3.0esr-x86-debug-msvc14.44.zip
    "1c3e644f4818a9d49a82647a023b92dff0a42b750ba4010fcbffb60eb45109c7;227")
set(_unibind_asset_spidermonkey-153.3.0esr-x64-release-msvc14.44.zip
    "a0ca05ef96fca61ebdd576a1817792f7bb53263c0bad822aa75a1ca413c89e7f;143")
set(_unibind_asset_spidermonkey-153.3.0esr-x64-debug-msvc14.44.zip
    "105ec737ca9c65f0fcb281e4d0a25231be3e304eefc7218a364274f6e467c1c3;238")

# Everything that differs between the two engines, in one place: where it is
# published, what the directory it unpacks to is called, and the two files that
# say a directory really holds an engine rather than the remains of one.
function(_unibind_engine_facts engine)
    # Decided by the architecture check in the root CMakeLists.txt, so there is
    # one answer rather than two that can drift apart.
    set(arch "${UNIBIND_ARCH}")
    string(TOLOWER "${UNIBIND_ENGINE_FLAVOR}" flavour)
    if(NOT flavour MATCHES "^(release|debug)$")
        message(FATAL_ERROR "unibind: UNIBIND_ENGINE_FLAVOR is '${UNIBIND_ENGINE_FLAVOR}' (expected release or debug)")
    endif()

    if(engine STREQUAL "v8")
        set(version "${UNIBIND_V8_VERSION}")
        set(repo "ResurrectedTrader/v8-static-win")
        set(tag "v8-${version}")
        set(lib "v8_monolith.lib")
        set(header "include/v8.h")
        # dependencies/v8/<version>/<arch>-<flavour>/, which is one directory per
        # version holding every flavour of it.
        set(dir "${CMAKE_SOURCE_DIR}/dependencies/v8/${version}/${arch}-${flavour}")
    elseif(engine STREQUAL "spidermonkey")
        set(version "${UNIBIND_SPIDERMONKEY_VERSION}")
        set(repo "ResurrectedTrader/spidermonkey-static-win")
        set(tag "spidermonkey-${version}")
        set(lib "spidermonkey.lib")
        set(header "include/jsapi.h")
        set(dir "${CMAKE_SOURCE_DIR}/dependencies/spidermonkey/${version}-${arch}-${flavour}")
    else()
        message(FATAL_ERROR "unibind: no such engine '${engine}'")
    endif()

    set(asset "${tag}-${arch}-${flavour}-${UNIBIND_ENGINE_TOOLSET}.zip")

    set(unibindEngineVersion "${version}" PARENT_SCOPE)
    set(unibindEngineArch "${arch}" PARENT_SCOPE)
    set(unibindEngineFlavour "${flavour}" PARENT_SCOPE)
    set(unibindEngineLib "${lib}" PARENT_SCOPE)
    set(unibindEngineHeader "${header}" PARENT_SCOPE)
    set(unibindEngineDir "${dir}" PARENT_SCOPE)
    set(unibindEngineAsset "${asset}" PARENT_SCOPE)
    set(unibindEngineUrl "https://github.com/${repo}/releases/download/${tag}/${asset}" PARENT_SCOPE)
endfunction()

# Download the asset, unpack it, and move the result into place as one step.
#
# Nothing is written to `dir` until the whole archive has been unpacked and the
# library and headers have been found inside it, so a fetch that is interrupted
# - a cancelled configure, a dropped connection, a full disk - leaves `dir`
# absent and the next configure starts over. The staging directory is a sibling
# of `dir` so that the move is a rename within one volume rather than a copy.
function(_unibind_fetch_engine engine)
    _unibind_engine_facts("${engine}")

    set(sha256 "")
    set(megabytes "")
    if(DEFINED "_unibind_asset_${unibindEngineAsset}")
        list(GET "_unibind_asset_${unibindEngineAsset}" 0 sha256)
        list(GET "_unibind_asset_${unibindEngineAsset}" 1 megabytes)
        set(sizeText "${megabytes} MB")
    else()
        set(sizeText "a few hundred MB")
    endif()

    get_filename_component(parent "${unibindEngineDir}" DIRECTORY)
    get_filename_component(name "${unibindEngineDir}" NAME)
    set(staging "${parent}/.${name}.incoming")
    set(archive "${staging}/${unibindEngineAsset}")

    if(engine STREQUAL "v8")
        set(optOut "UNIBIND_V8_DIR")
    else()
        set(optOut "UNIBIND_SPIDERMONKEY_DIR")
    endif()

    message(STATUS "unibind: no ${engine} ${unibindEngineVersion} (${unibindEngineArch} ${unibindEngineFlavour}) at ${unibindEngineDir}")
    message(STATUS "unibind: fetching ${sizeText} from ${unibindEngineUrl}")
    message(STATUS "unibind: once per version, and never again while that directory is there; "
                   "-D${optOut}=<path> uses a build you already have instead")

    file(REMOVE_RECURSE "${staging}")
    file(MAKE_DIRECTORY "${staging}")

    if(sha256)
        file(DOWNLOAD "${unibindEngineUrl}" "${archive}"
             SHOW_PROGRESS TLS_VERIFY ON
             EXPECTED_HASH "SHA256=${sha256}"
             STATUS downloadStatus)
    else()
        message(STATUS "unibind: no recorded sha256 for ${unibindEngineAsset}; the download cannot be verified")
        file(DOWNLOAD "${unibindEngineUrl}" "${archive}"
             SHOW_PROGRESS TLS_VERIFY ON
             STATUS downloadStatus)
    endif()
    list(GET downloadStatus 0 downloadCode)
    if(NOT downloadCode EQUAL 0)
        list(GET downloadStatus 1 downloadMessage)
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR
            "unibind: could not fetch ${unibindEngineUrl}: ${downloadMessage}\n"
            "Fetch it by hand into ${unibindEngineDir}, or point -D${optOut}=<path> at a build you have.")
    endif()

    message(STATUS "unibind: unpacking ${unibindEngineAsset}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${staging}/unpacked")
    file(REMOVE "${archive}")

    # The archive's internal layout is the publisher's business: find the library
    # and take the directory holding it, rather than assuming a top-level name.
    file(GLOB_RECURSE found LIST_DIRECTORIES false "${staging}/unpacked/${unibindEngineLib}")
    if(NOT found)
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR "unibind: ${unibindEngineAsset} holds no ${unibindEngineLib}")
    endif()
    list(GET found 0 library)
    get_filename_component(root "${library}" DIRECTORY)
    if(NOT EXISTS "${root}/${unibindEngineHeader}")
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR "unibind: ${unibindEngineAsset} has no ${unibindEngineHeader} beside its ${unibindEngineLib}")
    endif()

    file(MAKE_DIRECTORY "${parent}")
    file(RENAME "${root}" "${unibindEngineDir}" RESULT moved NO_REPLACE)
    file(REMOVE_RECURSE "${staging}")
    if(NOT moved STREQUAL "NO_ERROR" AND NOT EXISTS "${unibindEngineDir}/${unibindEngineLib}")
        message(FATAL_ERROR "unibind: could not move the unpacked engine into ${unibindEngineDir}: ${moved}")
    endif()
endfunction()

# Resolve where this build's engine is, fetching it if it is not anywhere yet,
# and leave the answer in UNIBIND_V8_DIR / UNIBIND_SPIDERMONKEY_DIR for the backend, the
# install rules and the parity trees to read.
#
# The knob stays what it always was: set the variable and nothing is downloaded,
# whatever is or is not under dependencies/.
function(unibind_provide_engine engine)
    if(engine STREQUAL "python")
        _unibind_provide_python()
        foreach(out UNIBIND_PYTHON_DIR UNIBIND_PYTHON_INCLUDE_DIR UNIBIND_PYTHON_LIB_NAME UNIBIND_PYTHON_LIBS
                    UNIBIND_PYTHON_DEP_LIB_NAMES UNIBIND_PYTHON_DEP_DEBUG_LIB_NAMES
                    UNIBIND_PYTHON_SYSTEM_LIBS UNIBIND_PYTHON_STDLIB UNIBIND_PYTHON_VERSION)
            set(${out} "${${out}}" PARENT_SCOPE)
        endforeach()
        return()
    endif()
    if(engine STREQUAL "v8")
        set(variable "UNIBIND_V8_DIR")
    else()
        set(variable "UNIBIND_SPIDERMONKEY_DIR")
    endif()

    if(${variable})
        message(STATUS "unibind: ${variable} is set; nothing is downloaded")
        return()
    endif()

    _unibind_engine_facts("${engine}")
    if(NOT EXISTS "${unibindEngineDir}/${unibindEngineLib}")
        if(NOT UNIBIND_FETCH_ENGINES)
            message(FATAL_ERROR
                "unibind: no ${engine} at ${unibindEngineDir} and UNIBIND_FETCH_ENGINES is OFF.\n"
                "Unpack ${unibindEngineAsset} there, or point -D${variable}=<path> at a build you have.")
        endif()
        _unibind_fetch_engine("${engine}")
    endif()

    # A directory-scope variable rather than a cache entry: a cached answer would
    # outlive the version it was derived from, and the next configure after a
    # version bump would keep using the old tree. The cache entry is the knob,
    # and it stays empty unless someone sets it.
    set(${variable} "${unibindEngineDir}" PARENT_SCOPE)
endfunction()

# What a translation unit that includes SpiderMonkey's headers has to define, so
# that it lays the engine's types out as the library was built to. Applied to
# every target that includes them - the backend and the fatal-path test - so
# there is one list and not two to drift apart.
#
# STATIC_JS_API and XP_WIN are not optional: the headers lay types out
# differently without them, and the mismatch mostly does not show up as a
# compile error. MOZ_STATIC_JS is the same switch under mozilla-config's name.
#
# ENABLE_EXPLICIT_RESOURCE_MANAGEMENT is the trap. The prebuilt library was
# compiled with it, but js-config.h - which is the header bundle's record of
# what the build enabled - does not mention it. It gates an enumerator in the
# middle of JSExnType (JSEXN_SUPPRESSEDERR) and entries in JSProtoKey and
# JS::SymbolCode, so without it every enumerator after that point means
# something different here from what it means inside the library: asking for a
# TypeError produces a SyntaxError. It compiles cleanly either way. See
# docs/spidermonkey.md.
#
# A debug engine adds two. DEBUG, because it is configured with --enable-debug
# and its js-config.h refuses to be included without it. And
# MOZ_DIAGNOSTIC_ASSERT_ENABLED, which is the same trap again: a debug build
# turns diagnostic assertions on, js-config.h does not say so, and the public
# headers lay types out by it - `JS::AutoAssertNoGC` carries the context it
# asserts against and defines its constructor and destructor in the library,
# instead of being an empty class with inline ones. Without it the backend
# builds an object of the wrong size, and the link says only "duplicate symbol",
# because the inline constructor meets the library's out-of-line one.
function(unibind_spidermonkey_definitions target)
    target_compile_definitions(${target} PRIVATE
        STATIC_JS_API MOZ_STATIC_JS XP_WIN ENABLE_EXPLICIT_RESOURCE_MANAGEMENT)
    string(TOLOWER "${UNIBIND_ENGINE_FLAVOR}" flavour)
    if(flavour STREQUAL "debug")
        target_compile_definitions(${target} PRIVATE DEBUG MOZ_DIAGNOSTIC_ASSERT_ENABLED)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# CPython
#
# The third engine is not a published archive: it is vcpkg's `python3` port,
# pulled in through this repository's `python` manifest feature (the root
# CMakeLists.txt turns that on for UNIBIND_BACKEND=python). On the
# `*-windows-static` triplets the port builds CPython as a **static library
# against the static CRT** - exactly the shape of the other two engines - so
# there is nothing to download by hand and no DLL to deploy.
#
# A static CPython on Windows cannot load C extension modules: every `.pyd`
# links `python3X.dll`, which does not exist here. So the overlay port in
# cmake/vcpkg-ports/python3 compiles the standard library's extension modules
# into the static library as built-in modules instead - `_socket`, `select`,
# `_asyncio`, `_overlapped` and the rest; cmake/vcpkg-ports/README.md has the
# list - and nothing ever looks for a `.pyd`.
#
# What a program does need at run time is the pure-Python half of the standard
# library, `tools/python3/Lib`. `UNIBIND_PYTHON_STDLIB` records where it is, and
# the backend bakes that in as its default - see docs/python.md, "Where the
# standard library comes from".
#
# UNIBIND_PYTHON_DIR, when set, points at another prefix with the same layout
# (include/python3.X/, lib/, debug/lib/, tools/python3/Lib/), and vcpkg is not
# consulted.
# ---------------------------------------------------------------------------
function(_unibind_provide_python)
    set(prefix "${UNIBIND_PYTHON_DIR}")
    if(NOT prefix)
        if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
            message(FATAL_ERROR
                "unibind: the python backend takes CPython from vcpkg, and this configure has no vcpkg toolchain. "
                "Set UNIBIND_VCPKG_ROOT (or VCPKG_ROOT), or point -DUNIBIND_PYTHON_DIR=<prefix> at a static CPython.")
        endif()
        if(NOT VCPKG_TARGET_TRIPLET MATCHES "-static$")
            message(FATAL_ERROR
                "unibind: VCPKG_TARGET_TRIPLET is '${VCPKG_TARGET_TRIPLET}'. The python backend needs the static-CRT "
                "CPython the *-windows-static triplets build; the presets set x86-windows-static / x64-windows-static.")
        endif()
        set(prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    endif()

    file(GLOB includeDirs LIST_DIRECTORIES true "${prefix}/include/python3.*")
    list(FILTER includeDirs INCLUDE REGEX "/python3\.[0-9]+$")
    if(NOT includeDirs)
        message(FATAL_ERROR "unibind: no include/python3.X under '${prefix}'. Is vcpkg's python feature installed?")
    endif()
    list(GET includeDirs 0 includeDir)
    string(REGEX MATCH "python(3)\.([0-9]+)$" _ "${includeDir}")
    set(version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
    set(tag "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")

    set(release "${prefix}/lib/python${tag}.lib")
    set(debug "${prefix}/debug/lib/python${tag}_d.lib")
    if(NOT EXISTS "${release}")
        message(FATAL_ERROR "unibind: no lib/python${tag}.lib under '${prefix}'")
    endif()
    set(stdlib "${prefix}/tools/python3/Lib")
    if(NOT EXISTS "${stdlib}/os.py")
        message(FATAL_ERROR "unibind: no standard library at '${stdlib}'")
    endif()

    # The static library holds CPython and the extension modules the overlay port
    # builds into it (cmake/vcpkg-ports/README.md), and nothing else: the
    # third-party libraries those modules call are vcpkg's own static libraries
    # for the same triplet, linked beside it. Each is a pair - the release name
    # in lib/ and the debug name in debug/lib/ - and one that this prefix does
    # not have is left out (a CPython built without the module that needs it
    # does not need it either).
    set(libs "")
    set(depNames "")
    set(depDebugNames "")
    if(EXISTS "${debug}")
        list(APPEND libs "$<$<CONFIG:Debug>:${debug}>" "$<$<NOT:$<CONFIG:Debug>>:${release}>")
    else()
        list(APPEND libs "${release}")
    endif()
    foreach(dep IN ITEMS
            "zlib|zs zlib z|zsd zlibd zd"
            "openssl-ssl|libssl|libssl"
            "openssl-crypto|libcrypto|libcrypto"
            "libffi|ffi libffi|ffi libffi"
            "sqlite3|sqlite3|sqlite3"
            "expat|libexpatMT libexpat|libexpatdMT libexpatd"
            "liblzma|lzma|lzma"
            "bzip2|bz2|bz2d"
            "mpdecimal|libmpdec|libmpdec"
            "zstd|zstd|zstdd zstd")
        string(REPLACE "|" ";" dep "${dep}")
        list(GET dep 0 depName)
        list(GET dep 1 relNames)
        list(GET dep 2 dbgNames)
        string(REPLACE " " ";" relNames "${relNames}")
        string(REPLACE " " ";" dbgNames "${dbgNames}")
        unset(depRelease)
        unset(depDebug)
        find_library(depRelease NAMES ${relNames} PATHS "${prefix}/lib" NO_DEFAULT_PATH NO_CACHE)
        find_library(depDebug NAMES ${dbgNames} PATHS "${prefix}/debug/lib" NO_DEFAULT_PATH NO_CACHE)
        if(depRelease AND depDebug)
            list(APPEND libs "$<$<CONFIG:Debug>:${depDebug}>" "$<$<NOT:$<CONFIG:Debug>>:${depRelease}>")
        elseif(depRelease)
            list(APPEND libs "${depRelease}")
        else()
            continue()
        endif()
        file(RELATIVE_PATH depRel "${prefix}" "${depRelease}")
        list(APPEND depNames "${depRel}")
        if(depDebug)
            file(RELATIVE_PATH depRel "${prefix}" "${depDebug}")
        endif()
        list(APPEND depDebugNames "${depRel}")
    endforeach()

    set(UNIBIND_PYTHON_DIR "${prefix}" PARENT_SCOPE)
    set(UNIBIND_PYTHON_INCLUDE_DIR "${includeDir}" PARENT_SCOPE)
    set(UNIBIND_PYTHON_LIB_NAME "python${tag}.lib" PARENT_SCOPE)
    set(UNIBIND_PYTHON_LIBS "${libs}" PARENT_SCOPE)
    # The engine's other link inputs, relative to the prefix, release flavour -
    # what the install tree's package files name beside python3X.lib.
    set(UNIBIND_PYTHON_DEP_LIB_NAMES "${depNames}" PARENT_SCOPE)
    # The same, debug flavour: what a Debug (/MTd) consumer links instead.
    set(UNIBIND_PYTHON_DEP_DEBUG_LIB_NAMES "${depDebugNames}" PARENT_SCOPE)
    # What the core, its built-in modules and openssl call in Windows itself:
    #   version shlwapi pathcch bcrypt advapi32 user32 kernel32 ole32 oleaut32 - the core
    #   ws2_32 - _socket, select, _overlapped, _multiprocessing, _ssl, openssl
    #   iphlpapi - _socket          rpcrt4 - _socket, _uuid
    #   crypt32 - _ssl, openssl     winmm - winsound
    #   wbemuuid propsys - _wmi
    set(UNIBIND_PYTHON_SYSTEM_LIBS
        version ws2_32 shlwapi pathcch bcrypt advapi32 user32 kernel32 ole32 oleaut32
        iphlpapi rpcrt4 crypt32 winmm wbemuuid propsys PARENT_SCOPE)
    set(UNIBIND_PYTHON_STDLIB "${stdlib}" PARENT_SCOPE)
    set(UNIBIND_PYTHON_VERSION "${version}" PARENT_SCOPE)
    message(STATUS "unibind: CPython ${version} (static, /MT) at ${prefix}")
endfunction()
