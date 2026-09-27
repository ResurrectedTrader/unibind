// Functions: `Function::New`, the call trampoline, receivers, results, errors,
// and calling from native into Python and back.

#include <array>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

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

ub::Local<ub::Function> NewFunction(const ub::Context& context, ub::FunctionCallback callback,
                                    ub::CallbackData data = {}) {
    auto function = ub::Function::New(context, callback, data);
    REQUIRE(function.has_value());
    return *function;
}

void Sum(const ub::CallbackInfo& info) {
    double total = 0;
    for (std::uint32_t i = 0; i < info.Length(); ++i) {
        const auto number = info[i].ToNumber(info.GetContext());
        if (!number) {
            return;
        }
        total += *number;
    }
    info.GetReturnValue().Set(total);
}

/// Describes its arguments: how many, and what the one past the end reads as.
void Describe(const ub::CallbackInfo& info) {
    const std::string text = std::to_string(info.Length()) + (info[info.Length()].IsUndefined() ? ":undefined" : ":?") +
                             (info.IsConstructCall() ? ":construct" : ":call");
    if (!info.GetReturnValue().Set(text)) {
        info.ThrowTypeError("no string");
    }
}

/// Answers with its receiver.
void Receiver(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(info.This());
}

/// Answers in the way its first argument names.
void AnswerAs(const ub::CallbackInfo& info) {
    const auto how = info[0].To<ub::String>();
    const std::string kind = how ? how->Utf8Value() : "";
    const ub::ReturnValue result = info.GetReturnValue();
    if (kind == "int") {
        result.Set(std::int32_t{-7});
    } else if (kind == "uint") {
        result.Set(std::uint32_t{4000000000U});
    } else if (kind == "double") {
        result.Set(2.5);
    } else if (kind == "integral double") {
        result.Set(3.0);
    } else if (kind == "negative zero") {
        result.Set(-0.0);
    } else if (kind == "bool") {
        result.Set(true);
    } else if (kind == "null") {
        result.SetNull();
    } else if (kind == "undefined") {
        result.Set(std::int32_t{1});
        result.SetUndefined();
    } else if (kind == "string") {
        (void)result.Set("text");
    } else if (kind == "lossy") {
        (void)result.Set(
            std::string_view("a\xff"
                             "b"));
    } else if (kind == "argument") {
        result.Set(info[1]);
    }
    // anything else: nothing written, which is undefined
}

struct Tally {
    int calls = 0;
};

struct Other {
    int unused = 0;
};

void CountCalls(const ub::CallbackInfo& info) {
    if (auto* tally = info.Data<Tally>()) {
        ++tally->calls;
        info.GetReturnValue().Set(tally->calls);
        return;
    }
    info.ThrowTypeError("no tally");
}

void ReadOther(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(info.Data<Other>() == nullptr);
}

void Throws(const ub::CallbackInfo& info) {
    info.Throw(ub::ErrorKind::RangeError, "out of range, natively");
}

void ThrowsCpp(const ub::CallbackInfo& info) {
    if (info.Length() > 0) {
        throw std::bad_alloc();
    }
    throw std::runtime_error("escaped");
}

/// Calls its first argument - a Python function - with 20 and 22, and answers
/// with what it answered.
void CallsBack(const ub::CallbackInfo& info) {
    const auto function = info[0].To<ub::Function>();
    if (!function) {
        info.ThrowTypeError("not a function");
        return;
    }
    const std::array<ub::Local<ub::Value>, 2> arguments{ub::Integer::New(info.GetIsolate(), 20),
                                                        ub::Integer::New(info.GetIsolate(), 22)};
    auto result = function->Call(info.GetContext(), ub::Undefined(info.GetIsolate()), arguments);
    if (result) {
        info.GetReturnValue().Set(*result);
    }
}

/// Calls its argument inside a TryCatch of its own and answers with the
/// message it caught - the handler at the callback's depth owns what the
/// script it called threw.
void CatchesScript(const ub::CallbackInfo& info) {
    const auto function = info[0].To<ub::Function>();
    if (!function) {
        return;
    }
    ub::TryCatch handler(info.GetIsolate());
    const auto result = function->Call(info.GetContext(), ub::Undefined(info.GetIsolate()));
    if (result || !handler.HasCaught()) {
        (void)info.GetReturnValue().Set("not caught");
        return;
    }
    (void)info.GetReturnValue().Set(handler.Message(info.GetContext()).value_or("?"));
}

/// Answers with the value it was made with.
void ValueData(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(info.Data());
}

/// Makes a fresh native function on every call, as a factory binding would.
void MakesFunctions(const ub::CallbackInfo& info) {
    auto made = ub::Function::New(info.GetContext(), &Sum);
    if (made) {
        info.GetReturnValue().Set(*made);
    }
}

