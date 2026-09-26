#pragma once
/// \file
/// `Class<T>`: a JavaScript class whose instances carry native `T` state.
///
/// The typed layer over `FunctionTemplate`. What it adds is ownership and type
/// safety: recovering an instance's native yields a `T*` or nothing - never an
/// unchecked cast, never a `void*`, never RTTI.
///
/// ---------------------------------------------------------------------------
/// What a wrapper owns
/// ---------------------------------------------------------------------------
///
/// **Every wrapper owns a *share* of its native, never the native itself.** A
/// wrapper holds a `std::shared_ptr<T>`; the native is destroyed when the last
/// share goes, whoever holds it. Two ordinary situations need that and neither
/// is expressible under exclusive ownership:
///
///   * **Two wrappers naming one native.** A collection hands out a wrapper
///     for a child, and an enumeration of the same collection hands out
///     another for the same child. Under exclusive ownership one of them frees
///     a native the other still reads, and nothing at the call site says which.
///   * **A native co-owned with the embedder.** The embedder keeps its own
///     reference to something script also has a wrapper for, and neither side
///     is "the" owner.
///
/// The alternative shapes were considered and rejected:
///
///   * **A borrowed, non-owning wrapper** - the obvious fix, and the one that
///     causes the bug. A wrapper that does not own is a wrapper whose native
///     can go while script still holds it, and nothing in the engine will tell
///     you. There is no such thing here: `Unwrap` on a live wrapper is always
///     a live native.
///   * **A per-class choice of exclusive or shared.** Either it is a template
///     parameter, and `Class<Counter>` and the shared kind become different
///     C++ types infecting every signature that names one, or it is a runtime
///     flag, and the holder has to carry both representations anyway - which
///     is this design with a branch in front of it. It buys back one atomic
///     pair per wrapper and costs the most-used type in the API a dial.
///   * **An intrusive refcount, so the stored pointer *is* the native.**
///     Cheapest of all, and it would fold the box and the native into one
///     allocation - but it makes every native type inherit from a `unibind` base,
///     which rules out wrapping a type the embedder did not write, and it
///     cannot be co-owned with an embedder that already holds a
///     `std::shared_ptr<T>`. The second bullet above is *exactly*
///     `shared_ptr`'s job; inventing a second vocabulary for it would be a
///     renamed standard type.
///
/// **What it costs.** Per wrapper: one atomic increment when it is made, one
/// decrement when it is collected, on top of the box allocation and the engine
/// object that were there before. Per native: nothing, if you build it with
/// `std::make_shared` (which fuses the control block into the object, so a
/// class instance is the same two allocations it always was); one extra
/// control-block allocation if you hand over a `std::unique_ptr`, which
/// converts. **Per `Unwrap`: nothing at all** - it is the same single load
/// from the box it always was, which is what matters, because a call reads its
/// receiver's native far more often than it makes a wrapper.
///
/// A native that must *not* be destroyed by the engine - one that lives in the
/// embedder's own storage - is handed over as a share with a no-op deleter
/// (`std::shared_ptr<T>(&thing, [](T*) {})`). That is a statement made once, at
/// the call site that makes it, and visible there; it is not a property of the
/// wrapper type, which is the difference between it and a borrowed wrapper.
///
/// ---------------------------------------------------------------------------
/// The two destruction invariants
/// ---------------------------------------------------------------------------
///
/// There used to be one, because under exclusive ownership there was one thing
/// to destroy. A share separates the wrapper's cell from the thing it points
/// at, and the two now die on different schedules, so the guarantee is two:
///
///   1. **Every box is destroyed exactly once, and all of them by the time the
///      isolate is gone.** A box is the per-wrapper cell holding the share.
///      This is the one a backend has to work for: an engine that does not
///      promise to run a finalizer before the heap goes away keeps a list of
///      live instances and destroys the survivors at teardown.
///   2. **Every native is destroyed exactly once, when its last share goes** -
///      which may be *after* the isolate, because the last share may be the
///      embedder's. That is not a leak and not a violation of (1): the engine
///      gave back everything it took, and what is left is something the
///      embedder still holds and still owns.
///
/// So the sentence to hold a backend to is "the engine gives back every share
/// it took, exactly once, by the time its isolate is gone" - not "every native
/// is destroyed by then", which is now false on purpose. An embedder co-owning
/// a native and outliving the isolate is the case shared ownership exists for.
///
/// **A box is destroyed on the isolate's own thread.** Never a background
/// collector thread, never a helper. This is a requirement rather than a
/// preference, and it is easy to mistake for one: an engine that offers
/// background finalization will happily take it, and the `~shared_ptr` that
/// runs there is then racing every other holder of that native, including the
/// embedder's. A backend that has such a choice - SpiderMonkey spells it
/// `JSCLASS_FOREGROUND_FINALIZE` - must take the foreground one and say in a
/// comment that it is load-bearing, because nothing else about the code will
/// say so and the failure it prevents does not reproduce.
///
/// Callbacks are template arguments rather than runtime arguments:
///
///     cls.Method<&Counter::Increment>("increment");
///
/// which keeps a class method one indirect call - the trampoline that unwraps
/// `this` - with no per-method storage to own and no capture to keep alive.

