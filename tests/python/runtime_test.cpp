// The runtime area: stopping a script from another thread, interrupts, posted
// work, the heap and stack budgets, and the lifetime of all of it. Promises
// and the event loop are in promises_test.cpp.

#include <windows.h>

// After <windows.h>, which it needs; a block of its own so sorting keeps it there.
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "support.h"

namespace ub_test {
// tests/support/allocations.cpp
[[nodiscard]] long long OutstandingAllocations() noexcept;
}  // namespace ub_test

using namespace std::chrono_literals;
using py_test::Eval;
using py_test::EvalError;
using py_test::EvalInt;
using py_test::EvalTruth;
using py_test::Expose;
using py_test::Fixture;

namespace {

/// Waits until script is actually running, then stops it from its own thread.
///
/// "Running" is learned from an interrupt: one asked for while the isolate is
/// idle fires only at a check inside running Python, so its firing is the
/// proof. The stop then waits a little longer, so that it lands in the script's
/// loop rather than in the compiler that runs first.
class Stopper {
   public:
    explicit Stopper(ub::Isolate& isolate, std::chrono::milliseconds settle = 30ms)
        : thread_([this, &isolate, settle] {
              CHECK(isolate.RequestInterrupt(&MarkSeen, ub::CallbackData::For(*this)));
              const auto give_up = std::chrono::steady_clock::now() + 10s;
              while (!seen_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < give_up) {
                  std::this_thread::sleep_for(1ms);
              }
              std::this_thread::sleep_for(settle);
              isolate.TerminateExecution();
          }) {}
    ~Stopper() { Join(); }

    Stopper(const Stopper&) = delete;
    Stopper& operator=(const Stopper&) = delete;
    Stopper(Stopper&&) = delete;
    Stopper& operator=(Stopper&&) = delete;

    void Join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] bool Seen() const noexcept { return seen_.load(); }

   private:
    static void MarkSeen(ub::Isolate& /*isolate*/, ub::CallbackData data) {
        data.As<Stopper>()->seen_.store(true, std::memory_order_release);
    }

    std::atomic<bool> seen_{false};
    std::thread thread_;
};

/// Run `source`, which must never finish by itself, and stop it from another
/// thread. Checks that it was stopped - not thrown out of, not finished - and
/// leaves the stop in force for the caller.
void RunUntilStopped(Fixture& f, std::string_view source) {
    bool produced = true;
    bool caught = false;
    bool terminated = false;
    {
        Stopper stopper(f.iso());
        {
            const ub::HandleScope scope(f.iso());
            ub::TryCatch handler(f.iso());
            produced = ub::Evaluate(f.context, source, {.resourceName = "stopped.py"}).has_value();
            caught = handler.HasCaught();
            terminated = handler.HasTerminated();
        }
        stopper.Join();
        CHECK(stopper.Seen());
    }
    CHECK_FALSE(produced);
    CHECK(caught);
    CHECK(terminated);
    CHECK(f.iso().IsExecutionTerminating());
}

/// Nothing that runs script works while a stop is in force.
void CheckStopped(Fixture& f) {
    const ub::HandleScope scope(f.iso());
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(ub::Evaluate(f.context, "1 + 1").has_value());
    CHECK(handler.HasTerminated());
}

void CancelAndCheckUsable(Fixture& f) {
    f.iso().CancelTerminateExecution();
    CHECK_FALSE(f.iso().IsExecutionTerminating());
    CHECK(EvalInt(f.context, "40 + 2") == 42);
}

}  // namespace

// ---------------------------------------------------------------------------
// Termination
// ---------------------------------------------------------------------------

TEST_CASE("termination: a tight loop is stopped from another thread") {
    Fixture f;
    RunUntilStopped(f, "started = True\nwhile True:\n    pass\nfinished = True\n");
    CheckStopped(f);
    CancelAndCheckUsable(f);
    CHECK(EvalTruth(f.context, "started"));
    CHECK(EvalTruth(f.context, "'finished' not in globals()"));
}

TEST_CASE("termination: a loop that swallows BaseException is stopped anyway") {
    Fixture f;
    RunUntilStopped(f, R"(
started = True
caught = 0
while True:
    try:
        while True:
            pass
    except BaseException:
        caught += 1
)");
    CancelAndCheckUsable(f);
    CHECK(EvalTruth(f.context, "started"));
}

TEST_CASE("termination: a loop in a finally block is stopped, and the finally's other lines do not run") {
    Fixture f;
    RunUntilStopped(f, R"(
started = True
finally_ran = False
try:
    while True:
        pass
finally:
    finally_ran = True
    while True:
        pass
)");
    CancelAndCheckUsable(f);
    CHECK(EvalTruth(f.context, "started"));
    // Straight-line code between two eval-breaker checks is what the monitoring
    // events are for: the stop is raised again at the first line of the block.
    CHECK_FALSE(EvalTruth(f.context, "finally_ran"));
}

TEST_CASE("termination: nested calls that catch everything are stopped") {
    Fixture f;
    RunUntilStopped(f, R"(
def leaf(n):
    try:
        return sum(i for i in range(n))
    except BaseException:
        return leaf(n)

def middle():
    while True:
        try:
            leaf(100)
        except BaseException:
            continue

started = True
middle()
)");
    CancelAndCheckUsable(f);
}

TEST_CASE("termination: recursion that catches everything is stopped") {
    Fixture f;
    RunUntilStopped(f, R"(
def f():
    try:
        f()
    except BaseException:
        f()

started = True
while True:
    try:
        f()
    except BaseException:
        pass
)");
    CancelAndCheckUsable(f);
}

TEST_CASE("termination: a generator that swallows what is thrown into it is stopped") {
    Fixture f;
    RunUntilStopped(f, R"(
def forever():
    while True:
        try:
            yield 1
        except BaseException:
            pass

started = True
for _ in forever():
    pass
)");
    CancelAndCheckUsable(f);
}

TEST_CASE("termination: a builtin called in an except block does not run") {
    Fixture f;
    RunUntilStopped(f, R"(
seen = []
started = True
try:
    while True:
        pass
except BaseException:
    seen.append('caught')
)");
    CancelAndCheckUsable(f);
    CHECK(EvalInt(f.context, "len(seen)") == 0);
}

