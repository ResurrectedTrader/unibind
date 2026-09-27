# vcpkg overlay ports

The python backend takes its engine from vcpkg (the `python` feature in
`vcpkg.json`, switched on by the root `CMakeLists.txt` when
`UNIBIND_BACKEND=python`). The ports here are used **instead of** the registry's,
and only for that backend: the root `CMakeLists.txt` adds this directory to
`VCPKG_OVERLAY_PORTS` before `project()`.

## python3

CPython **3.14.7**: vcpkg's own `python3` port as vcpkg master has it for 3.14.7
(newer than the manifest's baseline, which still has 3.12), with five patches of
ours, a longer `python_vcpkg.props.in`, and a `portfile.cmake` that builds the
standard library's C extension modules **into** the static library. The
registry's patches are master's, byte for byte: 0001-0019 rebased by vcpkg for
3.14, 0020 (`Py_NO_LINK_LIB`) gone because 3.14 has it, and 0021-0023 new - they
build `_decimal` and `_zstd` against vcpkg's libmpdec and zstd instead of the
sources CPython's own Windows build fetches. `port-version` is 0; a binary-cache
entry of this port is never taken for the registry's anyway, because vcpkg's ABI
hash covers every file in the port directory.

On a `*-windows-static` triplet the port builds CPython as a static library
against the static CRT - the shape every unibind engine has. The registry port
then gives up on extension modules altogether ("a static python core cannot load
extension modules": every `.pyd` links `python3X.dll`, which does not exist).
This one compiles them in as built-in modules instead, so nothing ever looks for
a `.pyd`, and `DLLs/` does not exist.

### What each change is for

**`0100-no-whole-program-optimization.patch`.** CPython's
`PCbuild/pyproject.props` sets `/GL` (whole-program optimisation) as
`ClCompile` item metadata, which the `/p:WholeProgramOptimization=false` that
`vcpkg_msbuild_install` passes for every static library cannot override. The
Release library then consists of LTCG objects, which

* `lld-link` - what the ClangCL toolset links with - cannot read at all
  ("is not a native COFF file. Recompile without /GL?"), and
* MSVC's own `link.exe` reads only when it is the *exact* compiler version that
  made them, which is not a property a static library should have.

The patch makes that metadata (and the matching `/LTCG` link and lib settings)
respect the property, so vcpkg's intent takes effect.

**`0101-builtin-extension-modules.patch`.** What building extension modules
into the core needs of the sources:

* `PC/config.c` includes `vcpkg_builtin_modules.h`, which the portfile
  generates: a `PyInit_*` declaration per module, and `_PyImport_Inittab`
  entries for them.
* `PC/_wmimodule.cpp` passes `bstr_t` a wide literal. A narrow one calls
  `comsupp.lib`'s `ConvertStringToBSTR`, and the `wchar_t` form a native-wchar
  consumer needs is missing from older toolsets' `comsupp.lib` (14.44's, which
  this repository's ClangCL build links against): an unresolved symbol, only
  in the consumer.
* `Modules/_ctypes/callbacks.c`: a built-in `_ctypes` does not define
  `DllGetClassObject` / `DllCanUnloadNow`. In a `.pyd` they are its COM entry
  points; in a static library they would become the embedding program's, and
  collide with a COM server's own.

Under 3.12 it also made `create_builtin` refuse a single-phase built-in in a
sub-interpreter before its init ran, reported `_zoneinfo`'s missing C datetime
API as an `ImportError`, and stopped `_elementtree` compiling a second copy of
expat. 3.14 needs none of it: every extension module's init now runs under the
main interpreter and a single-phase one is refused in an isolated
sub-interpreter afterwards, built-in and `.pyd` alike; `_datetime` is
multi-phase; and the registry's 0004 now takes `_elementtree`'s bundled expat
out itself.

**`0102-asyncio-proactor-in-subinterpreters.patch`.** asyncio's default loop on
Windows, `ProactorEventLoop`, calls `signal.set_wakeup_fd` whenever it is made
on the "main thread". To `threading`, a sub-interpreter's first thread *is* its
main thread, and `set_wakeup_fd` raises outside the main interpreter - so
`asyncio.run` failed in every isolate. The loop now does that only in the main
interpreter, the only one signals are delivered to. Still so in 3.14.7.

**`0104-ssl-no-shared-static-strings.patch`.** `_ssl`'s `certEncodingType` -
reached through `ssl.enum_certificates`, which `ssl.create_default_context`
calls on Windows to read the system stores - kept `"x509_asn"` and
`"pkcs_7_asn"` in function-level `static` variables. The first interpreter to
ask made them, in its own allocator; every other one then counted references on
them from its own thread under its own GIL, and went on using them after the
first had ended and freed them. Several isolates making default contexts at once
crashed (`stdlib: ssl - default contexts made in isolates on many threads at
once`, 10 runs of 10 in Debug without the patch). The strings are made per call
now. A scan of every built-in module's sources, and `pythoncore`'s, found no
other `static` object variable. Still so in 3.14.7.

**`0105-debug-stack-margin-on-windows.patch`.** 3.14 guards recursion with the
stack pointer: a *soft* limit raises `RecursionError`, and a *hard* limit one
margin below it is a fatal error, on the assumption that no two checks are more
than a margin apart. A debug build's margin is 4096 pointers, 32 KB on x64, and
MSVC's unoptimised (`/Od`) evaluation loop spends up to about 58 KB between two
checks - so a runaway recursion through C (`sorted(key=...)`, `map`) could step
straight past the soft limit into `Fatal Python error: Unrecoverable stack
overflow`. The patch makes the margin 64 KB in a debug build on Windows, x64 and
x86 alike. Release keeps upstream's (16 KB on x64), which its 2.5-7 KB between
checks is well inside. The backend repeats the figure, in `runtime.cpp`.

(0103, which kept 3.12's `_Py_ClearStandardStreamEncoding` from swapping the
process-wide RAW allocator under running sub-interpreters, is gone: 3.13 removed
that function and the swap with it, and nothing on 3.14's interpreter start-up
path calls `_PyMem_SetDefaultAllocator` - which no longer exists. The
eight-thread repro, `concurrency_test.cpp`, runs clean in Debug without it.)

**`portfile.cmake` and `python_vcpkg.props.in`.** For a static Windows build the
portfile lists the modules to build in (`PYTHON_BUILTIN_EXTENSIONS`), rewrites
each one's `.vcxproj` from `DynamicLibrary` to `StaticLibrary`, and generates
the header above. `python_vcpkg.props` - force-imported into every project just
before `Microsoft.Cpp.targets` - then, for those projects (recognised by name:
3.14's `PCbuild` has static libraries of its own, the vendored `zlib-ng` and
`liblzma`, which the devendoring patches take out of the build), defines
`Py_BUILD_CORE_BUILTIN` (so `PyMODINIT_FUNC` is not `dllexport` and the module
compiles as part of the core), drops their version resource and their
`ProjectReference`s to `pythoncore` and `python3dll`, and empties their
librarian inputs. It gives `pythoncore` the reverse references, so the modules
build first, and hands their `.lib`s to its librarian step, which merges their
objects into `python3X.lib`. `python.exe` links the libraries below.
`vcpkg.json` makes the third-party libraries dependencies for `windows &
static` (the `extensions` feature stays what it was, for the DLL build).

`python3X.lib` holds CPython and the built-in modules and nothing else: zlib is
no longer merged into it, and neither is anything newer. The third-party
libraries stay vcpkg's own, so a program that uses openssl or sqlite itself
links one copy of each.

### The built-in modules

Built in, in addition to what the core always has (`zlib` among them):

| module | needs | in an isolate |
|---|---|---|
| `_asyncio` | | yes |
| `_overlapped` | ws2_32 | yes |
| `_socket` | ws2_32, iphlpapi, rpcrt4 | yes |
| `select` | ws2_32 | yes |
| `_ssl` | openssl (libssl, libcrypto), crypt32, ws2_32 | yes |
| `_hashlib` | openssl (libcrypto) | yes |
| `_sqlite3` | sqlite3 | yes |
| `_bz2` | bzip2 | yes |
| `_lzma` | liblzma | yes |
| `_zstd` | zstd | yes (new in 3.14: `compression.zstd`) |
| `_decimal` | libmpdec | yes |
| `_ctypes` | libffi | yes |
| `pyexpat` | expat | yes |
| `_elementtree` | (pyexpat) | yes |
| `_zoneinfo` | | yes |
| `_queue` | | yes |
| `_multiprocessing` | ws2_32 | yes |
| `_uuid` | rpcrt4 | yes |
| `_remote_debugging` | | yes (new in 3.14) |
| `unicodedata` | | yes |
| `winsound` | winmm | yes |
| `_wmi` | wbemuuid, propsys, ole32 | **no** - multi-phase, but does not declare per-interpreter-GIL support |

and of the core's own, `_tracemalloc` (single-phase init) and `_suggestions`
(declares it cannot be shared; `traceback` does without it) are refused in an
isolate too. `_msi` is gone from 3.13 on.

"In an isolate" is an own-GIL sub-interpreter with
`check_multi_interp_extensions` on - what `ub::Isolate` is. The refusals are
CPython's own judgement of the module; each import fails with `ImportError:
module X does not support loading in subinterpreters` and leaves nothing
behind. Under 3.12 the list was much longer - `_ctypes`, `_decimal`, `_msi`,
`pyexpat`, `_elementtree`, `_zoneinfo` and the core's `_datetime` besides - so an
isolate had no ctypes and no XML parsing, and `decimal` and `datetime` ran on
their pure-Python twins. In 3.14 all of those load, with state of their own per
interpreter. `tests/python/stdlib_test.cpp` exercises every row.

Left out: `_tkinter` (needs Tcl/Tk), and the test modules (`_testcapi`,
`_testlimitedcapi`, `_testinternalcapi`, `_testbuffer`, `_testimportmultiple`,
`_testmultiphase`, `_testsinglephase`, `_testconsole`, `_testclinic`,
`_testclinic_limited`, `_testembed`, `_ctypes_test`, `xxlimited`,
`xxlimited_35`).

`zoneinfo` works, but Windows has no tz database: `ZoneInfo('Europe/London')`
needs the `tzdata` package on the path. `ssl` finds the Windows certificate
stores by itself.

### What a program links

Beside `python3X.lib` (`python3X_d.lib` for Debug), from the same vcpkg prefix
- release names in `lib/`, debug names in `debug/lib/`:

| library | release | debug |
|---|---|---|
| zlib | `zs.lib` | `zsd.lib` |
| openssl | `libssl.lib`, `libcrypto.lib` | same names |
| libffi | `ffi.lib` | `ffi.lib` |
| sqlite3 | `sqlite3.lib` | `sqlite3.lib` |
| expat | `libexpatMT.lib` | `libexpatdMT.lib` |
| liblzma | `lzma.lib` | `lzma.lib` |
| bzip2 | `bz2.lib` | `bz2d.lib` |
| mpdecimal | `libmpdec.lib` | `libmpdec.lib` |
| zstd | `zstd.lib` | `zstd.lib` |

and from Windows: `version ws2_32 shlwapi pathcch bcrypt advapi32 user32
kernel32 ole32 oleaut32 iphlpapi rpcrt4 crypt32 winmm wbemuuid propsys`.

`_unibind_provide_python` in `cmake/UnibindEngines.cmake` finds all of these
(`UNIBIND_PYTHON_LIBS`, per configuration; `UNIBIND_PYTHON_SYSTEM_LIBS`), and the
install tree's `unibind-backend-python.cmake` names them too.

### Cost

Building the modules in adds openssl, libffi, sqlite3, expat, liblzma, bzip2,
zstd and mpdecimal to the first vcpkg install of a triplet - about 20 minutes on
a 32-thread machine, nearly all of it openssl and libffi (autotools under msys),
once, then binary-cached. With those cached, mpdecimal and CPython 3.14 itself,
Release and Debug, take about five minutes there. `python314.lib` (Release x64)
is 88 MB and `python314_d.lib` 98 MB (3.12's were 81 MB and 87 MB). None of the
third-party libraries is in there - they are the link inputs above.

### When the version or the baseline moves

Start from vcpkg's own `ports/python3` at the version wanted (master's, if the
baseline is behind), copied **byte for byte** - `git cat-file blob
<rev>:ports/python3/<file>`, not `git archive`, which lets `core.autocrlf`
rewrite line endings; `.gitattributes` keeps the patches `-text` here. Then
re-apply ours: the `unibind:` blocks in `portfile.cmake`, the tail of
`python_vcpkg.props.in`, and the `windows & static` dependencies in
`vcpkg.json`. Check the module list against `PCbuild/pcbuild.proj`'s
`ExtensionModules`/`ExternalModules`, and which of them declare
`Py_MOD_PER_INTERPRETER_GIL_SUPPORTED`.

To regenerate one of our patches: take the release tarball (vcpkg's
`downloads/python-cpython-v3.X.Y.tar.gz`, or the source tree a failed port run
leaves in `buildtrees/python3/src/`), `git init` it with `core.autocrlf=false`,
apply the registry patches as the portfile lists them for Windows (`git apply
--ignore-whitespace`, in order), `git add -A` to stage that state, then redo the
change in the working tree and `git diff -- <file>` it into the patch file. No
commit is needed, and each diff is against exactly the tree vcpkg will patch.
CPython 3.14's `PCbuild` files are LF in the tarball; 3.12's were CRLF.

## mpdecimal

vcpkg master's `mpdecimal` port (4.0.1), unchanged. The 3.14 port's patch 0021
builds `_decimal` against it, and the manifest's baseline predates the port.
Drop it when the baseline has it.