#include <concepts>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#include "unibind/context.h"
#include "unibind/detail/backend.h"
#include "unibind/function.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/template.h"
#include "unibind/types.h"
#include "unibind/value.h"

namespace ub {

namespace detail {

/// What actually lives on an instance: the erased base the backend stores,
/// plus this wrapper's *share* of the native. Destroyed through
/// `NativeBox::destroy`, so the backend never needs to know `T` - and
/// destroying a box gives back one share rather than destroying the native, so
/// the native goes when the last holder does.
template <class T>
struct NativeHolder final : NativeBox {
    std::shared_ptr<T> value;

    explicit NativeHolder(std::shared_ptr<T> owned) noexcept : value(std::move(owned)) {
        // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer) - both are the base's, which a
        // member initializer list cannot name.
        type = TypeIdOf<T>();
        destroy = [](NativeBox* box) noexcept { delete static_cast<NativeHolder*>(box); };
        // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
    }
};

/// The holder on this object if it is exactly a `T`, else null.
template <class T>
[[nodiscard]] inline NativeHolder<T>* UnwrapHolder(Slot object) noexcept {
    NativeBox* box = GetNativeBox(object);
    if (box == nullptr || !(box->type == TypeIdOf<T>())) {
        return nullptr;
    }
    return static_cast<NativeHolder<T>*>(box);
}

/// The native on this object if it is exactly a `T`, else null. This is the
/// checked unwrap the whole class machinery rests on, and it hands back the
/// native itself - not a share of it, not a smart pointer, not a view.
template <class T>
[[nodiscard]] inline T* UnwrapNative(Slot object) noexcept {
    NativeHolder<T>* holder = UnwrapHolder<T>(object);
    return holder == nullptr ? nullptr : holder->value.get();
}

/// A constructor callback as the backend sees it: makes the native, or throws
/// and returns null.
using NativeConstructor = NativeBox* (*)(const CallbackInfo& info);

// The class half of the backend interface.
ClassRec* NewClass(Isolate& isolate, std::string_view name, TypeId nativeType);
/// `callableWithoutNew` decides the class's row of the call/construct grid: if
/// false, a plain call is a TypeError; if true, a plain call runs `constructor`
/// and yields a fresh instance, exactly as `new` does. See `Class<T>`.
void ClassSetConstructor(ClassRec* rec, NativeConstructor constructor, bool callableWithoutNew);
/// Instance methods and accessors are declared through the ordinary template
/// operations on these two records.
TemplateRec* ClassPrototypeTemplate(ClassRec* rec);
TemplateRec* ClassConstructorTemplate(ClassRec* rec);
TemplateRec* ClassInstanceTemplate(ClassRec* rec);
std::optional<Slot> ClassGetConstructor(const Context& context, ClassRec* rec);
/// A fresh instance carrying `native`, without running the JS constructor.
///
/// **It takes ownership of `native`, whatever happens.** On success the engine
/// has it and gives it back exactly once - through the instance's finalizer,
/// or through the isolate's own list at teardown. On failure this destroys it
/// before returning empty. Either way the caller has nothing left to free, and
/// that is the whole rule: ownership transfers at the call.
///
/// The alternative - the caller frees when the result is empty - is what this
/// replaces, and it was wrong in two directions at once. A backend publishes
/// the box to the engine (the instance's field, the isolate's list) and can
/// then still fail *after* that: the slot the handle needs is an allocation
/// like any other. The caller freeing there is a double free with a live
/// JavaScript object pointing at the freed box; and if the backend threw
/// instead of returning empty, the caller freed nothing at all. Neither is
/// visible from the call site, and both are on the path
/// `docs/lifetimes.md` rule 9 advertises as survivable.
///
/// `noexcept` for the same reason: a function that consumes ownership and
/// reports failure by returning empty has nowhere to put an exception, and an
/// exception escaping it is exactly the leak above.
std::optional<Slot> ClassInstantiate(const Context& context, ClassRec* rec, NativeBox* native) noexcept;
std::optional<bool> ClassHasInstance(const Context& context, ClassRec* rec, Slot value);

template <class T, auto Fn>
NativeBox* ConstructorTrampoline(const CallbackInfo& info) {
    // This runs inside an engine callback, and a C++ exception must not unwind
    // through the engine's frames: it skips everything the engine would have
    // done on the way out and leaves it in whatever state it was mid-call. So
    // running out of memory - in the embedder's constructor, or here, taking
    // its native over - fails the construction as a script exception instead.
    // A consumer built without exceptions has no such unwinding to prevent.
    const auto make = [&info]() -> NativeBox* {
        auto native = Fn(info);
        if (!native) {
            return nullptr;  // the callback threw, or declined
        }
        // A `std::unique_ptr` becomes the wrapper's first share; a
        // `std::shared_ptr` is taken as it is, deleter and all.
        return new NativeHolder<T>(std::shared_ptr<T>(std::move(native)));
    };
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        return make();
    } catch (const std::bad_alloc&) {
        try {
            info.Throw(ErrorKind::RangeError, "out of memory making the instance's native state");
        } catch (const std::bad_alloc&) {
            // Nothing left to say it with; the construction still fails.
        }
        return nullptr;
    }
#else
    return make();
#endif
}

template <class T, void (*Fn)(T&, const CallbackInfo&)>
void MethodTrampoline(const CallbackInfo& info) {
    T* self = UnwrapNative<T>(info.This().slot());
    if (self == nullptr) {
        info.ThrowTypeError("method called on a receiver of the wrong type");
        return;
    }
    Fn(*self, info);
}

template <class T, void (*Fn)(T&, const PropertyCallbackInfo&)>
void GetterTrampoline(const Local<Name>& /*property*/, const PropertyCallbackInfo& info) {
    T* self = UnwrapNative<T>(info.This().slot());
    if (self == nullptr) {
        info.ThrowTypeError("accessor read on a receiver of the wrong type");
        return;
    }
    Fn(*self, info);
}

template <class T, void (*Fn)(T&, const Local<Value>&, const PropertyCallbackInfo&)>
void SetterTrampoline(const Local<Name>& /*property*/, const Local<Value>& value, const PropertyCallbackInfo& info) {
    T* self = UnwrapNative<T>(info.This().slot());
    if (self == nullptr) {
        info.ThrowTypeError("accessor write on a receiver of the wrong type");
        return;
    }
    Fn(*self, value, info);
}

}  // namespace detail