/// Reads `marker` off the global object of the realm it was called in.
void ReadsItsRealm(const ub::CallbackInfo& info) {
    const auto marker = info.GetContext().GlobalObject().Get(info.GetContext(), "marker");
    if (marker) {
        info.GetReturnValue().Set(*marker);
    }
}

}  // namespace

TEST_CASE("functions: script calls native, and the arguments are borrowed as they are") {
    Fixture f;
    Expose(f.context, "add", NewFunction(f.context, &Sum));
    Expose(f.context, "describe", NewFunction(f.context, &Describe));

    CHECK(EvalInt(f.context, "add(1, 2, 3)") == 6);
    CHECK(EvalText(f.context, "repr(add(0.5, 0.25))") == "0.75");
    CHECK(EvalText(f.context, "describe()") == "0:undefined:call");
    CHECK(EvalText(f.context, "describe(1, 'two', None)") == "3:undefined:call");
    CHECK(EvalText(f.context, "describe(*range(40))") == "40:undefined:call");
    // JavaScript has no keyword arguments.
    CHECK(EvalError(f.context, "add(x=1)") == "TypeError: a native function takes no keyword arguments");
}

TEST_CASE("functions: every way of answering a call reaches Python as the value it names") {
    Fixture f;
    Expose(f.context, "answer", NewFunction(f.context, &AnswerAs));
    CHECK(EvalTruth(f.context, "answer('int') == -7 and type(answer('int')) is int"));
    CHECK(EvalTruth(f.context, "answer('uint') == 4000000000"));
    CHECK(EvalTruth(f.context, "answer('double') == 2.5"));
    // An integral Number is an int, so script can index and range with it.
    CHECK(EvalTruth(f.context, "type(answer('integral double')) is int"));
    CHECK(EvalText(f.context, "repr(answer('negative zero'))") == "-0.0");
    CHECK(EvalTruth(f.context, "answer('bool') is True"));
    CHECK(EvalTruth(f.context, "answer('null') is unibind.null"));
    CHECK(EvalTruth(f.context, "answer('undefined') is None"));
    CHECK(EvalTruth(f.context, "answer('nothing') is None"));
    CHECK(EvalText(f.context, "answer('string')") == "text");
    CHECK(EvalText(f.context, "answer('lossy')") ==
          "a\xEF\xBF\xBD"
          "b");
    CHECK(EvalTruth(f.context, "l = [1]\nanswer('argument', l) is l"));
}

TEST_CASE("functions: This is the receiver - the object a method was read from, or the realm's globals") {
    Fixture f;
    const auto receiver = NewFunction(f.context, &Receiver);
    Expose(f.context, "whoAmI", receiver);

    // A plain call: the global object, which is the globals dict.
    CHECK(EvalTruth(f.context, "whoAmI() is globals()"));
    // Read as an attribute of an object: bound to it.
    CHECK(EvalTruth(f.context, "o = unibind.Object(); o.m = whoAmI\no.m() is o"));
    CHECK(EvalTruth(f.context, "m = o.m\nm() is o and m.__self__ is o and m.__func__ is whoAmI and o.m == o.m"));
    // Read by subscript it is the value stored, unbound, as a dict's item is.
    CHECK(EvalTruth(f.context, "o['m']() is globals()"));
    // On a Python class it is a method, as a function on a prototype is.
    CHECK(EvalTruth(f.context, "class K:\n    me = whoAmI\nk = K()\nk.me() is k and K.me() is globals()"));

    // From native: the receiver passed, and undefined or null mean the global.
    auto object = ub::Object::New(f.context);
    REQUIRE(object.has_value());
    auto called = receiver.Call(f.context, *object);
    REQUIRE(called.has_value());
    CHECK(called->StrictEquals(*object));
    called = receiver.Call(f.context, ub::Undefined(f.iso()));
    REQUIRE(called.has_value());
    CHECK(called->StrictEquals(f.context.GlobalObject()));
    called = receiver.Call(f.context, ub::Null(f.iso()));
    REQUIRE(called.has_value());
    CHECK(called->StrictEquals(f.context.GlobalObject()));
    // A bound function keeps its own receiver, as a JavaScript bound one does.
    const auto bound = Eval(f.context, "o.m").To<ub::Function>();
    REQUIRE(bound.has_value());
    called = bound->Call(f.context, f.context.GlobalObject());
    REQUIRE(called.has_value());
    CHECK(called->StrictEquals(Eval(f.context, "o")));
}

