#pragma once
// Shared by the CPython backend's translation units. Not a public header:
// nothing includes this but the backend itself, and nothing outside this
// directory may see a CPython type.
//
// The shape of the backend, in the order a reader needs it:
//
//   * **An isolate is a sub-interpreter with a GIL of its own** (PEP 684). It
//     is created on the thread that calls `Isolate::New` and its thread state
//     stays attached to that thread for the isolate's whole life, which is
//     exactly the one-isolate-per-thread rule `unibind/isolate.h` already
//     makes. Nothing ever has to take a GIL on the way into an operation,
//     because the thread that may call one already holds it.
//   * **A handle is a strong reference.** CPython objects never move, so a
//     frame is simply an array of owned `PyObject*`, released when the frame
//     closes. `Global` is one more owned reference. See docs/lifetimes.md
//     section 12, which describes this backend before it existed.
//   * **The pending exception is CPython's own error indicator.** Throwing
//     sets it; a `TryCatch` takes it off; a native callback that returns with
//     it set propagates it into the Python code that called the native.
//   * **A realm is a globals dictionary.** `Context::GlobalObject()` is that
//     dictionary, and a script runs with it as both its globals and locals.
//
// docs/python.md has the long version of every decision here.

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "unibind/class.h"
#include "unibind/context.h"
#include "unibind/exception.h"
#include "unibind/function.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/script.h"
#include "unibind/template.h"
#include "unibind/value.h"

namespace ub {

namespace detail {
struct TemplateRec;
struct ClassRec;
struct CallbackRecord;
struct TryCatchState;
struct ContextRec;

// Each area of the backend keeps its per-isolate state in a struct of its own,
// defined in that area's translation unit, so that the areas can change
// independently. `Isolate::Impl` owns them through these.
struct BindingsState;  // bindings.cpp: callback records, templates, classes
struct RuntimeState;   // runtime.cpp: termination, interrupts, jobs, promises, heap
struct DataState;      // data.cpp: binary data, serialization, code cache

void DestroyBindingsState(BindingsState* state) noexcept;
void DestroyRuntimeState(RuntimeState* state) noexcept;
void DestroyDataState(DataState* state) noexcept;

struct BindingsStateDeleter {
    void operator()(BindingsState* state) const noexcept { DestroyBindingsState(state); }
};
struct RuntimeStateDeleter {
    void operator()(RuntimeState* state) const noexcept { DestroyRuntimeState(state); }
};
struct DataStateDeleter {
    void operator()(DataState* state) const noexcept { DestroyDataState(state); }
};

/// The types and singletons of the `unibind` module, per interpreter.
///
/// Heap types, every one of them, and one set per isolate: a static type
/// object is shared by every interpreter in the process and so cannot be used
/// by one with a GIL of its own. module.cpp creates the module and the core
/// types; each area fills in its own members from its `Init*Types` hook.
///
/// Every pointer here is a strong reference, dropped in `ReleaseTypes` before
/// the interpreter ends.
struct Types {
    // module.cpp
    PyObject* module = nullptr;      ///< the `unibind` module object
    PyTypeObject* null = nullptr;    ///< type of `unibind.null`
    PyObject* nullValue = nullptr;   ///< `unibind.null`, the one Null value
    PyTypeObject* symbol = nullptr;  ///< `unibind.Symbol`
    PyTypeObject* external = nullptr;
    PyObject* error = nullptr;           ///< `unibind.Error(Exception)` - ErrorKind::Error
    PyObject* rangeError = nullptr;      ///< `unibind.RangeError(ValueError)` - ErrorKind::RangeError
    PyObject* thrown = nullptr;          ///< `unibind.Thrown(Exception)`: a thrown value that is not an exception
    PyObject* terminated = nullptr;      ///< `unibind.Terminated(BaseException)`: TerminateExecution unwinding
    PyObject* wellKnown[5] = {};         ///< the well-known symbols, indexed by WellKnownSymbol
    PyObject* symbolRegistry = nullptr;  ///< Symbol.for: dict str -> Symbol
    PyObject* support = nullptr;         ///< module.cpp's pure-Python helpers (a dict)

