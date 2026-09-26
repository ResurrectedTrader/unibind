/// \file
/// `Class<T>`: a JavaScript class whose instances carry native state, with a
/// finalizer. The place where the two engines'
/// collectors have to be made to agree.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "support/harness.h"

namespace {

/// Counts its own births and deaths, so a test can ask whether the engine ever
/// destroyed what it was handed.
struct Counter {
    static inline int alive = 0;
    static inline int destroyed = 0;
    static inline int constructed = 0;

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
    }

    static void Reset() {
        alive = 0;
        destroyed = 0;
        constructed = 0;
    }

    std::int32_t value = 0;
};

/// A second native type, so "recovered as its real type or not at all" has
/// something to fail against.
struct Unrelated {
    int unused = 0;
};

/// Writes to its receiver before it answers, the way a binding author would:
/// `this` inside a constructor is the instance being made, on every engine
/// that has a receiver there at all.
std::unique_ptr<Counter> MakeCounterStamping(const ub::CallbackInfo& info) {
    auto made = std::make_unique<Counter>(1);
    const auto stamp = ub::Integer::New(info.GetIsolate(), 42);
    if (!info.This().Set(info.GetContext(), "stamped", stamp).value_or(false)) {
        info.ThrowTypeError("the constructor could not write to its receiver");
        return nullptr;
    }
    return made;
}

std::unique_ptr<Counter> MakeCounter(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;  // the coercion threw
        }
        start = *asInt;
    }
    if (start < 0) {
        info.ThrowTypeError("a counter starts at zero or above");
        return nullptr;
    }
    return std::make_unique<Counter>(start);
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
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return;
    }
    self.value = *asInt;
}

void HowManyAlive(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(Counter::alive);
}

/// Hands back an iterator built in script that counts up to the native's value.
void IterateCounter(Counter& self, const ub::CallbackInfo& info) {
    auto maker = ub::Evaluate(info.GetContext(),
                              "(function (n) { let i = 0; return { next: () => ({ value: i, done: i++ >= n }) }; })");
    if (!maker) {
        return;
    }
    const auto asFunction = maker->To<ub::Function>();
    if (!asFunction) {
        return;
    }
    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(info.GetIsolate(), self.value)};
    auto iterator = asFunction->Call(info.GetContext(), info.This(), arguments);
    if (!iterator) {
        return;
    }
    info.GetReturnValue().Set(*iterator);
}

/// Answers for one invented name and declines for everything else, so a test
/// can see both halves of an interceptor over a class's instances.
ub::Intercepted SparseGetter(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto asString = property.To<ub::String>();
    if (!asString || asString->Utf8Value() != "invented") {
        return ub::Intercepted::No;
    }
    if (!info.GetReturnValue().Set("from the interceptor")) {
        info.ThrowTypeError("could not make the string");
    }
    return ub::Intercepted::Yes;
}

/// What an embedder's own trampoline carries per member: which member it is,
/// and how often it ran. The untyped `Class<T>` overloads exist so that a
/// binding can go through one of these rather than straight to its body.
struct CallCounter {
    int calls = 0;
};

void CountedIncrement(const ub::CallbackInfo& info) {
    ++info.Data<CallCounter>()->calls;
    Counter* self = ub::Class<Counter>::Unwrap(info.This());
    if (self == nullptr) {
        info.ThrowTypeError("not a Counter");
        return;
    }
    self->value += 1;
    info.GetReturnValue().Set(self->value);
}

void CountedRead(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    ++info.Data<CallCounter>()->calls;
    Counter* self = ub::Class<Counter>::Unwrap(info.This());
    if (self == nullptr) {
        info.ThrowTypeError("not a Counter");
        return;
    }
    info.GetReturnValue().Set(self->value);
}

void CountedWrite(const ub::Local<ub::Name>& /*property*/, const ub::Local<ub::Value>& value,
                  const ub::PropertyCallbackInfo& info) {
    ++info.Data<CallCounter>()->calls;
    Counter* self = ub::Class<Counter>::Unwrap(info.This());
    const auto asInt = value.ToInt32(info.GetContext());
    if (self == nullptr || !asInt) {
        return;
    }
    self->value = *asInt;
}

