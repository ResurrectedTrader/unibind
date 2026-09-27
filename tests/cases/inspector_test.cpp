/// \file
/// The inspector: Chrome DevTools, over the protocol, where the engine has one.
///
/// Two kinds of case, and the split is `unibind/inspector.h`'s own. Whether an
/// inspector exists is a question every backend answers - `Supported()` - and
/// the first case holds every backend to answering it consistently. What an
/// inspector *does* is a question only one that has one can answer, so the
/// rest ask `Supported()` first and report a skip when it says no. They never
/// ask which backend they are on: the suite does not name engines, and a third
/// backend with an inspector should pass these unchanged.
///
/// The protocol is asserted only as far as it has to be - an id came back, a
/// value is in it, a notification of a given method arrived - by looking for
/// text in what the client was sent. Everything else about a message is the
/// engine's.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/harness.h"

namespace {

/// A client that keeps what it is sent, and runs a pause by dispatching what
/// the case queued for it.
struct Client final : ub::InspectorClient {
    Client() = default;

    void SendProtocolMessage(std::string_view message) override { messages.emplace_back(message); }

    void RunMessageLoopOnPause() override {
        ++pauses;
        // What a real client's loop depends on: DevTools only knows to send a
        // resume once it has been told the script is paused.
        if (Saw(R"("method":"Debugger.paused")")) {
            ++pausesAnnounced;
        }
        quit = false;
        if (duringPause) {
            // What the embedder does when the connection closes mid-pause. It
            // may take the session with it, and then there is nothing to feed.
            duringPause();
            if (quit || session == nullptr) {
                return;
            }
        }
        for (const std::string& message : onPause) {
            if (quit) {
                break;
            }
            session->DispatchProtocolMessage(message);
        }
        // Nothing left to feed and still paused: a real loop would wait on its
        // socket, and a case that got here would wait for ever. Resuming from
        // outside the protocol ends it the other way.
        if (!quit) {
            session->Resume();
        }
    }

    void QuitMessageLoopOnPause() override {
        ++quits;
        quit = true;
    }

    std::optional<std::string> ResourceNameToUrl(std::string_view resourceName) override {
        if (!urlPrefix) {
            return std::nullopt;
        }
        return *urlPrefix + std::string(resourceName);
    }

    /// Whether any message sent so far contains `needle`.
    [[nodiscard]] bool Saw(std::string_view needle) const {
        for (const std::string& message : messages) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// The response to request `id`, or empty if there was none.
    [[nodiscard]] std::string ResponseTo(int id) const {
        const std::string marker = "\"id\":" + std::to_string(id) + ",";
        for (const std::string& message : messages) {
            if (message.find(marker) != std::string::npos) {
                return message;
            }
        }
        return {};
    }

    std::vector<std::string> messages;
    std::vector<std::string> onPause;
    std::function<void()> duringPause;
    ub::InspectorSession* session = nullptr;
    std::optional<std::string> urlPrefix;
    int pauses = 0;
    int pausesAnnounced = 0;
    int quits = 0;
    bool quit = false;
};

/// An inspector over the fixture's realm, with one session connected - or a
/// skip, when the backend has no inspector.
struct Attached {
    std::unique_ptr<ub::Inspector> inspector;
    std::unique_ptr<ub::InspectorSession> session;

    [[nodiscard]] explicit operator bool() const noexcept { return session != nullptr; }
};

[[nodiscard]] Attached Attach(ub_test::Fixture& fixture, Client& client) {
    Attached attached;
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return attached;
    }
    attached.inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(attached.inspector != nullptr);
    attached.inspector->ContextCreated(fixture.context, "main");
    attached.session = attached.inspector->Connect();
    REQUIRE(attached.session != nullptr);
    client.session = attached.session.get();
    return attached;
}

/// A dispatch made from another thread, and where and whether it ran.
struct Remote {
    ub::InspectorSession* session = nullptr;
    std::string message;
    std::thread::id ranOn;
    std::atomic<bool> ran{false};
};

void DispatchRemote(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* remote = data.As<Remote>();
    if (remote == nullptr) {
        return;
    }
    remote->ranOn = std::this_thread::get_id();
    remote->session->DispatchProtocolMessage(remote->message);
    remote->ran = true;
}

}  // namespace

