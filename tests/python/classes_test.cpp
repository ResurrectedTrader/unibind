// Classes: `Class<T>`, whose instances carry native state - and the lifetime
// of that state, which is the part a passing suite most easily hides. Every
// lifetime case here counts destructions per native, because a total that
// comes out right is what a double destruction plus a leak looks like.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support.h"

using py_test::Eval;
using py_test::EvalError;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Expose;

namespace {
/// A fixture whose realm has imported `unibind`, which the scripts here name.
struct Fixture : py_test::Fixture {
    Fixture() { REQUIRE(ub::Evaluate(context, "import unibind").has_value()); }
};
}  // namespace
using py_test::Str;

namespace {

/// Counts births and deaths, and remembers which thread each death was on.
struct Counter {
    static inline int alive = 0;
    static inline int constructed = 0;
    static inline int destroyed = 0;
    static inline bool destroyedOffThread = false;
    static inline std::thread::id owner;

    explicit Counter(std::int32_t start) : value(start) {
        ++alive;
        ++constructed;
    }
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&) = delete;
    Counter& operator=(Counter&&) = delete;
    ~Counter() {
        --alive;
        ++destroyed;
        if (std::this_thread::get_id() != owner) {
            destroyedOffThread = true;
        }
    }

    static void Reset() {
        alive = 0;
        constructed = 0;
        destroyed = 0;
        destroyedOffThread = false;
        owner = std::this_thread::get_id();
    }

    std::int32_t value = 0;
};

struct Unrelated {
    int unused = 0;
};

std::unique_ptr<Counter> MakeCounter(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;
        }
        start = *asInt;
    }
    if (start < 0) {
        info.ThrowTypeError("a counter starts at zero or above");
        return nullptr;
    }
    // Written onto the receiver, which is the instance being made.
    (void)info.This().Set(info.GetContext(), "how",
                          Str(info.GetIsolate(), info.IsConstructCall() ? "constructed" : "called"));
    return std::make_unique<Counter>(start);
}

std::unique_ptr<Counter> Declines(const ub::CallbackInfo& /*info*/) {
    return nullptr;
}

std::unique_ptr<Counter> MakesThenRefuses(const ub::CallbackInfo& info) {
    auto native = std::make_unique<Counter>(1);
    info.ThrowTypeError("this constructor changed its mind");
    return nullptr;
}

std::shared_ptr<Counter> MakeShared(const ub::CallbackInfo& /*info*/) {
    return std::make_shared<Counter>(100);
}

void Increment(Counter& self, const ub::CallbackInfo& info) {
    std::int32_t by = 1;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return;
        }
        by = *asInt;
    }
    self.value += by;
    info.GetReturnValue().Set(self.value);
}

void ReadValue(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void WriteValue(Counter& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    if (const auto asInt = value.ToInt32(info.GetContext())) {
        self.value = *asInt;
    }
}

void HowManyAlive(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(Counter::alive);
}

/// A JavaScript-style iterator counting up to the native's value.
void IterateCounter(Counter& self, const ub::CallbackInfo& info) {
    auto maker = ub::Evaluate(info.GetContext(), R"(
class _CountUp:
    def __init__(self, n):
        self.i = 0
        self.n = n
    def next(self):
        i = self.i
        self.i += 1
        return {'done': i >= self.n, 'value': i}
_CountUp
)");
    if (!maker) {
        return;
    }
    const auto type = maker->To<ub::Function>();
    if (!type) {
        return;
    }
    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(info.GetIsolate(), self.value)};
    if (auto iterator = type->NewInstance(info.GetContext(), arguments)) {
        info.GetReturnValue().Set(*iterator);
    }
}

ub::Intercepted SparseGetter(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto asString = property.To<ub::String>();
    if (!asString || asString->Utf8Value() != "invented") {
        return ub::Intercepted::No;
    }
    // A hook on a class instance can reach the native through its receiver.
    Counter* self = ub::Class<Counter>::Unwrap(info.This());
    if (self == nullptr) {
        info.ThrowTypeError("not a counter");
        return ub::Intercepted::Yes;
    }
    info.GetReturnValue().Set(self->value * 1000);
    return ub::Intercepted::Yes;
}

