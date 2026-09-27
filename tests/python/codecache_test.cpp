// The compiled-code cache: a blob is the marshalled pair of code objects a
// script compiles to, and a compile offered one skips the compiler.

#include <cstring>
#include <thread>

#include "data_support.h"

using py_test::Eval;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Fixture;
using py_test::Run;

namespace {

using Blob = std::vector<std::uint8_t>;

constexpr std::string_view SOURCE =
    "def square(n):\n    return n * n\ntotal = sum(square(i) for i in range(10))\ntotal + 1";

[[nodiscard]] Blob MakeCache(const ub::Context& context, std::string_view source, const ub::ScriptOrigin& origin) {
    auto script = ub::Script::Compile(context, source, origin, ub::CompileOptions::EagerCompile);
    REQUIRE(script.has_value());
    CHECK_FALSE(script->UsedCodeCache());
    auto blob = script->CreateCodeCache();
    REQUIRE(blob.has_value());
    REQUIRE_FALSE(blob->empty());
    return *std::move(blob);
}

/// Compile `source` offering `blob`, run it, and hand back whether the blob was
/// used and the result as text.
struct Cached {
    bool used = false;
    std::string result;
};

[[nodiscard]] Cached RunWithCache(const ub::Context& context, std::string_view source, const Blob& blob,
                                  const ub::ScriptOrigin& origin) {
    ub::Isolate& isolate = context.GetIsolate();
    ub::HandleScope scope(isolate);
    ub::TryCatch tc(isolate);
    auto script = ub::Script::CompileWithCache(context, source, blob, origin);
    REQUIRE(script.has_value());
    CHECK_FALSE(tc.HasCaught());
    Cached out;
    out.used = script->UsedCodeCache();
    auto value = script->Run(context);
    if (!value) {
        FAIL_CHECK("running threw: " << tc.Message(context).value_or("?"));
        return out;
    }
    out.result = py_test::TextOf(context, *value);
    return out;
}

}  // namespace

TEST_CASE("codecache: the build id names the interpreter and its bytecode") {
    Fixture f;
    auto script = ub::Script::Compile(f.context, "1");
    REQUIRE(script.has_value());
    const std::string_view id = ub::detail::BackendBuildId();
    CHECK(id.starts_with("python-3.14."));
    // The version and the bytecode magic number of the interpreter running,
    // which is what a code cache blob is only good for.
    CHECK(std::string(id) == py_test::EvalText(f.context, R"(
import sys, importlib.util
'python-%d.%d.%d-%d' % (*sys.version_info[:3], int.from_bytes(importlib.util.MAGIC_NUMBER, 'little'))
)"));
}

TEST_CASE("codecache: a blob round-trips and is used") {
    Fixture f;
    const ub::ScriptOrigin origin{.resourceName = "cached.py"};
    const Blob blob = MakeCache(f.context, SOURCE, origin);
    const Cached run = RunWithCache(f.context, SOURCE, blob, origin);
    CHECK(run.used);
    CHECK(run.result == "286");
    CHECK(EvalInt(f.context, "square(12)") == 144);

    // A script compiled from the cache makes the same cache again.
    auto again = ub::Script::CompileWithCache(f.context, SOURCE, blob, origin);
    REQUIRE(again.has_value());
    auto second = again->CreateCodeCache();
    REQUIRE(second.has_value());
    CHECK(RunWithCache(f.context, SOURCE, *second, origin).used);
}

TEST_CASE("codecache: a script with no trailing expression, and one that is only an expression") {
    Fixture f;
    const ub::ScriptOrigin origin{.resourceName = "shapes.py"};
    const Blob statements = MakeCache(f.context, "x = 5\ny = x * 2", origin);
    const Cached a = RunWithCache(f.context, "x = 5\ny = x * 2", statements, origin);
    CHECK(a.used);
    CHECK(a.result == "None");
    CHECK(EvalInt(f.context, "y") == 10);

    const Blob expression = MakeCache(f.context, "'a' * 3", origin);
    const Cached b = RunWithCache(f.context, "'a' * 3", expression, origin);
    CHECK(b.used);
    CHECK(b.result == "aaa");
}

TEST_CASE("codecache: no blob, garbage and damage all compile the source instead") {
    Fixture f;
    const ub::ScriptOrigin origin{.resourceName = "damaged.py"};
    const Blob good = MakeCache(f.context, SOURCE, origin);

    const auto fallsBack = [&](const Blob& blob) {
        const Cached run = RunWithCache(f.context, SOURCE, blob, origin);
        return !run.used && run.result == "286";
    };
    CHECK(fallsBack({}));
    CHECK(fallsBack({1, 2, 3, 4}));
    CHECK(fallsBack(Blob(200, 0xAB)));
    CHECK(fallsBack(Blob(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2))));
    CHECK(fallsBack(Blob(good.begin(), good.end() - 1)));
    for (const std::size_t at : {std::size_t{0}, std::size_t{9}, std::size_t{40}, std::size_t{70}, good.size() - 1}) {
        Blob flipped = good;
        flipped[at] ^= 0x10;
        CAPTURE(at);
        CHECK(fallsBack(flipped));
    }
    // The good one still works after all that.
    CHECK(RunWithCache(f.context, SOURCE, good, origin).used);
}