TEST_CASE("termination: a stop stays in force until it is cancelled") {
    Fixture f;
    RunUntilStopped(f, "started = True\nwhile True:\n    pass\n");
    for (int i = 0; i < 3; ++i) {
        CheckStopped(f);
    }
    {
        // A handler closing, or being reset, does not end it either.
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Evaluate(f.context, "1").has_value());
        handler.Reset();
        CHECK(f.iso().IsExecutionTerminating());
    }
    CheckStopped(f);
    CancelAndCheckUsable(f);
    // And a second cancel is harmless.
    f.iso().CancelTerminateExecution();
    CHECK(EvalInt(f.context, "6 * 7") == 42);
}

TEST_CASE("termination: a stop requested while idle stops the next thing that runs") {
    Fixture f;
    CHECK(EvalInt(f.context, "1 + 1") == 2);
    f.iso().TerminateExecution();
    CHECK(f.iso().IsExecutionTerminating());
    CheckStopped(f);
    {
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Script::Compile(f.context, "1").has_value());
        CHECK(handler.HasTerminated());
        CHECK(handler.Exception().IsEmpty());
    }
    CancelAndCheckUsable(f);
}

TEST_CASE("termination: a stop is not a throw, and a throw is not a stop") {
    Fixture f;
    const ub::HandleScope scope(f.iso());
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(ub::Evaluate(f.context, "raise ValueError('ordinary')").has_value());
    CHECK(handler.HasCaught());
    CHECK_FALSE(handler.HasTerminated());
    CHECK_FALSE(f.iso().IsExecutionTerminating());
    // Catching `unibind.Terminated` by hand is not a stop either: only the
    // flag is.
    handler.Reset();
    CHECK(EvalInt(f.context,
                  "import unibind\ntry:\n    raise unibind.Terminated()\nexcept BaseException:\n    x = 5\nx") == 5);
}

TEST_CASE("termination: stopping the same isolate many times") {
    Fixture f;
    for (int round = 0; round < 5; ++round) {
        CAPTURE(round);
        RunUntilStopped(f, "started = True\nwhile True:\n    pass\n");
        CancelAndCheckUsable(f);
    }
}

TEST_CASE("termination: a stop that lands in the compiler is a stop") {
    Fixture f;
    // A script long enough that compiling it takes a while; stopped with no
    // settling time, it is as likely to land in the compile as in the loop.
    std::string source = "started = True\n";
    for (int i = 0; i < 2000; ++i) {
        source += "v" + std::to_string(i) + " = " + std::to_string(i) + " * 2\n";
    }
    source += "while True:\n    pass\n";
    for (int round = 0; round < 3; ++round) {
        bool produced = true;
        bool terminated = false;
        {
            Stopper stopper(f.iso(), 0ms);
            const ub::HandleScope scope(f.iso());
            ub::TryCatch handler(f.iso());
            produced = ub::Evaluate(f.context, source).has_value();
            terminated = handler.HasTerminated();
        }
        CHECK_FALSE(produced);
        CHECK(terminated);
        CancelAndCheckUsable(f);
    }
}

