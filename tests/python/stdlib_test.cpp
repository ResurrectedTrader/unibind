// The standard library's C extension modules, as the overlay port builds them:
// compiled into the static CPython as built-in modules (a static core cannot
// load a .pyd), and imported here inside an isolate - an own-GIL
// sub-interpreter with check_multi_interp_extensions on. See
// cmake/vcpkg-ports/README.md for the list and for the few that refuse to load
// in such an interpreter.

#include "support.h"

#include <latch>
#include <string>
#include <thread>

using py_test::Eval;
using py_test::EvalError;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Fixture;

namespace {

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
        {
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
    }
    return out;
}

constexpr std::string_view kGatherProgram = R"(
import asyncio
async def work(n):
    await asyncio.sleep(0.01)
    return n * 2
async def main():
    return sum(await asyncio.gather(*(work(i) for i in range(5))))
asyncio.run(main())
)";

}  // namespace

TEST_CASE("stdlib: the extension modules are built in, and nothing looks for a .pyd") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import sys
wanted = ['_asyncio', '_bz2', '_ctypes', '_decimal', '_elementtree', '_hashlib', '_lzma',
          '_multiprocessing', '_overlapped', '_queue', '_remote_debugging', '_socket', '_sqlite3', '_ssl',
          '_uuid', '_wmi', '_zoneinfo', '_zstd', 'pyexpat', 'select', 'unicodedata', 'winsound', 'zlib']
','.join(m for m in wanted if m not in sys.builtin_module_names)
)") == "");
    CHECK(EvalText(f.context, "import _socket\n_socket.__spec__.origin") == "built-in");
    CHECK(EvalText(f.context, "import select\nselect.__loader__.__name__") == "BuiltinImporter");
    // Everything that imports here, imported - and not one of them from a file.
    CHECK(EvalText(f.context, R"(
import asyncio, bz2, ctypes, decimal, hashlib, lzma, multiprocessing, queue, socket, sqlite3, ssl
import unicodedata, uuid, winsound, zlib, zoneinfo, compression.zstd, xml.etree.ElementTree
','.join(sorted(n for n, m in list(sys.modules.items())
                if (getattr(m, '__file__', None) or '').lower().endswith('.pyd')))
)") == "");
}