/// The backend's own "was this made by this class" - by the class that made
/// it, not by the native it carries. No public wrapper asks it, so ask it here.
bool ClassHas(const ub::Context& context, const ub::Class<Counter>& cls, const ub::Local<ub::Value>& value) {
    return ub::detail::ClassHasInstance(context, cls.rec(), value.slot()).value_or(false);
}

ub::Class<Counter> DeclareCounter(ub::Isolate& isolate, const char* name = "Counter") {
    const auto cls = ub::Class<Counter>::New(isolate, name);
    cls.Construct<&MakeCounter>();
    cls.Method<&Increment>("increment");
    cls.Accessor<&ReadValue, &WriteValue>("value");
    cls.StaticMethod("alive", &HowManyAlive);
    cls.StaticValue("LIMIT", ub::Constant(std::int32_t{1000}));
    return cls;
}

}  // namespace

TEST_CASE("classes: Python constructs an instance that carries native state") {
    Counter::Reset();
    {
        Fixture f;
        const auto cls = DeclareCounter(f.iso());
        Expose(f.context, "Counter", *cls.GetConstructor(f.context));

        CHECK(EvalInt(f.context, "c = Counter(5)\nc.increment()") == 6);
        CHECK(EvalInt(f.context, "c.increment(10)") == 16);
        CHECK(EvalInt(f.context, "c.value") == 16);
        CHECK(EvalInt(f.context, "c.value = 3\nc.value") == 3);
        CHECK(EvalText(f.context, "c.how") == "constructed");
        const auto instance = Eval(f.context, "c");
        CHECK(cls.IsInstance(instance));
        REQUIRE(ub::Class<Counter>::Unwrap(instance) != nullptr);
        CHECK(ub::Class<Counter>::Unwrap(instance)->value == 3);
        CHECK(ClassHas(f.context, cls, instance));
        CHECK_FALSE(ClassHas(f.context, cls, Eval(f.context, "unibind.Object()")));
        CHECK(Counter::alive == 1);
    }
    CHECK(Counter::alive == 0);
    CHECK(Counter::destroyed == Counter::constructed);
}

TEST_CASE("classes: a class is a named type, with statics on it and methods on its prototype") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));

    CHECK(EvalText(f.context, "Counter.__name__ + ' ' + type(Counter(1)).__name__") == "Counter Counter");
    CHECK(EvalText(f.context, "repr(Counter(1))") == "Counter {how: 'constructed'}");
    CHECK(EvalInt(f.context, "keep = [Counter(1), Counter(2)]\nCounter.alive()") == 2);
    CHECK(EvalInt(f.context, "Counter.LIMIT") == 1000);
    CHECK(EvalError(f.context, "Counter.LIMIT = 1").starts_with("TypeError"));
    CHECK(EvalTruth(f.context, "'increment' in dir(Counter.prototype) and 'increment' not in list(Counter(1))"));
    // The class constructor is the same object however it is asked for.
    CHECK(cls.GetConstructor(f.context)->StrictEquals(Eval(f.context, "Counter")));
}

TEST_CASE("classes: a plain call from native is refused, unless the class opted in") {
    Counter::Reset();
    Fixture f;
    const auto strict = DeclareCounter(f.iso(), "Strict");
    const auto callable = ub::Class<Counter>::New(f.iso(), "Callable");
    callable.ConstructOrCall<&MakeCounter>();
    const auto strictType = strict.GetConstructor(f.context);
    const auto callableType = callable.GetConstructor(f.context);
    REQUIRE(strictType.has_value());
    REQUIRE(callableType.has_value());

    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(f.iso(), 4)};
    {
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(strictType->Call(f.context, ub::Undefined(f.iso()), arguments).has_value());
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(f.context).value_or("") ==
              "TypeError: Class constructor Strict cannot be invoked without 'new'");
    }
    const auto called = callableType->Call(f.context, ub::Undefined(f.iso()), arguments);
    REQUIRE(called.has_value());
    CHECK(callable.IsInstance(*called));
    CHECK(ub::Class<Counter>::Unwrap(*called)->value == 4);
    CHECK(py_test::TextOf(f.context, *called->To<ub::Object>()->Get(f.context, "how")) == "called");
    // NewInstance is `new`, both ways.
    const auto made = strictType->NewInstance(f.context, arguments);
    REQUIRE(made.has_value());
    CHECK(strict.IsInstance(*made));
}

