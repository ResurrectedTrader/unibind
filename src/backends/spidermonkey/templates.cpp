// Templates, classes and the native state a class instance carries.
//
// A `unibind` template is an isolate-owned descriptor, not a JS object, so this
// file is mostly bookkeeping plus one function - `Materialise` - that turns a
// descriptor into a real constructor and prototype in one realm. That is the
// shape the public header asks for (see unibind/template.h), and it happens to be
// the only shape SpiderMonkey can offer: there is no engine-side "template"
// here to hold handles for us.
//
// Realms come and go while templates do not, so a template's materialisation
// is cached ON the realm - in a hidden slot of its global - rather than in the
// template. The cache then dies with the realm, traced by the engine, with no
// bookkeeping of ours to get wrong.

#include <js/Proxy.h>
#include <js/WeakMap.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

#include "internal.h"

namespace ub::detail {

namespace {

constexpr std::size_t FUNCTION_NAME_SLOT = 1;
constexpr std::size_t INSTANCE_NATIVE_SLOT = 0;
constexpr std::size_t INSTANCE_CLASS_SLOT = 1;
/// The template whose interceptor answers for this object, if any.
constexpr std::size_t INSTANCE_TEMPLATE_SLOT = 2;
constexpr std::uint32_t INSTANCE_SLOT_COUNT = 3;

}  // namespace

// ---------------------------------------------------------------------------
// The records
// ---------------------------------------------------------------------------

/// One declaration on a template, replayed into every realm the template is
/// materialised in.
struct TemplateEntry {
    enum class Kind : std::uint8_t { Constant, Method, SymbolMethod, Accessor, Child };

    Kind kind = Kind::Constant;
    std::string name;
    /// A `Constant` holds a `string_view`; the template outlives whatever the
    /// caller pointed it at, so the bytes are copied here.
    std::string text;
    WellKnownSymbol symbolKey = WellKnownSymbol::Iterator;
    Constant constant;
    CallbackRecord* record = nullptr;
    TemplateRec* child = nullptr;
    PropertyAttribute attributes = PropertyAttribute::None;
};

struct TemplateRec {
    Isolate* owner = nullptr;
    std::uint32_t id = 0;
    bool isFunction = false;
    CallbackRecord* callRecord = nullptr;
    std::string className;
    std::vector<TemplateEntry> entries;

    NamedPropertyHandler named{};
    bool hasNamed = false;
    IndexedPropertyHandler indexed{};
    bool hasIndexed = false;

