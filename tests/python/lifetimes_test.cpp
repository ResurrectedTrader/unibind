// Lifetimes: who owns what, for how long, and what happens at every edge of it.
//
// The CPython backend's model is simple to state - a handle is a strong
// reference its frame drops, a `Global` is one more, a realm is a globals dict,
// a native goes with the last reference to its instance - and every case here
// is one of the ways that statement could be quietly false: a reference taken
// twice or dropped twice, a record that outlives what it points at, a teardown
// step that runs in the wrong order. Most cases count something exactly -
// `sys.getrefcount`, a weak reference's callback, a native's destructor, the
// C++ heap's outstanding allocations - because a total that comes out right is
// what a double release plus a leak looks like.
//
// Isolates, threads and teardown are in teardown_test.cpp.

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lifetimes_support.h"

using py_lifetimes::FailAllocations;
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

// ===========================================================================
// Handles and scopes
// ===========================================================================

TEST_CASE("lifetimes: every handle a scope made is one reference, and closing the scope gives all of them back") {
    Fixture f;
    Run(f.context, "class Thing: pass\nthing = Thing()");
    const std::int32_t base = RefCount(f.context, "thing");
    {
        const ub::HandleScope scope(f.iso());
        auto global = f.context.GlobalObject();
        for (int i = 0; i < 1000; ++i) {
            REQUIRE(global.Get(f.context, "thing").has_value());
        }
        CHECK(RefCount(f.context, "thing") == base + 1000);
        {
            const ub::HandleScope inner(f.iso());
            for (int i = 0; i < 20; ++i) {
                REQUIRE(global.Get(f.context, "thing").has_value());
            }
            CHECK(RefCount(f.context, "thing") == base + 1020);
        }
        CHECK(RefCount(f.context, "thing") == base + 1000);
    }
    CHECK(RefCount(f.context, "thing") == base);
}

TEST_CASE("lifetimes: ten thousand handles in one frame, past the inline slots, each still its own value") {
    Fixture f;
    const long long before = ub_test::OutstandingAllocations();
    {
        const ub::HandleScope scope(f.iso());
        std::vector<ub::Local<ub::Value>> handles;
        handles.reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            auto text = ub::String::NewFromUtf8(f.iso(), "v" + std::to_string(i));
            REQUIRE(text.has_value());
            handles.emplace_back(*text);
        }
        // Read back after every spill growth: a spill that moved must not have
        // lost or reordered anything.
        for (int i = 0; i < 10000; i += 7) {
            CHECK(handles[static_cast<std::size_t>(i)].To<ub::String>()->Utf8Value() == "v" + std::to_string(i));
        }
    }
    // The spill buffer went with the frame (the vector above is gone too).
    CHECK(ub_test::OutstandingAllocations() == before);
}

TEST_CASE("lifetimes: an escaped handle outlives every frame it passed through") {
    Fixture f;
    Run(f.context, "import weakref\nclass Thing: pass\nw = None");
    ub::Local<ub::Value> survivor;
    {
        ub::EscapableHandleScope outer(f.iso());
        ub::Local<ub::Value> carried;
        {
            ub::EscapableHandleScope middle(f.iso());
            ub::Local<ub::Value> made;
            {
                ub::EscapableHandleScope inner(f.iso());
                made = inner.Escape(Eval(f.context, "t = Thing()\nw = weakref.ref(t)\nt"));
                Run(f.context, "del t");
            }
            carried = middle.Escape(made);
        }
        survivor = outer.Escape(carried);
    }
    Run(f.context, "gc.collect()");
    CHECK(EvalTruth(f.context, "w() is not None"));
    CHECK(survivor.StrictEquals(Eval(f.context, "w()")));
}