TEST_CASE("termination: cancelled while nothing ran, a stop leaves nothing behind") {
    Fixture f;
    f.iso().TerminateExecution();
    f.iso().CancelTerminateExecution();
    // The pending call it queued is still there, and does nothing.
    CHECK(EvalInt(f.context, "total = 0\nfor i in range(10000):\n    total += 1\ntotal") == 10000);
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

namespace {

struct InterruptLog {
    std::mutex mutex;
    std::vector<int> order;
    std::vector<std::thread::id> threads;
    ub::Isolate* requestAgain = nullptr;
    int again = 0;
    bool madeHandle = false;
    bool sawStop = false;

    void Note(int id) {
        const std::lock_guard<std::mutex> lock(mutex);
        order.push_back(id);
        threads.push_back(std::this_thread::get_id());
    }
    std::vector<int> Order() {
        const std::lock_guard<std::mutex> lock(mutex);
        return order;
    }
};

struct Tagged {
    InterruptLog* log;
    int id;
};

void NoteTagged(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    Tagged* tag = data.As<Tagged>();
    tag->log->Note(tag->id);
}

}  // namespace

TEST_CASE("interrupts: requests run in order, once each, at the next script") {
    Fixture f;
    InterruptLog log;
    Tagged tags[] = {{&log, 1}, {&log, 2}, {&log, 3}};
    for (Tagged& tag : tags) {
        CHECK(f.iso().RequestInterrupt(&NoteTagged, ub::CallbackData::For(tag)));
    }
    // Idle: nothing checks, nothing runs - not even a pump.
    std::this_thread::sleep_for(20ms);
    f.iso().PumpJobs();
    CHECK(log.Order().empty());

    CHECK(EvalInt(f.context, "total = 0\nfor i in range(1000):\n    total += i\ntotal") == 499500);
    CHECK(log.Order() == std::vector<int>{1, 2, 3});
    for (const std::thread::id id : log.threads) {
        CHECK(id == std::this_thread::get_id());
    }

    CHECK(EvalInt(f.context, "1") == 1);
    CHECK(log.Order().size() == 3);
}

TEST_CASE("interrupts: a null callback is refused") {
    Fixture f;
    CHECK_FALSE(f.iso().RequestInterrupt(nullptr, {}));
}

namespace {

void AskAgain(ub::Isolate& isolate, ub::CallbackData data) {
    auto* log = data.As<InterruptLog>();
    log->Note(10 + log->again);
    if (log->again++ < 3) {
        CHECK(isolate.RequestInterrupt(&AskAgain, data));
    }
}

}  // namespace

TEST_CASE("interrupts: one asked for from inside a callback runs in the same pass") {
    Fixture f;
    InterruptLog log;
    CHECK(f.iso().RequestInterrupt(&AskAgain, ub::CallbackData::For(log)));
    // One check is enough for all four: the pass runs until nothing waits.
    // `pass` as a whole script still reaches a check - the compiler is Python.
    CHECK(Eval(f.context, "pass").IsUndefined());
    CHECK(log.Order() == std::vector<int>{10, 11, 12, 13});
}

TEST_CASE("interrupts: an interrupt waits out a stop, and runs after the cancel") {
    Fixture f;
    InterruptLog log;
    Tagged tag{&log, 7};
    f.iso().TerminateExecution();
    CHECK(f.iso().RequestInterrupt(&NoteTagged, ub::CallbackData::For(tag)));
    CheckStopped(f);
    CHECK(log.Order().empty());
    f.iso().CancelTerminateExecution();
    CHECK(log.Order().empty());
    CHECK(EvalInt(f.context, "2 + 2") == 4);
    CHECK(log.Order() == std::vector<int>{7});
}

namespace {

void SampleThenStop(ub::Isolate& isolate, ub::CallbackData data) {
    auto* log = data.As<InterruptLog>();
    // A callback may make handles: it has a scope of its own.
    const auto text = ub::String::NewFromUtf8(isolate, "sampled");
    log->madeHandle = text.has_value() && text->Utf8Value() == "sampled";
    log->sawStop = isolate.IsExecutionTerminating();
    isolate.TerminateExecution();
}

void ThrowFromInterrupt(ub::Isolate& isolate, ub::CallbackData data) {
    data.As<InterruptLog>()->Note(1);
    isolate.ThrowError(ub::ErrorKind::Error, "an interrupt may not throw");
}

}  // namespace

TEST_CASE("interrupts: a watchdog samples a running script, then decides to stop it") {
    Fixture f;
    InterruptLog log;
    std::thread watchdog([&] {
        std::this_thread::sleep_for(30ms);
        CHECK(f.iso().RequestInterrupt(&SampleThenStop, ub::CallbackData::For(log)));
    });
    bool produced = true;
    bool terminated = false;
    {
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        produced = ub::Evaluate(f.context, "while True:\n    pass\n").has_value();
        terminated = handler.HasTerminated();
    }
    watchdog.join();
    CHECK_FALSE(produced);
    CHECK(terminated);
    CHECK(log.madeHandle);
    CHECK_FALSE(log.sawStop);
    CancelAndCheckUsable(f);
}

TEST_CASE("interrupts: what a callback throws is cleared, not handed to the script") {
    Fixture f;
    InterruptLog log;
    CHECK(f.iso().RequestInterrupt(&ThrowFromInterrupt, ub::CallbackData::For(log)));
    const ub::HandleScope scope(f.iso());
    ub::TryCatch handler(f.iso());
    const auto result = ub::Evaluate(f.context, "total = 0\nfor i in range(100):\n    total += i\ntotal");
    REQUIRE(result.has_value());
    CHECK_FALSE(handler.HasCaught());
    CHECK(log.Order() == std::vector<int>{1});
    CHECK(result->To<ub::Integer>()->Int32Value() == 4950);
}

TEST_CASE("interrupts: many requested from many threads while a script runs all run, once each") {
    Fixture f;
    InterruptLog log;
    constexpr int THREADS = 4;
    constexpr int EACH = 50;
    std::vector<Tagged> tags;
    tags.reserve(THREADS * EACH);
    for (int i = 0; i < THREADS * EACH; ++i) {
        tags.push_back({&log, i});
    }
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < EACH; ++i) {
                CHECK(f.iso().RequestInterrupt(&NoteTagged, ub::CallbackData::For(tags[t * EACH + i])));
                if (i % 10 == 0) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            done.fetch_add(1);
        });
    }
    // Run script until every request has been made, then once more so the
    // last of them fire.
    while (done.load() < THREADS) {
        CHECK(EvalInt(f.context, "t = 0\nfor i in range(20000):\n    t += 1\nt") == 20000);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(EvalInt(f.context, "1") == 1);
    std::vector<int> order = log.Order();
    REQUIRE(order.size() == static_cast<std::size_t>(THREADS * EACH));
    // Each thread's own requests ran in the order it made them.
    for (int t = 0; t < THREADS; ++t) {
        int last = -1;
        for (const int id : order) {
            if (id / EACH == t) {
                CHECK(id > last);
                last = id;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Posted work
// ---------------------------------------------------------------------------

namespace {

struct JobLog {
    std::mutex mutex;
    std::vector<int> order;
    std::vector<std::thread::id> threads;

    void Note(int id) {
        const std::lock_guard<std::mutex> lock(mutex);
        order.push_back(id);
        threads.push_back(std::this_thread::get_id());
    }
    std::vector<int> Order() {
        const std::lock_guard<std::mutex> lock(mutex);
        return order;
    }
};

struct Job {
    JobLog* log;
    int id;
    ub::Isolate* postAgain = nullptr;
    Job* next = nullptr;
    bool stop = false;
    bool throwToo = false;
};

void RunJob(ub::Isolate& isolate, ub::CallbackData data) {
    Job* job = data.As<Job>();
    job->log->Note(job->id);
    if (job->next != nullptr) {
        CHECK(isolate.PostJob(&RunJob, ub::CallbackData::For(*job->next)));
    }
    if (job->throwToo) {
        isolate.ThrowError(ub::ErrorKind::Error, "a job threw");
    }
    if (job->stop) {
        isolate.TerminateExecution();
    }
}

}  // namespace

TEST_CASE("jobs: posted work waits for the pump, and runs in order, once each, on the isolate's thread") {
    Fixture f;
    JobLog log;
    Job jobs[] = {{&log, 1}, {&log, 2}, {&log, 3}};
    for (Job& job : jobs) {
        CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(job)));
    }
    // Posting the same work twice runs it twice.
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(jobs[0])));
    CHECK(EvalInt(f.context, "1") == 1);
    CHECK(log.Order().empty());
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1, 2, 3, 1});
    for (const std::thread::id id : log.threads) {
        CHECK(id == std::this_thread::get_id());
    }
    f.iso().PumpJobs();
    CHECK(log.Order().size() == 4);
}

TEST_CASE("jobs: a null callback is refused, delayed or not") {
    Fixture f;
    CHECK_FALSE(f.iso().PostJob(nullptr, {}));
    CHECK_FALSE(f.iso().PostDelayedJob(nullptr, {}, 0.0));
    CHECK_FALSE(f.iso().PostDelayedJob(nullptr, {}, 1.0));
}

TEST_CASE("jobs: work posted from a job is drained by the same pump") {
    Fixture f;
    JobLog log;
    Job third{&log, 3};
    Job second{&log, 2, nullptr, &third};
    Job first{&log, 1, nullptr, &second};
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(first)));
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1, 2, 3});
}

TEST_CASE("jobs: what a job throws stops at the pump") {
    Fixture f;
    JobLog log;
    Job throwing{&log, 1};
    throwing.throwToo = true;
    Job after{&log, 2};
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(throwing)));
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(after)));
    {
        ub::TryCatch handler(f.iso());
        f.iso().PumpJobs();
        CHECK_FALSE(handler.HasCaught());
    }
    CHECK_FALSE(f.iso().HasPendingException());
    CHECK(log.Order() == std::vector<int>{1, 2});
}

