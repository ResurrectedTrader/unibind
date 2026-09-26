// Templates: object templates stamping `unibind.Object`s, function templates
// materialising into a type per realm, and the accessors and methods they
// declare.

#include <array>
#include <cstdint>
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

struct Cell {
    std::int32_t value = 0;
    int reads = 0;
    int writes = 0;
};

void ReadCell(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    auto* cell = info.Data<Cell>();
    if (cell == nullptr) {
        info.ThrowTypeError("accessor data missing");
        return;
    }
    ++cell->reads;
    info.GetReturnValue().Set(cell->value);
}

void WriteCell(const ub::Local<ub::Name>& /*property*/, const ub::Local<ub::Value>& value,
               const ub::PropertyCallbackInfo& info) {
    auto* cell = info.Data<Cell>();
    if (cell == nullptr) {
        info.ThrowTypeError("accessor data missing");
        return;
    }
    ++cell->writes;
    if (const auto asInt = value.ToInt32(info.GetContext())) {
        cell->value = *asInt;
    }
}

struct MakerLog {
    int calls = 0;
    int constructCalls = 0;
};

void Maker(const ub::CallbackInfo& info) {
    if (auto* log = info.Data<MakerLog>()) {
        ++log->calls;
        log->constructCalls += info.IsConstructCall() ? 1 : 0;
    }
    if (info.Length() == 0) {
        return;
    }
    const auto given = info[0].ToInt32(info.GetContext());
    if (!given) {
        return;
    }
    (void)info.This().Set(info.GetContext(), "made", ub::Integer::New(info.GetIsolate(), *given));
}

void EitherWay(const ub::CallbackInfo& info) {
    if (auto* log = info.Data<MakerLog>()) {
        ++log->calls;
        log->constructCalls += info.IsConstructCall() ? 1 : 0;
    }
    if (!info.IsConstructCall()) {
        (void)info.GetReturnValue().Set("called");
        return;
    }
    (void)info.This().Set(info.GetContext(), "how", Str(info.GetIsolate(), "constructed"));
}

/// A constructor that answers with an object of its own, which replaces the
/// instance - and one that answers with a primitive, which does not.
void Replaces(const ub::CallbackInfo& info) {
    if (info.Length() > 0) {
        info.GetReturnValue().Set(info[0]);
    }
}

void SaysHello(const ub::CallbackInfo& info) {
    (void)info.GetReturnValue().Set("hello");
}

void SaysWhoItIs(const ub::CallbackInfo& info) {
    if (auto name = info.This().Get(info.GetContext(), "name")) {
        info.GetReturnValue().Set(*name);
    }
}

/// Answers with `this.name` and whether the holder is the receiver.
void ReadReceiverName(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    auto name = info.This().Get(info.GetContext(), "name");
    if (!name) {
        return;
    }
    const std::string text =
        py_test::TextOf(info.GetContext(), *name) + (info.Holder().StrictEquals(info.This()) ? "/self" : "/inherited");
    (void)info.GetReturnValue().Set(text);
}

/// Answers with a JavaScript-style iterator over 0..n-1, n being `this.size`:
/// an object with a `next` method answering `{done, value}`, as a binding
/// written for JavaScript would.
void JsStyleIterator(const ub::CallbackInfo& info) {
    auto maker = ub::Evaluate(info.GetContext(), R"(
def _make_iterator(n):
    state = {'i': 0}
    def next():
        i = state['i']
        state['i'] = i + 1
        return {'done': i >= n, 'value': i}
    return unibind.Object(next=next)
_make_iterator
)");
    if (!maker) {
        return;
    }
    const auto size = info.This().Get(info.GetContext(), "size");
    if (!size) {
        return;
    }
    const std::array<ub::Local<ub::Value>, 1> arguments{*size};
    const auto function = maker->To<ub::Function>();
    if (!function) {
        return;
    }
    if (auto iterator = function->Call(info.GetContext(), info.This(), arguments)) {
        info.GetReturnValue().Set(*iterator);
    }
}

}  // namespace

