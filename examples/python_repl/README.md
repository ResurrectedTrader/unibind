# A Python prompt over unibind

`unibind_python_repl` is an interactive Python prompt built on the CPython backend.
Each input is a `ub::Evaluate` in one realm (a sub-interpreter's globals dict), and
that realm holds C++ bindings registered with the ordinary `ub::` API: an
`ObjectTemplate` of functions, two `Class<T>`s, `FunctionTemplate`s with
inheritance, named and indexed interceptors, accessors, typed arrays, structured
clone, timers and promises. Nothing in `host.cpp` is specific to Python. It is
the same binding code you would write for V8, and the scripts that call it happen
to be Python.

Read the files in this order:

| file | what it shows |
|---|---|
| [`host.cpp`](host.cpp) | **the bindings**, one small function per API feature, and `InstallHost`, which declares them all |
| [`demo.py`](demo.py) | every binding exercised from Python, with the expected results asserted |
| [`main.cpp`](main.cpp) | the REPL: evaluation, printing, pumping jobs, and stopping a script from another thread |

## Build and run

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset python-x64
cmake --build build/python-x64 --config Release --target unibind_python_repl

build\python-x64\examples\python_repl\Release\unibind_python_repl.exe            # the prompt
build\python-x64\examples\python_repl\Release\unibind_python_repl.exe --demo     # the tour
```

The root `CMakeLists.txt` adds this directory when `UNIBIND_BACKEND` is `python`
and `UNIBIND_BUILD_EXAMPLES` is on, which is the default. Unlike
[`examples/embed`](../embed), it is built in the library's own tree rather than
against an installed prefix, because it is also a test of the backend.

```
unibind_python_repl [--timeout SECONDS] [--demo | -c CODE | FILE [ARGS...]]

  (nothing)       an interactive prompt
  FILE [ARGS]     run a Python file; sys.argv is [FILE, ARGS...]
  -c CODE         run CODE
  --demo          run the bundled demo.py
  --timeout S     stop any single evaluation that runs longer than S seconds
```

A file, `-c` or `--demo` exits 0 on success. It exits 1 if the script raised or
was stopped, and with the code of a `SystemExit` if one was raised. Before
exiting it waits for the timers and promises the script started, the way Node
waits for its event loop.

## The standard library

CPython is linked statically, and so is its pure-Python standard library
(`Lib/`, which has `os.py`, `codeop.py`, `asyncio/` and the rest): the build
compiles it into the backend as frozen modules. `unibind_python_repl.exe` is the
whole program - copy it anywhere, alone, and it runs
(`example.python_repl.runs-alone-from-an-empty-directory` does exactly that).

A directory on disk still wins when there is one - the backend takes the first
of these that holds `os.py` (`FindStandardLibraryDirectory` in
`src/backends/python/core.cpp`), and the embedded standard library only when
neither does:

1. `UNIBIND_PYTHON_HOME`: that directory, or its `Lib` subdirectory;
2. a `python-stdlib` directory next to the executable.

Pointing `UNIBIND_PYTHON_HOME` at vcpkg's
`build/python-x64/vcpkg_installed/x64-windows-static/tools/python3/Lib` gives
tracebacks into the standard library their source lines back, which frozen code
has none of. `json.__spec__.origin` at the prompt says which one is in use:
`'frozen'`, or a path.

Built with `-DUNIBIND_PYTHON_EMBED_STDLIB=OFF`, the REPL reads `Lib/` from disk
instead, looks last in the path the build found it at, and prints a message and
exits 1 if none of the three holds `os.py`.
[`docs/python.md`](../../docs/python.md) section 10 has the rest: what an
isolate refuses to import, and what embedding the standard library costs and
leaves out.

## A session

This was captured with the input piped in. A piped line is echoed after the
prompt, so the transcript reads the way the session would look on a console.

```
Python 3.14.7 on unibind (python) - :help for help, :quit to leave
>>> c = Counter(3)
>>> c.increment(), list(c)
(4, [0, 1, 2, 3])
>>> v = Vec2(3, 4)
>>> v.add(Vec2(1, 1)).describe(), v.length()
('Vec2(4, 5)', 5)
>>> host.kind_of(Circle(2)), Circle(2).describe()
(['Circle', 'Shape'], 'circle with area 12.57')
>>> host.env.color = 'blue'
>>> list(host.env), 'color' in host.env
(['app', 'color', 'greeting', 'mode'], True)
>>> host.map([1, 2, 3], lambda x: x * x)
[1, 4, 9]
>>> host.set_timeout(lambda: print('tick'), 0.1)
1
>>> await host.fetch_later('hello from C++', 0.2)
<pending>
tick
'hello from C++'
>>> def slow():
...     while True:
...         pass
...
>>> :timeout 1
>>> slow()
TimeoutError: stopped by the watchdog after 1 s
>>> host.heap()
{'used_bytes': 64637, 'total_bytes': 64637, 'limit_bytes': 113433419776, 'malloced_bytes': 64637, 'peak_malloced_bytes': 156676}
>>> :quit
```

The REPL works like Python's own. `codeop` decides whether the input so far is a
complete statement, so a block continues with `... ` until you enter a blank line.
The last expression's `repr` is printed and kept in `_`. An exception prints
Python's traceback (`TryCatch::StackTrace`). Top-level `await` works: the input
then evaluates to a promise (an asyncio Task), and the REPL pumps until the
promise settles.

| command | |
|---|---|
| `:help` | the commands and keys |
| `:bindings` | everything the host registered, one line each |
| `:load FILE` | run a file in this session |
| `:pump [S]` | run pending jobs, and keep pumping for `S` seconds so that timers fire |
| `:heap`, `:gc` | heap statistics; ask for a collection |
| `:timeout [S\|off]` | the watchdog, as `--timeout` sets it |
| `:quit` | leave; so do Ctrl-Z Enter and `raise SystemExit` |

## What is in `host`

| binding | API feature |
|---|---|
| `host.version`, `host.backend` | read-only `Constant`s on an `ObjectTemplate` |
| `host.log(*args)`, `host.now()` | functions on the template, with the host as `CallbackData` |
| `host.throw_type_error(msg)` | a native throwing; Python sees a `TypeError` |
| `host.make_greeter(greeting)` | `Function::New` with a *script value* as its data |
| `host.map(items, fn)` | C++ calling a Python callable (`Function::Call`); exceptions pass back out |
| `host.bytes(n \| text)`, `host.checksum(view)` | `TypedArray::New` from a C++ span; `CopyBytes` back out |
| `host.clone(value)` | `Serialize` then `Deserialize`: structured clone |
| `host.stack()` | `CaptureStackFrames` |
| `host.heap()`, `host.gc()` | `GetHeapStatistics` as a real `dict` (via `NewInstance` on Python's `dict`); `RequestGarbageCollection` |
| `host.set_timeout(fn, s)`, `host.clear_timeout(id)` | `PostDelayedJob`, with the callback held in a `Global<Function>` |
| `host.fetch_later(v, s)`, `host.fail_later(e, s)` | a `Promise` (an `asyncio.Future`) settled from a posted job, so `await` works |
| `host.sleep(s)` | a blocking native that polls `IsExecutionTerminating()` |
| `host.shared_counter(name)`, `host.registry()` | `Class<Counter>::Wrap` over a `shared_ptr` a C++ registry also holds |
| `host.kind_of(value)` | `FunctionTemplate::HasInstance` for `Circle`, `Rect` and `Shape` |
| `host.env` | a **named interceptor** over a `std::map`: get, set, `in`, `del`, `dir()`, `len()`, with a locked key |
| `host.registers` | an **indexed interceptor**: eight integers |
| `host.session` | a plain `Object` with `SetAccessor`: `inputs` (read-only), `prompt` (read-write, and the REPL uses it) |
| `Counter(start, step)` | `Class<T>`: `Construct`, methods, getter/setter accessors that validate, a static method and value, and a `Symbol.iterator` method, so `for x in counter` works |
| `Vec2(x, y)` | `Class<T>` whose methods return new instances, with checked `Unwrap` of the other operand |
| `Shape`, `Circle(r)`, `Rect(w, h)` | `FunctionTemplate`s. `Circle` and `Rect` `Inherit` `Shape`, whose `describe()` calls the subclass's `area()` |

The typed class callbacks have no data slot, so they find the host through
`Isolate::SetEmbedderData`. Everything else is handed the host as its
`CallbackData`. Both lookups are checked: the wrong type comes back as null.

## Stopping a script: Ctrl-C, Ctrl-Break and `--timeout`

Only the main thread touches the isolate. Two other threads can still stop
Python while it runs, through the two `Isolate` calls that may be made from
any thread:

- **Ctrl-C.** Windows runs `SetConsoleCtrlHandler` handlers on a thread of their
  own. If Python is running, the handler calls `TerminateExecution()`. The
  script unwinds, and `except` blocks cannot catch the stop. Back at the prompt,
  the main thread calls `CancelTerminateExecution()`, because a stopped isolate
  stays stopped until it is told otherwise. It then prints
  `KeyboardInterrupt (terminated)`. At the prompt, Ctrl-C clears the line
  instead. While the REPL waits on a promise, Ctrl-C stops the wait, and the
  promise settles at a later pump.
- **`--timeout S`** (or `:timeout S`). A watchdog thread gives each evaluation
  `S` seconds and then calls `TerminateExecution()` the same way.
- **Ctrl-Break** calls `RequestInterrupt`. The callback runs on the main thread,
  between two bytecodes of the running script. It captures the stack and prints
  where the script is, and the script keeps running.

A stop requested while nothing runs is not lost. It would stop the *next*
input. So the check "is Python running?" and the stop itself happen under one
lock, and the main thread takes the same lock when an evaluation ends.

**A stop reaches script, not your C++.** `host.sleep(5)` under `:timeout 1`
returns after one second only because it polls `IsExecutionTerminating()`
itself. A native that blocks without polling runs until it returns.

## Tests

`ctest -C Release -L example` runs:

| test | checks |
|---|---|
| `example.python_repl.demo` | `--demo` passes every assert |
| `example.python_repl.script` | a file with arguments: `sys.argv`, a class, the exit code |
| `example.python_repl.script-that-raises` (+ `-prints-a-traceback`) | exit code 1, and the traceback names the file and the function |
| `example.python_repl.timeout-stops-a-runaway-loop` | `--timeout 1 -c "while True: pass"` is stopped and exits 1 |
| `example.python_repl.session` | [`tests/session.txt`](tests/session.txt) piped in; the transcript's lines must appear in order |
| `example.python_repl.runs-alone-from-an-empty-directory` | the REPL copied alone into an empty directory, `UNIBIND_PYTHON_HOME` unset, runs [`tests/standalone.py`](tests/standalone.py): `asyncio`, `json`, `sqlite3`, `ssl` and `email` from the embedded standard library, with `sys.path` empty |