TEST_CASE("stdlib: the few modules not safe under a GIL of their own refuse an isolate") {
    // An isolate is an own-GIL sub-interpreter with
    // check_multi_interp_extensions on. CPython 3.14 refuses there a module
    // with single-phase init (the core's _tracemalloc), one that says it
    // cannot be shared (the core's _suggestions), and one that does not
    // declare per-interpreter-GIL support (_wmi); every other built-in loads.
    // Each refusal is a clean ImportError - see cmake/vcpkg-ports/README.md.
    Fixture f;
    for (const char* module : {"_wmi", "_tracemalloc", "_suggestions"}) {
        CAPTURE(module);
        CHECK(EvalError(f.context, std::string("import ") + module) ==
              std::string("ImportError: module ") + module + " does not support loading in subinterpreters");
    }
    // Twice: a refused module must not be left half-made for the next attempt.
    CHECK(EvalError(f.context, "import tracemalloc") ==
          "ImportError: module _tracemalloc does not support loading in subinterpreters");
    CHECK(EvalError(f.context, "import tracemalloc") ==
          "ImportError: module _tracemalloc does not support loading in subinterpreters");
    // traceback, which asks for _suggestions, does without it.
    CHECK(EvalTruth(f.context, R"(
import traceback
try:
    None.nothing
except AttributeError as e:
    text = ''.join(traceback.format_exception(e))
'AttributeError' in text
)"));
}

TEST_CASE("stdlib: ctypes, decimal, datetime, zoneinfo and XML run on their C modules in an isolate") {
    // CPython 3.12 refused all of these in an own-GIL sub-interpreter
    // (single-phase init, or not isolated yet); 3.13 and 3.14 made them safe.
    Fixture f;
    CHECK(EvalText(f.context, R"(
import ctypes
buf = ctypes.create_string_buffer(b'unibind')
(ctypes.sizeof(ctypes.c_void_p) == ctypes.sizeof(ctypes.c_size_t), buf.value, ctypes.c_int32(-5).value)
)") == "(True, b'unibind', -5)");
    CHECK(EvalText(f.context, R"(
import decimal, sys
(str(decimal.Decimal('1.1') + decimal.Decimal('2.2')), decimal.Decimal is sys.modules['_decimal'].Decimal)
)") == "('3.3', True)");
    CHECK(EvalText(f.context, R"(
import datetime, sys
(str(datetime.date(2024, 2, 29) + datetime.timedelta(days=1)), '_pydatetime' in sys.modules)
)") == "('2024-03-01', False)");
    CHECK(EvalText(f.context, "import zoneinfo, _zoneinfo\nzoneinfo.ZoneInfo is _zoneinfo.ZoneInfo") == "True");
    CHECK(EvalText(f.context, R"(
import sys, xml.etree.ElementTree as ET, xml.dom.minidom, xml.sax, pyexpat
root = ET.fromstring('<a><b n="1"/><b n="2"/></a>')
dom = xml.dom.minidom.parseString('<x>text</x>')
(sum(int(b.get('n')) for b in root), dom.documentElement.firstChild.data, ET.XMLParser is sys.modules['_elementtree'].XMLParser)
)") == "(3, 'text', True)");
    // The same in two isolates at once, each with its own module state.
    std::string results[2];
    std::latch start(2);
    std::thread threads[2];
    for (int i = 0; i < 2; ++i) {
        threads[i] = std::thread([i, &results, &start] {
            start.arrive_and_wait();
            results[i] = EvalInFreshIsolate(R"(
import ctypes, decimal, xml.etree.ElementTree as ET
decimal.getcontext().prec = 5
str(decimal.Decimal(1) / decimal.Decimal(7)) + ' ' + ET.fromstring('<a>ok</a>').text + ' ' + str(ctypes.c_uint8(300).value)
)");
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(results[0] == "0.14286 ok 44");
    CHECK(results[1] == "0.14286 ok 44");
}

TEST_CASE("stdlib: ssl and hashlib - OpenSSL, built in") {
    Fixture f;
    CHECK(EvalTruth(f.context, "import ssl\nssl.OPENSSL_VERSION.startswith('OpenSSL 3')"));
    // create_default_context loads the Windows certificate stores (crypt32).
    CHECK(EvalTruth(f.context, R"(
ctx = ssl.create_default_context()
ctx.verify_mode == ssl.CERT_REQUIRED and ctx.check_hostname and ctx.cert_store_stats()['x509_ca'] > 0
)"));
    CHECK(EvalText(f.context, R"(
import hashlib
hashlib.sha256(b'abc').hexdigest()
)") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(EvalText(f.context, "hashlib.sha256.__name__") == "openssl_sha256");
    CHECK(EvalText(f.context, "hashlib.pbkdf2_hmac('sha1', b'password', b'salt', 1, 20).hex()") ==
          "0c60c80f961f0e71f3a9b524af6012062fe037a6");
}

TEST_CASE("stdlib: sqlite3 - an in-memory database") {
    Fixture f;
    CHECK(EvalInt(f.context, R"(
import sqlite3
db = sqlite3.connect(':memory:')
db.execute('create table t (n integer)')
db.executemany('insert into t values (?)', [(i,) for i in range(1, 11)])
total = db.execute('select sum(n) from t').fetchone()[0]
db.close()
total
)") == 55);
    CHECK(EvalTruth(f.context, "sqlite3.sqlite_version_info >= (3, 40)"));
}

TEST_CASE("stdlib: compression - zlib, bz2, lzma and zstd round trips") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import zlib, bz2, lzma
from compression import zstd
data = b'unibind ' * 1000
ok = []
for name, mod in (('zlib', zlib), ('bz2', bz2), ('lzma', lzma), ('zstd', zstd)):
    packed = mod.compress(data)
    if len(packed) < len(data) and mod.decompress(packed) == data:
        ok.append(name)
','.join(ok)
)") == "zlib,bz2,lzma,zstd");
}

TEST_CASE("stdlib: unicodedata, queue, uuid, zoneinfo, multiprocessing, winsound") {
    Fixture f;
    CHECK(EvalText(f.context, "import unicodedata\nunicodedata.name('\\u00e9')") == "LATIN SMALL LETTER E WITH ACUTE");
    CHECK(EvalText(f.context, "unicodedata.normalize('NFD', '\\u00e9') == 'e\\u0301'") == "True");

    CHECK(EvalInt(f.context, R"(
import queue, _queue
q = queue.SimpleQueue()
assert queue.SimpleQueue is _queue.SimpleQueue
for i in range(3):
    q.put(i)
q.get() + q.get() + q.get()
)") == 3);

    // uuid1 is _uuid's UuidCreateSequential (rpcrt4).
    CHECK(EvalText(f.context, "import uuid\n(uuid.uuid4().version, uuid.uuid1().version, uuid._UuidCreate is not None)") ==
          "(4, 1, True)");

    // No tz database on Windows without the tzdata package, so a zone built
    // from a TZif v1 blob: no transitions, one type, UTC. In an isolate this
    // is _zoneinfo's C ZoneInfo, as anywhere else.
    CHECK(EvalText(f.context, R"(
import io, struct, zoneinfo
from datetime import datetime, timedelta
tzif = b'TZif' + b'\0' * 16 + struct.pack('>6l', 0, 0, 0, 0, 1, 4) + struct.pack('>lBB', 0, 0, 0) + b'UTC\0'
zone = zoneinfo.ZoneInfo.from_file(io.BytesIO(tzif), key='Test/UTC')
moment = datetime(2024, 6, 1, 12, tzinfo=zone)
import _zoneinfo
(zoneinfo.ZoneInfo is _zoneinfo.ZoneInfo, moment.utcoffset() == timedelta(0), moment.tzname())
)") == "(True, True, 'UTC')");

    // Nothing is spawned: a lock is a Windows semaphore made by _multiprocessing.
    CHECK(EvalTruth(f.context, R"(
import _multiprocessing, multiprocessing
lock = multiprocessing.Lock()
lock.acquire(timeout=1) and (lock.release() is None)
)"));

    CHECK(EvalTruth(f.context, "import winsound\nwinsound.PlaySound(None, winsound.SND_PURGE) is None"));
}

TEST_CASE("stdlib: socket - a socketpair carries bytes, and select sees them") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import socket, select
a, b = socket.socketpair()
a.sendall(b'ping')
readable, _, _ = select.select([b], [], [], 5)
got = b.recv(16) if readable == [b] else b''
a.close(); b.close()
got.decode()
)") == "ping");
    CHECK(EvalTruth(f.context, "socket.gethostname() != ''"));
    CHECK(EvalText(f.context, "socket.inet_ntoa(socket.inet_aton('127.0.0.1'))") == "127.0.0.1");
}

TEST_CASE("stdlib: asyncio - run, sleep and gather") {
    Fixture f;
    CHECK(EvalInt(f.context, kGatherProgram) == 20);
    CHECK(EvalText(f.context, "import _asyncio, asyncio\nasyncio.Future is _asyncio.Future") == "True");
    CHECK(EvalText(f.context, "loop = asyncio.new_event_loop()\nkind = type(loop).__name__\nloop.close()\nkind") ==
          "ProactorEventLoop");
}

TEST_CASE("stdlib: asyncio - a loopback TCP echo through streams") {
    Fixture f;
    CHECK(EvalText(f.context, R"(
import asyncio
async def handle(reader, writer):
    line = await reader.readline()
    writer.write(line.upper())
    await writer.drain()
    writer.close()
    await writer.wait_closed()
async def main():
    server = await asyncio.start_server(handle, '127.0.0.1', 0)
    port = server.sockets[0].getsockname()[1]
    async with server:
        reader, writer = await asyncio.open_connection('127.0.0.1', port)
        writer.write(b'hello over tcp\n')
        await writer.drain()
        reply = await reader.readline()
        writer.close()
        await writer.wait_closed()
    return reply.decode().strip()
asyncio.run(asyncio.wait_for(main(), 10))
)") == "HELLO OVER TCP");
}

TEST_CASE("stdlib: asyncio - two isolates run event loops on two threads at once") {
    std::latch start(2);
    std::string first;
    std::string second;
    auto body = [&start](std::string& out) {
        start.arrive_and_wait();
        out = EvalInFreshIsolate(kGatherProgram);
    };
    std::thread a(body, std::ref(first));
    std::thread b(body, std::ref(second));
    a.join();
    b.join();
    CHECK(first == "20");
    CHECK(second == "20");
}