TEST_CASE("lifetimes: a handle is the only thing keeping a value alive once script lets it go") {
    Fixture f;
    ub::Local<ub::Object> held;
    {
        const auto made = Eval(f.context, R"(
died = []
class Thing:
    def __init__(self):
        self.payload = [1, 2, 3]
t = Thing()
w = weakref.ref(t, lambda _: died.append(True))
t
)");
        held = *made.To<ub::Object>();
    }
    Run(f.context, "del t\ngc.collect()\ngc.collect()");
    CHECK(EvalTruth(f.context, "w() is not None and not died"));
    // Still a working object, not a husk.
    CHECK(py_test::TextOf(f.context, *held.Get(f.context, "payload")) == "[1, 2, 3]");
    CHECK(held.Set(f.context, "extra", ub::Integer::New(f.iso(), 7)).value_or(false));
    CHECK(EvalInt(f.context, "w().extra") == 7);
}

TEST_CASE("lifetimes: when a frame cannot grow, the handle is empty, MemoryError is pending, nothing leaks") {
    Fixture f;
    Run(f.context, "class Thing: pass\nthing = Thing()\nw = weakref.ref(thing)");
    const std::int32_t base = RefCount(f.context, "thing");
    // Walk the failure across the first spill growth: whichever handle the
    // failure lands on is empty, reports MemoryError, and leaks no reference.
    bool sawEmpty = false;
    for (int filled = 0; filled < 40 && !sawEmpty; ++filled) {
        const ub::HandleScope scope(f.iso());
        for (int i = 0; i < filled; ++i) {
            (void)ub::Integer::New(f.iso(), i);
        }
        const auto global = f.context.GlobalObject();
        ub::TryCatch handler(f.iso());
        std::optional<ub::Local<ub::Value>> got;
        long long fired = 0;
        {
            FailAllocations failure(1);
            got = global.Get(f.context, "thing");
            fired = failure.Stop();
        }
        if (fired == 0) {
            continue;  // this handle fitted where it was; try one more filler
        }
        sawEmpty = true;
        CHECK_FALSE(got.has_value());
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(f.context).value_or("").starts_with("MemoryError"));
    }
    REQUIRE(sawEmpty);
    CHECK(RefCount(f.context, "thing") == base);
    Run(f.context, "del thing\ngc.collect()");
    CHECK(EvalTruth(f.context, "w() is None"));
    // And the isolate carries on.
    CHECK(EvalInt(f.context, "40 + 2") == 42);
}

TEST_CASE("lifetimes: an escape the parent frame cannot hold is empty, and drops the value with the inner frame") {
    Fixture f;
    Run(f.context, "class Thing: pass\nthing = Thing()\nw = weakref.ref(thing)");
    const std::int32_t base = RefCount(f.context, "thing");
    bool sawEmpty = false;
    for (int filled = 0; filled < 40 && !sawEmpty; ++filled) {
        const ub::HandleScope parent(f.iso());
        for (int i = 0; i < filled; ++i) {
            (void)ub::Integer::New(f.iso(), i);
        }
        ub::TryCatch handler(f.iso());
        ub::Local<ub::Value> escaped;
        long long fired = 0;
        {
            ub::EscapableHandleScope inner(f.iso());
            const auto value = f.context.GlobalObject().Get(f.context, "thing");
            REQUIRE(value.has_value());
            FailAllocations failure(1);
            escaped = inner.Escape(*value);
            fired = failure.Stop();
        }
        if (fired == 0) {
            continue;
        }
        sawEmpty = true;
        CHECK(escaped.IsEmpty());
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(f.context).value_or("").starts_with("MemoryError"));
    }
    REQUIRE(sawEmpty);
    CHECK(RefCount(f.context, "thing") == base);
}

// ===========================================================================
// Globals
// ===========================================================================

