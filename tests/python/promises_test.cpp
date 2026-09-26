// Promises and the event loop: `ub::Promise` is an `asyncio.Future` of the
// isolate's own loop, and `PumpJobs` is what runs the loop.
//
// Everything here needs `import asyncio` to work inside an isolate, which
// needs the interpreter's C extension modules (`_asyncio`, `_socket`,
// `select`). The suite is named so that a build without them reports these
// as the cases that fail, rather than failing somewhere less obvious:
// `--test-suite-exclude=asyncio` runs the rest.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "support.h"

using namespace std::chrono_literals;
using py_test::Eval;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Expose;
using py_test::Fixture;

namespace {

[[nodiscard]] ub::Local<ub::Promise> NewPromise(const ub::Context& context) {
    auto promise = ub::Promise::New(context);
    REQUIRE(promise.has_value());
    return *promise;
}

[[nodiscard]] ub::Local<ub::Promise> AsPromise(const ub::Local<ub::Value>& value) {
    auto promise = value.To<ub::Promise>();
    REQUIRE(promise.has_value());
    return *promise;
}

}  // namespace

TEST_SUITE("asyncio") {
    TEST_CASE("promises: asyncio imports in an isolate, and the isolate's loop is the current one") {
        Fixture f;
        CHECK(EvalTruth(f.context, "import asyncio\nasyncio.get_event_loop() is not None"));
        CHECK(EvalTruth(f.context, "asyncio.get_event_loop() is asyncio.get_event_loop()"));
        CHECK_FALSE(EvalTruth(f.context, "asyncio.get_event_loop().is_running()"));
    }

    TEST_CASE("promises: native makes one, settles it once, and reads its state") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        CHECK(promise.IsPromise());
        CHECK(ub::GetState(promise) == ub::PromiseState::Pending);
        CHECK(ub::Resolve(f.context, promise, ub::Integer::New(f.iso(), 42)) == std::optional<bool>(true));
        CHECK(ub::GetState(promise) == ub::PromiseState::Fulfilled);
        // The first settlement is the only one.
        CHECK(ub::Resolve(f.context, promise, ub::Integer::New(f.iso(), 1)) == std::optional<bool>(false));
        CHECK(ub::Reject(f.context, promise, ub::Integer::New(f.iso(), 1)) == std::optional<bool>(false));
        Expose(f.context, "p", promise);
        CHECK(EvalInt(f.context, "p.result()") == 42);
    }

    TEST_CASE("promises: a rejection with a value that is not an exception keeps the value") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        CHECK(ub::Reject(f.context, promise, py_test::Str(f.iso(), "no")) == std::optional<bool>(true));
        CHECK(ub::GetState(promise) == ub::PromiseState::Rejected);
        CHECK(ub::Resolve(f.context, promise, py_test::Str(f.iso(), "late")) == std::optional<bool>(false));
        Expose(f.context, "p", promise);
        // Script awaiting it sees `unibind.Thrown` carrying the value ...
        const auto task = AsPromise(Eval(f.context, R"(
import unibind
try:
    await p
    outcome = 'fulfilled'
except unibind.Thrown as thrown:
    outcome = thrown.value
outcome
)"));
        f.iso().PumpJobs();
        CHECK(ub::GetState(task) == ub::PromiseState::Fulfilled);
        CHECK(EvalText(f.context, "outcome") == "no");
        // ... and a handler outside it gets the value back.
        const ub::HandleScope scope(f.iso());
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(ub::Evaluate(f.context, "p.result()").has_value());
        REQUIRE(handler.HasCaught());
        CHECK(py_test::TextOf(f.context, handler.Exception()) == "no");
    }

    TEST_CASE("promises: an exception reason is the exception") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        const auto error = Eval(f.context, "ValueError('bad')");
        CHECK(ub::Reject(f.context, promise, error) == std::optional<bool>(true));
        Expose(f.context, "p", promise);
        CHECK(EvalTruth(f.context, "isinstance(p.exception(), ValueError)"));
    }

    TEST_CASE("promises: a continuation waits for the pump and nothing else") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        Expose(f.context, "p", promise);
        CHECK(Eval(f.context, "seen = []\np.add_done_callback(lambda done: seen.append(done.result()))").IsUndefined());
        CHECK(ub::Resolve(f.context, promise, ub::Integer::New(f.iso(), 7)) == std::optional<bool>(true));
        CHECK(EvalInt(f.context, "len(seen)") == 0);
        CHECK(EvalInt(f.context, "len(seen)") == 0);
        f.iso().PumpJobs();
        CHECK(EvalTruth(f.context, "seen == [7]"));
    }

    TEST_CASE("promises: a script with top-level await evaluates to a promise the pump settles") {
        Fixture f;
        const auto result = AsPromise(Eval(f.context, "import asyncio\nawait asyncio.sleep(0)\n40 + 2"));
        CHECK(ub::GetState(result) == ub::PromiseState::Pending);
        f.iso().PumpJobs();
        CHECK(ub::GetState(result) == ub::PromiseState::Fulfilled);
        Expose(f.context, "t", result);
        CHECK(EvalInt(f.context, "t.result()") == 42);
    }

    TEST_CASE("promises: script waits on a native promise, and the native settles it") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        Expose(f.context, "p", promise);
        const auto task = AsPromise(Eval(f.context, "value = await p\nvalue * 2"));
        f.iso().PumpJobs();
        CHECK(ub::GetState(task) == ub::PromiseState::Pending);
        CHECK(ub::Resolve(f.context, promise, ub::Integer::New(f.iso(), 21)) == std::optional<bool>(true));
        f.iso().PumpJobs();
        REQUIRE(ub::GetState(task) == ub::PromiseState::Fulfilled);
        Expose(f.context, "t", task);
        CHECK(EvalInt(f.context, "t.result()") == 42);
    }

    TEST_CASE("promises: a rejection propagates through a chain of awaits") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        Expose(f.context, "p", promise);
        const auto task = AsPromise(Eval(f.context, R"(
async def inner():
    return await p
async def middle():
    return await inner()
await middle()
)"));
        const auto error = Eval(f.context, "KeyError('missing')");
        CHECK(ub::Reject(f.context, promise, error) == std::optional<bool>(true));
        f.iso().PumpJobs();
        CHECK(ub::GetState(task) == ub::PromiseState::Rejected);
        Expose(f.context, "t", task);
        CHECK(EvalTruth(f.context, "isinstance(t.exception(), KeyError)"));
    }

    TEST_CASE("promises: resolving with another promise follows it") {
        Fixture f;
        const auto outer = NewPromise(f.context);
        const auto inner = NewPromise(f.context);
        CHECK(ub::Resolve(f.context, outer, inner) == std::optional<bool>(true));
        // Locked in: pending, and not resolvable again.
        CHECK(ub::GetState(outer) == ub::PromiseState::Pending);
        CHECK(ub::Resolve(f.context, outer, ub::Integer::New(f.iso(), 1)) == std::optional<bool>(false));
        f.iso().PumpJobs();
        CHECK(ub::GetState(outer) == ub::PromiseState::Pending);
        CHECK(ub::Resolve(f.context, inner, ub::Integer::New(f.iso(), 5)) == std::optional<bool>(true));
        f.iso().PumpJobs();
        CHECK(ub::GetState(outer) == ub::PromiseState::Fulfilled);
        Expose(f.context, "outer", outer);
        CHECK(EvalInt(f.context, "outer.result()") == 5);
    }

    TEST_CASE("promises: resolving a promise with itself rejects it") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        CHECK(ub::Resolve(f.context, promise, promise) == std::optional<bool>(true));
        CHECK(ub::GetState(promise) == ub::PromiseState::Rejected);
    }

    TEST_CASE("promises: a future and a task script made are promises too") {
        Fixture f;
        const auto future = Eval(f.context, "import asyncio\nfut = asyncio.get_event_loop().create_future()\nfut");
        CHECK(future.IsPromise());
        CHECK(ub::GetState(AsPromise(future)) == ub::PromiseState::Pending);
        CHECK(ub::Resolve(f.context, AsPromise(future), ub::Integer::New(f.iso(), 3)) == std::optional<bool>(true));
        CHECK(ub::GetState(AsPromise(future)) == ub::PromiseState::Fulfilled);
        const auto cancelled = Eval(f.context, "c = asyncio.get_event_loop().create_future()\nc.cancel()\nc");
        CHECK(ub::GetState(AsPromise(cancelled)) == ub::PromiseState::Rejected);
    }

    TEST_CASE("promises: callbacks see the loop running, and continuations queued by continuations drain") {
        Fixture f;
        CHECK(Eval(f.context, R"(
import asyncio
loop = asyncio.get_event_loop()
order = []
def step(n):
    order.append((n, asyncio.get_running_loop() is loop))
    if n < 5:
        loop.call_soon(step, n + 1)
_ = loop.call_soon(step, 0)
)")
                  .IsUndefined());
        f.iso().PumpJobs();
        CHECK(EvalInt(f.context, "len(order)") == 6);
        CHECK(EvalTruth(f.context, "all(running for _, running in order)"));
        // Outside the pump the loop is not running.
        CHECK(EvalTruth(f.context, "not loop.is_running()"));
    }

    TEST_CASE("promises: a timer that is not due does not block the pump") {
        Fixture f;
        const auto task = AsPromise(Eval(f.context, "import asyncio\nawait asyncio.sleep(0.1)\n'woke'"));
        const auto start = std::chrono::steady_clock::now();
        f.iso().PumpJobs();
        CHECK(std::chrono::steady_clock::now() - start < 50ms);
        CHECK(ub::GetState(task) == ub::PromiseState::Pending);
        std::this_thread::sleep_for(150ms);
        f.iso().PumpJobs();
        CHECK(ub::GetState(task) == ub::PromiseState::Fulfilled);
    }

    namespace {

    struct Settle {
        ub::Context* context;
        ub::Global<ub::Promise>* promise;
        std::vector<int>* order;
    };

    void SettleJob(ub::Isolate& isolate, ub::CallbackData data) {
        auto* settle = data.As<Settle>();
        const ub::HandleScope scope(isolate);
        const ub::ContextScope entered(*settle->context);
        settle->order->push_back(1);
        const auto promise = settle->promise->Get(isolate);
        CHECK(ub::Resolve(*settle->context, promise, ub::Integer::New(isolate, 9)) == std::optional<bool>(true));
    }

    void NoteJob(ub::Isolate& /*isolate*/, ub::CallbackData data) {
        data.As<std::vector<int>>()->push_back(3);
    }

    }  // namespace

    TEST_CASE("promises: a job settles one and the continuation runs before the next job") {
        Fixture f;
        std::vector<int> order;
        ub::Global<ub::Promise> global(f.iso(), NewPromise(f.context));
        Expose(f.context, "p", global.Get(f.iso()));
        CHECK(Eval(f.context, "import asyncio\nseen = []\np.add_done_callback(lambda d: seen.append(d.result()))")
                  .IsUndefined());
        Settle settle{&f.context, &global, &order};
        CHECK(f.iso().PostJob(&SettleJob, ub::CallbackData::For(settle)));
        CHECK(f.iso().PostJob(&NoteJob, ub::CallbackData::For(order)));
        f.iso().PumpJobs();
        CHECK(order == std::vector<int>{1, 3});
        CHECK(EvalTruth(f.context, "seen == [9]"));
        global.Reset();
    }

    TEST_CASE("promises: a stop in a continuation ends the pump and discards what is queued behind it") {
        Fixture f;
        CHECK(Eval(f.context, R"(
import asyncio
loop = asyncio.get_event_loop()
behind = []
def spin():
    while True:
        pass
_ = loop.call_soon(spin)
_ = loop.call_soon(lambda: behind.append('ran'))
)")
                  .IsUndefined());
        std::thread stopper([&] {
            std::this_thread::sleep_for(50ms);
            f.iso().TerminateExecution();
        });
        f.iso().PumpJobs();
        stopper.join();
        CHECK(f.iso().IsExecutionTerminating());
        f.iso().CancelTerminateExecution();
        f.iso().PumpJobs();
        CHECK(EvalInt(f.context, "len(behind)") == 0);
        // The loop is intact: it runs the next thing it is given.
        CHECK(Eval(f.context, "_ = loop.call_soon(lambda: behind.append('later'))").IsUndefined());
        f.iso().PumpJobs();
        CHECK(EvalTruth(f.context, "behind == ['later']"));
    }

    TEST_CASE("promises: nothing on the loop runs while a stop is in force, and it waits for the cancel") {
        Fixture f;
        const auto task = AsPromise(Eval(f.context, "import asyncio\nawait asyncio.sleep(0)\n1"));
        f.iso().TerminateExecution();
        f.iso().PumpJobs();
        CHECK(ub::GetState(task) == ub::PromiseState::Pending);
        f.iso().CancelTerminateExecution();
        f.iso().PumpJobs();
        CHECK(ub::GetState(task) == ub::PromiseState::Fulfilled);
    }

    TEST_CASE("promises: a stopped isolate refuses to make or settle one") {
        Fixture f;
        const auto promise = NewPromise(f.context);
        f.iso().TerminateExecution();
        CHECK_FALSE(ub::Promise::New(f.context).has_value());
        CHECK_FALSE(ub::Resolve(f.context, promise, ub::Integer::New(f.iso(), 1)).has_value());
        f.iso().CancelTerminateExecution();
        CHECK(ub::Resolve(f.context, promise, ub::Integer::New(f.iso(), 1)) == std::optional<bool>(true));
    }

    TEST_CASE("promises: an isolate destroyed with pending tasks, timers and futures goes quietly") {
        for (int i = 0; i < 3; ++i) {
            Fixture f;
            CHECK(Eval(f.context, R"(
import asyncio
loop = asyncio.get_event_loop()
async def forever():
    await loop.create_future()
started = loop.create_task(forever())
never_started = loop.create_task(forever())
_ = loop.call_later(1000, print, 'never')
failed = loop.create_future()
failed.set_exception(ValueError('never retrieved'))
)")
                      .IsUndefined());
            // One pump starts one of them; the other is never stepped.
            f.iso().PumpJobs();
            (void)NewPromise(f.context);
        }
    }

    TEST_CASE("promises: isolates on several threads each drive their own loop") {
        std::vector<std::thread> threads;
        std::atomic<int> fulfilled{0};
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&fulfilled] {
                // Nothing that REQUIREs on a thread of our own: a REQUIRE
                // throws, and a throw out of a thread is std::terminate.
                Fixture f;
                const auto result = Eval(f.context,
                                         "import asyncio\ntotal = 0\nfor i in range(50):\n    await asyncio.sleep(0)\n "
                                         "   total += i\ntotal");
                const auto task = result.To<ub::Promise>();
                if (!task) {
                    return;
                }
                for (int round = 0; round < 100 && ub::GetState(*task) == ub::PromiseState::Pending; ++round) {
                    f.iso().PumpJobs();
                }
                const auto total = Eval(f.context, "total").To<ub::Integer>();
                if (ub::GetState(*task) == ub::PromiseState::Fulfilled && total && total->Int32Value() == 1225) {
                    fulfilled.fetch_add(1);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        CHECK(fulfilled.load() == 4);
    }
}