UNIBIND_TEST_CASE(INSPECTOR, "inspector: an inspector exists exactly where the backend says it does") {
    // The one question every backend answers, and it links everywhere: a
    // program compiled once asks, and is told.
    ub_test::Fixture fixture;
    Client client;

    auto inspector = ub::Inspector::New(fixture.iso(), client);
    CHECK((inspector != nullptr) == ub::Inspector::Supported());
    MESSAGE("this backend ", std::string(ub::Inspector::Supported() ? "has an" : "has no"), " inspector");
    if (inspector == nullptr) {
        return;
    }

    // One per isolate, and another once that one has gone.
    Client second;
    CHECK(ub::Inspector::New(fixture.iso(), second) == nullptr);
    inspector.reset();
    CHECK(ub::Inspector::New(fixture.iso(), second) != nullptr);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Runtime.evaluate is answered through the client") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6*7"}})");
    const std::string answer = client.ResponseTo(1);
    INFO(answer);
    CHECK(answer.find("\"value\":42") != std::string::npos);

    // It runs in the realm the inspector was shown, and sees its globals.
    ub_test::Expose(fixture.context, "fromEmbedder", ub_test::Str(fixture.iso(), "hello"));
    attached.session->DispatchProtocolMessage(
        R"({"id":2,"method":"Runtime.evaluate","params":{"expression":"fromEmbedder.length"}})");
    CHECK(client.ResponseTo(2).find("\"value\":5") != std::string::npos);

    // And what goes in is UTF-8: a two-byte character is one character.
    attached.session->DispatchProtocolMessage(
        "{\"id\":3,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"'\xC3\xA9'.length\"}}");
    CHECK(client.ResponseTo(3).find("\"value\":1") != std::string::npos);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a debugger statement pauses into the client's loop until it resumes") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":10,"method":"Debugger.enable"})");
    REQUIRE_FALSE(client.ResponseTo(10).empty());
    client.onPause = {R"({"id":11,"method":"Debugger.resume"})"};

    // The pause happens inside this call, and the embedder's loop is what ends
    // it; the script then finishes as though nothing had happened.
    CHECK(ub_test::EvalInt(fixture.context, "var before = 1; debugger; before + 1") == 2);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.quits >= 1);
    CHECK(client.Saw("\"method\":\"Debugger.paused\""));
    CHECK(client.Saw("\"method\":\"Debugger.resumed\""));
    CHECK_FALSE(client.ResponseTo(11).empty());

    // A pause with nothing to feed it is left from outside the protocol.
    client.onPause.clear();
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 3") == 3);
    CHECK(client.pauses == 2);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a dispatch requested from another thread reaches a busy isolate") {
    // The case RequestDispatch exists for: DevTools asks while a script is
    // running, on a thread that is not the isolate's, and gets an answer -
    // here, one that stops the script.
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    Remote remote;
    remote.session = attached.session.get();
    remote.message =
        R"({"id":20,"method":"Runtime.evaluate","params":{"expression":"globalThis.stopLooping = true; 42"}})";
    std::atomic<bool> finished{false};

    const std::shared_ptr<ub::InspectorDispatcher> dispatcher = attached.inspector->Dispatcher();
    std::thread requester([dispatcher, &remote] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)dispatcher->RequestDispatch(&DispatchRemote, ub::CallbackData::For(remote));
    });
    // A request that never lands would leave the loop below spinning for ever;
    // this turns that into a failure instead of a hung suite.
    ub::Isolate* isolate = &fixture.iso();
    std::thread watchdog([isolate, &finished] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished) {
            isolate->TerminateExecution();
        }
    });

    ub_test::Expose(fixture.context, "stopLooping", ub::False(fixture.iso()));
    const auto result = ub::Evaluate(fixture.context, "while (!globalThis.stopLooping) {} 'finished'");
    finished = true;
    requester.join();
    watchdog.join();
    fixture.iso().CancelTerminateExecution();

    REQUIRE(result.has_value());
    CHECK(ub_test::TextOf(*result) == "finished");
    CHECK(remote.ran);
    CHECK(remote.ranOn == std::this_thread::get_id());
    CHECK(client.ResponseTo(20).find("\"value\":42") != std::string::npos);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a dispatch requested while the isolate is idle runs at the next pump") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    Remote remote;
    remote.session = attached.session.get();
    remote.message = R"({"id":30,"method":"Runtime.evaluate","params":{"expression":"'idle'"}})";

    const std::shared_ptr<ub::InspectorDispatcher> dispatcher = attached.inspector->Dispatcher();
    std::thread requester(
        [dispatcher, &remote] { (void)dispatcher->RequestDispatch(&DispatchRemote, ub::CallbackData::For(remote)); });
    requester.join();

    // No script is running, so nothing has checked for it yet.
    CHECK_FALSE(remote.ran);
    fixture.iso().PumpJobs();
    CHECK(remote.ran);
    CHECK(remote.ranOn == std::this_thread::get_id());
    CHECK(client.ResponseTo(30).find("idle") != std::string::npos);

    // Once: the interrupt that was also requested finds nothing left to run.
    remote.ran = false;
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
    fixture.iso().PumpJobs();
    CHECK_FALSE(remote.ran);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a script's URL is what the client says it is") {
    ub_test::Fixture fixture;
    Client client;
    client.urlPrefix = "file:///app/";
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":40,"method":"Debugger.enable"})");
    CHECK(ub_test::EvalInt(fixture.context, "7") == 7);
    const auto ran = ub::Evaluate(fixture.context, "8", {.resourceName = "main.js"});
    REQUIRE(ran.has_value());

    CHECK(client.Saw("\"method\":\"Debugger.scriptParsed\""));
    CHECK(client.Saw("file:///app/main.js"));
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: realms come and go, and a stopped session is let go cleanly") {
    ub_test::Fixture fixture;
    Client client;
    Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":50,"method":"Runtime.enable"})");
    REQUIRE_FALSE(client.ResponseTo(50).empty());

    {
        auto second = ub::Context::New(fixture.iso());
        REQUIRE(second.has_value());
        attached.inspector->ContextCreated(*second, "second");
        CHECK(client.Saw("\"name\":\"second\""));
        attached.inspector->ContextDestroyed(*second);
        CHECK(client.Saw("\"method\":\"Runtime.executionContextDestroyed\""));
    }

    // With the second realm gone, the default is the one announced before it
    // again, so an evaluation naming no context still has somewhere to run.
    attached.session->DispatchProtocolMessage(R"({"id":51,"method":"Runtime.evaluate","params":{"expression":"2+3"}})");
    CHECK(client.ResponseTo(51).find("\"value\":5") != std::string::npos);

    attached.session->Stop();
    attached.session.reset();
    attached.inspector->ContextDestroyed(fixture.context);
    attached.inspector.reset();

    // An inspector made and let go many times gives back what it took.
    const auto cycle = [&fixture] {
        Client again;
        auto inspector = ub::Inspector::New(fixture.iso(), again);
        if (inspector == nullptr) {
            return;
        }
        inspector->ContextCreated(fixture.context, "cycled");
        auto session = inspector->Connect();
        again.session = session.get();
        session->DispatchProtocolMessage(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"1"}})");
        session->Stop();
        session.reset();
        inspector->ContextDestroyed(fixture.context);
    };
    constexpr int WINDOW = 20;
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long afterWarmup = ub_test::OutstandingAllocations();
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long growth = ub_test::OutstandingAllocations() - afterWarmup;
    INFO("outstanding allocations grew by ", growth, " over ", WINDOW, " inspectors");
    CHECK(growth < WINDOW);
}

