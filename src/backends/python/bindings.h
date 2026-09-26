#pragma once
// What objects.cpp and bindings.cpp share, and nothing else includes: the
// template and class records, the per-call callback state, and the handful of
// entry points each file offers the other.
//
// The split between the two files:
//
//   * objects.cpp is the property model: `unibind.Object`, its [[Prototype]]
//     chain, per-property attributes, accessors as stored things, and every
//     property operation of the backend interface - on a `unibind.Object` and
//     on an ordinary Python object alike.
//   * bindings.cpp is everything that runs embedder code: native functions and
//     their call trampoline, accessor and interceptor hook calls, templates,
//     classes and the native state a class instance carries.
//
// Property lookup has to call hooks, and a hook call has to read properties, so
// each side calls into the other through the functions declared here.

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "internal.h"

namespace ub::detail {

// --- attributes ------------------------------------------------------------------

/// The bits `ObjectInstance::meta` records per key: `PropertyAttribute`'s three,
/// plus one of the backend's own saying that the value stored under the key is
/// an accessor record (a capsule over a `CallbackRecord`) rather than a value.
/// A key absent from `meta` is an ordinary writable, enumerable, configurable
/// data property - the overwhelmingly common case, which so costs nothing.
inline constexpr long ATTRIBUTE_MASK = 0x07;
inline constexpr long ATTR_ACCESSOR = 0x10;

// --- records ---------------------------------------------------------------------

/// An accessor property's two halves. Isolate-lifetime, owned by the bindings
/// state, and never freed before the interpreter is gone - decision 13 - so
/// the capsule a property holds can point at it without owning it.
struct CallbackRecord {
    AccessorGetterCallback getter = nullptr;
    AccessorSetterCallback setter = nullptr;
    CallbackData data;
};

/// One declaration on a template, replayed onto every object the template
/// stamps. The same shape as the SpiderMonkey backend's.
struct TemplateEntry {
    enum class Kind : std::uint8_t { Constant, Method, SymbolMethod, Accessor, Child };

    Kind kind = Kind::Constant;
    std::string name;
    /// A `Constant` holds a `string_view`; the template outlives whatever the
    /// caller pointed it at, so a string constant's bytes are copied here.
    std::string text;
    WellKnownSymbol symbolKey = WellKnownSymbol::Iterator;
    Constant constant;
    FunctionCallback callback = nullptr;  ///< Method, SymbolMethod
    CallbackData data;                    ///< Method, SymbolMethod
    CallbackRecord* record = nullptr;     ///< Accessor
    TemplateRec* child = nullptr;         ///< Child
    PropertyAttribute attributes = PropertyAttribute::None;
};

struct ClassRec;

/// An object or function template. Isolate-lifetime (decision 13): the bindings
/// state owns every one of them, and a raw pointer is what the public
/// `ObjectTemplate` and `FunctionTemplate` carry.
struct TemplateRec {
    Isolate* owner = nullptr;
    bool isFunction = false;
    FunctionCallback callback = nullptr;  ///< a function template's own callback, if any
    CallbackData callbackData;
    std::string className;
    std::vector<TemplateEntry> entries;

    NamedPropertyHandler named{};
    bool hasNamed = false;
    IndexedPropertyHandler indexed{};
    bool hasIndexed = false;

    TemplateRec* prototype = nullptr;  ///< a function template's PrototypeTemplate, made on demand
    TemplateRec* instance = nullptr;   ///< a function template's InstanceTemplate, made on demand
    TemplateRec* parent = nullptr;     ///< `Inherit`
    ClassRec* ownerClass = nullptr;    ///< set on the function template a `Class<T>` is built on
    /// Set on an instance template: the function template it belongs to. An
    /// instance made straight off `InstanceTemplate()` is still that
    /// constructor's instance, with its prototype.
    TemplateRec* instanceOf = nullptr;
    /// Set on a prototype template: the function template it belongs to.
    TemplateRec* prototypeOf = nullptr;
    /// Instantiated - or something that instantiates it with itself has been.
    /// From then on its class name, parent and handlers are fixed; see
    /// `unibind/template.h`.
    bool sealed = false;
};

struct ClassRec {
    Isolate* owner = nullptr;
    std::string name;
    TypeId nativeType;
    NativeConstructor constructor = nullptr;
    /// `ConstructOrCall` rather than `Construct`: a plain call makes an
    /// instance rather than being a TypeError.
    bool callableWithoutNew = false;
    TemplateRec* function = nullptr;
};

/// The template whose handlers answer for this object's properties, or null:
/// the instance template of the constructor that made it, or the object
/// template it was stamped from.
[[nodiscard]] inline TemplateRec* ShapeOf(const ObjectInstance* object) noexcept {
    TemplateRec* maker = object->tpl;
    if (maker == nullptr) {
        return nullptr;
    }
    return maker->isFunction ? maker->instance : maker;
}

// --- realms ----------------------------------------------------------------------

/// The realm a backend operation was asked to work in, for the hooks it calls.
///
/// A native called while an operation that took a `Context` is running should
/// see that realm - "operations taking a Context use that realm" - unless
/// Python code has been entered since, in which case the code that called it
/// is the more recent say. The frame recorded here is how the two are told
/// apart: the Python frame that was current when the operation began.
struct RealmScope {
    RealmScope(Isolate& isolate, ContextRec* rec) noexcept;
    ~RealmScope();
    RealmScope(const RealmScope&) = delete;
    RealmScope& operator=(const RealmScope&) = delete;
    RealmScope(RealmScope&&) = delete;
    RealmScope& operator=(RealmScope&&) = delete;

