// Interceptors: a template's catch-all for every named or indexed property of
// its instances, driving attribute access, subscripts, `in`, `del`, `len`,
// iteration and `dir()` from Python as well as the native operations.

#include <cstdint>
#include <map>
#include <optional>
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

struct Store {
    std::map<std::string, std::int32_t, std::less<>> named;
    std::map<std::uint32_t, std::int32_t> indexed;
    int getterCalls = 0;
    int setterCalls = 0;
    std::string lastKey;
};

Store* StoreOf(const ub::PropertyCallbackInfo& info) {
    return info.Data<Store>();
}

std::optional<std::string> KeyOf(const ub::Local<ub::Name>& property) {
    const auto asString = property.To<ub::String>();
    if (!asString) {
        return std::nullopt;
    }
    return asString->Utf8Value();
}

ub::Intercepted GetNamed(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key) {
        return ub::Intercepted::No;
    }
    ++store->getterCalls;
    store->lastKey = *key;
    const auto found = store->named.find(*key);
    if (found == store->named.end()) {
        return ub::Intercepted::No;
    }
    info.GetReturnValue().Set(found->second);
    return ub::Intercepted::Yes;
}

ub::Intercepted SetNamed(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                         const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key || key->starts_with("own_")) {
        return ub::Intercepted::No;
    }
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return ub::Intercepted::Yes;
    }
    ++store->setterCalls;
    store->named[*key] = *asInt;
    return ub::Intercepted::Yes;
}

ub::Intercepted SetNamedAndScribble(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                                    const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(std::int32_t{999});
    return SetNamed(property, value, info);
}

std::optional<ub::PropertyAttribute> QueryNamed(const ub::Local<ub::Name>& property,
                                                const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key || !store->named.contains(*key)) {
        return std::nullopt;
    }
    return key->starts_with("hidden_") ? ub::PropertyAttribute::DontEnum : ub::PropertyAttribute::None;
}

std::optional<bool> DeleteNamed(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key || !store->named.contains(*key)) {
        return std::nullopt;
    }
    if (key->starts_with("keep_")) {
        return false;
    }
    store->named.erase(*key);
    return true;
}

std::optional<ub::Local<ub::Array>> EnumerateNamed(const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr) {
        return std::nullopt;
    }
    auto keys = ub::Array::New(info.GetContext(), 0);
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t at = 0;
    for (const auto& entry : store->named) {
        auto name = ub::String::New(info.GetIsolate(), entry.first);
        if (!name || !keys->Set(info.GetContext(), at++, *name).value_or(false)) {
            return std::nullopt;
        }
    }
    return keys;
}

std::optional<ub::Local<ub::Array>> EnumerateNothing(const ub::PropertyCallbackInfo& info) {
    return ub::Array::New(info.GetContext(), 0);
}

ub::Intercepted GetIndexed(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto found = store->indexed.find(index);
    if (found == store->indexed.end()) {
        return ub::Intercepted::No;
    }
    info.GetReturnValue().Set(found->second);
    return ub::Intercepted::Yes;
}

ub::Intercepted SetIndexed(std::uint32_t index, const ub::Local<ub::Value>& value,
                           const ub::PropertyCallbackInfo& info) {
    const auto asInt = value.ToInt32(info.GetContext());
    if (asInt) {
        StoreOf(info)->indexed[index] = *asInt;
    }
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> QueryIndexed(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    if (!StoreOf(info)->indexed.contains(index)) {
        return std::nullopt;
    }
    return ub::PropertyAttribute::None;
}

std::optional<bool> DeleteIndexed(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    if (StoreOf(info)->indexed.erase(index) == 0) {
        return std::nullopt;
    }
    return true;
}

std::optional<ub::Local<ub::Array>> EnumerateIndexed(const ub::PropertyCallbackInfo& info) {
    auto keys = ub::Array::New(info.GetContext(), 0);
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t at = 0;
    for (const auto& entry : StoreOf(info)->indexed) {
        if (!keys->Set(info.GetContext(), at++, ub::Integer::NewFromUnsigned(info.GetIsolate(), entry.first))
                 .value_or(false)) {
            return std::nullopt;
        }
    }
    return keys;
}

ub::Intercepted GetHolderOrThis(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto key = KeyOf(property);
    if (key == "holder") {
        info.GetReturnValue().Set(info.Holder());
        return ub::Intercepted::Yes;
    }
    if (key == "receiver") {
        info.GetReturnValue().Set(info.This());
        return ub::Intercepted::Yes;
    }
    return ub::Intercepted::No;
}

ub::Intercepted ThrowsAndDeclines(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    if (KeyOf(property) == "boom") {
        info.ThrowTypeError("the hook threw");
    }
    return ub::Intercepted::No;
}

ub::Intercepted SetterThrowsAndDeclines(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& /*value*/,
                                        const ub::PropertyCallbackInfo& info) {
    if (KeyOf(property) == "boom") {
        info.ThrowTypeError("the setter threw");
    }
    return ub::Intercepted::No;
}

std::optional<bool> DeleterThrowsAndDeclines(const ub::Local<ub::Name>& property,
                                             const ub::PropertyCallbackInfo& info) {
    if (KeyOf(property) == "boom") {
        info.ThrowTypeError("the deleter threw");
    }
    return std::nullopt;
}

/// Answers every name with its own length: the catch-all a sandbox is.
ub::Intercepted AnswersEverything(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (!key) {
        return ub::Intercepted::No;
    }
    ++store->getterCalls;
    store->lastKey = *key;
    info.GetReturnValue().Set(static_cast<std::int32_t>(key->size()));
    return ub::Intercepted::Yes;
}

ub::Local<ub::Object> Instance(const Fixture& f, const ub::ObjectTemplate& shape) {
    auto instance = shape.NewInstance(f.context);
    REQUIRE(instance.has_value());
    return *instance;
}

}  // namespace