TEST_CASE("lifetimes: a Global outlives its scope, survives a move, and Duplicate is one more reference") {
    Fixture f;
    Run(f.context, "class Thing: pass\nthing = Thing()");
    const std::int32_t base = RefCount(f.context, "thing");
    ub::Global<ub::Value> first;
    {
        const ub::HandleScope scope(f.iso());
        first = ub::Global<ub::Value>(f.iso(), *f.context.GlobalObject().Get(f.context, "thing"));
    }
    CHECK(RefCount(f.context, "thing") == base + 1);
    ub::Global<ub::Value> second = first.Duplicate();
    CHECK(RefCount(f.context, "thing") == base + 2);
    ub::Global<ub::Value> moved = std::move(first);
    CHECK(first.IsEmpty());  // NOLINT(bugprone-use-after-move): the moved-from state is specified
    CHECK(RefCount(f.context, "thing") == base + 2);
    moved = std::move(second);  // drops what `moved` held, takes second's
    CHECK(RefCount(f.context, "thing") == base + 1);
    auto& same = moved;       // through a reference, so that it is the self-move it looks like at run time
    moved = std::move(same);  // a no-op
    CHECK(RefCount(f.context, "thing") == base + 1);
    {
        const ub::HandleScope scope(f.iso());
        CHECK(moved.Get(f.iso()).StrictEquals(Eval(f.context, "thing")));
    }
    moved.Reset();
    moved.Reset();  // twice is fine
    CHECK(RefCount(f.context, "thing") == base);
}

TEST_CASE("lifetimes: comparing and duplicating Globals needs no HandleScope at all") {
    auto isolate = ub::Isolate::New();
    REQUIRE(isolate != nullptr);
    ub::Global<ub::Value> a;
    ub::Global<ub::Value> b;
    ub::Global<ub::Value> other;
    ub::Global<ub::Value> nan;
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);
        a = ub::Global<ub::Value>(*isolate, Eval(*context, "x = object()\nx"));
        b = ub::Global<ub::Value>(*isolate, Eval(*context, "x"));
        other = ub::Global<ub::Value>(*isolate, Eval(*context, "object()"));
        nan = ub::Global<ub::Value>(*isolate, Eval(*context, "float('nan')"));
    }
    // No scope is open from here on; a checked build asserts on any handle
    // made now, so these touching a frame would fail the case.
    CHECK(a.StrictEquals(b));
    CHECK(a.SameValue(b));
    CHECK_FALSE(a.StrictEquals(other));
    CHECK_FALSE(nan.StrictEquals(nan));
    CHECK(nan.SameValue(nan));
    ub::Global<ub::Value> copy = a.Duplicate();
    CHECK(copy.StrictEquals(a));
    ub::Global<ub::Value> empty;
    CHECK_FALSE(empty.StrictEquals(empty));
    CHECK(empty.Duplicate().IsEmpty());
    copy.Reset();
    a.Reset();
    b.Reset();
    other.Reset();
    nan.Reset();
}

TEST_CASE("lifetimes: a Global holding the only reference keeps the object, and Reset frees it at once") {
    Fixture f;
    Run(f.context, R"(
freed = []
class Thing: pass
t = Thing()
w = weakref.ref(t, lambda _: freed.append(True))
)");
    ub::Global<ub::Object> root;
    {
        const ub::HandleScope scope(f.iso());
        root = ub::Global<ub::Object>(f.iso(), *Eval(f.context, "t").To<ub::Object>());
    }
    Run(f.context, "del t\ngc.collect()");
    CHECK(EvalTruth(f.context, "w() is not None and not freed"));
    root.Reset();
    // Reference counting: no collection needed.
    CHECK(EvalTruth(f.context, "w() is None and freed == [True]"));
}

