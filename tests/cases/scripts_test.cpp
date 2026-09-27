/// \file
/// Compiling and running source, with an origin for diagnostics.

#include <string>
#include <utility>

#include "support/harness.h"

TEST_CASE("scripts: compiled source runs more than once") {
    ub_test::Fixture fixture;

    REQUIRE(ub::Evaluate(fixture.context, "globalThis.counter = 0").has_value());

    const auto script = ub::Script::Compile(fixture.context, "++counter");
    REQUIRE(script.has_value());

    CHECK(script->Run(fixture.context)->To<ub::Integer>()->Int32Value() == 1);
    CHECK(script->Run(fixture.context)->To<ub::Integer>()->Int32Value() == 2);
    CHECK(script->Run(fixture.context)->To<ub::Integer>()->Int32Value() == 3);
    CHECK(ub_test::EvalInt(fixture.context, "counter") == 3);
}

TEST_CASE("scripts: compiled source outlives the scope it was compiled in") {
    ub_test::Fixture fixture;

    // A Script roots itself rather than living in a handle scope, which is the
    // whole reason it is not a handle: caching compiled code is the point.
    ub::Script script;
    {
        ub::HandleScope inner(fixture.iso());
        auto compiled = ub::Script::Compile(fixture.context, "'still runnable'");
        REQUIRE(compiled.has_value());
        script = std::move(*compiled);
    }

    fixture.iso().RequestGarbageCollection();

    const auto result = script.Run(fixture.context);
    REQUIRE(result.has_value());
    CHECK(ub_test::TextOf(*result) == "still runnable");
}

TEST_CASE("scripts: a script moves and resets") {
    ub_test::Fixture fixture;

    auto compiled = ub::Script::Compile(fixture.context, "7");
    REQUIRE(compiled.has_value());
    CHECK_FALSE(compiled->IsEmpty());

    ub::Script moved = std::move(*compiled);
    CHECK(compiled->IsEmpty());  // NOLINT(bugprone-use-after-move) - that it is empty is the point
    CHECK(moved.Run(fixture.context)->To<ub::Integer>()->Int32Value() == 7);

    moved.Reset();
    CHECK(moved.IsEmpty());
}

TEST_CASE("scripts: the completion value is the value of the last expression") {
    ub_test::Fixture fixture;

    CHECK(ub_test::EvalInt(fixture.context, "1; 2; 3") == 3);
    CHECK(ub_test::Eval(fixture.context, "var declared = 1;").Kind() == ub::ValueKind::Undefined);
    CHECK(ub_test::EvalText(fixture.context, "'a' + 'b'") == "ab");
}

UNIBIND_TEST_CASE(STACK_TRACE, "scripts: the origin's name shows up in diagnostics") {
    ub_test::Fixture fixture;

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "function named() { throw new Error('x'); } named();",
                             {.resourceName = "origin-test.js", .lineOffset = 10})
                    .has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto stack = tryCatch.StackTrace(fixture.context);
    REQUIRE(stack.has_value());
    CHECK(stack->find("origin-test.js") != std::string::npos);
}

TEST_CASE("scripts: a script sees the globals of the realm it runs in") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    auto third = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    // Compiled once, in the first realm.
    const auto script = ub::Script::Compile(fixture.context, "where");
    REQUIRE(script.has_value());

    REQUIRE(ub::Evaluate(fixture.context, "globalThis.where = 'first'").has_value());
    {
        ub::ContextScope entered(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.where = 'second'").has_value());
    }
    {
        ub::ContextScope entered(*third);
        REQUIRE(ub::Evaluate(*third, "globalThis.where = 'third'").has_value());
    }

    // Not the realm it was compiled in: the realm it is run in, which is what
    // makes "compile once, run in every sandbox" the thing a cached script is
    // for. See docs/status.md decision 10.
    CHECK(ub_test::TextOf(*script->Run(fixture.context)) == "first");
    CHECK(ub_test::TextOf(*script->Run(*second)) == "second");
    CHECK(ub_test::TextOf(*script->Run(*third)) == "third");
    CHECK(ub_test::TextOf(*script->Run(fixture.context)) == "first");
}

TEST_CASE("scripts: a script outlives a realm it ran in, and runs again in the same one") {
    ub_test::Fixture fixture;

    REQUIRE(ub::Evaluate(fixture.context, "globalThis.where = 'first'").has_value());
    const auto script = ub::Script::Compile(fixture.context, "where");
    const auto other = ub::Script::Compile(fixture.context, "where + '!'");
    REQUIRE(script.has_value());
    REQUIRE(other.has_value());

    {
        auto doomed = ub::Context::New(fixture.iso());
        REQUIRE(doomed.has_value());
        {
            ub::ContextScope entered(*doomed);
            REQUIRE(ub::Evaluate(*doomed, "globalThis.where = 'doomed'").has_value());
        }
        // Twice each, so that the second run is the one a backend may answer
        // from whatever it kept of the first; and two scripts, so that the
        // realm has more than one to let go of when it goes.
        CHECK(ub_test::TextOf(*script->Run(*doomed)) == "doomed");
        CHECK(ub_test::TextOf(*script->Run(*doomed)) == "doomed");
        CHECK(ub_test::TextOf(*other->Run(*doomed)) == "doomed!");
        CHECK(ub_test::TextOf(*other->Run(*doomed)) == "doomed!");
    }
    fixture.iso().RequestGarbageCollection();

    CHECK(ub_test::TextOf(*script->Run(fixture.context)) == "first");
    CHECK(ub_test::TextOf(*script->Run(fixture.context)) == "first");
    CHECK(ub_test::TextOf(*other->Run(fixture.context)) == "first!");
    REQUIRE(ub::Evaluate(fixture.context, "globalThis.where = 'changed'").has_value());
    CHECK(ub_test::TextOf(*script->Run(fixture.context)) == "changed");
}

TEST_CASE("scripts: an empty script is not a failure") {
    ub_test::Fixture fixture;

    const auto result = ub::Evaluate(fixture.context, "");
    REQUIRE(result.has_value());
    CHECK(result->Kind() == ub::ValueKind::Undefined);
}

TEST_CASE("scripts: a script that throws reports no value and leaves the isolate usable") {
    ub_test::Fixture fixture;

    const auto script = ub::Script::Compile(fixture.context, "throw new Error('thrown at run time')");
    REQUIRE(script.has_value());

    {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(script->Run(fixture.context).has_value());
        CHECK(tryCatch.HasCaught());
        tryCatch.Reset();
    }

    CHECK(ub_test::EvalInt(fixture.context, "5 * 5") == 25);
}