void CountedIterate(const ub::CallbackInfo& info) {
    ++info.Data<CallCounter>()->calls;
    Counter* self = ub::Class<Counter>::Unwrap(info.This());
    if (self == nullptr) {
        info.ThrowTypeError("not a Counter");
        return;
    }
    IterateCounter(*self, info);
}

/// How many natives a counting deleter has released - what a constructor that
/// hands back a share with a deleter of its own is for.
int g_releasedBySharedDeleter = 0;

std::shared_ptr<Counter> MakeSharedCounter(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;
        }
        start = *asInt;
    }
    return {new Counter(start), [](Counter* counter) {
                ++g_releasedBySharedDeleter;
                delete counter;
            }};
}

/// An accessor one instance carries and its class does not.
void ReadExtra(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(*info.Data<std::int32_t>());
}

/// Declares the class the rest of this file uses.
ub::Class<Counter> DeclareCounter(ub::Isolate& isolate) {
    auto cls = ub::Class<Counter>::New(isolate, "Counter");
    cls.Construct<&MakeCounter>();
    cls.Method<&Increment>("increment");
    cls.Accessor<&ReadValue, &WriteValue>("value");
    cls.StaticMethod("alive", &HowManyAlive);
    cls.StaticValue("KIND", ub::Constant(std::string_view("counter")));
    return cls;
}

}  // namespace

UNIBIND_TEST_CASE(CLASSES, "classes: script constructs an instance that carries native state") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    const auto constructor = cls.GetConstructor(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Counter", *constructor);

    CHECK(ub_test::EvalInt(fixture.context, "new Counter(5).value") == 5);
    CHECK(ub_test::EvalInt(fixture.context, "const c = new Counter(5); c.increment(); c.increment(3); c.value") == 9);
    CHECK(ub_test::EvalInt(fixture.context, "const d = new Counter(0); d.value = 12; d.value") == 12);
    CHECK(Counter::constructed >= 3);
}

UNIBIND_TEST_CASE(CLASSES, "classes: a class is named, and its instances say so") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    CHECK(ub_test::EvalText(fixture.context, "Counter.name") == "Counter");
    CHECK(ub_test::EvalText(fixture.context, "new Counter(0).constructor.name") == "Counter");
}

UNIBIND_TEST_CASE(CLASSES, "classes: a constructor called without new is refused") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    // The bottom row of the grid in `templates: a function template is a
    // function as well as a constructor`: a class is the one of the three that
    // requires `new`.
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { Counter(1); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
    CHECK(Counter::constructed == 0);

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "Counter(1)").has_value());
    CHECK(tryCatch.HasCaught());
    tryCatch.Reset();
    CHECK(Counter::alive == 0);
}

UNIBIND_TEST_CASE(CLASSES, "classes: a class with no constructor cannot be constructed from script") {
    Counter::Reset();
    ub_test::Fixture fixture;

    // Without Construct<>, instances can only come from Wrap.
    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Sealed");
    cls.Method<&Increment>("increment");
    ub_test::Expose(fixture.context, "Sealed", *cls.GetConstructor(fixture.context));

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "new Sealed()").has_value());
    CHECK(tryCatch.HasCaught());
    tryCatch.Reset();
    CHECK(Counter::constructed == 0);

    const auto wrapped = cls.Wrap(fixture.context, std::make_unique<Counter>(4));
    REQUIRE(wrapped.has_value());
    ub_test::Expose(fixture.context, "made", *wrapped);
    CHECK(ub_test::EvalInt(fixture.context, "made.increment()") == 5);
}

UNIBIND_TEST_CASE(CLASSES, "classes: a constructor that throws makes no instance") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "new Counter(-1)").has_value());
    REQUIRE(tryCatch.HasCaught());
    CHECK(tryCatch.Message(fixture.context).value_or("").find("starts at zero") != std::string::npos);
    tryCatch.Reset();

    CHECK(Counter::alive == 0);
    CHECK(Counter::destroyed == 0);  // nothing was made, so nothing was destroyed
}