TEST_CASE("lifetimes: a Global the allocator refuses is empty, and the value keeps its count") {
    Fixture f;
    Run(f.context, "class Thing: pass\nthing = Thing()");
    const std::int32_t base = RefCount(f.context, "thing");
    const auto value = Eval(f.context, "thing");
    {
        long long fired = 0;
        std::optional<ub::Global<ub::Value>> root;
        {
            FailAllocations failure(1);
            root.emplace(f.iso(), value);
            fired = failure.Stop();
        }
        REQUIRE(fired == 1);
        CHECK(root->IsEmpty());
    }
    // `value` itself holds one reference while this scope is open.
    CHECK(RefCount(f.context, "thing") == base + 1);
    ub::Global<ub::Value> good(f.iso(), value);
    {
        long long fired = 0;
        std::optional<ub::Global<ub::Value>> copy;
        {
            FailAllocations failure(1);
            copy.emplace(good.Duplicate());
            fired = failure.Stop();
        }
        REQUIRE(fired == 1);
        CHECK(copy->IsEmpty());
    }
    CHECK(RefCount(f.context, "thing") == base + 2);
    good.Reset();
    CHECK(RefCount(f.context, "thing") == base + 1);
}

// ===========================================================================
// Contexts
// ===========================================================================

namespace {

/// Answers the `marker` global of the realm the call arrived in.
void ReadsMarker(const ub::CallbackInfo& info) {
    const auto marker = info.GetContext().GlobalObject().Get(info.GetContext(), "marker");
    if (marker) {
        info.GetReturnValue().Set(*marker);
    }
}

}  // namespace

TEST_CASE("lifetimes: script that empties its own globals does not take the realm's record with it") {
    // The realm's dictionary is script's to do with as it likes - `clear()`,
    // `del`, overwrite every key. None of that may end the realm while a
    // Context still names it.
    for (const char* wipe : {"globals().clear()", "for k in list(globals()): del globals()[k]",
                             "g = globals()\nfor k in [k for k in g if k != '__builtins__']:\n    g[k] = None",
                             "globals().update({k: 0 for k in globals() if k != '__builtins__'})"}) {
        CAPTURE(wipe);
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        {
            const ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            REQUIRE(context.has_value());
            {
                const ub::ContextScope entered(*context);
                Expose(*context, "readsMarker", Native(*context, &ReadsMarker));
                Run(*context, "marker = 'here'\ndef later():\n    return readsMarker()");
                const ub::Global<ub::Value> later(*isolate, Eval(*context, "later"));
                (void)ub::Evaluate(*context, wipe);
                Run(*context, "marker = 'still here'");
                Expose(*context, "readsMarker", Native(*context, &ReadsMarker));
                // The realm is whole: the native finds it from a function the
                // script defined before the wipe, and from a fresh script.
                const auto again = later.Get(*isolate).To<ub::Function>();
                REQUIRE(again.has_value());
                const auto seen = again->Call(*context, ub::Undefined(*isolate));
                REQUIRE(seen.has_value());
                CHECK(py_test::TextOf(*context, *seen) == "still here");
                Expose(*context, "readsMarker", Native(*context, &ReadsMarker));
                CHECK(EvalText(*context, "readsMarker()") == "still here");
                // A copy the scope still holds, and one more round of wiping.
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): the copy is the case
                ub::Context copy = *context;
                (void)ub::Evaluate(copy, wipe);
                CHECK(EvalInt(copy, "1 + 1") == 2);
            }
        }
    }
}