// ---------------------------------------------------------------------------
// The dispatcher: the one object another thread may hold
// ---------------------------------------------------------------------------

namespace {

void CountDispatch(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    if (auto* count = data.As<std::atomic<int>>()) {
        ++*count;
    }
}

/// A request that says which one it was.
struct Numbered {
    std::vector<int>* log = nullptr;
    int number = 0;
};

void LogNumber(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    if (auto* numbered = data.As<Numbered>()) {
        numbered->log->push_back(numbered->number);
    }
}

}  // namespace

UNIBIND_TEST_CASE(INSPECTOR,
                  "inspector: a dispatcher may be used from another thread while and after its inspector goes") {
    // A socket thread cannot know when the isolate's thread destroys the
    // inspector - that is the whole difficulty - so the thing it holds has to
    // stay safe to call through the destruction and after it, and say no.
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    std::atomic<int> ran{0};
    int taken = 0;
    int declined = 0;
    constexpr int ROUNDS = 40;
    constexpr int BURST = 100;
    for (int round = 0; round < ROUNDS; ++round) {
        Client client;
        auto inspector = ub::Inspector::New(fixture.iso(), client);
        REQUIRE(inspector != nullptr);
        std::atomic<bool> burstDone{false};
        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::atomic<int> takenHere{0};
        std::atomic<int> declinedHere{0};
        // The thread's copy is the last one standing, so the dispatcher is
        // also destroyed on the foreign thread.
        std::thread hammer(
            [dispatcher = inspector->Dispatcher(), &burstDone, &go, &stop, &ran, &takenHere, &declinedHere] {
                const auto request = [&] {
                    if (dispatcher->RequestDispatch(&CountDispatch, ub::CallbackData::For(ran))) {
                        ++takenHere;
                    } else {
                        ++declinedHere;
                    }
                };
                for (int i = 0; i < BURST; ++i) {
                    request();
                }
                burstDone = true;
                while (!go) {
                    std::this_thread::yield();
                }
                while (!stop) {
                    request();
                }
            });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!burstDone && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        // While the thread holds off, what it asked for runs. Not while it
        // hammers: a pump runs until nothing is left, and nothing is ever left
        // while another thread keeps asking.
        fixture.iso().PumpJobs();
        CHECK(ran == (round + 1) * BURST);

        // The race itself: the inspector goes while the thread is mid-request.
        go = true;
        while (takenHere < 2 * BURST && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        inspector.reset();
        while (declinedHere == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        stop = true;
        hammer.join();
        taken += takenHere;
        declined += declinedHere;

        // Nothing runs once the inspector has gone - not from the jobs it
        // posted, not from the interrupts it requested.
        const int before = ran;
        fixture.iso().PumpJobs();
        CHECK(ub_test::EvalInt(fixture.context, "for (var i = 0; i < 1000; ++i) {} 2") == 2);
        fixture.iso().PumpJobs();
        CHECK(ran == before);
    }
    INFO("taken ", taken, ", declined ", declined, ", ran ", ran.load());
    CHECK(ran == ROUNDS * BURST);
    CHECK(taken > ROUNDS * BURST);
    CHECK(declined >= ROUNDS);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a burst of requests shares one wake-up, and every request still runs once") {
    // What is coalesced is the wake-up and never the work: a request is a
    // callback and its data, run exactly once, in order - asked for twice, run
    // twice. The wake-up - an engine interrupt and a posted job - is what a
    // burst made before the isolate gets to it does not need one each of, and
    // the only place that shows is in what the burst costs.
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    Client client;
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    const std::shared_ptr<ub::InspectorDispatcher> dispatcher = inspector->Dispatcher();

    constexpr int BURST = 2000;
    std::vector<int> log;
    log.reserve(BURST + 4);
    std::vector<Numbered> requests(BURST);
    for (int i = 0; i < BURST; ++i) {
        requests[static_cast<size_t>(i)] = Numbered{.log = &log, .number = i};
    }
    int taken = 0;
    const long long before = ub_test::OutstandingAllocations();
    for (Numbered& request : requests) {
        taken += dispatcher->RequestDispatch(&LogNumber, ub::CallbackData::For(request)) ? 1 : 0;
    }
    const long long growth = ub_test::OutstandingAllocations() - before;
    CHECK(taken == BURST);
    // The requests themselves have to be kept somewhere; a wake-up each on top
    // of that - a job and an interrupt per request - would be two more.
    INFO("a burst of ", BURST, " requests left ", growth, " allocations outstanding");
    CHECK(growth < BURST + (BURST / 2));

    fixture.iso().PumpJobs();
    REQUIRE(log.size() == static_cast<size_t>(BURST));
    bool inOrder = true;
    for (int i = 0; i < BURST; ++i) {
        inOrder = inOrder && log[static_cast<size_t>(i)] == i;
    }
    CHECK(inOrder);

    // The same callback and data twice is two requests.
    log.clear();
    CHECK(dispatcher->RequestDispatch(&LogNumber, ub::CallbackData::For(requests[7])));
    CHECK(dispatcher->RequestDispatch(&LogNumber, ub::CallbackData::For(requests[7])));
    CHECK(ub_test::EvalInt(fixture.context, "for (var i = 0; i < 1000; ++i) {} 3") == 3);
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<int>{7, 7});

    // A null callback is not a request.
    CHECK_FALSE(dispatcher->RequestDispatch(nullptr));
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: New and Connect answer null only when there is not the memory") {
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    Client client;

    std::unique_ptr<ub::Inspector> refused;
    long long fired = 0;
    {
        ub_test::AllocationFailure failing(1);
        refused = ub::Inspector::New(fixture.iso(), client);
        fired = failing.Stop();
    }
    if (fired == 0) {
        ub_test::ReportSkip("the failed allocation never reached the inspector");
        return;
    }
    CHECK(refused == nullptr);
    // The refusal left nothing behind: the isolate takes an inspector.
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    inspector->ContextCreated(fixture.context, "main");

    std::unique_ptr<ub::InspectorSession> none;
    {
        ub_test::AllocationFailure failing(1);
        none = inspector->Connect();
        fired = failing.Stop();
    }
    CHECK(fired == 1);
    CHECK(none == nullptr);

    // And in no other case: several at once, and one made during a pause.
    auto first = inspector->Connect();
    auto second = inspector->Connect();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    client.session = first.get();
    first->DispatchProtocolMessage(R"({"id":70,"method":"Debugger.enable"})");
    std::unique_ptr<ub::InspectorSession> duringPause;
    client.duringPause = [&inspector, &duringPause] { duringPause = inspector->Connect(); };
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 8") == 8);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(duringPause != nullptr);

    duringPause.reset();
    second.reset();
    first.reset();
    inspector->ContextDestroyed(fixture.context);
}

// ---------------------------------------------------------------------------
// Resume, Stop and destruction, in every order
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Resume leaves a pause and does nothing outside one") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }
    attached.session->DispatchProtocolMessage(R"({"id":80,"method":"Debugger.enable"})");

    attached.session->Resume();
    attached.session->Resume();
    CHECK(client.quits == 0);

    // The client's loop resumes from outside the protocol when it has nothing
    // to feed, and the inspector answers with a quit before Resume returns.
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 9") == 9);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.quits == 1);

    attached.session->Resume();
    CHECK(client.quits == 1);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Stop during a pause ends the pause, and a stopped session stays stopped") {
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }
    attached.session->DispatchProtocolMessage(R"({"id":90,"method":"Debugger.enable"})");

    client.duringPause = [&attached] { attached.session->Stop(); };
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 10") == 10);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.quits == 1);
    client.duringPause = nullptr;

    // Stop again, Resume after Stop: nothing, and no error.
    attached.session->Stop();
    attached.session->Resume();
    CHECK(client.quits == 1);
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 11") == 11);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);

    // Still answered - but the debugger does not come back on.
    attached.session->DispatchProtocolMessage(R"({"id":91,"method":"Runtime.evaluate","params":{"expression":"6*7"}})");
    CHECK(client.ResponseTo(91).find("\"value\":42") != std::string::npos);
    attached.session->DispatchProtocolMessage(R"({"id":92,"method":"Debugger.enable"})");
    const std::string refused = client.ResponseTo(92);
    INFO(refused);
    CHECK(refused.find("\"error\"") != std::string::npos);
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 12") == 12);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: a session destroyed inside a pause ends the pause and says nothing more") {
    ub_test::Fixture fixture;
    Client client;
    Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    // A pause in the embedder's own script.
    attached.session->DispatchProtocolMessage(R"({"id":100,"method":"Debugger.enable"})");
    size_t sentAtClose = 0;
    client.duringPause = [&attached, &client, &sentAtClose] {
        attached.session.reset();
        client.session = nullptr;
        sentAtClose = client.messages.size();
    };
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 13") == 13);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.quits == 1);
    CHECK(client.messages.size() == sentAtClose);

    // A pause inside the session's own dispatch - the engine's session is
    // several frames up the stack when the embedder destroys it.
    auto own = attached.inspector->Connect();
    REQUIRE(own != nullptr);
    client.session = own.get();
    own->DispatchProtocolMessage(R"({"id":101,"method":"Debugger.enable"})");
    client.duringPause = [&own, &client, &sentAtClose] {
        own.reset();
        client.session = nullptr;
        sentAtClose = client.messages.size();
    };
    ub::InspectorSession* raw = own.get();
    raw->DispatchProtocolMessage(R"({"id":102,"method":"Runtime.evaluate","params":{"expression":"debugger; 14"}})");
    CHECK(own == nullptr);
    CHECK(client.pauses == 2);
    CHECK(client.quits == 2);
    CHECK(client.messages.size() == sentAtClose);
    CHECK(client.ResponseTo(102).empty());

    // The inspector is none the worse: a new connection works, and pauses.
    client.duringPause = nullptr;
    auto again = attached.inspector->Connect();
    REQUIRE(again != nullptr);
    client.session = again.get();
    again->DispatchProtocolMessage(R"({"id":103,"method":"Runtime.evaluate","params":{"expression":"5*5"}})");
    CHECK(client.ResponseTo(103).find("\"value\":25") != std::string::npos);
    again->DispatchProtocolMessage(R"({"id":104,"method":"Debugger.enable"})");
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 15") == 15);
    CHECK(client.pauses == 3);

    again.reset();
    attached.inspector->ContextDestroyed(fixture.context);
}