    // objects.cpp
    PyTypeObject* object = nullptr;  ///< `unibind.Object`: a JavaScript-shaped property bag

    // bindings.cpp
    PyTypeObject* function = nullptr;       ///< `unibind.NativeFunction`
    PyTypeObject* boundFunction = nullptr;  ///< a NativeFunction bound to a receiver

    // runtime.cpp
    PyTypeObject* promise = nullptr;  ///< `unibind.Promise`

    // data.cpp
    PyTypeObject* typedArray = nullptr;  ///< `unibind.TypedArray`
    PyTypeObject* dataView = nullptr;    ///< `unibind.DataView`
    PyObject* dataCloneError = nullptr;  ///< `unibind.DataCloneError(unibind.Error)`
};

}  // namespace detail

struct Isolate::Impl {
    Isolate* self = nullptr;
    /// This isolate's interpreter and its one thread state, attached to the
    /// isolate's thread from `Isolate::New` to `~Isolate`.
    PyInterpreterState* interp = nullptr;
    PyThreadState* tstate = nullptr;

    detail::Frame* current = nullptr;
    std::uint32_t nextEpoch = 1;
    CallbackData embedder;
    detail::TryCatchState* tryCatch = nullptr;
    /// How many native calls are running on this isolate's stack, nested. A
    /// handler takes an exception only if it was opened at the depth the
    /// operation that raised it runs at; see `CatchPendingException`.
    std::uint32_t nativeDepth = 0;
    /// The realm a `ContextScope` entered, innermost.
    detail::ContextRec* entered = nullptr;

    /// Whether `Isolate::TerminateExecution` is in force. Library state, not
    /// engine state - CPython has no such thing - and so every path that
    /// would run Python code asks `Terminating()` first. Atomic because it is
    /// set from other threads; it is the only field here that is.
    std::atomic<bool> terminating{false};

    detail::Types types;

    /// Natives attached to instances that have not been deallocated yet, given
    /// back at teardown so that every box is destroyed exactly once and all of
    /// them before the isolate is gone (backend.h, `NativeBox`). A set, because
    /// a deallocation has to find its own entry.
    std::unordered_set<detail::NativeBox*> liveNatives;
    /// Set once teardown has given every native back. An instance deallocated
    /// after that - during `Py_EndInterpreter` - still carries its box pointer,
    /// which is gone; `GetNativeBox` answers null for all of them from here.
    bool nativesReleased = false;

    /// Every realm's globals dictionary, to the realm it belongs to. How a
    /// native called from Python finds the realm the call came from: the
    /// calling frame's globals.
    std::unordered_map<PyObject*, detail::ContextRec*> realms;
    /// The dictionary watcher (PEP 669's sibling, `PyDict_AddWatcher`) that
    /// tells a realm its dictionary is being deallocated; see `ContextRec`.
    /// -1 when none could be had.
    int realmWatcher = -1;
    /// Set while `~Isolate` gives the natives back: `GetNativeBox` then answers
    /// only for a box still in `liveNatives`, because a native's destructor
    /// can run script that reaches an instance whose box went a moment ago.
    bool nativesTearingDown = false;

    /// `Context`, `Script` and `Global` objects the embedder still holds.
    /// Diagnosed in `~Isolate` in a checked build.
    std::int32_t embedderRefs = 0;