TEST_CASE("codecache: a blob for one source is not used for another") {
    Fixture f;
    const ub::ScriptOrigin origin{.resourceName = "keyed.py"};
    const Blob blob = MakeCache(f.context, "40 + 2", origin);
    const Cached other = RunWithCache(f.context, "40 + 3", blob, origin);
    CHECK_FALSE(other.used);
    CHECK(other.result == "43");
    // Nor for the same source under another name, or at another line.
    CHECK_FALSE(RunWithCache(f.context, "40 + 2", blob, {.resourceName = "elsewhere.py"}).used);
    CHECK_FALSE(RunWithCache(f.context, "40 + 2", blob, {.resourceName = "keyed.py", .lineOffset = 3}).used);
    CHECK(RunWithCache(f.context, "40 + 2", blob, origin).used);
}

TEST_CASE("codecache: a payload re-framed for another source is caught by the backend's own check") {
    // unibind/script.h's frame is what normally refuses a blob made for other
    // source. Forge one that passes it - the payload of a blob for "1 + 1",
    // framed with the key of different source - and the backend's own check
    // must still refuse it: the code in it was compiled under another name.
    Fixture f;
    const ub::ScriptOrigin made{.resourceName = "made.py"};
    const ub::ScriptOrigin offered{.resourceName = "offered.py"};
    const Blob blob = MakeCache(f.context, "1 + 1", made);
    constexpr std::size_t HEADER = sizeof(ub::detail::CodeCacheHeader);
    REQUIRE(blob.size() > HEADER);
    const std::span<const std::uint8_t> payload(blob.data() + HEADER, blob.size() - HEADER);

    const auto reframe = [&](std::string_view source, const ub::ScriptOrigin& origin,
                             std::span<const std::uint8_t> body) {
        ub::detail::CodeCacheHeader header{
            .magic = ub::detail::CODE_CACHE_MAGIC,
            .format = ub::detail::CODE_CACHE_FORMAT,
            .key = ub::detail::CodeCacheKey(source, origin, ub::detail::BackendBuildId()),
            .payloadLength = body.size(),
            .payloadHash = ub::detail::CodeCachePayloadHash(body)};
        Blob out(HEADER);
        std::memcpy(out.data(), &header, HEADER);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };
    // The forgery is framed right: under its own name it is used.
    CHECK(RunWithCache(f.context, "1 + 1", reframe("1 + 1", made, payload), made).used);
    const Cached forged = RunWithCache(f.context, "1 + 1", reframe("1 + 1", offered, payload), offered);
    CHECK_FALSE(forged.used);
    CHECK(forged.result == "2");

    // And a payload damaged under a valid outer frame is refused by the inner one.
    Blob damaged(payload.begin(), payload.end());
    damaged.back() ^= 0x01;
    const Cached broken = RunWithCache(f.context, "1 + 1", reframe("1 + 1", made, damaged), made);
    CHECK_FALSE(broken.used);
    CHECK(broken.result == "2");
    // Garbage under a valid outer frame, likewise.
    const Blob garbage(64, 0x5A);
    CHECK_FALSE(RunWithCache(f.context, "1 + 1", reframe("1 + 1", made, garbage), made).used);
}