namespace {

/// A native the script calls, which dispatches a message while it is inside -
/// what an embedder's `delay()` does when it services the debugger between
/// slices of sleep.
struct DispatchInside {
    ub::InspectorSession* session = nullptr;
    std::string message;
    int calls = 0;
};

void DispatchInsideNative(const ub::CallbackInfo& info) {
    auto* inside = info.Data<DispatchInside>();
    if (inside->calls++ == 0) {
        inside->session->DispatchProtocolMessage(inside->message);
    }
}

}  // namespace

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Debugger.pause while idle pauses the next script that runs") {
    // DevTools' pause button, pressed while nothing is running: the engine
    // breaks as soon as script next runs.
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":40,"method":"Debugger.enable"})");
    REQUIRE_FALSE(client.ResponseTo(40).empty());
    attached.session->DispatchProtocolMessage(R"({"id":41,"method":"Debugger.pause"})");
    CHECK_FALSE(client.ResponseTo(41).empty());
    client.onPause = {R"({"id":42,"method":"Debugger.resume"})"};

    CHECK(ub_test::EvalInt(fixture.context, "function two() { return 2; } two() + 1") == 3);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.Saw("\"method\":\"Debugger.paused\""));
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Debugger.pause dispatched into a busy script pauses it") {
    // The pause button against a script spinning in JavaScript: the request
    // comes from the socket's thread, and the pause lands in the loop.
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":50,"method":"Debugger.enable"})");
    REQUIRE_FALSE(client.ResponseTo(50).empty());
    client.onPause = {
        R"({"id":51,"method":"Runtime.evaluate","params":{"expression":"globalThis.stopLooping = true"}})",
        R"({"id":52,"method":"Debugger.resume"})"};

    Remote remote;
    remote.session = attached.session.get();
    remote.message = R"({"id":53,"method":"Debugger.pause"})";
    std::atomic<bool> finished{false};
    const std::shared_ptr<ub::InspectorDispatcher> dispatcher = attached.inspector->Dispatcher();
    std::thread requester([dispatcher, &remote] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)dispatcher->RequestDispatch(&DispatchRemote, ub::CallbackData::For(remote));
    });
    ub::Isolate* isolate = &fixture.iso();
    std::thread watchdog([isolate, &finished] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished) {
            isolate->TerminateExecution();
        }
    });

    ub_test::Expose(fixture.context, "stopLooping", ub::False(fixture.iso()));
    const auto result =
        ub::Evaluate(fixture.context, "function tick() {} while (!globalThis.stopLooping) { tick(); } 'finished'");
    finished = true;
    requester.join();
    watchdog.join();
    fixture.iso().CancelTerminateExecution();

    REQUIRE(result.has_value());
    CHECK(ub_test::TextOf(*result) == "finished");
    CHECK(remote.ran);
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.Saw("\"method\":\"Debugger.paused\""));
}