    std::unique_ptr<detail::BindingsState, detail::BindingsStateDeleter> bindings;
    std::unique_ptr<detail::RuntimeState, detail::RuntimeStateDeleter> runtime;
    std::unique_ptr<detail::DataState, detail::DataStateDeleter> data;
};

namespace detail {

/// The isolate on this thread, or null. One per thread (decision 11), so this
/// is how code entered from Python - a type slot, a vectorcall - finds its way
/// back to the isolate it belongs to.
[[nodiscard]] Isolate* CurrentIsolate() noexcept;

[[nodiscard]] inline Isolate::Impl& ImplOf(Isolate& isolate) noexcept {
    return isolate.impl();
}
[[nodiscard]] inline detail::Types& TypesOf(Isolate& isolate) noexcept {
    return isolate.impl().types;
}

// --- frames ------------------------------------------------------------------

/// A handle frame: owned strong references, released in `~Frame`.
///
/// Slots `[0, argCount)` are borrowed from the vectorcall argument array of the
/// native call this frame belongs to - CPython holds those for the length of
/// the call, so the frame need not. Slots above that are the frame's own.
///
/// The first `UNIBIND_FRAME_INLINE_SLOTS` owned slots live inline; after that
/// they spill to a buffer from `::operator new(std::nothrow)`, so that the
/// frame-exhaustion rule (docs/lifetimes.md rule 9) is reachable.
struct Frame {
    Frame(Isolate& owner, Frame* parent, PyObject* const* args, std::uint32_t argCount) noexcept
        : owner(&owner), parent(parent), epoch(owner.impl().nextEpoch++), args(args), argCount(argCount) {}

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;
    ~Frame();

    static constexpr SlotIndex NO_SLOT = ~SlotIndex{0};

    /// Takes ownership of `value` (a new reference) and hands back its slot,
    /// or `NO_SLOT` - having released `value` - when the frame cannot grow.
    /// `value` must not be null.
    [[nodiscard]] SlotIndex Push(PyObject* value) noexcept;

    /// Borrowed. `NO_SLOT` reads as `None`, which nothing should ever ask for.
    [[nodiscard]] PyObject* At(SlotIndex index) const noexcept {
        if (index == NO_SLOT) {
            return Py_None;
        }
        if (index < argCount) {
            return args[index];
        }
        const SlotIndex own = index - argCount;
        assert(own < count && "slot index out of range for its frame");
        return own < UNIBIND_FRAME_INLINE_SLOTS ? inlineSlots[own] : spill[own - UNIBIND_FRAME_INLINE_SLOTS];
    }