TEST_CASE("jobs: a stop in a job ends the pump, and the rest waits for the cancel") {
    Fixture f;
    JobLog log;
    Job stopping{&log, 1};
    stopping.stop = true;
    Job after{&log, 2};
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(stopping)));
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(after)));
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1});
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1});
    f.iso().CancelTerminateExecution();
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1, 2});
}

TEST_CASE("jobs: nothing is pumped while a stop is in force, and the queue survives it") {
    Fixture f;
    JobLog log;
    Job job{&log, 1};
    f.iso().TerminateExecution();
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(job)));
    f.iso().PumpJobs();
    CHECK(log.Order().empty());
    f.iso().CancelTerminateExecution();
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1});
}

TEST_CASE("jobs: delayed work waits for its delay, and then for the pump") {
    Fixture f;
    JobLog log;
    Job job{&log, 1};
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(job), 0.05));
    f.iso().PumpJobs();
    CHECK(log.Order().empty());
    std::this_thread::sleep_for(80ms);
    CHECK(log.Order().empty());
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1});
}

TEST_CASE("jobs: delayed work runs in the order it fell due, after work already queued") {
    Fixture f;
    JobLog log;
    Job later{&log, 1};
    Job sooner{&log, 2};
    Job tie{&log, 3};
    Job plain{&log, 4};
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(later), 0.04));
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(sooner), 0.01));
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(tie), 0.04));
    std::this_thread::sleep_for(70ms);
    CHECK(f.iso().PostJob(&RunJob, ub::CallbackData::For(plain)));
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{4, 2, 1, 3});
}

TEST_CASE("jobs: a delay that is zero, negative or not a number is no delay, and one past the clock is never") {
    Fixture f;
    JobLog log;
    Job zero{&log, 1};
    Job negative{&log, 2};
    Job nan{&log, 3};
    Job infinite{&log, 4};
    Job huge{&log, 5};
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(zero), 0.0));
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(negative), -5.0));
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(nan), std::numeric_limits<double>::quiet_NaN()));
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(infinite), std::numeric_limits<double>::infinity()));
    CHECK(f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(huge), 1e30));
    f.iso().PumpJobs();
    CHECK(log.Order() == std::vector<int>{1, 2, 3});
}

namespace {

struct ScriptJob {
    ub::Context* context;
    int answer = 0;
    bool noRealm = false;
};

void RunScriptJob(ub::Isolate& isolate, ub::CallbackData data) {
    auto* job = data.As<ScriptJob>();
    // No realm entered and no scope open for us: open our own.
    const ub::HandleScope scope(isolate);
    const ub::ContextScope entered(*job->context);
    ub::TryCatch handler(isolate);
    const auto result = ub::Evaluate(*job->context, "21 * 2");
    if (result) {
        job->answer = result->To<ub::Integer>()->Int32Value();
    }
}

}  // namespace

TEST_CASE("jobs: a job can open a scope and a realm of its own and run script") {
    Fixture f;
    ScriptJob job{&f.context};
    CHECK(f.iso().PostJob(&RunScriptJob, ub::CallbackData::For(job)));
    f.iso().PumpJobs();
    CHECK(job.answer == 42);
}