    TemplateRec* prototype = nullptr;
    TemplateRec* instance = nullptr;
    TemplateRec* parent = nullptr;
    ClassRec* ownerClass = nullptr;
    /// Set on an instance template: the function template it belongs to. An
    /// instance made straight off `InstanceTemplate()` still has to come out
    /// with the constructor's prototype, or `HasInstance` disowns it.
    TemplateRec* instanceOf = nullptr;
    /// Set on a prototype template: the function template it belongs to.
    TemplateRec* prototypeOf = nullptr;
    /// Whether the template has been instantiated - or something that
    /// instantiates it with itself has. From then on its class name, parent and
    /// handlers are fixed, as they are on V8, where changing them is a fatal
    /// error; see `unibind/template.h`.
    bool sealed = false;
};

struct ClassRec {
    Isolate* owner = nullptr;
    std::string name;
    TypeId nativeType;
    NativeConstructor constructor = nullptr;
    /// `Class<T>::ConstructOrCall` rather than `Construct`: a plain call makes
    /// an instance instead of being a TypeError. Default false, because that is
    /// what a JavaScript `class` declaration does.
    bool callableWithoutNew = false;
    TemplateRec* function = nullptr;
    JSClassOps ops{};
    JSClass klass{};
};

void DestroyTemplate(TemplateRec* tpl) noexcept {
    delete tpl;
}
void DestroyClass(ClassRec* rec) noexcept {
    delete rec;
}

namespace {

/// Mark `rec`, and everything instantiating it instantiates with it, as
/// instantiated: the function template an object template belongs to, a
/// function template's parent and its two object templates, and every
/// template set as a property on any of them. The same closure the V8 backend
/// seals, so a late shape call is ignored on both at the same moment.
void Seal(TemplateRec* rec) noexcept {
    if (rec == nullptr || rec->sealed) {
        return;
    }
    rec->sealed = true;
    Seal(rec->instanceOf);
    Seal(rec->prototypeOf);
    Seal(rec->parent);
    Seal(rec->prototype);
    Seal(rec->instance);
    for (const TemplateEntry& entry : rec->entries) {
        if (entry.kind == TemplateEntry::Kind::Child) {
            Seal(entry.child);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The per-realm cache
// ---------------------------------------------------------------------------

namespace {

/// The realm's template cache: a plain object in a hidden slot of the global,
/// mapping "c<id>" to a materialised constructor and "p<id>" to its prototype.
JSObject* CacheObject(JSContext* cx, const Context& context, bool create) {
    JS::RootedObject global(cx, GlobalOf(context));
    JS::Value slot = JS::GetReservedSlot(global, GLOBAL_TEMPLATE_CACHE_SLOT);
    if (slot.isObject()) {
        return &slot.toObject();
    }
    if (!create) {
        return nullptr;
    }
    JSObject* cache = JS_NewPlainObject(cx);
    if (cache == nullptr) {
        return nullptr;
    }
    JS::SetReservedSlot(global, GLOBAL_TEMPLATE_CACHE_SLOT, JS::ObjectValue(*cache));
    return cache;
}

[[nodiscard]] std::string CacheKey(char kind, std::uint32_t id) {
    return std::string(1, kind) + std::to_string(id);
}

bool CacheLookup(JSContext* cx, const Context& context, char kind, std::uint32_t id, JS::MutableHandleObject out) {
    JS::RootedObject cache(cx, CacheObject(cx, context, false));
    if (cache == nullptr) {
        return false;
    }
    const std::string key = CacheKey(kind, id);
    JS::RootedValue value(cx);
    if (!JS_GetProperty(cx, cache, key.c_str(), &value) || !value.isObject()) {
        return false;
    }
    out.set(&value.toObject());
    return true;
}

bool CacheStore(JSContext* cx, const Context& context, char kind, std::uint32_t id, JS::HandleObject value) {
    JS::RootedObject cache(cx, CacheObject(cx, context, true));
    if (cache == nullptr) {
        return false;
    }
    const std::string key = CacheKey(kind, id);
    JS::RootedValue wrapped(cx, JS::ObjectValue(*value));
    return JS_SetProperty(cx, cache, key.c_str(), wrapped);
}

}  // namespace

// ---------------------------------------------------------------------------
// Accessor trampolines
//
// SpiderMonkey hands a native accessor no property name, so the name travels
// in the function object's second reserved slot - the same place the callback
// record uses its first.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] CallbackRecord* RecordOfCallee(const JS::CallArgs& args) {
    return static_cast<CallbackRecord*>(
        js::GetFunctionNativeReserved(&args.callee(), FUNCTION_RECORD_SLOT).toPrivate());
}

[[nodiscard]] JS::Value NameOfCallee(const JS::CallArgs& args) {
    return js::GetFunctionNativeReserved(&args.callee(), FUNCTION_NAME_SLOT);
}

bool AccessorGetterTrampoline(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    // Boxed before anything is copied out of the callee: it allocates, and a
    // moving collector would leave such a copy stale.
    JS::RootedValue receiver(cx, args.thisv());
    if (!ReceiverObject(cx, &receiver)) {
        return false;
    }
    CallbackRecord* record = RecordOfCallee(args);
    const JS::Value name = NameOfCallee(args);

    auto* isolate = static_cast<Isolate*>(JS_GetContextPrivate(cx));
    CallFrame frame(*isolate, &args);
    const SlotIndex self = frame.frame().Push(receiver);
    // Into the frame before anything else can collect: `name` was copied out
    // of the callee's reserved slot and is not rooted where it stands.
    const SlotIndex nameSlot = frame.frame().Push(name);
    args.rval().setUndefined();

    CallbackState state{.owner = isolate,
                        .frame = &frame.frame(),
                        .context = CurrentContext(cx),
                        .call = &args,
                        .result = args.rval().address(),
                        .thisSlot = self,
                        .holderSlot = self,
                        .data = record->data};
    record->getter(Local<Name>::FromSlot(SlotOrEmpty(frame.frame(), nameSlot)), PropertyCallbackInfo(state));
    return FinishNativeCall(cx, args);
}

bool AccessorSetterTrampoline(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedValue receiver(cx, args.thisv());
    if (!ReceiverObject(cx, &receiver)) {
        return false;
    }
    CallbackRecord* record = RecordOfCallee(args);
    const JS::Value name = NameOfCallee(args);
    const JS::Value incoming = args.get(0);

    auto* isolate = static_cast<Isolate*>(JS_GetContextPrivate(cx));
    CallFrame frame(*isolate, &args);
    const SlotIndex self = frame.frame().Push(receiver);
    const SlotIndex nameSlot = frame.frame().Push(name);
    const SlotIndex valueSlot = frame.frame().Push(incoming);
    // A setter's result is ignored by the language, so it is given somewhere
    // harmless to write; the public API does not even offer it a return slot.
    JS::RootedValue discarded(cx);

    CallbackState state{.owner = isolate,
                        .frame = &frame.frame(),
                        .context = CurrentContext(cx),
                        .call = &args,
                        .result = discarded.address(),
                        .thisSlot = self,
                        .holderSlot = self,
                        .data = record->data};
    record->setter(Local<Name>::FromSlot(SlotOrEmpty(frame.frame(), nameSlot)),
                   Local<Value>::FromSlot(SlotOrEmpty(frame.frame(), valueSlot)), PropertyCallbackInfo(state));
    args.rval().setUndefined();
    return !JS_IsExceptionPending(cx);
}

[[nodiscard]] JSObject* NewAccessorFunction(JSContext* cx, JSNative native, CallbackRecord* record,
                                            const std::string& name) {
    // Unnamed, as V8 leaves both halves of a native accessor: `name` is what
    // the callback is told it answers for, below, not what script reads off
    // the function.
    JSFunction* function = NewNamedFunction(cx, native, native == &AccessorSetterTrampoline ? 1 : 0, 0, "");
    if (function == nullptr) {
        return nullptr;
    }
    JSObject* object = JS_GetFunctionObject(function);
    js::SetFunctionNativeReserved(object, FUNCTION_RECORD_SLOT, JS::PrivateValue(record));
    JSString* text = MakeTextString(cx, name);
    if (text == nullptr) {
        return nullptr;
    }
    js::SetFunctionNativeReserved(object, FUNCTION_NAME_SLOT, JS::StringValue(text));
    return object;
}

}  // namespace

bool DefineAccessor(JSContext* cx, JS::HandleObject target, const std::string& name, CallbackRecord* record,
                    PropertyAttribute attributes, JS::ObjectOpResult* result) {
    JS::RootedObject getter(cx);
    JS::RootedObject setter(cx);
    if (record->getter != nullptr) {
        getter = NewAccessorFunction(cx, &AccessorGetterTrampoline, record, name);
        if (getter == nullptr) {
            return false;
        }
    }
    if (record->setter != nullptr) {
        setter = NewAccessorFunction(cx, &AccessorSetterTrampoline, record, name);
        if (setter == nullptr) {
            return false;
        }
    }
    JS::RootedId id(cx);
    if (!NameToId(cx, name, &id)) {
        return false;
    }
    // JSPROP_READONLY is meaningless on an accessor, and SpiderMonkey rejects
    // it, so the ReadOnly bit is carried by the absence of a setter instead.
    const unsigned native = ToNativeAttributes(attributes) & ~static_cast<unsigned>(JSPROP_READONLY);
    if (result == nullptr) {
        return JS_DefinePropertyById(cx, target, id, getter, setter, native);
    }
    JS::Rooted<JS::PropertyDescriptor> descriptor(cx, JS::PropertyDescriptor::Accessor(getter, setter, native));
    return JS_DefinePropertyById(cx, target, id, descriptor, *result);
}

// ---------------------------------------------------------------------------
// Replaying a template's declarations onto a real object
// ---------------------------------------------------------------------------

namespace {

/// Define `value` on `target` under a declared name. By key rather than by the
/// engine's `const char*` overload, which reads a name as Latin-1; see
/// `NameToId`.
bool DefineNamed(JSContext* cx, JS::HandleObject target, const std::string& name, const JS::Value& value,
                 PropertyAttribute attributes) {
    JS::RootedValue rooted(cx, value);
    JS::RootedId id(cx);
    return NameToId(cx, name, &id) && JS_DefinePropertyById(cx, target, id, rooted, ToNativeAttributes(attributes));
}

bool ApplyConstant(JSContext* cx, JS::HandleObject target, const TemplateEntry& entry) {
    JS::RootedValue value(cx);
    switch (entry.constant.GetKind()) {
        case Constant::Kind::Undefined:
            value.setUndefined();
            break;
        case Constant::Kind::Null:
            value.setNull();
            break;
        case Constant::Kind::Boolean:
            value.setBoolean(entry.constant.AsBoolean());
            break;
        case Constant::Kind::Number:
            value.setNumber(entry.constant.AsNumber());
            break;
        case Constant::Kind::Integer:
            value.setInt32(entry.constant.AsInteger());
            break;
        case Constant::Kind::String: {
            JSString* text = MakeTextString(cx, entry.text);
            if (text == nullptr) {
                return false;
            }
            value.setString(text);
            break;
        }
    }
    return DefineNamed(cx, target, entry.name, value, entry.attributes);
}

bool Materialise(JSContext* cx, const Context& context, TemplateRec* tpl, JS::MutableHandleObject functionOut,
                 JS::MutableHandleObject prototypeOut);

bool ApplyEntries(JSContext* cx, const Context& context, JS::HandleObject target, TemplateRec* tpl);

JSObject* NewInstanceOf(JSContext* cx, const Context& context, TemplateRec* tpl, JS::HandleObject prototypeOverride);

bool ApplyEntry(JSContext* cx, const Context& context, JS::HandleObject target, const TemplateEntry& entry) {
    switch (entry.kind) {
        case TemplateEntry::Kind::Constant:
            return ApplyConstant(cx, target, entry);

        case TemplateEntry::Kind::Method: {
            JS::RootedObject function(cx, NewNativeFunction(cx, entry.record, entry.name));
            if (function == nullptr) {
                return false;
            }
            return DefineNamed(cx, target, entry.name, JS::ObjectValue(*function), entry.attributes);
        }

        case TemplateEntry::Kind::SymbolMethod: {
            JS::RootedObject function(cx, NewNativeFunction(cx, entry.record, entry.name));
            if (function == nullptr) {
                return false;
            }
            JS::Symbol* symbol = nullptr;
            switch (entry.symbolKey) {
                case WellKnownSymbol::Iterator:
                    symbol = JS::GetWellKnownSymbol(cx, JS::SymbolCode::iterator);
                    break;
                case WellKnownSymbol::AsyncIterator:
                    symbol = JS::GetWellKnownSymbol(cx, JS::SymbolCode::asyncIterator);
                    break;
                case WellKnownSymbol::HasInstance:
                    symbol = JS::GetWellKnownSymbol(cx, JS::SymbolCode::hasInstance);
                    break;
                case WellKnownSymbol::ToPrimitive:
                    symbol = JS::GetWellKnownSymbol(cx, JS::SymbolCode::toPrimitive);
                    break;
                case WellKnownSymbol::ToStringTag:
                    symbol = JS::GetWellKnownSymbol(cx, JS::SymbolCode::toStringTag);
                    break;
            }
            if (symbol == nullptr) {
                return false;
            }
            JS::Rooted<JS::Symbol*> rooted(cx, symbol);
            JS::RootedId id(cx, JS::PropertyKey::Symbol(rooted));
            return JS_DefinePropertyById(cx, target, id, function, ToNativeAttributes(PropertyAttribute::DontEnum));
        }

        case TemplateEntry::Kind::Accessor:
            return DefineAccessor(cx, target, entry.name, entry.record, entry.attributes);

        case TemplateEntry::Kind::Child: {
            JS::RootedObject function(cx);
            JS::RootedObject prototype(cx);
            if (entry.child->isFunction) {
                if (!Materialise(cx, context, entry.child, &function, &prototype)) {
                    return false;
                }
            } else {
                // A whole instance of the inner template, as `NewInstance`
                // would make one - with its handler, if it declares one - and
                // not merely its declarations replayed onto a plain object.
                JS::RootedObject noOverride(cx);
                function = NewInstanceOf(cx, context, entry.child, noOverride);
                if (function == nullptr) {
                    return false;
                }
            }
            return DefineNamed(cx, target, entry.name, JS::ObjectValue(*function), entry.attributes);
        }
    }
    return false;
}

// A template's function members, made once per realm and shared by every
// object the template stamps - as V8 makes a template's functions once per
// context. Replaying an instance template used to make a getter, a setter and
// a name string per accessor per instance, which is most of what wrapping an
// object with many members cost.
//
// Cached per realm as an array beside the materialised constructors: for entry
// i, element 3i is its method or getter, 3i+1 its setter, and 3i+2 its name as
// a property key. The array's length is the entry count it was built for, so
// entries declared after it was built make it be built again.
constexpr std::uint32_t SHARED_STRIDE = 3;

[[nodiscard]] bool HasSharedFunctions(const TemplateRec* tpl) {
    for (const TemplateEntry& entry : tpl->entries) {
        if (entry.kind == TemplateEntry::Kind::Accessor || entry.kind == TemplateEntry::Kind::Method) {
            return true;
        }
    }
    return false;
}

bool SharedFunctions(JSContext* cx, const Context& context, TemplateRec* tpl, JS::MutableHandleObject out) {
    const auto wanted = static_cast<std::uint32_t>(tpl->entries.size() * SHARED_STRIDE);
    if (CacheLookup(cx, context, 'f', tpl->id, out)) {
        std::uint32_t length = 0;
        if (!JS::GetArrayLength(cx, out, &length)) {
            return false;
        }
        if (length == wanted) {
            return true;
        }
    }
    JS::RootedObject shared(cx, JS::NewArrayObject(cx, wanted));
    if (shared == nullptr) {
        return false;
    }
    JS::RootedValue first(cx);
    JS::RootedValue second(cx);
    JS::RootedValue key(cx);
    JS::RootedId id(cx);
    for (std::uint32_t i = 0; i < tpl->entries.size(); ++i) {
        const TemplateEntry& entry = tpl->entries[i];
        first.setUndefined();
        second.setUndefined();
        key.setUndefined();
        if (entry.kind == TemplateEntry::Kind::Accessor) {
            if (entry.record->getter != nullptr) {
                JSObject* getter = NewAccessorFunction(cx, &AccessorGetterTrampoline, entry.record, entry.name);
                if (getter == nullptr) {
                    return false;
                }
                first.setObject(*getter);
            }
            if (entry.record->setter != nullptr) {
                JSObject* setter = NewAccessorFunction(cx, &AccessorSetterTrampoline, entry.record, entry.name);
                if (setter == nullptr) {
                    return false;
                }
                second.setObject(*setter);
            }
        } else if (entry.kind == TemplateEntry::Kind::Method) {
            JSObject* function = NewNativeFunction(cx, entry.record, entry.name);
            if (function == nullptr) {
                return false;
            }
            first.setObject(*function);
        }
        if (entry.kind == TemplateEntry::Kind::Accessor || entry.kind == TemplateEntry::Kind::Method) {
            if (!NameToId(cx, entry.name, &id) || !JS_IdToValue(cx, id, &key)) {
                return false;
            }
        }
        if (!JS_SetElement(cx, shared, (i * SHARED_STRIDE), first) ||
            !JS_SetElement(cx, shared, (i * SHARED_STRIDE) + 1, second) ||
            !JS_SetElement(cx, shared, (i * SHARED_STRIDE) + 2, key)) {
            return false;
        }
    }
    if (!CacheStore(cx, context, 'f', tpl->id, shared)) {
        return false;
    }
    out.set(shared);
    return true;
}

/// Define entry `index` of `tpl` - an accessor or a method - on `target` from
/// the realm's shared functions.
bool ApplySharedEntry(JSContext* cx, JS::HandleObject target, JS::HandleObject shared, std::uint32_t index,
                      const TemplateEntry& entry) {
    JS::RootedValue first(cx);
    JS::RootedValue second(cx);
    JS::RootedValue key(cx);
    JS::RootedId id(cx);
    if (!JS_GetElement(cx, shared, (index * SHARED_STRIDE), &first) ||
        !JS_GetElement(cx, shared, (index * SHARED_STRIDE) + 1, &second) ||
        !JS_GetElement(cx, shared, (index * SHARED_STRIDE) + 2, &key) || !JS_ValueToId(cx, key, &id)) {
        return false;
    }
    if (entry.kind == TemplateEntry::Kind::Method) {
        return JS_DefinePropertyById(cx, target, id, first, ToNativeAttributes(entry.attributes));
    }
    JS::RootedObject getter(cx, first.isObject() ? &first.toObject() : nullptr);
    JS::RootedObject setter(cx, second.isObject() ? &second.toObject() : nullptr);
    // As DefineAccessor: no JSPROP_READONLY on an accessor.
    const unsigned native = ToNativeAttributes(entry.attributes) & ~static_cast<unsigned>(JSPROP_READONLY);
    return JS_DefinePropertyById(cx, target, id, getter, setter, native);
}

bool ApplyEntries(JSContext* cx, const Context& context, JS::HandleObject target, TemplateRec* tpl) {
    if (tpl == nullptr) {
        return true;
    }
    JS::RootedObject shared(cx);
    if (HasSharedFunctions(tpl) && !SharedFunctions(cx, context, tpl, &shared)) {
        return false;
    }
    for (std::uint32_t i = 0; i < tpl->entries.size(); ++i) {
        const TemplateEntry& entry = tpl->entries[i];
        const bool isShared = entry.kind == TemplateEntry::Kind::Accessor || entry.kind == TemplateEntry::Kind::Method;
        if (isShared ? !ApplySharedEntry(cx, target, shared, i, entry) : !ApplyEntry(cx, context, target, entry)) {
            return false;
        }
    }
    return true;
}

/// The constructor a `FunctionTemplate` or a `Class<T>` produces.
bool ConstructorTrampoline(JSContext* cx, unsigned argc, JS::Value* vp);

/// Builds (and caches) the constructor function and prototype object this
/// template names in this realm.
bool Materialise(JSContext* cx, const Context& context, TemplateRec* tpl, JS::MutableHandleObject functionOut,
                 JS::MutableHandleObject prototypeOut) {
    Seal(tpl);
    if (CacheLookup(cx, context, 'c', tpl->id, functionOut)) {
        // Both halves are stored together, so one without the other is a cache
        // that cannot be trusted - and trusting it would hand back a null
        // prototype, which is instances with no methods, no `instanceof` and
        // nothing reported. Refusing is the only answer that is not silently
        // wrong for the life of the realm.
        return CacheLookup(cx, context, 'p', tpl->id, prototypeOut);
    }

    // The parent first, so this prototype can inherit from it. Only the
    // prototypes are chained, as V8's `Inherit` chains them: the constructor
    // itself keeps `Function.prototype`, and the parent's statics stay the
    // parent's.
    JS::RootedObject parentPrototype(cx);
    if (tpl->parent != nullptr) {
        JS::RootedObject parentFunction(cx);
        if (!Materialise(cx, context, tpl->parent, &parentFunction, &parentPrototype)) {
            return false;
        }
    }

    const std::string name = tpl->className.empty() ? std::string("") : tpl->className;
    JSFunction* raw = NewNamedFunction(cx, &ConstructorTrampoline, 0, JSFUN_CONSTRUCTOR, name);
    if (raw == nullptr) {
        return false;
    }
    JS::RootedObject function(cx, JS_GetFunctionObject(raw));
    js::SetFunctionNativeReserved(function, FUNCTION_RECORD_SLOT, JS::PrivateValue(tpl));

    JS::RootedObject prototype(cx);
    if (parentPrototype != nullptr) {
        prototype = JS_NewObjectWithGivenProto(cx, nullptr, parentPrototype);
    } else {
        prototype = JS_NewPlainObject(cx);
    }
    if (prototype == nullptr) {
        return false;
    }

    // Cache before applying entries: a template that mentions itself (a static
    // holding its own constructor, say) would otherwise recurse forever.
    if (!CacheStore(cx, context, 'c', tpl->id, function) || !CacheStore(cx, context, 'p', tpl->id, prototype)) {
        return false;
    }

    if (!ApplyEntries(cx, context, prototype, tpl->prototype)) {
        return false;
    }
    if (!ApplyEntries(cx, context, function, tpl)) {
        return false;
    }

    JS::RootedValue prototypeValue(cx, JS::ObjectValue(*prototype));
    // Writable, and neither enumerable nor configurable: the `prototype` every
    // ordinary function has, and what V8 gives a function made from a template.
    if (!JS_DefineProperty(cx, function, "prototype", prototypeValue, JSPROP_PERMANENT)) {
        return false;
    }
    JS::RootedValue functionValue(cx, JS::ObjectValue(*function));
    if (!JS_DefineProperty(cx, prototype, "constructor", functionValue, 0)) {
        return false;
    }

    functionOut.set(function);
    prototypeOut.set(prototype);
    return true;
}

void InstanceFinalize(JS::GCContext* gcx, JSObject* object);

/// The class of an interceptor's *target* when the template is not a
/// `Class<T>`: an ordinary object plus the three reserved slots every object
/// this backend makes carries, so the hooks can be found from the target.
const JSClassOps TARGET_CLASS_OPS = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &InstanceFinalize, nullptr, nullptr, nullptr,
};
const JSClass TARGET_CLASS = {
    "ub::Object", JSCLASS_HAS_RESERVED_SLOTS(INSTANCE_SLOT_COUNT) | JSCLASS_FOREGROUND_FINALIZE, &TARGET_CLASS_OPS};

// ---------------------------------------------------------------------------
// Interceptors
//
// V8's interceptor is consulted on EVERY named (or indexed) access, before the
// object's own properties, and may decline. SpiderMonkey's nearest native
// hook, `JSClassOps::resolve`, is not that: it fires only when a property is
// missing, and what it defines then sticks. A resolve hook cannot express "ask
// me again next time", and it has no setter at all.
//
// So an object with an interceptor is a *proxy* here, whose target is the
// ordinary object carrying the template's declared properties and, for a
// class, its native state. That is the one construct on this engine whose
// traps are called on every access and may fall through to ordinary lookup -
// which is exactly the contract `Intercepted::No` names. The cost is an
// indirection per access on intercepted objects only; objects without a
// handler stay ordinary.
// ---------------------------------------------------------------------------

class InterceptorHandler final : public js::BaseProxyHandler {
   public:
    constexpr InterceptorHandler() : BaseProxyHandler(&FAMILY) {}

    static const char FAMILY;

    [[nodiscard]] static JSObject* TargetOf(JSObject* proxy) { return js::GetProxyTargetObject(proxy); }

    bool getOwnPropertyDescriptor(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                                  JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) const override;
    bool defineProperty(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, JS::Handle<JS::PropertyDescriptor> desc,
                        JS::ObjectOpResult& result) const override;
    bool ownPropertyKeys(JSContext* cx, JS::HandleObject proxy, JS::MutableHandleIdVector props) const override;
    bool delete_(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, JS::ObjectOpResult& result) const override;

    bool get(JSContext* cx, JS::HandleObject proxy, JS::HandleValue receiver, JS::HandleId id,
             JS::MutableHandleValue vp) const override;
    bool set(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, JS::HandleValue value, JS::HandleValue receiver,
             JS::ObjectOpResult& result) const override;
    bool has(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, bool* found) const override;
    bool hasOwn(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, bool* found) const override;

    bool getPrototype(JSContext* cx, JS::HandleObject proxy, JS::MutableHandleObject protop) const override;
    bool getPrototypeIfOrdinary(JSContext* cx, JS::HandleObject proxy, bool* isOrdinary,
                                JS::MutableHandleObject protop) const override;
    bool setPrototype(JSContext* cx, JS::HandleObject proxy, JS::HandleObject proto,
                      JS::ObjectOpResult& result) const override;
    bool preventExtensions(JSContext* cx, JS::HandleObject proxy, JS::ObjectOpResult& result) const override;
    bool isExtensible(JSContext* cx, JS::HandleObject proxy, bool* extensible) const override;

    const char* className(JSContext* cx, JS::HandleObject proxy) const override { return "Object"; }
};

const char InterceptorHandler::FAMILY = 0;
const InterceptorHandler INTERCEPTOR_HANDLER;

[[nodiscard]] bool IsInterceptorProxy(JSObject* object) {
    return js::IsProxy(object) && js::GetProxyHandler(object) == &INTERCEPTOR_HANDLER;
}

[[nodiscard]] TemplateRec* HooksOf(JSObject* target) {
    const JSClass* klass = JS::GetClass(target);
    if (klass == nullptr || !klass->hasFinalize() || klass->cOps->finalize != &InstanceFinalize) {
        return nullptr;
    }
    const JS::Value slot = JS::GetReservedSlot(target, INSTANCE_TEMPLATE_SLOT);
    return slot.isUndefined() ? nullptr : static_cast<TemplateRec*>(slot.toPrivate());
}

/// One interceptor hook call: a borrowed-nothing frame, a callback state, and
/// somewhere for a result to land.
class HookCall {
   public:
    HookCall(JSContext* cx, JS::HandleObject proxy)
        : isolate_(*static_cast<Isolate*>(JS_GetContextPrivate(cx))), frame_(isolate_, nullptr), result_(cx) {
        state_.owner = &isolate_;
        state_.frame = &frame_.frame();
        state_.context = CurrentContext(cx);
        // A root of its own, not a slot: the frame's vector reallocates, so the
        // address of a slot is not a place to leave a pointer for the engine.
        state_.result = result_.address();
        // Receiver and holder are the same object: the engine hands a trap the
        // proxy and nothing else, which is the case the header allows for.
        state_.thisSlot = frame_.frame().Push(JS::ObjectValue(*proxy));
        state_.holderSlot = state_.thisSlot;
    }

    [[nodiscard]] PropertyCallbackInfo Info(CallbackData data) {
        state_.data = data;
        return PropertyCallbackInfo(state_);
    }

    [[nodiscard]] Slot PushName(JSContext* cx, JS::HandleId id) {
        JS::RootedValue value(cx);
        if (!JS_IdToValue(cx, id, &value)) {
            value.setUndefined();
        }
        return Push(isolate_, value);
    }

    [[nodiscard]] Isolate& isolate() const noexcept { return isolate_; }

    /// What the hook wrote, in the compartment of whoever asked.
    ///
    /// A hook is entitled to enter another realm of the same isolate and answer
    /// with something it found there - that is what a sandbox object does, and
    /// `unibind/context.h` promises a value may be used with any realm of its
    /// isolate. The value it wrote is then a stranger to the realm the trap is
    /// returning into, and handing it back unwrapped is how a cross-compartment
    /// object escapes into a realm that cannot name it. `JS_WrapValue` is a
    /// no-op when there is nothing to do, which is the overwhelmingly common
    /// case.
    [[nodiscard]] bool TakeResult(JSContext* cx, JS::MutableHandleValue out) {
        out.set(result_);
        return JS_WrapValue(cx, out);
    }

   private:
    Isolate& isolate_;
    CallFrame frame_;
    // Declared after the frame and before the state: the frame's root is
    // outermost, this one nests inside it, and both unwind in order.
    JS::RootedValue result_;
    CallbackState state_;
};

/// An index if the key is one, for choosing between the two handler halves.
///
/// An array index is any integer below 2^32 - 1, which is what V8 hands the
/// indexed half. This engine keeps only those that fit an int32 as integer
/// keys and spells the rest as strings, so a string key is asked too - or
/// `o[3000000000]` would reach the named half, as text.
[[nodiscard]] bool AsIndex(JS::HandleId id, std::uint32_t* index) {
    if (id.isString()) {
        return js::StringIsArrayIndex(id.toLinearString(), index);
    }
    if (!id.isInt()) {
        return false;
    }
    const std::int32_t value = id.toInt();
    if (value < 0) {
        return false;
    }
    *index = static_cast<std::uint32_t>(value);
    return true;
}

bool InterceptorHandler::get(JSContext* cx, JS::HandleObject proxy, JS::HandleValue receiver, JS::HandleId id,
                             JS::MutableHandleValue vp) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    TemplateRec* tpl = HooksOf(target);
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(id, &index);
    if (tpl != nullptr) {
        if (isIndex && tpl->hasIndexed && tpl->indexed.getter != nullptr) {
            HookCall call(cx, proxy);
            if (tpl->indexed.getter(index, call.Info(tpl->indexed.data)) == Intercepted::Yes) {
                if (!call.TakeResult(cx, vp)) {
                    return false;
                }
                return !JS_IsExceptionPending(cx);
            }
            if (JS_IsExceptionPending(cx)) {
                return false;
            }
        } else if (!isIndex && tpl->hasNamed && tpl->named.getter != nullptr) {
            HookCall call(cx, proxy);
            const Slot name = call.PushName(cx, id);
            if (tpl->named.getter(Local<Name>::FromSlot(name), call.Info(tpl->named.data)) == Intercepted::Yes) {
                if (!call.TakeResult(cx, vp)) {
                    return false;
                }
                return !JS_IsExceptionPending(cx);
            }
            if (JS_IsExceptionPending(cx)) {
                return false;
            }
        }
    }
    // Not intercepted: the ordinary lookup, on the target and its prototype
    // chain - which is the proxy's prototype chain, by construction - with the
    // receiver the access was made on. Not the target: a getter up the chain
    // would see it as `this`, and the target is the one object script must
    // never hold, since nothing on it goes through a hook.
    return JS_ForwardGetPropertyTo(cx, target, id, receiver, vp);
}

bool InterceptorHandler::set(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, JS::HandleValue value,
                             JS::HandleValue receiver, JS::ObjectOpResult& result) const {
    // A write made on something that inherits from this object - V8 hands its
    // setter hook only a write made on the object itself. What the object has
    // there is still asked, through `getOwnPropertyDescriptor` and so through
    // the query and getter hooks, and the property lands on the receiver: the
    // language's own ordinary set, as V8 does it too.
    if (!receiver.isObject() || &receiver.toObject() != proxy) {
        return BaseProxyHandler::set(cx, proxy, id, value, receiver, result);
    }
    JS::RootedObject target(cx, TargetOf(proxy));
    TemplateRec* tpl = HooksOf(target);
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(id, &index);
    if (tpl != nullptr) {
        if (isIndex && tpl->hasIndexed && tpl->indexed.setter != nullptr) {
            HookCall call(cx, proxy);
            const Slot incoming = Push(call.isolate(), value);
            if (tpl->indexed.setter(index, Local<Value>::FromSlot(incoming), call.Info(tpl->indexed.data)) ==
                Intercepted::Yes) {
                return JS_IsExceptionPending(cx) ? false : result.succeed();
            }
            if (JS_IsExceptionPending(cx)) {
                return false;
            }
        } else if (!isIndex && tpl->hasNamed && tpl->named.setter != nullptr) {
            HookCall call(cx, proxy);
            const Slot name = call.PushName(cx, id);
            const Slot incoming = Push(call.isolate(), value);
            if (tpl->named.setter(Local<Name>::FromSlot(name), Local<Value>::FromSlot(incoming),
                                  call.Info(tpl->named.data)) == Intercepted::Yes) {
                return JS_IsExceptionPending(cx) ? false : result.succeed();
            }
            if (JS_IsExceptionPending(cx)) {
                return false;
            }
        }
    }
    // Declined: the ordinary set on the target, with this object as receiver,
    // so a setter up the chain sees it as `this` - see `get`. A data write
    // still lands on the target, through `defineProperty` below.
    return JS_ForwardSetPropertyTo(cx, target, id, value, receiver, result);
}

namespace {

/// Ask the query hook, then the getter, for what an own property looks like.
/// Empty means the interceptor declined and ordinary lookup should answer.
bool InterceptDescriptor(JSContext* cx, JS::HandleObject proxy, TemplateRec* tpl, JS::HandleId id, bool* intercepted,
                         JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
    *intercepted = false;
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(id, &index);

    Maybe<PropertyAttribute> attributes;
    if (isIndex && tpl->hasIndexed && tpl->indexed.query != nullptr) {
        HookCall call(cx, proxy);
        attributes = tpl->indexed.query(index, call.Info(tpl->indexed.data));
    } else if (!isIndex && tpl->hasNamed && tpl->named.query != nullptr) {
        HookCall call(cx, proxy);
        const Slot name = call.PushName(cx, id);
        attributes = tpl->named.query(Local<Name>::FromSlot(name), call.Info(tpl->named.data));
    }
    if (JS_IsExceptionPending(cx)) {
        return false;
    }

    JS::RootedValue value(cx);
    bool gotValue = false;
    if (isIndex && tpl->hasIndexed && tpl->indexed.getter != nullptr) {
        HookCall call(cx, proxy);
        gotValue = tpl->indexed.getter(index, call.Info(tpl->indexed.data)) == Intercepted::Yes;
        if (!call.TakeResult(cx, &value)) {
            return false;
        }
    } else if (!isIndex && tpl->hasNamed && tpl->named.getter != nullptr) {
        HookCall call(cx, proxy);
        const Slot name = call.PushName(cx, id);
        gotValue = tpl->named.getter(Local<Name>::FromSlot(name), call.Info(tpl->named.data)) == Intercepted::Yes;
        if (!call.TakeResult(cx, &value)) {
            return false;
        }
    }
    if (JS_IsExceptionPending(cx)) {
        return false;
    }

    if (!attributes && !gotValue) {
        return true;
    }
    *intercepted = true;
    desc.set(mozilla::Some(
        JS::PropertyDescriptor::Data(value, ToNativeAttributes(attributes.value_or(PropertyAttribute::None)))));
    return true;
}

}  // namespace

bool InterceptorHandler::getOwnPropertyDescriptor(
    JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    TemplateRec* tpl = HooksOf(target);
    if (tpl != nullptr) {
        bool intercepted = false;
        if (!InterceptDescriptor(cx, proxy, tpl, id, &intercepted, desc)) {
            return false;
        }
        if (intercepted) {
            return true;
        }
    }
    return JS_GetOwnPropertyDescriptorById(cx, target, id, desc);
}

bool InterceptorHandler::defineProperty(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                                        JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    return JS_DefinePropertyById(cx, target, id, desc, result);
}

bool InterceptorHandler::ownPropertyKeys(JSContext* cx, JS::HandleObject proxy, JS::MutableHandleIdVector props) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    if (!js::GetPropertyKeys(cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, props)) {
        return false;
    }
    TemplateRec* tpl = HooksOf(target);
    if (tpl == nullptr) {
        return true;
    }
    for (int pass = 0; pass < 2; ++pass) {
        NamedEnumeratorCallback named = tpl->hasNamed ? tpl->named.enumerator : nullptr;
        IndexedEnumeratorCallback indexed = tpl->hasIndexed ? tpl->indexed.enumerator : nullptr;
        if ((pass == 0 && named == nullptr) || (pass == 1 && indexed == nullptr)) {
            continue;
        }
        HookCall call(cx, proxy);
        Maybe<Local<Array>> keys =
            pass == 0 ? named(call.Info(tpl->named.data)) : indexed(call.Info(tpl->indexed.data));
        if (JS_IsExceptionPending(cx)) {
            return false;
        }
        // An optional holding an EMPTY handle is not an empty optional, and it
        // is what a hook hands back when its array could not be made:
        // resolving it would read through a null frame. Both mean "no own
        // keys here".
        if (!keys || keys->IsEmpty()) {
            continue;
        }
        JS::RootedValue element(cx);
        // Wrapped into this realm rather than unwrapped out of its own: a hook
        // may make its array in another realm, and reading an object from a
        // compartment that is not the current one is a compartment mismatch -
        // asserted by a debug engine, and undefined behaviour in a release one.
        JS::RootedValue listed(cx, Resolve(keys->slot()));
        if (!listed.isObject()) {
            continue;
        }
        if (!JS_WrapValue(cx, &listed)) {
            return false;
        }
        JS::RootedObject array(cx, &listed.toObject());
        std::uint32_t length = 0;
        if (!JS::GetArrayLength(cx, array, &length)) {
            return false;
        }
        for (std::uint32_t i = 0; i < length; ++i) {
            if (!JS_GetElement(cx, array, i, &element)) {
                return false;
            }
            JS::RootedId id(cx);
            if (!JS_ValueToId(cx, element, &id) || !props.append(id)) {
                return false;
            }
        }
    }
    // An object has no key twice, and V8 folds a hook's repeats - and a key the
    // object already has - into the first. Done in one pass that makes no GC
    // thing, so no key can move while the raw bits stand for it.
    try {
        std::unordered_set<std::uintptr_t> seen;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < props.length(); ++i) {
            if (seen.insert(props[i].get().asRawBits()).second) {
                props[kept++].set(props[i]);
            }
        }
        props.shrinkBy(props.length() - kept);
    } catch (const std::bad_alloc&) {
        JS_ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

bool InterceptorHandler::delete_(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                                 JS::ObjectOpResult& result) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    TemplateRec* tpl = HooksOf(target);
    std::uint32_t index = 0;
    const bool isIndex = AsIndex(id, &index);
    if (tpl != nullptr) {
        Maybe<bool> answer;
        if (isIndex && tpl->hasIndexed && tpl->indexed.deleter != nullptr) {
            HookCall call(cx, proxy);
            answer = tpl->indexed.deleter(index, call.Info(tpl->indexed.data));
        } else if (!isIndex && tpl->hasNamed && tpl->named.deleter != nullptr) {
            HookCall call(cx, proxy);
            const Slot name = call.PushName(cx, id);
            answer = tpl->named.deleter(Local<Name>::FromSlot(name), call.Info(tpl->named.data));
        }
        if (JS_IsExceptionPending(cx)) {
            return false;
        }
        if (answer) {
            return *answer ? result.succeed() : result.failCantDelete();
        }
    }
    return JS_DeletePropertyById(cx, target, id, result);
}

bool InterceptorHandler::has(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, bool* found) const {
    if (!hasOwn(cx, proxy, id, found)) {
        return false;
    }
    if (*found) {
        return true;
    }
    JS::RootedObject target(cx, TargetOf(proxy));
    return JS_HasPropertyById(cx, target, id, found);
}

bool InterceptorHandler::hasOwn(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, bool* found) const {
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);
    if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
        return false;
    }
    *found = desc.get().isSome();
    return true;
}