/// A JavaScript class backed by native `T`.
///
/// Lives as long as its isolate; the handle is a non-owning reference and is
/// freely copied. Declare once, instantiate into any number of contexts.
template <class T>
class Class {
   public:
    /// Makes the native for a `new Foo(...)` call. Return null after throwing
    /// to reject the construction; null without a throw rejects it too, with
    /// an `Error` saying the constructor declined - no instance without a
    /// native ever reaches script.
    ///
    /// What comes out of `new` is always the instance: anything the callback
    /// writes through `info.GetReturnValue()` is discarded, as for an
    /// interceptor setter. A `FunctionTemplate`, which has no native to
    /// guarantee, is where a construct call may answer with another object.
    ///
    /// A *fresh* native, which is why this is a `std::unique_ptr` while a
    /// wrapper holds a share: `new` means a new instance, and the instance's
    /// native is the thing being made. Handing script a wrapper over a native
    /// that already exists is `Wrap`, which takes the share.
    using ConstructorFn = std::unique_ptr<T> (*)(const CallbackInfo& info);
    /// The other shape a constructor may have: a share rather than a fresh
    /// owner, for a native that must be released through a deleter of its own
    /// - one that counts, say - or that something else already holds.
    using SharedConstructorFn = std::shared_ptr<T> (*)(const CallbackInfo& info);
    using MethodFn = void (*)(T& self, const CallbackInfo& info);
    using GetterFn = void (*)(T& self, const PropertyCallbackInfo& info);
    using SetterFn = void (*)(T& self, const Local<Value>& value, const PropertyCallbackInfo& info);

    [[nodiscard]] static Class New(Isolate& isolate, std::string_view name) {
        return Class(detail::NewClass(isolate, name, TypeIdOf<T>()));
    }

    /// Makes the class constructable from script. Without this, instances can
    /// only come from `Wrap`.
    ///
    /// Constructable and nothing else: calling the constructor without `new` is
    /// a TypeError, exactly as it is for a JavaScript `class`. This is the
    /// default and stays the default - it is what a `class` declaration does,
    /// and a class that answers to a plain call by accident hands script
    /// something nobody asked it to make.
    template <auto Fn>
        requires std::same_as<decltype(Fn), ConstructorFn> || std::same_as<decltype(Fn), SharedConstructorFn>
    const Class& Construct() const {
        detail::ClassSetConstructor(rec_, &detail::ConstructorTrampoline<T, Fn>, false);
        return *this;
    }