TEST_CASE("functions: embedder data is typed, and the wrong type reads as nothing") {
    Fixture f;
    Tally tally;
    Expose(f.context, "count", NewFunction(f.context, &CountCalls, ub::CallbackData::For(tally)));
    Expose(f.context, "other", NewFunction(f.context, &ReadOther, ub::CallbackData::For(tally)));
    Expose(f.context, "none", NewFunction(f.context, &CountCalls));

    CHECK(EvalInt(f.context, "count(); count()") == 2);
    CHECK(tally.calls == 2);
    CHECK(EvalTruth(f.context, "other()"));
    CHECK(EvalError(f.context, "none()") == "TypeError: no tally");
}

TEST_CASE("functions: a native function is callable and not a constructor") {
    Fixture f;
    const auto function = NewFunction(f.context, &Describe);
    Expose(f.context, "f", function);

    CHECK(EvalTruth(f.context, "callable(f) and isinstance(f, unibind.NativeFunction)"));
    CHECK(function.Call(f.context, ub::Undefined(f.iso())).has_value());
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(function.NewInstance(f.context).has_value());
    REQUIRE(handler.HasCaught());
    CHECK(handler.Message(f.context).value_or("").starts_with("TypeError"));
    handler.Reset();
    // Nor can Python make one.
    CHECK(EvalError(f.context, "unibind.NativeFunction()").starts_with("TypeError"));
    CHECK(function.Kind() == ub::ValueKind::Function);
}

TEST_CASE("functions: a native function has a repr and an empty name") {
    Fixture f;
    Expose(f.context, "f", NewFunction(f.context, &Describe));
    CHECK(EvalText(f.context, "repr(f)") == "<native function>");
    CHECK(EvalText(f.context, "f.__name__") == "");
    CHECK(EvalTruth(f.context, "o = unibind.Object(); o.f = f\nrepr(o.f).startswith('<bound native function')"));
}

TEST_CASE("functions: native calls a function script wrote, and constructs a class script wrote") {
    Fixture f;
    const auto add = Eval(f.context, "def add(a, b):\n    return a + b\nadd").To<ub::Function>();
    REQUIRE(add.has_value());
    const std::array<ub::Local<ub::Value>, 2> arguments{ub::Integer::New(f.iso(), 40), ub::Integer::New(f.iso(), 2)};
    const auto sum = add->Call(f.context, ub::Undefined(f.iso()), arguments);
    REQUIRE(sum.has_value());
    CHECK(sum->To<ub::Integer>()->Int32Value() == 42);

    const auto point =
        Eval(f.context, "class Point:\n    def __init__(self, x, y):\n        self.x = x\nPoint").To<ub::Function>();
    REQUIRE(point.has_value());
    const auto made = point->NewInstance(f.context, arguments);
    REQUIRE(made.has_value());
    CHECK(made->Get(f.context, "x")->To<ub::Integer>()->Int32Value() == 40);
    CHECK(EvalTruth(f.context, "True"));

    // A Python function has no `new`: it is not a constructor.
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(add->NewInstance(f.context, arguments).has_value());
    CHECK(handler.HasCaught());
}

TEST_CASE("functions: a native throw is a Python exception, catchable in script") {
    Fixture f;
    Expose(f.context, "throws", NewFunction(f.context, &Throws));
    CHECK(EvalError(f.context, "throws()") == "unibind.RangeError: out of range, natively");
    CHECK(EvalText(f.context,
                   "try:\n    throws()\nexcept ValueError as e:\n    r = type(e).__name__ + ': ' + str(e)\nr") ==
          "RangeError: out of range, natively");
    CHECK_FALSE(f.iso().HasPendingException());
    CHECK(EvalInt(f.context, "1 + 1") == 2);
}

TEST_CASE("functions: a C++ exception does not unwind through the interpreter") {
    Fixture f;
    Expose(f.context, "throwsCpp", NewFunction(f.context, &ThrowsCpp));
    CHECK(EvalError(f.context, "throwsCpp()").starts_with("SystemError"));
    CHECK(EvalError(f.context, "throwsCpp(1)").starts_with("MemoryError"));
    CHECK(EvalInt(f.context, "2 + 2") == 4);
}

TEST_CASE("functions: native into script into native, with each handler owning its own depth") {
    Fixture f;
    Expose(f.context, "callsBack", NewFunction(f.context, &CallsBack));
    Expose(f.context, "catches", NewFunction(f.context, &CatchesScript));
    Expose(f.context, "add", NewFunction(f.context, &Sum));

    CHECK(EvalInt(f.context, "callsBack(lambda a, b: add(a, b))") == 42);
    CHECK(EvalInt(f.context, "callsBack(lambda a, b: callsBack(lambda c, d: a + b + c + d))") == 84);
    // An exception from the script the native called is the native's to catch...
    CHECK(EvalText(f.context, "def bad():\n    raise KeyError('k')\ncatches(bad)") == "KeyError: 'k'");
    // ...and one it does not catch goes back through it to the script.
    CHECK(EvalText(f.context, "try:\n    callsBack(lambda a, b: bad())\nexcept KeyError:\n    r = 'through'\nr") ==
          "through");
    CHECK_FALSE(f.iso().HasPendingException());
}

