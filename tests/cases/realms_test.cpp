/// \file
/// Several realms in one isolate, and running code in a chosen one.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "support/harness.h"

TEST_CASE("realms: two contexts in one isolate have separate globals") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    CHECK(&second->GetIsolate() == &fixture.iso());
    CHECK_FALSE(second->GlobalObject().StrictEquals(fixture.context.GlobalObject()));

    {
        ub::ContextScope entered(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.onlyHere = 5").has_value());
        CHECK(ub_test::EvalInt(*second, "onlyHere") == 5);
    }

    CHECK(ub_test::EvalText(fixture.context, "typeof onlyHere") == "undefined");
}

TEST_CASE("realms: a value made in one realm is a value in the other") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    ub::Local<ub::Object> fromSecond;
    {
        ub::ContextScope entered(*second);
        auto object = ub::Object::New(*second);
        REQUIRE(object.has_value());
        REQUIRE(object->Set(*second, "made", ub_test::Str(fixture.iso(), "over there")).value_or(false));
        fromSecond = *object;
    }

    // Read it from the first realm; a handle belongs to an isolate, not a realm.
    // That a value *works* in another realm is the guarantee, and it is what
    // makes a sandbox useful at all.
    const auto made = fromSecond.Get(fixture.context, "made");
    REQUIRE(made.has_value());
    CHECK(ub_test::TextOf(*made) == "over there");

    ub_test::Expose(fixture.context, "visitor", fromSecond);
    CHECK(ub_test::EvalText(fixture.context, "visitor.made") == "over there");

    // What is deliberately NOT asserted here, and must not be added later: that
    // the object seen from this realm is the *same object* as the one made in
    // the other. An engine that wraps across realm boundaries hands out a
    // distinct wrapper, so a StrictEquals across realms is a question with no
    // portable answer. Compare within one realm, or compare something the
    // values carry - as the property read above does.
}

TEST_CASE("realms: each realm has its own built-ins, so instanceof does not cross") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    ub::Local<ub::Value> arrayFromSecond;
    {
        ub::ContextScope entered(*second);
        arrayFromSecond = ub_test::Eval(*second, "[1, 2, 3]");
    }

    ub_test::Expose(fixture.context, "visitor", arrayFromSecond);

    // Not a quirk of this API - it is what every engine does, and the reason a
    // cross-realm check has to ask what a value *is* rather than what it
    // inherits from.
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "visitor instanceof Array"));
    CHECK(ub_test::EvalTruth(fixture.context, "Array.isArray(visitor)"));
    CHECK(arrayFromSecond.Is<ub::Array>());
}

TEST_CASE("realms: a native function made in one realm is callable from the other") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    const auto function = ub_test::Eval(fixture.context, "(function () { return 'from the first realm'; })");
    {
        ub::ContextScope entered(*second);
        REQUIRE(second->GlobalObject().Set(*second, "borrowed", function).value_or(false));
        CHECK(ub_test::EvalText(*second, "borrowed()") == "from the first realm");
    }
}

TEST_CASE("realms: a context is reference counted, so a copy keeps the realm alive") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    ub::Context copy = *second;
    CHECK(copy);
    CHECK(copy.rec() == second->rec());

    second->Reset();
    CHECK(second->IsEmpty());

    // The copy still names a working realm.
    ub::ContextScope entered(copy);
    CHECK(ub_test::EvalInt(copy, "6 * 7") == 42);
    CHECK(&copy.GetIsolate() == &fixture.iso());
}

TEST_CASE("realms: a context moves without disturbing the realm") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    {
        ub::ContextScope entered(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.marker = 'kept'").has_value());
    }

    ub::Context moved = std::move(*second);
    CHECK(second->IsEmpty());  // NOLINT(bugprone-use-after-move) - that it is empty is the point

    ub::ContextScope entered(moved);
    CHECK(ub_test::EvalText(moved, "marker") == "kept");
}

TEST_CASE("realms: entered realms nest and unwind") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    auto third = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    REQUIRE(ub::Evaluate(fixture.context, "globalThis.name_ = 'one'").has_value());
    {
        ub::ContextScope enteredSecond(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.name_ = 'two'").has_value());
        {
            ub::ContextScope enteredThird(*third);
            REQUIRE(ub::Evaluate(*third, "globalThis.name_ = 'three'").has_value());
            CHECK(ub_test::EvalText(*third, "name_") == "three");
        }
        CHECK(ub_test::EvalText(*second, "name_") == "two");
    }
    CHECK(ub_test::EvalText(fixture.context, "name_") == "one");
}

TEST_CASE("realms: many realms can live at once") {
    ub_test::Fixture fixture;

    constexpr int COUNT = 16;
    std::vector<ub::Context> realms;
    realms.reserve(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        auto made = ub::Context::New(fixture.iso());
        REQUIRE(made.has_value());
        ub::ContextScope entered(*made);
        REQUIRE(made->GlobalObject().Set(*made, "index", ub::Integer::New(fixture.iso(), i)).value_or(false));
        realms.push_back(std::move(*made));
    }

    fixture.iso().RequestGarbageCollection();

    for (int i = 0; i < COUNT; ++i) {
        ub::ContextScope entered(realms[static_cast<std::size_t>(i)]);
        CHECK(ub_test::EvalInt(realms[static_cast<std::size_t>(i)], "index") == i);
    }
}