bool InterceptorHandler::getPrototype(JSContext* cx, JS::HandleObject proxy, JS::MutableHandleObject protop) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    return JS_GetPrototype(cx, target, protop);
}

bool InterceptorHandler::getPrototypeIfOrdinary(JSContext* cx, JS::HandleObject proxy, bool* isOrdinary,
                                                JS::MutableHandleObject protop) const {
    *isOrdinary = true;
    return getPrototype(cx, proxy, protop);
}

bool InterceptorHandler::setPrototype(JSContext* cx, JS::HandleObject proxy, JS::HandleObject proto,
                                      JS::ObjectOpResult& result) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    if (!JS_SetPrototype(cx, target, proto)) {
        return false;
    }
    return result.succeed();
}

bool InterceptorHandler::preventExtensions(JSContext* cx, JS::HandleObject proxy, JS::ObjectOpResult& result) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    return JS_PreventExtensions(cx, target, result);
}

bool InterceptorHandler::isExtensible(JSContext* cx, JS::HandleObject proxy, bool* extensible) const {
    JS::RootedObject target(cx, TargetOf(proxy));
    return JS_IsExtensible(cx, target, extensible);
}

/// The realm's record of which template made each of its template instances,
/// or null - with nothing pending - if `create` is false and there is none yet.
///
/// A weak map on the realm's template cache, keyed by the instance as script
/// sees it (the proxy, for an intercepted one) and holding the maker's
/// `TemplateRec` as a private value. It keeps no instance alive, and an
/// instance never changes realm, so its own realm's map is the one to ask.
/// Plain objects stay plain objects: an object of a class of our own would
/// answer `HasInstance` from a slot, but it would stop cloning the way a plain
/// object clones, which is what V8 does with a template instance.
JSObject* InstanceRegistry(JSContext* cx, JS::HandleObject cache, bool create) {
    JS::RootedValue found(cx);
    if (!JS_GetProperty(cx, cache, "instances", &found)) {
        return nullptr;
    }
    if (found.isObject()) {
        return &found.toObject();
    }
    if (!create) {
        return nullptr;
    }
    JS::RootedObject registry(cx, JS::NewWeakMapObject(cx));
    if (registry == nullptr) {
        return nullptr;
    }
    JS::RootedValue value(cx, JS::ObjectValue(*registry));
    if (!JS_DefineProperty(cx, cache, "instances", value, 0)) {
        return nullptr;
    }
    return registry;
}

