// Lifetimes of natives and of the calls that reach them: a `Class<T>` native is
// destroyed exactly once, on the isolate's thread, whatever script does to its
// instance - cycles, resurrection, a destructor that runs script of its own -
// and a native callback's values live exactly as long as its call.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lifetimes_support.h"

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

/// A native that counts itself, and can hold a realm and a root of its own -
/// which its destructor gives back, into the interpreter, as the members go.
///
/// It also remembers where every destroyed one was, so that a method reaching
/// one after its destructor has begun is caught for certain - rather than
/// reading freed memory that happens to still look right.
struct Tracked {
    static inline std::atomic<int> alive{0};
    static inline std::atomic<int> constructed{0};
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<bool> destroyedOffThread{false};
    static inline std::atomic<int> usedAfterDestroy{0};
    static inline std::mutex graveyardMutex;
    static inline std::set<const Tracked*> graveyard;

    Tracked() : owner(std::this_thread::get_id()) {
        ++alive;
        ++constructed;
        const std::lock_guard<std::mutex> lock(graveyardMutex);
        graveyard.erase(this);  // an address the allocator gave back out
    }
    explicit Tracked(int start) : Tracked() { value = start; }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() {
        --alive;
        ++destroyed;
        if (std::this_thread::get_id() != owner) {
            destroyedOffThread = true;
        }
        const std::lock_guard<std::mutex> lock(graveyardMutex);
        graveyard.insert(this);
    }

    /// Called by every method: counts a call on a native being or already
    /// destroyed.
    void Check() const {
        const std::lock_guard<std::mutex> lock(graveyardMutex);
        if (graveyard.contains(this)) {
            ++usedAfterDestroy;
        }
    }

    static void Reset() {
        alive = 0;
        constructed = 0;
        destroyed = 0;
        destroyedOffThread = false;
        usedAfterDestroy = 0;
    }

    std::thread::id owner;
    int value = 0;
    std::optional<ub::Context> realm;
    ub::Global<ub::Value> held;
};

std::unique_ptr<Tracked> MakeTracked(const ub::CallbackInfo& info) {
    int start = 0;
    if (info.Length() > 0) {
        start = info[0].ToInt32(info.GetContext()).value_or(0);
    }
    return std::make_unique<Tracked>(start);
}

void ReadTracked(Tracked& self, const ub::CallbackInfo& info) {
    self.Check();
    info.GetReturnValue().Set(self.value);
}

/// `hold(value)`: the native keeps a root on `value`, and the realm it was
/// called in.
void HoldInTracked(Tracked& self, const ub::CallbackInfo& info) {
    self.held = ub::Global<ub::Value>(info.GetIsolate(), info[0]);
    self.realm = info.GetContext();
}

/// `holdOnly(value)`: a root, and no realm.
void HoldOnly(Tracked& self, const ub::CallbackInfo& info) {
    self.held = ub::Global<ub::Value>(info.GetIsolate(), info[0]);
}

/// `dropHolder()`: runs script that lets go of every reference script has to
/// the receiver, collects, and then uses the receiver and its native.
void DropsItsHolder(Tracked& self, const ub::CallbackInfo& info) {
    (void)ub::Evaluate(info.GetContext(), "holder.clear()\ngc.collect()");
    Tracked* again = ub::Class<Tracked>::Unwrap(info.This());
    const bool same = again == &self;
    const auto tag = info.This().Get(info.GetContext(), "tag");
    info.GetReturnValue().Set(same && tag.has_value() && tag->IsString() ? self.value : -1);
}

ub::Class<Tracked> DeclareTracked(ub::Isolate& isolate, const char* name = "Tracked") {
    const auto cls = ub::Class<Tracked>::New(isolate, name);
    cls.Construct<&MakeTracked>();
    cls.Method<&ReadTracked>("read");
    cls.Method<&HoldInTracked>("hold");
    cls.Method<&HoldOnly>("holdOnly");
    cls.Method<&DropsItsHolder>("dropHolder");
    return cls;
}