TEST_CASE("interceptors: a named handler answers for properties that are not there") {
    Fixture f;
    Store store;
    store.named["fromTheStore"] = 11;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(
        ub::NamedPropertyHandler{.getter = &GetNamed, .setter = &SetNamed, .data = ub::CallbackData::For(store)});
    const auto instance = Instance(f, shape);
    Expose(f.context, "sandbox", instance);

    CHECK(EvalInt(f.context, "sandbox.fromTheStore") == 11);
    CHECK(EvalInt(f.context, "sandbox['fromTheStore']") == 11);
    CHECK(store.getterCalls == 2);
    CHECK(Eval(f.context, "sandbox.written = 22").IsUndefined());
    CHECK(store.setterCalls == 1);
    CHECK(store.named["written"] == 22);
    CHECK(EvalInt(f.context, "sandbox.written") == 22);
    CHECK(instance.Get(f.context, "written")->To<ub::Integer>()->Int32Value() == 22);
    REQUIRE(instance.Set(f.context, "viaNative", ub::Integer::New(f.iso(), 3)).value_or(false));
    CHECK(store.named["viaNative"] == 3);
    // Nothing landed on the object itself, though the getter makes the key
    // look like an own property.
    CHECK(instance.GetOwnPropertyNames(f.context)->Length() == 0);
    CHECK(instance.HasOwn(f.context, Str(f.iso(), "written")).value_or(false));
}

TEST_CASE("interceptors: declining lets the ordinary lookup carry on") {
    Fixture f;
    Store store;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.Set("declared", ub::Constant(std::int32_t{5}));
    shape.SetHandler(
        ub::NamedPropertyHandler{.getter = &GetNamed, .setter = &SetNamed, .data = ub::CallbackData::For(store)});
    const auto instance = Instance(f, shape);
    Expose(f.context, "sandbox", instance);

    CHECK(EvalInt(f.context, "sandbox.declared") == 5);
    CHECK(Eval(f.context, "sandbox.own_thing = 9").IsUndefined());
    CHECK(store.named.find("own_thing") == store.named.end());
    CHECK(EvalInt(f.context, "sandbox.own_thing") == 9);
    CHECK(EvalTruth(f.context, "'own_thing' in list(sandbox)"));
    CHECK(instance.HasOwn(f.context, Str(f.iso(), "own_thing")).value_or(false));
    // Missing is still missing: undefined to native, AttributeError to Python.
    CHECK(instance.Get(f.context, "neverHeardOf")->IsUndefined());
    CHECK(EvalTruth(f.context, "getattr(sandbox, 'neverHeardOf', 'absent') == 'absent'"));
}