    Isolate* owner;
    Frame* parent;
    std::uint32_t epoch;
    PyObject* const* args;
    std::uint32_t argCount;
    std::uint32_t count = 0;
    std::uint32_t spillCapacity = 0;
    PyObject* inlineSlots[UNIBIND_FRAME_INLINE_SLOTS] = {};
    PyObject** spill = nullptr;
};

static_assert(sizeof(Frame) <= UNIBIND_FRAME_STORAGE_SIZE,
              "UNIBIND_FRAME_STORAGE_SIZE in CMakeLists.txt is too small for the CPython frame");
static_assert(alignof(Frame) <= UNIBIND_FRAME_STORAGE_ALIGN, "UNIBIND_FRAME_STORAGE_ALIGN is too small");

[[nodiscard]] inline Slot MakeSlot(Frame& frame, SlotIndex index) noexcept {
#if UNIBIND_HANDLE_CHECKS
    return Slot{.frame = &frame, .index = index, .epoch = frame.epoch};
#else
    return Slot{.frame = &frame, .index = index};
#endif
}

/// A slot a trampoline reserved when a call began, or an empty handle if the
/// frame could not grow then. Never a slot that reads as a value.
[[nodiscard]] inline Slot SlotOrEmpty(Frame& frame, SlotIndex index) noexcept {
    return index == Frame::NO_SLOT ? Slot{} : MakeSlot(frame, index);
}

/// The object a handle names. Borrowed: valid while the handle's frame is.
[[nodiscard]] inline PyObject* Resolve(Slot slot) noexcept {
    assert(slot.frame != nullptr && "handle used while empty");
#if UNIBIND_HANDLE_CHECKS
    assert(slot.epoch == slot.frame->epoch && "handle used after its HandleScope closed");
#endif
    return slot.frame->At(slot.index);
}

[[nodiscard]] inline Isolate& IsolateFor(Slot slot) noexcept {
    return *slot.frame->owner;
}

/// An empty handle, with the condition reported the way CPython reports
/// running out of memory: `MemoryError` pending. `PyErr_NoMemory` allocates
/// nothing - the instance is preallocated - which matters here.
[[nodiscard]] inline Slot NoSlot() noexcept {
    PyErr_NoMemory();
    return Slot{};
}

/// Root `value` - a NEW reference, consumed whatever happens - in the
/// isolate's current frame. Null `value` means the call that made it failed,
/// and yields an empty handle with that call's exception left pending.
[[nodiscard]] Slot Push(Isolate& isolate, PyObject* value) noexcept;

/// The `std::optional<Slot>` form: failure of either kind is an empty answer.
[[nodiscard]] inline std::optional<Slot> PushOrNothing(Isolate& isolate, PyObject* value) noexcept {
    const Slot slot = Push(isolate, value);
    if (slot.IsEmpty()) {
        return std::nullopt;
    }
    return slot;
}

/// Same, for a borrowed reference.
[[nodiscard]] inline Slot PushBorrowed(Isolate& isolate, PyObject* value) noexcept {
    return Push(isolate, Py_NewRef(value));
}

// --- realms --------------------------------------------------------------------

/// A realm. `globals` is its global object and the dictionary every script run
/// in it executes against.
///
/// **Lifetime.** Two things keep a realm alive: the embedder's `Context`
/// references (`refs`), and the dictionary itself. While `refs` is non-zero the
/// record holds a strong reference to the dictionary. Once it is zero the
/// record lives exactly as long as the dictionary does - a function defined in
/// a realm the embedder has let go still finds its realm when it calls a
/// native, through its `__globals__` - and is deleted when the dictionary is
/// deallocated, which the isolate's dictionary watcher (`realmWatcher`)
/// reports.
///
/// A watcher rather than anything stored *in* the dictionary, because the
/// dictionary is script's: a record hung on one of its keys goes when script
/// clears the dictionary (`globals().clear()`) - while a `Context` still
/// names it - and outlives it when script copies it (`dict(globals())`),
/// leaving the realm map pointing at a dead dictionary's address for the
/// next one allocated there to inherit.
struct ContextRec {
    Isolate* owner = nullptr;
    PyObject* globals = nullptr;  ///< borrowed while refs == 0, strong while refs > 0
    int refs = 0;
    /// What a template made in this realm, keyed by the template - so that a
    /// realm instantiates each template once. Owned by bindings.cpp: a dict
    /// from `PyLong(TemplateRec*)` to the materialised object, or null.
    PyObject* templateCache = nullptr;
};

[[nodiscard]] inline Isolate& OwnerOf(const Context& context) noexcept {
    return *context.rec()->owner;
}
[[nodiscard]] inline PyObject* GlobalsOf(const Context& context) noexcept {
    return context.rec()->globals;
}

/// The realm a Python frame is running in, found from its globals; null if
/// those are not a realm's (code the backend itself runs, say).
[[nodiscard]] ContextRec* RealmOfGlobals(Isolate& isolate, PyObject* globals) noexcept;

/// The realm a native call arriving *now* belongs to: the calling Python
/// frame's, else the entered `ContextScope`'s. Null if neither exists.
[[nodiscard]] ContextRec* CurrentRealm(Isolate& isolate) noexcept;

struct ContextScopeState {
    Isolate* owner = nullptr;
    ContextRec* previous = nullptr;
    ContextRec* rec = nullptr;
};

static_assert(sizeof(ContextScopeState) <= UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE,
              "UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE is too small");

// --- globals, scripts ----------------------------------------------------------

struct GlobalNode {
    Isolate* owner = nullptr;
    PyObject* value = nullptr;  ///< strong
};

/// Compiled source: a code object for everything but a trailing expression
/// statement, and one for that expression, whose value is the script's
/// completion value (`Evaluate("1 + 1")` is 2). Either may be absent.
struct ScriptRec {
    Isolate* owner = nullptr;
    PyObject* body = nullptr;  ///< code, mode "exec"; strong
    PyObject* tail = nullptr;  ///< code, mode "eval"; strong or null
    bool usedCache = false;
    std::string name;
};

// --- exceptions ----------------------------------------------------------------

/// CPython has no `v8::TryCatch` - an exception is pending in the thread state
/// until something takes it - so, as on SpiderMonkey, this is a stack of
/// handlers the backend keeps itself.
struct TryCatchState {
    Isolate* owner = nullptr;
    TryCatchState* prev = nullptr;
    bool caught = false;
    bool rethrow = false;
    /// What stopped the code was `TerminateExecution`, not a throw. Closing
    /// the handler does not consume it.
    bool terminated = false;
    PyObject* exception = nullptr;  ///< strong; the caught exception instance
    /// An exception already pending when this handler opened: not ours to
    /// catch, parked here and put back on close.
    PyObject* outer = nullptr;
    std::uint32_t nativeDepth = 0;
};

static_assert(sizeof(TryCatchState) <= UNIBIND_TRY_CATCH_STORAGE_SIZE, "UNIBIND_TRY_CATCH_STORAGE_SIZE is too small");
static_assert(alignof(TryCatchState) <= UNIBIND_TRY_CATCH_STORAGE_ALIGN,
              "UNIBIND_TRY_CATCH_STORAGE_ALIGN is too small");

/// Hand an exception pending in the thread state to the innermost `TryCatch`,
/// if that handler owns it; otherwise leave it for the Python code a native
/// call is about to return to, or - with no handler and no native call on the
/// stack - drop it, which is what V8 does with an uncaught exception.
///
/// Every operation that ran Python code calls this before returning to the
/// embedder. See `ScriptGate`.
void CatchPendingException(Isolate& isolate) noexcept;

/// Whether a termination is in force. Asked at every gate into Python code:
/// CPython has no sticky "stop" of its own, so this check *is* the
/// enforcement of the promise in `unibind/isolate.h`.
[[nodiscard]] inline bool Terminating(const Isolate& isolate) noexcept {
    return isolate.impl().terminating.load(std::memory_order_acquire);
}

/// Raise `unibind.Terminated`, unless an exception is already pending.
/// runtime.cpp.
void RaiseStop(Isolate& isolate) noexcept;

/// Around every operation that may run Python code. `Open()` is false - and
/// the operation must answer empty without running anything - while a
/// termination is in force; the destructor hands any exception the operation
/// left to whoever owns it.
class ScriptGate {
   public:
    explicit ScriptGate(Isolate& isolate) noexcept : isolate_(&isolate) {}
    ScriptGate(const ScriptGate&) = delete;
    ScriptGate& operator=(const ScriptGate&) = delete;
    ScriptGate(ScriptGate&&) = delete;
    ScriptGate& operator=(ScriptGate&&) = delete;
    ~ScriptGate() { CatchPendingException(*isolate_); }