TEST_CASE("jobs: work posted from other threads while the isolate pumps runs once each, in each thread's order") {
    Fixture f;
    JobLog log;
    constexpr int THREADS = 4;
    constexpr int EACH = 200;
    std::vector<Job> jobs;
    jobs.reserve(THREADS * EACH);
    for (int i = 0; i < THREADS * EACH; ++i) {
        jobs.push_back({&log, i});
    }
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < EACH; ++i) {
                const bool delayed = i % 7 == 0;
                auto& job = jobs[t * EACH + i];
                CHECK((delayed ? f.iso().PostDelayedJob(&RunJob, ub::CallbackData::For(job), 0.001)
                               : f.iso().PostJob(&RunJob, ub::CallbackData::For(job))));
            }
            done.fetch_add(1);
        });
    }
    while (done.load() < THREADS) {
        f.iso().PumpJobs();
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::this_thread::sleep_for(10ms);
    f.iso().PumpJobs();
    const std::vector<int> order = log.Order();
    REQUIRE(order.size() == static_cast<std::size_t>(THREADS * EACH));
    std::vector<int> seen(THREADS * EACH, 0);
    for (const int id : order) {
        ++seen[id];
    }
    for (const int count : seen) {
        CHECK(count == 1);
    }
    // Undelayed work from one thread runs in the order that thread posted it.
    for (int t = 0; t < THREADS; ++t) {
        int last = -1;
        for (const int id : order) {
            if (id / EACH == t && (id % EACH) % 7 != 0) {
                CHECK(id > last);
                last = id;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------

TEST_CASE("heap: the statistics are sane and follow what the script allocates") {
    Fixture f;
    const ub::HeapStatistics before = f.iso().GetHeapStatistics();
    CHECK(before.totalBytes >= before.usedBytes);
    CHECK(before.limitBytes >= before.usedBytes);
    CHECK(before.limitBytes > 0);
    REQUIRE(before.mallocedBytes.has_value());
    REQUIRE(before.peakMallocedBytes.has_value());

    CHECK(Eval(f.context, "big = bytearray(16 * 1024 * 1024)").IsUndefined());
    const ub::HeapStatistics grown = f.iso().GetHeapStatistics();
    CHECK(grown.usedBytes >= before.usedBytes + 16 * 1024 * 1024);
    CHECK(grown.totalBytes >= grown.usedBytes);
    CHECK(grown.limitBytes >= grown.usedBytes);
    CHECK(*grown.peakMallocedBytes >= grown.usedBytes);

    CHECK(Eval(f.context, "del big").IsUndefined());
    f.iso().RequestGarbageCollection();
    const ub::HeapStatistics shrunk = f.iso().GetHeapStatistics();
    CHECK(shrunk.usedBytes + 15 * 1024 * 1024 < grown.usedBytes);
    CHECK(*shrunk.peakMallocedBytes >= grown.usedBytes);
}

TEST_CASE("heap: a limit is reported as the limit") {
    auto isolate = ub::Isolate::New({.heapLimitBytes = 96 * 1024 * 1024});
    REQUIRE(isolate != nullptr);
    CHECK(isolate->GetHeapStatistics().limitBytes == 96 * 1024 * 1024);
}

TEST_CASE("heap: a script that reaches the limit gets a MemoryError it can catch, and a fault is reported") {
    auto isolate = ub::Isolate::New({.heapLimitBytes = 64 * 1024 * 1024});
    REQUIRE(isolate != nullptr);
    const ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    REQUIRE(context.has_value());
    const ub::ContextScope entered(*context);

    const std::uint64_t faults = py_test::OutOfMemoryReports();
    CHECK(EvalError(*context, "x = bytearray(256 * 1024 * 1024)").find("MemoryError") != std::string::npos);
    CHECK(py_test::OutOfMemoryReports() > faults);

    // Growing one piece at a time reaches it too, and the script survives.
    CHECK(EvalTruth(*context, R"(
chunks = []
try:
    while True:
        chunks.append(bytearray(1024 * 1024))
except MemoryError:
    chunks = None
    survived = True
survived
)"));
    const ub::HeapStatistics stats = isolate->GetHeapStatistics();
    CHECK(stats.usedBytes <= stats.limitBytes);
    CHECK(stats.limitBytes == 64 * 1024 * 1024);
    // And the isolate carries on.
    CHECK(EvalInt(*context, "sum(range(100))") == 4950);
}

namespace {

struct Rescue {
    int asked = 0;
    std::size_t sawCurrent = 0;
    std::size_t sawInitial = 0;
    bool terminate = true;
    bool raise = true;
};

std::size_t RescueOnce(ub::Isolate& isolate, std::size_t current, std::size_t initial, ub::CallbackData data) {
    auto* rescue = data.As<Rescue>();
    ++rescue->asked;
    rescue->sawCurrent = current;
    rescue->sawInitial = initial;
    if (rescue->terminate) {
        isolate.TerminateExecution();
    }
    return rescue->raise && rescue->asked == 1 ? initial * 2 : current;
}

}  // namespace

TEST_CASE("heap: the limit callback raises the ceiling and stops the script - the rescue") {
    auto isolate = ub::Isolate::New({.heapLimitBytes = 48 * 1024 * 1024});
    REQUIRE(isolate != nullptr);
    const ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    REQUIRE(context.has_value());
    const ub::ContextScope entered(*context);

    Rescue rescue;
    isolate->SetHeapLimitCallback(&RescueOnce, ub::CallbackData::For(rescue));
    {
        ub::TryCatch handler(*isolate);
        CHECK_FALSE(ub::Evaluate(*context, R"(
hoard = []
while True:
    try:
        hoard.append(bytearray(512 * 1024))
    except MemoryError:
        pass
)")
                        .has_value());
        CHECK(handler.HasTerminated());
    }
    CHECK(rescue.asked == 1);
    CHECK(rescue.sawCurrent == 48 * 1024 * 1024);
    CHECK(rescue.sawInitial == 48 * 1024 * 1024);
    CHECK(isolate->GetHeapStatistics().limitBytes == 96 * 1024 * 1024);
    isolate->CancelTerminateExecution();
    CHECK(Eval(*context, "hoard = None").IsUndefined());
    CHECK(EvalInt(*context, "7 * 6") == 42);
}

TEST_CASE("heap: a callback that declines is asked once per crossing, and the script gets MemoryError") {
    auto isolate = ub::Isolate::New({.heapLimitBytes = 48 * 1024 * 1024});
    REQUIRE(isolate != nullptr);
    const ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    REQUIRE(context.has_value());
    const ub::ContextScope entered(*context);

    Rescue rescue;
    rescue.terminate = false;
    rescue.raise = false;
    isolate->SetHeapLimitCallback(&RescueOnce, ub::CallbackData::For(rescue));
    // Fill the heap to its ceiling, then keep failing there: one crossing,
    // because the count never goes well back under in between.
    CHECK(EvalInt(*context, R"(
hoard = []
try:
    while True:
        hoard.append(bytearray(256 * 1024))
except MemoryError:
    pass
failures = 0
for _ in range(20):
    try:
        hoard.append(bytearray(1024 * 1024))
    except MemoryError:
        failures += 1
failures
)") == 20);
    CHECK(rescue.asked == 1);
    // Let go, and the next time it fills is a new crossing, asked again.
    CHECK(EvalInt(*context, R"(
hoard = None
again = 0
try:
    hoard = []
    while True:
        hoard.append(bytearray(256 * 1024))
except MemoryError:
    again = 1
hoard = None
again
)") == 1);
    CHECK(rescue.asked == 2);
    CHECK(isolate->GetHeapStatistics().limitBytes == 48 * 1024 * 1024);

    // A null callback stops the asking.
    isolate->SetHeapLimitCallback(nullptr, {});
    CHECK(EvalError(*context, "y = bytearray(100 * 1024 * 1024)").find("MemoryError") != std::string::npos);
    CHECK(rescue.asked == 2);
}

TEST_CASE("heap: a limit belongs to its isolate, not to the thread's next one or to the process") {
    {
        auto limited = ub::Isolate::New({.heapLimitBytes = 32 * 1024 * 1024});
        REQUIRE(limited != nullptr);
    }
    // Same thread, no limit now.
    Fixture f;
    CHECK(Eval(f.context, "big = bytearray(128 * 1024 * 1024)\ndel big").IsUndefined());

    // And a limited isolate on another thread does not limit this one.
    std::thread other([] {
        auto limited = ub::Isolate::New({.heapLimitBytes = 32 * 1024 * 1024});
        REQUIRE(limited != nullptr);
        const ub::HandleScope scope(*limited);
        auto context = ub::Context::New(*limited);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);
        CHECK(EvalError(*context, "x = bytearray(64 * 1024 * 1024)").find("MemoryError") != std::string::npos);
    });
    other.join();
    CHECK(Eval(f.context, "big = bytearray(64 * 1024 * 1024)\ndel big").IsUndefined());
}

// ---------------------------------------------------------------------------
// Stack
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view DEEP_PYTHON = R"(
def down(n):
    return down(n + 1)
try:
    down(0)
    outcome = 'returned'
except RecursionError:
    outcome = 'caught'
outcome
)";

/// Recursion that goes through C at every level - `map` calls the lambda,
/// which re-enters the evaluation loop - and so consumes native stack.
constexpr std::string_view DEEP_NATIVE = R"(
depth = 0
def down():
    global depth
    depth += 1
    return next(map(lambda _: down(), [0]))
try:
    down()
    outcome = 'returned'
except RecursionError:
    outcome = 'caught'
outcome
)";

/// The same through a builtin with a large frame: `sorted` keeps a 2 KB merge
/// buffer on the stack, and calls `key` from under it. CPython 3.12 guarded
/// native recursion with a count, which a frame this size outran - it
/// overflowed the stack, in a stock `python.exe` 3.12 too. 3.14 checks the
/// stack pointer itself.
constexpr std::string_view DEEP_SORTED = R"(
def down(_):
    return sorted([0, 1], key=down)
try:
    down(0)
    outcome = 'returned'
except RecursionError:
    outcome = 'caught'
outcome
)";

constexpr std::string_view DEEP_REPR = R"(
nested = []
for _ in range(200000):
    nested = [nested]
try:
    repr(nested)
    outcome = 'returned'
except RecursionError:
    outcome = 'caught'
nested = None
outcome
)";

/// Run `body` on a thread with a stack of `bytes`, as an embedder's own
/// thread would be made.
template <class F>
void OnThreadWithStack(std::size_t bytes, F body) {
    struct Box {
        F* body;
    } box{&body};
    const HANDLE thread = CreateThread(
        nullptr, bytes,
        [](void* arg) -> DWORD {
            (*static_cast<Box*>(arg)->body)();
            return 0;
        },
        &box, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    REQUIRE(thread != nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

int DepthReached(const ub::IsolateOptions& options) {
    int depth = -1;
    auto isolate = ub::Isolate::New(options);
    REQUIRE(isolate != nullptr);
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);
        CHECK(py_test::EvalText(*context, DEEP_NATIVE) == "caught");
        depth = EvalInt(*context, "depth");
    }
    return depth;
}

}  // namespace

/// The smallest thread stack the cases below run an isolate on. A debug
/// CPython's evaluation loop is unoptimised and some twenty times the size, and
/// bringing an interpreter up - before anything here is in play - overflows a
/// 256 KB stack by itself.
#if defined(NDEBUG)
constexpr std::size_t SMALL_STACK = std::size_t{256} * 1024;
#else
constexpr std::size_t SMALL_STACK = std::size_t{1024} * 1024;
#endif

TEST_CASE("stack: runaway recursion is a RecursionError, not a crash, on threads of several sizes") {
    for (const std::size_t stack : {SMALL_STACK, std::size_t{1024} * 1024, std::size_t{8} * 1024 * 1024}) {
        CAPTURE(stack);
        OnThreadWithStack(stack, [] {
            Fixture f;
            CHECK(py_test::EvalText(f.context, DEEP_PYTHON) == "caught");
            CHECK(py_test::EvalText(f.context, DEEP_NATIVE) == "caught");
            CHECK(py_test::EvalText(f.context, DEEP_SORTED) == "caught");
            CHECK(py_test::EvalText(f.context, DEEP_REPR) == "caught");
            // And the isolate is fine afterwards.
            CHECK(EvalInt(f.context, "sum(range(10))") == 45);
        });
    }
}

TEST_CASE("stack: a smaller stackLimitBytes stops native recursion sooner") {
    int roomy = 0;
    int tight = 0;
    // A debug CPython's evaluation loop costs some twenty times the stack a
    // level, so a debug build gets a limit four times the size.
#if defined(NDEBUG)
    constexpr std::size_t TIGHT = std::size_t{256} * 1024;
#else
    constexpr std::size_t TIGHT = std::size_t{1024} * 1024;
#endif
    OnThreadWithStack(4 * 1024 * 1024, [&] {
        roomy = DepthReached({});
        tight = DepthReached({.stackLimitBytes = TIGHT});
    });
    CAPTURE(roomy);
    CAPTURE(tight);
    CHECK(tight > 10);
    CHECK(tight < roomy);
}

TEST_CASE("stack: a stackLimitBytes past the end of the thread's stack is held to the stack") {
    OnThreadWithStack(2 * SMALL_STACK, [] {
        auto isolate = ub::Isolate::New({.stackLimitBytes = std::size_t{1} << 30});
        REQUIRE(isolate != nullptr);
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);
        CHECK(py_test::EvalText(*context, DEEP_NATIVE) == "caught");
    });
}

// ---------------------------------------------------------------------------
// Several isolates at once
// ---------------------------------------------------------------------------

TEST_CASE("concurrency: isolates on several threads stop, interrupt, pump and allocate at once") {
    constexpr int THREADS = 6;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([t, &failures] {
            auto isolate = ub::Isolate::New({.heapLimitBytes = (t % 2 == 0) ? std::size_t{128} * 1024 * 1024 : 0});
            if (isolate == nullptr) {
                failures.fetch_add(1);
                return;
            }
            const ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            REQUIRE(context.has_value());
            const ub::ContextScope entered(*context);
            for (int round = 0; round < 3; ++round) {
                // A stop from a watchdog thread of its own.
                {
                    Stopper stopper(*isolate, 10ms);
                    ub::TryCatch handler(*isolate);
                    CHECK_FALSE(ub::Evaluate(*context, "x = 0\nwhile True:\n    x += 1\n").has_value());
                    CHECK(handler.HasTerminated());
                }
                isolate->CancelTerminateExecution();

                JobLog log;
                Job jobs[] = {{&log, 1}, {&log, 2}};
                for (Job& job : jobs) {
                    CHECK(isolate->PostJob(&RunJob, ub::CallbackData::For(job)));
                }
                isolate->PumpJobs();
                CHECK(log.Order() == std::vector<int>{1, 2});

                CHECK(EvalInt(*context, "len(bytearray(4 * 1024 * 1024))") == 4 * 1024 * 1024);
                const ub::HeapStatistics stats = isolate->GetHeapStatistics();
                CHECK(stats.usedBytes <= stats.limitBytes);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(failures.load() == 0);
}

TEST_CASE("concurrency: isolates on different threads run Python in parallel") {
    if (std::thread::hardware_concurrency() < 4) {
        MESSAGE("fewer than four hardware threads; nothing to measure");
        return;
    }
    // Only the Python is timed: each thread makes its isolate first, then all
    // of them start together. Sharing one lock, each of four would take about
    // four times as long as one alone; with a lock each, about as long. Best
    // of three, so that a busy machine does not fail it.
    constexpr std::string_view SOURCE = "t = 0\nfor i in range(3_000_000):\n    t += 1\nt";
    const auto alone = [&] {
        Fixture f;
        const auto start = std::chrono::steady_clock::now();
        CHECK(EvalInt(f.context, SOURCE) == 3000000);
        return std::chrono::steady_clock::now() - start;
    };
    const auto together = [&] {
        constexpr int THREADS = 4;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::chrono::steady_clock::duration> took(THREADS);
        std::vector<std::thread> threads;
        for (int i = 0; i < THREADS; ++i) {
            threads.emplace_back([&, i] {
                Fixture f;
                ready.fetch_add(1);
                while (!go.load()) {
                    std::this_thread::yield();
                }
                const auto start = std::chrono::steady_clock::now();
                const auto total = Eval(f.context, SOURCE).To<ub::Integer>();
                took[i] = std::chrono::steady_clock::now() - start;
                CHECK((total && total->Int32Value() == 3000000));
            });
        }
        while (ready.load() < THREADS) {
            std::this_thread::yield();
        }
        go.store(true);
        for (std::thread& thread : threads) {
            thread.join();
        }
        return *std::max_element(took.begin(), took.end());
    };
    double best = 1e9;
    for (int attempt = 0; attempt < 3 && best >= 2.5; ++attempt) {
        const auto one = alone();
        const auto four = together();
        best = (std::min)(best, std::chrono::duration<double>(four) / std::chrono::duration<double>(one));
    }
    CAPTURE(best);
    CHECK(best < 2.5);
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

namespace {

struct NeverRuns {
    std::atomic<int> ran{0};
};

void MustNotRun(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    data.As<NeverRuns>()->ran.fetch_add(1);
}

[[nodiscard]] std::size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters)) != 0);
    return counters.PrivateUsage;
}

}  // namespace

TEST_CASE("lifetime: queued jobs, delayed jobs and interrupts are dropped with the isolate, not run") {
    // The data is freed after the isolate: nothing may touch it once it is gone.
    auto never = std::make_unique<NeverRuns>();
    {
        Fixture f;
        CHECK(f.iso().PostJob(&MustNotRun, ub::CallbackData::For(*never)));
        CHECK(f.iso().PostDelayedJob(&MustNotRun, ub::CallbackData::For(*never), 0.001));
        CHECK(f.iso().PostDelayedJob(&MustNotRun, ub::CallbackData::For(*never), 1000.0));
        CHECK(f.iso().RequestInterrupt(&MustNotRun, ub::CallbackData::For(*never)));
        std::this_thread::sleep_for(5ms);
    }
    CHECK(never->ran.load() == 0);
    never.reset();
}

TEST_CASE("lifetime: an isolate destroyed while stopped, with work queued, goes cleanly") {
    NeverRuns never;
    {
        Fixture f;
        RunUntilStopped(f, "started = True\nwhile True:\n    pass\n");
        CHECK(f.iso().PostJob(&MustNotRun, ub::CallbackData::For(never)));
        CHECK(f.iso().RequestInterrupt(&MustNotRun, ub::CallbackData::For(never)));
        // Not cancelled: the isolate goes with the stop in force.
    }
    {
        Fixture f;
        f.iso().TerminateExecution();
    }
    CHECK(never.ran.load() == 0);
}

TEST_CASE("lifetime: a stop requested from another thread just before the isolate goes is safe") {
    for (int i = 0; i < 20; ++i) {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        std::thread stopper([&] { isolate->TerminateExecution(); });
        stopper.join();  // the isolate outlives the call: the embedder's rule
        isolate.reset();
    }
}

namespace {

/// What churning `count` isolates left behind, per isolate: in the process's
/// private bytes, and in the C++ heap (the backend's own allocations, which
/// tests/support/allocations.cpp counts) - on this thread, or spread over
/// `threads` threads.
struct Cost {
    std::int64_t processBytes = 0;
    double allocations = 0;
};

[[nodiscard]] Cost CostPerIsolate(int count, int threads, bool busy) {
    const auto churn = [busy](int n) {
        NeverRuns never;
        for (int i = 0; i < n; ++i) {
            auto isolate = ub::Isolate::New({.heapLimitBytes = busy ? std::size_t{256} * 1024 * 1024 : 0});
            REQUIRE(isolate != nullptr);
            {
                const ub::HandleScope scope(*isolate);
                auto context = ub::Context::New(*isolate);
                REQUIRE(context.has_value());
                const ub::ContextScope entered(*context);
                CHECK(EvalInt(*context, "len([str(i) for i in range(10000)])") == 10000);
                if (busy) {
                    // Everything this area keeps, left behind for teardown.
                    (void)isolate->PostJob(&MustNotRun, ub::CallbackData::For(never));
                    (void)isolate->PostDelayedJob(&MustNotRun, ub::CallbackData::For(never), 1000.0);
                    (void)isolate->RequestInterrupt(&MustNotRun, ub::CallbackData::For(never));
                    isolate->TerminateExecution();
                    CHECK_FALSE(ub::Evaluate(*context, "1").has_value());
                }
            }
        }
        CHECK(never.ran.load() == 0);
    };
    const std::size_t before = PrivateBytes();
    const long long allocationsBefore = ub_test::OutstandingAllocations();
    if (threads <= 1) {
        churn(count);
    } else {
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] { churn(count / threads); });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
    return {.processBytes = (static_cast<std::int64_t>(PrivateBytes()) - static_cast<std::int64_t>(before)) / count,
            .allocations = static_cast<double>(ub_test::OutstandingAllocations() - allocationsBefore) / count};
}

}  // namespace