TEST_CASE("classes: a class with no constructor can only be wrapped") {
    Counter::Reset();
    Fixture f;
    const auto cls = ub::Class<Counter>::New(f.iso(), "Opaque");
    cls.Accessor<&ReadValue>("value");
    Expose(f.context, "Opaque", *cls.GetConstructor(f.context));
    CHECK(EvalError(f.context, "Opaque()") == "TypeError: Opaque cannot be constructed from script");

    const auto wrapped = cls.Wrap(f.context, std::make_shared<Counter>(9));
    REQUIRE(wrapped.has_value());
    Expose(f.context, "wrapped", *wrapped);
    CHECK(EvalInt(f.context, "wrapped.value") == 9);
    CHECK(EvalTruth(f.context, "type(wrapped) is Opaque"));
    CHECK(Counter::constructed == 1);
}

TEST_CASE("classes: a constructor that throws or declines makes no instance and keeps no native") {
    Counter::Reset();
    {
        Fixture f;
        const auto cls = DeclareCounter(f.iso());
        const auto refuses = ub::Class<Counter>::New(f.iso(), "Refuses");
        refuses.Construct<&MakesThenRefuses>();
        const auto declines = ub::Class<Counter>::New(f.iso(), "Declines");
        declines.Construct<&Declines>();
        Expose(f.context, "Counter", *cls.GetConstructor(f.context));
        Expose(f.context, "Refuses", *refuses.GetConstructor(f.context));
        Expose(f.context, "Declines", *declines.GetConstructor(f.context));

        CHECK(EvalError(f.context, "Counter(-1)") == "TypeError: a counter starts at zero or above");
        CHECK(EvalError(f.context, "Refuses()") == "TypeError: this constructor changed its mind");
        CHECK(EvalError(f.context, "Declines()") == "unibind.Error: constructor declined");
        CHECK(EvalTruth(f.context, "isinstance(unibind.Error('x'), Exception)"));
        CHECK(Counter::alive == 0);
        CHECK(Counter::destroyed == 1);
        CHECK(EvalError(f.context, "Counter('not a number but a string that ToInt32 makes zero') and Counter(-5)")
                  .starts_with("TypeError"));
    }
    CHECK(Counter::alive == 0);
    CHECK(Counter::destroyed == Counter::constructed);
}

TEST_CASE("classes: an instance-template accessor is an own property of every instance") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    cls.InstanceTemplate().SetAccessor("own",
                                       [](const ub::Local<ub::Name>& /*name*/, const ub::PropertyCallbackInfo& info) {
                                           Counter* self = ub::Class<Counter>::Unwrap(info.This());
                                           info.GetReturnValue().Set(self != nullptr ? self->value : -1);
                                       });
    cls.InstanceTemplate().Set("tag", ub::Constant(std::string_view("counted")));
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));

    CHECK(EvalInt(f.context, "Counter(8).own") == 8);
    CHECK(EvalText(f.context, "','.join(Counter(8))") == "own,tag,how");
    const auto wrapped = cls.Wrap(f.context, std::make_unique<Counter>(2));
    REQUIRE(wrapped.has_value());
    CHECK(wrapped->Get(f.context, "own")->To<ub::Integer>()->Int32Value() == 2);
    CHECK(wrapped->HasOwn(f.context, Str(f.iso(), "own")).value_or(false));
    CHECK_FALSE(wrapped->HasOwn(f.context, Str(f.iso(), "value")).value_or(true));
}

