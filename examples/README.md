# Examples

Two programs:

- **`embed/`**, below: the smallest useful embedding, built against an
  *installed* prefix to prove the install is complete.
- **[`python_repl/`](python_repl/README.md)**: an interactive Python prompt over
  the CPython backend. Its host bindings cover most of the binding API: classes,
  templates with inheritance, interceptors, accessors, typed arrays, structured
  clone, timers and promises, and stopping a script from another thread. It has a
  `--demo` script that exercises all of them. It is built inside the library's
  tree when `UNIBIND_BACKEND=python`
  (`cmake --build build/python-x64 --config Release --target unibind_python_repl`),
  and its CTest cases carry the label `example`.

## embed

`embed/main.cpp`: a platform, an isolate, a realm, a native class
bound into it, a script, a JavaScript function called from C++, an exception
caught, and the pump that promise continuations wait for.

What makes it worth having is not the program, it is **how it is built**. It is
built against an *installed* unibind - the prefix a stranger would be handed - and
not against this source tree, so a missing header, a missing library or a
half-written property sheet fails here rather than in someone else's afternoon.

### Install first

```powershell
cmake --preset v8
cmake --build build/v8 --config Release --parallel 1
cmake --install build/v8 --config Release --prefix build/install
```

Installing the SpiderMonkey tree into the *same* prefix is supported and is
worth doing once: one prefix then holds both backends, the MSBuild example
switches between them with one property, and the CMake example builds **both**
out of one compile. The headers and the generated `config.h` are shared - they
name no engine - and only the library differs.

The examples below are `Win32`, matching an x86 prefix. An x64 prefix wants
`-p:Platform=x64` and `-A x64`; the prefix knows which it was installed for and
both consumption paths refuse the other one rather than letting the link fail.

### MSBuild, which is the path that matters

```powershell
msbuild examples\embed\embed.vcxproj -p:Configuration=Release -p:Platform=Win32 `
        -p:UnibindPrefix=<repo>\build\install\ -m:1
```

The whole of consuming unibind is the one `<Import>` in that project's
`PropertySheets` group. Add `-p:UnibindBackend=spidermonkey` to build the same
source against the other engine; the output is identical except for the backend
name and the wording of the engine's own error message.

Two things the project has to say for itself, both marked in the file:
`PreferredToolArchitecture=x64` (a 32-bit `lld-link` cannot link an engine this
size, and does not say so - see the README's gotchas) and the static CRT.

### CMake, for a CMake consumer

```powershell
cmake -S examples/embed -B build/example-cmake -G "Visual Studio 17 2022" `
      -A Win32 -T ClangCL -DCMAKE_PREFIX_PATH=<repo>/build/install
cmake --build build/example-cmake --config Release --parallel 1
```

**This is the one that proves the link-time claim.** `main.cpp` is compiled
once, into an object library that links `unibind::headers` and names no engine;
then every backend the prefix holds gets an executable built from those same
objects - `embed_v8.exe` and `embed_spidermonkey.exe`. Run both: the same
object code prints a different backend.

A consumer who ships one engine writes `find_package(unibind REQUIRED)` and
`target_link_libraries(app PRIVATE unibind::unibind)`, and adds
`-DUNIBIND_BACKEND=spidermonkey` to choose which. The three-target shape is in
the root README.

### What it prints

```
backend: v8 15.6.8
the counter says 42
the script evaluated to 42
describe() said: the answer is 42
as expected: Uncaught TypeError: a counter starts at zero or above
before the pump: ran = false
after the pump:  ran = true
```

The last two lines are the point of including promises at all: the continuation
does not run until `PumpJobs` runs it.