TEST_CASE("templates: an object template installs its constants, and ReadOnly holds") {
    Fixture f;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.Set("flag", ub::Constant(true));
    shape.Set("count", ub::Constant(std::int32_t{7}));
    shape.Set("ratio", ub::Constant(0.5));
    shape.Set("whole", ub::Constant(2.0));
    shape.Set("label", ub::Constant(std::string_view("named")));
    shape.Set("nothing", ub::Constant::Null());
    shape.Set("missing", ub::Constant());
    shape.Set("frozen", ub::Constant(std::int32_t{1}), ub::PropertyAttribute::ReadOnly);

    const auto instance = shape.NewInstance(f.context);
    REQUIRE(instance.has_value());
    Expose(f.context, "shaped", *instance);

    CHECK(EvalTruth(f.context, "shaped.flag is True and shaped.count == 7 and shaped.ratio == 0.5"));
    CHECK(EvalTruth(f.context, "type(shaped.whole) is int and shaped.label == 'named'"));
    CHECK(EvalTruth(f.context, "shaped.nothing is unibind.null and shaped.missing is None"));
    CHECK(EvalError(f.context, "shaped.frozen = 99").starts_with("TypeError"));
    CHECK(EvalInt(f.context, "shaped.frozen") == 1);
    CHECK(instance->Set(f.context, "frozen", ub::Integer::New(f.iso(), 99)).value_or(false));
    CHECK(EvalInt(f.context, "shaped.frozen") == 1);
    CHECK(EvalTruth(f.context, "type(shaped) is unibind.Object"));
}

TEST_CASE("templates: a method is callable on every instance, and not enumerable") {
    Fixture f;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.Set("greet", &SaysHello);
    shape.Set("count", ub::Constant(std::int32_t{1}));

    const auto first = shape.NewInstance(f.context);
    const auto second = shape.NewInstance(f.context);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK_FALSE(first->StrictEquals(*second));
    Expose(f.context, "a", *first);
    Expose(f.context, "b", *second);

    CHECK(EvalText(f.context, "a.greet() + ' ' + b.greet()") == "hello hello");
    CHECK(EvalTruth(f.context, "list(a) == ['count'] and 'greet' in dir(a) and a.greet.__name__ == 'greet'"));
    // A method is not a constructor: native cannot `new` one either.
    const auto method = Eval(f.context, "a['greet']").To<ub::Function>();
    REQUIRE(method.has_value());
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(method->NewInstance(f.context).has_value());
    CHECK(handler.HasCaught());
}

TEST_CASE("templates: an accessor runs native code on read and write, from both sides") {
    Fixture f;
    Cell cell{.value = 3};
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetAccessor("value", &ReadCell, &WriteCell, ub::CallbackData::For(cell));
    shape.SetAccessor("fixed", &ReadCell, nullptr, ub::CallbackData::For(cell));
    shape.SetAccessor("broken", &ReadCell);

    const auto instance = shape.NewInstance(f.context);
    REQUIRE(instance.has_value());
    Expose(f.context, "cell", *instance);

    CHECK(EvalInt(f.context, "cell.value") == 3);
    CHECK(cell.reads == 1);
    CHECK(Eval(f.context, "cell.value = 11").IsUndefined());
    CHECK(cell.writes == 1);
    CHECK(cell.value == 11);
    const auto read = instance->Get(f.context, "value");
    REQUIRE(read.has_value());
    CHECK(read->To<ub::Integer>()->Int32Value() == 11);
    CHECK(cell.reads == 2);

    CHECK(EvalError(f.context, "cell.fixed = 1").starts_with("TypeError"));
    CHECK(cell.writes == 1);
    // Without data the getter throws, which is how this knows it ran.
    CHECK(EvalError(f.context, "cell.broken") == "TypeError: accessor data missing");
}

TEST_CASE("templates: an inherited accessor sees the instance as This and the prototype as Holder") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Holder");
    tpl.InstanceTemplate().Set("name", ub::Constant(std::string_view("the instance")));
    tpl.InstanceTemplate().SetAccessor("own", &ReadReceiverName);
    tpl.PrototypeTemplate().SetAccessor("whose", &ReadReceiverName);
    Expose(f.context, "Holder", *tpl.GetFunction(f.context));

    CHECK(EvalText(f.context, "Holder().whose") == "the instance/inherited");
    CHECK(EvalText(f.context, "Holder().own") == "the instance/self");
}

TEST_CASE("templates: templates nest, an object template's as an instance and a function template's as a type") {
    Fixture f;
    MakerLog log;
    const auto inner = ub::ObjectTemplate::New(f.iso());
    inner.Set("depth", ub::Constant(std::int32_t{2}));
    const auto maker = ub::FunctionTemplate::New(f.iso(), &Maker, ub::CallbackData::For(log));
    maker.SetClassName("Maker");

    const auto outer = ub::ObjectTemplate::New(f.iso());
    outer.Set("depth", ub::Constant(std::int32_t{1}));
    outer.Set("nested", inner);
    outer.Set("Maker", maker);

    const auto instance = outer.NewInstance(f.context);
    REQUIRE(instance.has_value());
    Expose(f.context, "tree", *instance);
    CHECK(EvalInt(f.context, "tree.depth * 10 + tree.nested.depth") == 12);
    CHECK(EvalTruth(f.context, "isinstance(tree.Maker, type) and tree.Maker(6).made == 6"));
    CHECK(log.constructCalls == 1);
    // One type per template per realm, however it is reached.
    Expose(f.context, "Direct", *maker.GetFunction(f.context));
    CHECK(EvalTruth(f.context, "Direct is tree.Maker"));
}