    BindingsState* state;
    RealmScope* previous;
    ContextRec* rec;
    PyFrameObject* frame;
};

/// The realm a native called now belongs to: the innermost operation's, unless
/// Python code has run since; the calling Python frame's; the entered
/// `ContextScope`'s; `fallback`; any live realm of the isolate. Null only when
/// the isolate has no realm at all.
[[nodiscard]] ContextRec* CallingRealm(Isolate& isolate, ContextRec* fallback = nullptr) noexcept;

// --- the call state ----------------------------------------------------------------

/// The backend's `CallbackState`: one type for a function call, a construct
/// call, an accessor and every interceptor hook, as `unibind/function.h` asks.
///
/// Every value in it is a slot of `frame`, the frame the trampoline opened for
/// the call; the first `argc` slots of that frame borrow the call's arguments.
/// The result is a strong reference held here - there is no engine-side return
/// slot in CPython, the trampoline hands the reference back as its answer.
struct CallbackState {
    Isolate* owner = nullptr;
    Frame* frame = nullptr;
    /// The calling realm, retained for the length of the call so that a
    /// callback's `GetContext()` is a live realm whatever it does.
    Context context;
    std::uint32_t argc = 0;
    SlotIndex thisSlot = Frame::NO_SLOT;
    SlotIndex holderSlot = Frame::NO_SLOT;
    SlotIndex valueSlot = Frame::NO_SLOT;
    bool hasValue = false;
    CallbackData data;
    bool isConstruct = false;
    /// What the callback answered, a new reference, or null for "nothing
    /// written" - which is `undefined`. Mutable because the public wrappers
    /// hand the state around as `const&` and the six `SetReturn*` write here.
    mutable PyObject* result = nullptr;
    /// A slot holding `None`, made the first time a callback reads past its
    /// arguments or asks for data it does not have.
    mutable SlotIndex undefinedSlot = Frame::NO_SLOT;
};

/// One native call: a frame whose first `argc` slots borrow `args`, the
/// isolate's native depth raised for its length, and the state a callback is
/// handed. Everything a trampoline has to undo, undone in the destructor.
class NativeCall {
   public:
    NativeCall(Isolate& isolate, PyObject* const* args, std::uint32_t argc, ContextRec* realm) noexcept;
    ~NativeCall();
    NativeCall(const NativeCall&) = delete;
    NativeCall& operator=(const NativeCall&) = delete;
    NativeCall(NativeCall&&) = delete;
    NativeCall& operator=(NativeCall&&) = delete;

    /// Root `value`, a new reference consumed whatever happens, in the call's
    /// frame. `NO_SLOT` if the frame could not grow; `SlotOrEmpty` makes that
    /// an empty handle rather than a value.
    [[nodiscard]] SlotIndex Push(PyObject* value) noexcept { return frame_.Push(value); }
    [[nodiscard]] Frame& frame() noexcept { return frame_; }
    /// The callback's answer, a new reference, or null if it wrote none.
    [[nodiscard]] PyObject* TakeResult() noexcept;

    CallbackState state;

   private:
    Isolate::Impl& impl_;
    Frame* saved_;
    Frame frame_;
};

/// Run a callback, turning a C++ exception into a Python one: nothing may
/// unwind through CPython's frames.
template <class F>
void Shielded(F&& body) noexcept {
    try {
        body();
    } catch (const std::bad_alloc&) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_NoMemory();
        }
    } catch (...) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_SystemError, "unibind: a native callback threw a C++ exception");
        }
    }
}

// --- bindings.cpp, for objects.cpp ---------------------------------------------------

/// A `unibind.NativeFunction`, exactly - not the bound form.
[[nodiscard]] bool IsNativeFunction(Isolate& isolate, PyObject* value) noexcept;
/// The bound form of a native function: `o.method` read as an attribute. New
/// reference, or null with an exception pending.
[[nodiscard]] PyObject* BindFunction(Isolate& isolate, PyObject* function, PyObject* receiver) noexcept;
/// Call `callable` with `receiver` as its `this` where it has one - a native
/// function - and as a plain call otherwise.
[[nodiscard]] PyObject* CallWithReceiver(Isolate& isolate, PyObject* callable, PyObject* receiver,
                                         PyObject* const* args, std::size_t argc) noexcept;
/// A Python iterator over what an iterator method answered: itself if it is a
/// Python iterator, an adapter if it is a JavaScript-style one (an object with
/// `next()` answering `{done, value}`), `iter()` of it otherwise. Steals
/// `produced`.
[[nodiscard]] PyObject* AdaptIterator(Isolate& isolate, PyObject* produced) noexcept;