TEST_CASE("lifetimes: a copy of a realm's globals that outlives the realm confuses no later realm") {
    Fixture f;
    Expose(f.context, "readsMarker", Native(f.context, &ReadsMarker));
    Run(f.context, "import builtins\nbuiltins.kept = []\nmarker = 'first'");
    // A realm that dies while a copy of its globals - `dict(globals())`,
    // `globals().copy()` - lives on somewhere script can reach, and then a new
    // realm, whose dictionary the allocator is likely to put where the dead
    // one was. Then the copies go too. Through all of it, a function the new
    // realm defined must find the new realm when it calls a native - from
    // outside that realm, where only its globals can say which realm it is.
    int wrong = 0;
    for (int round = 0; round < 50; ++round) {
        {
            auto dying = ub::Context::New(f.iso());
            REQUIRE(dying.has_value());
            Run(*dying,
                "import builtins\nbuiltins.kept.append(dict(globals()))\nbuiltins.kept.append(globals().copy())");
        }
        Run(f.context, "gc.collect()");
        auto fresh = ub::Context::New(f.iso());
        REQUIRE(fresh.has_value());
        const std::string marker = "fresh " + std::to_string(round);
        Expose(*fresh, "readsMarker", Native(*fresh, &ReadsMarker));
        Run(*fresh, "marker = '" + marker + "'\ndef probe():\n    return readsMarker()");
        const ub::HandleScope scope(f.iso());
        const auto probe = fresh->GlobalObject().Get(*fresh, "probe")->To<ub::Function>();
        REQUIRE(probe.has_value());
        Run(f.context, "builtins.kept.clear()\ngc.collect()");
        // Called from the first realm, which is the one entered.
        const auto seen = probe->Call(f.context, ub::Undefined(f.iso()));
        REQUIRE(seen.has_value());
        if (py_test::TextOf(f.context, *seen) != marker) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
    CHECK(EvalText(f.context, "readsMarker()") == "first");
}

TEST_CASE("lifetimes: a callback that keeps its Context brings a released realm back, whole") {
    static std::optional<ub::Context> kept;
    kept.reset();
    Fixture f;
    ub::Global<ub::Value> function;
    {
        auto other = ub::Context::New(f.iso());
        REQUIRE(other.has_value());
        const ub::ContextScope entered(*other);
        Expose(*other, "keep", Native(*other, [](const ub::CallbackInfo& info) { kept = info.GetContext(); }));
        Run(*other, "marker = 'other'\ndef callsKeep():\n    keep()\n    return marker");
        function = ub::Global<ub::Value>(f.iso(), Eval(*other, "callsKeep"));
    }
    // The embedder has no Context for the other realm now; only the function
    // script wrote there keeps its globals alive. Calling it hands the callback
    // that realm, and the callback keeps it.
    Run(f.context, "gc.collect()");
    const auto result = function.Get(f.iso()).To<ub::Function>()->Call(f.context, ub::Undefined(f.iso()));
    REQUIRE(result.has_value());
    CHECK(py_test::TextOf(f.context, *result) == "other");
    function.Reset();
    Run(f.context, "gc.collect()");
    REQUIRE(kept.has_value());
    {
        const ub::ContextScope entered(*kept);
        CHECK(EvalText(*kept, "marker") == "other");
        CHECK(EvalText(*kept, "callsKeep()") == "other");
    }
    kept.reset();
    Run(f.context, "gc.collect()");
}

TEST_CASE("lifetimes: Context copies, moves and assignments count their references exactly") {
    Fixture f;
    const long long before = ub_test::OutstandingAllocations();
    {
        auto made = ub::Context::New(f.iso());
        REQUIRE(made.has_value());
        ub::Context a = *made;
        made.reset();
        ub::Context b = a;             // copy
        ub::Context c = std::move(b);  // move
        CHECK(b.IsEmpty());            // NOLINT(bugprone-use-after-move)
        ub::Context d;
        d = c;  // copy-assign into empty
        const ub::Context& self = d;
        d = self;  // self-assign
        d = a;     // same realm, other object
        ub::Context e = *ub::Context::New(f.iso());
        e = a;             // drops e's own realm, which goes now
        a = std::move(c);  // move-assign over the same realm
        CHECK(EvalInt(d, "7 * 6") == 42);
        Run(e, "shared = 'yes'");
        CHECK(EvalText(a, "shared") == "yes");
        c.Reset();  // NOLINT(bugprone-use-after-move): resetting a moved-from one is specified
        c.Reset();
    }
    Run(f.context, "gc.collect()");
    CHECK(ub_test::OutstandingAllocations() == before);
}

TEST_CASE("lifetimes: ContextScopes nest across realms, and each native sees the realm it was called in") {
    Fixture f;
    auto second = ub::Context::New(f.iso());
    auto third = ub::Context::New(f.iso());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    Run(f.context, "marker = 'first'");
    Run(*second, "marker = 'second'");
    Run(*third, "marker = 'third'");
    const auto reader = Native(f.context, &ReadsMarker);
    Expose(f.context, "readsMarker", reader);
    Expose(*second, "readsMarker", reader);
    Expose(*third, "readsMarker", reader);
    {
        const ub::ContextScope two(*second);
        {
            const ub::ContextScope three(*third);
            CHECK(EvalText(*third, "readsMarker()") == "third");
            CHECK(EvalText(*second, "readsMarker()") == "second");
            // A native called straight from C++ is in the innermost entered
            // realm, or the one the call was given.
            const auto direct = reader.Call(*third, ub::Undefined(f.iso()));
            CHECK(py_test::TextOf(*third, *direct) == "third");
            // The realms can go while entered: the scope holds its own.
            third.reset();
            CHECK(py_test::TextOf(*second, *reader.Call(*second, ub::Undefined(f.iso()))) == "second");
        }
        second.reset();
        CHECK(EvalText(f.context, "readsMarker()") == "first");
    }
    CHECK(EvalText(f.context, "readsMarker()") == "first");
}

TEST_CASE("lifetimes: ten thousand realms made and dropped leave the C++ heap where it was") {
    Fixture f;
    // Warm up: first-use caches and the like.
    for (int i = 0; i < 100; ++i) {
        auto context = ub::Context::New(f.iso());
        Run(*context, "x = 1");
    }
    Run(f.context, "gc.collect()");
    const long long before = ub_test::OutstandingAllocations();
    const auto heapBefore = f.iso().GetHeapStatistics().usedBytes;
    for (int i = 0; i < 10000; ++i) {
        const ub::HandleScope scope(f.iso());
        auto context = ub::Context::New(f.iso());
        REQUIRE(context.has_value());
        if (i % 3 == 0) {
            Run(*context, "def f():\n    return f\nloop = f");  // a cycle through the globals
        }
    }
    Run(f.context, "gc.collect()");
    CHECK(ub_test::OutstandingAllocations() - before <= 16);
    const auto heapAfter = f.iso().GetHeapStatistics().usedBytes;
    CAPTURE(heapBefore);
    CAPTURE(heapAfter);
    CHECK(heapAfter < heapBefore + (std::uint64_t{2} * 1024 * 1024));
}

namespace {
void NothingAtAll(const ub::CallbackInfo& /*info*/) {}
}  // namespace

TEST_CASE("lifetimes: what a template made in a realm goes with the realm") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso(), &NothingAtAll);
    tpl.SetClassName("Made");
    tpl.PrototypeTemplate().Set("method", &NothingAtAll);
    Run(f.context, "watch = []\ngone = []");
    for (int i = 0; i < 20; ++i) {
        auto realm = ub::Context::New(f.iso());
        REQUIRE(realm.has_value());
        const ub::HandleScope scope(f.iso());
        const auto type = tpl.GetFunction(*realm);
        REQUIRE(type.has_value());
        Expose(f.context, "made", *type);
        Expose(*realm, "Made", *type);
        Run(*realm, "instance = Made()");
        Run(f.context, "watch.append(weakref.ref(made, lambda _: gone.append(1)))\ndel made");
    }
    Run(f.context, "gc.collect()");
    CHECK(EvalInt(f.context, "len(gone)") == 20);
    // The same template still instantiates, into a new realm, afresh.
    auto realm = ub::Context::New(f.iso());
    CHECK(tpl.GetFunction(*realm).has_value());
}