    /// Constructable *and* callable: `Foo(...)` does what `new Foo(...)` does.
    ///
    /// The built-in shape this exists for is `Error`, where both spellings mean
    /// the same thing, and it completes the grid in `unibind/template.h` - a
    /// `FunctionTemplate` has always been allowed to answer to both, and a
    /// `Class<T>` had no way to opt in.
    ///
    /// The callback is the same `ConstructorFn` and tells the two apart with
    /// `CallbackInfo::IsConstructCall()`, which is there to be branched on -
    /// for a different default, a warning, a different name in an error - not
    /// to change what comes back. **A plain call yields an instance**, made the
    /// same way `new` makes one - including that the callback's `info.This()` is
    /// that instance, not the receiver the call was made on.
    ///
    /// Letting a plain call return something that is *not* an instance was the
    /// alternative and is rejected: a class whose call does not make an
    /// instance is a function with a constructor bolted on, and the grid
    /// already has a row for that - `FunctionTemplate`. It would also need a
    /// second callback with a different signature, so that the one thing the
    /// typed layer exists to guarantee - what comes out of a `Class<T>` carries
    /// a `T` - would stop being true.
    template <auto Fn>
        requires std::same_as<decltype(Fn), ConstructorFn> || std::same_as<decltype(Fn), SharedConstructorFn>
    const Class& ConstructOrCall() const {
        detail::ClassSetConstructor(rec_, &detail::ConstructorTrampoline<T, Fn>, true);
        return *this;
    }

    template <MethodFn Fn>
    const Class& Method(std::string_view name, PropertyAttribute attributes = PropertyAttribute::DontEnum) const {
        detail::TemplateSetMethod(detail::ClassPrototypeTemplate(rec_), name, &detail::MethodTrampoline<T, Fn>, {},
                                  attributes);
        return *this;
    }

    /// A method under a well-known symbol. `Symbol.iterator` is the reason
    /// this exists.
    template <MethodFn Fn>
    const Class& SymbolMethod(WellKnownSymbol key) const {
        detail::TemplateSetSymbolMethod(detail::ClassPrototypeTemplate(rec_), key, &detail::MethodTrampoline<T, Fn>,
                                        {});
        return *this;
    }

    template <GetterFn Get>
    const Class& Accessor(std::string_view name, PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetAccessor(detail::ClassPrototypeTemplate(rec_), name, &detail::GetterTrampoline<T, Get>,
                                    nullptr, {}, attributes);
        return *this;
    }

    template <GetterFn Get, SetterFn Set>
    const Class& Accessor(std::string_view name, PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetAccessor(detail::ClassPrototypeTemplate(rec_), name, &detail::GetterTrampoline<T, Get>,
                                    &detail::SetterTrampoline<T, Set>, {}, attributes);
        return *this;
    }

    /// The untyped forms of `Method`, `SymbolMethod` and `Accessor`: a plain
    /// callback chosen at run time, plus data, on the same prototype.
    ///
    /// For an embedder that sends every binding through one trampoline of its
    /// own - a per-call profiling or stack-capture hook is the case this is
    /// for - and so has a function *value* per member rather than a constant
    /// it could name as a template argument. The callback unwraps the receiver
    /// itself with `Unwrap(info.This())`, and gets null for a receiver that is
    /// not a `T`, exactly as the typed trampolines do before they decline.
    const Class& Method(std::string_view name, FunctionCallback callback, CallbackData data = {},
                        PropertyAttribute attributes = PropertyAttribute::DontEnum) const {
        detail::TemplateSetMethod(detail::ClassPrototypeTemplate(rec_), name, callback, data, attributes);
        return *this;
    }
    const Class& SymbolMethod(WellKnownSymbol key, FunctionCallback callback, CallbackData data = {}) const {
        detail::TemplateSetSymbolMethod(detail::ClassPrototypeTemplate(rec_), key, callback, data);
        return *this;
    }
    const Class& Accessor(std::string_view name, AccessorGetterCallback getter, AccessorSetterCallback setter = nullptr,
                          CallbackData data = {}, PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetAccessor(detail::ClassPrototypeTemplate(rec_), name, getter, setter, data, attributes);
        return *this;
    }