UNIBIND_TEST_CASE(INSPECTOR, "inspector: Debugger.pause dispatched inside a native call pauses when script resumes") {
    // An embedder's delay() services the debugger while the script is inside
    // it: the pause is dispatched with script on the stack but not running,
    // and must land once the native returns and script carries on.
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":60,"method":"Debugger.enable"})");
    REQUIRE_FALSE(client.ResponseTo(60).empty());
    client.onPause = {R"({"id":61,"method":"Debugger.resume"})"};

    DispatchInside inside{.session = attached.session.get(), .message = R"({"id":62,"method":"Debugger.pause"})"};
    const auto native = ub::Function::New(fixture.context, &DispatchInsideNative, ub::CallbackData::For(inside));
    REQUIRE(native.has_value());
    ub_test::Expose(fixture.context, "idle", *native);

    CHECK(ub_test::EvalInt(fixture.context, R"(
        function step(n) { return n + 1; }
        let n = 0;
        for (let i = 0; i < 3; i++) { idle(); n = step(n); }
        n)") == 3);
    CHECK_FALSE(client.ResponseTo(62).empty());
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.Saw("\"method\":\"Debugger.paused\""));
}

namespace {

/// An embedder's delay(): pump jobs and sleep, in slices, with script below it
/// on the stack.
void PumpingDelay(const ub::CallbackInfo& info) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < until) {
        info.GetIsolate().PumpJobs();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

UNIBIND_TEST_CASE(INSPECTOR,
                  "inspector: Debugger.pause requested while script sleeps in a pumping native pauses after it") {
    // The whole path as an embedder has it: the pause arrives on the socket's
    // thread while the script is inside a native that pumps jobs and sleeps,
    // so the dispatch runs from PumpJobs with script suspended beneath it. The
    // script must break once the native returns.
    ub_test::Fixture fixture;
    Client client;
    const Attached attached = Attach(fixture, client);
    if (!attached) {
        return;
    }

    attached.session->DispatchProtocolMessage(R"({"id":70,"method":"Debugger.enable"})");
    REQUIRE_FALSE(client.ResponseTo(70).empty());
    client.onPause = {R"({"id":71,"method":"Debugger.resume"})"};

    const auto delay = ub::Function::New(fixture.context, &PumpingDelay);
    REQUIRE(delay.has_value());
    ub_test::Expose(fixture.context, "delay", *delay);

    Remote remote;
    remote.session = attached.session.get();
    remote.message = R"({"id":72,"method":"Debugger.pause"})";
    const std::shared_ptr<ub::InspectorDispatcher> dispatcher = attached.inspector->Dispatcher();
    std::thread requester([dispatcher, &remote] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)dispatcher->RequestDispatch(&DispatchRemote, ub::CallbackData::For(remote));
    });

    const int result = ub_test::EvalInt(fixture.context, R"(
        function step(n) { return n + 1; }
        let m = 0;
        delay();
        m = step(m);
        m = step(m);
        m)");
    requester.join();

    CHECK(result == 2);
    CHECK(remote.ran);
    CHECK_FALSE(client.ResponseTo(72).empty());
    CHECK(client.pauses == 1);
    CHECK(client.pausesAnnounced == client.pauses);
    CHECK(client.Saw("\"method\":\"Debugger.paused\""));
}
