// Objects: `unibind.Object` and the property operations of `ub::Object`, on it
// and on the ordinary Python objects - dicts, lists, anything with attributes -
// that script hands the embedder.
//
// The shared suite's objects cases, in Python terms, and the places where the
// Python side of `unibind.Object` has to feel like Python.

#include <cstdint>
#include <optional>
#include <string>
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

/// Own keys as text, in the order the backend lists them.
std::vector<std::string> OwnKeys(const ub::Context& context, const ub::Local<ub::Object>& object,
                                 ub::KeyFilter filter = {}) {
    auto names = object.GetOwnPropertyNames(context, filter);
    REQUIRE(names.has_value());
    std::vector<std::string> keys;
    for (std::uint32_t i = 0; i < names->Length(); ++i) {
        auto key = names->Get(context, i);
        REQUIRE(key.has_value());
        keys.push_back(py_test::TextOf(context, *key));
    }
    return keys;
}

ub::Local<ub::Object> NewObject(const Fixture& f) {
    auto object = ub::Object::New(f.context);
    REQUIRE(object.has_value());
    return *object;
}

std::int32_t IntOf(const std::optional<ub::Local<ub::Value>>& value) {
    REQUIRE(value.has_value());
    const auto asInt = value->To<ub::Integer>();
    REQUIRE(asInt.has_value());
    return asInt->Int32Value();
}

/// The state behind one accessor, and what its callbacks were asked.
struct Cell {
    std::int32_t value = 0;
    int reads = 0;
    int writes = 0;
    std::string lastName;
    bool receiverWasHolder = false;
};

void ReadCell(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    auto* cell = info.Data<Cell>();
    ++cell->reads;
    if (auto name = property.To<ub::String>()) {
        cell->lastName = name->Utf8Value();
    }
    cell->receiverWasHolder = info.This().StrictEquals(info.Holder());
    info.GetReturnValue().Set(cell->value);
}

void WriteCell(const ub::Local<ub::Name>& /*property*/, const ub::Local<ub::Value>& value,
               const ub::PropertyCallbackInfo& info) {
    auto* cell = info.Data<Cell>();
    ++cell->writes;
    cell->receiverWasHolder = info.This().StrictEquals(info.Holder());
    if (auto asInt = value.ToInt32(info.GetContext())) {
        cell->value = *asInt;
    }
}

void ThrowingGetter(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    info.ThrowTypeError("bang");
}

}  // namespace

TEST_CASE("objects: a property set from native reads back from script, and back again") {
    Fixture f;
    auto object = NewObject(f);
    REQUIRE(object.Set(f.context, "answer", ub::Integer::New(f.iso(), 42)).value_or(false));
    Expose(f.context, "probe", object);

    CHECK(EvalInt(f.context, "probe.answer") == 42);
    CHECK(EvalInt(f.context, "probe['answer']") == 42);
    CHECK(Eval(f.context, "probe.answer = 43").IsUndefined());
    CHECK(IntOf(object.Get(f.context, "answer")) == 43);
    CHECK(EvalTruth(f.context, "isinstance(probe, unibind.Object)"));
}

TEST_CASE("objects: a property is one property whether named by string, handle or index") {
    Fixture f;
    auto object = NewObject(f);
    const auto key = Str(f.iso(), "byHandle");
    REQUIRE(object.Set(f.context, "byName", ub::Integer::New(f.iso(), 1)).value_or(false));
    REQUIRE(object.Set(f.context, key, ub::Integer::New(f.iso(), 2)).value_or(false));
    REQUIRE(object.Set(f.context, 7U, ub::Integer::New(f.iso(), 3)).value_or(false));

    CHECK(IntOf(object.Get(f.context, "byName")) == 1);
    CHECK(IntOf(object.Get(f.context, key)) == 2);
    CHECK(IntOf(object.Get(f.context, 7U)) == 3);
    // A canonical index is one key however it is spelled, as in JavaScript.
    CHECK(IntOf(object.Get(f.context, "7")) == 3);

    Expose(f.context, "o", object);
    CHECK(EvalInt(f.context, "o[7]") == 3);
    CHECK(EvalInt(f.context, "o['7']") == 3);
    // Not canonical: a different key.
    CHECK(EvalTruth(f.context, "'07' not in o"));
}