TEST_CASE("functions: one callback behind many functions, each with its own value") {
    Fixture f;
    for (const char* name : {"alpha", "beta"}) {
        const auto value = Str(f.iso(), name);
        auto function = ub::Function::New(f.context, &ValueData, value);
        REQUIRE(function.has_value());
        Expose(f.context, name, *function);
    }
    auto plain = NewFunction(f.context, &ValueData);
    Expose(f.context, "plain", plain);
    CHECK(EvalText(f.context, "alpha() + ',' + beta()") == "alpha,beta");
    // Made without a value, it reads undefined.
    CHECK(EvalTruth(f.context, "plain() is None"));

    // A value of any kind comes back as itself.
    const auto list = Eval(f.context, "shared = [1, 2]\nshared");
    auto holding = ub::Function::New(f.context, &ValueData, list);
    REQUIRE(holding.has_value());
    Expose(f.context, "holding", *holding);
    CHECK(EvalTruth(f.context, "holding() is shared"));
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(holding->NewInstance(f.context).has_value());
    CHECK(handler.HasCaught());
}

TEST_CASE("functions: the value lives exactly as long as the function, cycles included") {
    Fixture f;
    REQUIRE(ub::Evaluate(f.context, "import gc, weakref\nclass Box:\n    pass\nbox = Box()\nw = weakref.ref(box)")
                .has_value());
    {
        const ub::HandleScope scope(f.iso());
        const auto box = Eval(f.context, "box");
        auto function = ub::Function::New(f.context, &ValueData, box);
        REQUIRE(function.has_value());
        Expose(f.context, "holder", *function);
    }
    // Held through the function alone.
    CHECK(EvalTruth(f.context, "del box\ngc.collect()\nw() is not None and holder() is w()"));
    // A value that refers back to its function does not keep the pair alive.
    CHECK(
        EvalTruth(f.context,
                  "w().fn = holder\nfw = weakref.ref(holder)\ndel holder\ngc.collect()\nw() is None and fw() is None"));

    // And a Global over the value outlives the function, not the other way round.
    ub::Global<ub::Value> kept;
    {
        const ub::HandleScope scope(f.iso());
        const auto value = Eval(f.context, "Box()");
        kept = ub::Global<ub::Value>(f.iso(), value);
        auto function = ub::Function::New(f.context, &ValueData, value);
        REQUIRE(function.has_value());
        Expose(f.context, "second", *function);
    }
    CHECK(EvalTruth(f.context, "del second\ngc.collect()\nTrue"));
    const auto still = kept.Get(f.iso());
    CHECK(still.IsObject());
    kept.Reset();
}

TEST_CASE("functions: a native called from a realm whose Context was released still has its realm") {
    Fixture f;
    ub::Global<ub::Value> caller;
    {
        auto other = ub::Context::New(f.iso());
        REQUIRE(other.has_value());
        const ub::ContextScope entered(*other);
        Expose(*other, "readsRealm", NewFunction(*other, &ReadsItsRealm));
        REQUIRE(other->GlobalObject().Set(*other, "marker", Str(f.iso(), "the other realm")).value_or(false));
        caller = ub::Global<ub::Value>(f.iso(), Eval(*other, "def viaScript():\n    return readsRealm()\nviaScript"));
    }
    // The Context is gone; the function script wrote there still holds its
    // globals, and a native it calls finds that realm through them.
    REQUIRE(f.context.GlobalObject().Set(f.context, "marker", Str(f.iso(), "the first realm")).value_or(false));
    const auto function = caller.Get(f.iso()).To<ub::Function>();
    REQUIRE(function.has_value());
    const auto result = function->Call(f.context, ub::Undefined(f.iso()));
    REQUIRE(result.has_value());
    CHECK(py_test::TextOf(f.context, *result) == "the other realm");
    // Called from the first realm directly, it is the first realm's.
    Expose(f.context, "readsRealm", NewFunction(f.context, &ReadsItsRealm));
    CHECK(EvalText(f.context, "readsRealm()") == "the first realm");
    caller.Reset();
}

TEST_CASE("functions: a native function made on every call is freed with its last reference") {
    Fixture f;
    Expose(f.context, "make", NewFunction(f.context, &MakesFunctions));
    CHECK(EvalTruth(f.context, R"(
import weakref
made = make()
w = weakref.ref(made)
ok = made(1, 2) == 3
del made
ok and w() is None
)"));
    // Many of them, and the isolate is none the worse.
    CHECK(EvalInt(f.context, "sum(make()(i, 1) for i in range(1000))") == 500500);
}