bool RecordMaker(JSContext* cx, const Context& context, JS::HandleObject instance, TemplateRec* maker) {
    JS::RootedObject cache(cx, CacheObject(cx, context, true));
    if (cache == nullptr) {
        return false;
    }
    JS::RootedObject registry(cx, InstanceRegistry(cx, cache, true));
    if (registry == nullptr) {
        return false;
    }
    JS::RootedValue key(cx, JS::ObjectValue(*instance));
    JS::RootedValue value(cx, JS::PrivateValue(maker));
    return JS::SetWeakMapEntry(cx, registry, key, value);
}

/// The template that made `object`, which must not be a cross-compartment
/// wrapper; null, with nothing pending, if no template did.
bool MakerOf(JSContext* cx, JS::HandleObject object, TemplateRec** maker) {
    *maker = nullptr;
    JSObject* target = IsInterceptorProxy(object) ? InterceptorHandler::TargetOf(object) : object.get();
    const JSClass* klass = JS::GetClass(target);
    if (klass != nullptr && klass->hasFinalize() && klass->cOps->finalize == &InstanceFinalize) {
        const JS::Value slot = JS::GetReservedSlot(target, INSTANCE_CLASS_SLOT);
        if (!slot.isUndefined() && slot.toPrivate() != nullptr) {
            *maker = static_cast<ClassRec*>(slot.toPrivate())->function;
            return true;
        }
    }
    JS::RootedObject global(cx, JS::GetNonCCWObjectGlobal(object));
    const JSAutoRealm realm(cx, global);
    const JS::Value slot = JS::GetReservedSlot(global, GLOBAL_TEMPLATE_CACHE_SLOT);
    if (!slot.isObject()) {
        return true;
    }
    JS::RootedObject cache(cx, &slot.toObject());
    JS::RootedObject registry(cx, InstanceRegistry(cx, cache, false));
    if (registry == nullptr) {
        return !JS_IsExceptionPending(cx);
    }
    JS::RootedValue key(cx, JS::ObjectValue(*object));
    JS::RootedValue found(cx);
    if (!JS::GetWeakMapEntry(cx, registry, key, &found)) {
        return false;
    }
    if (!found.isUndefined()) {
        *maker = static_cast<TemplateRec*>(found.toPrivate());
    }
    return true;
}