TEST_CASE("objects: a missing property is undefined to native and an error to Python") {
    Fixture f;
    auto object = NewObject(f);
    Expose(f.context, "o", object);

    const auto missing = object.Get(f.context, "nope");
    REQUIRE(missing.has_value());
    CHECK(missing->IsUndefined());
    CHECK_FALSE(object.Has(f.context, Str(f.iso(), "nope")).value_or(true));

    // Python's own conventions for absence, which `getattr` with a default and
    // `in` rely on.
    CHECK(EvalError(f.context, "o.nope").starts_with("AttributeError"));
    CHECK(EvalError(f.context, "o['nope']").starts_with("KeyError"));
    CHECK(EvalTruth(f.context, "getattr(o, 'nope', 5) == 5 and 'nope' not in o and not hasattr(o, 'nope')"));
}

TEST_CASE("objects: has looks along the prototype chain and hasOwn does not") {
    Fixture f;
    auto parent = NewObject(f);
    auto child = NewObject(f);
    REQUIRE(parent.Set(f.context, "inherited", ub::Integer::New(f.iso(), 1)).value_or(false));
    REQUIRE(child.Set(f.context, "own", ub::Integer::New(f.iso(), 2)).value_or(false));
    REQUIRE(child.SetPrototype(f.context, parent).value_or(false));

    const auto inherited = Str(f.iso(), "inherited");
    const auto own = Str(f.iso(), "own");
    CHECK(child.Has(f.context, inherited).value_or(false));
    CHECK_FALSE(child.HasOwn(f.context, inherited).value_or(true));
    CHECK(child.HasOwn(f.context, own).value_or(false));
    CHECK(IntOf(child.Get(f.context, "inherited")) == 1);

    Expose(f.context, "child", child);
    Expose(f.context, "parent", parent);
    CHECK(EvalTruth(f.context, "'inherited' in child and child.inherited == 1 and child.__proto__ is parent"));
    // Only own keys are listed or iterated.
    CHECK(EvalText(f.context, "','.join(child)") == "own");
    // A write lands on the receiver and shadows what it inherits.
    CHECK(EvalInt(f.context, "child.inherited = 5; child.inherited * 10 + parent.inherited") == 51);
}

TEST_CASE("objects: delete removes an own property and says whether it could") {
    Fixture f;
    auto object = NewObject(f);
    Expose(f.context, "o", object);
    const auto key = Str(f.iso(), "gone");
    REQUIRE(object.Set(f.context, key, ub::Integer::New(f.iso(), 1)).value_or(false));

    CHECK(object.Delete(f.context, key).value_or(false));
    CHECK_FALSE(object.HasOwn(f.context, key).value_or(true));
    // Deleting what is not there succeeds, as in the language...
    CHECK(object.Delete(f.context, key).value_or(false));

    // ...and is Python's error from Python, where `del` of nothing is a bug.
    CHECK(EvalError(f.context, "del o.gone").starts_with("AttributeError"));
    CHECK(EvalError(f.context, "del o['gone']").starts_with("KeyError"));
    CHECK(EvalTruth(f.context, "o.x = 1; del o.x; o[3] = 1; del o[3]; len(o) == 0"));
}