    /// Refusing raises `unibind.Terminated`, so that the `TryCatch` around the
    /// operation reports `HasTerminated` - a stop that was remembered while
    /// idle is still a stop, as `unibind/isolate.h` says - and a native that
    /// asked on script's behalf hands the stop back to the script.
    [[nodiscard]] bool Open() const noexcept {
        if (!Terminating(*isolate_)) {
            return true;
        }
        RaiseStop(*isolate_);
        return false;
    }

   private:
    Isolate* isolate_;
};

/// Set the pending exception to `kind` with `message`, decoded lossily.
void RaiseError(Isolate& isolate, ErrorKind kind, std::string_view message) noexcept;
/// A new exception instance of `kind`, not raised. Null with an exception
/// pending if it could not be made.
[[nodiscard]] PyObject* NewError(Isolate& isolate, ErrorKind kind, std::string_view message) noexcept;
/// The exception class `kind` maps to. Borrowed.
[[nodiscard]] PyObject* ErrorClass(Isolate& isolate, ErrorKind kind) noexcept;

/// Raise `value`: an exception instance as itself, anything else wrapped in
/// `unibind.Thrown`, whose `value` is what `TryCatch::Exception` hands back.
void RaiseValue(Isolate& isolate, PyObject* value) noexcept;
/// The inverse: the value a caught exception stands for. New reference.
[[nodiscard]] PyObject* UnwrapThrown(Isolate& isolate, PyObject* exception) noexcept;

/// Compile `source` into the pair of code objects a `ScriptRec` holds - see
/// `ScriptRec`. False with the SyntaxError pending if it does not compile.
/// core.cpp.
[[nodiscard]] bool CompileSource(Isolate& isolate, std::string_view source, const ScriptOrigin& origin, PyObject** body,
                                 PyObject** tail) noexcept;
/// A record over two code objects, taking both references. Null (having
/// released them) when there is not the memory. core.cpp.
[[nodiscard]] ScriptRec* NewScriptRec(Isolate& isolate, PyObject* body, PyObject* tail, std::string_view name) noexcept;

/// Turn a coroutine into a `unibind.Promise` that settles with its result,
/// driven by `PumpJobs`. Steals `coroutine`. New reference, or null with an
/// exception pending. runtime.cpp - a script that uses top-level `await`
/// evaluates to one of these.
[[nodiscard]] PyObject* SpawnCoroutine(Isolate& isolate, PyObject* coroutine) noexcept;

/// True, with `RecursionError` pending, when this thread's stack has reached
/// the floor `IsolateOptions::stackLimitBytes` set. runtime.cpp. For native
/// paths that recurse without entering CPython's evaluation loop - a callback
/// calling a native function directly, say - which CPython's own recursion
/// count does not see: call it on the way in and fail the call if it answers
/// true.
[[nodiscard]] bool StackExhausted(Isolate& isolate) noexcept;

// --- platform --------------------------------------------------------------------

/// What `PlatformOptions` said, for the areas that need it after the
/// constructor has returned. core.cpp.
[[nodiscard]] EngineFaultCallback PlatformFaultHandler() noexcept;
[[nodiscard]] CallbackData PlatformFaultData() noexcept;

// --- values --------------------------------------------------------------------

/// JavaScript's `===` and SameValue over two objects, for the handle and the
/// `Global` forms alike. values.cpp. Neither runs Python code.
[[nodiscard]] bool StrictEqualsObjects(Isolate& isolate, PyObject* lhs, PyObject* rhs) noexcept;
[[nodiscard]] bool SameValueObjects(Isolate& isolate, PyObject* lhs, PyObject* rhs) noexcept;

/// The UTF-8 text of a `str`, with a lone surrogate - which a Python string
/// may hold and UTF-8 may not - written as U+FFFD. values.cpp.
[[nodiscard]] std::string Utf8Of(PyObject* string);

/// Text the embedder declared - a name, a message - decoded as
/// `String::NewFromUtf8` decodes: invalid UTF-8 becomes U+FFFD. New reference.
[[nodiscard]] PyObject* TextString(std::string_view utf8) noexcept;

[[nodiscard]] inline bool IsNull(Isolate& isolate, PyObject* value) noexcept {
    return value == isolate.impl().types.nullValue;
}

/// `unibind.Symbol` instances.
struct SymbolObject {
    PyObject_HEAD PyObject* description;  ///< str or None
    PyObject* dunder;                     ///< the Python protocol name a well-known symbol stands for, or null
};

[[nodiscard]] bool IsSymbol(Isolate& isolate, PyObject* value) noexcept;

/// A property key as the backend uses it: a `str`, a `unibind.Symbol`, or an
/// `int` for a canonical array index. A well-known symbol that stands for a
/// Python protocol - `Symbol.iterator` for `__iter__` - is keyed by that
/// protocol's name, so that what a template installs under it is what Python
/// looks up. New reference, or null with an exception pending.
[[nodiscard]] PyObject* NormalizeKey(Isolate& isolate, PyObject* key) noexcept;

/// `unibind.External` instances.
struct ExternalObject {
    PyObject_HEAD CallbackData data;
};

// --- objects (objects.cpp) -------------------------------------------------------

/// `unibind.Object` instances - and instances of every type a template or a
/// class makes, which derive from it. The fields here are the ones other
/// translation units need; objects.cpp owns their meaning.
struct ObjectInstance {
    PyObject_HEAD PyObject*
        properties;       ///< dict: normalized key -> value (or an accessor record); owned by objects.cpp
    PyObject* meta;       ///< dict or null: normalized key -> attributes, where not the default
    PyObject* prototype;  ///< the [[Prototype]]: a unibind.Object, or null
    PyObject* weaklist;
    TemplateRec* tpl;  ///< the template that made this instance, or null
    NativeBox* box;    ///< the native a Class<T> instance carries, or null
};

// --- area hooks ----------------------------------------------------------------
//
// Called from module.cpp's module exec, in this order, with the isolate's
// `Types` to fill in. False with an exception pending fails `Isolate::New`.

[[nodiscard]] bool InitObjectTypes(Isolate& isolate, PyObject* module) noexcept;
[[nodiscard]] bool InitBindingTypes(Isolate& isolate, PyObject* module) noexcept;
[[nodiscard]] bool InitRuntimeTypes(Isolate& isolate, PyObject* module) noexcept;
[[nodiscard]] bool InitDataTypes(Isolate& isolate, PyObject* module) noexcept;

/// Called by `Isolate::New` before the interpreter exists / by `Platform`
/// before CPython is initialised, for the runtime area's process-wide setup
/// (the allocator hooks that make a heap limit possible). False fails it.
[[nodiscard]] bool PlatformRuntimeSetup() noexcept;
/// `Isolate::New`, after the module is imported: per-isolate runtime state.
[[nodiscard]] bool IsolateRuntimeSetup(Isolate& isolate, const IsolateOptions& options) noexcept;
/// `~Isolate`, while the interpreter is still alive and before any native is
/// given back: stop accepting work, drop queues.
void IsolateRuntimeTeardown(Isolate& isolate) noexcept;
/// `~Isolate`, same moment: bindings drop per-realm caches, give back natives.
void IsolateBindingsTeardown(Isolate& isolate) noexcept;

/// `~Isolate`, first thing: stop every thread a script started in the
/// isolate's interpreter and wait for them to end - for up to a couple of
/// seconds if `wait`, else for a moment. True once the isolate's thread is the
/// interpreter's only one, which `Py_EndInterpreter` requires. runtime.cpp.
[[nodiscard]] bool StopScriptThreads(Isolate& isolate, bool wait) noexcept;
/// Drop what stopping those threads kept, once none is left. runtime.cpp.
void ReleaseScriptThreadStop(Isolate& isolate) noexcept;
/// Take the runtime state off the isolate, which is about to leave its
/// interpreter behind with threads still in it: those may yet run the pending
/// call and the monitoring callback that point at the state, so it lives on,
/// until `EndAbandonedInterpreter`. runtime.cpp.
[[nodiscard]] RuntimeState* AbandonRuntime(Isolate& isolate) noexcept;
/// At `~Platform`, on the platform's thread: end an interpreter an isolate
/// left behind, through the thread state it left, if its threads have all
/// ended since - true - or leave it as it is. runtime.cpp.
[[nodiscard]] bool EndAbandonedInterpreter(PyThreadState* tstate, RuntimeState* runtime) noexcept;

/// `Isolate::New`: the dictionary watcher that ends a realm with its globals,
/// and the interpreter's note of which isolate it belongs to. core.cpp.
[[nodiscard]] bool InstallRealmWatcher(Isolate& isolate) noexcept;
/// Unhook both and delete every realm record still alive, for an interpreter
/// that will not be ended. core.cpp.
void ForgetRealms(Isolate& isolate) noexcept;

/// Drop every strong reference in `Types`. module.cpp.
void ReleaseTypes(Isolate& isolate) noexcept;

/// Create and import the `unibind` module into the isolate's interpreter.
/// module.cpp. Registered with the inittab by `Platform` before CPython comes
/// up, so every interpreter builds its own.
[[nodiscard]] bool RegisterModule() noexcept;
[[nodiscard]] bool ImportModule(Isolate& isolate) noexcept;

}  // namespace detail
}  // namespace ub