UNIBIND_TEST_CASE(CLASSES, "classes: statics live on the constructor, methods on the prototype") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    CHECK(ub_test::EvalText(fixture.context, "Counter.KIND") == "counter");
    CHECK(ub_test::EvalTruth(fixture.context, "typeof Counter.alive === 'function'"));
    CHECK(ub_test::EvalTruth(fixture.context, "typeof new Counter(0).increment === 'function'"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.hasOwn(new Counter(0), 'increment')"));
    CHECK(ub_test::EvalTruth(fixture.context, "new Counter(0) instanceof Counter"));
}

namespace {

void ReadValueOwn(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    if (Counter* self = ub::Class<Counter>::Unwrap(info.This())) {
        info.GetReturnValue().Set(self->value);
    }
}

void WriteValueOwn(const ub::Local<ub::Name>& /*property*/, const ub::Local<ub::Value>& value,
                   const ub::PropertyCallbackInfo& info) {
    Counter* self = ub::Class<Counter>::Unwrap(info.This());
    const auto asInt = value.ToInt32(info.GetContext());
    if (self != nullptr && asInt) {
        self->value = *asInt;
    }
}

}  // namespace

UNIBIND_TEST_CASE(CLASSES, "classes: an instance-template accessor is an own property of every instance") {
    // Script that copies an object by `for...in` + `hasOwnProperty`, or with
    // `Object.keys` / `JSON.stringify`, sees only own properties - an accessor
    // on the prototype is invisible to it. Instances made by `new` and by
    // `Wrap` must both carry it.
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Owned");
    cls.Construct<&MakeCounter>();
    cls.InstanceTemplate().SetAccessor("value", &ReadValueOwn, &WriteValueOwn);
    ub_test::Expose(fixture.context, "Owned", *cls.GetConstructor(fixture.context));
    const auto wrapped = cls.Wrap(fixture.context, std::make_unique<Counter>(7));
    REQUIRE(wrapped.has_value());
    ub_test::Expose(fixture.context, "wrapped", *wrapped);

    for (const char* made : {"new Owned(7)", "wrapped"}) {
        INFO(made);
        // Each check in a scope of its own: they share one context.
        const auto with = [made](const char* body) {
            return std::string("(() => { const o = ") + made + "; " + body + " })()";
        };
        CHECK(ub_test::EvalInt(fixture.context, with("return o.value;")) == 7);
        CHECK(ub_test::EvalTruth(fixture.context, with("return o.hasOwnProperty('value');")));
        CHECK(ub_test::EvalText(fixture.context, with("return Object.keys(o).join();")) == "value");
        CHECK(ub_test::EvalText(fixture.context, with("return JSON.stringify(o);")) == R"({"value":7})");
        CHECK(ub_test::EvalInt(fixture.context, with("const copy = {};"
                                                     " for (const k in o) { if (o.hasOwnProperty(k)) copy[k] = o[k]; }"
                                                     " return copy.value;")) == 7);
        CHECK(ub_test::EvalInt(fixture.context, with("o.value = 12; return o.value;")) == 12);
    }
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.hasOwn(Owned.prototype, 'value')"));
}

UNIBIND_TEST_CASE(CLASSES, "classes: native can wrap an instance without running the constructor") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    const auto wrapped = cls.Wrap(fixture.context, std::make_unique<Counter>(100));
    REQUIRE(wrapped.has_value());
    ub_test::Expose(fixture.context, "made", *wrapped);

    CHECK(ub_test::EvalInt(fixture.context, "made.value") == 100);
    CHECK(ub_test::EvalInt(fixture.context, "made.increment(5)") == 105);
    CHECK(ub_test::EvalTruth(fixture.context, "made instanceof Counter"));
    CHECK(cls.IsInstance(*wrapped));
}

UNIBIND_TEST_CASE(CLASSES, "classes: a constructor's receiver is the instance being made") {
    // `info.This()` inside a constructor is the object `new` is making, and a
    // binding that writes to it - a computed property, a cached shape, an id -
    // expects the write to be there afterwards. A receiver that is a sentinel
    // instead makes that write *vanish*: the callback cannot tell, the
    // constructor reports success, and the property is simply not there. That
    // is the silent loss this case exists to catch, so it asserts from script
    // rather than from C++.
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Stamped");
    cls.Construct<&MakeCounterStamping>();
    cls.Accessor<&ReadValue>("value");
    ub_test::Expose(fixture.context, "Stamped", *cls.GetConstructor(fixture.context));

    CHECK(ub_test::EvalInt(fixture.context, "new Stamped().stamped") == 42);
    CHECK(ub_test::EvalInt(fixture.context, "new Stamped().value") == 1);

    // And the same receiver carries the native the constructor made, so the
    // write landed on the instance rather than on something thrown away.
    CHECK(ub_test::EvalTruth(fixture.context, "const s = new Stamped(); s.stamped === 42 && s.value === 1"));
}