TEST_CASE("objects: own keys come in the language's order, filtered as asked") {
    Fixture f;
    auto object = NewObject(f);
    Expose(f.context, "o", object);
    auto symbol = ub::Symbol::New(f.iso(), "tag");
    REQUIRE(symbol.has_value());
    REQUIRE(object.Set(f.context, *symbol, ub::Integer::New(f.iso(), 0)).value_or(false));
    REQUIRE(ub::Evaluate(f.context, "o.b = 1; o[10] = 1; o.a = 1; o[2] = 1").has_value());
    REQUIRE(object
                .DefineOwnProperty(f.context, Str(f.iso(), "hidden"), ub::Integer::New(f.iso(), 1),
                                   ub::PropertyAttribute::DontEnum)
                .value_or(false));

    // Indices ascending, then strings in insertion order, then symbols.
    CHECK(OwnKeys(f.context, object) == std::vector<std::string>{"2", "10", "b", "a"});
    CHECK(OwnKeys(f.context, object, {.includeNonEnumerable = true}) ==
          std::vector<std::string>{"2", "10", "b", "a", "hidden"});
    const auto all = OwnKeys(f.context, object, {.includeNonEnumerable = true, .includeSymbols = true});
    REQUIRE(all.size() == 6);
    CHECK(all.back() == "Symbol(tag)");

    // An index comes back as a Number, as V8 hands it back.
    auto names = object.GetOwnPropertyNames(f.context);
    REQUIRE(names.has_value());
    CHECK(names->Get(f.context, 0U)->IsNumber());

    // Python sees the same order, and only what is enumerable.
    CHECK(EvalText(f.context, "repr(list(o))") == "[2, 10, 'b', 'a']");
    CHECK(EvalInt(f.context, "len(o)") == 4);
}

TEST_CASE("objects: a defined property honours its attributes, from Python and from native") {
    Fixture f;
    auto object = NewObject(f);
    Expose(f.context, "probe", object);

    REQUIRE(object
                .DefineOwnProperty(f.context, Str(f.iso(), "readOnly"), ub::Integer::New(f.iso(), 1),
                                   ub::PropertyAttribute::ReadOnly)
                .value_or(false));
    REQUIRE(object
                .DefineOwnProperty(f.context, Str(f.iso(), "hidden"), ub::Integer::New(f.iso(), 2),
                                   ub::PropertyAttribute::DontEnum)
                .value_or(false));
    REQUIRE(object
                .DefineOwnProperty(f.context, Str(f.iso(), "permanent"), ub::Integer::New(f.iso(), 3),
                                   ub::PropertyAttribute::DontDelete)
                .value_or(false));

    // From Python there is no sloppy mode: a write to a read-only property is
    // strict mode's TypeError, and so is deleting a permanent one.
    CHECK(EvalError(f.context, "probe.readOnly = 99") ==
          "TypeError: Cannot assign to read only property 'readOnly' of object");
    CHECK(EvalError(f.context, "probe['readOnly'] = 99").starts_with("TypeError"));
    CHECK(EvalInt(f.context, "probe.readOnly") == 1);
    CHECK(EvalError(f.context, "del probe.permanent") == "TypeError: Cannot delete property 'permanent' of object");
    CHECK(EvalInt(f.context, "probe.permanent") == 3);
    CHECK(EvalTruth(f.context, "'hidden' not in list(probe) and probe.hidden == 2 and 'hidden' in dir(probe)"));

    // From native, V8's sloppy mode: `Set` reports success and changes
    // nothing; `Delete` reports the refusal.
    CHECK(object.Set(f.context, "readOnly", ub::Integer::New(f.iso(), 7)) == std::optional<bool>(true));
    CHECK(IntOf(object.Get(f.context, "readOnly")) == 1);
    CHECK(object.Delete(f.context, Str(f.iso(), "permanent")) == std::optional<bool>(false));

    // A permanent property may not be redefined into something else.
    CHECK(object
              .DefineOwnProperty(f.context, Str(f.iso(), "permanent"), ub::Integer::New(f.iso(), 4),
                                 ub::PropertyAttribute::DontEnum)
              .value() == false);
    CHECK(object
              .DefineOwnProperty(f.context, Str(f.iso(), "permanent"), ub::Integer::New(f.iso(), 4),
                                 ub::PropertyAttribute::DontDelete)
              .value());
    CHECK(IntOf(object.Get(f.context, "permanent")) == 4);
}