TEST_CASE("classes: a Python subclass keeps its own methods and still carries the native") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));
    REQUIRE(ub::Evaluate(f.context, R"(
class Sub(Counter):
    def twice(self):
        return self.value * 2
    @property
    def label(self):
        return 'sub:' + str(self.value)
s = Sub(21)
)")
                .has_value());

    CHECK(EvalInt(f.context, "s.twice()") == 42);
    CHECK(EvalText(f.context, "s.label") == "sub:21");
    CHECK(EvalInt(f.context, "Sub(5).increment(2)") == 7);
    CHECK(EvalTruth(f.context, "type(s) is Sub and isinstance(s, Counter) and s.constructor is Counter"));
    const auto made = Eval(f.context, "s");
    CHECK(cls.IsInstance(made));
    CHECK(ub::Class<Counter>::Unwrap(made)->value == 21);
    CHECK(ClassHas(f.context, cls, made));
}

TEST_CASE("classes: a Python subclass overrides a class method, and can call the base's") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));
    REQUIRE(ub::Evaluate(f.context, R"(
class Loud(Counter):
    def increment(self, by=1):
        return 1000 + super().increment(by)
class Deeper(Loud):
    pass
l = Loud(1)
)")
                .has_value());

    // The subclass's method runs, and `super()` still reaches the native one.
    CHECK(EvalInt(f.context, "l.increment(2)") == 1003);
    CHECK(EvalInt(f.context, "l.value") == 3);
    CHECK(EvalInt(f.context, "Deeper(0).increment()") == 1001);
    // The base class is untouched.
    CHECK(EvalInt(f.context, "Counter(1).increment(2)") == 3);
    // An own property of the instance still comes first, as an instance
    // attribute does in Python.
    CHECK(EvalInt(f.context, "l.increment = lambda by=1: -1\nl.increment()") == -1);
}

TEST_CASE("classes: the native is recovered as its real type or not at all") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    const auto wrapped = cls.Wrap(f.context, std::make_unique<Counter>(3));
    REQUIRE(wrapped.has_value());
    CHECK(ub::Class<Counter>::Unwrap(*wrapped)->value == 3);
    CHECK(ub::Class<Unrelated>::Unwrap(*wrapped) == nullptr);
    CHECK(ub::Class<Counter>::Unwrap(Eval(f.context, "unibind.Object()")) == nullptr);
    CHECK(ub::Class<Counter>::Unwrap(Eval(f.context, "{}")) == nullptr);
    CHECK(ub::Class<Counter>::Unwrap(Eval(f.context, "3")) == nullptr);
    const auto share = ub::Class<Counter>::UnwrapShared(*wrapped);
    REQUIRE(share != nullptr);
    CHECK(share.use_count() == 2);
}

TEST_CASE("classes: a method called on the wrong receiver throws rather than unwrapping") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));
    CHECK(EvalError(f.context, "Counter.prototype.increment()") ==
          "TypeError: method called on a receiver of the wrong type");
    CHECK(EvalError(f.context, "o = unibind.Object()\no.increment = Counter.prototype['increment']\no.increment()")
              .starts_with("TypeError"));
    CHECK(EvalError(f.context, "Counter.prototype.value").starts_with("TypeError"));
}

TEST_CASE("classes: a prototype swap fools neither the native check nor HasInstance") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));
    const auto swapped = Eval(f.context, "a = Counter(4)\na.__proto__ = None\na");
    CHECK(cls.IsInstance(swapped));
    CHECK(ClassHas(f.context, cls, swapped));
    CHECK(EvalTruth(f.context, "not hasattr(a, 'increment')"));
    const auto lookalike = Eval(f.context, "b = unibind.Object()\nb.__proto__ = Counter.prototype\nb");
    CHECK_FALSE(cls.IsInstance(lookalike));
    CHECK_FALSE(ClassHas(f.context, cls, lookalike));
    CHECK(EvalError(f.context, "b.increment()").starts_with("TypeError"));
}