UNIBIND_TEST_CASE(CLASSES, "classes: a script subclass gets its own prototype, and still carries the native") {
    // `class Sub extends Counter` is the ordinary way script extends something
    // an embedder exposed, and what it expects is the language's rule:
    // `new Sub()` makes an object whose prototype is `Sub.prototype`, so the
    // subclass's own methods are there and `instanceof Sub` is true. A backend
    // that builds the instance from the *class's* prototype instead loses
    // every one of those - silently, with the constructor reporting success -
    // while `instanceof Counter` keeps answering true, which is what makes it
    // so easy to miss.
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    REQUIRE(
        ub::Evaluate(fixture.context, "class Sub extends Counter { twice() { return this.value * 2; } }").has_value());
    const auto made = ub::Evaluate(fixture.context, "new Sub(21)");
    REQUIRE(made.has_value());

    CHECK(ub_test::EvalTruth(fixture.context, "new Sub(1) instanceof Sub"));
    CHECK(ub_test::EvalTruth(fixture.context, "new Sub(1) instanceof Counter"));
    CHECK(ub_test::EvalTruth(fixture.context, "Object.getPrototypeOf(new Sub(1)) === Sub.prototype"));
    CHECK(ub_test::EvalText(fixture.context, "new Sub(1).constructor.name") == "Sub");

    // The subclass's own method, the inherited accessor and the native
    // underneath all answer.
    CHECK(ub_test::EvalInt(fixture.context, "new Sub(21).twice()") == 42);
    CHECK(ub_test::EvalInt(fixture.context, "new Sub(5).increment(2)") == 7);
    CHECK(cls.IsInstance(*made));
    CHECK(ub::Class<Counter>::Unwrap(*made)->value == 21);
}

UNIBIND_TEST_CASE(CLASSES, "classes: the native is recovered as its real type or not at all") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    const auto wrapped = cls.Wrap(fixture.context, std::make_unique<Counter>(3));
    REQUIRE(wrapped.has_value());

    Counter* native = ub::Class<Counter>::Unwrap(*wrapped);
    REQUIRE(native != nullptr);
    CHECK(native->value == 3);

    // A different native type on the same object is not a cast, it is nothing.
    CHECK(ub::Class<Unrelated>::Unwrap(*wrapped) == nullptr);

    // Nor is an instance of a different class carrying a different native.
    const auto other = ub::Class<Unrelated>::New(fixture.iso(), "Unrelated");
    const auto otherInstance = other.Wrap(fixture.context, std::make_unique<Unrelated>());
    REQUIRE(otherInstance.has_value());
    CHECK(ub::Class<Counter>::Unwrap(*otherInstance) == nullptr);
    CHECK_FALSE(cls.IsInstance(*otherInstance));
    CHECK(other.IsInstance(*otherInstance));

    // And a value that is not an instance at all yields nothing either.
    const auto stranger = ub_test::Eval(fixture.context, "({})");
    CHECK(ub::Class<Counter>::Unwrap(stranger) == nullptr);
    CHECK_FALSE(cls.IsInstance(stranger));
}

UNIBIND_TEST_CASE(CLASSES, "classes: a method called on the wrong receiver throws rather than unwrapping") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            const stolen = Counter.prototype.increment;
            try { stolen.call({}); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
}

UNIBIND_TEST_CASE(CLASSES, "classes: a prototype swap does not fool the native check") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    const auto impostor = ub_test::Eval(fixture.context, "Object.create(Counter.prototype)");
    // `instanceof` believes it; asking what native it carries does not.
    CHECK(ub_test::EvalTruth(fixture.context, "Object.create(Counter.prototype) instanceof Counter"));
    CHECK_FALSE(cls.IsInstance(impostor));
    CHECK(ub::Class<Counter>::Unwrap(impostor) == nullptr);
}