TEST_CASE("objects: an inherited read-only property refuses the write that would shadow it") {
    Fixture f;
    auto parent = NewObject(f);
    auto child = NewObject(f);
    REQUIRE(parent
                .DefineOwnProperty(f.context, Str(f.iso(), "fixed"), ub::Integer::New(f.iso(), 1),
                                   ub::PropertyAttribute::ReadOnly)
                .value_or(false));
    REQUIRE(child.SetPrototype(f.context, parent).value_or(false));
    Expose(f.context, "child", child);

    CHECK(EvalError(f.context, "child.fixed = 2").starts_with("TypeError"));
    CHECK(child.Set(f.context, "fixed", ub::Integer::New(f.iso(), 2)).value_or(false));
    CHECK_FALSE(child.HasOwn(f.context, Str(f.iso(), "fixed")).value_or(true));
    // Define is not assignment: it makes the own property.
    CHECK(child.DefineOwnProperty(f.context, Str(f.iso(), "fixed"), ub::Integer::New(f.iso(), 3)).value_or(false));
    CHECK(EvalInt(f.context, "child.fixed") == 3);
}

TEST_CASE("objects: attributes read back, and an absent property has none") {
    Fixture f;
    auto object = NewObject(f);
    const auto plain = Str(f.iso(), "plain");
    const auto locked = Str(f.iso(), "locked");
    REQUIRE(object.Set(f.context, plain, ub::Integer::New(f.iso(), 1)).value_or(false));
    REQUIRE(object
                .DefineOwnProperty(f.context, locked, ub::Integer::New(f.iso(), 2),
                                   ub::PropertyAttribute::ReadOnly | ub::PropertyAttribute::DontDelete)
                .value_or(false));

    CHECK(object.GetPropertyAttributes(f.context, plain) == std::optional(ub::PropertyAttribute::None));
    const auto attributes = object.GetPropertyAttributes(f.context, locked);
    REQUIRE(attributes.has_value());
    CHECK(ub::HasAttribute(*attributes, ub::PropertyAttribute::ReadOnly));
    CHECK(ub::HasAttribute(*attributes, ub::PropertyAttribute::DontDelete));
    CHECK_FALSE(ub::HasAttribute(*attributes, ub::PropertyAttribute::DontEnum));

    ub::TryCatch handler(f.iso());
    CHECK_FALSE(object.GetPropertyAttributes(f.context, Str(f.iso(), "absent")).has_value());
    CHECK_FALSE(handler.HasCaught());
    CHECK_FALSE(f.iso().HasPendingException());
}

TEST_CASE("objects: an accessor on one object runs native code on every read and write") {
    Fixture f;
    auto object = NewObject(f);
    Cell cell{.value = 7};
    CHECK(object.SetAccessor(f.context, "level", &ReadCell, &WriteCell, ub::CallbackData::For(cell)) ==
          std::optional<bool>(true));
    Expose(f.context, "thing", object);

    CHECK(EvalInt(f.context, "thing.level") == 7);
    CHECK(EvalInt(f.context, "thing.level = 12\nthing.level + 1") == 13);
    CHECK(cell.value == 12);
    CHECK(cell.writes == 1);
    CHECK(cell.reads == 2);
    CHECK(cell.lastName == "level");
    CHECK(cell.receiverWasHolder);
    // Through the native API and the subscript too.
    CHECK(IntOf(object.Get(f.context, "level")) == 12);
    CHECK(EvalInt(f.context, "thing['level']") == 12);
    CHECK(cell.reads == 4);
    // The repr shows it without running it.
    CHECK(EvalText(f.context, "repr(thing)") == "{level: [Getter/Setter]}");
    CHECK(cell.reads == 4);
}

TEST_CASE("objects: an accessor with no setter is read-only, and ReadOnly on it changes nothing") {
    Fixture f;
    auto object = NewObject(f);
    Cell cell{.value = 3};
    CHECK(object
              .SetAccessor(f.context, "fixed", &ReadCell, nullptr, ub::CallbackData::For(cell),
                           ub::PropertyAttribute::ReadOnly)
              .value_or(false));
    Expose(f.context, "thing", object);

    CHECK(EvalError(f.context, "thing.fixed = 9") ==
          "TypeError: Cannot set property fixed of object which has only a getter");
    CHECK(EvalInt(f.context, "thing.fixed") == 3);
    CHECK(cell.writes == 0);
    const auto attributes = object.GetPropertyAttributes(f.context, Str(f.iso(), "fixed"));
    REQUIRE(attributes.has_value());
    CHECK_FALSE(ub::HasAttribute(*attributes, ub::PropertyAttribute::ReadOnly));
}