TEST_CASE("lifetime: many isolates in sequence and on many threads leak nothing of this area's") {
    constexpr int THREADS = 6;
    // Warm up whatever the process keeps once: the heap grows to hold as many
    // interpreters as are ever alive at once, and keeps that.
    (void)CostPerIsolate(4, 1, true);
    (void)CostPerIsolate(THREADS, THREADS, true);
    const Cost plain = CostPerIsolate(20, 1, false);
    const Cost busy = CostPerIsolate(20, 1, true);
    // Six threads at once.
    const Cost threaded = CostPerIsolate(24, THREADS, true);
    CAPTURE(plain.processBytes);
    CAPTURE(busy.processBytes);
    CAPTURE(threaded.processBytes);
    CAPTURE(plain.allocations);
    CAPTURE(busy.allocations);
    CAPTURE(threaded.allocations);
    MESSAGE("process memory kept per isolate: plain "
            << plain.processBytes / 1024 << " KB, with queued work and a stop " << busy.processBytes / 1024
            << " KB, on " << THREADS << " threads " << threaded.processBytes / 1024 << " KB");
    // What this area allocates for itself - its state, the queues, the timers,
    // the interrupts - is counted exactly, and whatever an isolate left for
    // teardown is given back with it. The heap account an isolate charged is
    // pooled, and once every block charged to it has been freed - which with
    // CPython 3.14 is at the end of the interpreter - the next isolate takes
    // it over; the threaded figure is the pool growing to as many accounts as
    // there are isolates alive at once.
    CHECK(plain.allocations <= 0.5);
    CHECK(busy.allocations <= 0.5);
    CHECK(threaded.allocations <= 0.5);
    // CPython 3.14 frees a sub-interpreter's object arenas when it ends - if
    // not one block of them is still allocated. A single reference kept past
    // the end, or a cycle the collector cannot see, keeps all of them, several
    // megabytes an isolate (3.12 kept them always, about 9.5 MB). So an
    // isolate that is gone must cost the process next to nothing. Process
    // memory is a noisy measure - the heap's own fragmentation, and other
    // processes pressing on this one's working set - so the bound is an
    // average of a megabyte, well clear of the noise and well under what a
    // kept interpreter costs (3.5 MB, release). A debug heap is noisier - the
    // same runs swing a megabyte either way - so there it is two.
#if defined(NDEBUG)
    constexpr std::int64_t BOUND = std::int64_t{1024} * 1024;
#else
    constexpr std::int64_t BOUND = std::int64_t{2} * 1024 * 1024;
#endif
    CHECK(plain.processBytes < BOUND);
    CHECK(busy.processBytes < BOUND);
    CHECK(threaded.processBytes < BOUND);
}