struct NativeFixture : Fixture {
    NativeFixture() : cls(DeclareTracked(iso())) { Expose(context, "Tracked", *cls.GetConstructor(context)); }
    ub::Class<Tracked> cls;
};

}  // namespace

// ===========================================================================
// Natives
// ===========================================================================

TEST_CASE("lifetimes: a native in a cycle through Python containers and closures goes once, at collection") {
    Tracked::Reset();
    {
        NativeFixture f;
        Run(f.context, R"(
def closure_cycle():
    t = Tracked(1)
    t.callback = lambda: t          # instance -> its properties -> closure -> cell -> instance
    return weakref.ref(t)
def container_cycle():
    box = [Tracked(2)]
    box.append(box)                 # a list holding itself and the instance
    box[0].box = box                # and the instance holding the list
    return weakref.ref(box[0])
def dict_cycle():
    d = {'t': Tracked(3)}
    d['t'].back = d
    return weakref.ref(d['t'])
refs = [closure_cycle(), container_cycle(), dict_cycle()]
)");
        CHECK(Tracked::alive == 3);
        CHECK(Tracked::destroyed == 0);
        Run(f.context, "gc.collect()");
        CHECK(EvalTruth(f.context, "all(r() is None for r in refs)"));
        CHECK(Tracked::alive == 0);
        CHECK(Tracked::destroyed == 3);
    }
    CHECK(Tracked::destroyed == 3);
    CHECK_FALSE(Tracked::destroyedOffThread);
}

TEST_CASE("lifetimes: an instance a __del__ resurrects keeps its native, which goes once, at the real end") {
    Tracked::Reset();
    {
        NativeFixture f;
        Run(f.context, R"(
saved = []
class Phoenix(Tracked):
    def __del__(self):
        saved.append(self)
p = Phoenix(41)
del p
)");
        // Resurrected by reference counting: the native is untouched.
        CHECK(Tracked::destroyed == 0);
        CHECK(EvalInt(f.context, "saved[0].read()") == 41);
        // And by the collector, from a cycle.
        Run(f.context, "q = Phoenix(42)\nq.me = q\ndel q\ngc.collect()");
        CHECK(Tracked::destroyed == 0);
        CHECK(EvalInt(f.context, "saved[1].read()") == 42);
        // `__del__` runs once per object: the second time they go, they go.
        Run(f.context, "saved[1].me = None\nsaved.clear()\ngc.collect()");
        CHECK(Tracked::destroyed == 2);
        CHECK(Tracked::alive == 0);
    }
    CHECK(Tracked::destroyed == 2);
}

TEST_CASE("lifetimes: a native's destructor gives back its own realm and root, at collection and at teardown") {
    Tracked::Reset();
    {
        NativeFixture f;
        Run(f.context, "freed = []\nclass Payload: pass");
        // One collected while the isolate lives: its destructor releases a
        // Global from inside a deallocation, and the Payload goes with it.
        Run(f.context, "a = Tracked(1)\np = Payload()\nwp = weakref.ref(p, lambda _: freed.append('a'))\n"
                       "a.holdOnly(p)\ndel p\ndel a");
        CHECK(Tracked::destroyed == 1);
        CHECK(EvalTruth(f.context, "freed == ['a']"));

        // One held only by its own realm, which it holds: a Context in a
        // native, and the native in that realm's globals - a cycle through C++
        // that no collection can see. The isolate's teardown breaks it.
        auto other = ub::Context::New(f.iso());
        REQUIRE(other.has_value());
        Expose(*other, "Tracked", *f.cls.GetConstructor(*other));
        Run(*other, "import weakref\nclass Payload: pass\nq = Payload()\nsurvivor = Tracked(2)\nsurvivor.hold(q)\n"
                    "watch = weakref.ref(q)");
        Run(f.context, "import builtins");
        other.reset();
        Run(f.context, "gc.collect()");
        CHECK(Tracked::alive == 1);
    }
    CHECK(Tracked::alive == 0);
    CHECK(Tracked::destroyed == 2);
    CHECK_FALSE(Tracked::destroyedOffThread);
}