TEST_CASE("objects: an inherited accessor sees the receiver as This and its owner as Holder") {
    Fixture f;
    auto parent = NewObject(f);
    auto child = NewObject(f);
    Cell cell{.value = 1};
    REQUIRE(
        parent.SetAccessor(f.context, "through", &ReadCell, &WriteCell, ub::CallbackData::For(cell)).value_or(false));
    REQUIRE(child.SetPrototype(f.context, parent).value_or(false));
    Expose(f.context, "child", child);

    CHECK(EvalInt(f.context, "child.through") == 1);
    CHECK_FALSE(cell.receiverWasHolder);
    // Set goes through the inherited setter rather than making an own property.
    REQUIRE(child.Set(f.context, "through", ub::Integer::New(f.iso(), 5)).value_or(false));
    CHECK(cell.value == 5);
    CHECK_FALSE(cell.receiverWasHolder);
    CHECK_FALSE(child.HasOwn(f.context, Str(f.iso(), "through")).value_or(true));
    // Define ignores the setter and makes one.
    REQUIRE(child.DefineOwnProperty(f.context, Str(f.iso(), "through"), ub::Integer::New(f.iso(), 6)).value_or(false));
    CHECK(child.HasOwn(f.context, Str(f.iso(), "through")).value_or(false));
    CHECK(cell.value == 5);
}

TEST_CASE("objects: an accessor that throws reports no value, and Python sees the exception") {
    Fixture f;
    auto object = NewObject(f);
    REQUIRE(object.SetAccessor(f.context, "boom", &ThrowingGetter).value_or(false));
    Expose(f.context, "o", object);

    {
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(object.Get(f.context, "boom").has_value());
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(f.context).value_or("") == "TypeError: bang");
    }
    CHECK(EvalText(f.context, "try:\n    o.boom\n    r = 'no'\nexcept TypeError as e:\n    r = str(e)\nr") == "bang");
    CHECK_FALSE(f.iso().HasPendingException());
}

TEST_CASE("objects: a prototype can be read, replaced and cleared, and never made a cycle") {
    Fixture f;
    auto parent = NewObject(f);
    auto child = NewObject(f);
    REQUIRE(parent.Set(f.context, "fromParent", ub::Integer::New(f.iso(), 11)).value_or(false));
    REQUIRE(child.SetPrototype(f.context, parent).value_or(false));

    const auto prototype = child.GetPrototype(f.context);
    REQUIRE(prototype.has_value());
    CHECK(prototype->StrictEquals(parent));
    CHECK(IntOf(child.Get(f.context, "fromParent")) == 11);

    {
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(parent.SetPrototype(f.context, child).has_value());
        CHECK(handler.HasCaught());
    }

    REQUIRE(child.SetPrototype(f.context, ub::Null(f.iso())).value_or(false));
    const auto none = child.GetPrototype(f.context);
    REQUIRE(none.has_value());
    CHECK(none->IsNull());

    Expose(f.context, "a", parent);
    Expose(f.context, "b", child);
    CHECK(EvalTruth(f.context, "b.__proto__ = a\nb.fromParent == 11 and b.__proto__ is a"));
    CHECK(EvalError(f.context, "a.__proto__ = b") == "TypeError: Cyclic __proto__ value");
    CHECK(EvalError(f.context, "a.__proto__ = {}").starts_with("TypeError"));
    CHECK(EvalTruth(f.context, "b.__proto__ = None\nb.__proto__ is None and not hasattr(b, 'fromParent')"));
}