TEST_CASE("templates: one template instantiates into several realms, as a type of each") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Widget");
    tpl.PrototypeTemplate().Set("greet", &SaysHello);

    auto second = ub::Context::New(f.iso());
    REQUIRE(second.has_value());
    const auto here = tpl.GetFunction(f.context);
    REQUIRE(here.has_value());
    Expose(f.context, "Widget", *here);
    CHECK(EvalText(f.context, "Widget().greet()") == "hello");

    const ub::ContextScope entered(*second);
    const auto there = tpl.GetFunction(*second);
    REQUIRE(there.has_value());
    Expose(*second, "Widget", *there);
    CHECK(EvalText(*second, "Widget().greet()") == "hello");
    CHECK_FALSE(here->StrictEquals(*there));
    // An instance from one realm is still what the template made, in another.
    const auto instance = Eval(*second, "Widget()");
    CHECK(tpl.HasInstance(f.context, instance).value_or(false));
}

TEST_CASE("templates: a function template is a type with a prototype, statics and instance members") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Widget");
    tpl.InstanceTemplate().Set("name", ub::Constant(std::string_view("widget")));
    tpl.PrototypeTemplate().Set("whoAmI", &SaysWhoItIs);
    tpl.Set("KIND", ub::Constant(std::string_view("widget-kind")));
    tpl.Set("create", &SaysHello);
    const auto constructor = tpl.GetFunction(f.context);
    REQUIRE(constructor.has_value());
    Expose(f.context, "Widget", *constructor);

    CHECK(EvalText(f.context, "Widget.__name__") == "Widget");
    CHECK(EvalText(f.context, "Widget.__module__") == "unibind");
    CHECK(EvalTruth(f.context,
                    "w = Widget()\nw.constructor is Widget and type(w) is Widget and isinstance(w, unibind.Object)"));
    CHECK(EvalTruth(f.context, "w.__proto__ is Widget.prototype and 'constructor' not in list(Widget.prototype)"));
    CHECK(EvalText(f.context, "Widget.KIND + ' ' + Widget.create()") == "widget-kind hello");
    CHECK(EvalText(f.context, "w.name + ' ' + w.whoAmI()") == "widget widget");
    CHECK(EvalText(f.context, "repr(w)") == "Widget {name: 'widget'}");
    // The instance's own members are its own; the prototype's are shared.
    CHECK(EvalTruth(f.context, "list(w) == ['name']"));
    const auto instance = Eval(f.context, "w").To<ub::Object>();
    REQUIRE(instance.has_value());
    CHECK(instance->HasOwn(f.context, Str(f.iso(), "name")).value_or(false));
    CHECK_FALSE(instance->HasOwn(f.context, Str(f.iso(), "whoAmI")).value_or(true));
    CHECK(instance->Has(f.context, Str(f.iso(), "whoAmI")).value_or(false));
    // From native, the type's attributes are its properties.
    CHECK(py_test::TextOf(f.context, *constructor->Get(f.context, "KIND")) == "widget-kind");
    CHECK(constructor->Get(f.context, "prototype")->IsObject());
}

TEST_CASE("templates: a static declared ReadOnly or DontDelete holds on the type") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Limits");
    tpl.Set("MAX", ub::Constant(std::int32_t{10}), ub::PropertyAttribute::ReadOnly);
    tpl.Set("KEEP", ub::Constant(std::int32_t{1}), ub::PropertyAttribute::DontDelete);
    tpl.Set("FREE", ub::Constant(std::int32_t{2}));
    Expose(f.context, "Limits", *tpl.GetFunction(f.context));

    CHECK(EvalError(f.context, "Limits.MAX = 11").starts_with("TypeError"));
    CHECK(EvalError(f.context, "del Limits.KEEP").starts_with("TypeError"));
    CHECK(EvalInt(f.context, "Limits.FREE = 3\nLimits.KEEP = 4\nLimits.MAX + Limits.FREE + Limits.KEEP") == 17);
}