namespace {

/// What the callbacks below were called with, and what they saw.
struct RealmProbe {
    ub::Context* enterFirst = nullptr;
    ub::Context* scriptRealm = nullptr;
    ub::Context* caller = nullptr;
    ub::Global<ub::Function>* spin = nullptr;
    ub::detail::ContextRec* seen = nullptr;
    ub::detail::ContextRec* seenInside = nullptr;
    bool interrupted = false;
};

/// Enters another realm, and only then asks which one it was called in.
void AskAfterEntering(const ub::CallbackInfo& info) {
    auto* probe = info.Data<RealmProbe>();
    ub::ContextScope entered(*probe->enterFirst);
    probe->seen = info.GetContext().rec();
}

void RecordRealm(const ub::CallbackInfo& info) {
    info.Data<RealmProbe>()->seenInside = info.GetContext().rec();
}

/// Runs script in another realm - which calls back into native code - and
/// only then asks which realm it was called in.
void AskAfterScript(const ub::CallbackInfo& info) {
    auto* probe = info.Data<RealmProbe>();
    {
        ub::ContextScope entered(*probe->scriptRealm);
        CHECK(ub::Evaluate(*probe->scriptRealm, "record(); for (let i = 0; i < 100000; ++i) {} 1").has_value());
    }
    probe->seen = info.GetContext().rec();
}

void EnterDuringInterrupt(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* probe = data.As<RealmProbe>();
    ub::ContextScope entered(*probe->enterFirst);
    probe->interrupted = true;
}

/// Asks for an interrupt that enters a realm, then calls a function of another
/// realm - which V8 runs in that one, with nothing entered on the way - until
/// the interrupt has fired, and only then asks which realm it was called in.
void AskAfterInterrupt(const ub::CallbackInfo& info) {
    auto* probe = info.Data<RealmProbe>();
    ub::Isolate& isolate = info.GetIsolate();
    CHECK(isolate.RequestInterrupt(&EnterDuringInterrupt, ub::CallbackData::For(*probe)));
    CHECK(probe->spin->Get(isolate).Call(*probe->caller, ub::Undefined(isolate)).has_value());
    probe->seen = info.GetContext().rec();
}

}  // namespace

TEST_CASE("realms: a callback is told the realm it was called in, whatever it entered since") {
    // A backend may find the realm only when a callback asks for it. It still
    // has to answer with the realm the call began in: not one the callback
    // entered before asking, not the one some script it called ran in, and
    // not one an interrupt entered in the middle of that script.
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    auto third = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    RealmProbe probe;
    ub::Global<ub::Function> spin;
    probe.enterFirst = &*second;
    probe.scriptRealm = &*third;
    probe.caller = &fixture.context;
    probe.spin = &spin;
    {
        ub::ContextScope entered(*third);
        const auto record = ub::Function::New(*third, &RecordRealm, ub::CallbackData::For(probe));
        REQUIRE(record.has_value());
        ub_test::Expose(*third, "record", *record);
        const auto made = ub::Evaluate(
            *third, "(function () { let s = 0; for (let i = 0; i < 1000000; ++i) { s += i; } return s; })");
        REQUIRE(made.has_value());
        const auto asFunction = made->To<ub::Function>();
        REQUIRE(asFunction.has_value());
        spin = ub::Global<ub::Function>(fixture.iso(), *asFunction);
    }

    const auto afterEntering = ub::Function::New(fixture.context, &AskAfterEntering, ub::CallbackData::For(probe));
    const auto afterScript = ub::Function::New(fixture.context, &AskAfterScript, ub::CallbackData::For(probe));
    const auto afterInterrupt = ub::Function::New(fixture.context, &AskAfterInterrupt, ub::CallbackData::For(probe));
    REQUIRE(afterEntering.has_value());
    REQUIRE(afterScript.has_value());
    REQUIRE(afterInterrupt.has_value());
    ub_test::Expose(fixture.context, "afterEntering", *afterEntering);
    ub_test::Expose(fixture.context, "afterScript", *afterScript);
    ub_test::Expose(fixture.context, "afterInterrupt", *afterInterrupt);

    REQUIRE(ub::Evaluate(fixture.context, "afterEntering()").has_value());
    CHECK(probe.seen == fixture.context.rec());

    probe.seen = nullptr;
    REQUIRE(ub::Evaluate(fixture.context, "afterScript()").has_value());
    CHECK(probe.seenInside == third->rec());
    CHECK(probe.seen == fixture.context.rec());

    probe.seen = nullptr;
    REQUIRE(ub::Evaluate(fixture.context, "afterInterrupt()").has_value());
    CHECK(probe.interrupted);
    CHECK(probe.seen == fixture.context.rec());
}