TEST_CASE("objects: Python builds, copies and iterates an object as it would a mapping") {
    Fixture f;
    CHECK(EvalText(f.context, "o = unibind.Object({'a': 1, 'b': 'two'}, c=[3])\nrepr(o)") ==
          "{a: 1, b: 'two', c: [3]}");
    CHECK(EvalText(f.context, "repr(unibind.Object([('x', 1), (2, 2)]))") == "{2: 2, x: 1}");
    CHECK(EvalText(f.context, "repr(unibind.Object(o))") == "{a: 1, b: 'two', c: [3]}");
    CHECK(EvalTruth(f.context, "sorted(o) == ['a', 'b', 'c'] and len(o) == 3"));
    CHECK(EvalTruth(f.context, "{k: o[k] for k in o} == {'a': 1, 'b': 'two', 'c': [3]}"));
    // An object is truthy however empty, as in JavaScript.
    CHECK(EvalTruth(f.context, "bool(unibind.Object()) and len(unibind.Object()) == 0"));
    // Identity, not structure, is equality - an object is not a dict.
    CHECK(EvalTruth(f.context, "unibind.Object() != unibind.Object() and o == o"));
    CHECK(EvalText(f.context, "repr(unibind.Object({'not an identifier': None, 'n': unibind.null}))") ==
          "{'not an identifier': None, n: null}");
    CHECK(EvalText(f.context, "r = unibind.Object(); r.me = r; repr(r)") == "{me: {...}}");
}

TEST_CASE("objects: a symbol is a key of its own, and a well-known one is the protocol it names") {
    Fixture f;
    auto object = NewObject(f);
    auto symbol = ub::Symbol::New(f.iso(), "secret");
    REQUIRE(symbol.has_value());
    REQUIRE(object.Set(f.context, *symbol, ub::Integer::New(f.iso(), 9)).value_or(false));
    Expose(f.context, "o", object);
    Expose(f.context, "secret", *symbol);

    CHECK(EvalInt(f.context, "o[secret]") == 9);
    CHECK(EvalTruth(f.context, "secret in o and 'secret' not in o and list(o) == []"));
    CHECK(IntOf(object.Get(f.context, *symbol)) == 9);

    // Symbol.iterator is `__iter__`: set it from native and Python iterates.
    REQUIRE(ub::Evaluate(f.context, "def _it():\n    return iter([1, 2, 3])").has_value());
    auto iterator = ub::Symbol::WellKnown(f.iso(), ub::WellKnownSymbol::Iterator);
    REQUIRE(iterator.has_value());
    auto function = ub::Evaluate(f.context, "_it");
    REQUIRE(function.has_value());
    REQUIRE(object.Set(f.context, *iterator, *function).value_or(false));
    CHECK(EvalText(f.context, "repr(list(o))") == "[1, 2, 3]");
    // And it is listed as the symbol it was set as, not as a string.
    const auto keys = OwnKeys(f.context, object, {.includeSymbols = true});
    CHECK(keys == std::vector<std::string>{"Symbol(secret)", "Symbol(Symbol.iterator)"});
}

TEST_CASE("objects: a Python subclass keeps Python's descriptors and methods") {
    Fixture f;
    REQUIRE(ub::Evaluate(f.context, R"(
class Point(unibind.Object):
    def __init__(self, x, y):
        super().__init__()
        self.x = x
        self.y = y
    @property
    def norm1(self):
        return abs(self.x) + abs(self.y)
    def moved(self, dx):
        return Point(self.x + dx, self.y)
p = Point(3, -4)
)")
                .has_value());
    CHECK(EvalInt(f.context, "p.norm1") == 7);
    CHECK(EvalInt(f.context, "p.moved(1).x") == 4);
    CHECK(EvalText(f.context, "repr(p)") == "Point {x: 3, y: -4}");
    // A property the class declares is not shadowed by an own property.
    CHECK(EvalError(f.context, "p.norm1 = 1").starts_with("AttributeError"));

    const auto point = Eval(f.context, "p").To<ub::Object>();
    REQUIRE(point.has_value());
    CHECK(IntOf(point->Get(f.context, "x")) == 3);
    CHECK(OwnKeys(f.context, *point) == std::vector<std::string>{"x", "y"});
}