TEST_CASE("templates: a function template with a callback constructs, and IsConstructCall says so") {
    Fixture f;
    MakerLog log;
    const auto tpl = ub::FunctionTemplate::New(f.iso(), &Maker, ub::CallbackData::For(log));
    tpl.SetClassName("Maker");
    const auto constructor = tpl.GetFunction(f.context);
    REQUIRE(constructor.has_value());
    Expose(f.context, "Maker", *constructor);

    CHECK(EvalInt(f.context, "Maker(3).made") == 3);
    CHECK(log.constructCalls == 1);
    CHECK(EvalTruth(f.context, "isinstance(Maker(1), Maker)"));
    CHECK(EvalError(f.context, "Maker(1, key=2)").starts_with("TypeError"));

    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(f.iso(), 5)};
    const auto instance = constructor->NewInstance(f.context, arguments);
    REQUIRE(instance.has_value());
    CHECK(instance->Get(f.context, "made")->To<ub::Integer>()->Int32Value() == 5);
    CHECK(log.constructCalls == 3);
}

TEST_CASE("templates: a function template is a function as well as a constructor") {
    Fixture f;
    MakerLog log;
    const auto tpl = ub::FunctionTemplate::New(f.iso(), &EitherWay, ub::CallbackData::For(log));
    tpl.SetClassName("EitherWay");
    const auto function = tpl.GetFunction(f.context);
    REQUIRE(function.has_value());
    Expose(f.context, "EitherWay", *function);

    // Python can only construct a type; the plain call is native's.
    CHECK(EvalText(f.context, "EitherWay().how") == "constructed");
    CHECK(log.constructCalls == 1);
    const auto called = function->Call(f.context, f.context.GlobalObject());
    REQUIRE(called.has_value());
    CHECK(py_test::TextOf(f.context, *called) == "called");
    CHECK(log.calls == 2);
    CHECK(log.constructCalls == 1);
    const auto constructed = function->NewInstance(f.context);
    REQUIRE(constructed.has_value());
    CHECK(py_test::TextOf(f.context, *constructed->Get(f.context, "how")) == "constructed");

    // A template with no callback: a plain call runs nothing.
    const auto bare = ub::FunctionTemplate::New(f.iso()).GetFunction(f.context);
    REQUIRE(bare.has_value());
    CHECK(bare->Call(f.context, ub::Undefined(f.iso()))->IsUndefined());
}

TEST_CASE("templates: a constructor answering with an object replaces the instance, and a primitive does not") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso(), &Replaces);
    tpl.SetClassName("Replaces");
    Expose(f.context, "Replaces", *tpl.GetFunction(f.context));
    CHECK(EvalTruth(f.context, "d = {'mine': 1}\nReplaces(d) is d"));
    CHECK(EvalTruth(f.context, "type(Replaces(5)) is Replaces and type(Replaces('s')) is Replaces"));
}

TEST_CASE("templates: a static lives on the type, and an instance finds the prototype's first") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Widget");
    tpl.Set("describe", &SaysHello);
    tpl.PrototypeTemplate().Set("describe", &SaysWhoItIs);
    tpl.InstanceTemplate().Set("name", ub::Constant(std::string_view("an instance")));
    Expose(f.context, "Widget", *tpl.GetFunction(f.context));

    CHECK(EvalText(f.context, "Widget.describe()") == "hello");
    CHECK(EvalText(f.context, "Widget().describe()") == "an instance");
}

TEST_CASE("templates: a function template says which values it made, whatever their prototype") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Widget");
    Expose(f.context, "Widget", *tpl.GetFunction(f.context));

    const auto instance = Eval(f.context, "Widget()");
    const auto stranger = Eval(f.context, "unibind.Object()");
    const auto lookalike = Eval(f.context, "l = unibind.Object(); l.__proto__ = Widget.prototype\nl");
    const auto swapped = Eval(f.context, "s = Widget(); s.__proto__ = None\ns");
    CHECK(tpl.HasInstance(f.context, instance).value_or(false));
    CHECK_FALSE(tpl.HasInstance(f.context, stranger).value_or(true));
    CHECK_FALSE(tpl.HasInstance(f.context, lookalike).value_or(true));
    CHECK(tpl.HasInstance(f.context, swapped).value_or(false));
    CHECK_FALSE(tpl.HasInstance(f.context, Eval(f.context, "Widget.prototype")).value_or(true));
    CHECK_FALSE(tpl.HasInstance(f.context, Eval(f.context, "{}")).value_or(true));

    const auto direct = tpl.InstanceTemplate().NewInstance(f.context);
    REQUIRE(direct.has_value());
    CHECK(tpl.HasInstance(f.context, *direct).value_or(false));
    Expose(f.context, "direct", *direct);
    CHECK(EvalTruth(f.context, "type(direct) is Widget"));
}