// ---------------------------------------------------------------------------
// Natives
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] ub::Local<ub::Function> Native(const ub::Context& context, ub::FunctionCallback callback,
                                             ub::CallbackData data = {}) {
    auto function = ub::Function::New(context, callback, data);
    REQUIRE(function.has_value());
    return *function;
}

/// Calls its argument with itself: native recursion that never enters the
/// evaluation loop, so CPython's own count never sees it.
void CallsItself(const ub::CallbackInfo& info) {
    const auto function = info[0].To<ub::Function>();
    if (!function) {
        return;
    }
    const std::array<ub::Local<ub::Value>, 1> arguments{info[0]};
    (void)function->Call(info.GetContext(), ub::Undefined(info.GetIsolate()), arguments);
}

/// Calls its argument, a Python function, with no arguments.
void CallsBack(const ub::CallbackInfo& info) {
    const auto function = info[0].To<ub::Function>();
    if (!function) {
        return;
    }
    (void)function->Call(info.GetContext(), ub::Undefined(info.GetIsolate()), {});
}

void StopsItsIsolate(const ub::CallbackInfo& info) {
    info.GetIsolate().TerminateExecution();
}

struct Marks {
    std::atomic<int> count{0};
    std::atomic<bool> sawStop{false};
};

void Mark(const ub::CallbackInfo& info) {
    info.Data<Marks>()->count.fetch_add(1);
}