UNIBIND_TEST_CASE(CLASSES, "classes: one class declaration serves several realms") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));
    CHECK(ub_test::EvalInt(fixture.context, "new Counter(1).value") == 1);

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    ub::ContextScope entered(*second);
    const auto there = cls.GetConstructor(*second);
    REQUIRE(there.has_value());
    REQUIRE(second->GlobalObject().Set(*second, "Counter", *there).value_or(false));
    CHECK(ub_test::EvalInt(*second, "new Counter(2).value") == 2);

    // The native check crosses realms even though `instanceof` does not.
    const auto fromSecond = ub_test::Eval(*second, "new Counter(3)");
    CHECK(cls.IsInstance(fromSecond));
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "classes: an interceptor can cover every property of every instance") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Sparse");
    cls.Construct<&MakeCounter>();
    cls.Method<&Increment>("increment");
    cls.SetHandler(ub::NamedPropertyHandler{.getter = &SparseGetter});

    ub_test::Expose(fixture.context, "Sparse", *cls.GetConstructor(fixture.context));

    // The handler answers for a name nothing declared, declines for everything
    // else, and the class's own prototype method still works.
    CHECK(ub_test::EvalText(fixture.context, "new Sparse(0).invented") == "from the interceptor");
    CHECK(ub_test::Eval(fixture.context, "new Sparse(0).nothingLikeIt").Kind() == ub::ValueKind::Undefined);
    CHECK(ub_test::EvalInt(fixture.context, "new Sparse(4).increment()") == 5);

    // And the native is still where it was: an interceptor does not displace it.
    const auto instance = ub_test::Eval(fixture.context, "new Sparse(8)");
    Counter* native = ub::Class<Counter>::Unwrap(instance);
    REQUIRE(native != nullptr);
    CHECK(native->value == 8);
}

UNIBIND_TEST_CASE2(CLASSES, SYMBOL_METHODS, "classes: a class method under Symbol.iterator makes instances iterable") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Countdown");
    cls.Construct<&MakeCounter>();
    cls.SymbolMethod<&IterateCounter>(ub::WellKnownSymbol::Iterator);

    ub_test::Expose(fixture.context, "Countdown", *cls.GetConstructor(fixture.context));
    CHECK(ub_test::EvalText(fixture.context, "[...new Countdown(3)].join(',')") == "0,1,2");
}