TEST_CASE("codecache: a script compiled from a blob still quotes its lines in a traceback") {
    // Made in one isolate, used in a fresh one - whose linecache has never seen
    // the source, so the quoted line has to come from the cached compile.
    const std::string_view source = "x = 1\ndef fail():\n    raise ValueError('boom')\nfail()";
    const ub::ScriptOrigin origin{.resourceName = "trace.py", .lineOffset = 2};
    Blob blob;
    {
        Fixture a;
        blob = MakeCache(a.context, source, origin);
    }
    Fixture b;
    auto script = ub::Script::CompileWithCache(b.context, source, blob, origin);
    REQUIRE(script.has_value());
    CHECK(script->UsedCodeCache());
    ub::TryCatch tc(b.iso());
    CHECK_FALSE(script->Run(b.context).has_value());
    REQUIRE(tc.HasCaught());
    CHECK(tc.Message(b.context).value_or("") == "ValueError: boom");
    const auto location = tc.Location(b.context);
    REQUIRE(location.has_value());
    CHECK(location->scriptName == "trace.py");
    CHECK(location->lineNumber == 5);  // line 3 of the source, two lines in
    CHECK(location->sourceLine.value_or("") == "    raise ValueError('boom')");
    CHECK(tc.StackTrace(b.context).value_or("").find("raise ValueError('boom')") != std::string::npos);
}

TEST_CASE("codecache: a blob made in one isolate is used in another, on another thread") {
    Blob blob;
    const ub::ScriptOrigin origin{.resourceName = "threaded.py"};
    {
        Fixture a;
        blob = MakeCache(a.context, SOURCE, origin);
    }
    bool used = false;
    std::string result;
    std::thread worker([&] {
        auto isolate = ub::Isolate::New();
        if (!isolate) {
            return;
        }
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return;
        }
        ub::ContextScope entered(*context);
        auto script = ub::Script::CompileWithCache(*context, SOURCE, blob, origin);
        if (!script) {
            return;
        }
        used = script->UsedCodeCache();
        if (auto value = script->Run(*context)) {
            if (auto text = value->ToString(*context)) {
                result = text->Utf8Value();
            }
        }
    });
    worker.join();
    CHECK(used);
    CHECK(result == "286");
}

TEST_CASE("codecache: a script with top-level await caches like any other") {
    Fixture f;
    const std::string_view source = "import asyncio\nawait asyncio.sleep(0)\ndone = 42\ndone";
    const ub::ScriptOrigin origin{.resourceName = "async.py"};
    const Blob blob = MakeCache(f.context, source, origin);
    auto script = ub::Script::CompileWithCache(f.context, source, blob, origin);
    REQUIRE(script.has_value());
    CHECK(script->UsedCodeCache());

    // Running one needs the runtime area's promises.
    if (!ub::Promise::New(f.context).has_value()) {
        MESSAGE("top-level await needs the runtime area's promises; only compilation was checked");
        return;
    }
    auto result = script->Run(f.context);
    REQUIRE(result.has_value());
    CHECK(result->Is<ub::Promise>());
    f.iso().PumpJobs();
    CHECK(EvalInt(f.context, "done") == 42);
}

TEST_CASE("codecache: a script outlives the realm it was compiled in") {
    Fixture f;
    const ub::ScriptOrigin origin{.resourceName = "outlive.py"};
    std::optional<ub::Script> script;
    Blob blob;
    {
        auto other = ub::Context::New(f.iso());
        REQUIRE(other.has_value());
        blob = MakeCache(*other, "21 * 2", origin);
        script = ub::Script::CompileWithCache(*other, "21 * 2", blob, origin);
        REQUIRE(script.has_value());
    }
    Run(f.context, "import gc\ngc.collect()");
    // Still runs - in a realm that is alive - and still makes a cache.
    auto value = script->Run(f.context);
    REQUIRE(value.has_value());
    CHECK(py_test::TextOf(f.context, *value) == "42");
    auto again = script->CreateCodeCache();
    REQUIRE(again.has_value());
    CHECK(RunWithCache(f.context, "21 * 2", *again, origin).used);
    script.reset();
}

TEST_CASE("codecache: a syntax error with a blob for other source is still a syntax error") {
    Fixture f;
    const ub::ScriptOrigin origin{.resourceName = "syntax.py"};
    const Blob blob = MakeCache(f.context, "1", origin);
    ub::TryCatch tc(f.iso());
    CHECK_FALSE(ub::Script::CompileWithCache(f.context, "def (", blob, origin).has_value());
    REQUIRE(tc.HasCaught());
    CHECK(tc.Message(f.context).value_or("").starts_with("SyntaxError"));
}