TEST_CASE("lifetimes: natives destroyed at teardown whose destructors run script that reaches other natives") {
    // Each native roots an object whose `__del__` calls `read()` on every
    // instance - including ones whose natives teardown has already destroyed,
    // and the one being destroyed now. Those calls must fail cleanly, never
    // reach a destroyed native.
    Tracked::Reset();
    {
        NativeFixture f;
        Run(f.context, R"(
import builtins
builtins.everyone = [Tracked(i) for i in range(16)]
builtins.reads = []
class Spite:
    def __del__(self):
        for other in builtins.everyone:
            try:
                builtins.reads.append(other.read())
            except TypeError:
                builtins.reads.append(None)
for s in builtins.everyone:
    s.holdOnly(Spite())
)");
        CHECK(Tracked::alive == 16);
    }
    CHECK(Tracked::alive == 0);
    CHECK(Tracked::destroyed == 16);
    CHECK(Tracked::usedAfterDestroy == 0);
}

TEST_CASE("lifetimes: a native whose destructor makes another native at teardown - that one goes too") {
    Tracked::Reset();
    {
        NativeFixture f;
        Run(f.context, R"(
import builtins
class MakesMore:
    def __del__(self):
        try:
            builtins.late = Tracked(99)   # the bindings are going: this may be refused
        except TypeError:
            builtins.late = None
builtins.first = Tracked(1)
builtins.first.holdOnly(MakesMore())
)");
        CHECK(Tracked::alive == 1);
    }
    CHECK(Tracked::alive == 0);
    CHECK(Tracked::destroyed == Tracked::constructed);
}

TEST_CASE("lifetimes: one shared native wrapped in two isolates on two threads at once") {
    Tracked::Reset();
    auto shared = std::make_shared<Tracked>(7);
    std::atomic<int> reads{0};
    auto body = [&shared, &reads] {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        {
            const ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            REQUIRE(context.has_value());
            const ub::ContextScope entered(*context);
            const auto cls = DeclareTracked(*isolate);
            for (int i = 0; i < 200; ++i) {
                const ub::HandleScope inner(*isolate);
                const auto wrapped = cls.Wrap(*context, shared);
                REQUIRE(wrapped.has_value());
                Expose(*context, "w", *wrapped);
                if (EvalInt(*context, "w.read()") == 7) {
                    ++reads;
                }
                if (i % 50 == 0) {
                    Run(*context, "import gc\ngc.collect()");
                }
            }
        }
    };
    std::thread a(body);
    std::thread b(body);
    a.join();
    b.join();
    CHECK(reads == 400);
    // Every share the wrappers took went back with them.
    CHECK(shared.use_count() == 1);
    CHECK(Tracked::destroyed == 0);
    shared.reset();
    CHECK(Tracked::destroyed == 1);
}

TEST_CASE("lifetimes: an instance that dies on a script's thread gives its native back on the isolate's") {
    Tracked::Reset();
    {
        NativeFixture f;
        Run(f.context, R"(
import threading
box = [Tracked(3) for _ in range(5)]
def drop():
    box.clear()          # the last references go on this thread
    gc.collect()
    try:
        Tracked(4)       # a native call from a thread with no isolate
    except RuntimeError as e:
        box.append(str(e))
t = threading.Thread(target=drop)
t.start()
t.join()
)");
        CHECK(EvalText(f.context, "box[0]") == "unibind: no isolate on this thread");
        // Not destroyed there: a native is the embedder's, and goes on the
        // embedder's thread - at the latest when the isolate does.
        CHECK(Tracked::destroyed == 0);
    }
    CHECK(Tracked::alive == 0);
    CHECK(Tracked::destroyed == 5);
    CHECK_FALSE(Tracked::destroyedOffThread);
}