/// A fresh instance of whatever `tpl` describes: a class instance if the
/// template belongs to a `Class<T>`, an ordinary object otherwise - wrapped in
/// an interceptor proxy if the shape declares a handler.
///
/// It makes the object and nothing else: a class instance comes back with its
/// native slot **empty**, and `AttachNative` is what fills it once there is
/// something to put there. Keeping the two apart is what lets a constructor
/// run with the instance as its receiver - `info.This()` has to be the object
/// being made, not a sentinel - and it is also what keeps the box out of the
/// engine until every fallible step of building the object is behind us.
// Stamping a class's instances from a model, as V8 stamps them from a map.
//
// Replaying an instance template defines its members one by one, on every
// instance. When every instance of a class would come out the same - the same
// class, prototype and members - one hidden model instance is stamped that way
// per realm, and each real instance takes its properties from the model in one
// step (JS_InitializePropertiesFromCompatibleNativeObject, which Gecko's DOM
// bindings use for the same purpose): it shares the model's shape rather than
// building its own. Reserved slots are not copied, so the native, which lives
// in one, stays each instance's own.
//
// Not for an intercepted instance (a proxy, not a native object), one made with
// a subclass's prototype (the model has the class's own), or a template with a
// nested template member (every instance gets an object of its own, which a
// copied shape would share). Those replay as before.
//
// The model is cached per realm beside the constructors, with the entry count
// it was stamped from: members declared after it was made make it be made
// again. It is never handed to script, and it carries no native, which the
// finalizer and every unwrap already treat as "not an instance".
[[nodiscard]] bool CanStampFromModel(const TemplateRec* shape) {
    for (const TemplateEntry& entry : shape->entries) {
        if (entry.kind == TemplateEntry::Kind::Child) {
            return false;
        }
    }
    return true;
}

