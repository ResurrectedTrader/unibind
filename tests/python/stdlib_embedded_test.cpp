// The pure-Python standard library, compiled into the backend
// (UNIBIND_PYTHON_EMBED_STDLIB, on by default): every module a frozen one,
// served by CPython's FrozenImporter from the program's own image, so that a
// program needs nothing on disk beside it. See docs/python.md, "Where the
// standard library comes from".
//
// A directory still wins when one is given - UNIBIND_PYTHON_HOME, or a
// `python-stdlib` directory beside the program - and so every case here asks
// which of the two this run should see, and checks that one. CTest runs them
// three ways (tests/python/CMakeLists.txt): in place; from a copy of this
// program in an empty directory, with nothing on disk to find; and with
// UNIBIND_PYTHON_HOME pointing at the build's `Lib`, which must win.

#include "support.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Fixture;

namespace {

namespace fs = std::filesystem;

fs::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return fs::path(path).parent_path();
}

bool HoldsStandardLibrary(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "os.py", ec) || fs::exists(dir / "Lib" / "os.py", ec);
}

/// Whether this run's standard library comes from a directory on disk rather
/// than from the embedded table - by the backend's own precedence.
bool StandardLibraryFromDisk() {
#if !UNIBIND_TEST_STDLIB_EMBEDDED
    return true;
#else
    wchar_t* env = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&env, &length, L"UNIBIND_PYTHON_HOME") == 0 && env != nullptr) {
        const fs::path home(env);
        std::free(env);
        if (HoldsStandardLibrary(home)) {
            return true;
        }
    }
    std::error_code ec;
    return fs::exists(ExecutableDirectory() / "python-stdlib" / "os.py", ec);
#endif
}

// Evaluate `source` in a fresh isolate on the calling thread and hand back the
// result's text, or "error: <message>". No doctest assertions: this runs on
// threads of its own.
std::string EvalInFreshIsolate(std::string_view source) {
    auto isolate = ub::Isolate::New();
    if (!isolate) {
        return "error: no isolate";
    }
    std::string out;
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return "error: no context";
        }
        const ub::ContextScope entered(*context);
        ub::TryCatch tc(*isolate);
        auto result = ub::Evaluate(*context, source, {.resourceName = "thread.py"});
        if (!result) {
            out = "error: " + (tc.HasCaught() ? tc.Message(*context).value_or("?") : std::string("?"));
        } else if (auto text = result->ToString(*context)) {
            out = text->Utf8Value();
        } else {
            out = "error: no text";
        }
    }
    return out;
}

// One module of each kind a program leans on - packages and plain modules,
// pure Python over a built-in C half (json, re, decimal, ssl, sqlite3) and pure
// Python all through - each made to do something, not only imported.
// `ssl.create_default_context()` reads the Windows certificate stores, which
// the overlay port's patch 0104 made safe from several isolates at once.
constexpr std::string_view kRepresentative = R"(
import asyncio, collections, dataclasses, decimal, email, email.message, email.parser, json, pathlib
import re, sqlite3, ssl, typing

async def twice(n):
    await asyncio.sleep(0)
    return n * 2
async def gathered():
    return sum(await asyncio.gather(*(twice(i) for i in range(5))))

@dataclasses.dataclass
class Point:
    x: int
    y: int

T = typing.TypeVar('T')
def first(items: typing.Sequence[T]) -> T:
    return items[0]

db = sqlite3.connect(':memory:')
db.execute('create table t (v integer)')
db.executemany('insert into t values (?)', [(1,), (2,), (3,)])
message = email.parser.Parser().parsestr('Subject: hello\n\nbody\n')

results = [
    json.dumps({'a': [1, 2]}, sort_keys=True),
    re.sub(r'(\d+)', r'<\1>', 'a1b22'),
    str(asyncio.run(gathered())),
    str(ssl.create_default_context().verify_mode == ssl.CERT_REQUIRED),
    str(db.execute('select sum(v) from t').fetchone()[0]),
    str(decimal.Decimal('0.1') + decimal.Decimal('0.2')),
    str(collections.Counter('abca').most_common(1)[0]),
    str(Point(1, 2)),
    first(['typed']),
    pathlib.PurePosixPath('a/b/c.txt').suffix,
    message['Subject'],
]
'|'.join(results)
)";

constexpr std::string_view kRepresentativeResult =
    R"({"a": [1, 2]}|a<1>b<22>|20|True|6|0.3|('a', 2)|Point(x=1, y=2)|typed|.txt|hello)";

}  // namespace