/// Spins until it is told to stop, as a well-behaved long-running native
/// does: `IsExecutionTerminating` is the only thing that reaches it.
void SpinsUntilStopped(const ub::CallbackInfo& info) {
    auto* marks = info.Data<Marks>();
    marks->count.fetch_add(1);
    const auto give_up = std::chrono::steady_clock::now() + 10s;
    while (!info.GetIsolate().IsExecutionTerminating() && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    marks->sawStop.store(info.GetIsolate().IsExecutionTerminating());
}

}  // namespace

TEST_CASE("stack: native recursion that never enters Python is a RecursionError, not a crash") {
    for (const std::size_t stack : {SMALL_STACK, std::size_t{2048} * 1024}) {
        CAPTURE(stack);
        OnThreadWithStack(stack, [] {
            Fixture f;
            Expose(f.context, "recurse", Native(f.context, &CallsItself));
            CHECK(py_test::EvalText(f.context, R"(
try:
    recurse(recurse)
    outcome = 'returned'
except RecursionError:
    outcome = 'caught'
outcome
)") == "caught");
            CHECK(EvalInt(f.context, "sum(range(10))") == 45);
        });
    }
}

TEST_CASE("stack: recursion bouncing between a native and Python is a RecursionError, not a crash") {
    for (const std::size_t stack : {SMALL_STACK, std::size_t{2048} * 1024}) {
        CAPTURE(stack);
        OnThreadWithStack(stack, [] {
            Fixture f;
            Expose(f.context, "bounce", Native(f.context, &CallsBack));
            CHECK(py_test::EvalText(f.context, R"(
def down():
    bounce(down)
try:
    down()
    outcome = 'returned'
except RecursionError:
    outcome = 'caught'
outcome
)") == "caught");
            CHECK(EvalInt(f.context, "sum(range(10))") == 45);
        });
    }
}

TEST_CASE("termination: a native that stops its own isolate stops the script that called it") {
    Fixture f;
    Expose(f.context, "stop", Native(f.context, &StopsItsIsolate));
    {
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Evaluate(f.context, "before = True\nstop()\nafter = True\n").has_value());
        CHECK(handler.HasTerminated());
    }
    CancelAndCheckUsable(f);
    CHECK(EvalTruth(f.context, "before"));
    CHECK(EvalTruth(f.context, "'after' not in globals()"));
}

TEST_CASE("termination: a native is not interrupted, sees the stop, and the script stops when it returns") {
    Fixture f;
    Marks marks;
    Expose(f.context, "spin", Native(f.context, &SpinsUntilStopped, ub::CallbackData::For(marks)));
    std::thread stopper([&] {
        while (marks.count.load() == 0) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(20ms);
        f.iso().TerminateExecution();
    });
    {
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Evaluate(f.context, "spin()\nafter = True\n").has_value());
        CHECK(handler.HasTerminated());
    }
    stopper.join();
    CHECK(marks.sawStop.load());
    CancelAndCheckUsable(f);
    CHECK(EvalTruth(f.context, "'after' not in globals()"));
}

TEST_CASE("termination: a native called from a finally block of a stopped script does not run") {
    Fixture f;
    Marks marks;
    Expose(f.context, "mark", Native(f.context, &Mark, ub::CallbackData::For(marks)));
    RunUntilStopped(f, R"(
started = True
try:
    while True:
        pass
finally:
    mark()
)");
    CancelAndCheckUsable(f);
    CHECK(marks.count.load() == 0);
}

TEST_CASE("termination: a stopped isolate refuses a native's call back into Python") {
    Fixture f;
    Expose(f.context, "bounce", Native(f.context, &CallsBack));
    Expose(f.context, "stop", Native(f.context, &StopsItsIsolate));
    {
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Evaluate(f.context, R"(
reached = []
def inner():
    reached.append(1)
def outer():
    stop()
bounce(outer)
bounce(inner)
)")
                        .has_value());
        CHECK(handler.HasTerminated());
    }
    CancelAndCheckUsable(f);
    CHECK(EvalInt(f.context, "len(reached)") == 0);
}