TEST_CASE("templates: inheritance puts the parent's prototype behind the child's, and its type behind the child's") {
    Fixture f;
    const auto base = ub::FunctionTemplate::New(f.iso());
    base.SetClassName("Base");
    base.PrototypeTemplate().Set("fromBase", &SaysHello);
    const auto derived = ub::FunctionTemplate::New(f.iso());
    derived.SetClassName("Derived");
    derived.Inherit(base);
    derived.InstanceTemplate().Set("name", ub::Constant(std::string_view("derived")));
    derived.PrototypeTemplate().Set("fromDerived", &SaysWhoItIs);
    Expose(f.context, "Base", *base.GetFunction(f.context));
    Expose(f.context, "Derived", *derived.GetFunction(f.context));

    CHECK(EvalText(f.context, "Derived().fromBase() + ' ' + Derived().fromDerived()") == "hello derived");
    CHECK(EvalTruth(f.context, "isinstance(Derived(), Base) and isinstance(Derived(), Derived)"));
    CHECK(EvalTruth(f.context, "not isinstance(Base(), Derived)"));
    CHECK(EvalTruth(f.context, "Derived.prototype.__proto__ is Base.prototype"));
    const auto instance = Eval(f.context, "Derived()");
    CHECK(base.HasInstance(f.context, instance).value_or(false));
    CHECK_FALSE(derived.HasInstance(f.context, Eval(f.context, "Base()")).value_or(true));
}

TEST_CASE("templates: the shape is fixed at the first instantiation, and members may still be added") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("First");
    Expose(f.context, "First", *tpl.GetFunction(f.context));
    tpl.SetClassName("Second");
    tpl.PrototypeTemplate().Set("late", &SaysHello);

    auto second = ub::Context::New(f.iso());
    REQUIRE(second.has_value());
    const ub::ContextScope entered(*second);
    Expose(*second, "Again", *tpl.GetFunction(*second));
    CHECK(EvalText(*second, "Again.__name__ + ' ' + Again().late()") == "First hello");
    CHECK(EvalText(f.context, "First.__name__") == "First");
}

TEST_CASE("templates: a Symbol.iterator method written for JavaScript iterates from Python") {
    Fixture f;
    const auto tpl = ub::FunctionTemplate::New(f.iso());
    tpl.SetClassName("Range");
    tpl.InstanceTemplate().Set("size", ub::Constant(std::int32_t{3}));
    tpl.PrototypeTemplate().Set(ub::WellKnownSymbol::Iterator, &JsStyleIterator);
    Expose(f.context, "Range", *tpl.GetFunction(f.context));

    CHECK(EvalText(f.context, "repr(list(Range()))") == "[0, 1, 2]");
    CHECK(EvalInt(f.context, "sum(x for x in Range())") == 3);
    CHECK(EvalTruth(
        f.context,
        "it = iter(Range())\nnext(it) == 0 and next(it) == 1 and next(it) == 2 and next(it, 'end') == 'end'"));
    // The method is listed as the symbol it was declared under.
    const auto prototype = Eval(f.context, "Range.prototype").To<ub::Object>();
    REQUIRE(prototype.has_value());
    const auto names =
        prototype->GetOwnPropertyNames(f.context, {.includeNonEnumerable = true, .includeSymbols = true});
    REQUIRE(names.has_value());
    REQUIRE(names->Length() == 2);
    CHECK(names->Get(f.context, 1U)->IsSymbol());
}

TEST_CASE("templates: a Python subclass of a template's type constructs through it") {
    Fixture f;
    MakerLog log;
    const auto tpl = ub::FunctionTemplate::New(f.iso(), &Maker, ub::CallbackData::For(log));
    tpl.SetClassName("Maker");
    tpl.PrototypeTemplate().Set("greet", &SaysHello);
    Expose(f.context, "Maker", *tpl.GetFunction(f.context));

    REQUIRE(ub::Evaluate(f.context, R"(
class Sub(Maker):
    def twice(self):
        return self.made * 2
s = Sub(21)
)")
                .has_value());
    CHECK(EvalInt(f.context, "s.twice()") == 42);
    CHECK(EvalText(f.context, "s.greet()") == "hello");
    CHECK(EvalTruth(f.context, "type(s) is Sub and isinstance(s, Maker)"));
    CHECK(log.constructCalls == 1);
    CHECK(tpl.HasInstance(f.context, Eval(f.context, "s")).value_or(false));
}