/// A record for one accessor, kept for the life of the isolate. Null - with
/// `MemoryError` pending - if there was not the memory.
[[nodiscard]] CallbackRecord* StoreAccessorRecord(Isolate& isolate, AccessorGetterCallback getter,
                                                  AccessorSetterCallback setter, CallbackData data) noexcept;

/// Run an accessor's getter for a read of `key` made on `receiver` and found
/// on `holder`. New reference, or null with the exception pending.
[[nodiscard]] PyObject* RunAccessorGetter(Isolate& isolate, const CallbackRecord& record, PyObject* key,
                                          PyObject* receiver, PyObject* holder) noexcept;
/// Its setter. False with the exception pending.
[[nodiscard]] bool RunAccessorSetter(Isolate& isolate, const CallbackRecord& record, PyObject* key, PyObject* value,
                                     PyObject* receiver, PyObject* holder) noexcept;

/// What an interceptor hook did with an access.
enum class Hook : std::uint8_t {
    Declined,  ///< not intercepted: carry on with the ordinary lookup
    Handled,   ///< intercepted, and the out parameter holds the answer
    Failed,    ///< it threw: intercepted, and the exception is pending
};

/// Whether `shape` has a hook of the half `key` belongs to - the indexed half
/// for an int key, the named half for everything else.
[[nodiscard]] bool HasHandlerFor(const TemplateRec* shape, PyObject* key) noexcept;

[[nodiscard]] Hook InterceptGet(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* receiver,
                                PyObject* holder, PyObject** out) noexcept;
[[nodiscard]] Hook InterceptSet(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* value,
                                PyObject* receiver) noexcept;
[[nodiscard]] Hook InterceptQuery(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* receiver,
                                  PropertyAttribute* out) noexcept;
[[nodiscard]] Hook InterceptDelete(Isolate& isolate, TemplateRec* shape, PyObject* key, PyObject* receiver,
                                   bool* out) noexcept;
/// Append the keys the enumerators claim, normalized, to `keys`. False with
/// the exception pending if a hook threw.
[[nodiscard]] bool InterceptEnumerate(Isolate& isolate, TemplateRec* shape, PyObject* receiver,
                                      PyObject* keys) noexcept;

/// The template a type was made from - the nearest along its bases, so that a
/// Python subclass of a template's type finds it - or null.
[[nodiscard]] TemplateRec* TemplateOfType(Isolate& isolate, PyTypeObject* type) noexcept;
/// Whether `type` itself was made from a template - as opposed to a Python
/// subclass of such a type, which `TemplateOfType` also answers for.
[[nodiscard]] bool IsTemplateMadeType(Isolate& isolate, PyTypeObject* type) noexcept;
/// Where a template's type records the prototype methods it mirrors for
/// `super()`'s sake (bindings.cpp, `MirrorPrototypeMethods`).
inline constexpr const char* MIRRORED_KEY = "__unibind_mirrored__";
/// Whether the class attribute ordinary lookup would find for `name` is only
/// such a mirror - which the prototype chain, not the class, answers for.
[[nodiscard]] bool MirroredOnly(Isolate& isolate, PyTypeObject* type, PyObject* name) noexcept;
/// `type(*args)` for a template's type: the construct path. `isConstruct` is
/// what the callback's `IsConstructCall()` says - false only for a class that
/// opted into `ConstructOrCall` being called plainly. New reference, or null
/// with the exception pending.
[[nodiscard]] PyObject* ConstructTemplate(Isolate& isolate, PyTypeObject* type, TemplateRec* tpl, PyObject* const* args,
                                          std::size_t argc, bool isConstruct = true) noexcept;

// --- objects.cpp, for bindings.cpp ---------------------------------------------------

[[nodiscard]] bool IsObjectInstance(Isolate& isolate, PyObject* value) noexcept;
/// A fresh, empty instance of `type` - `unibind.Object` or a type derived from
/// it - with `prototype` (a `unibind.Object`, or null) as its [[Prototype]].
/// New reference, or null with the exception pending.
[[nodiscard]] ObjectInstance* NewObjectInstance(Isolate& isolate, PyTypeObject* type, PyObject* prototype) noexcept;
/// Store a data property as it is, with exactly these attribute bits,
/// bypassing setters, hooks and attribute checks - how a template stamps an
/// object. `key` is normalized. False with the exception pending.
[[nodiscard]] bool DefineRaw(ObjectInstance* object, PyObject* key, PyObject* value, long attributes) noexcept;
/// The same, for an accessor.
[[nodiscard]] bool DefineAccessorRaw(ObjectInstance* object, PyObject* key, CallbackRecord* record,
                                     PropertyAttribute attributes) noexcept;
/// A property read of any object, as a script would make it: through a
/// `unibind.Object`'s chain and hooks, a dict's items, a list's indices, or
/// any other object's attributes. `key` is normalized. New reference, `None`
/// with `*found` false when absent, or null with the exception pending.
[[nodiscard]] PyObject* GetAny(Isolate& isolate, PyObject* object, PyObject* key, bool* found) noexcept;

}  // namespace ub::detail