// ===========================================================================
// Callbacks
// ===========================================================================

TEST_CASE("lifetimes: a native that makes script drop every reference to its receiver still has it") {
    Tracked::Reset();
    NativeFixture f;
    Run(f.context, "holder = [Tracked(5)]\nholder[0].tag = 'kept'");
    CHECK(EvalInt(f.context, "holder[0].dropHolder()") == 5);
    // The call was the last owner: the instance and its native went with it.
    CHECK(Tracked::destroyed == 1);
    CHECK(Tracked::alive == 0);
}

namespace {
ub::Global<ub::Object> gKeptReceiver;
void KeepsReceiver(const ub::CallbackInfo& info) {
    gKeptReceiver = ub::Global<ub::Object>(info.GetIsolate(), info.This());
}
}  // namespace

TEST_CASE("lifetimes: a receiver kept in a Global past the call, and released later") {
    Fixture f;
    Expose(f.context, "keep", Native(f.context, &KeepsReceiver));
    Run(f.context, "o = unibind.Object()\no.keep = keep\no.name = 'the object'\nwo = weakref.ref(o)\n"
                   "o.keep()\ndel o\ngc.collect()");
    CHECK(EvalTruth(f.context, "wo() is not None"));
    {
        const ub::HandleScope scope(f.iso());
        CHECK(py_test::TextOf(f.context, *gKeptReceiver.Get(f.iso()).Get(f.context, "name")) == "the object");
    }
    gKeptReceiver.Reset();
    CHECK(EvalTruth(f.context, "wo() is None"));
}

namespace {
/// Makes values - some of them objects script watches - and then throws.
void BuildsThenThrows(const ub::CallbackInfo& info) {
    for (int i = 0; i < 40; ++i) {
        (void)ub::Evaluate(info.GetContext(), "Watched()");
        (void)ub::Object::New(info.GetContext());
        (void)ub::String::NewFromUtf8(info.GetIsolate(), "garbage");
    }
    info.Throw(ub::ErrorKind::Error, "built, then threw");
}
}  // namespace

TEST_CASE("lifetimes: a native that throws after building values leaves none of them alive") {
    Fixture f;
    Run(f.context, "made = weakref.WeakSet()\nclass Watched:\n    def __init__(self):\n        made.add(self)");
    Expose(f.context, "buildsThenThrows", Native(f.context, &BuildsThenThrows));
    for (int round = 0; round < 3; ++round) {
        CHECK(EvalError(f.context, "buildsThenThrows()") == "unibind.Error: built, then threw");
        CHECK(EvalInt(f.context, "len(made)") == 0);
    }
    CHECK(EvalTruth(f.context, "try:\n    buildsThenThrows()\nexcept unibind.Error:\n    pass\nlen(made) == 0"));
}

namespace {
/// `descend(n, down)`: makes a handle, calls `down(n - 1)` - Python that calls
/// `descend` again - and checks the handle is intact when it comes back.
void Descend(const ub::CallbackInfo& info) {
    const ub::Context& context = info.GetContext();
    const std::int32_t n = info[0].ToInt32(context).value_or(0);
    auto mine = ub::Object::New(context);
    if (!mine) {
        return;
    }
    (void)mine->Set(context, "depth", ub::Integer::New(info.GetIsolate(), n));
    std::int32_t below = 0;
    if (n > 0) {
        const auto down = info[1].To<ub::Function>();
        if (!down) {
            return;
        }
        const std::array<ub::Local<ub::Value>, 2> arguments{ub::Integer::New(info.GetIsolate(), n - 1), info[1]};
        const auto result = down->Call(context, ub::Undefined(info.GetIsolate()), arguments);
        if (!result) {
            return;  // an exception is on its way up
        }
        below = result->ToInt32(context).value_or(-1000000);
    }
    const auto depth = mine->Get(context, "depth");
    const bool intact = depth && depth->ToInt32(context).value_or(-1) == n;
    info.GetReturnValue().Set(intact ? below + 1 : -1000000);
}
}  // namespace