bool ModelFor(JSContext* cx, const Context& context, TemplateRec* shape, ClassRec* owner, JS::HandleObject prototype,
              JS::MutableHandleObject out) {
    const auto entryCount = static_cast<std::int32_t>(shape->entries.size());
    JS::RootedObject cached(cx);
    if (CacheLookup(cx, context, 'm', shape->id, &cached)) {
        JS::RootedValue model(cx);
        JS::RootedValue count(cx);
        if (!JS_GetElement(cx, cached, 0, &model) || !JS_GetElement(cx, cached, 1, &count)) {
            return false;
        }
        if (model.isObject() && count.isInt32() && count.toInt32() == entryCount) {
            out.set(&model.toObject());
            return true;
        }
    }
    JS::RootedObject model(cx, JS_NewObjectWithoutMetadata(cx, &owner->klass, prototype));
    if (model == nullptr || !ApplyEntries(cx, context, model, shape)) {
        return false;
    }
    JS::RootedObject record(cx, JS::NewArrayObject(cx, 2));
    if (record == nullptr) {
        return false;
    }
    JS::RootedValue modelValue(cx, JS::ObjectValue(*model));
    JS::RootedValue countValue(cx, JS::Int32Value(entryCount));
    if (!JS_SetElement(cx, record, 0, modelValue) || !JS_SetElement(cx, record, 1, countValue) ||
        !CacheStore(cx, context, 'm', shape->id, record)) {
        return false;
    }
    out.set(model);
    return true;
}

JSObject* NewInstanceOf(JSContext* cx, const Context& context, TemplateRec* tpl, JS::HandleObject prototypeOverride) {
    Seal(tpl);
    // `tpl` is either a function template (instantiate what it constructs) or a
    // shape. A shape that is some function template's instance template still
    // gets that constructor's prototype.
    TemplateRec* constructor = tpl->isFunction ? tpl : tpl->instanceOf;
    TemplateRec* shape = tpl->isFunction ? tpl->instance : tpl;

    JS::RootedObject function(cx);
    JS::RootedObject prototype(cx);
    if (constructor != nullptr && !Materialise(cx, context, constructor, &function, &prototype)) {
        return nullptr;
    }
    // `new.target` decides the prototype when script subclassed this class:
    // `class Sub extends Foo` makes instances whose prototype is
    // `Sub.prototype`, and building them from the class's own prototype
    // instead loses every method the subclass declared - silently, with
    // `instanceof Sub` answering false and nothing reporting anything.
    // A subclass's instance has another prototype, and so cannot take the
    // class's model; `new` on the class itself passes the class's own.
    const bool subclassed = prototypeOverride != nullptr && prototypeOverride != prototype;
    if (prototypeOverride != nullptr) {
        prototype = prototypeOverride;
    }

    const bool intercepted = shape != nullptr && (shape->hasNamed || shape->hasIndexed);

    ClassRec* owner = constructor != nullptr ? constructor->ownerClass : nullptr;
    JS::RootedObject instance(cx);
    if (owner != nullptr) {
        instance = JS_NewObjectWithGivenProto(cx, &owner->klass, prototype);
    } else if (intercepted) {
        // A shape of no constructor makes an ordinary object, and an ordinary
        // object inherits from `Object.prototype` - the plain branch below gets
        // that from `JS_NewPlainObject`, and this one has to ask for it.
        if (prototype == nullptr) {
            prototype = JS::GetRealmObjectPrototype(cx);
            if (prototype == nullptr) {
                return nullptr;
            }
        }
        instance = JS_NewObjectWithGivenProto(cx, &TARGET_CLASS, prototype);
    } else if (prototype != nullptr) {
        instance = JS_NewObjectWithGivenProto(cx, nullptr, prototype);
    } else {
        instance = JS_NewPlainObject(cx);
    }
    if (instance == nullptr) {
        return nullptr;
    }

    if (owner != nullptr) {
        JS::SetReservedSlot(instance, INSTANCE_CLASS_SLOT, JS::PrivateValue(owner));
    }
    if (intercepted) {
        JS::SetReservedSlot(instance, INSTANCE_TEMPLATE_SLOT, JS::PrivateValue(shape));
    }

    if (owner != nullptr && !intercepted && !subclassed && shape != nullptr && CanStampFromModel(shape)) {
        JS::RootedObject model(cx);
        JS::RootedObject target(cx, instance);
        if (!ModelFor(cx, context, shape, owner, prototype, &model) ||
            !JS_InitializePropertiesFromCompatibleNativeObject(cx, target, model)) {
            return nullptr;
        }
    } else if (!ApplyEntries(cx, context, instance, shape)) {
        return nullptr;
    }
    JS::RootedObject made(cx, instance);
    if (intercepted) {
        // With a lazy prototype, so that the proxy has none of its own: every
        // read and write of it goes to the handler, and so to the target. A
        // proxy made with a prototype keeps that one and changes *it* on
        // `setPrototypeOf` without asking - while a declined lookup walks the
        // target's chain, which then never changes.
        JS::RootedValue target(cx, JS::ObjectValue(*instance));
        made = js::NewProxyObject(cx, &INTERCEPTOR_HANDLER, target, nullptr, js::ProxyOptions().setLazyProto(true));
        if (made == nullptr) {
            return nullptr;
        }
    }
    // A class instance says what made it in a slot of its own; anything else
    // is a plain object, and is filed under its template instead. See
    // `TemplateHasInstance`.
    if (owner == nullptr && constructor != nullptr && !RecordMaker(cx, context, made, constructor)) {
        return nullptr;
    }
    return made;
}

void DestroyBox(NativeBox* box) noexcept {
    if (box != nullptr && box->destroy != nullptr) {
        box->destroy(box);
    }
}

/// Hand `box` to the engine, or hand nothing over at all.
///
/// The last step of making an instance, and deliberately the last: everything
/// that can fail has already failed by the time the engine is told about the
/// box, so there is no window in which the box is both the engine's and the
/// caller's. After a true answer the finalizer or `~Isolate` gives it back
/// exactly once; after a false one nothing was published and the caller still
/// owns it.
[[nodiscard]] bool AttachNative(JSObject* instance, NativeBox* box) noexcept {
    JSObject* target = IsInterceptorProxy(instance) ? InterceptorHandler::TargetOf(instance) : instance;
    if (target == nullptr) {
        return false;
    }
    const JS::Value classSlot = JS::GetReservedSlot(target, INSTANCE_CLASS_SLOT);
    if (classSlot.isUndefined()) {
        return false;  // not an object that can carry one
    }
    if (box == nullptr) {
        return true;
    }
    auto* owner = static_cast<ClassRec*>(classSlot.toPrivate());
    if (owner == nullptr || owner->owner == nullptr) {
        return false;
    }
    try {
        // Recorded before it is published, so that a failure here publishes
        // nothing. The insert is the only thing in this function that can
        // fail, and it can only fail by throwing.
        owner->owner->impl().liveNatives.insert(box);
    } catch (const std::bad_alloc&) {
        return false;
    }
    JS::SetReservedSlot(target, INSTANCE_NATIVE_SLOT, JS::PrivateValue(box));
    return true;
}

bool ConstructInstance(JSContext* cx, JS::CallArgs& args, TemplateRec* tpl, ClassRec* owner, Isolate* isolate,
                       bool isConstruct);