TEST_CASE("classes: one declaration serves several realms, each with a type of its own") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    auto second = ub::Context::New(f.iso());
    REQUIRE(second.has_value());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));
    {
        const ub::ContextScope entered(*second);
        Expose(*second, "Counter", *cls.GetConstructor(*second));
        CHECK(EvalInt(*second, "Counter(2).increment()") == 3);
        const auto there = Eval(*second, "Counter(1)");
        CHECK(cls.IsInstance(there));
        CHECK(ClassHas(f.context, cls, there));
    }
    CHECK(EvalInt(f.context, "Counter(7).increment()") == 8);
    CHECK_FALSE(cls.GetConstructor(f.context)->StrictEquals(*cls.GetConstructor(*second)));
}

TEST_CASE("classes: an interceptor covers every instance, and can reach the native") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    cls.SetHandler(ub::NamedPropertyHandler{.getter = &SparseGetter});
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));

    CHECK(EvalInt(f.context, "Counter(3).invented") == 3000);
    // Declined names reach the prototype, where the methods and accessors are.
    CHECK(EvalInt(f.context, "Counter(3).increment()") == 4);
    CHECK(EvalInt(f.context, "Counter(3).value") == 3);
    const auto wrapped = cls.Wrap(f.context, std::make_unique<Counter>(5));
    REQUIRE(wrapped.has_value());
    CHECK(wrapped->Get(f.context, "invented")->To<ub::Integer>()->Int32Value() == 5000);
}

TEST_CASE("classes: a Symbol.iterator method makes instances iterable from Python") {
    Counter::Reset();
    Fixture f;
    const auto cls = ub::Class<Counter>::New(f.iso(), "Countdown");
    cls.Construct<&MakeCounter>();
    cls.SymbolMethod<&IterateCounter>(ub::WellKnownSymbol::Iterator);
    Expose(f.context, "Countdown", *cls.GetConstructor(f.context));

    CHECK(EvalText(f.context, "','.join(str(i) for i in Countdown(3))") == "0,1,2");
    CHECK(EvalTruth(f.context, "list(Countdown(0)) == [] and sorted(Countdown(4), reverse=True) == [3, 2, 1, 0]"));
}

