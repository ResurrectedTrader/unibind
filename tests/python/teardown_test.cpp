// Isolates, threads and teardown: what an isolate owns goes with it - in the
// right order, on the right thread, exactly once - whatever state it is left
// in, including threads its scripts started; isolates on many threads share
// nothing; and a stop lands cleanly on code that holds things.

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lifetimes_support.h"

using namespace std::chrono_literals;
using py_lifetimes::Fixture;
using py_lifetimes::Native;
using py_lifetimes::RefCount;
using py_lifetimes::Run;
using py_test::Eval;
using py_test::EvalError;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Expose;
using py_test::Str;

namespace {

/// Everything written to the process's standard error while it lives - which
/// is where CPython prints an unraisable exception, a thread's uncaught one,
/// or a fatal error's last words.
class CaptureStderr {
   public:
    // The descriptor is duplicated before anything is flushed; that changes
    // nothing, since stderr is only redirected below, after the flush.
    CaptureStderr() : saved_(_dup(2)) {
        std::fflush(stderr);
        path_ = (std::filesystem::temp_directory_path() /
                 ("unibind-stderr-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".txt"))
                    .string();
        int file = -1;
        REQUIRE(_sopen_s(&file, path_.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _SH_DENYNO,
                         _S_IREAD | _S_IWRITE) == 0);
        _dup2(file, 2);
        _close(file);
    }
    ~CaptureStderr() { (void)Stop(); }
    CaptureStderr(const CaptureStderr&) = delete;
    CaptureStderr& operator=(const CaptureStderr&) = delete;
    CaptureStderr(CaptureStderr&&) = delete;
    CaptureStderr& operator=(CaptureStderr&&) = delete;

    std::string Stop() {
        if (saved_ >= 0) {
            std::fflush(stderr);
            _dup2(saved_, 2);
            _close(saved_);
            saved_ = -1;
            std::ifstream in(path_, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            captured_ = text.str();
            in.close();
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
        return captured_;
    }

   private:
    std::string path_;
    int saved_ = -1;
    std::string captured_;
};

/// A native that counts itself.
struct Counted {
    static inline std::atomic<int> alive{0};
    static inline std::atomic<int> constructed{0};
    static inline std::atomic<int> destroyed{0};
    /// If set, the destructor stops its isolate - a native destructor may call
    /// `TerminateExecution` like any other code on the thread.
    static inline std::atomic<bool> stopOnDestroy{false};
    static inline ub::Isolate* stopTarget = nullptr;

    Counted() {
        ++alive;
        ++constructed;
    }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    Counted(Counted&&) = delete;
    Counted& operator=(Counted&&) = delete;
    ~Counted() {
        --alive;
        ++destroyed;
        if (stopOnDestroy && stopTarget != nullptr) {
            stopTarget->TerminateExecution();
        }
    }
    static void Reset() {
        alive = 0;
        constructed = 0;
        destroyed = 0;
        stopOnDestroy = false;
        stopTarget = nullptr;
    }
};

std::unique_ptr<Counted> MakeCounted(const ub::CallbackInfo& /*info*/) {
    return std::make_unique<Counted>();
}

/// A constructor that stops its isolate and still hands back a native.
std::unique_ptr<Counted> MakeCountedAndStop(const ub::CallbackInfo& info) {
    info.GetIsolate().TerminateExecution();
    return std::make_unique<Counted>();
}

void CountedPing(Counted& /*self*/, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(1);
}

ub::Class<Counted> DeclareCounted(ub::Isolate& isolate) {
    const auto cls = ub::Class<Counted>::New(isolate, "Counted");
    cls.Construct<&MakeCounted>();
    cls.Method<&CountedPing>("ping");
    return cls;
}

/// Evaluate expecting a stop, and cancel it.
void RunStopped(const Fixture& f, std::string_view source) {
    {
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Evaluate(f.context, source).has_value());
        CHECK(handler.HasTerminated());
    }
    f.iso().CancelTerminateExecution();
}

}  // namespace

// ===========================================================================
// Isolates in sequence and in parallel
// ===========================================================================

TEST_CASE("teardown: a second isolate on a thread is refused while the first lives, and allowed after") {
    auto first = ub::Isolate::New();
    REQUIRE(first != nullptr);
    CHECK(ub::Isolate::New() == nullptr);
    CHECK(ub::Isolate::New() == nullptr);
    first.reset();
    auto second = ub::Isolate::New();
    REQUIRE(second != nullptr);
    const ub::HandleScope scope(*second);
    auto context = ub::Context::New(*second);
    REQUIRE(context.has_value());
    CHECK(EvalInt(*context, "6 * 7") == 42);
}

namespace {

/// One isolate's worth of everything: realms, a class, a template, scripts,
/// roots, typed arrays with exported buffers, a promise, a clone, a cycle,
/// and whatever is left for teardown. Answers a checksum.
std::int64_t UseEverything(int seed) {
    auto isolate = ub::Isolate::New();
    REQUIRE(isolate != nullptr);
    std::int64_t sum = 0;
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);
        const auto cls = DeclareCounted(*isolate);
        Expose(*context, "Counted", *cls.GetConstructor(*context));
        const auto tpl = ub::FunctionTemplate::New(*isolate);
        tpl.SetClassName("Shape");
        tpl.PrototypeTemplate().Set("sides", ub::Constant(std::int32_t{4}));
        Expose(*context, "Shape", *tpl.GetFunction(*context));
        ub::Global<ub::Value> root(*isolate, Eval(*context, "import gc, asyncio, unibind\nobject()"));
        sum += EvalInt(*context, "keep = [Counted() for _ in range(10)]\nsum(c.ping() for c in keep)");
        sum += EvalInt(*context, "Shape().sides");
        sum += EvalInt(*context, "t = unibind.TypedArray('int32', range(" + std::to_string((seed % 7) + 1) +
                                     "))\nm = memoryview(t)\nsum(m)");
        sum += EvalInt(*context, "a = []\na.append(a)\nx = Counted()\nx.a = a\na.append(x)\ndel a, x\ngc.collect()\n0");
        const auto promise = ub::Promise::New(*context);
        REQUIRE(promise.has_value());
        Expose(*context, "p", *promise);
        Run(*context, "async def later():\n    return await p\ntask = asyncio.ensure_future(later())");
        REQUIRE(ub::Resolve(*context, *promise, ub::Integer::New(*isolate, seed)).value_or(false));
        isolate->PumpJobs();
        sum += EvalInt(*context, "task.result()");
        for (int i = 0; i < 3; ++i) {
            auto realm = ub::Context::New(*isolate);
            REQUIRE(realm.has_value());
            sum += EvalInt(*realm, "len([n for n in range(10)])");
        }
        auto script = ub::Script::Compile(*context, "late = Counted()\n5");
        REQUIRE(script.has_value());
        sum += script->Run(*context)->To<ub::Integer>()->Int32Value();
        // Left for teardown: live natives, a memoryview over a typed array
        // and one over a bytearray, a pending task and a timer.
        Run(*context,
            "import builtins\nbuiltins.left = [Counted(), memoryview(bytearray(8)), m, Counted()]\n"
            "pending = asyncio.ensure_future(asyncio.sleep(3600))\n"
            "asyncio.get_event_loop().call_later(3600, print, 'never')");
        root.Reset();
    }
    return sum;
}

#if defined(NDEBUG)
constexpr int SEQUENTIAL = 100;
#else
constexpr int SEQUENTIAL = 15;  // a debug CPython starts an interpreter many times slower
#endif
constexpr int ROUNDS_PER_THREAD = 4;

/// Threads running isolates at once - as many against a debug CPython, whose
/// debug heap once caught CPython 3.12 switching its process-wide allocator
/// under running sub-interpreters (docs/python.md, section 11).
constexpr int CONCURRENT_THREADS = 8;

/// What `UseEverything(seed)` answers when every part of it worked.
std::int64_t ExpectedSum(int seed) {
    const std::int64_t n = (seed % 7) + 1;  // the typed array holds 0 .. n - 1
    return 10 + 4 + (n * (n - 1) / 2) + 0 + seed + 30 + 5;
}

}  // namespace

TEST_CASE("teardown: isolates in sequence on one thread each give back all they took") {
    Counted::Reset();
    (void)UseEverything(0);  // first-use statics
    const long long before = ub_test::OutstandingAllocations();
    std::string printed;
    {
        CaptureStderr capture;
        for (int i = 0; i < SEQUENTIAL; ++i) {
            CHECK(UseEverything(i) == ExpectedSum(i));
        }
        printed = capture.Stop();
    }
    CHECK(printed.empty());
    CHECK(Counted::alive == 0);
    CHECK(Counted::destroyed == Counted::constructed);
    const long long kept = ub_test::OutstandingAllocations() - before;
    CAPTURE(kept);
    // Nothing per isolate: the heap account an interpreter charged gets every
    // block back when it ends, and the next isolate reuses it. Anything past a
    // handful of first-use allocations is the backend's own leak.
    CHECK(kept <= 16);
}

TEST_CASE("teardown: isolates on many threads at once, each churning, share nothing") {
    Counted::Reset();
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(CONCURRENT_THREADS);
    for (int t = 0; t < CONCURRENT_THREADS; ++t) {
        threads.emplace_back([t, &failures] {
            for (int round = 0; round < ROUNDS_PER_THREAD; ++round) {
                const int seed = (t * 10) + round;
                if (UseEverything(seed) != ExpectedSum(seed)) {
                    ++failures;
                }
            }
            // And one isolate whose types a script poisons: per-interpreter
            // types mean no other thread sees it.
            auto isolate = ub::Isolate::New();
            const ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            Run(*context, "import unibind\nunibind.Object.poisoned = " + std::to_string(t));
            for (int i = 0; i < 50; ++i) {
                if (EvalInt(*context, "unibind.Object.poisoned + unibind.Object().poisoned") != 2 * t) {
                    ++failures;
                }
                std::this_thread::yield();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(failures == 0);
    CHECK(Counted::alive == 0);
    CHECK(Counted::destroyed == Counted::constructed);
}

TEST_CASE("teardown: one isolate churning realms, scripts, templates, roots and natives ten thousand times") {
    Counted::Reset();
    Fixture f;
    const auto cls = DeclareCounted(f.iso());
    const auto tpl = ub::ObjectTemplate::New(f.iso());
    tpl.Set("answer", ub::Constant(std::int32_t{42}));
    const auto churn = [&](int i) {
        const ub::HandleScope scope(f.iso());
        auto realm = ub::Context::New(f.iso());
        REQUIRE(realm.has_value());
        const ub::ContextScope entered(*realm);
        Expose(*realm, "Counted", *cls.GetConstructor(*realm));
        const auto made = tpl.NewInstance(*realm);
        REQUIRE(made.has_value());
        Expose(*realm, "made", *made);
        auto script = ub::Script::Compile(*realm, "c = Counted()\nc.self = c\nmade.answer + c.ping()");
        REQUIRE(script.has_value());
        CHECK(script->Run(*realm)->To<ub::Integer>()->Int32Value() == 43);
        ub::Global<ub::Value> root(f.iso(), *made);
        ub::Global<ub::Value> copy = root.Duplicate();
        CHECK(copy.StrictEquals(root));
        if (i % 500 == 0) {
            f.iso().RequestGarbageCollection();
        }
    };
    for (int i = 0; i < 200; ++i) {
        churn(i);
    }
    f.iso().RequestGarbageCollection();
    const long long before = ub_test::OutstandingAllocations();
    const auto heapBefore = f.iso().GetHeapStatistics().usedBytes;
    for (int i = 0; i < 10000; ++i) {
        churn(i);
    }
    f.iso().RequestGarbageCollection();
    const long long kept = ub_test::OutstandingAllocations() - before;
    const auto heapAfter = f.iso().GetHeapStatistics().usedBytes;
    CAPTURE(kept);
    CAPTURE(heapBefore);
    CAPTURE(heapAfter);
    CHECK(kept <= 16);
    CHECK(heapAfter < heapBefore + (std::uint64_t{4} * 1024 * 1024));
    CHECK(Counted::alive == 0);
}

// ===========================================================================
// Threads a script started
// ===========================================================================

TEST_CASE("teardown: a threading.Thread still running Python when the isolate goes is stopped, quietly") {
    std::string printed;
    std::chrono::steady_clock::duration teardown{};
    {
        CaptureStderr capture;
        std::chrono::steady_clock::time_point stopping;
        {
            Fixture f;
            Run(f.context, R"(
import threading
state = {'n': 0}
def spin():
    while True:
        try:
            while True:
                state['n'] += 1
        except BaseException:
            pass                     # swallowing the stop does not help
t = threading.Thread(target=spin)
t.start()
while state['n'] == 0:
    pass
)");
            stopping = std::chrono::steady_clock::now();
        }
        teardown = std::chrono::steady_clock::now() - stopping;
        printed = capture.Stop();
    }
    // Timed from the script's return, so that only the stop and the teardown
    // are in it: bringing the isolate up is not, and a debug CPython takes
    // well over a second for that alone. The stop and the teardown take tens
    // of milliseconds, Debug or not. Under `~Isolate`'s two-second grace
    // (runtime.cpp, `THREAD_GRACE`) also means the interpreter was ended,
    // not left behind: `~Isolate` abandons it only once the whole grace has
    // passed with the thread still in it.
    CHECK(teardown < 1500ms);
    CHECK(printed.empty());
}

TEST_CASE("teardown: a raw _thread thread still running when the isolate goes is stopped, not fatal") {
    std::string printed;
    {
        CaptureStderr capture;
        {
            Fixture f;
            Run(f.context, R"(
import _thread, time
started = []
def spin():
    started.append(1)
    while True:
        time.sleep(0.001)
for _ in range(3):
    _thread.start_new_thread(spin, ())
while len(started) < 3:
    time.sleep(0.001)
)");
        }
        printed = capture.Stop();
    }
    CHECK(printed.empty());
    // And the thread can have an isolate again.
    Fixture after;
    CHECK(EvalInt(after.context, "1 + 1") == 2);
}

TEST_CASE(
    "teardown: a thread in a short blocking call is waited for, and one that sleeps past the grace is left behind") {
    {
        // Sleeping less than the grace: the isolate waits for it to wake,
        // meet the stop, and end.
        Fixture f;
        Run(f.context, "import threading, time\nt = threading.Thread(target=time.sleep, args=(0.3,))\nt.start()");
    }
    std::string printed;
    const auto start = std::chrono::steady_clock::now();
    {
        CaptureStderr capture;
        {
            // Sleeping past it: the interpreter is left behind with the
            // thread in it rather than ended under it, which would be fatal.
            Counted::Reset();
            Fixture f;
            const auto cls = DeclareCounted(f.iso());
            Expose(f.context, "Counted", *cls.GetConstructor(f.context));
            Run(f.context, R"(
import threading, time, builtins
builtins.kept = [Counted(), Counted()]
def sleeper():
    time.sleep(4)
    builtins.woke = True             # the stop is still there when it wakes
    while True:
        pass
threading.Thread(target=sleeper).start()
)");
        }
        // The natives went all the same, on this thread.
        CHECK(Counted::alive == 0);
        CHECK(Counted::destroyed == 2);
        // This thread is free for another isolate straight away.
        {
            Fixture next;
            CHECK(EvalInt(next.context, "2 + 2") == 4);
        }
        // Let the abandoned thread wake, meet the stop and end.
        std::this_thread::sleep_for(3s);
        printed = capture.Stop();
    }
    CHECK(std::chrono::steady_clock::now() - start >= 2s);
    CHECK(printed.empty());
}

TEST_CASE(
    "teardown: TerminateExecution stops a script's threads too, and an interrupt waits for the isolate's thread") {
    Fixture f;
    static std::atomic<std::thread::id> interruptThread;
    interruptThread = std::thread::id();
    Run(f.context, R"(
import threading
state = {'n': 0, 'ended': False}
def spin():
    try:
        while True:
            state['n'] += 1
    finally:
        state['ended'] = True
t = threading.Thread(target=spin)
t.start()
)");
    std::thread stopper([&f] {
        std::this_thread::sleep_for(100ms);
        // Asked for while the script's thread is the one running Python.
        (void)f.iso().RequestInterrupt(
            [](ub::Isolate& /*isolate*/, ub::CallbackData /*data*/) { interruptThread = std::this_thread::get_id(); },
            {});
        std::this_thread::sleep_for(100ms);
        f.iso().TerminateExecution();
    });
    // The isolate's thread waits on the other one; the stop ends it.
    RunStopped(f, "while state['n'] == 0:\n    pass\nt.join()\nafter = True");
    stopper.join();
    CHECK(EvalTruth(f.context, "'after' not in globals()"));
    // The interrupt was asked for while only the script's thread ran Python; it
    // waited for the isolate's thread - out the stop, too - and ran there.
    CHECK(interruptThread.load() == std::this_thread::get_id());
    // The thread is gone: its count stands still while this thread sleeps,
    // and `is_alive` says so. (Under 3.12 it could not: the stop landed inside
    // `t.join()` between a Python lock's acquire and its release, and the
    // bookkeeping after was never done. 3.13 joins through a C thread handle.)
    CHECK(EvalTruth(f.context, "import time\nbefore = state['n']\ntime.sleep(0.05)\nstate['n'] == before"));
    CHECK(EvalTruth(f.context, "not t.is_alive()"));
    // Stopped straight away, so its `finally` never ran.
    CHECK_FALSE(EvalTruth(f.context, "state['ended']"));
    // New threads run normally after the cancel.
    CHECK(EvalInt(f.context,
                  "out = []\nt2 = threading.Thread(target=lambda: out.append(5))\nt2.start()\nt2.join()\nout[0]") == 5);
}

// ===========================================================================
// Stops that land on code holding things
// ===========================================================================

namespace {
/// Makes handles, stops its own isolate, makes more, and returns one.
void StopsWhileHolding(const ub::CallbackInfo& info) {
    std::vector<ub::Local<ub::Value>> held;
    for (int i = 0; i < 30; ++i) {
        if (auto made = ub::Evaluate(info.GetContext(), "Watched()")) {
            held.push_back(*made);
        }
    }
    info.GetIsolate().TerminateExecution();
    for (int i = 0; i < 30; ++i) {
        if (auto made = ub::Object::New(info.GetContext())) {
            held.emplace_back(*made);
        }
    }
    info.GetReturnValue().Set(held.front());
}
}  // namespace

TEST_CASE("teardown: a stop from inside a native that holds handles drops every one of them") {
    Fixture f;
    // Plain weak references, not a WeakSet: a WeakSet forgets an entry from a
    // Python callback, and while a stop is in force no Python runs - the
    // objects go, and the set would go on counting them.
    Run(f.context, "made = []\nclass Watched:\n    def __init__(self):\n        made.append(weakref.ref(self))");
    Expose(f.context, "stopsWhileHolding", Native(f.context, &StopsWhileHolding));
    RunStopped(f, "kept = stopsWhileHolding()\nafter = True");
    Run(f.context, "gc.collect()");
    CHECK(EvalInt(f.context, "len(made)") == 30);
    CHECK(EvalInt(f.context, "sum(r() is not None for r in made)") == 0);
    CHECK(EvalTruth(f.context, "'kept' not in globals() and 'after' not in globals()"));
}

TEST_CASE("teardown: a stop from inside a class constructor, and from a native's destructor during collection") {
    Counted::Reset();
    {
        Fixture f;
        const auto stops = ub::Class<Counted>::New(f.iso(), "Stops");
        stops.Construct<&MakeCountedAndStop>();
        Expose(f.context, "Stops", *stops.GetConstructor(f.context));
        const auto cls = DeclareCounted(f.iso());
        Expose(f.context, "Counted", *cls.GetConstructor(f.context));

        RunStopped(f, "s = Stops()\nafter = True");
        Run(f.context, "gc.collect()");
        // Made, never reached script, and gone - once.
        CHECK(Counted::constructed == 1);
        CHECK(Counted::destroyed == 1);
        CHECK(EvalTruth(f.context, "'s' not in globals()"));

        Counted::stopTarget = &f.iso();
        Counted::stopOnDestroy = true;
        RunStopped(f, "c = Counted()\nc.me = c\ndel c\ngc.collect()\nafter = True");
        Counted::stopOnDestroy = false;
        CHECK(Counted::destroyed == 2);
        CHECK(EvalInt(f.context, "len([Counted() for _ in range(3)])") == 3);
    }
    CHECK(Counted::alive == 0);
    CHECK(Counted::destroyed == Counted::constructed);
}

namespace {
void StopsIsolate(const ub::CallbackInfo& info) {
    info.GetIsolate().TerminateExecution();
}
}  // namespace

TEST_CASE("teardown: a stop inside an asyncio task, then the cancel, and the isolate is whole") {
    Counted::Reset();
    {
        Fixture f;
        const auto cls = DeclareCounted(f.iso());
        Expose(f.context, "Counted", *cls.GetConstructor(f.context));
        Expose(f.context, "stop", Native(f.context, &StopsIsolate));
        Run(f.context, R"(
import asyncio
progress = []
async def worker():
    held = [Counted() for _ in range(4)]
    progress.append('started')
    await asyncio.sleep(0)
    stop()
    progress.append('after stop')
task = asyncio.ensure_future(worker())
)");
        f.iso().PumpJobs();
        CHECK(f.iso().IsExecutionTerminating());
        f.iso().CancelTerminateExecution();
        CHECK(EvalTruth(f.context, "progress == ['started']"));
        f.iso().PumpJobs();
        Run(f.context,
            "async def fine():\n    await asyncio.sleep(0)\n    return 9\nt2 = asyncio.ensure_future(fine())");
        f.iso().PumpJobs();
        CHECK(EvalInt(f.context, "t2.result()") == 9);
    }
    CHECK(Counted::alive == 0);
    CHECK(Counted::destroyed == 4);
}

// ===========================================================================
// asyncio's current loop
// ===========================================================================

TEST_CASE("teardown: after asyncio.run the isolate's loop is the current one again") {
    Fixture f;
    Run(f.context, R"(
import asyncio
own = asyncio.get_event_loop()
async def main():
    await asyncio.sleep(0)
    return asyncio.get_running_loop()
inner = asyncio.run(main())
)");
    CHECK(EvalTruth(f.context, "inner is not own and inner.is_closed()"));
    // A later script finds the isolate's loop, not "no current event loop".
    CHECK(EvalTruth(f.context, "asyncio.get_event_loop() is own"));
    CHECK(EvalTruth(f.context, "fut = asyncio.Future()\nfut.get_loop() is own"));
    CHECK(EvalTruth(f.context,
                    "async def seven():\n    return 7\nt = asyncio.ensure_future(seven())\nt.get_loop() is own"));
    f.iso().PumpJobs();
    CHECK(EvalInt(f.context, "t.result()") == 7);

    // A promise from C++, and a script with top-level await.
    const auto promise = ub::Promise::New(f.context);
    REQUIRE(promise.has_value());
    Expose(f.context, "p", *promise);
    const auto awaited = Eval(f.context, "await p");
    REQUIRE(awaited.IsPromise());
    REQUIRE(ub::Resolve(f.context, *promise, ub::Integer::New(f.iso(), 11)).value_or(false));
    f.iso().PumpJobs();
    CHECK(EvalTruth(f.context, "p.result() == 11"));

    // asyncio.run again, and again the loop comes back; inside a running
    // loop - a top-level-await script - it refuses, as it does in Python.
    CHECK(EvalInt(f.context, "asyncio.run(seven())") == 7);
    CHECK(EvalTruth(f.context, "asyncio.get_event_loop() is own"));
    const auto nested = Eval(f.context,
                             "c = seven()\ntry:\n    await asyncio.sleep(0)\n    asyncio.run(c)\n    outcome = "
                             "'ran'\nexcept RuntimeError:\n    c.close()\n    outcome = 'refused'");
    REQUIRE(nested.IsPromise());
    f.iso().PumpJobs();
    CHECK(EvalText(f.context, "outcome") == "refused");
    CHECK(EvalTruth(f.context, "asyncio.get_event_loop() is own"));
    // A script that sets a loop of its own keeps it.
    CHECK(EvalTruth(f.context,
                    "mine = asyncio.new_event_loop()\nasyncio.set_event_loop(mine)\n"
                    "ok = asyncio.get_event_loop() is mine\nasyncio.set_event_loop(None)\nmine.close()\nok"));
    CHECK(EvalTruth(f.context, "asyncio.get_event_loop() is own"));
}