TEST_CASE("interceptors: query, delete and enumerate make the store look like own properties") {
    Fixture f;
    Store store;
    store.named["alpha"] = 1;
    store.named["beta"] = 2;
    store.named["hidden_gamma"] = 3;
    store.named["keep_delta"] = 4;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed,
                                              .setter = &SetNamed,
                                              .query = &QueryNamed,
                                              .deleter = &DeleteNamed,
                                              .enumerator = &EnumerateNamed,
                                              .data = ub::CallbackData::For(store)});
    const auto instance = Instance(f, shape);
    Expose(f.context, "sandbox", instance);

    CHECK(EvalTruth(f.context, "'alpha' in sandbox and 'gamma' not in sandbox"));
    // Enumerable per the query: the hidden one is listed only when asked for.
    CHECK(EvalText(f.context, "','.join(sandbox)") == "alpha,beta,keep_delta");
    CHECK(EvalInt(f.context, "len(sandbox)") == 3);
    CHECK(EvalTruth(f.context, "'hidden_gamma' in dir(sandbox)"));
    const auto all = instance.GetOwnPropertyNames(f.context, {.includeNonEnumerable = true});
    REQUIRE(all.has_value());
    CHECK(all->Length() == 4);

    CHECK(Eval(f.context, "del sandbox.alpha").IsUndefined());
    CHECK(store.named.find("alpha") == store.named.end());
    CHECK(EvalText(f.context, "','.join(sandbox)") == "beta,keep_delta");
    // A deleter that says no is a refused delete.
    CHECK(EvalError(f.context, "del sandbox.keep_delta").starts_with("TypeError"));
    CHECK(instance.Delete(f.context, Str(f.iso(), "keep_delta")) == std::optional<bool>(false));
    CHECK(instance.Delete(f.context, Str(f.iso(), "beta")) == std::optional<bool>(true));

    CHECK(instance.Has(f.context, Str(f.iso(), "keep_delta")).value_or(false));
    CHECK(instance.GetPropertyAttributes(f.context, Str(f.iso(), "hidden_gamma")) ==
          std::optional(ub::PropertyAttribute::DontEnum));
    CHECK_FALSE(instance.GetPropertyAttributes(f.context, Str(f.iso(), "nothing")).has_value());
}

TEST_CASE("interceptors: an indexed handler answers for element access however it is spelled") {
    Fixture f;
    Store store;
    store.indexed[3] = 30;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::IndexedPropertyHandler{.getter = &GetIndexed,
                                                .setter = &SetIndexed,
                                                .query = &QueryIndexed,
                                                .deleter = &DeleteIndexed,
                                                .enumerator = &EnumerateIndexed,
                                                .data = ub::CallbackData::For(store)});
    const auto instance = Instance(f, shape);
    Expose(f.context, "slots", instance);

    CHECK(EvalInt(f.context, "slots[3]") == 30);
    CHECK(EvalInt(f.context, "slots['3']") == 30);
    CHECK(EvalError(f.context, "slots[4]").starts_with("KeyError"));
    CHECK(Eval(f.context, "slots[7] = 70").IsUndefined());
    CHECK(store.indexed[7] == 70);
    CHECK(EvalTruth(f.context, "7 in slots and 9 not in slots"));
    CHECK(EvalText(f.context, "repr(list(slots))") == "[3, 7]");
    CHECK(Eval(f.context, "del slots[7]").IsUndefined());
    CHECK(store.indexed.find(7) == store.indexed.end());
    CHECK(instance.Get(f.context, 3U)->To<ub::Integer>()->Int32Value() == 30);
    // Not an index: past 2^32 - 2, and not canonical, are named keys.
    CHECK(Eval(f.context, "slots[4294967295] = 1\nslots['03'] = 2").IsUndefined());
    CHECK(store.indexed.size() == 1);
    CHECK(EvalTruth(f.context, "slots[4294967295] == 1 and slots['03'] == 2"));
}

TEST_CASE("interceptors: a named handler does not see indexed access, nor the other way round") {
    Fixture f;
    Store store;
    store.named["5"] = 55;
    store.indexed[5] = 5000;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed, .data = ub::CallbackData::For(store)});
    shape.SetHandler(ub::IndexedPropertyHandler{.getter = &GetIndexed, .data = ub::CallbackData::For(store)});
    Expose(f.context, "both", Instance(f, shape));

    CHECK(EvalInt(f.context, "both[5]") == 5000);
    CHECK(EvalInt(f.context, "both['5']") == 5000);
    CHECK(EvalTruth(f.context, "getattr(both, 'alpha', None) is None"));
    CHECK(store.getterCalls == 1);
}

TEST_CASE("interceptors: a getter on its own is a whole handler, and a write with no setter is ordinary") {
    Fixture f;
    Store store;
    store.named["only"] = 1;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed, .data = ub::CallbackData::For(store)});
    const auto instance = Instance(f, shape);
    Expose(f.context, "sandbox", instance);

    CHECK(EvalInt(f.context, "sandbox.only") == 1);
    CHECK(EvalTruth(f.context, "'only' in sandbox"));
    CHECK(Eval(f.context, "sandbox.other = 2").IsUndefined());
    CHECK(instance.HasOwn(f.context, Str(f.iso(), "other")).value_or(false));
    CHECK(store.named.find("other") == store.named.end());
}