bool ConstructorTrampoline(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    auto* tpl =
        static_cast<TemplateRec*>(js::GetFunctionNativeReserved(&args.callee(), FUNCTION_RECORD_SLOT).toPrivate());
    auto* isolate = static_cast<Isolate*>(JS_GetContextPrivate(cx));
    ClassRec* owner = tpl->ownerClass;

    // A FunctionTemplate's function may be called as well as constructed, and
    // the callback tells the two apart with IsConstructCall(). Only a Class<T>
    // insists on `new`, because there is a native to make and nowhere to put
    // it. The plain-call answer for a template with *no* callback is
    // deliberately unspecified - there is nothing to run - so nothing is
    // promised here beyond not failing.
    if (!args.isConstructing()) {
        // A class that did not opt in refuses, exactly as a JavaScript `class`
        // declaration does. One that did falls straight through to the
        // construct path below: `Foo(...)` then does what `new Foo(...)` does,
        // and the callback tells the two apart with `IsConstructCall()`. It is
        // the same native either way, so what comes out is still an instance
        // carrying a `T` - which is the one thing the typed layer exists to
        // guarantee.
        if (owner != nullptr && !owner->callableWithoutNew) {
            ThrowError(*isolate, ErrorKind::TypeError, "class constructor requires 'new'");
            return false;
        }
        if (owner != nullptr) {
            return ConstructInstance(cx, args, tpl, owner, isolate, /*isConstruct=*/false);
        }
        args.rval().setUndefined();
        if (tpl->callRecord == nullptr || tpl->callRecord->callback == nullptr) {
            return true;
        }
        JS::RootedValue receiver(cx, args.thisv());
        if (!ReceiverObject(cx, &receiver)) {
            return false;
        }
        CallFrame frame(*isolate, &args);
        const SlotIndex self = frame.frame().Push(receiver);
        CallbackState state{.owner = isolate,
                            .frame = &frame.frame(),
                            .context = CurrentContext(cx),
                            .call = &args,
                            .result = args.rval().address(),
                            .thisSlot = self,
                            .holderSlot = self,
                            .data = tpl->callRecord->data,
                            .isConstruct = false};
        tpl->callRecord->callback(CallbackInfo(state));
        return FinishNativeCall(cx, args);
    }

    return ConstructInstance(cx, args, tpl, owner, isolate, /*isConstruct=*/true);
}

/// Build an instance and hand it back: the body of `new Foo(...)`, and also of
/// `Foo(...)` for a class that opted in with `ConstructOrCall`.
///
/// `isConstruct` is passed through to the callback rather than inferred,
/// because that is the whole of the difference between the two spellings -
/// `IsConstructCall()` is there to be branched on for a different default or a
/// different message, not to change what comes back.
///
/// The instance is built from the prototype `new.target` names - the class's
/// own, unless script subclassed it - and from the class's own when there is
/// no `new.target` to ask, which is what makes a plain call work at all.
bool ConstructInstance(JSContext* cx, JS::CallArgs& args, TemplateRec* tpl, ClassRec* owner, Isolate* isolate,
                       bool isConstruct) {
    const Context context = CurrentContext(cx);

    if (owner != nullptr && owner->constructor == nullptr) {
        // A TypeError, as for anything asked of a value that cannot do it.
        ThrowError(*isolate, ErrorKind::TypeError, "this class cannot be constructed from script");
        return false;
    }

    // The object first, and the native afterwards. `args.thisv()` in a
    // construct call is `MagicValue(JS_IS_CONSTRUCTING)` - the engine has not
    // made a receiver, because a `JSNative` constructor is expected to make
    // its own - so a callback handed that as `info.This()` gets a sentinel
    // where every other engine gives it the instance. Writing to it then
    // *silently does nothing*: the property is not there afterwards and the
    // constructor reported success. Building the instance up front is what
    // makes the receiver real.

    // The prototype `new.target` names, when script reached here through a
    // subclass of this class. `args.newTarget()` is the constructor the `new`
    // named, which for `new Sub()` is `Sub` and for `new Foo()` is `Foo`, so
    // reading its `prototype` is right either way - and it is what the
    // language itself does (OrdinaryCreateFromConstructor).
    JS::RootedObject prototypeOverride(cx);
    if (isConstruct && args.newTarget().isObject()) {
        JS::RootedObject newTarget(cx, &args.newTarget().toObject());
        JS::RootedValue prototypeValue(cx);
        if (!JS_GetProperty(cx, newTarget, "prototype", &prototypeValue)) {
            return false;
        }
        if (prototypeValue.isObject()) {
            prototypeOverride = &prototypeValue.toObject();
        } else {
            // Not an object: the language's fallback is `Object.prototype` of
            // new.target's realm (GetPrototypeFromConstructor), as for any
            // constructor script writes - not this class's own prototype.
            JS::RootedObject realmOf(cx, JS::GetNonCCWObjectGlobal(js::UncheckedUnwrap(newTarget)));
            {
                const JSAutoRealm there(cx, realmOf);
                prototypeOverride = JS::GetRealmObjectPrototype(cx);
            }
            if (prototypeOverride == nullptr || !JS_WrapObject(cx, &prototypeOverride)) {
                return false;
            }
        }
    }

    JS::RootedObject instance(cx, NewInstanceOf(cx, context, tpl, prototypeOverride));
    if (instance == nullptr) {
        return false;
    }

    if (owner != nullptr) {
        CallFrame frame(*isolate, &args);
        const SlotIndex self = frame.frame().Push(JS::ObjectValue(*instance));
        JS::RootedValue discarded(cx);
        CallbackState state{.owner = isolate,
                            .frame = &frame.frame(),
                            .context = context,
                            .call = &args,
                            .result = discarded.address(),
                            .thisSlot = self,
                            .holderSlot = self,
                            .isConstruct = isConstruct};
        NativeBox* native = owner->constructor(CallbackInfo(state));
        if (native == nullptr) {
            if (!JS_IsExceptionPending(cx)) {
                JS_ReportErrorASCII(cx, "constructor declined to make an instance");
            }
            return false;
        }
        if (!AttachNative(instance, native)) {
            DestroyBox(native);
            JS_ReportErrorASCII(cx, "this instance could not be given its native state");
            return false;
        }
    }

    // A bare FunctionTemplate's own callback runs with the new object as its
    // receiver, which is what V8 does - and, as there, an object it answers
    // with is what `new` evaluates to, which is the language's own rule for a
    // constructor that returns one. Anything else it answers is ignored.
    if (owner == nullptr && tpl->callRecord != nullptr && tpl->callRecord->callback != nullptr) {
        CallFrame frame(*isolate, &args);
        const SlotIndex self = frame.frame().Push(JS::ObjectValue(*instance));
        JS::RootedValue answer(cx);
        CallbackState state{.owner = isolate,
                            .frame = &frame.frame(),
                            .context = context,
                            .call = &args,
                            .result = answer.address(),
                            .thisSlot = self,
                            .holderSlot = self,
                            .data = tpl->callRecord->data,
                            .isConstruct = isConstruct};
        tpl->callRecord->callback(CallbackInfo(state));
        if (JS_IsExceptionPending(cx)) {
            return false;
        }
        if (answer.isObject()) {
            // It may have come from another realm of the isolate.
            if (!JS_WrapValue(cx, &answer)) {
                return false;
            }
            args.rval().set(answer);
            return true;
        }
    }

    args.rval().setObject(*instance);
    return true;
}

void InstanceFinalize(JS::GCContext* /*gcx*/, JSObject* object) {
    const JS::Value slot = JS::GetReservedSlot(object, INSTANCE_NATIVE_SLOT);
    if (slot.isUndefined()) {
        return;
    }
    auto* native = static_cast<NativeBox*>(slot.toPrivate());
    if (native == nullptr) {
        return;
    }
    const JS::Value classSlot = JS::GetReservedSlot(object, INSTANCE_CLASS_SLOT);
    if (!classSlot.isUndefined()) {
        auto* owner = static_cast<ClassRec*>(classSlot.toPrivate());
        if (owner != nullptr) {
            // By key, not by walking. This runs once per instance collected,
            // and a walk would make it cost what the isolate has *ever* made
            // rather than what it still holds.
            owner->owner->impl().liveNatives.erase(native);
        }
    }
    if (native->destroy != nullptr) {
        native->destroy(native);
    }
}

}  // namespace

NativeBox* GetNativeBox(Slot object) noexcept {
    const JS::Value raw = Resolve(object);
    if (!raw.isObject()) {
        return nullptr;
    }
    JSObject* target = Unwrapped(raw);
    if (target == nullptr) {
        return nullptr;
    }
    if (IsInterceptorProxy(target)) {
        target = InterceptorHandler::TargetOf(target);
    }
    const JSClass* klass = JS::GetClass(target);
    // The finalizer is the marker: only a class made by this API has it.
    if (klass == nullptr || !klass->hasFinalize() || klass->cOps->finalize != &InstanceFinalize) {
        return nullptr;
    }
    const JS::Value slot = JS::GetReservedSlot(target, INSTANCE_NATIVE_SLOT);
    if (slot.isUndefined()) {
        return nullptr;
    }
    return static_cast<NativeBox*>(slot.toPrivate());
}

// ---------------------------------------------------------------------------
// The template half of the backend interface
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] TemplateRec* NewTemplate(Isolate& isolate, bool isFunction) {
    auto owned = std::unique_ptr<TemplateRec, TemplateDeleter>(new TemplateRec());
    TemplateRec* raw = owned.get();
    raw->owner = &isolate;
    raw->id = isolate.impl().nextTemplateId++;
    raw->isFunction = isFunction;
    isolate.impl().templates.push_back(std::move(owned));
    return raw;
}