/// How deep the round trip goes: fifty, or ten against a debug CPython, whose
/// unoptimised evaluation loop costs some 50 KB of stack a level - fifty of
/// those do not fit the 1 MB main thread.
#if defined(NDEBUG)
constexpr int ROUND_TRIP_DEPTH = 50;
#else
constexpr int ROUND_TRIP_DEPTH = 10;
#endif

TEST_CASE("lifetimes: native into Python into native, fifty deep, and back out intact") {
    Fixture f;
    Expose(f.context, "descend", Native(f.context, &Descend));
    Run(f.context, "marker = object()\ndef down(n, again):\n    m = marker\n    return descend(n, again)");
    const std::int32_t base = RefCount(f.context, "marker");
    CHECK(EvalInt(f.context, "descend(" + std::to_string(ROUND_TRIP_DEPTH) + ", down)") == ROUND_TRIP_DEPTH + 1);
    CHECK(RefCount(f.context, "marker") == base);
    // Deep enough to hit a limit is an exception, not a crash, and leaves
    // nothing behind either.
    CHECK(EvalError(f.context, "descend(1000000, down)").starts_with("RecursionError"));
    CHECK(RefCount(f.context, "marker") == base);
    CHECK(EvalInt(f.context, "descend(5, down)") == 6);
}

namespace {

/// A named getter that answers `value` by clearing `holder` - the last
/// reference script had to the object it was asked about - and collecting,
/// then reading that object.
ub::Intercepted SelfDroppingGetter(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto name = property.To<ub::String>();
    if (!name || name->Utf8Value() != "value") {
        return ub::Intercepted::No;
    }
    (void)ub::Evaluate(info.GetContext(), "holder.clear()\ngc.collect()");
    const auto own = info.This().Get(info.GetContext(), "own");
    if (own) {
        info.GetReturnValue().Set(*own);
    }
    return ub::Intercepted::Yes;
}

/// A deleter that drops the object too, deletes the property itself, and says
/// it did.
std::optional<bool> SelfDroppingDeleter(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto name = property.To<ub::String>();
    if (!name || name->Utf8Value() != "doomed") {
        return std::nullopt;
    }
    (void)ub::Evaluate(info.GetContext(), "holder.clear()\ngc.collect()");
    (void)info.This().Set(info.GetContext(), "sawDelete", ub::Boolean::New(info.GetIsolate(), true));
    return true;
}

}  // namespace

TEST_CASE("lifetimes: interceptor hooks that let go of the object they were asked about") {
    Fixture f;
    const auto tpl = ub::ObjectTemplate::New(f.iso());
    tpl.SetHandler(ub::NamedPropertyHandler{.getter = &SelfDroppingGetter, .deleter = &SelfDroppingDeleter});
    const auto make = [&] {
        const ub::HandleScope scope(f.iso());
        const auto made = tpl.NewInstance(f.context);
        REQUIRE(made.has_value());
        REQUIRE(made->Set(f.context, "own", Str(f.iso(), "own value")).value_or(false));
        Expose(f.context, "made", *made);
        Run(f.context, "holder = [made]\nw = weakref.ref(made)\ndel made");
    };
    make();
    CHECK(EvalText(f.context, "holder[0].value") == "own value");
    Run(f.context, "gc.collect()");
    CHECK(EvalTruth(f.context, "w() is None"));
    make();
    CHECK(EvalTruth(f.context, "x = holder[0]\ndel x.doomed\nseen = x.sawDelete\ndel x\ngc.collect()\nseen"));
    CHECK(EvalTruth(f.context, "w() is None"));
}