// ===========================================================================
// Scripts
// ===========================================================================

TEST_CASE("lifetimes: a Script released while what it defined lives on, and one run in a realm it was not made in") {
    Fixture f;
    ub::Global<ub::Value> defined;
    {
        auto home = ub::Context::New(f.iso());
        REQUIRE(home.has_value());
        auto script = ub::Script::Compile(*home, "def made():\n    return [n * 2 for n in range(3)]\nmade");
        REQUIRE(script.has_value());
        const auto first = script->Run(*home);
        REQUIRE(first.has_value());
        defined = ub::Global<ub::Value>(f.iso(), *first);
        // The script's realm goes before the script does; the script runs
        // elsewhere, and then goes too.
        home.reset();
        Run(f.context, "gc.collect()");
        const auto elsewhere = script->Run(f.context);
        REQUIRE(elsewhere.has_value());
        script.reset();
    }
    Run(f.context, "gc.collect()");
    const auto call = defined.Get(f.iso()).To<ub::Function>()->Call(f.context, ub::Undefined(f.iso()));
    REQUIRE(call.has_value());
    CHECK(py_test::TextOf(f.context, *call) == "[0, 2, 4]");
    CHECK(EvalText(f.context, "str(made())") == "[0, 2, 4]");
    defined.Reset();
}