TEST_CASE("objects: an object is weakly referenceable and collected in a cycle") {
    Fixture f;
    CHECK(EvalTruth(f.context, R"(
import gc, weakref
a = unibind.Object(); b = unibind.Object()
a.other = b; b.other = a; a.__proto__ = b
w = weakref.ref(a)
del a, b
gc.collect()
w() is None
)"));
}

TEST_CASE("objects: an array is a list, grown by index, with holes that read as None") {
    Fixture f;
    auto array = ub::Array::New(f.context, 3);
    REQUIRE(array.has_value());
    CHECK(array->Length() == 3);
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(array->Set(f.context, i, ub::Integer::New(f.iso(), static_cast<std::int32_t>(i * i))).value_or(false));
    }
    REQUIRE(array->Set(f.context, 9U, ub::Integer::New(f.iso(), 81)).value_or(false));
    CHECK(array->Length() == 10);
    CHECK(IntOf(array->Get(f.context, 2U)) == 4);
    CHECK(array->Get(f.context, 5U)->IsUndefined());
    CHECK(array->Get(f.context, 50U)->IsUndefined());
    CHECK(IntOf(array->Get(f.context, "length")) == 10);

    Expose(f.context, "probe", *array);
    CHECK(EvalTruth(f.context, "isinstance(probe, list) and len(probe) == 10 and probe[5] is None"));

    // `length` is writable, and truncates.
    REQUIRE(array->Set(f.context, "length", ub::Integer::New(f.iso(), 2)).value_or(false));
    CHECK(EvalText(f.context, "repr(probe)") == "[0, 1]");
    CHECK(OwnKeys(f.context, *array) == std::vector<std::string>{"0", "1"});
    CHECK(OwnKeys(f.context, *array, {.includeNonEnumerable = true}) == std::vector<std::string>{"0", "1", "length"});
    // `delete a[i]` leaves a hole rather than shifting.
    CHECK(array->Delete(f.context, Str(f.iso(), "0")).value_or(false));
    CHECK(EvalText(f.context, "repr(probe)") == "[None, 1]");
    CHECK(array->Has(f.context, Str(f.iso(), "length")).value_or(false));
}

TEST_CASE("objects: an array too long to be dense is refused, not attempted") {
    Fixture f;
    ub::TryCatch handler(f.iso());
    CHECK_FALSE(ub::Array::New(f.context, 3000000000U).has_value());
    REQUIRE(handler.HasCaught());
    CHECK(handler.Message(f.context).value_or("").find("RangeError") != std::string::npos);
    handler.Reset();
    const auto large = ub::Array::New(f.context, 1000U);
    REQUIRE(large.has_value());
    CHECK(large->Length() == 1000U);
}

TEST_CASE("objects: a tuple is a read-only array") {
    Fixture f;
    const auto tuple = Eval(f.context, "(1, 2, 3)").To<ub::Array>();
    REQUIRE(tuple.has_value());
    CHECK(tuple->Length() == 3);
    CHECK(IntOf(tuple->Get(f.context, 1U)) == 2);
    CHECK(tuple->Set(f.context, 0U, ub::Integer::New(f.iso(), 9)) == std::optional<bool>(false));
    CHECK(tuple->Delete(f.context, Str(f.iso(), "0")) == std::optional<bool>(false));
    CHECK(IntOf(tuple->Get(f.context, 0U)) == 1);
}