UNIBIND_TEST_CASE2(CLASSES, SYMBOL_METHODS,
                   "classes: members can be run-time callbacks with data, on the same prototype") {
    Counter::Reset();
    ub_test::Fixture fixture;

    CallCounter method;
    CallCounter accessor;
    CallCounter iterator;
    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Hooked");
    cls.Construct<&MakeCounter>();
    cls.Method("increment", &CountedIncrement, ub::CallbackData::For(method));
    cls.Accessor("value", &CountedRead, &CountedWrite, ub::CallbackData::For(accessor));
    cls.SymbolMethod(ub::WellKnownSymbol::Iterator, &CountedIterate, ub::CallbackData::For(iterator));
    ub_test::Expose(fixture.context, "Hooked", *cls.GetConstructor(fixture.context));

    CHECK(ub_test::EvalInt(fixture.context,
                           "const h = new Hooked(2); h.increment(); h.value = h.value + 10; h.value") == 13);
    CHECK(ub_test::EvalText(fixture.context, "[...new Hooked(3)].join(',')") == "0,1,2");
    CHECK(method.calls == 1);
    CHECK(accessor.calls == 3);
    CHECK(iterator.calls == 1);

    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.hasOwn(new Hooked(0), 'increment')"));
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { Hooked.prototype.increment.call({}); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
}

UNIBIND_TEST_CASE(CLASSES, "classes: a constructor can hand back a share, and its deleter is the one that runs") {
    Counter::Reset();
    g_releasedBySharedDeleter = 0;
    {
        ub_test::Fixture fixture;
        const auto cls = ub::Class<Counter>::New(fixture.iso(), "SharedCounter");
        cls.Construct<&MakeSharedCounter>();
        cls.Method<&Increment>("increment");
        ub_test::Expose(fixture.context, "SharedCounter", *cls.GetConstructor(fixture.context));

        CHECK(ub_test::EvalInt(fixture.context, "new SharedCounter(3).increment()") == 4);
        CHECK(ub_test::EvalTruth(fixture.context, "new SharedCounter(1) instanceof SharedCounter"));
        CHECK(Counter::constructed == 2);
    }
    // The isolate is gone, so every share the engine held has been given back
    // - through the constructor's own deleter, not a default one.
    CHECK(g_releasedBySharedDeleter == 2);
    CHECK(Counter::alive == 0);
}

UNIBIND_TEST_CASE2(CLASSES, OBJECT_ACCESSORS, "classes: one instance can carry an accessor its class does not") {
    Counter::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareCounter(fixture.iso());
    ub_test::Expose(fixture.context, "Counter", *cls.GetConstructor(fixture.context));

    std::int32_t extra = 99;
    auto special = cls.Wrap(fixture.context, std::make_shared<Counter>(4));
    REQUIRE(special.has_value());
    CHECK(special->SetAccessor(fixture.context, "extra", &ReadExtra, nullptr, ub::CallbackData::For(extra)) ==
          std::optional<bool>(true));
    ub_test::Expose(fixture.context, "special", *special);

    // Its own member, and every member of its class, still there.
    CHECK(ub_test::EvalInt(fixture.context, "special.extra") == 99);
    CHECK(ub_test::EvalInt(fixture.context, "special.increment(); special.value") == 5);
    CHECK(ub_test::EvalTruth(fixture.context, "special instanceof Counter"));
    CHECK(ub::Class<Counter>::Unwrap(*special) != nullptr);
    // And no other instance has it.
    CHECK(ub_test::EvalTruth(fixture.context, "new Counter(1).extra === undefined"));
}

// ---------------------------------------------------------------------------
// Finalizers. The invariant these two hold both engines to is the one a backend
// works for: **the engine gives back every share it took, exactly once, by the
// time its isolate is gone.** Every native here is owned by the engine alone,
// so giving back the share is destroying the native - and that is why these
// cases can say so. Where the embedder holds a share too, the native outlives
// the isolate on purpose, which is `cases/ownership_test.cpp`.
//
// *When* inside that window is the collector's business, and the two engines
// answer differently, so that is reported and not asserted.
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(CLASSES, "classes: the engine gives back every share it took by the time the isolate is gone") {
    Counter::Reset();
    constexpr int MADE = 64;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareCounter(*isolate);

        // Garbage: nothing refers to these once the inner scope closes.
        {
            ub::HandleScope inner(*isolate);
            for (int i = 0; i < MADE / 2; ++i) {
                REQUIRE(cls.Wrap(*context, std::make_unique<Counter>(i)).has_value());
            }
        }

        // Survivors: held by a root that is still alive at teardown.
        std::vector<ub::Global<ub::Object>> kept;
        kept.reserve(MADE / 2);
        for (int i = 0; i < MADE / 2; ++i) {
            auto instance = cls.Wrap(*context, std::make_unique<Counter>(i));
            REQUIRE(instance.has_value());
            kept.emplace_back(*isolate, *instance);
        }

        REQUIRE(Counter::constructed == MADE);

        for (int round = 0; round < 4; ++round) {
            isolate->RequestGarbageCollection();
        }
        MESSAGE("of ", MADE, " natives, ", Counter::destroyed, " were destroyed while the isolate was alive");
        CHECK(Counter::destroyed <= MADE / 2);  // nothing rooted may be finalised
        CHECK(Counter::alive >= MADE / 2);
    }

    // The isolate is gone. Whatever the collector did or did not get to, the
    // natives are gone with it - the guarantee that makes this test writable on
    // two engines at all (docs/status.md).
    CHECK(Counter::destroyed == MADE);
    CHECK(Counter::alive == 0);
}