TEST_CASE("classes: a constructor may hand back a share, and it is the share that goes") {
    Counter::Reset();
    {
        Fixture f;
        const auto cls = ub::Class<Counter>::New(f.iso(), "Shared");
        cls.Construct<&MakeShared>();
        cls.Method<&Increment>("increment");
        Expose(f.context, "Shared", *cls.GetConstructor(f.context));
        CHECK(EvalInt(f.context, "Shared().increment()") == 101);
        CHECK(Counter::alive == 0);  // the temporary went with its last reference
        CHECK(EvalInt(f.context, "kept = Shared()\nkept.increment(2)") == 102);
        CHECK(Counter::alive == 1);
    }
    CHECK(Counter::alive == 0);
    CHECK(Counter::destroyed == 2);
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

TEST_CASE("classes: a native goes with its instance's last reference, exactly once") {
    Counter::Reset();
    {
        Fixture f;
        const auto cls = DeclareCounter(f.iso());
        Expose(f.context, "Counter", *cls.GetConstructor(f.context));
        CHECK(EvalInt(f.context, "c = Counter(1)\nCounter.alive()") == 1);
        CHECK(Eval(f.context, "del c").IsUndefined());
        CHECK(Counter::alive == 0);
        CHECK(Counter::destroyed == 1);

        // Stored in Python's own containers, it lives as long as they hold it.
        CHECK(EvalInt(f.context, "box = {'k': [Counter(2), (Counter(3),)]}\nCounter.alive()") == 2);
        CHECK(Eval(f.context, "del box['k'][0]").IsUndefined());
        CHECK(Counter::alive == 1);
        CHECK(Eval(f.context, "box.clear()").IsUndefined());
        CHECK(Counter::alive == 0);
        CHECK(Counter::destroyed == 3);
    }
    CHECK(Counter::destroyed == Counter::constructed);
    CHECK_FALSE(Counter::destroyedOffThread);
}

TEST_CASE("classes: instances in a cycle are collected by gc.collect(), natives with them") {
    Counter::Reset();
    Fixture f;
    const auto cls = DeclareCounter(f.iso());
    Expose(f.context, "Counter", *cls.GetConstructor(f.context));
    CHECK(EvalInt(f.context, R"(
import gc
gc.disable()
a = Counter(1); b = Counter(2)
a.other = b; b.other = a
a.me = a
del a, b
Counter.alive()
)") == 2);
    CHECK(EvalInt(f.context, "gc.collect()\ngc.enable()\nCounter.alive()") == 0);
    CHECK(Counter::destroyed == 2);
    CHECK_FALSE(Counter::destroyedOffThread);
}

TEST_CASE("classes: the isolate gives back every share it still holds when it goes") {
    Counter::Reset();
    std::shared_ptr<Counter> coOwned = std::make_shared<Counter>(50);
    std::weak_ptr<Counter> watch = coOwned;
    {
        Fixture f;
        const auto cls = DeclareCounter(f.iso());
        Expose(f.context, "Counter", *cls.GetConstructor(f.context));
        // Survivors of every kind: in the globals, in a cycle the collector
        // never got round to, in a container, and co-owned with the embedder.
        REQUIRE(ub::Evaluate(f.context, R"(
import gc
gc.disable()
survivor = Counter(1)
x = Counter(2); y = Counter(3); x.y = y; y.x = x
del x, y
kept = [Counter(4)]
)")
                    .has_value());
        const auto wrapped = cls.Wrap(f.context, coOwned);
        REQUIRE(wrapped.has_value());
        Expose(f.context, "shared", *wrapped);
        // Twice over one native: two wrappers, one native, two shares.
        const auto again = cls.Wrap(f.context, coOwned);
        REQUIRE(again.has_value());
        Expose(f.context, "sharedAgain", *again);
        CHECK(coOwned.use_count() == 3);
        CHECK(Counter::alive == 5);
    }
    // The four the engine owned outright are gone; the shared one is still
    // the embedder's, and the engine gave back both of its shares.
    CHECK(Counter::alive == 1);
    CHECK(coOwned.use_count() == 1);
    CHECK(Counter::destroyed == 4);
    coOwned.reset();
    CHECK(watch.expired());
    CHECK(Counter::destroyed == 5);
    CHECK_FALSE(Counter::destroyedOffThread);
}

TEST_CASE("classes: a native in the embedder's own storage is never destroyed by the engine") {
    Counter::Reset();
    Counter stack(7);
    {
        Fixture f;
        const auto cls = DeclareCounter(f.iso());
        const auto wrapped = cls.Wrap(f.context, std::shared_ptr<Counter>(&stack, [](Counter*) {}));
        REQUIRE(wrapped.has_value());
        Expose(f.context, "borrowed", *wrapped);
        CHECK(EvalInt(f.context, "borrowed.increment()") == 8);
        CHECK(Eval(f.context, "del borrowed").IsUndefined());
    }
    CHECK(stack.value == 8);
    CHECK(Counter::destroyed == 0);
}

TEST_CASE("classes: a share taken out of a wrapper outlives the wrapper and the isolate") {
    Counter::Reset();
    std::shared_ptr<Counter> taken;
    {
        Fixture f;
        const auto cls = DeclareCounter(f.iso());
        Expose(f.context, "Counter", *cls.GetConstructor(f.context));
        taken = ub::Class<Counter>::UnwrapShared(Eval(f.context, "c = Counter(12)\nc"));
        REQUIRE(taken != nullptr);
        CHECK(Eval(f.context, "del c").IsUndefined());
        CHECK(Counter::alive == 1);
    }
    CHECK(taken->value == 12);
    taken.reset();
    CHECK(Counter::alive == 0);
    CHECK(Counter::destroyed == 1);
}