TEST_CASE("interceptors: what a setter writes to its return slot changes nothing") {
    Fixture f;
    Store store;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{
        .getter = &GetNamed, .setter = &SetNamedAndScribble, .data = ub::CallbackData::For(store)});
    Expose(f.context, "sandbox", Instance(f, shape));
    CHECK(EvalInt(f.context, "sandbox.written = 7\nsandbox.written") == 7);
    CHECK(store.named["written"] == 7);
}

TEST_CASE("interceptors: an enumerator with nothing to say means no own keys, not no properties") {
    Fixture f;
    Store store;
    store.named["notListed"] = 1;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed,
                                              .query = &QueryNamed,
                                              .enumerator = &EnumerateNothing,
                                              .data = ub::CallbackData::For(store)});
    Expose(f.context, "sandbox", Instance(f, shape));
    CHECK(EvalInt(f.context, "len(sandbox)") == 0);
    CHECK(EvalInt(f.context, "sandbox.notListed") == 1);
    CHECK(EvalTruth(f.context, "'notListed' in sandbox"));
}

TEST_CASE("interceptors: a hook is told the object it lives on and the one it was reached through") {
    Fixture f;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetHolderOrThis});
    Expose(f.context, "sandbox", Instance(f, shape));

    CHECK(EvalTruth(f.context, "sandbox.holder is sandbox and sandbox.receiver is sandbox"));
    // Through something that inherits from it: CPython gives the lookup both.
    CHECK(EvalTruth(f.context,
                    "child = unibind.Object()\nchild.__proto__ = sandbox\n"
                    "child.holder is sandbox and child.receiver is child"));
}

TEST_CASE("interceptors: a hook that throws has intercepted the access whatever it returned") {
    Fixture f;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.Set("boom", ub::Constant(11));
    shape.SetHandler(ub::NamedPropertyHandler{
        .getter = &ThrowsAndDeclines, .setter = &SetterThrowsAndDeclines, .deleter = &DeleterThrowsAndDeclines});
    const auto instance = Instance(f, shape);
    Expose(f.context, "probe", instance);

    CHECK(EvalText(f.context,
                   "try:\n    probe.boom\n    r = 'answered'\nexcept TypeError as e:\n    r = 'threw: ' + str(e)\nr") ==
          "threw: the hook threw");
    CHECK_FALSE(f.iso().HasPendingException());
    CHECK(EvalError(f.context, "probe.boom = 1") == "TypeError: the setter threw");
    CHECK(EvalError(f.context, "del probe.boom") == "TypeError: the deleter threw");
    {
        ub::TryCatch handler(f.iso());
        CHECK_FALSE(instance.Get(f.context, "boom").has_value());
        CHECK(handler.HasCaught());
    }
    // A key the hooks do not throw for reaches the ordinary property.
    CHECK(EvalInt(f.context, "probe.quiet = 5\nprobe.quiet") == 5);
    // And the delete that threw did not happen.
    const auto attributes = instance.GetOwnPropertyNames(f.context);
    REQUIRE(attributes.has_value());
    CHECK(attributes->Length() == 2);
}

TEST_CASE("interceptors: Python's own dunder lookups do not reach a catch-all") {
    Fixture f;
    Store store;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &AnswersEverything, .data = ub::CallbackData::For(store)});
    Expose(f.context, "sandbox", Instance(f, shape));

    CHECK(EvalInt(f.context, "sandbox.anything") == 8);
    CHECK(EvalInt(f.context, "sandbox['__len__']") == 7);
    const int before = store.getterCalls;
    // repr, copy and hasattr look up protocol names on the instance; a
    // sandbox that answered them would break every one of them.
    CHECK(EvalTruth(f.context, "import copy\nr = repr(sandbox)\nnot hasattr(sandbox, '__missing__') and r == '{}'"));
    CHECK(store.getterCalls == before);
    CHECK(EvalInt(f.context, "sandbox.more") == 4);
    CHECK(store.lastKey == "more");
}

TEST_CASE("interceptors: a symbol key reaches the named handler as a symbol") {
    Fixture f;
    Store store;
    const auto shape = ub::ObjectTemplate::New(f.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed, .data = ub::CallbackData::For(store)});
    Expose(f.context, "sandbox", Instance(f, shape));
    CHECK(EvalTruth(f.context, "s = unibind.Symbol('k')\nsandbox.x = 1\n(s in sandbox) is False"));
    // GetNamed declines symbols: they are not in its string store.
    CHECK(store.getterCalls == 0);
}