TEST_CASE("objects: a dict is its items, the realm's globals among them") {
    Fixture f;
    const auto dict = Eval(f.context, "d = {'a': 1, 3: 'three', '4': 'four'}\nd").To<ub::Object>();
    REQUIRE(dict.has_value());
    CHECK(IntOf(dict->Get(f.context, "a")) == 1);
    CHECK(py_test::TextOf(f.context, *dict->Get(f.context, 3U)) == "three");
    // A key is normalized on the way in, not the dict's own keys on the way
    // out: '4' is stored as a string, and index 4 does not find it.
    CHECK(dict->Get(f.context, 4U)->IsUndefined());
    CHECK(dict->Get(f.context, "missing")->IsUndefined());
    REQUIRE(dict->Set(f.context, "b", ub::Integer::New(f.iso(), 2)).value_or(false));
    CHECK(EvalInt(f.context, "d['b']") == 2);
    CHECK(dict->HasOwn(f.context, Str(f.iso(), "b")).value_or(false));
    CHECK(dict->Delete(f.context, Str(f.iso(), "b")).value_or(false));
    CHECK(dict->Delete(f.context, Str(f.iso(), "b")).value_or(false));
    CHECK(EvalTruth(f.context, "'b' not in d"));
    CHECK(OwnKeys(f.context, *dict) == std::vector<std::string>{"a", "3", "4"});

    // Nothing but a unibind.Object can keep an attribute: refused, not faked.
    CHECK(dict->DefineOwnProperty(f.context, Str(f.iso(), "c"), ub::Integer::New(f.iso(), 1),
                                  ub::PropertyAttribute::ReadOnly) == std::optional<bool>(false));
    CHECK(dict->DefineOwnProperty(f.context, Str(f.iso(), "c"), ub::Integer::New(f.iso(), 1)) ==
          std::optional<bool>(true));
    CHECK(dict->SetAccessor(f.context, "x", &ReadCell) == std::optional<bool>(false));

    // The global object is the realm's globals dict: what native sets there,
    // script reads as a global, and the backend's own entries are not listed.
    const auto global = f.context.GlobalObject();
    REQUIRE(global.Set(f.context, "fromNative", ub::Integer::New(f.iso(), 5)).value_or(false));
    CHECK(EvalInt(f.context, "fromNative + 1") == 6);
    CHECK(EvalTruth(f.context, "globals()['fromNative'] == 5"));
    for (const auto& key : OwnKeys(f.context, global, {.includeNonEnumerable = true})) {
        CHECK_FALSE(key.starts_with("__unibind"));
    }
    CHECK(global.GetPrototype(f.context)->IsNull());
}

TEST_CASE("objects: any other object is its attributes") {
    Fixture f;
    const auto thing = Eval(f.context, R"(
class Thing:
    kind = 'thing'
    def __init__(self):
        self.visible = 1
        self._private = 2
t = Thing()
t
)")
                           .To<ub::Object>();
    REQUIRE(thing.has_value());
    CHECK(IntOf(thing->Get(f.context, "visible")) == 1);
    CHECK(py_test::TextOf(f.context, *thing->Get(f.context, "kind")) == "thing");
    CHECK(thing->Get(f.context, "absent")->IsUndefined());
    CHECK(thing->Has(f.context, Str(f.iso(), "kind")).value_or(false));
    CHECK_FALSE(thing->HasOwn(f.context, Str(f.iso(), "kind")).value_or(true));
    CHECK(thing->HasOwn(f.context, Str(f.iso(), "visible")).value_or(false));

    REQUIRE(thing->Set(f.context, "added", ub::Integer::New(f.iso(), 3)).value_or(false));
    REQUIRE(thing->Set(f.context, 5U, ub::Integer::New(f.iso(), 4)).value_or(false));
    CHECK(EvalInt(f.context, "t.added + getattr(t, '5')") == 7);
    CHECK(thing->Delete(f.context, Str(f.iso(), "added")).value_or(false));
    CHECK(EvalTruth(f.context, "not hasattr(t, 'added')"));

    // An underscore marks what Python code does not mean to list.
    CHECK(OwnKeys(f.context, *thing) == std::vector<std::string>{"visible", "5"});
    CHECK(OwnKeys(f.context, *thing, {.includeNonEnumerable = true}) ==
          std::vector<std::string>{"visible", "_private", "5"});
    CHECK(thing->GetPropertyAttributes(f.context, Str(f.iso(), "_private")) ==
          std::optional(ub::PropertyAttribute::DontEnum));
    CHECK(thing->GetPrototype(f.context)->IsNull());
    {
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(thing->SetPrototype(f.context, ub::Null(f.iso())).has_value());
        CHECK(handler.HasCaught());
    }
}

TEST_CASE("objects: dir lists own and inherited names beside Python's own") {
    Fixture f;
    CHECK(EvalTruth(f.context, R"(
p = unibind.Object(shared=1)
c = unibind.Object(own=2)
c.__proto__ = p
names = dir(c)
'own' in names and 'shared' in names and '__class__' in names
)"));
}