TEST_CASE("lifetimes: many scripts compiled, run and dropped leave the C++ heap where it was") {
    Fixture f;
    for (int i = 0; i < 50; ++i) {
        (void)ub::Script::Compile(f.context, "x = 1\nx");
    }
    const long long before = ub_test::OutstandingAllocations();
    for (int i = 0; i < 5000; ++i) {
        const ub::HandleScope scope(f.iso());
        auto script = ub::Script::Compile(f.context, "def g():\n    return 3\ng() + " + std::to_string(i % 10));
        REQUIRE(script.has_value());
        REQUIRE(script->Run(f.context).has_value());
        if (i % 100 == 0) {
            const auto blob = script->CreateCodeCache();
            REQUIRE(blob.has_value());
            auto again = ub::Script::CompileWithCache(f.context, "def g():\n    return 3\ng() + 0", *blob);
            REQUIRE(again.has_value());
        }
    }
    Run(f.context, "gc.collect()");
    CHECK(ub_test::OutstandingAllocations() - before <= 16);
}

// ===========================================================================
// Buffers
// ===========================================================================

namespace {
/// Asks for an ArrayBuffer no machine can hold; answers whether it was refused
/// the way the header says - empty, with nothing thrown.
void MakesHugeBuffer(const ub::CallbackInfo& info) {
    const auto huge = ub::ArrayBuffer::New(info.GetContext(), (std::numeric_limits<std::size_t>::max() / 2) - 64);
    info.GetReturnValue().Set(!huge.has_value() && !info.GetIsolate().HasPendingException());
}
}  // namespace

TEST_CASE("lifetimes: a bytearray too large to allocate is refused without a word about exported buffers") {
    // CPython's `PyByteArray_FromStringAndSize` (3.12, and still 3.14) frees
    // the object it was making, when the storage cannot be had, before it has
    // set the field that
    // counts buffer exports - so the deallocator reads whatever the allocator
    // left there, and when that is positive prints "SystemError: deallocated
    // bytearray object has exported buffers". The litter below puts a
    // positive word exactly there: freed ints of the same size class, whose
    // digits cover that offset with 0x3FFFFFFF.
    Fixture f;
    Expose(f.context, "makesHugeBuffer", Native(f.context, &MakesHugeBuffer));
    CHECK(EvalTruth(f.context, R"(
printed = []
saved = sys.excepthook
sys.excepthook = lambda kind, value, tb: printed.append(repr(value))
refused = 0
try:
    for round in range(200):
        litter = [(2**300 - 1) - i for i in range(64)]
        del litter
        if makesHugeBuffer():
            refused += 1
        litter = [(2**300 - 1) - i for i in range(64)]
        del litter
        try:
            unibind.TypedArray('int8', sys.maxsize - 64)
        except MemoryError:
            refused += 1
finally:
    sys.excepthook = saved
refused == 400 and printed == [] and not hasattr(sys, 'last_exc')
)"));
}