    /// What every instance is stamped with - V8's
    /// `FunctionTemplate::InstanceTemplate()`. Members declared here are *own*
    /// properties of each instance, where `Method` and `Accessor` put them on
    /// the prototype: `hasOwnProperty`, `Object.keys` and `JSON.stringify` see
    /// them, as script that copies an object property by property expects. It
    /// applies to instances made by `new` and by `Wrap` alike.
    ///
    /// The cost is per instance: SpiderMonkey defines each member on each
    /// object as it is made, where the prototype holds it once. Declare here
    /// only what has to be own.
    [[nodiscard]] ObjectTemplate InstanceTemplate() const {
        return ObjectTemplate(detail::ClassInstanceTemplate(rec_));
    }

    /// Statics take a plain function callback: there is no instance to unwrap.
    const Class& StaticMethod(std::string_view name, FunctionCallback callback, CallbackData data = {}) const {
        detail::TemplateSetMethod(detail::ClassConstructorTemplate(rec_), name, callback, data,
                                  PropertyAttribute::DontEnum);
        return *this;
    }
    const Class& StaticValue(std::string_view name, Constant value,
                             PropertyAttribute attributes = PropertyAttribute::ReadOnly) const {
        detail::TemplateSetConstant(detail::ClassConstructorTemplate(rec_), name, value, attributes);
        return *this;
    }

    /// An interceptor over every property of every instance.
    const Class& SetHandler(const NamedPropertyHandler& handler) const {
        detail::TemplateSetNamedHandler(detail::ClassInstanceTemplate(rec_), handler);
        return *this;
    }
    const Class& SetHandler(const IndexedPropertyHandler& handler) const {
        detail::TemplateSetIndexedHandler(detail::ClassInstanceTemplate(rec_), handler);
        return *this;
    }

    /// Install the constructor on a context - normally on its global object.
    [[nodiscard]] std::optional<Local<Function>> GetConstructor(const Context& context) const {
        return detail::WrapSlot<Function>(detail::ClassGetConstructor(context, rec_));
    }

    /// An instance holding a share of `native`, without running the script
    /// constructor.
    ///
    /// Call it twice with the same `std::shared_ptr` and you get two wrappers
    /// over one native, each safe to read for as long as it lives; the native
    /// goes when the last share does, which may be the engine's or may be
    /// yours. A `std::unique_ptr` converts, which is the common case and costs
    /// one control-block allocation - `std::make_shared<T>(...)` avoids even
    /// that.
    [[nodiscard]] std::optional<Local<Object>> Wrap(const Context& context, std::shared_ptr<T> native) const {
        // The box is the backend's from here: `ClassInstantiate` consumes it
        // whether or not it can make the instance, which is what keeps a
        // failure half way through the hand-over from being a double free.
        auto* holder = new detail::NativeHolder<T>(std::move(native));
        return detail::WrapSlot<Object>(detail::ClassInstantiate(context, rec_, holder));
    }

    /// Is this value an instance of this class? Cheaper and stricter than
    /// `instanceof`: it asks what native the object carries, so it is not
    /// fooled by a prototype swap and does not cross realms.
    template <class U>
    [[nodiscard]] bool IsInstance(const Local<U>& value) const noexcept {
        return detail::UnwrapNative<T>(value.slot()) != nullptr;
    }

    /// The native, or null if this is not an instance of this class. The only
    /// way to get at it, and it cannot lie.
    ///
    /// A plain `T*`, and that is deliberate: it is what a method body wants,
    /// it is one load, and it touches no reference count. It is valid for as
    /// long as the wrapper you unwrapped is - to keep it longer, take a share.
    template <class U>
    [[nodiscard]] static T* Unwrap(const Local<U>& value) noexcept {
        return detail::UnwrapNative<T>(value.slot());
    }

    /// A *share* of the native, or empty if this is not an instance of this
    /// class.
    ///
    /// This is how a native outlives the wrapper it came from, and how a second
    /// wrapper over the same native is made from a wrapper you were handed
    /// rather than from bookkeeping you kept yourself: unwrap the share, hand
    /// it back to `Wrap`. It costs one atomic increment, which is why it is a
    /// separate call from `Unwrap` rather than what `Unwrap` returns.
    template <class U>
    [[nodiscard]] static std::shared_ptr<T> UnwrapShared(const Local<U>& value) noexcept {
        detail::NativeHolder<T>* holder = detail::UnwrapHolder<T>(value.slot());
        return holder == nullptr ? std::shared_ptr<T>() : holder->value;
    }

    /// Implementation detail: the backend's record.
    [[nodiscard]] constexpr detail::ClassRec* rec() const noexcept { return rec_; }

   private:
    explicit constexpr Class(detail::ClassRec* rec) noexcept : rec_(rec) {}

    detail::ClassRec* rec_;
};

}  // namespace ub