TEST_CASE("stdlib embedded: the standard library comes from the frozen table, or from the directory given") {
    Fixture f;
    if (StandardLibraryFromDisk()) {
        // A directory: sys.path is it, and a module is a file in it.
        CHECK(EvalTruth(f.context, R"(
import sys, importlib.util, os
origin = importlib.util.find_spec('json').origin
len(sys.path) == 1 and os.path.isfile(origin) and os.path.samefile(os.path.dirname(os.path.dirname(origin)), sys.path[0])
)"));
        CHECK(EvalText(f.context, "import json\ntype(json.__loader__).__name__") == "SourceFileLoader");
        return;
    }

    // Embedded: every module the FrozenImporter's, and nothing looked for on
    // disk - sys.path is empty, and no module has a file.
    CHECK(EvalText(f.context, "import importlib.util\nimportlib.util.find_spec('json').origin") == "frozen");
    CHECK(EvalText(f.context, "import json.decoder\njson.decoder.__loader__.__name__") == "FrozenImporter");
    CHECK(EvalText(f.context, "import sys\nrepr(sys.path)") == "[]");
    CHECK(EvalText(f.context, R"(
import sys, asyncio, email.message, sqlite3
','.join(sorted(name for name, module in list(sys.modules.items()) if getattr(module, '__file__', None)))
)") == "");
    // A package is one, and its submodules are found by name, not by its path.
    CHECK(EvalText(f.context, R"(
import importlib.util, asyncio
spec = importlib.util.find_spec('asyncio.events')
f'{asyncio.__path__!r} {spec.origin} {importlib.util.find_spec("asyncio").submodule_search_locations!r}'
)") == "[] frozen []");
    // The whole standard library, less what the build left out.
    CHECK(EvalText(f.context, R"(
import importlib.util
present = ['os', 'json', 'asyncio.base_events', 'encodings.utf_8', 'email.mime.text', 'xml.etree.ElementTree']
left_out = ['test', 'idlelib', 'tkinter', 'turtle', 'lib2to3', 'ensurepip', 'venv', 'pydoc_data']
found = [n for n in present if importlib.util.find_spec(n).origin == 'frozen']
found += [n for n in left_out if importlib.util.find_spec(n) is not None]
','.join(found)
)") == "os,json,asyncio.base_events,encodings.utf_8,email.mime.text,xml.etree.ElementTree");
    // What the build left out is not there, and is not looked for anywhere else.
    CHECK(EvalText(f.context, R"(
try:
    import tkinter
    r = 'imported'
except ModuleNotFoundError as e:
    r = e.name
r
)") == "tkinter");
}

TEST_CASE("stdlib embedded: representative modules import and work in an isolate") {
    Fixture f;
    CHECK(EvalText(f.context, kRepresentative) == kRepresentativeResult);
    if (!StandardLibraryFromDisk()) {
        CHECK(EvalText(f.context, R"(
import sys
names = ['asyncio', 'collections', 'dataclasses', 'decimal', 'email', 'json', 'pathlib', 're', 'sqlite3', 'ssl', 'typing']
','.join(n for n in names if sys.modules[n].__spec__.origin != 'frozen')
)") == "");
    }
}

TEST_CASE("stdlib embedded: isolates on many threads unmarshal the same table at once") {
    // Each isolate is an own-GIL interpreter making its own code objects from
    // the one read-only copy of the bytes, all at the same time.
    constexpr int kThreads = 6;
    std::vector<std::string> results(kThreads);
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&results, i] { results[i] = EvalInFreshIsolate(kRepresentative); });
        }
    }
    for (const std::string& result : results) {
        CHECK(result == kRepresentativeResult);
    }
}

TEST_CASE("stdlib embedded: a traceback into the standard library names the module and the line") {
    Fixture f;
    const std::string trace = EvalText(f.context, R"(
import json, traceback
try:
    json.loads('{')
except ValueError:
    text = traceback.format_exc()
text
)");
    CHECK(trace.find("JSONDecodeError") != std::string::npos);
    if (StandardLibraryFromDisk()) {
        // From a directory, the file and the line's source.
        CHECK(trace.find("decoder.py\", line ") != std::string::npos);
        CHECK(trace.find("self.scan_once(") != std::string::npos);
    } else {
        // Frozen: CPython's own name for the module's code, and the line number,
        // without the line's text - there is no source to show it from.
        CHECK(trace.find("File \"<frozen json.decoder>\", line ") != std::string::npos);
        CHECK(trace.find("self.scan_once(") == std::string::npos);
    }
}

TEST_CASE("stdlib embedded: isolate start-up, measured") {
    // What an isolate costs to make - its interpreter, its asyncio, a realm and
    // a first script - with the standard library wherever this run takes it
    // from. Measured, not asserted: docs/python.md has the figures, embedded and
    // from disk.
    using Clock = std::chrono::steady_clock;
    constexpr int kIsolates = 10;
    std::vector<double> ms;
    for (int i = 0; i < kIsolates; ++i) {
        const auto start = Clock::now();
        const std::string result = EvalInFreshIsolate("import json, re, dataclasses, typing\n'ok'");
        const auto end = Clock::now();
        REQUIRE(result == "ok");
        ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(ms);
    MESSAGE("isolate start-up with json, re, dataclasses and typing, standard library "
            << std::string(StandardLibraryFromDisk() ? "from disk" : "embedded") << ": median " << ms[kIsolates / 2]
            << " ms, fastest " << ms.front() << " ms");
}