UNIBIND_TEST_CASE(CLASSES, "classes: dropping every reference eventually destroys the native") {
    Counter::Reset();

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareCounter(*isolate);

        {
            ub::HandleScope inner(*isolate);
            for (int i = 0; i < 256; ++i) {
                REQUIRE(cls.Wrap(*context, std::make_unique<Counter>(i)).has_value());
            }
        }

        for (int round = 0; round < 8 && Counter::destroyed == 0; ++round) {
            {
                ub::HandleScope churn(*isolate);
                for (int i = 0; i < 4000; ++i) {
                    (void)ub::Object::New(*context);
                }
            }
            isolate->RequestGarbageCollection();
        }

        // Neither engine promises to collect on request, so the only thing that
        // can be asserted here is that nothing was destroyed twice.
        CHECK(Counter::destroyed + Counter::alive == 256);
        MESSAGE("the collector destroyed ", Counter::destroyed, " of 256 unreachable natives on request");
    }

    CHECK(Counter::destroyed == 256);
    CHECK(Counter::alive == 0);
}

// ---------------------------------------------------------------------------
// The call/construct grid, completed.
//
// `templates: a function template is a function as well as a constructor` pins
// the first two rows; these are the two a `Class<T>` can be:
//
//   | made by                     | f()       | new f()  |
//   | Function::New               | yes       | TypeError|
//   | FunctionTemplate            | yes       | yes      |
//   | Class<T>::Construct         | TypeError | yes      |
//   | Class<T>::ConstructOrCall   | yes       | yes      |
//
// The fourth row exists for the shape `Error` has, where both spellings mean
// the same thing. What makes it safe is that a plain call yields an *instance*
// - so whatever comes out of a `Class<T>` still carries a `T`.
// ---------------------------------------------------------------------------

namespace {

/// Remembers how it was called, so the grid can assert that the callback can
/// tell the two apart rather than assuming it.
struct CallShape {
    inline static int calls = 0;
    inline static bool lastWasConstruct = false;

    static void Reset() {
        calls = 0;
        lastWasConstruct = false;
    }
};

std::unique_ptr<Counter> MakeNotingHowItWasCalled(const ub::CallbackInfo& info) {
    ++CallShape::calls;
    CallShape::lastWasConstruct = info.IsConstructCall();
    return MakeCounter(info);
}

}  // namespace

UNIBIND_TEST_CASE(CALLABLE_CLASS, "classes: a class that opts in answers to a plain call as well as to new") {
    Counter::Reset();
    CallShape::Reset();
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Counter>::New(fixture.iso(), "Both");
    cls.ConstructOrCall<&MakeNotingHowItWasCalled>();
    cls.Accessor<&ReadValue, &WriteValue>("value");
    ub_test::Expose(fixture.context, "Both", *cls.GetConstructor(fixture.context));

    // `new` is unchanged.
    CHECK(ub_test::EvalInt(fixture.context, "new Both(5).value") == 5);
    CHECK(CallShape::lastWasConstruct);

    // And a plain call makes one too - the same instance shape, not something
    // else that happens to answer.
    CHECK(ub_test::EvalInt(fixture.context, "Both(7).value") == 7);
    CHECK_FALSE(CallShape::lastWasConstruct);
    CHECK(ub_test::EvalTruth(fixture.context, "Both(1) instanceof Both"));

    const auto made = ub_test::Eval(fixture.context, "Both(9)");
    CHECK(cls.IsInstance(made));
    Counter* native = ub::Class<Counter>::Unwrap(made);
    REQUIRE(native != nullptr);
    CHECK(native->value == 9);

    CHECK(CallShape::calls == 4);
}

UNIBIND_TEST_CASE(CALLABLE_CLASS, "classes: a class that did not opt in still refuses a plain call") {
    // The default, and it stays the default: a class that answered a plain call
    // by accident would hand script something nobody asked it to make.
    Counter::Reset();
    CallShape::Reset();
    ub_test::Fixture fixture;

    const auto strict = ub::Class<Counter>::New(fixture.iso(), "Strict");
    strict.Construct<&MakeNotingHowItWasCalled>();
    strict.Accessor<&ReadValue, &WriteValue>("value");
    ub_test::Expose(fixture.context, "Strict", *strict.GetConstructor(fixture.context));

    CHECK(ub_test::EvalInt(fixture.context, "new Strict(3).value") == 3);
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { Strict(3); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
    // The refusal happens before the callback, so nothing was made and nothing
    // has to be cleaned up.
    CHECK(CallShape::calls == 1);
    CHECK(Counter::alive == 1);
}