/// A record for a *declaration*, which lives as long as the isolate and has
/// nowhere to report a failure: every caller below returns `void`. Running out
/// of memory while declaring a method is what it has always been here, an
/// exception out of the declaration, rather than a template that quietly does
/// not carry what it was given.
[[nodiscard]] CallbackRecord* StoreDeclaration(Isolate& isolate, CallbackRecord record) {
    CallbackRecord* stored = StoreCallback(isolate, record);
    if (stored == nullptr) {
        throw std::bad_alloc();
    }
    return stored;
}

}  // namespace

TemplateRec* NewObjectTemplate(Isolate& isolate) {
    return NewTemplate(isolate, false);
}

TemplateRec* NewFunctionTemplate(Isolate& isolate, FunctionCallback callback, CallbackData data) {
    TemplateRec* tpl = NewTemplate(isolate, true);
    if (callback != nullptr) {
        tpl->callRecord = StoreDeclaration(isolate, CallbackRecord{.callback = callback, .data = data});
    }
    return tpl;
}

void TemplateSetConstant(TemplateRec* tpl, std::string_view name, Constant value, PropertyAttribute attributes) {
    TemplateEntry entry{
        .kind = TemplateEntry::Kind::Constant, .name = std::string(name), .constant = value, .attributes = attributes};
    if (value.GetKind() == Constant::Kind::String) {
        entry.text = std::string(value.AsString());
    }
    tpl->entries.push_back(std::move(entry));
}

void TemplateSetMethod(TemplateRec* tpl, std::string_view name, FunctionCallback callback, CallbackData data,
                       PropertyAttribute attributes) {
    tpl->entries.push_back(
        TemplateEntry{.kind = TemplateEntry::Kind::Method,
                      .name = std::string(name),
                      .record = StoreDeclaration(*tpl->owner, CallbackRecord{.callback = callback, .data = data}),
                      .attributes = attributes});
}

void TemplateSetSymbolMethod(TemplateRec* tpl, WellKnownSymbol key, FunctionCallback callback, CallbackData data) {
    tpl->entries.push_back(
        TemplateEntry{.kind = TemplateEntry::Kind::SymbolMethod,
                      .name = "[symbol]",
                      .symbolKey = key,
                      .record = StoreDeclaration(*tpl->owner, CallbackRecord{.callback = callback, .data = data})});
}

void TemplateSetAccessor(TemplateRec* tpl, std::string_view name, AccessorGetterCallback getter,
                         AccessorSetterCallback setter, CallbackData data, PropertyAttribute attributes) {
    tpl->entries.push_back(TemplateEntry{
        .kind = TemplateEntry::Kind::Accessor,
        .name = std::string(name),
        .record = StoreDeclaration(*tpl->owner, CallbackRecord{.getter = getter, .setter = setter, .data = data}),
        .attributes = attributes});
}

void TemplateSetTemplate(TemplateRec* tpl, std::string_view name, TemplateRec* value, PropertyAttribute attributes) {
    tpl->entries.push_back(TemplateEntry{
        .kind = TemplateEntry::Kind::Child, .name = std::string(name), .child = value, .attributes = attributes});
    if (tpl->sealed) {
        // It will be instantiated with `tpl` in every realm from now on.
        Seal(value);
    }
}

// Class name, parent and handlers are fixed at the first instantiation - see
// unibind/template.h - so each of these four ignores a sealed template.

void TemplateSetNamedHandler(TemplateRec* tpl, const NamedPropertyHandler& handler) {
    if (tpl->sealed) {
        return;
    }
    tpl->named = handler;
    tpl->hasNamed = true;
}

void TemplateSetIndexedHandler(TemplateRec* tpl, const IndexedPropertyHandler& handler) {
    if (tpl->sealed) {
        return;
    }
    tpl->indexed = handler;
    tpl->hasIndexed = true;
}

void TemplateSetClassName(TemplateRec* tpl, std::string_view name) {
    if (tpl->sealed) {
        return;
    }
    tpl->className = std::string(name);
}

void TemplateInherit(TemplateRec* child, TemplateRec* parent) {
    if (child->sealed) {
        return;
    }
    child->parent = parent;
}

TemplateRec* TemplatePrototype(TemplateRec* tpl) {
    if (tpl->prototype == nullptr) {
        tpl->prototype = NewTemplate(*tpl->owner, false);
        tpl->prototype->prototypeOf = tpl;
        tpl->prototype->sealed = tpl->sealed;
    }
    return tpl->prototype;
}

TemplateRec* TemplateInstance(TemplateRec* tpl) {
    if (tpl->instance == nullptr) {
        tpl->instance = NewTemplate(*tpl->owner, false);
        tpl->instance->instanceOf = tpl;
        tpl->instance->sealed = tpl->sealed;
    }
    return tpl->instance;
}

Maybe<Slot> TemplateNewInstance(const Context& context, TemplateRec* tpl) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject noOverride(cx);
    JSObject* instance = NewInstanceOf(cx, context, tpl, noOverride);
    if (instance == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), instance);
}

Maybe<Slot> TemplateGetFunction(const Context& context, TemplateRec* tpl) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject function(cx);
    JS::RootedObject prototype(cx);
    if (!Materialise(cx, context, tpl, &function, &prototype)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), function);
}

Maybe<bool> TemplateHasInstance(const Context& context, TemplateRec* tpl, Slot value) {
    // Whether a template made the object - this one, or one that inherits
    // from it - as V8 answers, and not `instanceof`: that reads a prototype
    // chain script can rewrite, and from another realm finds none of the
    // prototypes it knows.
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject object(cx, Unwrapped(Resolve(value)));
    if (object == nullptr) {
        return false;
    }
    TemplateRec* maker = nullptr;
    if (!MakerOf(cx, object, &maker)) {
        return std::nullopt;
    }
    for (; maker != nullptr; maker = maker->parent) {
        if (maker == tpl) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The class half of the backend interface
// ---------------------------------------------------------------------------

ClassRec* NewClass(Isolate& isolate, std::string_view name, TypeId nativeType) {
    auto owned = std::unique_ptr<ClassRec, ClassDeleter>(new ClassRec());
    ClassRec* rec = owned.get();
    rec->owner = &isolate;
    rec->name = std::string(name);
    rec->nativeType = nativeType;
    rec->ops.finalize = &InstanceFinalize;
    rec->klass.name = rec->name.c_str();
    rec->klass.flags = JSCLASS_HAS_RESERVED_SLOTS(INSTANCE_SLOT_COUNT) | JSCLASS_FOREGROUND_FINALIZE;
    rec->klass.cOps = &rec->ops;

    rec->function = NewTemplate(isolate, true);
    rec->function->className = rec->name;
    rec->function->ownerClass = rec;

    isolate.impl().classes.push_back(std::move(owned));
    return rec;
}

void ClassSetConstructor(ClassRec* rec, NativeConstructor constructor, bool callableWithoutNew) {
    rec->constructor = constructor;
    rec->callableWithoutNew = callableWithoutNew;
}

TemplateRec* ClassPrototypeTemplate(ClassRec* rec) {
    return TemplatePrototype(rec->function);
}

TemplateRec* ClassConstructorTemplate(ClassRec* rec) {
    return rec->function;
}

TemplateRec* ClassInstanceTemplate(ClassRec* rec) {
    return TemplateInstance(rec->function);
}

Maybe<Slot> ClassGetConstructor(const Context& context, ClassRec* rec) {
    return TemplateGetFunction(context, rec->function);
}

Maybe<Slot> ClassInstantiate(const Context& context, ClassRec* rec, NativeBox* native) noexcept {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject instance(cx);
    try {
        JS::RootedObject noOverride(cx);
        instance = NewInstanceOf(cx, context, rec->function, noOverride);
        if (instance == nullptr || !AttachNative(instance, native)) {
            // Ownership transferred at the call, so a hand-over that did not
            // happen ends here rather than back at the caller.
            DestroyBox(native);
            return std::nullopt;
        }
    } catch (const std::bad_alloc&) {
        // The engine allocates here too, and this function is `noexcept`: an
        // escaping `bad_alloc` is `std::terminate` rather than the empty
        // answer the contract promises, and it would leave the caller holding
        // a box it was told it had handed over.
        DestroyBox(native);
        return std::nullopt;
    }
    // Past this point the *engine* owns the box: the instance carries it and
    // the isolate has it on the list ~Isolate finishes. The handle below can
    // still fail to be made, and the box is not this function's to give back
    // when it does - the finalizer or teardown gives it back exactly once.
    return PushOrNothing(OwnerOf(context), instance);
}

Maybe<bool> ClassHasInstance(const Context& context, ClassRec* rec, Slot value) {
    const JS::Value raw = Resolve(value);
    if (!raw.isObject()) {
        return false;
    }
    JSObject* target = Unwrapped(raw);
    if (target == nullptr) {
        return false;
    }
    if (IsInterceptorProxy(target)) {
        target = InterceptorHandler::TargetOf(target);
    }
    if (JS::GetClass(target) != &rec->klass) {
        return false;
    }
    const JS::Value slot = JS::GetReservedSlot(target, INSTANCE_CLASS_SLOT);
    return !slot.isUndefined() && slot.toPrivate() == rec;
}

}  // namespace ub::detail
