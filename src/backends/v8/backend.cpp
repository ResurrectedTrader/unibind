// The V8 backend.
//
// Everything V8-specific in the library lives behind this file: it defines the
// types the public headers only declare (`detail::Frame`, `ContextRec`, ...)
// and the functions the public headers only declare. Nothing above it includes
// a V8 header, which is what `unibind_headers_only` proves.
//
// SCOPE: this is the vertical slice - isolates, contexts, frames, values,
// objects, functions, scripts, exceptions and globals. Templates, classes,
// interceptors and symbols are declared in the public headers and NOT defined
// here yet; they are the next agent's work. An unimplemented operation is a
// link error at the call site, which is the diagnosis we want.

#include <libplatform/libplatform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
// V8 deletes HandleScope::operator new, which hides the global placement new,
// so every in-place construction here spells `::new`.
// v8-template.h names std::span<const v8::CFunction> while v8.h only forward
// declares CFunction; this header is what completes it.
#include <v8-fast-api-calls.h>
#include <v8-inspector.h>
#include <v8.h>
// For GetCurrentThreadStackLimits, which is how a stack limit gets clamped to a
// stack that actually exists. WIN32_LEAN_AND_MEAN and NOMINMAX come from the
// project's compile definitions.
#include <windows.h>

#include "unibind/class.h"
#include "unibind/context.h"
#include "unibind/exception.h"
#include "unibind/function.h"
#include "unibind/handle.h"
#include "unibind/inspector.h"
#include "unibind/interop/v8.h"
#include "unibind/isolate.h"
#include "unibind/script.h"
#include "unibind/value.h"

namespace ub {
namespace {

/// Slot 0 of a v8::Isolate's embedder data holds the owning ub::Isolate, and
/// slot 0 of a v8::Context's holds its ContextRec. Both are set at creation.
constexpr uint32_t ISOLATE_SLOT = 0;
constexpr int CONTEXT_SLOT = 0;

/// Internal field 0 of a class instance holds its NativeBox, which is why a
/// class's instance template asks for one internal field.
constexpr int NATIVE_FIELD = 0;
constexpr int NATIVE_FIELD_COUNT = NATIVE_FIELD + 1;

/// What `PlatformOptions` installed, for the life of the process.
///
/// Not settable afterwards, which is what lets these be read with no lock from
/// whatever thread faults - see the field's comment in unibind/isolate.h. A
/// `Platform` is the scope of the whole program's use of the engine, so
/// "written once before V8 comes up, read until it goes down" is the whole
/// lifecycle.
EngineFaultCallback g_faultHandler = nullptr;  // NOLINT(*-avoid-non-const-global-variables)
CallbackData g_faultData;                      // NOLINT(*-avoid-non-const-global-variables)

/// The one isolate this thread is allowed to have alive, if it has one.
///
/// V8 is happy with any number per thread; SpiderMonkey keeps the running
/// context in a single thread-local slot and cannot have two. So the contract
/// is one per thread (unibind/isolate.h) and this backend holds itself to it,
/// because an embedder finding out by porting is exactly the failure this
/// library exists to prevent.
///
/// It is also how a fault that names no heap - V8's process-wide check, which
/// is handed a file and a line and nothing else - still arrives with an
/// isolate in the report: one per thread means the thread names it.
thread_local Isolate* g_threadIsolate = nullptr;  // NOLINT(*-avoid-non-const-global-variables)

/// Hand a fault to whatever the embedder installed, if anything.
///
/// **Nothing here allocates**, and that is the contract rather than an
/// implementation note: the commonest caller is out of memory. The report is a
/// stack aggregate of views into strings the engine or this file already owns.
void ReportEngineFault(EngineFault fault, Isolate* isolate, const char* location, const char* message) noexcept {
    if (g_faultHandler == nullptr) {
        return;
    }
    const EngineFaultReport report{.fault = fault,
                                   .isolate = isolate,
                                   .location = location == nullptr ? std::string_view{} : std::string_view(location),
                                   .message = message == nullptr ? std::string_view{} : std::string_view(message)};
    g_faultHandler(report, g_faultData);
}

}  // namespace

namespace detail {
// Backend-only records the isolate owns. The ones the public headers name are
// forward declared in unibind/fwd.h; these exist for no other layer, so they are
// declared here and defined below.
struct AccessorRecord;
struct InstanceRecord;

/// The realm a callback was called in, found the first time the callback asks
/// for it rather than on every call: most callbacks never do, and finding it
/// is a call into V8, a handle and an embedder-data read.
///
/// Late, but not wrong. The one thing that changes the isolate's current realm
/// while a callback runs, and is still in force when it asks, is a
/// `ContextScope` the callback opened - script it calls into leaves the realm
/// as it found it - so `ContextEnter` settles the innermost callback's realm
/// before it enters anything. That is the only callback that can be asking:
/// an outer one is suspended until everything inside it has returned and
/// closed its scopes. The isolate keeps that innermost one in `innermostCall`.
struct CallRealm {
    Context context;
    CallRealm* outer = nullptr;
};
}  // namespace detail

// ---------------------------------------------------------------------------
// The types the public headers declare and never define
// ---------------------------------------------------------------------------

struct Isolate::Impl {
    v8::Isolate* isolate = nullptr;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    detail::Frame* current = nullptr;
    uint32_t nextEpoch = 1;
    CallbackData embedder;
    Isolate* self = nullptr;
    /// Stable homes for the (callback, data) pairs a native function needs,
    /// because V8 hands a callback one pointer and we need two.
    ///
    /// Keyed by the record's own address so that one can be given back when
    /// the *value* it belongs to is collected. A function made by
    /// `Function::New` and an external are ordinary values - they are
    /// collected like any other, and a record that outlived them would make
    /// every such value cost its isolate a permanent allocation. A record a
    /// template or a class owns stays until the isolate goes, which is what
    /// decision 13 says those are.
    std::unordered_map<detail::CallbackRecord*, std::unique_ptr<detail::CallbackRecord>> callbacks;
    /// Templates and accessor pairs are isolate-lifetime by design, so the
    /// handles callers hold are raw pointers into these and there is no
    /// refcount anywhere. See the header comment in unibind/template.h.
    std::vector<std::unique_ptr<detail::TemplateRec>> templates;
    std::vector<std::unique_ptr<detail::AccessorRecord>> accessors;
    std::vector<std::unique_ptr<detail::ClassRec>> classes;
    /// An interceptor's hooks live as long as the template that carries them,
    /// which is as long as the isolate.
    std::vector<std::unique_ptr<NamedPropertyHandler>> namedHandlers;
    std::vector<std::unique_ptr<IndexedPropertyHandler>> indexedHandlers;
    /// Every instance that still carries a native. V8 does not promise to run
    /// a weak callback before an isolate goes away, so the survivors are
    /// destroyed in ~Isolate - which is what makes "the finalizer runs exactly
    /// once" a rule a test can hold both backends to.
    std::unordered_map<detail::InstanceRecord*, std::unique_ptr<detail::InstanceRecord>> liveNatives;
    /// Work posted from another thread, and interrupt callbacks waiting to
    /// fire. Both queues are ours rather than V8's, so that ordering and what
    /// happens to leftovers is the same on every backend rather than whatever
    /// each engine's own list does. One mutex: neither queue is contended and
    /// two locks would be two chances to get the order wrong.
    /// A stop, from the moment it is requested until it is cancelled. V8 has no
    /// "requested" bit of its own - IsExecutionTerminating only answers while
    /// the termination exception is actually pending, which is never while a
    /// native is running - so the sticky part is ours, exactly as it has to be
    /// on SpiderMonkey. See unibind/isolate.h.
    std::atomic<bool> terminating{false};
    std::mutex work;
    std::deque<std::pair<JobCallback, CallbackData>> jobs;
    /// `PostDelayedJob`'s work, keyed on when it falls due. A multimap keeps
    /// jobs that fall due together in the order they were posted, which is
    /// the order the header promises. Moved into `jobs` by `PumpJobs`.
    std::multimap<std::chrono::steady_clock::time_point, std::pair<JobCallback, CallbackData>> delayedJobs;
    std::deque<std::pair<InterruptCallback, CallbackData>> interrupts;
    /// Contexts, scripts and `Global<T>` roots the embedder is still holding.
    ///
    /// These three are `new`ed on demand and `delete`d by the call that gives
    /// them back, which is the embedder's to make - so unlike everything else
    /// here they are not in a container this isolate can empty. One still alive
    /// in `~Isolate` leaks its own memory *and* leaves a later `Reset` writing
    /// to a disposed isolate, so `unibind/isolate.h` makes it a rule and this
    /// counter is what diagnoses breaking it, in a checked build, where it
    /// happens.
    std::int32_t embedderRefs = 0;
    /// What `SetHeapLimitCallback` installed. V8 hands its near-heap-limit
    /// callback one pointer and we need three things, so the pair lives here
    /// and the pointer is the isolate. `heapLimitArmed` is whether V8 is
    /// currently holding our trampoline, which it has to be told to stop
    /// doing by name.
    HeapLimitCallback heapLimitCallback = nullptr;
    CallbackData heapLimitData;
    bool heapLimitArmed = false;
    /// The shape of the data a `Function::New` with a script value carries:
    /// an object with two internal fields, the callback and the value. Made
    /// the first time one is asked for, and kept, because a template is an
    /// isolate-level thing and making one per function would be a template per
    /// function.
    v8::Global<v8::ObjectTemplate> valueDataTemplate;
    /// What an eager compile's origin carries so that V8's in-isolate
    /// compilation cache files it apart from a lazy compile of the same source.
    /// See `CompileInto`.
    v8::Global<v8::PrimitiveArray> eagerMarker;
    /// A realm of the backend's own, made the first time something needs one
    /// and none is entered: an error thrown from native code with no
    /// `ContextScope` open - from a posted job, say - has to be made in some
    /// realm, and V8 makes it in the current one or crashes. SpiderMonkey's
    /// backend has the same thing for the same reason.
    v8::Global<v8::Context> utility;
    /// The lowest stack address a native call may start at - see
    /// `RefuseDeeperNative`. The isolate's own stack limit when one was asked
    /// for, and otherwise the bottom of the thread's real stack less the room
    /// the engine needs to throw; 0, which refuses nothing, where the thread's
    /// stack could not be asked about.
    uintptr_t stackGuard = 0;
    /// The isolate's inspector, if it has one - there is at most one - and its
    /// dispatcher, which holds the queue `InspectorDispatcher::RequestDispatch`
    /// fills. What a V8 interrupt and a posted job are handed is this isolate,
    /// which outlives any request still in flight, and they find the queue
    /// through here - so a wake-up that arrives after the inspector has gone
    /// finds nothing rather than a freed queue. Isolate thread only.
    Inspector* inspector = nullptr;
    Inspector::Impl* inspectorImpl = nullptr;
    std::shared_ptr<InspectorDispatcher> inspectorDispatcher;
    /// The innermost callback running, for `ContextEnter` to settle its realm
    /// before it enters another. See `detail::CallRealm`.
    detail::CallRealm* innermostCall = nullptr;
};

namespace {

/// Hides the running callback from `ContextEnter` while V8 has called into
/// this library for something else in the middle of it - an interrupt, a
/// finalizer, a heap-limit or inspector callback. Those run wherever script
/// happened to be, not in the callback's realm, so a `ContextScope` they open
/// must not settle that callback's realm from there. See `detail::CallRealm`.
class OutsideCallback {
   public:
    explicit OutsideCallback(Isolate& isolate) noexcept
        : impl_(&isolate.impl()), saved_(std::exchange(isolate.impl().innermostCall, nullptr)) {}
    ~OutsideCallback() { impl_->innermostCall = saved_; }

    OutsideCallback(const OutsideCallback&) = delete;
    OutsideCallback& operator=(const OutsideCallback&) = delete;
    OutsideCallback(OutsideCallback&&) = delete;
    OutsideCallback& operator=(OutsideCallback&&) = delete;

   private:
    Isolate::Impl* impl_;
    detail::CallRealm* saved_;
};

}  // namespace

namespace detail {

/// An empty vector, made by a constructor that is allowed to fail.
///
/// Under the MSVC STL's iterator debugging - every Debug build - even an empty
/// vector allocates: its container proxy. The default constructor makes that
/// allocation and is `noexcept`, so running out of memory there is
/// `std::terminate`, not the `std::bad_alloc` the caller is waiting to catch.
/// The range constructor makes the same allocation and may throw. An empty range
/// of move iterators is used so that it asks nothing of `T` beyond being movable.
template <class T>
[[nodiscard]] std::unique_ptr<std::vector<T>> NewEmptyVector() {
    T* const none = nullptr;
    return std::make_unique<std::vector<T>>(std::make_move_iterator(none), std::make_move_iterator(none));
}

/// What a native function was declared with.
///
/// Lives as long as the isolate when a template or a class owns it, and as
/// long as the *value* when `Function::New` or `External::New` made it -
/// `keeper` is that second case: a weak root over the value, whose collection
/// is what gives the record back.
struct CallbackRecord {
    Isolate* owner = nullptr;
    FunctionCallback callback = nullptr;
    CallbackData data;
    v8::Global<v8::Value> keeper;
};

/// A handle frame: a V8 handle scope plus the slot array `Local` indexes into.
///
/// Slots `[0, argCount)` are *borrowed* from the call this frame belongs to -
/// V8 has already rooted those, so reading one copies nothing. Slots above
/// that are the frame's own, inline up to UNIBIND_FRAME_INLINE_SLOTS and on the
/// heap after that. See docs/lifetimes.md section 8.
///
/// The V8 scope is constructed in place rather than held as a member, because
/// escaping needs it destroyed before the deferred escapes are written into
/// the parent - see EscapeSlot.
///
/// A frame opened for a FunctionCallback has no V8 scope of its own
/// (`ownScope` false): V8 has one open around every such call. From script the
/// call stub opens it and closes it as the callback returns, which is when this
/// frame closes too; from the API (`Function::Call`, a `Get` that reaches an
/// accessor) it is the entry point's own internal scope, closed as that call
/// returns; from V8's C++ runtime it is the runtime operation's, closed when
/// the operation is done. A second one inside it bought nothing and cost an
/// open and a close per call. Such a frame is never escapable, so nothing
/// needs its scope.
struct Frame {
    Frame(Isolate& owner, Frame* parent, const v8::FunctionCallbackInfo<v8::Value>* args, bool escapable,
          bool ownScope = true)
        : owner(&owner),
          parent(parent),
          epoch(owner.impl().nextEpoch++),
          args(args),
          argCount(args == nullptr ? 0U : static_cast<uint32_t>(args->Length())),
          escapable(escapable),
          scopeAlive(ownScope) {
        assert((ownScope || !escapable) && "an escapable frame needs a scope of its own to escape from");
        if (!ownScope) {
            return;
        }
        v8::Isolate* isolate = owner.impl().isolate;
        if (escapable) {
            ::new (static_cast<void*>(scopeStorage)) v8::EscapableHandleScope(isolate);
        } else {
            ::new (static_cast<void*>(scopeStorage)) v8::HandleScope(isolate);
        }
    }

    /// A closed frame's epoch is one no handle of it carries, so a handle that
    /// outlived it is diagnosed whether or not a later frame reused the storage.
    /// An optimised build usually does reuse it and an unoptimised one usually
    /// does not, and without this the check caught only the first. Volatile,
    /// because a store in a destructor to an object that is about to end is
    /// exactly the store an optimiser may drop.
    ~Frame() {
        CloseV8Scope();
        ApplyDeferredEscapes();
#if UNIBIND_HANDLE_CHECKS
        *static_cast<volatile uint32_t*>(&epoch) = ~epoch;
#endif
    }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;

    /// `NO_SLOT` when the frame could not grow. The caller turns that into an
    /// empty handle - never into a slot that reads as a value. The overflow
    /// vector is the only thing here that can fail, and it fails by throwing,
    /// so the throw is caught and answered rather than escaping a noexcept
    /// boundary into the engine (which would be std::terminate, i.e. "fatal"
    /// chosen by accident instead of by decision).
    static constexpr SlotIndex NO_SLOT = ~SlotIndex{0};

    [[nodiscard]] SlotIndex Push(v8::Local<v8::Value> value) noexcept {
        if (count < UNIBIND_FRAME_INLINE_SLOTS) {
            inlineSlots[count] = value;  // NOLINT(*-constant-array-index)
            return argCount + count++;
        }
        try {
            if (overflow == nullptr) {
                overflow = NewEmptyVector<v8::Local<v8::Value>>();
            }
            overflow->push_back(value);
        } catch (const std::bad_alloc&) {
            return NO_SLOT;
        }
        return argCount + count++;
    }

    /// Make room for `more` pushes, so that they cannot fail. False when the
    /// room could not be made; nothing has been pushed either way.
    [[nodiscard]] bool Reserve(SlotIndex more) noexcept {
        const SlotIndex needed = count + more;
        if (needed <= UNIBIND_FRAME_INLINE_SLOTS) {
            return true;
        }
        try {
            if (overflow == nullptr) {
                overflow = NewEmptyVector<v8::Local<v8::Value>>();
            }
            overflow->reserve(needed - UNIBIND_FRAME_INLINE_SLOTS);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }

    [[nodiscard]] v8::Local<v8::Value> At(SlotIndex index) const {
        if (index < argCount) {
            return (*args)[static_cast<int>(index)];
        }
        const SlotIndex own = index - argCount;
        assert(own < count);
        if (own < UNIBIND_FRAME_INLINE_SLOTS) {
            return inlineSlots[own];  // NOLINT(*-constant-array-index)
        }
        return (*overflow)[own - UNIBIND_FRAME_INLINE_SLOTS];
    }

    void SetAt(SlotIndex index, v8::Local<v8::Value> value) {
        assert(index >= argCount);
        const SlotIndex own = index - argCount;
        if (own < UNIBIND_FRAME_INLINE_SLOTS) {
            inlineSlots[own] = value;  // NOLINT(*-constant-array-index)
        } else {
            (*overflow)[own - UNIBIND_FRAME_INLINE_SLOTS] = value;
        }
    }

    [[nodiscard]] v8::EscapableHandleScope& EscapableScope() noexcept {
        return *reinterpret_cast<v8::EscapableHandleScope*>(scopeStorage);
    }

    void CloseV8Scope() noexcept {
        if (!scopeAlive) {
            return;
        }
        scopeAlive = false;
        if (escapable) {
            EscapableScope().~EscapableHandleScope();
        } else {
            reinterpret_cast<v8::HandleScope*>(scopeStorage)->~HandleScope();
        }
    }

    /// Runs after this frame's V8 scope has closed, so the parent's is current
    /// again and a handle made here belongs to the parent.
    // it writes the parent's slots and empties `deferred`; const is reachable only because both are behind pointers,
    // and would say the opposite of what this does. NOLINTNEXTLINE(readability-make-member-function-const)
    void ApplyDeferredEscapes() {
        if (deferred == nullptr) {
            return;
        }
        v8::Isolate* isolate = owner->impl().isolate;
        for (auto& entry : *deferred) {
            parent->SetAt(entry.first, entry.second.Get(isolate));
            entry.second.Reset();
        }
    }

    Isolate* owner;
    Frame* parent;
    uint32_t epoch;
    const v8::FunctionCallbackInfo<v8::Value>* args;
    uint32_t argCount;
    bool escapable;
    bool escapeUsed = false;
    bool scopeAlive;
    uint32_t count = 0;
    // Owned: the frame allocates each one lazily and it goes with the frame.
    std::unique_ptr<std::vector<v8::Local<v8::Value>>> overflow;
    std::unique_ptr<std::vector<std::pair<SlotIndex, v8::Global<v8::Value>>>> deferred;
    // The frame's own storage, sized by the ABI constants: a fixed run of slots
    // indexed by ordinal, and the bytes the V8 scope is constructed in.
    v8::Local<v8::Value> inlineSlots[UNIBIND_FRAME_INLINE_SLOTS];
    alignas(alignof(v8::EscapableHandleScope)) unsigned char scopeStorage[sizeof(v8::EscapableHandleScope)];
};
static_assert(sizeof(Frame) <= UNIBIND_FRAME_STORAGE_SIZE,
              "UNIBIND_FRAME_STORAGE_SIZE in CMakeLists.txt is too small for the V8 frame");
static_assert(alignof(Frame) <= UNIBIND_FRAME_STORAGE_ALIGN, "UNIBIND_FRAME_STORAGE_ALIGN is too small");

struct ContextRec {
    Isolate* owner = nullptr;
    v8::Global<v8::Context> handle;
    int refs = 1;
    /// This realm's own `Object.getPrototypeOf` and `Object.setPrototypeOf`,
    /// read when it was made and before any script could replace them.
    ///
    /// V8's API has no call that runs a proxy's `getPrototypeOf` or
    /// `setPrototypeOf` trap: `GetPrototype` reads the proxy's own map, and
    /// `SetPrototype` runs the trap but swallows what it throws - along with the
    /// TypeError of any refusal, ordinary objects included. The language's
    /// functions do all of it, invariant checks and all, so the backend calls
    /// them. See `GetPrototype` and `SetPrototype`.
    v8::Global<v8::Function> getPrototypeOf;
    v8::Global<v8::Function> setPrototypeOf;
    /// The scripts whose last run was in this realm, and which keep it bound -
    /// the head of a list threaded through `ScriptRec`. Each is unbound when
    /// this record goes, so that no script's cache outlives the realm or keeps
    /// it alive. See `ScriptRec::bound`.
    ScriptRec* boundScripts = nullptr;
};

/// Compiled source, kept *unbound* - not tied to the realm it was compiled in.
///
/// `v8::Script` is bound to a context and keeps looking at that context's
/// globals whatever you pass to `Run`, which would make `Script::Run`'s context
/// parameter a lie the moment a second realm exists. `v8::UnboundScript` is the
/// facility for exactly this: compile once, bind per realm.
struct ScriptRec {
    Isolate* owner = nullptr;
    v8::Global<v8::UnboundScript> handle;
    /// Whether a blob handed to CompileScriptWithCache was accepted. False for
    /// a plain compile and false for a rejected blob - to a caller those are
    /// the same thing, which is that this compile paid full price.
    bool usedCache = false;
    /// The script bound to the realm it last ran in, so that running it there
    /// again does not bind it again: binding makes a new function each time,
    /// and was most of what a `Run` cost. One realm, the last - a script run
    /// in turn across several binds on every change, as it always did.
    ///
    /// A bound script holds its realm, so the cache follows the realm's
    /// record: the script is on `boundRealm->boundScripts`, and `ReleaseContext`
    /// unbinds it when that record goes. Keyed on the record rather than on
    /// the V8 context, because a record is what the embedder's `Context` owns.
    ContextRec* boundRealm = nullptr;
    v8::Global<v8::Script> bound;
    ScriptRec* previousBound = nullptr;
    ScriptRec* nextBound = nullptr;
};

namespace {

/// Drop `script`'s bound copy, if it has one, and take it off its realm's list.
void Unbind(ScriptRec* script) noexcept {
    ContextRec* realm = script->boundRealm;
    if (realm == nullptr) {
        return;
    }
    if (script->previousBound != nullptr) {
        script->previousBound->nextBound = script->nextBound;
    } else {
        realm->boundScripts = script->nextBound;
    }
    if (script->nextBound != nullptr) {
        script->nextBound->previousBound = script->previousBound;
    }
    script->previousBound = nullptr;
    script->nextBound = nullptr;
    script->boundRealm = nullptr;
    script->bound.Reset();
}

/// Put `script`, bound to `realm`, at the head of that realm's list.
void BindTo(ScriptRec* script, ContextRec* realm, v8::Local<v8::Script> bound) {
    script->bound.Reset(realm->owner->impl().isolate, bound);
    script->boundRealm = realm;
    script->previousBound = nullptr;
    script->nextBound = realm->boundScripts;
    if (realm->boundScripts != nullptr) {
        realm->boundScripts->previousBound = script;
    }
    realm->boundScripts = script;
}

}  // namespace

struct GlobalNode {
    Isolate* owner = nullptr;
    v8::Global<v8::Value> handle;
};

/// A native getter and setter pair, and the property name they answer for.
///
/// The pair is installed as a real ECMAScript accessor property backed by two
/// function templates, not as one of V8's native data properties. V8 15.6
/// hands a native accessor a `PropertyCallbackInfo`, which reports the holder
/// and never the receiver - so an accessor declared on a prototype, which is
/// where instance accessors belong, could not find the instance it was read
/// on, and `Class<T>::Accessor` would have nothing to unwrap. A function call
/// has a receiver, so this shape has one too.
struct AccessorRecord {
    Isolate* owner = nullptr;
    AccessorGetterCallback getter = nullptr;
    AccessorSetterCallback setter = nullptr;
    CallbackData data;
    v8::Global<v8::Name> name;
};

/// An object or function template. Isolate-owned and isolate-lifetime, so the
/// handle a caller holds is a raw pointer and copying one is free.
struct TemplateRec {
    enum class Kind : uint8_t { Object, Function };

    Isolate* owner = nullptr;
    Kind kind = Kind::Object;
    v8::Global<v8::Template> handle;
    /// A function template's two child templates, made on first ask so that
    /// asking twice yields the same record.
    TemplateRec* prototype = nullptr;
    TemplateRec* instance = nullptr;

    /// What instantiating this template instantiates too, so that `Seal` can
    /// follow it: the function template an object template is the prototype
    /// or instance template of, the one a function template inherits from,
    /// and every template set as a property of this one.
    TemplateRec* function = nullptr;
    TemplateRec* parent = nullptr;
    std::vector<TemplateRec*> nested;
    /// Whether V8 may have instantiated this template. From then on it treats
    /// a change to the shape of the constructor it built - class name, parent,
    /// instance handlers - as a fatal error, so those calls are ignored rather
    /// than passed on. See `unibind/template.h`.
    bool sealed = false;
};

/// A class: a constructor template, its prototype and its instance shape, plus
/// the callback that makes the native. Isolate-lifetime like a template.
struct ClassRec {
    Isolate* owner = nullptr;
    TypeId nativeType;
    NativeConstructor constructor = nullptr;
    /// Whether a plain call is legal, and makes an instance the way `new`
    /// does. Off by default; `Class<T>::ConstructOrCall` turns it on.
    bool callable = false;
    TemplateRec* function = nullptr;
    TemplateRec* prototype = nullptr;
    TemplateRec* instance = nullptr;
};

/// One live instance carrying a native, and the weak handle that says when it
/// has gone. The isolate keeps these so every *box* is destroyed exactly once:
/// by the finalizer if V8 gets to it, by ~Isolate if it does not. Destroying a
/// box gives back one share of the native, which is a different event - see the
/// two invariants at the top of unibind/class.h.
struct InstanceRecord {
    Isolate* owner = nullptr;
    NativeBox* box = nullptr;
    v8::Global<v8::Object> handle;
};

struct TryCatchState {
    explicit TryCatchState(Isolate& owner) : owner(&owner), tryCatch(owner.impl().isolate) {}

    Isolate* owner;
    v8::TryCatch tryCatch;
};

static_assert(sizeof(TryCatchState) <= UNIBIND_TRY_CATCH_STORAGE_SIZE, "UNIBIND_TRY_CATCH_STORAGE_SIZE is too small");
static_assert(alignof(TryCatchState) <= UNIBIND_TRY_CATCH_STORAGE_ALIGN,
              "UNIBIND_TRY_CATCH_STORAGE_ALIGN is too small");

struct ContextScopeState {
    v8::Local<v8::Context> context;
};

static_assert(sizeof(ContextScopeState) <= UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE,
              "UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE is too small");

struct CallbackState;

/// Where a callback's result goes, with the engine's type taken off it.
///
/// V8 types a return slot by the hook that owns it - `FunctionCallbackInfo<Value>`
/// for a call, `PropertyCallbackInfo<Value>` for an interceptor getter,
/// `<Integer>` for a query, `<Boolean>` for a setter or deleter, `<Array>` for
/// an enumerator - and the setters on each are not the same set: you cannot
/// hand a `double` to a `ReturnValue<Integer>` and it will not compile. So the
/// six `SetReturn*` entry points, which are one set for every hook, cannot
/// name one V8 type.
///
/// This is the erasure: a table of six function pointers, one static instance
/// per hook shape, picked where the `CallbackState` is built - which is the
/// one place that statically knows the shape. Each entry is a one-line
/// instantiation over the info type, so it inlines straight back to V8's own
/// typed setter; the cost is one indirect call per result written, once per
/// call rather than once per value.
///
/// Nothing public names this type. `CallbackState` is the backend's own, so a
/// second engine erases its return slot however it likes - SpiderMonkey's
/// `JS::MutableHandleValue` out-parameter would not need a table at all. What
/// the two backends must share is the behaviour, not the mechanism: see the
/// contract at the top of unibind/function.h.
struct ReturnSink {
    void (*handle)(const CallbackState& state, Slot value) noexcept;
    void (*undefinedValue)(const CallbackState& state) noexcept;
    void (*nullValue)(const CallbackState& state) noexcept;
    void (*boolean)(const CallbackState& state, bool value) noexcept;
    void (*number)(const CallbackState& state, double value) noexcept;
    void (*integer)(const CallbackState& state, int32_t value) noexcept;
};

/// One per-call state for every shape of callback: a call, a construction, an
/// accessor, an interceptor hook. `call` is the argument list and is null for
/// anything that has none; `info` is whatever V8 handed the hook, erased, and
/// is understood only by `returns`.
struct CallbackState {
    Isolate* owner = nullptr;
    Frame* frame = nullptr;
    /// The realm the call is running in, settled on first ask: see `CallRealm`.
    CallRealm* realm = nullptr;
    CallbackData data;
    v8::Local<v8::Object> receiver;
    v8::Local<v8::Object> holder;
    const v8::FunctionCallbackInfo<v8::Value>* call = nullptr;
    const void* info = nullptr;
    const ReturnSink* returns = nullptr;
    /// The script value a `Function::New` with one carries, or empty for every
    /// other callback. A `Local` is safe to hold here where it would not be on
    /// the other engine: V8 does not move what a handle names, and the call's
    /// own handle scope keeps it for as long as this state exists.
    v8::Local<v8::Value> value;
};

// ---------------------------------------------------------------------------
// Slot plumbing
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] Slot MakeSlot(Frame& frame, SlotIndex index) noexcept {
#if UNIBIND_HANDLE_CHECKS
    return Slot{.frame = &frame, .index = index, .epoch = frame.epoch};
#else
    return Slot{.frame = &frame, .index = index};
#endif
}

[[nodiscard]] v8::Local<v8::Value> Resolve(Slot slot) noexcept {
    assert(slot.frame != nullptr && "handle used while empty");
#if UNIBIND_HANDLE_CHECKS
    assert(slot.epoch == slot.frame->epoch && "handle used after its HandleScope closed");
#endif
    return slot.frame->At(slot.index);
}

[[nodiscard]] Isolate& IsolateFor(Slot slot) noexcept {
    return *slot.frame->owner;
}

[[nodiscard]] v8::Isolate* Raw(Isolate& isolate) noexcept {
    return isolate.impl().isolate;
}

/// An empty handle, plus the condition reported where script will see it. The
/// message is a literal, so saying so costs no allocation of ours - which
/// matters, because the reason we are here is that an allocation failed.
///
/// The embedder hears about it too, as `EngineFault::OutOfMemory`. This engine
/// has its own hook for its own heap and nothing that covers ours, so raising
/// it here is what makes running out of memory reach an embedder the same way
/// on both backends - the other one routes this path through the engine's own
/// out-of-memory report and gets it for free. Reporting comes first because
/// the throw below builds an Error object, on a heap that may be no better off
/// than the allocator that just failed.
[[nodiscard]] Slot NoSlot(Isolate& isolate) noexcept {
    ReportEngineFault(EngineFault::OutOfMemory, &isolate, "HandleScope", "a handle scope could not grow");
    Raw(isolate)->ThrowError("out of memory: a handle scope could not grow");
    return Slot{};
}

[[nodiscard]] Slot Push(Isolate& isolate, v8::Local<v8::Value> value) noexcept {
    Frame* frame = isolate.impl().current;
    assert(frame != nullptr && "a value was created with no HandleScope open");
    const SlotIndex index = frame->Push(value);
    if (index == Frame::NO_SLOT) {
        return NoSlot(isolate);
    }
    return MakeSlot(*frame, index);
}

/// The `std::optional<Slot>` form: a frame that could not grow is an empty answer,
/// exactly as a call that threw is.
[[nodiscard]] std::optional<Slot> PushOrNothing(Isolate& isolate, v8::Local<v8::Value> value) noexcept {
    const Slot slot = Push(isolate, value);
    if (slot.IsEmpty()) {
        return std::nullopt;
    }
    return slot;
}

[[nodiscard]] std::optional<Slot> PushMaybe(Isolate& isolate, v8::MaybeLocal<v8::Value> value) noexcept {
    v8::Local<v8::Value> local;
    if (!value.ToLocal(&local)) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, local);
}

[[nodiscard]] ContextRec* RecOf(const Context& context) noexcept {
    return context.rec();
}

[[nodiscard]] v8::Local<v8::Context> Raw(const Context& context) noexcept {
    ContextRec* rec = RecOf(context);
    return rec->handle.Get(Raw(*rec->owner));
}

[[nodiscard]] Isolate& OwnerOf(const Context& context) noexcept {
    return *RecOf(context)->owner;
}

/// `context` entered for the life of this object - unless it is already the
/// isolate's current realm, which is the common case, and then nothing at all.
///
/// V8 makes an object, an array, a buffer, a view or an error in the current
/// realm, and binds a script to it; that is the only reason the factories enter
/// one. When the realm asked for is already current, entering it again makes
/// the same thing in the same realm, and costs an `Enter` and an `Exit` - each a
/// push onto two of V8's stacks - on every call. Compared against V8's own
/// answer rather than against anything this backend tracks: V8 changes the
/// current realm behind any tracking of ours whenever script calls a function
/// of another one, and an interop caller can enter one itself.
class EnterUnlessCurrent {
   public:
    explicit EnterUnlessCurrent(const Context& context) noexcept {
        ContextRec* rec = RecOf(context);
        v8::Isolate* raw = rec->owner->impl().isolate;
        if (rec->handle != raw->GetCurrentContext()) {
            entered_ = rec->handle.Get(raw);
            entered_->Enter();
        }
    }
    ~EnterUnlessCurrent() {
        if (!entered_.IsEmpty()) {
            entered_->Exit();
        }
    }

    EnterUnlessCurrent(const EnterUnlessCurrent&) = delete;
    EnterUnlessCurrent& operator=(const EnterUnlessCurrent&) = delete;
    EnterUnlessCurrent(EnterUnlessCurrent&&) = delete;
    EnterUnlessCurrent& operator=(EnterUnlessCurrent&&) = delete;

   private:
    v8::Local<v8::Context> entered_;
};

/// Whether a stop is in force - and if one is, V8's own termination armed
/// again, so that whatever script is reached next stops at its first check.
///
/// V8 refuses to run script while its own termination is pending, and that is
/// not the same thing as a stop being in force: it forgets one as soon as its
/// unwind is done, and a native whose own `TryCatch` has caught the stop is no
/// longer "terminating" to V8 either. Either way the next getter, trap,
/// conversion or call ran. The stop is ours to keep (`unibind/isolate.h`), so
/// every entry point that can reach script asks here first, as every one of
/// SpiderMonkey's backend does; the ones that run script outright ask again
/// when it came back empty, which re-arms the engine for whatever is reached
/// afterwards.
[[nodiscard]] bool Stopped(Isolate& isolate) noexcept {
    if (!isolate.impl().terminating.load(std::memory_order_acquire)) {
        return false;
    }
    isolate.impl().isolate->TerminateExecution();
    return true;
}

/// Whether these bytes are UTF-8 - strictly, which is the only useful kind.
///
/// V8 does not ask: `NewFromUtf8` replaces whatever it does not understand
/// with U+FFFD and reports success, so `String::New`'s promise ("empty if the
/// bytes are not valid UTF-8") is this library's to keep. Silence there is the
/// failure the whole API is written against - the caller is told it handed
/// over text, and what arrived is a row of replacement characters.
///
/// Rejected, because each of them is a way for two encoders to disagree about
/// the same bytes: a continuation byte where a lead byte belongs, a sequence
/// cut short, an overlong form, an encoded surrogate, and anything above
/// U+10FFFF.
///
/// The rule is the one `String::NewFromUtf8` repairs by, in `unibind/value.h`,
/// so that the two cannot disagree about which bytes needed repairing.
[[nodiscard]] bool IsUtf8(std::string_view text) noexcept {
    return FirstInvalidUtf8(text) == std::string_view::npos;
}

/// Every string this backend makes goes through here, for two reasons.
///
/// V8's length is an `int` and a **negative** one means "read until the NUL":
/// a view longer than `kMaxLength` would be read past its end - a
/// `string_view` need not be terminated at all - rather than refused. And
/// `NewFromUtf8` accepts bytes that are not UTF-8, which the header says it
/// does not.
[[nodiscard]] v8::MaybeLocal<v8::String> NewString(Isolate& isolate, std::string_view text) noexcept {
    if (text.size() > static_cast<size_t>(v8::String::kMaxLength) || !IsUtf8(text)) {
        return {};
    }
    return v8::String::NewFromUtf8(Raw(isolate), text.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(text.size()));
}

/// A string of text the embedder declared - a property or function name, a
/// class name, a template's string constant, a script's resource name -
/// decoded as `String::NewFromUtf8` decodes: bytes that are not UTF-8 become
/// U+FFFD. `NewString` is the strict form, for `String::New`, and refusing is
/// not an answer here: there is nowhere to report it, and the check behind a
/// refusal is one that ends the process.
[[nodiscard]] v8::Local<v8::String> RawString(Isolate& isolate, std::string_view text) noexcept {
    const size_t firstInvalid = FirstInvalidUtf8(text);
    if (firstInvalid == std::string_view::npos) {
        return NewString(isolate, text).ToLocalChecked();
    }
    return NewString(isolate, ReplaceInvalidUtf8(text, firstInvalid)).ToLocalChecked();
}

[[nodiscard]] std::optional<bool> FromV8(v8::Maybe<bool> value) noexcept {
    if (value.IsNothing()) {
        return std::nullopt;
    }
    return value.FromJust();
}

/// The sink for a hook whose result the engine reads as a value: a call, and
/// an interceptor getter. `Info` is `FunctionCallbackInfo<Value>` or
/// `PropertyCallbackInfo<Value>`; both carry a `ReturnValue<Value>`, which is
/// the only one of V8's return slots that takes every kind of result.
template <class Info>
struct ValueReturn {
    [[nodiscard]] static const Info& Of(const CallbackState& state) noexcept {
        return *static_cast<const Info*>(state.info);
    }

    static void SetHandle(const CallbackState& state, Slot value) noexcept {
        Of(state).GetReturnValue().Set(Resolve(value));
    }
    static void SetUndefined(const CallbackState& state) noexcept { Of(state).GetReturnValue().SetUndefined(); }
    static void SetNull(const CallbackState& state) noexcept { Of(state).GetReturnValue().SetNull(); }
    static void SetBoolean(const CallbackState& state, bool value) noexcept { Of(state).GetReturnValue().Set(value); }
    static void SetNumber(const CallbackState& state, double value) noexcept { Of(state).GetReturnValue().Set(value); }
    static void SetInteger(const CallbackState& state, int32_t value) noexcept {
        Of(state).GetReturnValue().Set(value);
    }

    static constexpr ReturnSink SINK{.handle = &SetHandle,
                                     .undefinedValue = &SetUndefined,
                                     .nullValue = &SetNull,
                                     .boolean = &SetBoolean,
                                     .number = &SetNumber,
                                     .integer = &SetInteger};
};

/// The sink for a hook whose answer is its C++ return value - an interceptor
/// query, deleter, setter or enumerator. V8's return slot on those carries the
/// hook's protocol (the attributes, the delete result, the key array), so a
/// callback writing its own value there would corrupt it. The contract in
/// unibind/function.h says such a write does nothing; this is that nothing, and it
/// is a deliberate no-op rather than an unimplemented operation.
struct DiscardReturn {
    static void SetHandle(const CallbackState& /*state*/, Slot /*value*/) noexcept {}
    static void SetUndefined(const CallbackState& /*state*/) noexcept {}
    static void SetNull(const CallbackState& /*state*/) noexcept {}
    static void SetBoolean(const CallbackState& /*state*/, bool /*value*/) noexcept {}
    static void SetNumber(const CallbackState& /*state*/, double /*value*/) noexcept {}
    static void SetInteger(const CallbackState& /*state*/, int32_t /*value*/) noexcept {}

    static constexpr ReturnSink SINK{.handle = &SetHandle,
                                     .undefinedValue = &SetUndefined,
                                     .nullValue = &SetNull,
                                     .boolean = &SetBoolean,
                                     .number = &SetNumber,
                                     .integer = &SetInteger};
};

using CallReturn = ValueReturn<v8::FunctionCallbackInfo<v8::Value>>;
using PropertyValueReturn = ValueReturn<v8::PropertyCallbackInfo<v8::Value>>;

/// The realm a callback is running in, as its record. Every context this
/// library makes carries one in embedder slot 0.
[[nodiscard]] ContextRec* CurrentContextRec(v8::Isolate* isolate) noexcept {
    v8::Local<v8::Context> raw = isolate->GetCurrentContext();
    assert(!raw.IsEmpty() && "a callback ran with no current context");
    return static_cast<ContextRec*>(
        raw->GetAlignedPointerFromEmbedderData(CONTEXT_SLOT, v8::kEmbedderDataTypeTagDefault));
}

[[nodiscard]] Isolate& OwnerOf(v8::Isolate* isolate) noexcept {
    return *static_cast<Isolate*>(isolate->GetData(ISOLATE_SLOT));
}

}  // namespace

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

Frame& OpenFrame(Isolate& isolate, FrameStorage& storage, bool escapable) noexcept {
    auto* frame = ::new (static_cast<void*>(storage.bytes)) Frame(isolate, isolate.impl().current, nullptr, escapable);
    isolate.impl().current = frame;
    return *frame;
}

void CloseFrame(Frame& frame) noexcept {
    Isolate& owner = *frame.owner;
    assert(owner.impl().current == &frame && "handle scopes must close in reverse order of opening");
    // The parent must be current before ~Frame writes the deferred escapes
    // into it, because a handle made then belongs to whatever scope is open.
    owner.impl().current = frame.parent;
    frame.~Frame();
}

// V8 handles are pointers into the innermost open scope's block, so copying a
// Local into the parent's slot array would copy a pointer into storage that is
// about to be released. The value has to be handed over the boundary, and V8
// offers exactly one cheap way to do that per scope: EscapableHandleScope,
// which reserves a slot in the parent when it is constructed.
//
// So the first escape is free. Any further one parks the value in a global
// root, reserves an empty parent slot now, and fills it in once this frame's
// V8 scope has closed - see Frame::ApplyDeferredEscapes. That costs one global
// handle per escape beyond the first, which is the honest price of V8's
// one-escape rule; see docs/lifetimes.md section 6.
Slot EscapeSlot(Frame& closing, Slot value) noexcept {
    Frame* parent = closing.parent;
    assert(parent != nullptr && "nothing to escape into: no enclosing HandleScope");
    assert(closing.escapable && "this frame was not opened as escapable");

    v8::Local<v8::Value> local = Resolve(value);
    Isolate& owner = *closing.owner;

    if (!closing.escapeUsed) {
        closing.escapeUsed = true;
        const SlotIndex index = parent->Push(closing.EscapableScope().Escape(local));
        return index == Frame::NO_SLOT ? NoSlot(owner) : MakeSlot(*parent, index);
    }

    const SlotIndex index = parent->Push(v8::Local<v8::Value>());
    if (index == Frame::NO_SLOT) {
        return NoSlot(owner);
    }
    try {
        if (closing.deferred == nullptr) {
            closing.deferred = NewEmptyVector<std::pair<SlotIndex, v8::Global<v8::Value>>>();
        }
        closing.deferred->emplace_back(index, v8::Global<v8::Value>(Raw(owner), local));
    } catch (const std::bad_alloc&) {
        // The parent slot is reserved and empty, and nothing will ever fill
        // it, so the handle naming it would read as an empty local rather than
        // as the escaped value. Say so instead.
        return NoSlot(owner);
    }
    return MakeSlot(*parent, index);
}
Isolate& IsolateOf(Frame& frame) noexcept {
    return *frame.owner;
}

Frame* CurrentFrame(Isolate& isolate) noexcept {
    return isolate.impl().current;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

ValueKind KindOf(Slot value) noexcept {
    v8::Local<v8::Value> raw = Resolve(value);
    if (raw.IsEmpty() || raw->IsUndefined()) {
        return ValueKind::Undefined;
    }
    if (raw->IsNull()) {
        return ValueKind::Null;
    }
    if (raw->IsBoolean()) {
        return ValueKind::Boolean;
    }
    if (raw->IsNumber()) {
        return ValueKind::Number;
    }
    if (raw->IsString()) {
        return ValueKind::String;
    }
    if (raw->IsSymbol()) {
        return ValueKind::Symbol;
    }
    if (raw->IsBigInt()) {
        return ValueKind::BigInt;
    }
    if (raw->IsExternal()) {
        return ValueKind::External;
    }
    if (raw->IsFunction()) {
        return ValueKind::Function;
    }
    if (raw->IsArray()) {
        return ValueKind::Array;
    }
    if (raw->IsObject()) {
        return ValueKind::Object;
    }
    return ValueKind::Other;
}

bool IsType(Slot value, TypeCode type) noexcept {
    v8::Local<v8::Value> raw = Resolve(value);
    switch (type) {
        case TypeCode::Value:
            return true;
        case TypeCode::Primitive:
            return !raw->IsObject();
        case TypeCode::Boolean:
            return raw->IsBoolean();
        case TypeCode::Number:
            return raw->IsNumber();
        case TypeCode::Integer:
            return raw->IsInt32();
        case TypeCode::Name:
            return raw->IsName();
        case TypeCode::String:
            return raw->IsString();
        case TypeCode::Symbol:
            return raw->IsSymbol();
        case TypeCode::BigInt:
            return raw->IsBigInt();
        case TypeCode::Object:
            // V8 15.6 counts its own External as an object; this API does not
            // (`unibind/handle.h`), because an External has no properties to
            // offer and SpiderMonkey's is a private class no script may touch.
            return raw->IsObject() && !raw->IsExternal();
        case TypeCode::Array:
            return raw->IsArray();
        case TypeCode::Function:
            return raw->IsFunction();
        case TypeCode::ArrayBuffer:
            return raw->IsArrayBuffer();
        case TypeCode::ArrayBufferView:
            return raw->IsArrayBufferView();
        case TypeCode::TypedArray:
            return raw->IsTypedArray();
        case TypeCode::DataView:
            return raw->IsDataView();
        case TypeCode::Promise:
            return raw->IsPromise();
        case TypeCode::External:
            return raw->IsExternal();
    }
    return false;
}

bool StrictEquals(Slot lhs, Slot rhs) noexcept {
    return Resolve(lhs)->StrictEquals(Resolve(rhs));
}

bool SameValue(Slot lhs, Slot rhs) noexcept {
    return Resolve(lhs)->SameValue(Resolve(rhs));
}

std::optional<bool> LooseEquals(const Context& context, Slot lhs, Slot rhs) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return FromV8(Resolve(lhs)->Equals(Raw(context), Resolve(rhs)));
}

// ---------------------------------------------------------------------------
// Reading primitives
// ---------------------------------------------------------------------------

bool BooleanValue(Slot value) noexcept {
    return Resolve(value)->BooleanValue(Raw(IsolateFor(value)));
}

double NumberValue(Slot value) noexcept {
    return Resolve(value).As<v8::Number>()->Value();
}

int32_t Int32Value(Slot value) noexcept {
    v8::Local<v8::Value> raw = Resolve(value);
    if (raw->IsInt32()) {
        return raw.As<v8::Int32>()->Value();
    }
    return NumberToInt32(raw.As<v8::Number>()->Value());
}

size_t Utf8Length(Slot string) noexcept {
    return Resolve(string).As<v8::String>()->Utf8Length(Raw(IsolateFor(string)));
}

size_t WriteUtf8(Slot string, std::span<char> out) noexcept {
    if (out.empty()) {
        return 0;
    }
    // `kReplaceInvalidUtf8` because this hands back UTF-8 and a JavaScript
    // string is UTF-16: script can hold half a surrogate pair, whose naive
    // encoding (ED A0 80) is not UTF-8 at all and is not something this API
    // can hand an embedder that asked for UTF-8. U+FFFD is three bytes, the
    // same as the sequence it replaces, so nothing about the length moves.
    return Resolve(string).As<v8::String>()->WriteUtf8(Raw(IsolateFor(string)), out.data(), out.size(),
                                                       v8::String::WriteFlags::kReplaceInvalidUtf8);
}

std::string ToStdString(Slot string) {
    v8::Isolate* isolate = Raw(IsolateFor(string));
    v8::Local<v8::String> raw = Resolve(string).As<v8::String>();
    std::string result(raw->Utf8Length(isolate), '\0');
    const size_t written =
        raw->WriteUtf8(isolate, result.data(), result.size(), v8::String::WriteFlags::kReplaceInvalidUtf8);
    result.resize(written);
    return result;
}

std::optional<std::string> SymbolDescription(Slot symbol) {
    v8::Isolate* isolate = Raw(IsolateFor(symbol));
    v8::Local<v8::Value> description = Resolve(symbol).As<v8::Symbol>()->Description(isolate);
    if (description.IsEmpty() || description->IsUndefined()) {
        return std::nullopt;
    }
    const v8::String::Utf8Value text(isolate, description);
    if (*text == nullptr) {
        return std::nullopt;
    }
    return std::string(*text, static_cast<size_t>(text.length()));
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

std::optional<bool> ToBoolean(const Context& context, Slot value) {
    (void)context;
    return Resolve(value)->BooleanValue(Raw(IsolateFor(value)));
}

std::optional<double> ToNumber(const Context& context, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Maybe<double> number = Resolve(value)->NumberValue(Raw(context));
    if (number.IsNothing()) {
        return std::nullopt;
    }
    return number.FromJust();
}

std::optional<int32_t> ToInt32(const Context& context, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Maybe<int32_t> number = Resolve(value)->Int32Value(Raw(context));
    if (number.IsNothing()) {
        return std::nullopt;
    }
    return number.FromJust();
}

std::optional<uint32_t> ToUint32(const Context& context, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Maybe<uint32_t> number = Resolve(value)->Uint32Value(Raw(context));
    if (number.IsNothing()) {
        return std::nullopt;
    }
    return number.FromJust();
}

std::optional<Slot> ToJsString(const Context& context, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::String> string;
    if (!Resolve(value)->ToString(Raw(context)).ToLocal(&string)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), string);
}

std::optional<Slot> ToJsObject(const Context& context, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::Object> object;
    if (!Resolve(value)->ToObject(Raw(context)).ToLocal(&object)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), object);
}

// ---------------------------------------------------------------------------
// Making values
// ---------------------------------------------------------------------------

Slot MakeUndefined(Isolate& isolate) noexcept {
    return Push(isolate, v8::Undefined(Raw(isolate)));
}
Slot MakeNull(Isolate& isolate) noexcept {
    return Push(isolate, v8::Null(Raw(isolate)));
}
Slot MakeBoolean(Isolate& isolate, bool value) noexcept {
    return Push(isolate, v8::Boolean::New(Raw(isolate), value));
}
Slot MakeNumber(Isolate& isolate, double value) noexcept {
    return Push(isolate, v8::Number::New(Raw(isolate), value));
}
Slot MakeInteger(Isolate& isolate, int32_t value) noexcept {
    return Push(isolate, v8::Integer::New(Raw(isolate), value));
}
Slot MakeUnsigned(Isolate& isolate, uint32_t value) noexcept {
    return Push(isolate, v8::Integer::NewFromUnsigned(Raw(isolate), value));
}

std::optional<Slot> MakeString(Isolate& isolate, std::string_view utf8) {
    v8::Local<v8::String> string;
    if (!NewString(isolate, utf8).ToLocal(&string)) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, string);
}

std::optional<Slot> MakeSymbol(Isolate& isolate, std::optional<std::string_view> description) {
    v8::Local<v8::String> text;
    if (description && !NewString(isolate, *description).ToLocal(&text)) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, v8::Symbol::New(Raw(isolate), text));
}

std::optional<Slot> MakeSymbolFor(Isolate& isolate, std::string_view key) {
    v8::Local<v8::String> text;
    if (!NewString(isolate, key).ToLocal(&text)) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, v8::Symbol::For(Raw(isolate), text));
}

namespace {

[[nodiscard]] v8::Local<v8::Symbol> RawWellKnownSymbol(Isolate& isolate, WellKnownSymbol which) noexcept {
    v8::Isolate* raw = Raw(isolate);
    switch (which) {
        case WellKnownSymbol::Iterator:
            return v8::Symbol::GetIterator(raw);
        case WellKnownSymbol::AsyncIterator:
            return v8::Symbol::GetAsyncIterator(raw);
        case WellKnownSymbol::HasInstance:
            return v8::Symbol::GetHasInstance(raw);
        case WellKnownSymbol::ToPrimitive:
            return v8::Symbol::GetToPrimitive(raw);
        case WellKnownSymbol::ToStringTag:
            return v8::Symbol::GetToStringTag(raw);
    }
    return {};
}

}  // namespace

std::optional<Slot> GetWellKnownSymbol(Isolate& isolate, WellKnownSymbol which) {
    v8::Local<v8::Symbol> symbol = RawWellKnownSymbol(isolate, which);
    if (symbol.IsEmpty()) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, symbol);
}

/// The longest array `MakeArray` has the engine size a backing store for up
/// front: long enough for anything built element by element, short enough that
/// the store is small.
constexpr uint32_t ARRAY_PREALLOCATED = 1U << 16;

std::optional<Slot> MakeObject(const Context& context) {
    // Entered unless current, here and in every factory below that is handed a
    // realm: V8 makes an object, an array, a buffer, a view or an error in the
    // isolate's *current* realm, and the caller may have a different one entered.
    const EnterUnlessCurrent entered(context);
    return PushOrNothing(OwnerOf(context), v8::Object::New(Raw(OwnerOf(context))));
}

std::optional<Slot> MakeArray(const Context& context, uint32_t length) {
    // A length up to `ARRAY_PREALLOCATED` is handed to `Array::New`, which sizes
    // a backing store for it, so filling it in costs no growth. Anything longer
    // is an empty array whose length is then set - what `new Array(length)`
    // makes, holes throughout and nothing allocated for them - because
    // `Array::New` takes an `int`, reading a length above INT_MAX as zero, and
    // ends the process ("invalid size") on a large one within it.
    const EnterUnlessCurrent entered(context);
    Isolate& owner = OwnerOf(context);
    v8::Isolate* raw = Raw(owner);
    if (length <= ARRAY_PREALLOCATED) {
        return PushOrNothing(owner, v8::Array::New(raw, static_cast<int>(length)));
    }
    v8::Local<v8::Array> array = v8::Array::New(raw, 0);
    if (array.IsEmpty()) {
        return std::nullopt;
    }
    v8::Local<v8::String> key = v8::String::NewFromUtf8Literal(raw, "length");
    if (array->Set(Raw(context), key, v8::Number::New(raw, length)).IsNothing()) {
        return std::nullopt;
    }
    return PushOrNothing(owner, array);
}

std::optional<Slot> MakeError(const Context& context, ErrorKind kind, std::string_view message) {
    const EnterUnlessCurrent entered(context);
    Isolate& owner = OwnerOf(context);
    v8::Local<v8::String> text = RawString(owner, message);
    v8::Local<v8::Value> error;
    switch (kind) {
        case ErrorKind::TypeError:
            error = v8::Exception::TypeError(text);
            break;
        case ErrorKind::RangeError:
            error = v8::Exception::RangeError(text);
            break;
        case ErrorKind::ReferenceError:
            error = v8::Exception::ReferenceError(text);
            break;
        case ErrorKind::SyntaxError:
            error = v8::Exception::SyntaxError(text);
            break;
        case ErrorKind::Error:
            error = v8::Exception::Error(text);
            break;
    }
    return PushOrNothing(owner, error);
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

std::optional<Slot> GetProperty(const Context& context, Slot object, Slot key) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return PushMaybe(OwnerOf(context), Resolve(object).As<v8::Object>()->Get(Raw(context), Resolve(key)));
}

namespace {

/// The largest array index, 2^32 - 2. The one `uint32_t` above it is an
/// ordinary property name, and V8 cannot be handed it as an index: its by-index
/// calls look the index up as a `size_t`, and on a 32-bit build 2^32 - 1 is
/// `size_t(-1)` - the lookup's own "not an index" marker - so it goes looking
/// for a name that is not there and crashes. As a key it is what script
/// spells `o[4294967295]`, which V8 reads correctly everywhere.
constexpr uint32_t MAX_ARRAY_INDEX = 0xFFFFFFFEU;

}  // namespace

std::optional<Slot> GetIndex(const Context& context, Slot object, uint32_t index) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::Object> target = Resolve(object).As<v8::Object>();
    if (index > MAX_ARRAY_INDEX) {
        return PushMaybe(OwnerOf(context), target->Get(Raw(context), v8::Number::New(Raw(OwnerOf(context)), index)));
    }
    return PushMaybe(OwnerOf(context), target->Get(Raw(context), index));
}

std::optional<bool> SetProperty(const Context& context, Slot object, Slot key, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return FromV8(Resolve(object).As<v8::Object>()->Set(Raw(context), Resolve(key), Resolve(value)));
}

std::optional<bool> SetIndex(const Context& context, Slot object, uint32_t index, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::Object> target = Resolve(object).As<v8::Object>();
    if (index > MAX_ARRAY_INDEX) {
        return FromV8(target->Set(Raw(context), v8::Number::New(Raw(OwnerOf(context)), index), Resolve(value)));
    }
    return FromV8(target->Set(Raw(context), index, Resolve(value)));
}

std::optional<bool> DefineProperty(const Context& context, Slot object, Slot key, Slot value,
                                   PropertyAttribute attributes) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return FromV8(Resolve(object).As<v8::Object>()->DefineOwnProperty(
        Raw(context), Resolve(key).As<v8::Name>(), Resolve(value),
        static_cast<v8::PropertyAttribute>(static_cast<uint8_t>(attributes))));
}

std::optional<bool> HasProperty(const Context& context, Slot object, Slot key) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return FromV8(Resolve(object).As<v8::Object>()->Has(Raw(context), Resolve(key)));
}

std::optional<bool> HasOwnProperty(const Context& context, Slot object, Slot key) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return FromV8(Resolve(object).As<v8::Object>()->HasOwnProperty(Raw(context), Resolve(key).As<v8::Name>()));
}

std::optional<bool> DeleteProperty(const Context& context, Slot object, Slot key) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    return FromV8(Resolve(object).As<v8::Object>()->Delete(Raw(context), Resolve(key)));
}

// V8 reports `None` for a property that is not there at all, which would be a
// lie: `None` is what a plain writable/enumerable/configurable property has.
// Asking whether it exists first is one extra lookup on a cold path, and it is
// the only way an absent property can answer "no attributes" rather than "the
// default ones".
//
// And V8's own attribute query answers `None` for a proxy that reports no
// descriptor, so the attributes are read from the descriptor itself, one
// object up the chain at a time - `Object.getOwnPropertyDescriptor` at each,
// prototypes as `GetPrototype` reads them, traps included - which is how the
// other engine finds them. No descriptor anywhere, whatever `has` said, is no
// attributes.
std::optional<PropertyAttribute> GetPropertyAttributes(const Context& context, Slot object, Slot key) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::Context> realm = Raw(context);
    v8::Isolate* raw = Raw(OwnerOf(context));
    v8::Local<v8::Object> holder = Resolve(object).As<v8::Object>();
    v8::Local<v8::Name> name = Resolve(key).As<v8::Name>();
    v8::Maybe<bool> present = holder->Has(realm, name);
    if (present.IsNothing() || !present.FromJust()) {
        return std::nullopt;
    }
    while (true) {
        v8::Local<v8::Value> found;
        if (!holder->GetOwnPropertyDescriptor(realm, name).ToLocal(&found)) {
            return std::nullopt;
        }
        if (found->IsObject()) {
            v8::Local<v8::Object> descriptor = found.As<v8::Object>();
            const auto flag = [&](const char* field) -> std::optional<bool> {
                v8::Local<v8::Value> value;
                if (!descriptor->Get(realm, v8::String::NewFromUtf8(raw, field).ToLocalChecked()).ToLocal(&value)) {
                    return std::nullopt;
                }
                return value->BooleanValue(raw);
            };
            const std::optional<bool> enumerable = flag("enumerable");
            const std::optional<bool> configurable = flag("configurable");
            const v8::Maybe<bool> isData =
                descriptor->HasOwnProperty(realm, v8::String::NewFromUtf8Literal(raw, "value"));
            if (!enumerable || !configurable || isData.IsNothing()) {
                return std::nullopt;
            }
            // Only a data descriptor has one; an accessor's lookup would reach
            // `Object.prototype`, which script may have given a `writable`.
            const std::optional<bool> writable = isData.FromJust() ? flag("writable") : std::optional<bool>(true);
            if (!writable) {
                return std::nullopt;
            }
            PropertyAttribute attributes = PropertyAttribute::None;
            if (!*enumerable) {
                attributes = attributes | PropertyAttribute::DontEnum;
            }
            if (!*configurable) {
                attributes = attributes | PropertyAttribute::DontDelete;
            }
            if (!*writable) {
                attributes = attributes | PropertyAttribute::ReadOnly;
            }
            return attributes;
        }
        v8::Local<v8::Value> next;
        if (holder->IsProxy()) {
            std::array<v8::Local<v8::Value>, 1> arguments{holder};
            if (!RecOf(context)
                     ->getPrototypeOf.Get(raw)
                     ->Call(realm, v8::Undefined(raw), 1, arguments.data())
                     .ToLocal(&next)) {
                return std::nullopt;
            }
        } else {
            next = holder->GetPrototype();
        }
        if (!next->IsObject()) {
            return std::nullopt;
        }
        holder = next.As<v8::Object>();
    }
}

std::optional<Slot> GetOwnPropertyNames(const Context& context, Slot object, KeyFilter filter) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    int propertyFilter = filter.includeNonEnumerable ? v8::ALL_PROPERTIES : v8::ONLY_ENUMERABLE;
    if (!filter.includeSymbols) {
        propertyFilter |= v8::SKIP_SYMBOLS;
    }
    v8::Local<v8::Array> names;
    if (!Resolve(object)
             .As<v8::Object>()
             ->GetOwnPropertyNames(Raw(context), static_cast<v8::PropertyFilter>(propertyFilter),
                                   v8::KeyConversionMode::kKeepNumbers)
             .ToLocal(&names)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), names);
}

// `Object.getPrototypeOf` and `Object.setPrototypeOf`, as the realm's own
// functions run them - see `ContextRec::getPrototypeOf` for why V8's API calls
// will not do. Reading an ordinary object's prototype runs no script, so that
// one still takes the API's short way; a proxy's goes through its trap.

std::optional<Slot> GetPrototype(const Context& context, Slot object) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::Object> target = Resolve(object).As<v8::Object>();
    if (!target->IsProxy()) {
        return PushOrNothing(OwnerOf(context), target->GetPrototype());
    }
    v8::Isolate* raw = Raw(OwnerOf(context));
    std::array<v8::Local<v8::Value>, 1> arguments{target};
    return PushMaybe(OwnerOf(context), RecOf(context)->getPrototypeOf.Get(raw)->Call(Raw(context), v8::Undefined(raw),
                                                                                     1, arguments.data()));
}

std::optional<bool> SetPrototype(const Context& context, Slot object, Slot prototype) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Isolate* raw = Raw(OwnerOf(context));
    std::array<v8::Local<v8::Value>, 2> arguments{Resolve(object), Resolve(prototype)};
    v8::Local<v8::Value> result;
    if (!RecOf(context)
             ->setPrototypeOf.Get(raw)
             ->Call(Raw(context), v8::Undefined(raw), 2, arguments.data())
             .ToLocal(&result)) {
        return std::nullopt;
    }
    return true;
}
uint32_t ArrayLength(Slot array) noexcept {
    return Resolve(array).As<v8::Array>()->Length();
}

// ---------------------------------------------------------------------------
// Binary data
//
// Copied in, copied out. unibind/value.h argues why there is no borrowing
// alternative; here it just means every path below ends in a memcpy against a
// backing store this isolate owns.
// ---------------------------------------------------------------------------

std::optional<Slot> MakeArrayBuffer(const Context& context, std::span<const std::byte> bytes, size_t byteLength) {
    assert(bytes.size() <= byteLength && "an array buffer was asked to hold more than it is long");
    const EnterUnlessCurrent entered(context);
    Isolate& owner = OwnerOf(context);
    // `MaybeNew`, and the ceiling checked first: `New` answers an allocation
    // that fails by ending the process, and both answer a length past the
    // engine's maximum that way. A length is often a number read from input,
    // and the header promises an empty answer for a buffer that cannot be made.
    if (byteLength > v8::ArrayBuffer::kMaxByteLength) {
        return std::nullopt;
    }
    v8::Local<v8::ArrayBuffer> buffer;
    if (!v8::ArrayBuffer::MaybeNew(Raw(owner), byteLength).ToLocal(&buffer)) {
        return std::nullopt;
    }
    if (!bytes.empty()) {
        std::memcpy(buffer->Data(), bytes.data(), bytes.size());
    }
    return PushOrNothing(owner, buffer);
}

size_t ArrayBufferByteLength(Slot buffer) noexcept {
    return Resolve(buffer).As<v8::ArrayBuffer>()->ByteLength();
}

size_t ArrayBufferCopyOut(Slot buffer, std::span<std::byte> out) noexcept {
    v8::Local<v8::ArrayBuffer> raw = Resolve(buffer).As<v8::ArrayBuffer>();
    const size_t count = std::min(raw->ByteLength(), out.size());
    if (count != 0) {
        std::memcpy(out.data(), raw->Data(), count);
    }
    return count;
}

namespace {

/// Every typed-array constructor, in one place, because the only thing that
/// differs between them is the V8 type.
template <class View>
[[nodiscard]] v8::Local<v8::TypedArray> NewView(v8::Local<v8::ArrayBuffer> buffer, size_t byteOffset, size_t length) {
    return View::New(buffer, byteOffset, length);
}

[[nodiscard]] v8::Local<v8::TypedArray> NewTypedArray(ElementType type, v8::Local<v8::ArrayBuffer> buffer,
                                                      size_t byteOffset, size_t length) {
    switch (type) {
        case ElementType::Int8:
            return NewView<v8::Int8Array>(buffer, byteOffset, length);
        case ElementType::Uint8:
            return NewView<v8::Uint8Array>(buffer, byteOffset, length);
        case ElementType::Uint8Clamped:
            return NewView<v8::Uint8ClampedArray>(buffer, byteOffset, length);
        case ElementType::Int16:
            return NewView<v8::Int16Array>(buffer, byteOffset, length);
        case ElementType::Uint16:
            return NewView<v8::Uint16Array>(buffer, byteOffset, length);
        case ElementType::Int32:
            return NewView<v8::Int32Array>(buffer, byteOffset, length);
        case ElementType::Uint32:
            return NewView<v8::Uint32Array>(buffer, byteOffset, length);
        case ElementType::Float32:
            return NewView<v8::Float32Array>(buffer, byteOffset, length);
        case ElementType::Float64:
            return NewView<v8::Float64Array>(buffer, byteOffset, length);
        case ElementType::BigInt64:
            return NewView<v8::BigInt64Array>(buffer, byteOffset, length);
        case ElementType::BigUint64:
            return NewView<v8::BigUint64Array>(buffer, byteOffset, length);
        case ElementType::Float16:
            return NewView<v8::Float16Array>(buffer, byteOffset, length);
    }
    return {};
}

}  // namespace

std::optional<Slot> MakeTypedArray(const Context& context, ElementType type, Slot buffer, size_t byteOffset,
                                   size_t length) {
    const EnterUnlessCurrent entered(context);
    Isolate& owner = OwnerOf(context);
    v8::Local<v8::ArrayBuffer> raw = Resolve(buffer).As<v8::ArrayBuffer>();
    // V8 checks this too, but by aborting rather than by failing, so the check
    // has to be made here and made in a form that cannot wrap: a length whose
    // byte count overflows would otherwise pass a `byteOffset + wanted >
    // byteLength` test and reach V8 with a length it aborts on.
    const size_t elementSize = ElementSize(type);
    const size_t byteLength = raw->ByteLength();
    if (elementSize == 0 || length > static_cast<size_t>(-1) / elementSize) {
        return std::nullopt;
    }
    const size_t wanted = length * elementSize;
    if (byteOffset > byteLength || wanted > byteLength - byteOffset) {
        return std::nullopt;
    }
    // Two more that V8 enforces by aborting, or not at all: an offset the
    // element type cannot start at - a RangeError in script, a failed CHECK
    // here - and a detached buffer, a TypeError in script that V8's API would
    // make a view over without a word. `DataView::New` already refuses it.
    if (byteOffset % elementSize != 0 || raw->WasDetached()) {
        return std::nullopt;
    }
    v8::Local<v8::TypedArray> view = NewTypedArray(type, raw, byteOffset, length);
    if (view.IsEmpty()) {
        return std::nullopt;
    }
    return PushOrNothing(owner, view);
}

ElementType TypedArrayElementType(Slot view) noexcept {
    v8::Local<v8::Value> raw = Resolve(view);
    if (raw->IsInt8Array()) {
        return ElementType::Int8;
    }
    if (raw->IsUint8Array()) {
        return ElementType::Uint8;
    }
    if (raw->IsUint8ClampedArray()) {
        return ElementType::Uint8Clamped;
    }
    if (raw->IsInt16Array()) {
        return ElementType::Int16;
    }
    if (raw->IsUint16Array()) {
        return ElementType::Uint16;
    }
    if (raw->IsInt32Array()) {
        return ElementType::Int32;
    }
    if (raw->IsUint32Array()) {
        return ElementType::Uint32;
    }
    if (raw->IsFloat32Array()) {
        return ElementType::Float32;
    }
    if (raw->IsBigInt64Array()) {
        return ElementType::BigInt64;
    }
    if (raw->IsBigUint64Array()) {
        return ElementType::BigUint64;
    }
    if (raw->IsFloat16Array()) {
        return ElementType::Float16;
    }
    return ElementType::Float64;
}

size_t TypedArrayLength(Slot view) noexcept {
    return Resolve(view).As<v8::TypedArray>()->Length();
}

size_t TypedArrayByteOffset(Slot view) noexcept {
    return Resolve(view).As<v8::TypedArray>()->ByteOffset();
}

// The same answer as for any view, a `SharedArrayBuffer` refused included.
std::optional<Slot> TypedArrayBuffer(const Context& context, Slot view) {
    return ArrayBufferViewBuffer(context, view);
}

size_t TypedArrayCopyOut(Slot view, std::span<std::byte> out) noexcept {
    v8::Local<v8::TypedArray> raw = Resolve(view).As<v8::TypedArray>();
    return raw->CopyContents(out.data(), out.size());
}

// Any view, typed array or DataView: V8's own ArrayBufferView answers every
// one of these, including zero for a view whose buffer has been detached.

size_t ArrayBufferViewByteLength(Slot view) noexcept {
    return Resolve(view).As<v8::ArrayBufferView>()->ByteLength();
}

size_t ArrayBufferViewByteOffset(Slot view) noexcept {
    return Resolve(view).As<v8::ArrayBufferView>()->ByteOffset();
}

std::optional<Slot> ArrayBufferViewBuffer(const Context& context, Slot view) {
    v8::Local<v8::ArrayBufferView> raw = Resolve(view).As<v8::ArrayBufferView>();
    // `Buffer()` is typed as an ArrayBuffer and hands back a SharedArrayBuffer
    // regardless when that is what is underneath, which is the lie the header
    // refuses to pass on.
    v8::Local<v8::Value> buffer = raw->Buffer();
    if (buffer.IsEmpty() || !buffer->IsArrayBuffer()) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), buffer);
}

size_t ArrayBufferViewCopyOut(Slot view, std::span<std::byte> out) noexcept {
    return Resolve(view).As<v8::ArrayBufferView>()->CopyContents(out.data(), out.size());
}

std::optional<Slot> MakeDataView(const Context& context, Slot buffer, size_t byteOffset, size_t byteLength) {
    const EnterUnlessCurrent entered(context);
    v8::Local<v8::ArrayBuffer> raw = Resolve(buffer).As<v8::ArrayBuffer>();
    // Checked here for the reason `MakeTypedArray` checks: V8 enforces the
    // bounds by aborting, not by failing. A detached buffer is refused too -
    // `new DataView` over one is a TypeError in script, and V8's API would
    // otherwise make one without a word.
    const size_t available = raw->ByteLength();
    if (raw->WasDetached() || byteOffset > available || byteLength > available - byteOffset) {
        return std::nullopt;
    }
    v8::Local<v8::DataView> view = v8::DataView::New(raw, byteOffset, byteLength);
    if (view.IsEmpty()) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), view);
}

// ---------------------------------------------------------------------------
// Promises
// ---------------------------------------------------------------------------

std::optional<Slot> MakePromise(const Context& context) {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(Raw(context)).ToLocal(&resolver)) {
        return std::nullopt;
    }
    // The resolver IS the promise on this engine, and SpiderMonkey has no
    // resolver at all, so the public API hands back the promise and settles it
    // by casting back. See unibind/value.h.
    return PushOrNothing(OwnerOf(context), resolver->GetPromise());
}

std::optional<bool> ResolvePromise(const Context& context, Slot promise, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    v8::Local<v8::Promise> raw = Resolve(promise).As<v8::Promise>();
    if (raw->State() != v8::Promise::kPending) {
        return false;
    }
    return FromV8(raw.As<v8::Promise::Resolver>()->Resolve(Raw(context), Resolve(value)));
}

std::optional<bool> RejectPromise(const Context& context, Slot promise, Slot reason) {
    v8::Local<v8::Promise> raw = Resolve(promise).As<v8::Promise>();
    if (raw->State() != v8::Promise::kPending) {
        return false;
    }
    return FromV8(raw.As<v8::Promise::Resolver>()->Reject(Raw(context), Resolve(reason)));
}

PromiseState PromiseStateOf(Slot promise) noexcept {
    switch (Resolve(promise).As<v8::Promise>()->State()) {
        case v8::Promise::kFulfilled:
            return PromiseState::Fulfilled;
        case v8::Promise::kRejected:
            return PromiseState::Rejected;
        case v8::Promise::kPending:
            break;
    }
    return PromiseState::Pending;
}

// ---------------------------------------------------------------------------
// Structured clone
//
// The default delegate refuses a host object rather than inventing one, which
// is the behaviour unibind/value.h specifies: a value that will not clone fails the
// whole call. `ThrowDataCloneError` is pure virtual, so there has to be a
// delegate at all; ours throws the ordinary error and lets the TryCatch or the
// empty optional carry it.
// ---------------------------------------------------------------------------

namespace {

class CloneDelegate final : public v8::ValueSerializer::Delegate {
   public:
    explicit CloneDelegate(v8::Isolate* isolate) noexcept : isolate_(isolate) {}

    void ThrowDataCloneError(v8::Local<v8::String> message) override {
        isolate_->ThrowException(v8::Exception::Error(message));
    }

   private:
    v8::Isolate* isolate_;
};

}  // namespace

std::optional<std::vector<uint8_t>> SerializeValue(const Context& context, Slot value) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    Isolate& owner = OwnerOf(context);
    const EnterUnlessCurrent entered(context);
    CloneDelegate delegate(Raw(owner));
    v8::ValueSerializer serializer(Raw(owner), &delegate);
    serializer.WriteHeader();
    if (serializer.WriteValue(Raw(context), Resolve(value)).IsNothing()) {
        return std::nullopt;
    }
    std::pair<uint8_t*, size_t> buffer = serializer.Release();
    std::vector<uint8_t> blob(buffer.first, buffer.first + buffer.second);
    // Release() hands over memory the serializer allocated with its delegate's
    // allocator, which here is the default - free, not delete.
    free(buffer.first);  // NOLINT(cppcoreguidelines-no-malloc)
    return blob;
}

std::optional<Slot> DeserializeValue(const Context& context, std::span<const uint8_t> blob) {
    Isolate& owner = OwnerOf(context);
    const EnterUnlessCurrent entered(context);
    v8::ValueDeserializer deserializer(Raw(owner), blob.data(), blob.size());
    if (deserializer.ReadHeader(Raw(context)).IsNothing()) {
        return std::nullopt;
    }
    v8::Local<v8::Value> value;
    if (!deserializer.ReadValue(Raw(context)).ToLocal(&value)) {
        return std::nullopt;
    }
    return PushOrNothing(owner, value);
}

// ---------------------------------------------------------------------------
// Calling
// ---------------------------------------------------------------------------

namespace {

/// Slots are (frame, index) pairs, so an argument list has to be resolved into
/// real V8 handles before the call. Small lists stay on the stack.
class ArgumentBuffer {
   public:
    explicit ArgumentBuffer(std::span<const Slot> slots) : values_(slots.size()) {
        for (size_t i = 0; i < slots.size(); ++i) {
            values_[i] = Resolve(slots[i]);
        }
    }

    [[nodiscard]] v8::Local<v8::Value>* data() noexcept { return values_.data(); }
    [[nodiscard]] int size() const noexcept { return static_cast<int>(values_.size()); }

   private:
    std::vector<v8::Local<v8::Value>> values_;
};

}  // namespace

std::optional<Slot> CallFunction(const Context& context, Slot function, Slot receiver,
                                 std::span<const Slot> arguments) {
    Isolate& owner = OwnerOf(context);
    if (Stopped(owner)) {
        return std::nullopt;
    }
    ArgumentBuffer buffer(arguments);
    v8::Local<v8::Value> result;
    if (!Resolve(function)
             .As<v8::Function>()
             ->Call(Raw(context), Resolve(receiver), buffer.size(), buffer.data())
             .ToLocal(&result)) {
        (void)Stopped(owner);
        return std::nullopt;
    }
    return PushOrNothing(owner, result);
}

std::optional<Slot> ConstructObject(const Context& context, Slot function, std::span<const Slot> arguments) {
    Isolate& owner = OwnerOf(context);
    if (Stopped(owner)) {
        return std::nullopt;
    }
    ArgumentBuffer buffer(arguments);
    v8::Local<v8::Object> result;
    if (!Resolve(function)
             .As<v8::Function>()
             ->NewInstance(Raw(context), buffer.size(), buffer.data())
             .ToLocal(&result)) {
        (void)Stopped(owner);
        return std::nullopt;
    }
    return PushOrNothing(owner, result);
}

// ---------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------

namespace {

/// Whether this native call is too deep to be let run, in which case a
/// RangeError has been thrown in its place and it must return at once.
///
/// V8 checks the stack where it enters script and nowhere else. A callback
/// that goes straight back through the engine from C++ - reading the property
/// it is answering for, calling the function it is - enters no script: V8
/// calls an interceptor, an accessor's function and a native function directly
/// from its API, so that recursion never meets the check and walks off the end
/// of the thread's stack. Every trampoline asks this first, against the guard
/// `Isolate::New` worked out from the thread's real stack.
[[nodiscard]] bool RefuseDeeperNative(Isolate& isolate) {
    const char here = 0;
    if (reinterpret_cast<uintptr_t>(&here) >= isolate.impl().stackGuard) {
        return false;
    }
    ThrowError(isolate, ErrorKind::RangeError, "Maximum call stack size exceeded");
    return true;
}

/// Opens a frame for the duration of a callback, borrowing the argument array.
class CallFrame {
   public:
    /// `args` is null for a callback with no argument list - an accessor read
    /// through an interceptor, say - which simply means the frame borrows
    /// nothing and every slot in it is its own.
    ///
    /// A call borrows V8's own handle scope as well as its arguments: V8 opens
    /// one around every FunctionCallback (see `Frame`). A property hook keeps a
    /// scope of its own, because V8 promises it none - the runtime calls
    /// interceptors from C++ as often as from generated code, and a lookup
    /// that asks one per key would otherwise keep every key's handles until it
    /// was done.
    CallFrame(Isolate& isolate, const v8::FunctionCallbackInfo<v8::Value>* args) : isolate_(&isolate) {
        frame_ = ::new (static_cast<void*>(storage_.bytes))
            Frame(isolate, isolate.impl().current, args, false, /*ownScope=*/args == nullptr);
        isolate.impl().current = frame_;
        realm_.outer = isolate.impl().innermostCall;
        isolate.impl().innermostCall = &realm_;
    }
    ~CallFrame() {
        isolate_->impl().innermostCall = realm_.outer;
        isolate_->impl().current = frame_->parent;
        frame_->~Frame();
    }

    CallFrame(const CallFrame&) = delete;
    CallFrame& operator=(const CallFrame&) = delete;
    CallFrame(CallFrame&&) = delete;
    CallFrame& operator=(CallFrame&&) = delete;

    [[nodiscard]] Frame& frame() const noexcept { return *frame_; }
    [[nodiscard]] CallRealm& realm() noexcept { return realm_; }

   private:
    Isolate* isolate_;
    Frame* frame_ = nullptr;
    CallRealm realm_;
    // Not zeroed: the frame is constructed into it at once, and zeroing a
    // frame's worth of bytes on every call was a cost with nothing to show.
    FrameStorage storage_;
};

/// The per-call state for anything V8 calls as a function: a native function,
/// a template method, a class constructor, or one half of an accessor pair.
[[nodiscard]] CallbackState CallState(Isolate& isolate, CallFrame& frame,
                                      const v8::FunctionCallbackInfo<v8::Value>& info, CallbackData data) {
    return CallbackState{.owner = &isolate,
                         .frame = &frame.frame(),
                         .realm = &frame.realm(),
                         .data = data,
                         // V8 15.6 drops Holder() from a function call, so the
                         // receiver is all there is to report as either.
                         .receiver = info.This(),
                         .holder = info.This(),
                         .call = &info,
                         .info = &info,
                         .returns = &CallReturn::SINK};
}

/// The pointer inside the External a callback was declared with.
template <class T>
[[nodiscard]] T* PointerFrom(v8::Local<v8::Data> data) noexcept {
    return static_cast<T*>(data.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
}

/// V8 hands a callback one pointer and every shape here needs at least two, so
/// the pair gets a stable home in the isolate and the pointer to it is what
/// goes into the External.
///
/// What is made here lives as long as the isolate. A record that belongs to a
/// *value* is made the same way and then tied to it with `KeepWithValue`.
[[nodiscard]] CallbackRecord* AdoptCallback(Isolate& isolate, FunctionCallback callback, CallbackData data) {
    auto record = std::make_unique<CallbackRecord>();
    record->owner = &isolate;
    record->callback = callback;
    record->data = data;
    CallbackRecord* raw = record.get();
    isolate.impl().callbacks.emplace(raw, std::move(record));
    return raw;
}

void FinalizeCallbackRecordLate(const v8::WeakCallbackInfo<CallbackRecord>& data) {
    CallbackRecord* record = data.GetParameter();
    record->owner->impl().callbacks.erase(record);
}

void FinalizeCallbackRecord(const v8::WeakCallbackInfo<CallbackRecord>& data) {
    // V8 insists the first pass resets the handle before asking for a second,
    // and the erase - which destroys the record - belongs in the second.
    data.GetParameter()->keeper.Reset();
    data.SetSecondPassCallback(&FinalizeCallbackRecordLate);
}

/// Tie a record's life to the value that carries it.
///
/// A function from `Function::New` and an external are values: script drops
/// them, the collector takes them, and nothing in the API says they leave
/// anything behind. Without this the record stays until the isolate goes, so a
/// program that makes a function per call leaks one small allocation per call
/// and nothing reports it.
void KeepWithValue(Isolate& isolate, CallbackRecord* record, v8::Local<v8::Value> value) {
    record->keeper.Reset(Raw(isolate), value);
    record->keeper.SetWeak(record, &FinalizeCallbackRecord, v8::WeakCallbackType::kParameter);
}

template <class T>
[[nodiscard]] v8::Local<v8::External> Pointer(Isolate& isolate, T* value) noexcept {
    return v8::External::New(Raw(isolate), value, v8::kExternalPointerTypeTagDefault);
}

void FunctionTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    auto* record = PointerFrom<CallbackRecord>(info.DataV2());

    CallFrame frame(isolate, &info);
    CallbackState state = CallState(isolate, frame, info, record->data);
    record->callback(CallbackInfo(state));
}

/// The two internal fields of a value-carrying function's data.
constexpr int VALUE_DATA_CALLBACK_FIELD = 0;
constexpr int VALUE_DATA_VALUE_FIELD = 1;

/// A function with a script value for its data.
///
/// V8 gives a function one data value, and this needs two things in it: the
/// callback, and the embedder's value. So the data is an object of the
/// isolate's `valueDataTemplate` with both in internal fields - an object the
/// collector traces like any other, reachable from the function and from
/// nothing else. That is the whole of the lifetime story: no record in the
/// isolate, no weak root to give one back, and no way for a value that refers
/// to its own function to pin the pair, which a strong root of ours would do.
///
/// A trampoline of its own, so that the `CallbackData` path costs what it
/// always did.
void ValueFunctionTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    v8::Local<v8::Object> bundle = info.DataV2().As<v8::Value>().As<v8::Object>();
    const auto callback = reinterpret_cast<FunctionCallback>(bundle->GetInternalField(VALUE_DATA_CALLBACK_FIELD)
                                                                 .As<v8::Value>()
                                                                 .As<v8::External>()
                                                                 ->Value(v8::kExternalPointerTypeTagDefault));

    CallFrame frame(isolate, &info);
    CallbackState state = CallState(isolate, frame, info, {});
    state.value = bundle->GetInternalField(VALUE_DATA_VALUE_FIELD).As<v8::Value>();
    callback(CallbackInfo(state));
}

[[nodiscard]] v8::Local<v8::ObjectTemplate> ValueDataTemplate(Isolate& isolate) {
    v8::Isolate* raw = Raw(isolate);
    auto& kept = isolate.impl().valueDataTemplate;
    if (kept.IsEmpty()) {
        v8::Local<v8::ObjectTemplate> shape = v8::ObjectTemplate::New(raw);
        shape->SetInternalFieldCount(2);
        kept.Reset(raw, shape);
    }
    return kept.Get(raw);
}

}  // namespace

std::optional<Slot> MakeFunction(const Context& context, FunctionCallback callback, CallbackData data) {
    Isolate& owner = OwnerOf(context);
    CallbackRecord* record = AdoptCallback(owner, callback, data);
    v8::Local<v8::Function> function;
    // Callable, not constructable - the decision documented on Function::New.
    if (!v8::Function::New(Raw(context), &FunctionTrampoline, Pointer(owner, record), 0,
                           v8::ConstructorBehavior::kThrow)
             .ToLocal(&function)) {
        // Nothing will ever reach the record, so nothing would ever give it
        // back either.
        owner.impl().callbacks.erase(record);
        return std::nullopt;
    }
    KeepWithValue(owner, record, function);
    return PushOrNothing(owner, function);
}

std::optional<Slot> MakeFunctionWithValue(const Context& context, FunctionCallback callback, Slot data) {
    Isolate& owner = OwnerOf(context);
    v8::Local<v8::Context> realm = Raw(context);
    v8::Local<v8::Object> bundle;
    if (!ValueDataTemplate(owner)->NewInstance(realm).ToLocal(&bundle)) {
        return std::nullopt;
    }
    bundle->SetInternalField(VALUE_DATA_CALLBACK_FIELD, Pointer(owner, reinterpret_cast<void*>(callback)));
    bundle->SetInternalField(VALUE_DATA_VALUE_FIELD, Resolve(data));
    v8::Local<v8::Function> function;
    // Callable, not constructable - exactly as `MakeFunction`.
    if (!v8::Function::New(realm, &ValueFunctionTrampoline, bundle, 0, v8::ConstructorBehavior::kThrow)
             .ToLocal(&function)) {
        return std::nullopt;
    }
    return PushOrNothing(owner, function);
}

std::optional<Slot> MakeExternal(Isolate& isolate, CallbackData data) {
    // An External holds one pointer; CallbackData is two words, so it lives in
    // the isolate alongside the callback records - and goes when the external
    // that names it does.
    CallbackRecord* record = AdoptCallback(isolate, nullptr, data);
    v8::Local<v8::External> external = Pointer(isolate, record);
    KeepWithValue(isolate, record, external);
    return PushOrNothing(isolate, external);
}

CallbackData ExternalData(Slot external) noexcept {
    auto* record = PointerFrom<CallbackRecord>(Resolve(external));
    return record->data;
}

// --- what a callback can ask -----------------------------------------------

Isolate& CallbackIsolate(const CallbackState& state) noexcept {
    return *state.owner;
}
const Context& CallbackContext(const CallbackState& state) noexcept {
    CallRealm& realm = *state.realm;
    if (realm.context.IsEmpty()) {
        realm.context = Context::FromRec(CurrentContextRec(Raw(*state.owner)));
    }
    return realm.context;
}

uint32_t CallbackArgumentCount(const CallbackState& state) noexcept {
    // Only a CallbackInfo exposes an argument list, and only a call ever makes
    // one, so a null `call` here means a PropertyCallbackInfo was asked a
    // question its public wrapper does not have.
    assert(state.call != nullptr && "a property callback has no argument list");
    return state.call == nullptr ? 0U : static_cast<uint32_t>(state.call->Length());
}

Slot CallbackArgument(const CallbackState& state, uint32_t index) noexcept {
    if (index >= CallbackArgumentCount(state)) {
        return Push(*state.owner, v8::Undefined(Raw(*state.owner)));
    }
    // Borrowed: the argument is already slot `index` of the call's frame.
    return MakeSlot(*state.frame, index);
}

Slot CallbackThis(const CallbackState& state) noexcept {
    return Push(*state.owner, state.receiver);
}

Slot CallbackHolder(const CallbackState& state) noexcept {
    return Push(*state.owner, state.holder);
}

bool CallbackIsConstruct(const CallbackState& state) noexcept {
    assert(state.call != nullptr && "a property callback is never a construct call");
    return state.call != nullptr && state.call->IsConstructCall();
}

CallbackData CallbackDataOf(const CallbackState& state) noexcept {
    return state.data;
}

Slot CallbackValueData(const CallbackState& state) noexcept {
    if (state.value.IsEmpty()) {
        return Push(*state.owner, v8::Undefined(Raw(*state.owner)));
    }
    return Push(*state.owner, state.value);
}

void SetReturnSlot(const CallbackState& state, Slot value) noexcept {
    state.returns->handle(state, value);
}
void SetReturnUndefined(const CallbackState& state) noexcept {
    state.returns->undefinedValue(state);
}
void SetReturnNull(const CallbackState& state) noexcept {
    state.returns->nullValue(state);
}
void SetReturnBoolean(const CallbackState& state, bool value) noexcept {
    state.returns->boolean(state, value);
}
void SetReturnNumber(const CallbackState& state, double value) noexcept {
    state.returns->number(state, value);
}
void SetReturnInteger(const CallbackState& state, int32_t value) noexcept {
    state.returns->integer(state, value);
}

// ---------------------------------------------------------------------------
// Templates
//
// A unibind template maps one-to-one onto a V8 template, which is already exactly
// this: an isolate-level descriptor instantiated per context. So every
// TemplateSet* applies to the real thing immediately rather than accumulating
// a description to replay later.
//
// A template is built before any context exists and with no handle scope open,
// so each of these opens a V8 handle scope of its own and lets nothing escape
// it except into a Global. The three that DO produce a value - NewInstance,
// GetFunction, HasInstance - do not, because their result belongs in the
// caller's frame like any other.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] v8::Local<v8::Template> RawTemplate(TemplateRec* rec) noexcept {
    return rec->handle.Get(Raw(*rec->owner));
}

[[nodiscard]] v8::Local<v8::ObjectTemplate> RawObjectTemplate(TemplateRec* rec) noexcept {
    assert(rec->kind == TemplateRec::Kind::Object && "this operation wants an object template");
    return RawTemplate(rec).As<v8::ObjectTemplate>();
}

[[nodiscard]] v8::Local<v8::FunctionTemplate> RawFunctionTemplate(TemplateRec* rec) noexcept {
    assert(rec->kind == TemplateRec::Kind::Function && "this operation wants a function template");
    return RawTemplate(rec).As<v8::FunctionTemplate>();
}

template <class T>
[[nodiscard]] TemplateRec* AdoptTemplate(Isolate& isolate, TemplateRec::Kind kind, v8::Local<T> value) {
    auto rec = std::make_unique<TemplateRec>();
    rec->owner = &isolate;
    rec->kind = kind;
    rec->handle.Reset(Raw(isolate), value);
    TemplateRec* raw = rec.get();
    isolate.impl().templates.push_back(std::move(rec));
    return raw;
}

[[nodiscard]] v8::PropertyAttribute RawAttributes(PropertyAttribute attributes) noexcept {
    return static_cast<v8::PropertyAttribute>(static_cast<uint8_t>(attributes));
}

[[nodiscard]] v8::Local<v8::Data> RawConstant(Isolate& isolate, Constant value) noexcept {
    v8::Isolate* raw = Raw(isolate);
    switch (value.GetKind()) {
        case Constant::Kind::Null:
            return v8::Null(raw);
        case Constant::Kind::Boolean:
            return v8::Boolean::New(raw, value.AsBoolean());
        case Constant::Kind::Number:
            return v8::Number::New(raw, value.AsNumber());
        case Constant::Kind::Integer:
            return v8::Integer::New(raw, value.AsInteger());
        case Constant::Kind::String:
            return RawString(isolate, value.AsString());
        case Constant::Kind::Undefined:
            break;
    }
    return v8::Undefined(raw);
}

/// A method as V8 wants one: a function template over our trampoline, carrying
/// the (callback, embedder pointer) pair.
[[nodiscard]] v8::Local<v8::FunctionTemplate> MethodTemplate(Isolate& isolate, FunctionCallback callback,
                                                             CallbackData data) {
    // A method is callable, not constructable; only a FunctionTemplate the
    // caller asked for, or a Class, makes a constructor. See unibind/value.h.
    return v8::FunctionTemplate::New(Raw(isolate), &FunctionTrampoline,
                                     Pointer(isolate, AdoptCallback(isolate, callback, data)),
                                     v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
}

void AccessorGetTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    auto* record = PointerFrom<AccessorRecord>(info.DataV2());

    CallFrame frame(isolate, &info);
    CallbackState state = CallState(isolate, frame, info, record->data);
    const Slot name = Push(isolate, record->name.Get(Raw(isolate)));
    record->getter(Local<Name>::FromSlot(name), PropertyCallbackInfo(state));
}

void AccessorSetTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    auto* record = PointerFrom<AccessorRecord>(info.DataV2());

    CallFrame frame(isolate, &info);
    CallbackState state = CallState(isolate, frame, info, record->data);
    const Slot name = Push(isolate, record->name.Get(Raw(isolate)));
    // Slot 0 of the frame is the assigned value, borrowed from the call.
    const Slot value = info.Length() > 0 ? MakeSlot(frame.frame(), 0) : Push(isolate, v8::Undefined(Raw(isolate)));
    record->setter(Local<Name>::FromSlot(name), Local<Value>::FromSlot(value), PropertyCallbackInfo(state));
}

}  // namespace

TemplateRec* NewObjectTemplate(Isolate& isolate) {
    v8::HandleScope scope(Raw(isolate));
    return AdoptTemplate(isolate, TemplateRec::Kind::Object, v8::ObjectTemplate::New(Raw(isolate)));
}

TemplateRec* NewFunctionTemplate(Isolate& isolate, FunctionCallback callback, CallbackData data) {
    v8::HandleScope scope(Raw(isolate));
    // Not MethodTemplate: this is the facility an embedder reaches for when it
    // wants a constructor, so unlike a method it keeps kAllow.
    v8::Local<v8::FunctionTemplate> tpl =
        callback == nullptr ? v8::FunctionTemplate::New(Raw(isolate))
                            : v8::FunctionTemplate::New(Raw(isolate), &FunctionTrampoline,
                                                        Pointer(isolate, AdoptCallback(isolate, callback, data)));
    return AdoptTemplate(isolate, TemplateRec::Kind::Function, tpl);
}

void TemplateSetConstant(TemplateRec* tpl, std::string_view name, Constant value, PropertyAttribute attributes) {
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));
    RawTemplate(tpl)->Set(RawString(owner, name), RawConstant(owner, value), RawAttributes(attributes));
}

void TemplateSetMethod(TemplateRec* tpl, std::string_view name, FunctionCallback callback, CallbackData data,
                       PropertyAttribute attributes) {
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));
    RawTemplate(tpl)->Set(RawString(owner, name), MethodTemplate(owner, callback, data), RawAttributes(attributes));
}

void TemplateSetSymbolMethod(TemplateRec* tpl, WellKnownSymbol key, FunctionCallback callback, CallbackData data) {
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));
    RawTemplate(tpl)->Set(RawWellKnownSymbol(owner, key), MethodTemplate(owner, callback, data), v8::DontEnum);
}

namespace {

/// The record an accessor's two functions carry, kept for the life of the
/// isolate: a template's accessor is replayed onto every instance it makes,
/// and an object's is reachable for as long as the object is.
[[nodiscard]] AccessorRecord* StoreAccessor(Isolate& owner, v8::Local<v8::String> key, AccessorGetterCallback getter,
                                            AccessorSetterCallback setter, CallbackData data) {
    auto owned = std::make_unique<AccessorRecord>();
    owned->owner = &owner;
    owned->getter = getter;
    owned->setter = setter;
    owned->data = data;
    owned->name.Reset(Raw(owner), key);
    AccessorRecord* record = owned.get();
    owner.impl().accessors.push_back(std::move(owned));
    return record;
}

/// An accessor property has no [[Writable]]: a getter with no setter IS the
/// read-only form, so ReadOnly here would be a contradiction rather than a
/// restriction. Drop it instead of handing V8 a nonsensical descriptor.
[[nodiscard]] PropertyAttribute AccessorAttributes(PropertyAttribute attributes) noexcept {
    return static_cast<PropertyAttribute>(static_cast<uint8_t>(attributes) &
                                          ~static_cast<uint8_t>(PropertyAttribute::ReadOnly));
}

}  // namespace

void TemplateSetAccessor(TemplateRec* tpl, std::string_view name, AccessorGetterCallback getter,
                         AccessorSetterCallback setter, CallbackData data, PropertyAttribute attributes) {
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));
    v8::Local<v8::String> key = RawString(owner, name);
    AccessorRecord* record = StoreAccessor(owner, key, getter, setter, data);

    v8::Local<v8::FunctionTemplate> read;
    v8::Local<v8::FunctionTemplate> write;
    if (getter != nullptr) {
        read = v8::FunctionTemplate::New(Raw(owner), &AccessorGetTrampoline, Pointer(owner, record),
                                         v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
    }
    if (setter != nullptr) {
        write = v8::FunctionTemplate::New(Raw(owner), &AccessorSetTrampoline, Pointer(owner, record),
                                          v8::Local<v8::Signature>(), 1, v8::ConstructorBehavior::kThrow);
    }

    RawTemplate(tpl)->SetAccessorProperty(key, read, write, RawAttributes(AccessorAttributes(attributes)));
}

std::optional<bool> SetAccessorProperty(const Context& context, Slot object, std::string_view name,
                                        AccessorGetterCallback getter, AccessorSetterCallback setter, CallbackData data,
                                        PropertyAttribute attributes) {
    if (Stopped(OwnerOf(context))) {
        return std::nullopt;
    }
    Isolate& owner = IsolateFor(object);
    v8::Local<v8::Context> raw = Raw(context);
    v8::Local<v8::String> key = RawString(owner, name);
    AccessorRecord* record = StoreAccessor(owner, key, getter, setter, data);

    v8::Local<v8::Function> read;
    v8::Local<v8::Function> write;
    if (getter != nullptr &&
        !v8::Function::New(raw, &AccessorGetTrampoline, Pointer(owner, record), 0, v8::ConstructorBehavior::kThrow)
             .ToLocal(&read)) {
        return std::nullopt;
    }
    if (setter != nullptr &&
        !v8::Function::New(raw, &AccessorSetTrampoline, Pointer(owner, record), 1, v8::ConstructorBehavior::kThrow)
             .ToLocal(&write)) {
        return std::nullopt;
    }
    // `DefineProperty` rather than `SetAccessorProperty`, which returns nothing
    // and does nothing on a proxy, and which adds the property to a frozen
    // object without a word: this reports a refusal as false, as
    // `DefineOwnProperty` does, and goes through a proxy's trap.
    v8::PropertyDescriptor descriptor(
        read.IsEmpty() ? v8::Undefined(Raw(owner)).As<v8::Value>() : read.As<v8::Value>(),
        write.IsEmpty() ? v8::Undefined(Raw(owner)).As<v8::Value>() : write.As<v8::Value>());
    descriptor.set_enumerable(!HasAttribute(attributes, PropertyAttribute::DontEnum));
    descriptor.set_configurable(!HasAttribute(attributes, PropertyAttribute::DontDelete));
    return FromV8(Resolve(object).As<v8::Object>()->DefineProperty(raw, key, descriptor));
}

namespace {

/// Mark `rec`, and everything instantiating it instantiates, as possibly
/// instantiated by V8 - the object template's function template, a function
/// template's parent and its two object templates, and every template set as
/// a property on any of them. A superset of what V8 publishes is the safe
/// side: a shape call it would have allowed is ignored, where one it would
/// not have is a fatal error.
void Seal(TemplateRec* rec) noexcept {
    if (rec == nullptr || rec->sealed) {
        return;
    }
    rec->sealed = true;
    Seal(rec->function);
    Seal(rec->parent);
    Seal(rec->prototype);
    Seal(rec->instance);
    for (TemplateRec* nested : rec->nested) {
        Seal(nested);
    }
}

}  // namespace

void TemplateSetTemplate(TemplateRec* tpl, std::string_view name, TemplateRec* value, PropertyAttribute attributes) {
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));
    tpl->nested.push_back(value);
    if (tpl->sealed) {
        // It will be instantiated with `tpl` in every realm from now on.
        Seal(value);
    }
    RawTemplate(tpl)->Set(RawString(owner, name), RawTemplate(value), RawAttributes(attributes));
}

void TemplateSetClassName(TemplateRec* tpl, std::string_view name) {
    if (tpl->sealed) {
        return;  // fixed at the first instantiation - see unibind/template.h
    }
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));
    RawFunctionTemplate(tpl)->SetClassName(RawString(owner, name));
}

void TemplateInherit(TemplateRec* child, TemplateRec* parent) {
    if (child->sealed) {
        return;  // fixed at the first instantiation - see unibind/template.h
    }
    Isolate& owner = *child->owner;
    v8::HandleScope scope(Raw(owner));
    child->parent = parent;
    RawFunctionTemplate(child)->Inherit(RawFunctionTemplate(parent));
}

TemplateRec* TemplatePrototype(TemplateRec* tpl) {
    if (tpl->prototype == nullptr) {
        Isolate& owner = *tpl->owner;
        v8::HandleScope scope(Raw(owner));
        tpl->prototype = AdoptTemplate(owner, TemplateRec::Kind::Object, RawFunctionTemplate(tpl)->PrototypeTemplate());
        tpl->prototype->function = tpl;
        tpl->prototype->sealed = tpl->sealed;
    }
    return tpl->prototype;
}

TemplateRec* TemplateInstance(TemplateRec* tpl) {
    if (tpl->instance == nullptr) {
        Isolate& owner = *tpl->owner;
        v8::HandleScope scope(Raw(owner));
        tpl->instance = AdoptTemplate(owner, TemplateRec::Kind::Object, RawFunctionTemplate(tpl)->InstanceTemplate());
        tpl->instance->function = tpl;
        tpl->instance->sealed = tpl->sealed;
    }
    return tpl->instance;
}

std::optional<Slot> TemplateNewInstance(const Context& context, TemplateRec* tpl) {
    Seal(tpl);
    v8::Local<v8::Object> instance;
    if (!RawObjectTemplate(tpl)->NewInstance(Raw(context)).ToLocal(&instance)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), instance);
}

std::optional<Slot> TemplateGetFunction(const Context& context, TemplateRec* tpl) {
    Seal(tpl);
    v8::Local<v8::Function> function;
    if (!RawFunctionTemplate(tpl)->GetFunction(Raw(context)).ToLocal(&function)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), function);
}

std::optional<bool> TemplateHasInstance(const Context& context, TemplateRec* tpl, Slot value) {
    (void)context;
    return RawFunctionTemplate(tpl)->HasInstance(Resolve(value));
}

// ---------------------------------------------------------------------------
// Interceptors
//
// A unibind hook answers with its C++ return value - Intercepted for a get or a
// set, an optional for a query, a delete or an enumeration - and V8 answers
// partly by return value and partly through a typed return slot. These
// trampolines are the translation, and they are the only place that knows
// which half is which; the hook itself only ever sees a PropertyCallbackInfo.
// ---------------------------------------------------------------------------

namespace {

/// The per-call state for a property hook. It has no argument list, so the
/// frame borrows nothing, and V8 15.6 tells it the holder and never the
/// receiver - which is why the two are the same object here. See the contract
/// at the top of unibind/function.h.
template <class Info>
[[nodiscard]] CallbackState PropertyState(Isolate& isolate, CallFrame& frame, const Info& info, CallbackData data,
                                          const ReturnSink& returns) {
    return CallbackState{.owner = &isolate,
                         .frame = &frame.frame(),
                         .realm = &frame.realm(),
                         .data = data,
                         .receiver = info.Holder(),
                         .holder = info.Holder(),
                         .call = nullptr,
                         .info = &info,
                         .returns = &returns};
}

/// What the hook said, unless it threw.
///
/// A hook that leaves an exception pending has intercepted the access
/// whatever it returned (unibind/template.h): declining says "carry on with the
/// ordinary lookup", and V8 must not re-enter that lookup with an exception
/// pending. Answering `kYes` stops it there and lets the throw propagate,
/// which is what the embedder asked for by throwing.
[[nodiscard]] v8::Intercepted RawIntercepted(Isolate& isolate, Intercepted answer) noexcept {
    if (Raw(isolate)->HasPendingException()) {
        return v8::Intercepted::kYes;
    }
    return answer == Intercepted::Yes ? v8::Intercepted::kYes : v8::Intercepted::kNo;
}

/// An enumerator has no way to decline on V8, so an empty answer is an empty
/// key list - which is the same observable result, and is what unibind/template.h
/// promises.
template <class Info>
void WriteKeys(Isolate& isolate, const Info& info, const std::optional<Local<Array>>& keys) {
    // An optional holding an EMPTY handle is not the same as an empty
    // optional, and it is what a hook hands back when its array could not be
    // made: resolving it would read through a null frame. Both answer "no own
    // keys", which is the only thing V8's enumerator can say anyway.
    if (!keys || keys->IsEmpty()) {
        info.GetReturnValue().Set(v8::Array::New(Raw(isolate), 0));
        return;
    }
    info.GetReturnValue().Set(Resolve(keys->slot()).As<v8::Array>());
}

v8::Intercepted NamedGetter(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<NamedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, PropertyValueReturn::SINK);
    const Slot name = Push(isolate, property);
    return RawIntercepted(isolate, handler->getter(Local<Name>::FromSlot(name), PropertyCallbackInfo(state)));
}

v8::Intercepted NamedSetter(v8::Local<v8::Name> property, v8::Local<v8::Value> value,
                            const v8::PropertyCallbackInfo<v8::Boolean>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<NamedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    const Slot name = Push(isolate, property);
    const Slot assigned = Push(isolate, value);
    return RawIntercepted(isolate, handler->setter(Local<Name>::FromSlot(name), Local<Value>::FromSlot(assigned),
                                                   PropertyCallbackInfo(state)));
}

v8::Intercepted NamedQuery(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Integer>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<NamedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    const Slot name = Push(isolate, property);
    std::optional<PropertyAttribute> attributes =
        handler->query(Local<Name>::FromSlot(name), PropertyCallbackInfo(state));
    if (!attributes) {
        return RawIntercepted(isolate, Intercepted::No);
    }
    info.GetReturnValue().Set(static_cast<int32_t>(static_cast<uint8_t>(*attributes)));
    return v8::Intercepted::kYes;
}

v8::Intercepted NamedDeleter(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Boolean>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<NamedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    const Slot name = Push(isolate, property);
    std::optional<bool> deleted = handler->deleter(Local<Name>::FromSlot(name), PropertyCallbackInfo(state));
    if (!deleted) {
        return RawIntercepted(isolate, Intercepted::No);
    }
    info.GetReturnValue().Set(*deleted);
    return v8::Intercepted::kYes;
}

void NamedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    auto* handler = PointerFrom<NamedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    WriteKeys(isolate, info, handler->enumerator(PropertyCallbackInfo(state)));
}

v8::Intercepted IndexedGetter(uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<IndexedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, PropertyValueReturn::SINK);
    return RawIntercepted(isolate, handler->getter(index, PropertyCallbackInfo(state)));
}

v8::Intercepted IndexedSetter(uint32_t index, v8::Local<v8::Value> value,
                              const v8::PropertyCallbackInfo<v8::Boolean>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<IndexedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    const Slot assigned = Push(isolate, value);
    return RawIntercepted(isolate,
                          handler->setter(index, Local<Value>::FromSlot(assigned), PropertyCallbackInfo(state)));
}

v8::Intercepted IndexedQuery(uint32_t index, const v8::PropertyCallbackInfo<v8::Integer>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<IndexedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    std::optional<PropertyAttribute> attributes = handler->query(index, PropertyCallbackInfo(state));
    if (!attributes) {
        return RawIntercepted(isolate, Intercepted::No);
    }
    info.GetReturnValue().Set(static_cast<int32_t>(static_cast<uint8_t>(*attributes)));
    return v8::Intercepted::kYes;
}

v8::Intercepted IndexedDeleter(uint32_t index, const v8::PropertyCallbackInfo<v8::Boolean>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return v8::Intercepted::kYes;
    }
    auto* handler = PointerFrom<IndexedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    std::optional<bool> deleted = handler->deleter(index, PropertyCallbackInfo(state));
    if (!deleted) {
        return RawIntercepted(isolate, Intercepted::No);
    }
    info.GetReturnValue().Set(*deleted);
    return v8::Intercepted::kYes;
}

void IndexedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    auto* handler = PointerFrom<IndexedPropertyHandler>(info.DataV2());
    CallFrame frame(isolate, nullptr);
    CallbackState state = PropertyState(isolate, frame, info, handler->data, DiscardReturn::SINK);
    WriteKeys(isolate, info, handler->enumerator(PropertyCallbackInfo(state)));
}

}  // namespace

void TemplateSetNamedHandler(TemplateRec* tpl, const NamedPropertyHandler& handler) {
    if (tpl->sealed) {
        return;  // fixed at the first instantiation - see unibind/template.h
    }
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));

    auto owned = std::make_unique<NamedPropertyHandler>(handler);
    NamedPropertyHandler* record = owned.get();
    owner.impl().namedHandlers.push_back(std::move(owned));

    // A hook the caller did not supply is left null, so V8 falls through to the
    // ordinary lookup rather than calling a trampoline with nothing to call.
    RawObjectTemplate(tpl)->SetHandler(v8::NamedPropertyHandlerConfiguration(
        handler.getter != nullptr ? &NamedGetter : nullptr, handler.setter != nullptr ? &NamedSetter : nullptr,
        handler.query != nullptr ? &NamedQuery : nullptr, handler.deleter != nullptr ? &NamedDeleter : nullptr,
        handler.enumerator != nullptr ? &NamedEnumerator : nullptr, Pointer(owner, record)));
}

void TemplateSetIndexedHandler(TemplateRec* tpl, const IndexedPropertyHandler& handler) {
    if (tpl->sealed) {
        return;  // fixed at the first instantiation - see unibind/template.h
    }
    Isolate& owner = *tpl->owner;
    v8::HandleScope scope(Raw(owner));

    auto owned = std::make_unique<IndexedPropertyHandler>(handler);
    IndexedPropertyHandler* record = owned.get();
    owner.impl().indexedHandlers.push_back(std::move(owned));

    RawObjectTemplate(tpl)->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        handler.getter != nullptr ? &IndexedGetter : nullptr, handler.setter != nullptr ? &IndexedSetter : nullptr,
        handler.query != nullptr ? &IndexedQuery : nullptr, handler.deleter != nullptr ? &IndexedDeleter : nullptr,
        handler.enumerator != nullptr ? &IndexedEnumerator : nullptr, Pointer(owner, record)));
}

// ---------------------------------------------------------------------------
// Classes and the natives their instances carry
// ---------------------------------------------------------------------------

namespace {

void DestroyBox(NativeBox* box) noexcept {
    if (box != nullptr && box->destroy != nullptr) {
        box->destroy(box);
    }
}

void DestroyNative(InstanceRecord& record) noexcept {
    DestroyBox(std::exchange(record.box, nullptr));
}

/// The second pass runs after the collection is over, which is where an
/// arbitrary embedder destructor is allowed to run; the first pass may not
/// touch the heap. V8's weak handle is already reset by the time either runs.
///
/// Both passes run on the isolate's own thread, which is **required** rather
/// than convenient: destroying a box drops a `shared_ptr` whose other holders
/// are the embedder's, so anywhere else would race them. V8 gives no choice
/// here; an engine that does must take the foreground one. See `NativeBox` in
/// unibind/detail/backend.h.
void FinalizeNativeLate(const v8::WeakCallbackInfo<InstanceRecord>& data) {
    InstanceRecord* record = data.GetParameter();
    Isolate& owner = *record->owner;
    const OutsideCallback outside(owner);
    DestroyNative(*record);
    owner.impl().liveNatives.erase(record);
}

void FinalizeNative(const v8::WeakCallbackInfo<InstanceRecord>& data) {
    // V8 insists the first pass resets the handle before asking for a second.
    data.GetParameter()->handle.Reset();
    data.SetSecondPassCallback(&FinalizeNativeLate);
}

/// File `box` on the isolate's list of survivors, under a record of its own,
/// before anything has been published. Null when either could not be
/// allocated, and then nothing has been filed.
///
/// These are the backend's own allocations for a hand-over, and they are made
/// before the engine is asked for anything, so that running out of memory in
/// them is a hand-over that never started. The engine's own allocations are a
/// different matter: see `ClassInstantiate`.
[[nodiscard]] InstanceRecord* FileNative(Isolate& isolate, NativeBox* box) noexcept {
    try {
        auto owned = std::make_unique<InstanceRecord>();
        owned->owner = &isolate;
        owned->box = box;
        InstanceRecord* record = owned.get();
        isolate.impl().liveNatives.emplace(record, std::move(owned));
        return record;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

/// Take a record `FileNative` made back off the list, leaving its box alone:
/// the hand-over did not happen, and the box is still the caller's.
void UnfileNative(Isolate& isolate, InstanceRecord* record) noexcept {
    record->box = nullptr;
    isolate.impl().liveNatives.erase(record);
}

/// Put a filed box on `instance` and give the record the weak root that says
/// when the instance has gone. Past this the engine owns the box: the instance
/// carries it, and the finalizer or `~Isolate` gives it back exactly once.
void PublishNative(Isolate& isolate, v8::Local<v8::Object> instance, InstanceRecord* record) noexcept {
    instance->SetAlignedPointerInInternalField(NATIVE_FIELD, record->box, v8::kEmbedderDataTypeTagDefault);
    record->handle.Reset(Raw(isolate), instance);
    record->handle.SetWeak(record, &FinalizeNative, v8::WeakCallbackType::kParameter);
}

/// Hand `box` to the engine, or hand nothing over at all.
///
/// All-or-nothing on purpose, and it is what lets the construct trampoline say
/// plainly who owns the box: after a true answer the engine does, and after a
/// false one nothing has been published, so the caller still does and destroys
/// it.
///
/// The internal field is checked rather than assumed. An object that cannot
/// carry a native must not be given one: writing past the field count is a
/// V8 `ApiCheck`, which aborts the process, and an instance whose native the
/// engine could not record would be one nothing ever gives back.
[[nodiscard]] bool AttachNative(Isolate& isolate, v8::Local<v8::Object> instance, NativeBox* box) noexcept {
    if (instance->InternalFieldCount() <= NATIVE_FIELD) {
        return false;
    }
    if (box == nullptr) {
        instance->SetAlignedPointerInInternalField(NATIVE_FIELD, nullptr, v8::kEmbedderDataTypeTagDefault);
        return true;
    }
    InstanceRecord* record = FileNative(isolate, box);
    if (record == nullptr) {
        return false;
    }
    PublishNative(isolate, instance, record);
    return true;
}

void ClassConstructTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Isolate& isolate = OwnerOf(info.GetIsolate());
    if (RefuseDeeperNative(isolate)) {
        return;
    }
    auto* rec = PointerFrom<ClassRec>(info.DataV2());

    const bool constructing = info.IsConstructCall();
    if (!constructing && !rec->callable) {
        ThrowError(isolate, ErrorKind::TypeError, "this constructor cannot be invoked without 'new'");
        return;
    }
    if (rec->constructor == nullptr) {
        ThrowError(isolate, ErrorKind::TypeError, "this class cannot be constructed from script");
        return;
    }

    // The instance first, and the native afterwards, so that the constructor's
    // receiver is the object being made either way. `new` has one already; a
    // plain call on a ConstructOrCall class makes the one `new` would have, or
    // the constructor would run against the call's receiver - the global
    // object, as often as not - and write there. See Class<T>::ConstructOrCall.
    v8::Local<v8::Object> self;
    if (constructing) {
        self = info.This();
    } else if (!RawObjectTemplate(rec->instance)->NewInstance(Raw(isolate)->GetCurrentContext()).ToLocal(&self)) {
        return;
    }
    // The field exists but nothing has written it, and reading an aligned
    // pointer out of an unwritten field is undefined - so it gets a value
    // before anything, including a constructor that unwraps its own `this`,
    // can ask for one.
    if (self->InternalFieldCount() > NATIVE_FIELD) {
        self->SetAlignedPointerInInternalField(NATIVE_FIELD, nullptr, v8::kEmbedderDataTypeTagDefault);
    }

    NativeBox* box = nullptr;
    {
        CallFrame frame(isolate, &info);
        CallbackState state = CallState(isolate, frame, info, {});
        state.receiver = self;
        state.holder = self;
        // A class constructor answers with its native, never through the
        // return slot. V8 makes an object left in a construct call's slot the
        // result of `new` - right for a `FunctionTemplate`, and wrong here,
        // where the instance carrying the `T` would be swapped for whatever
        // the callback wrote. So the slot goes nowhere, as on SpiderMonkey.
        state.returns = &DiscardReturn::SINK;
        box = rec->constructor(CallbackInfo(state));
    }
    if (box == nullptr) {
        // The callback threw, or declined. Declining without a word must
        // still fail the construction: returning here would hand script the
        // receiver V8 made, an instance of the class with no native behind it.
        if (!Raw(isolate)->HasPendingException()) {
            ThrowError(isolate, ErrorKind::Error, "constructor declined to make an instance");
        }
        return;
    }

    if (!AttachNative(isolate, self, box)) {
        DestroyBox(box);
        ThrowError(isolate, ErrorKind::Error, "this instance could not be given its native state");
        return;
    }
    if (!constructing) {
        info.GetReturnValue().Set(self);
    }
}

}  // namespace

NativeBox* GetNativeBox(Slot object) noexcept {
    v8::Local<v8::Value> raw = Resolve(object);
    if (raw.IsEmpty() || !raw->IsObject()) {
        return nullptr;
    }
    v8::Local<v8::Object> instance = raw.As<v8::Object>();
    if (instance->InternalFieldCount() <= NATIVE_FIELD) {
        return nullptr;
    }
    return static_cast<NativeBox*>(
        instance->GetAlignedPointerFromInternalField(NATIVE_FIELD, v8::kEmbedderDataTypeTagDefault));
}

ClassRec* NewClass(Isolate& isolate, std::string_view name, TypeId nativeType) {
    v8::HandleScope scope(Raw(isolate));

    auto owned = std::make_unique<ClassRec>();
    owned->owner = &isolate;
    owned->nativeType = nativeType;
    ClassRec* rec = owned.get();
    isolate.impl().classes.push_back(std::move(owned));

    v8::Local<v8::FunctionTemplate> tpl =
        v8::FunctionTemplate::New(Raw(isolate), &ClassConstructTrampoline, Pointer(isolate, rec));
    tpl->SetClassName(RawString(isolate, name));
    tpl->InstanceTemplate()->SetInternalFieldCount(NATIVE_FIELD_COUNT);

    rec->function = AdoptTemplate(isolate, TemplateRec::Kind::Function, tpl);
    rec->prototype = TemplatePrototype(rec->function);
    rec->instance = TemplateInstance(rec->function);
    return rec;
}

void ClassSetConstructor(ClassRec* rec, NativeConstructor constructor, bool callableWithoutNew) {
    rec->constructor = constructor;
    rec->callable = callableWithoutNew;
}

TemplateRec* ClassPrototypeTemplate(ClassRec* rec) {
    return rec->prototype;
}
TemplateRec* ClassConstructorTemplate(ClassRec* rec) {
    return rec->function;
}
TemplateRec* ClassInstanceTemplate(ClassRec* rec) {
    return rec->instance;
}

std::optional<Slot> ClassGetConstructor(const Context& context, ClassRec* rec) {
    return TemplateGetFunction(context, rec->function);
}

std::optional<Slot> ClassInstantiate(const Context& context, ClassRec* rec, NativeBox* native) noexcept {
    assert((native == nullptr || native->type == rec->nativeType) &&
           "a class was handed a native of a type it does not wrap");
    Isolate& owner = OwnerOf(context);
    Seal(rec->function);
    Frame* frame = owner.impl().current;
    assert(frame != nullptr && "a value was created with no HandleScope open");

    // Everything of the backend's own that the hand-over needs is allocated
    // first: room in the frame for the handle, and the record that files the
    // box on the isolate. A failure among them has handed nothing over, and
    // ownership transferred at the call, so the box is given back here.
    //
    // First, because the engine's allocations cannot fail the same way. V8 is
    // built without exceptions, so a `std::bad_alloc` out of an `operator new`
    // it called unwinds through frames that clean nothing up - an open handle
    // scope, a half-made instance, a root half-recorded - and there is no state
    // to go on from. Debug V8 says so (a handle scope level mismatch); release
    // V8 carries on with it. Nothing here catches such a throw, and
    // `noexcept` turns it into `std::terminate`: see docs/gotchas.md.
    if (!frame->Reserve(1)) {
        DestroyBox(native);
        return std::nullopt;
    }
    InstanceRecord* const record = native != nullptr ? FileNative(owner, native) : nullptr;
    if (native != nullptr && record == nullptr) {
        DestroyBox(native);
        return std::nullopt;
    }

    // Built from the instance template, so it gets the class's prototype and
    // its internal field without the script constructor running.
    v8::Local<v8::Object> instance;
    if (!RawObjectTemplate(rec->instance)->NewInstance(Raw(context)).ToLocal(&instance) ||
        instance->InternalFieldCount() <= NATIVE_FIELD) {
        if (record != nullptr) {
            UnfileNative(owner, record);
        }
        DestroyBox(native);
        return std::nullopt;
    }
    if (record != nullptr) {
        PublishNative(owner, instance, record);
    } else {
        instance->SetAlignedPointerInInternalField(NATIVE_FIELD, nullptr, v8::kEmbedderDataTypeTagDefault);
    }
    // The room was made above, so this cannot fail: a hand-over either happens
    // with a handle to show for it or does not happen at all.
    return PushOrNothing(owner, instance);
}

std::optional<bool> ClassHasInstance(const Context& context, ClassRec* rec, Slot value) {
    return TemplateHasInstance(context, rec->function, value);
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

GlobalNode* MakeGlobal(Isolate& isolate, Slot value) {
    auto* node = new GlobalNode();
    node->owner = &isolate;
    ++isolate.impl().embedderRefs;
    node->handle.Reset(Raw(isolate), Resolve(value));
    return node;
}

GlobalNode* DuplicateGlobal(GlobalNode* node) {
    if (node == nullptr) {
        return nullptr;
    }
    v8::Isolate* raw = Raw(*node->owner);
    // A scope of the backend's own: copying one root into another is not a
    // handle the caller asked for, and this has to work with nothing open -
    // `v8::Global::Get` makes a Local, and a Local needs somewhere to live.
    v8::HandleScope scope(raw);
    auto* copy = new GlobalNode();
    copy->owner = node->owner;
    ++copy->owner->impl().embedderRefs;
    copy->handle.Reset(raw, node->handle.Get(raw));
    return copy;
}

void ReleaseGlobal(GlobalNode* node) noexcept {
    if (node == nullptr) {
        return;
    }
    --node->owner->impl().embedderRefs;
    delete node;
}

Slot GlobalToSlot(Isolate& isolate, GlobalNode* node) noexcept {
    // A root that names nothing materialises as a handle that names nothing.
    // `undefined` would be worse here than anywhere else in the API, because
    // the handle is typed: a `Global<Function>` would answer with a
    // `Local<Function>` that is neither a function nor empty.
    if (node == nullptr || node->handle.IsEmpty()) {
        return Slot{};
    }
    return Push(isolate, node->handle.Get(Raw(isolate)));
}

namespace {

/// Both roots materialised into a scope of the backend's own, so a caller with
/// no HandleScope open still gets an answer. Two roots over one object is the
/// normal case, which is why nothing here compares nodes.
template <class Compare>
[[nodiscard]] bool CompareGlobals(const GlobalNode* lhs, const GlobalNode* rhs, Compare compare) noexcept {
    if (lhs == nullptr || rhs == nullptr || lhs->owner != rhs->owner) {
        return false;
    }
    v8::Isolate* raw = Raw(*lhs->owner);
    v8::HandleScope scope(raw);
    return compare(lhs->handle.Get(raw), rhs->handle.Get(raw));
}

template <class Compare>
[[nodiscard]] bool CompareGlobalToSlot(const GlobalNode* lhs, Slot rhs, Compare compare) noexcept {
    if (lhs == nullptr || rhs.IsEmpty()) {
        return false;
    }
    v8::Local<v8::Value> other = Resolve(rhs);
    v8::Isolate* raw = Raw(*lhs->owner);
    v8::HandleScope scope(raw);
    return compare(lhs->handle.Get(raw), other);
}

constexpr auto STRICT_EQUALS = [](v8::Local<v8::Value> a, v8::Local<v8::Value> b) { return a->StrictEquals(b); };
constexpr auto SAME_VALUE = [](v8::Local<v8::Value> a, v8::Local<v8::Value> b) { return a->SameValue(b); };

}  // namespace

bool GlobalStrictEquals(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return CompareGlobals(lhs, rhs, STRICT_EQUALS);
}
bool GlobalSameValue(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return CompareGlobals(lhs, rhs, SAME_VALUE);
}
bool GlobalStrictEqualsSlot(const GlobalNode* lhs, Slot rhs) noexcept {
    return CompareGlobalToSlot(lhs, rhs, STRICT_EQUALS);
}
bool GlobalSameValueSlot(const GlobalNode* lhs, Slot rhs) noexcept {
    return CompareGlobalToSlot(lhs, rhs, SAME_VALUE);
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

void ThrowValue(Isolate& isolate, Slot value) {
    Raw(isolate)->ThrowException(Resolve(value));
}

void ThrowError(Isolate& isolate, ErrorKind kind, std::string_view message) {
    // An error is an object and V8 makes it in the current realm; with none
    // entered it would dereference nothing. `unibind/isolate.h` lets an
    // embedder throw with no `ContextScope` open, so the backend's own realm
    // stands in.
    v8::Isolate* raw = Raw(isolate);
    std::optional<v8::Context::Scope> entered;
    if (!raw->InContext()) {
        auto& utility = isolate.impl().utility;
        if (utility.IsEmpty()) {
            v8::Local<v8::Context> made = v8::Context::New(raw);
            if (made.IsEmpty()) {
                return;
            }
            utility.Reset(raw, made);
        }
        entered.emplace(utility.Get(raw));
    }
    v8::Local<v8::String> text = RawString(isolate, message);
    v8::Local<v8::Value> error;
    switch (kind) {
        case ErrorKind::TypeError:
            error = v8::Exception::TypeError(text);
            break;
        case ErrorKind::RangeError:
            error = v8::Exception::RangeError(text);
            break;
        case ErrorKind::ReferenceError:
            error = v8::Exception::ReferenceError(text);
            break;
        case ErrorKind::SyntaxError:
            error = v8::Exception::SyntaxError(text);
            break;
        case ErrorKind::Error:
            error = v8::Exception::Error(text);
            break;
    }
    Raw(isolate)->ThrowException(error);
}

bool HasPendingException(Isolate& isolate) noexcept {
    return Raw(isolate)->HasPendingException();
}

namespace {

[[nodiscard]] std::string Utf8Of(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    if (value.IsEmpty()) {
        return {};
    }
    v8::String::Utf8Value text(isolate, value);
    if (*text == nullptr) {
        return {};
    }
    return {*text, static_cast<size_t>(text.length())};
}

[[nodiscard]] std::vector<StackFrame> FramesOf(Isolate& owner, v8::Local<v8::StackTrace> trace) {
    std::vector<StackFrame> frames;
    if (trace.IsEmpty()) {
        return frames;
    }
    v8::Isolate* raw = Raw(owner);
    const int count = trace->GetFrameCount();
    frames.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        v8::Local<v8::StackFrame> frame = trace->GetFrame(raw, static_cast<uint32_t>(index));
        frames.push_back(StackFrame{.functionName = Utf8Of(raw, frame->GetFunctionName()),
                                    .scriptName = Utf8Of(raw, frame->GetScriptName()),
                                    .lineNumber = frame->GetLineNumber(),
                                    .columnNumber = frame->GetColumn()});
    }
    return frames;
}

}  // namespace

std::vector<StackFrame> CaptureStack(Isolate& isolate, uint32_t limit) {
    v8::HandleScope scope(Raw(isolate));
    return FramesOf(
        isolate, v8::StackTrace::CurrentStackTrace(Raw(isolate), static_cast<int>(limit), v8::StackTrace::kDetailed));
}

void TryCatchOpen(Isolate& isolate, TryCatchState& storage) noexcept {
    ::new (static_cast<void*>(&storage)) TryCatchState(isolate);
}

void TryCatchClose(TryCatchState& state) noexcept {
    // A handler consumes what it caught (decision 3) - except a termination,
    // which is not an exception and must go on unwinding whether or not ReThrow
    // was called, and whether or not Reset was. Swallowing one would leave the
    // script it was told to stop running, which is the whole point of the
    // facility. Re-arming rather than re-throwing is what survives a Reset,
    // which has already taken V8's own pending termination away.
    Isolate* owner = state.owner;
    const bool stopping = owner->impl().terminating.load(std::memory_order_acquire);
    if (state.tryCatch.HasTerminated()) {
        state.tryCatch.ReThrow();
    }
    state.~TryCatchState();
    if (stopping) {
        Raw(*owner)->TerminateExecution();
    }
}

bool TryCatchHasCaught(const TryCatchState& state) noexcept {
    return state.tryCatch.HasCaught();
}

Slot TryCatchException(const TryCatchState& state, Isolate& isolate) noexcept {
    // Nothing caught, or a termination - which carries no value at all. Both
    // answer with an EMPTY handle, which is what unibind/exception.h promises
    // and the only answer that cannot be mistaken for someone having thrown
    // `undefined`.
    v8::Local<v8::Value> exception = state.tryCatch.Exception();
    if (exception.IsEmpty() || TryCatchHasTerminated(state)) {
        return Slot{};
    }
    return Push(isolate, exception);
}

std::optional<std::string> TryCatchMessage(const TryCatchState& state, const Context& context) {
    v8::Local<v8::Message> message = state.tryCatch.Message();
    if (message.IsEmpty()) {
        return std::nullopt;
    }
    v8::String::Utf8Value text(Raw(OwnerOf(context)), message->Get());
    if (*text == nullptr) {
        return std::nullopt;
    }
    return std::string(*text, static_cast<size_t>(text.length()));
}

std::optional<std::string> TryCatchStackTrace(const TryCatchState& state, const Context& context) {
    // Rendering a stack reads `error.stack`, which is a property like any
    // other and may be an accessor the script wrote. If it throws, the throw
    // lands in the innermost open handler - which is the one being asked - and
    // *replaces* the exception it caught: `HasCaught()` stays true and
    // `Exception()` and `Message()` describe the getter's error instead. A
    // handler of our own catches that where it happens, so the caller's keeps
    // what it caught and this answers empty.
    v8::Local<v8::Value> stack;
    {
        v8::TryCatch rendering(Raw(OwnerOf(context)));
        if (!state.tryCatch.StackTrace(Raw(context)).ToLocal(&stack)) {
            return std::nullopt;
        }
    }
    v8::String::Utf8Value text(Raw(OwnerOf(context)), stack);
    if (*text == nullptr) {
        return std::nullopt;
    }
    return std::string(*text, static_cast<size_t>(text.length()));
}

std::optional<std::vector<StackFrame>> TryCatchStackFrames(const TryCatchState& state, const Context& context) {
    Isolate& owner = OwnerOf(context);
    v8::HandleScope scope(Raw(owner));

    // The Error object's own trace, captured where the error was constructed,
    // which is the frame an embedder means by "where did this come from".
    //
    // Deliberately not the Message's trace as a fallback: V8 will capture one
    // for any throw if asked to, including `throw 1`, and a stack invented for a
    // value that never carried one is exactly the plausible wrong answer this
    // API exists to avoid. A thrown non-Error has no stack, on both engines,
    // and answers empty.
    v8::Local<v8::Value> exception = state.tryCatch.Exception();
    if (exception.IsEmpty()) {
        return std::nullopt;
    }
    v8::Local<v8::StackTrace> trace = v8::Exception::GetStackTrace(exception);
    if (trace.IsEmpty()) {
        return std::nullopt;
    }
    return FramesOf(owner, trace);
}

std::optional<MessageLocation> TryCatchLocation(const TryCatchState& state, const Context& context) {
    if (TryCatchHasTerminated(state)) {
        return std::nullopt;
    }
    Isolate& owner = OwnerOf(context);
    v8::HandleScope scope(Raw(owner));
    v8::Local<v8::Message> message = state.tryCatch.Message();
    if (message.IsEmpty()) {
        return std::nullopt;
    }
    v8::Local<v8::Context> raw = Raw(context);
    MessageLocation location;
    v8::Local<v8::Value> resource = message->GetScriptResourceName();
    if (!resource.IsEmpty() && resource->IsString()) {
        location.scriptName = Utf8Of(Raw(owner), resource);
    }
    location.lineNumber = message->GetLineNumber(raw).FromMaybe(0);
    // V8 counts columns from zero; `MessageLocation` does not.
    location.columnNumber = message->GetStartColumn(raw).FromMaybe(-1) + 1;
    v8::Local<v8::String> line;
    if (message->GetSourceLine(raw).ToLocal(&line)) {
        location.sourceLine = Utf8Of(Raw(owner), line);
    }
    return location;
}

void TryCatchReThrow(TryCatchState& state) noexcept {
    state.tryCatch.ReThrow();
}

void TryCatchReset(TryCatchState& state) noexcept {
    state.tryCatch.Reset();
    // Reset consumes V8's pending termination along with everything else, and a
    // stop is not the handler's to consume: put it back at once so the scope
    // cannot go on to run script it was told to stop.
    if (state.owner->impl().terminating.load(std::memory_order_acquire)) {
        Raw(*state.owner)->TerminateExecution();
    }
}

bool TryCatchHasTerminated(const TryCatchState& state) noexcept {
    return state.tryCatch.HasTerminated() || state.owner->impl().terminating.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Contexts
// ---------------------------------------------------------------------------

ContextRec* NewContext(Isolate& isolate) {
    // Ours first, before the engine is asked for anything: out of memory is
    // then the empty answer `Context::New` documents, where after the realm
    // was made it was a `std::bad_alloc` thrown out of it instead.
    std::unique_ptr<ContextRec> rec(new (std::nothrow) ContextRec());
    if (rec == nullptr) {
        return nullptr;
    }
    v8::HandleScope scope(Raw(isolate));
    v8::Local<v8::Context> context = v8::Context::New(Raw(isolate));
    if (context.IsEmpty()) {
        return nullptr;
    }
    // Read now, while nothing but the engine has touched this realm; see
    // `ContextRec::getPrototypeOf`.
    v8::Local<v8::Function> getPrototypeOf;
    v8::Local<v8::Function> setPrototypeOf;
    {
        v8::Isolate* raw = Raw(isolate);
        const v8::Context::Scope entered(context);
        v8::Local<v8::Value> objectConstructor;
        v8::Local<v8::Value> getter;
        v8::Local<v8::Value> setter;
        if (!context->Global()
                 ->Get(context, v8::String::NewFromUtf8Literal(raw, "Object"))
                 .ToLocal(&objectConstructor) ||
            !objectConstructor->IsObject() ||
            !objectConstructor.As<v8::Object>()
                 ->Get(context, v8::String::NewFromUtf8Literal(raw, "getPrototypeOf"))
                 .ToLocal(&getter) ||
            !objectConstructor.As<v8::Object>()
                 ->Get(context, v8::String::NewFromUtf8Literal(raw, "setPrototypeOf"))
                 .ToLocal(&setter) ||
            !getter->IsFunction() || !setter->IsFunction()) {
            return nullptr;
        }
        getPrototypeOf = getter.As<v8::Function>();
        setPrototypeOf = setter.As<v8::Function>();
    }
    rec->owner = &isolate;
    ++isolate.impl().embedderRefs;
    rec->handle.Reset(Raw(isolate), context);
    rec->getPrototypeOf.Reset(Raw(isolate), getPrototypeOf);
    rec->setPrototypeOf.Reset(Raw(isolate), setPrototypeOf);
    context->SetAlignedPointerInEmbedderData(CONTEXT_SLOT, rec.get(), v8::kEmbedderDataTypeTagDefault);
    return rec.release();
}

void RetainContext(ContextRec* rec) noexcept {
    if (rec != nullptr) {
        ++rec->refs;
    }
}

void ReleaseContext(ContextRec* rec) noexcept {
    if (rec != nullptr && --rec->refs == 0) {
        // A script still bound here would keep the realm alive, and would be
        // checked against a record that is about to be freed.
        while (rec->boundScripts != nullptr) {
            Unbind(rec->boundScripts);
        }
        --rec->owner->impl().embedderRefs;
        delete rec;
    }
}

Isolate& ContextIsolate(const Context& context) noexcept {
    return *RecOf(context)->owner;
}

Slot ContextGlobalObject(const Context& context) noexcept {
    return Push(OwnerOf(context), Raw(context)->Global());
}

void ContextEnter(const Context& context, ContextScopeState& storage) noexcept {
    // The realm the innermost callback was called in, before this changes it.
    // See `CallRealm`.
    CallRealm* call = OwnerOf(context).impl().innermostCall;
    if (call != nullptr && call->context.IsEmpty()) {
        call->context = Context::FromRec(CurrentContextRec(Raw(OwnerOf(context))));
    }
    auto* state = ::new (static_cast<void*>(&storage)) ContextScopeState();
    state->context = Raw(context);
    state->context->Enter();
}

void ContextLeave(ContextScopeState& state) noexcept {
    state.context->Exit();
    state.~ContextScopeState();
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

namespace {

/// The mark an eager compile's origin carries.
///
/// V8 answers a repeat compile of the same source and origin out of an
/// in-isolate cache, and answers it with whatever it compiled first: source
/// compiled lazily once and then asked for with `kEagerCompile` comes back
/// lazy, and a code cache made from it covers a top level and no functions.
/// Nothing reports that. The cache does tell compiles apart by their
/// host-defined options, which this library otherwise never sets, so an eager
/// compile carries a one-element array there and lands in an entry of its own.
/// Nothing reads the mark back: host-defined options only ever reach a module
/// loader's callback, and this library installs none.
[[nodiscard]] v8::Local<v8::PrimitiveArray> EagerMarker(Isolate& isolate) {
    v8::Isolate* raw = Raw(isolate);
    auto& kept = isolate.impl().eagerMarker;
    if (kept.IsEmpty()) {
        v8::Local<v8::PrimitiveArray> marker = v8::PrimitiveArray::New(raw, 1);
        marker->Set(raw, 0, v8::True(raw));
        kept.Reset(raw, marker);
    }
    return kept.Get(raw);
}

/// The one compile. `codeCache` empty means an ordinary compile; otherwise the
/// blob is offered and V8 says on the way out whether it took it.
[[nodiscard]] ScriptRec* CompileInto(const Context& context, std::string_view source, const ScriptOrigin& origin,
                                     std::span<const uint8_t> codeCache, CompileOptions options) {
    Isolate& owner = OwnerOf(context);
    // A compile may not start while a termination is unwinding: V8's compile
    // entry asserts it in a debug build, and a release one compiles anyway. A
    // stopped isolate runs nothing (decision 15), so nothing compiled now could
    // run; it fails as every other call made during a stop does, and the
    // caller's `TryCatch` says why.
    if (Raw(owner)->IsExecutionTerminating()) {
        return nullptr;
    }
    v8::Local<v8::String> text;
    if (!NewString(owner, source).ToLocal(&text)) {
        // Source is UTF-8 text, and bytes that are not do not compile. That is
        // a syntax error - what the other engine's tokenizer throws - and the
        // header promises one is pending; V8 never sees the source to say so.
        const size_t firstInvalid = FirstInvalidUtf8(source);
        if (firstInvalid != std::string_view::npos) {
            const EnterUnlessCurrent entered(context);
            const std::string message =
                "source is not UTF-8: byte " + std::to_string(firstInvalid) + " does not begin or continue a character";
            Raw(owner)->ThrowException(v8::Exception::SyntaxError(RawString(owner, message)));
        }
        return nullptr;
    }
    const bool eager = options == CompileOptions::EagerCompile;
    // A name ends at a NUL, as it must on the other engine, which takes it as a
    // C string (`ScriptOrigin` in `unibind/types.h`).
    const std::string_view resourceName = origin.resourceName.substr(0, origin.resourceName.find('\0'));
    v8::ScriptOrigin scriptOrigin(RawString(owner, resourceName), origin.lineOffset, origin.columnOffset, false, -1, {},
                                  false, false, false, eager ? EagerMarker(owner) : v8::Local<v8::PrimitiveArray>());

    // v8::ScriptCompiler::Source takes ownership of the CachedData object (not
    // of the buffer, which is the caller's), and `rejected` is only readable
    // while the Source is alive.
    auto* cached = codeCache.empty()
                       ? nullptr
                       : new v8::ScriptCompiler::CachedData(codeCache.data(), static_cast<int>(codeCache.size()),
                                                            v8::ScriptCompiler::CachedData::BufferNotOwned);
    // V8 will not consume a cache and compile eagerly in one call, and when it
    // refuses a blob it falls back to an ordinary - lazy - compile. So an eager
    // request asks first whether the blob would be refused, and does not offer
    // one that would: the embedder asking for eager with a stale blob is the
    // embedder about to make a fresh one, and a lazy fallback would make it
    // worthless. What the check cannot see is which source a blob belongs to;
    // `unibind/script.h` has already refused a blob for any other.
    if (eager && cached != nullptr &&
        cached->CompatibilityCheck(Raw(owner)) != v8::ScriptCompiler::CachedData::kSuccess) {
        delete cached;
        cached = nullptr;
    }
    v8::ScriptCompiler::Source compilerSource(text, scriptOrigin, cached);

    // Compiling still happens in a realm - that is where a syntax error is
    // reported from - but the result is not tied to it.
    const EnterUnlessCurrent entered(context);
    v8::Local<v8::UnboundScript> script;
    auto option = v8::ScriptCompiler::kNoCompileOptions;
    if (cached != nullptr) {
        option = v8::ScriptCompiler::kConsumeCodeCache;
    } else if (eager) {
        option = v8::ScriptCompiler::kEagerCompile;
    }
    if (!v8::ScriptCompiler::CompileUnboundScript(Raw(owner), &compilerSource, option).ToLocal(&script)) {
        return nullptr;
    }
    auto* rec = new ScriptRec();
    rec->owner = &owner;
    ++owner.impl().embedderRefs;
    rec->handle.Reset(Raw(owner), script);
    rec->usedCache = cached != nullptr && !cached->rejected;
    return rec;
}

}  // namespace

ScriptRec* CompileScript(const Context& context, std::string_view source, const ScriptOrigin& origin,
                         CompileOptions options) {
    return CompileInto(context, source, origin, {}, options);
}

ScriptRec* CompileScriptWithCache(const Context& context, std::string_view source, const ScriptOrigin& origin,
                                  std::span<const uint8_t> codeCache, CompileOptions options) {
    return CompileInto(context, source, origin, codeCache, options);
}

bool ScriptUsedCodeCache(const ScriptRec* script) noexcept {
    return script != nullptr && script->usedCache;
}

std::optional<std::vector<uint8_t>> ScriptCreateCodeCache(const ScriptRec* script) {
    if (script == nullptr) {
        return std::nullopt;
    }
    v8::Isolate* raw = Raw(*script->owner);
    v8::HandleScope scope(raw);
    std::unique_ptr<v8::ScriptCompiler::CachedData> data(v8::ScriptCompiler::CreateCodeCache(script->handle.Get(raw)));
    if (!data || data->data == nullptr || data->length <= 0) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(data->data, data->data + data->length);
}

std::string_view BackendBuildId() noexcept {
    // V8's own answer to "would this engine refuse a blob from that one": a tag
    // over the version and the enabled features, which v8-script.h documents as
    // the thing to store beside cached data and compare when using it. The
    // version string is in here too, so that a human looking at a rejected blob
    // can see which build wrote it; the tag is what actually decides.
    //
    // Built once. It is a pure function of this binary and of flags, which are
    // the process's and are fixed before any isolate exists.
    static const std::string ID = std::string("unibind-v8-") + v8::V8::GetVersion() + "-" +
                                  std::to_string(v8::ScriptCompiler::CachedDataVersionTag());
    return ID;
}

void ReleaseScript(ScriptRec* script) noexcept {
    if (script == nullptr) {
        return;
    }
    Unbind(script);
    --script->owner->impl().embedderRefs;
    delete script;
}

std::optional<Slot> RunScript(const Context& context, ScriptRec* script) {
    if (script == nullptr) {
        return std::nullopt;
    }
    Isolate& owner = OwnerOf(context);
    if (Stopped(owner)) {
        return std::nullopt;
    }
    // Bound to the realm being run in, not the one that compiled it, so the
    // code sees this realm's globals (unibind/script.h) - once per realm it
    // runs in, not once per run: see `ScriptRec::bound`.
    const EnterUnlessCurrent entered(context);
    ContextRec* realm = RecOf(context);
    v8::Local<v8::Script> bound;
    if (script->boundRealm == realm) {
        bound = script->bound.Get(Raw(owner));
    } else {
        Unbind(script);
        bound = script->handle.Get(Raw(owner))->BindToCurrentContext();
        BindTo(script, realm, bound);
    }
    v8::Local<v8::Value> result;
    if (!bound->Run(Raw(context)).ToLocal(&result)) {
        (void)Stopped(owner);
        return std::nullopt;
    }
    return PushOrNothing(owner, result);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Platform, Isolate, Context: the public classes the backend implements
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<v8::Platform> g_platform;  // NOLINT(*-avoid-non-const-global-variables)
bool g_initialized = false;                // NOLINT(*-avoid-non-const-global-variables)

/// The background-thread count in effect, when we know it. V8 takes any count
/// it is given, so an explicit request is the figure; with no request V8 sizes
/// its own pool and there is no way to ask it, which is what an empty answer
/// means.
std::optional<uint32_t> g_workerThreads;  // NOLINT(*-avoid-non-const-global-variables)

/// V8 running out of memory, for one isolate. Installed with the owning
/// `ub::Isolate` as its data, so the report names a heap without having to ask
/// the thread.
///
/// **V8 does not come back from this**, whatever the callback does: the engine
/// calls it and then ends the process, which is why the header tells an
/// embedder to write a handler that is correct if the next line never runs.
void OnIsolateOutOfMemory(const char* location, const v8::OOMDetails& details, void* data) {
    // A literal either way, so the distinction costs nothing to carry and an
    // embedder can tell "the JavaScript heap filled up" from "the process
    // could not get memory at all", which are different problems.
    const char* what = "the process is out of memory";
    if (details.detail != nullptr) {
        what = details.detail;
    } else if (details.is_heap_oom) {
        what = "the JavaScript heap is out of memory";
    }
    ReportEngineFault(EngineFault::OutOfMemory, static_cast<Isolate*>(data), location, what);
}

/// An API misuse V8 detected. Per-isolate.
///
/// **It ends the process, and that is the point of doing it here.** V8's own
/// behaviour with no handler installed is to print and abort; its behaviour
/// *with* one is to call it, mark the isolate as having had a fatal error, and
/// carry on - so a backend that simply reported and returned would have made
/// installing a handler change whether the program survives, which is the last
/// thing a diagnostic hook should do. Reported first, aborted second, and
/// `EngineFault::Fatal` therefore means one thing rather than two.
[[noreturn]] void OnIsolateFatalError(const char* location, const char* message) {
    ReportEngineFault(EngineFault::Fatal, g_threadIsolate, location, message);
    std::abort();
}

/// A failed internal check - a CHECK, or in a debug engine a DCHECK - anywhere
/// in the process. It names a file and a line rather than an isolate, so the
/// isolate - if there is one - comes from the thread, which one isolate per
/// thread makes an honest answer.
///
/// Aborts afterwards for the same reason as the hook above, and for one more:
/// whether V8 itself would abort after dispatching this is a detail of how the
/// hook is wired rather than something the header can promise, and a failed
/// internal check that *returns* carries on with an engine that has already
/// said it cannot.
[[noreturn]] void OnProcessFatalError(const char* file, int line, const char* message) {
    // Formatted into a stack buffer: this is the fatal path, and an allocation
    // here is the one thing the fault contract forbids.
    std::array<char, 256> where{};
    std::snprintf(where.data(), where.size(), "%s:%d", file == nullptr ? "<unknown>" : file, line);
    ReportEngineFault(EngineFault::Fatal, g_threadIsolate, where.data(), message);
    std::abort();
}

/// What V8 hands its near-heap-limit callback: one pointer, and we need the
/// isolate, the embedder's callback and the embedder's data.
size_t OnNearHeapLimit(void* data, size_t currentLimit, size_t initialLimit) {
    auto* isolate = static_cast<Isolate*>(data);
    if (isolate == nullptr || isolate->impl().heapLimitCallback == nullptr) {
        return currentLimit;
    }
    const OutsideCallback outside(*isolate);
    const size_t answer =
        isolate->impl().heapLimitCallback(*isolate, currentLimit, initialLimit, isolate->impl().heapLimitData);
    // Clamped rather than trusted: a handler that answers with something below
    // the current ceiling means "leave it", and lowering a heap's limit while
    // it is already at that limit is not a thing this API offers a way to ask
    // for by accident.
    return answer < currentLimit ? currentLimit : answer;
}

/// `wanted`, or as much of this thread's stack below `top` as can safely be
/// promised - what the SpiderMonkey backend clamps its quota to as well.
///
/// V8 takes whatever limit it is given, and a limit below the bottom of the
/// real stack is one it never reaches: runaway recursion walks off the end and
/// the process dies, which is the failure `stackLimitBytes` exists to turn into
/// an exception. The reserve is what the engine unwinds through after it has
/// decided it is out of stack - throwing the RangeError runs code, and that
/// code needs frames of its own.
std::size_t UsableStackBytes(uintptr_t top, std::size_t wanted) noexcept {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (top <= low || top > high) {
        return wanted;
    }
    constexpr std::size_t RESERVE = std::size_t{128} * 1024;
    const auto available = static_cast<std::size_t>(top - low);
    const std::size_t usable = available > RESERVE ? available - RESERVE : available / 2;
    return wanted < usable ? wanted : usable;
}

}  // namespace

Platform::Platform(const PlatformOptions& options) {
    assert(!g_initialized && "a Platform already exists");
    // First, and before anything that could fail: these are what a failure
    // during the rest of this constructor would be reported through, and V8's
    // process-level handler is documented as state to install before the engine
    // comes up.
    g_faultHandler = options.onEngineFault;
    g_faultData = options.engineFaultData;
    if (g_faultHandler != nullptr) {
        // Only when there is somewhere to send it. With no handler V8's own
        // behaviour - a message on stderr and an abort - is what an embedder
        // asked for by not asking for anything.
        v8::V8::SetFatalErrorHandler(&OnProcessFatalError);
        // A failed DCHECK is the same thing from the embedder's side - the
        // engine has said it cannot continue - and it arrives in the same
        // shape. Only a debug engine has any; a release one compiles them out
        // and never calls this.
        v8::V8::SetDcheckErrorHandler(&OnProcessFatalError);
    }
    // The embedder's flags go first: they are the process's command line and
    // ours are the ones the library needs regardless.
    if (!options.engineFlags.empty()) {
        const std::string flags(options.engineFlags);
        v8::V8::SetFlagsFromString(flags.c_str());
    }
    // --expose-gc is what makes Isolate::RequestGarbageCollection mean
    // anything; lifetime tests need it and it costs nothing otherwise.
    v8::V8::SetFlagsFromString("--expose-gc");
    // A code-cache blob is only checksummed on consume when this is on, and it
    // is off in a release build. unibind/script.h's framing already refuses a blob
    // that is not one, so this is the engine's own scale of the same check
    // rather than the whole of it - and it is a hash per consume, which is
    // nothing beside the compile it is avoiding.
    v8::V8::SetFlagsFromString("--verify-snapshot-checksum");

    int threads = 0;  // V8's "decide for yourself"
    if (options.workerThreads) {
        threads = static_cast<int>(*options.workerThreads);
        if (threads == 0) {
            // A pool size of zero on its own only stops the *platform* posting
            // work; this is what stops V8 wanting to.
            v8::V8::SetFlagsFromString("--single-threaded");
        }
        g_workerThreads = static_cast<uint32_t>(threads);
    }
    g_platform = v8::platform::NewDefaultPlatform(threads);
    if (!g_platform) {
        return;  // IsInitialized() stays false; see unibind/isolate.h
    }
    v8::V8::InitializePlatform(g_platform.get());
    // Both of these report failure, and a constructor has nowhere to put it, so
    // the answer is the one the header documents: leave IsInitialized() false
    // and let every Isolate::New refuse. Unwinding what did come up first, so
    // that a failed Platform is an object that did nothing rather than one that
    // did half of it.
    if (!v8::V8::Initialize()) {
        v8::V8::DisposePlatform();
        g_platform.reset();
        return;
    }
    g_initialized = true;
}

Platform::~Platform() {
    // A Platform whose bring-up failed already unwound itself, so there is
    // nothing here to take down and disposing anyway would be disposing what
    // was never initialised.
    if (!g_initialized) {
        g_workerThreads.reset();
        g_faultHandler = nullptr;
        g_faultData = CallbackData{};
        return;
    }
    // Both report failure, and the order matters more than the report: a
    // platform disposed under a V8 that would not go down is a pointer the
    // engine still holds into something that no longer exists. So a refused
    // `Dispose` leaves the platform up and owned - deliberately leaked, which
    // is the smaller of the two wrongs - and only says that this process has
    // no initialised engine any more.
    if (!v8::V8::Dispose()) {
        g_workerThreads.reset();
        g_initialized = false;
        (void)g_platform.release();
        // Left armed on purpose: the engine is still up and can still fault,
        // and this is the one path where a Platform that has been destroyed
        // has not taken V8 down with it.
        return;
    }
    v8::V8::DisposePlatform();
    g_platform.reset();
    // The thread count described *this* platform. Leaving it behind would have
    // WorkerThreads() report a figure for a platform that no longer exists,
    // where the header promises what is actually in effect.
    g_workerThreads.reset();
    g_initialized = false;
    // Last, because everything above could still have faulted. The handler
    // belongs to this Platform and the next one brings its own.
    g_faultHandler = nullptr;
    g_faultData = CallbackData{};
}

bool Platform::IsInitialized() noexcept {
    return g_initialized;
}

std::optional<uint32_t> Platform::WorkerThreads() noexcept {
    // Belt and braces with the reset in ~Platform: with no platform up there is
    // no count in effect, whatever was asked for last time.
    if (!g_initialized) {
        return std::nullopt;
    }
    return g_workerThreads;
}

std::string_view Platform::BackendName() noexcept {
    return "v8";
}

std::string_view Platform::BackendVersion() noexcept {
    // A literal in V8's own binary, so there is nothing to build or own.
    return v8::V8::GetVersion();
}

std::unique_ptr<Isolate> Isolate::New(const IsolateOptions& options) {
    if (!Platform::IsInitialized()) {
        return nullptr;
    }
    if (g_threadIsolate != nullptr) {
        return nullptr;  // one per thread; see unibind/isolate.h
    }
    // Nothrow, because the header promises an empty answer and never a crash
    // when a heap cannot be made - and running out of memory is one of the ways
    // it cannot. A `make_unique` here would throw `std::bad_alloc` out of a
    // function that says it returns empty instead, before V8 is even reached.
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
    if (!impl) {
        return nullptr;
    }
    impl->allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    if (!impl->allocator) {
        return nullptr;
    }

    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = impl->allocator.get();
    if (options.heapLimitBytes != 0) {
        params.constraints.ConfigureDefaultsFromHeapSize(0, options.heapLimitBytes);
    }
    impl->isolate = v8::Isolate::New(params);
    if (impl->isolate == nullptr) {
        return nullptr;
    }
    impl->isolate->Enter();
    // The engines disagree on when a job queue drains itself - V8's default
    // policy runs microtasks when a call returns, SpiderMonkey's never does -
    // and when a promise continuation runs is something a script can see. So
    // the pump is taken away from both and given to Isolate::PumpJobs.
    impl->isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
    // What makes v8::Exception::GetStackTrace answer at all: without it an
    // Error carries its `stack` string and no structured trace, and
    // TryCatch::StackFrames would have nothing to read. It attaches a trace to
    // Error objects, not to a thrown primitive, which is exactly the line
    // unibind/exception.h draws.
    impl->isolate->SetCaptureStackTraceForUncaughtExceptions(true, 64, v8::StackTrace::kDetailed);
    {
        // V8 wants the address of the lowest usable stack slot, and this thread
        // is the isolate's thread, so here is as good a datum as exists.
        const char here = 0;
        const auto top = reinterpret_cast<uintptr_t>(&here);
        // What was asked for, or else V8's own default - its `--stack-size`,
        // 984 KiB on every architecture - and either way no more of the
        // thread's stack than can be promised. A default measured from here can
        // run past the bottom of a 1 MiB thread stack by as much as the thread
        // had used before the isolate was made, and x64's frames are big enough
        // for runaway script recursion to get there. Native calls are held to
        // the same limit (see `RefuseDeeperNative`).
        constexpr std::size_t V8_DEFAULT_STACK = std::size_t{984} * 1024;
        const std::size_t wanted = options.stackLimitBytes != 0 ? options.stackLimitBytes : V8_DEFAULT_STACK;
        const std::size_t limit = UsableStackBytes(top, wanted);
        if (top > limit) {
            impl->stackGuard = top - limit;
            impl->isolate->SetStackLimit(top - limit);
        }
    }

    std::unique_ptr<Isolate> isolate(new (std::nothrow) Isolate(std::move(impl)));
    // NOLINTBEGIN(bugprone-use-after-move) - on failure the ctor never ran, so the move never happened
    if (!isolate) {
        // The constructor never ran, so `impl` still holds the v8::Isolate and
        // nothing owns it: ~Isolate is what disposes one, and there is no
        // Isolate. Take it down here or it stays entered on this thread.
        impl->isolate->Exit();
        impl->isolate->Dispose();
        return nullptr;
    }
    // NOLINTEND(bugprone-use-after-move)
    isolate->impl().self = isolate.get();
    isolate->impl().isolate->SetData(ISOLATE_SLOT, isolate.get());
    g_threadIsolate = isolate.get();
    // Here rather than earlier because both want the owning ub::Isolate, which
    // does not exist until the line above. The cost is that a failure inside
    // `v8::Isolate::New` itself reports no isolate - which is honest, because
    // at that point there is not one.
    if (g_faultHandler != nullptr) {
        isolate->impl().isolate->SetOOMErrorHandler(&OnIsolateOutOfMemory, isolate.get());
        isolate->impl().isolate->SetFatalErrorHandler(&OnIsolateFatalError);
    }
    return isolate;
}

Isolate::Isolate(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Isolate::~Isolate() {
    if (g_threadIsolate == this) {
        g_threadIsolate = nullptr;
    }
    // V8 does not promise to run a weak callback before an isolate goes away,
    // so any box whose finalizer never ran is destroyed here. That is what
    // makes "the engine gives back every share it took, exactly once, by the
    // time its isolate is gone" a rule rather than a hope, and it is the only
    // reason a finalizer parity test between two backends can be written. A
    // native the embedder still co-owns outlives this, on purpose.
    for (auto& entry : impl_->liveNatives) {
        detail::DestroyNative(*entry.second);
    }
    impl_->liveNatives.clear();

    // Everything the isolate owns that holds a v8::Global has to let go while
    // the isolate is still alive: resetting one after Dispose is undefined, and
    // impl_ is destroyed after this body runs.
    impl_->classes.clear();
    impl_->templates.clear();
    impl_->accessors.clear();
    impl_->valueDataTemplate.Reset();
    impl_->eagerMarker.Reset();
    impl_->utility.Reset();
    assert(impl_->inspector == nullptr && "an Inspector outlived its Isolate");
    // Callback records outlive nothing: the isolate is going, so anything that
    // could still reach them is going too.
    impl_->callbacks.clear();
#if UNIBIND_HANDLE_CHECKS
    // The rule in unibind/isolate.h, diagnosed where it is broken rather than
    // at the crash it causes later: a Context, a Script or a Global<T> the
    // *embedder* is still holding is memory this isolate can never give back,
    // and the release, when it comes, resets an engine handle against an
    // isolate that has been disposed.
    //
    // Here rather than at the top of this function, because everything above
    // legitimately gives some of these back: a native the isolate destroys may
    // itself own a realm and a root, and a sandbox does exactly that. What is
    // left at this line is what nothing but the embedder holds.
    assert(impl_->embedderRefs == 0 && "a Context, Script or Global outlived its Isolate");
#endif
    impl_->isolate->Exit();
    impl_->isolate->Dispose();
}

bool Isolate::HasPendingException() const noexcept {
    return impl_->isolate->HasPendingException();
}

void Isolate::ThrowError(ErrorKind kind, std::string_view message) {
    detail::ThrowErrorLossy(*this, kind, message);
}

HeapStatistics Isolate::GetHeapStatistics() const noexcept {
    v8::HeapStatistics stats;
    impl_->isolate->GetHeapStatistics(&stats);
    return HeapStatistics{.usedBytes = stats.used_heap_size(),
                          .totalBytes = stats.total_heap_size(),
                          .limitBytes = stats.heap_size_limit(),
                          .physicalBytes = stats.total_physical_size(),
                          .externalBytes = stats.external_memory(),
                          .mallocedBytes = stats.malloced_memory(),
                          .peakMallocedBytes = stats.peak_malloced_memory(),
                          .usedGlobalHandlesBytes = stats.used_global_handles_size(),
                          .totalGlobalHandlesBytes = stats.total_global_handles_size()};
}

void Isolate::RequestGarbageCollection() noexcept {
    impl_->isolate->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
}

void Isolate::SetHeapLimitCallback(HeapLimitCallback callback, CallbackData data) noexcept {
    impl_->heapLimitCallback = callback;
    impl_->heapLimitData = data;
    if (callback != nullptr) {
        if (!impl_->heapLimitArmed) {
            impl_->isolate->AddNearHeapLimitCallback(&OnNearHeapLimit, this);
            impl_->heapLimitArmed = true;
        }
        return;
    }
    if (impl_->heapLimitArmed) {
        // Zero, so that whatever ceiling a handler raised the heap to stays
        // where it is. V8 reads the second argument as "and put the limit back
        // to this", and putting it back below the live set is how removing a
        // callback turns into an out-of-memory that nobody asked for.
        impl_->isolate->RemoveNearHeapLimitCallback(&OnNearHeapLimit, 0);
        impl_->heapLimitArmed = false;
    }
}

void Isolate::TerminateExecution() noexcept {
    // The engine first and the flag second, the reverse of the cancel below,
    // and the order is what makes a stop and a cancel racing from two threads
    // end as one or the other. Flag first, a cancel landing between the two
    // cleared the flag and then the engine, and the stop armed the engine again
    // after it: an isolate that said it was not stopped, whose next script
    // failed anyway. This way round, whenever the engine is left armed the
    // flag is left set.
    impl_->isolate->TerminateExecution();
    impl_->terminating.store(true, std::memory_order_release);
}

bool Isolate::IsExecutionTerminating() const noexcept {
    return impl_->terminating.load(std::memory_order_acquire) || impl_->isolate->IsExecutionTerminating();
}

void Isolate::CancelTerminateExecution() noexcept {
    impl_->terminating.store(false, std::memory_order_release);
    impl_->isolate->CancelTerminateExecution();
}

namespace {

/// One request, one callback: V8 dispatches an interrupt per RequestInterrupt,
/// and its list is FIFO, so taking the front of ours keeps the two in step.
void InterruptTrampoline(v8::Isolate* raw, void* data) {
    auto* isolate = static_cast<Isolate*>(data);
    InterruptCallback callback = nullptr;
    CallbackData payload;
    {
        const std::scoped_lock guard(isolate->impl().work);
        if (isolate->impl().interrupts.empty()) {
            return;
        }
        std::tie(callback, payload) = isolate->impl().interrupts.front();
        isolate->impl().interrupts.pop_front();
    }
    // V8 has already opened a HandleScope and an external VM state for us, and
    // the contract in unibind/isolate.h forbids the callback running script - so
    // there is nothing to set up and nothing to catch.
    (void)raw;
    const OutsideCallback outside(*isolate);
    callback(*isolate, payload);
}

}  // namespace

bool Isolate::RequestInterrupt(InterruptCallback callback, CallbackData data) noexcept {
    if (callback == nullptr) {
        return false;
    }
    {
        const std::scoped_lock guard(impl_->work);
        // Growing the queue can run out of memory, and this is noexcept: an
        // exception here would be std::terminate, not a request refused.
        try {
            impl_->interrupts.emplace_back(callback, data);
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    impl_->isolate->RequestInterrupt(&InterruptTrampoline, this);
    return true;
}

bool Isolate::PostJob(JobCallback callback, CallbackData data) noexcept {
    if (callback == nullptr) {
        return false;
    }
    const std::scoped_lock guard(impl_->work);
    try {
        impl_->jobs.emplace_back(callback, data);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool Isolate::PostDelayedJob(JobCallback callback, CallbackData data, double delayInSeconds) noexcept {
    // `!(x > 0)` rather than `x <= 0`, so that a NaN is no delay too.
    if (!(delayInSeconds > 0)) {
        return PostJob(callback, data);
    }
    if (callback == nullptr) {
        return false;
    }
    // Past what the clock can count - a little under three hundred years from
    // now, in integer nanoseconds - the conversion would overflow and land in
    // the past, and a delay nothing will outlive would run at the next pump.
    // Half the room left is the cut-off, well clear of rounding, and beyond it
    // the job is due never.
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> delay(delayInSeconds);
    const std::chrono::duration<double> room(std::chrono::steady_clock::time_point::max() - now);
    const auto due = delay < room / 2 ? now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay)
                                      : std::chrono::steady_clock::time_point::max();
    const std::scoped_lock guard(impl_->work);
    try {
        impl_->delayedJobs.emplace(due, std::make_pair(callback, data));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void Isolate::PumpJobs() {
    // Engine jobs, then posted work, then round again - so a job that settles a
    // promise sees its continuations in the same pump. A termination stops the
    // whole thing without emptying either queue: cancel it and pump again.
    while (!IsExecutionTerminating()) {
        impl_->isolate->PerformMicrotaskCheckpoint();
        // A continuation may have been what was stopped, and then the posted
        // work behind it waits for the cancel like everything else.
        if (IsExecutionTerminating()) {
            return;
        }

        JobCallback callback = nullptr;
        CallbackData payload;
        {
            const std::scoped_lock guard(impl_->work);
            // Whatever has fallen due joins the queue first, in the order it
            // fell due, so it runs in this pump.
            const auto now = std::chrono::steady_clock::now();
            auto& delayed = impl_->delayedJobs;
            while (!delayed.empty() && delayed.begin()->first <= now) {
                impl_->jobs.push_back(delayed.begin()->second);
                delayed.erase(delayed.begin());
            }
            if (impl_->jobs.empty()) {
                return;
            }
            std::tie(callback, payload) = impl_->jobs.front();
            impl_->jobs.pop_front();
        }
        // A pump is not a call and has nowhere to put an exception, so anything
        // a job leaves pending stops here rather than surfacing in whatever the
        // script thread does next.
        TryCatch caught(*this);
        callback(*this, payload);
    }
}

void Isolate::StoreEmbedderData(CallbackData data) noexcept {
    impl_->embedder = data;
}

CallbackData Isolate::LoadEmbedderData() const noexcept {
    return impl_->embedder;
}

std::optional<Context> Context::New(Isolate& isolate) {
    detail::ContextRec* rec = detail::NewContext(isolate);
    if (rec == nullptr) {
        return std::nullopt;
    }
    return Context(rec);
}

namespace interop {

v8::Isolate* V8Isolate(Isolate& isolate) noexcept {
    return detail::Raw(isolate);
}

v8::Local<v8::Context> V8Context(const Context& context) noexcept {
    if (context.IsEmpty()) {
        return {};
    }
    return detail::Raw(context);
}

}  // namespace interop

// ---------------------------------------------------------------------------
// The inspector
//
// `v8_inspector` is this API's model, so this is adapters and nothing more: a
// `V8InspectorClient` that forwards to the embedder's `InspectorClient`, a
// `Channel` per session that does the same for protocol messages, and the
// conversion between the embedder's UTF-8 and the inspector's `StringView`,
// which is either 8-bit or UTF-16 and says which.
// ---------------------------------------------------------------------------

namespace {

/// Every realm is in one context group. The group is the inspector's unit of
/// "what one debugger sees", and this API has one debugger per isolate.
constexpr int CONTEXT_GROUP = 1;

/// What an 8-bit `StringView` holds depends on where it came from, and nothing
/// in it says so. A protocol message is JSON the inspector serialised, which
/// is UTF-8 - in practice ASCII, with anything else escaped. A name converted
/// from one of V8's own strings is Latin-1, one byte per UTF-16 unit below
/// 256.
enum class EightBit : bool { Utf8, Latin1 };

void AppendUtf8(std::string& out, std::uint32_t point) {
    if (point < 0x80) {
        out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (point >> 6)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
}

[[nodiscard]] std::string ToUtf8(const v8_inspector::StringView& view, EightBit eightBit) {
    std::string out;
    if (view.is8Bit()) {
        const auto* bytes = view.characters8();
        if (eightBit == EightBit::Utf8) {
            out.assign(reinterpret_cast<const char*>(bytes), view.length());
            return out;
        }
        out.reserve(view.length());
        for (size_t i = 0; i < view.length(); ++i) {
            AppendUtf8(out, bytes[i]);
        }
        return out;
    }
    // UTF-16, where a surrogate that is not half of a pair has no UTF-8 of its
    // own and becomes U+FFFD, as `Utf8Value` makes it.
    const auto* units = view.characters16();
    out.reserve(view.length());
    for (size_t i = 0; i < view.length(); ++i) {
        const std::uint32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < view.length() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            AppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            ++i;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            AppendUtf8(out, 0xFFFD);
        } else {
            AppendUtf8(out, unit);
        }
    }
    return out;
}

/// UTF-8 to the UTF-16 the inspector takes a name or a URL in. Bytes that are
/// not UTF-8 become U+FFFD, by the rule `String::NewFromUtf8` uses.
[[nodiscard]] std::vector<uint16_t> ToUtf16(std::string_view utf8) {
    std::vector<uint16_t> out;
    out.reserve(utf8.size());
    for (size_t at = 0; at < utf8.size();) {
        const detail::Utf8Step step = detail::NextUtf8(utf8, at);
        std::uint32_t point = 0xFFFD;
        if (step.valid) {
            static constexpr std::array<std::uint8_t, 5> LEAD_MASK{0, 0x7F, 0x1F, 0x0F, 0x07};
            point = static_cast<std::uint8_t>(utf8[at]) & LEAD_MASK[step.length];
            for (size_t k = 1; k < step.length; ++k) {
                point = (point << 6) | (static_cast<std::uint8_t>(utf8[at + k]) & 0x3FU);
            }
        }
        if (point >= 0x10000) {
            out.push_back(static_cast<uint16_t>(0xD800 + ((point - 0x10000) >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + ((point - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(point));
        }
        at += step.length;
    }
    return out;
}

[[nodiscard]] v8_inspector::StringView ViewOf(const std::vector<uint16_t>& units) noexcept {
    return {units.data(), units.size()};
}

}  // namespace

/// What `InspectorDispatcher::RequestDispatch` fills, shared between the
/// inspector and every thread holding its dispatcher. The mutex is what makes a
/// request safe against the inspector going: `~Inspector` takes it to clear
/// `owner`, so a request either finishes asking the isolate for a wake-up
/// before the inspector - and therefore the isolate - can go, or finds `owner`
/// null and declines.
struct InspectorDispatcher::Impl {
    std::mutex mutex;
    /// The isolate, while the inspector lives; null from the moment it starts
    /// going.
    Isolate* owner = nullptr;
    std::deque<std::pair<JobCallback, CallbackData>> queue;
    /// An interrupt and a job have been asked for since a drain last started.
    /// A request made while it is set needs no wake-up of its own: whichever of
    /// the two arrives first starts a drain after the request was queued, and
    /// a drain runs until the queue is empty. Cleared when a drain starts
    /// rather than when it ends, so a request made while callbacks are running
    /// - into a nested pause that pumps, say - asks for a wake-up of its own.
    bool wakePending = false;
};

namespace {

void SettleInspector(Isolate& isolate);

/// Run what `RequestDispatch` queued, in order, until nothing is left - which
/// includes anything requested while it runs. Called from a V8 interrupt and
/// from a posted job, whichever reaches the isolate first; the other finds the
/// queue empty.
void DrainDispatches(Isolate& isolate) {
    // Held for the whole drain: a callback may destroy the inspector, and the
    // isolate's reference with it.
    const std::shared_ptr<InspectorDispatcher> dispatcher = isolate.impl().inspectorDispatcher;
    if (dispatcher == nullptr) {
        return;
    }
    InspectorDispatcher::Impl& shared = dispatcher->impl();
    {
        const std::scoped_lock guard(shared.mutex);
        shared.wakePending = false;
    }
    // Whatever a notification put off is done here too; see
    // `Inspector::Impl::notifying`.
    SettleInspector(isolate);
    for (;;) {
        JobCallback callback = nullptr;
        CallbackData data;
        {
            const std::scoped_lock guard(shared.mutex);
            if (shared.owner == nullptr || shared.queue.empty()) {
                return;
            }
            std::tie(callback, data) = shared.queue.front();
            shared.queue.pop_front();
        }
        callback(isolate, data);
    }
}

void DispatchInterrupt(v8::Isolate* /*raw*/, void* data) {
    const OutsideCallback outside(*static_cast<Isolate*>(data));
    DrainDispatches(*static_cast<Isolate*>(data));
}

void DispatchJob(Isolate& isolate, CallbackData /*data*/) {
    DrainDispatches(isolate);
}

}  // namespace

/// The inspector's side of the embedder's client. V8 asks it for the default
/// realm, the time and a script's URL, and tells it about pauses.
struct Inspector::Impl final : v8_inspector::V8InspectorClient {
    Impl(Isolate& owner, InspectorClient& client);

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() override;

    /// What was put off inside a notification is done first, and if that ends
    /// the pause - a session going was what kept it - the embedder's loop is
    /// not started for a pause that is already over.
    void runMessageLoopOnPause(int /*contextGroupId*/) override {
        const OutsideCallback outside(*owner);
        settling = true;
        quitWhileSettling = false;
        Settle();
        settling = false;
        if (quitWhileSettling) {
            return;
        }
        client->RunMessageLoopOnPause();
    }
    void quitMessageLoopOnPause() override {
        const OutsideCallback outside(*owner);
        if (settling) {
            quitWhileSettling = true;
            return;
        }
        client->QuitMessageLoopOnPause();
    }
    double currentTimeMS() override {
        const OutsideCallback outside(*owner);
        return client->CurrentTimeMs();
    }

    /// The realm announced most recently and not yet withdrawn - or collected,
    /// since what is kept here does not keep a realm alive.
    v8::Local<v8::Context> ensureDefaultContextInGroup(int /*contextGroupId*/) override {
        v8::Isolate* raw = owner->impl().isolate;
        for (const auto& kept : std::views::reverse(contexts)) {
            if (!kept.IsEmpty()) {
                return kept.Get(raw);
            }
        }
        return {};
    }

    std::unique_ptr<v8_inspector::StringBuffer> resourceNameToUrl(
        const v8_inspector::StringView& resourceName) override {
        const OutsideCallback outside(*owner);
        auto url = client->ResourceNameToUrl(ToUtf8(resourceName, EightBit::Latin1));
        if (!url) {
            return nullptr;
        }
        return v8_inspector::StringBuffer::create(ViewOf(ToUtf16(*url)));
    }

    /// Stop and destroy the sessions that asked for it inside a notification,
    /// now that no notification is being sent. See `notifying`.
    void Settle();

    /// Keep the isolate coming back here soon even if the embedder does
    /// nothing else with the inspector: a wake-up drains, and a drain settles.
    void WakeToSettle() const noexcept;

    Isolate* owner;
    InspectorClient* client;
    std::shared_ptr<InspectorDispatcher> dispatcher;
    /// The realms announced and not withdrawn, oldest first; the last live one
    /// is where an evaluation naming no context runs. Weak: DevTools seeing a
    /// realm is no reason for it to live.
    std::vector<v8::Global<v8::Context>> contexts;
    /// How many notifications are being sent right now, across every session.
    ///
    /// A notification is sent from inside the agent that raised it - a console
    /// message from the runtime agent, a parsed script from the debugger's -
    /// and V8 goes on using that agent after the send returns. A session
    /// destroyed there frees the agent under it, and one stopped there tears
    /// the debugger agent's state down under it. So while this is above zero,
    /// `~InspectorSession` and `InspectorSession::Stop` detach at once - the
    /// client hears nothing more from a session that went - and leave the rest
    /// here for `Settle`, which runs at the next point nothing of V8's is
    /// mid-notification: the next call into the inspector, the next pause, or
    /// the wake-up they ask for. A response needs none of this: V8 sends it
    /// after the agent has returned, through a session it holds weakly.
    int notifying = 0;
    bool settling = false;
    bool quitWhileSettling = false;
    std::vector<InspectorSession::Impl*> stopping;
    std::vector<std::unique_ptr<InspectorSession::Impl>> doomed;
    // Last, so that it goes first: V8's inspector calls back into this object
    // while it is being torn down.
    std::unique_ptr<v8_inspector::V8Inspector> inspector;
};

/// One connection: V8's channel, forwarding every message to the embedder.
///
/// Destroying one inside a pause, or inside a response to its own dispatch, is
/// V8's design and not something to defer: V8 reaches a session and its channel
/// through weak pointers across exactly those calls, because a DevTools
/// connection that closes in the middle of a pause is ordinary. Inside a
/// notification it is not - see `Inspector::Impl::notifying`. The suite holds
/// it to both.
struct InspectorSession::Impl final : v8_inspector::V8Inspector::Channel {
    Impl(Inspector::Impl& inspector, InspectorClient& client)
        : owner(inspector.owner), inspector(&inspector), client(&client) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() override = default;

    void sendResponse(int /*callId*/, std::unique_ptr<v8_inspector::StringBuffer> message) override {
        if (client != nullptr) {
            const OutsideCallback outside(*owner);
            client->SendProtocolMessage(ToUtf8(message->string(), EightBit::Utf8));
        }
    }
    void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
        if (client == nullptr) {
            return;
        }
        // The count is the inspector's, which outlives this call whatever the
        // embedder does inside it; `this` may be put aside for `Settle` by then.
        Inspector::Impl& counted = *inspector;
        ++counted.notifying;
        const OutsideCallback outside(*owner);
        client->SendProtocolMessage(ToUtf8(message->string(), EightBit::Utf8));
        --counted.notifying;
    }
    // Nothing is buffered on this side, so there is nothing to flush.
    void flushProtocolNotifications() override {}

    Isolate* owner;
    Inspector::Impl* inspector;
    /// Null once the session has gone, while V8's half waits for `Settle`: the
    /// client is sent nothing more on its behalf.
    InspectorClient* client;
    /// `Stop` is final, and ours rather than V8's to promise: a stopped
    /// session's `Resume` and `Stop` do nothing, whatever the engine would
    /// make of being asked again.
    bool stopped = false;
    // Last, so that it goes before the channel it reports through.
    std::unique_ptr<v8_inspector::V8InspectorSession> session;
};

Inspector::Impl::Impl(Isolate& owner, InspectorClient& client) : owner(&owner), client(&client) {}

Inspector::Impl::~Impl() = default;

void Inspector::Impl::Settle() {
    if (notifying > 0) {
        return;
    }
    if (stopping.empty() && doomed.empty()) {
        return;
    }
    const v8::HandleScope scope(owner->impl().isolate);
    // One at a time, and off the list before V8 is asked: stopping or
    // destroying a session calls back into this object.
    while (!stopping.empty()) {
        InspectorSession::Impl* next = stopping.back();
        stopping.pop_back();
        next->session->stop();
    }
    while (!doomed.empty()) {
        const std::unique_ptr<InspectorSession::Impl> next = std::move(doomed.back());
        doomed.pop_back();
        next->session.reset();
    }
}

namespace {

void NothingToDispatch(Isolate& /*isolate*/, CallbackData /*data*/) {}

}  // namespace

void Inspector::Impl::WakeToSettle() const noexcept {
    // Refused only when out of memory, and then the next call into the
    // inspector settles instead.
    (void)dispatcher->RequestDispatch(&NothingToDispatch);
}

namespace {

void SettleInspector(Isolate& isolate) {
    if (isolate.impl().inspectorImpl != nullptr) {
        isolate.impl().inspectorImpl->Settle();
    }
}

}  // namespace

bool Inspector::Supported() noexcept {
    return true;
}

std::unique_ptr<Inspector> Inspector::New(Isolate& isolate, InspectorClient& client) {
    if (isolate.impl().inspector != nullptr) {
        return nullptr;
    }
    // Everything of ours is allocated before V8 is asked for anything, so that
    // running out of memory is a null here rather than an exception thrown
    // through the engine's frames.
    std::unique_ptr<Inspector> made;
    try {
        auto impl = std::make_unique<Impl>(isolate, client);
        impl->dispatcher.reset(new InspectorDispatcher(std::make_unique<InspectorDispatcher::Impl>()));
        made.reset(new Inspector(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    {
        const v8::HandleScope scope(isolate.impl().isolate);
        made->impl_->inspector = v8_inspector::V8Inspector::create(isolate.impl().isolate, made->impl_.get());
    }
    if (made->impl_->inspector == nullptr) {
        return nullptr;
    }
    isolate.impl().inspector = made.get();
    isolate.impl().inspectorImpl = made->impl_.get();
    isolate.impl().inspectorDispatcher = made->impl_->dispatcher;
    {
        InspectorDispatcher::Impl& shared = made->impl_->dispatcher->impl();
        const std::scoped_lock guard(shared.mutex);
        shared.owner = &isolate;
    }
    return made;
}

Inspector::Inspector(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Inspector::~Inspector() {
    Isolate& owner = *impl_->owner;
    {
        InspectorDispatcher::Impl& shared = impl_->dispatcher->impl();
        const std::scoped_lock guard(shared.mutex);
        shared.owner = nullptr;
        shared.queue.clear();
    }
    if (owner.impl().inspector == this) {
        owner.impl().inspector = nullptr;
        owner.impl().inspectorImpl = nullptr;
        owner.impl().inspectorDispatcher.reset();
    }
    const v8::HandleScope scope(owner.impl().isolate);
    // Whatever is still put aside goes before the engine's inspector it
    // belongs to: a session never outlives its inspector.
    impl_->notifying = 0;
    impl_->Settle();
    impl_->inspector.reset();
    impl_->contexts.clear();
}

void Inspector::ContextCreated(const Context& context, std::string_view name) {
    impl_->Settle();
    v8::Isolate* raw = impl_->owner->impl().isolate;
    const v8::HandleScope scope(raw);
    v8::Local<v8::Context> local = detail::Raw(context);
    // Collected entries go here rather than in a weak callback, which would
    // have to reach this vector from inside a collection. A realm announced
    // before is withdrawn first: V8 files each announcement as a context of its
    // own and `contextDestroyed` finds only the latest, so announcing twice
    // would leave DevTools an entry for the realm that nothing could take back.
    bool announced = false;
    std::erase_if(impl_->contexts, [raw, local, &announced](const v8::Global<v8::Context>& kept) {
        if (kept.IsEmpty()) {
            return true;
        }
        const bool same = kept.Get(raw) == local;
        announced = announced || same;
        return same;
    });
    if (announced) {
        impl_->inspector->contextDestroyed(local);
    }
    const std::vector<uint16_t> title = ToUtf16(name);
    const v8_inspector::V8ContextInfo info(local, CONTEXT_GROUP, ViewOf(title));
    impl_->inspector->contextCreated(info);
    impl_->contexts.emplace_back(raw, local).SetWeak();
    impl_->Settle();
}

void Inspector::ContextDestroyed(const Context& context) {
    impl_->Settle();
    v8::Isolate* raw = impl_->owner->impl().isolate;
    const v8::HandleScope scope(raw);
    v8::Local<v8::Context> local = detail::Raw(context);
    impl_->inspector->contextDestroyed(local);
    std::erase_if(impl_->contexts, [raw, local](const v8::Global<v8::Context>& kept) {
        return kept.IsEmpty() || kept.Get(raw) == local;
    });
    impl_->Settle();
}

std::unique_ptr<InspectorSession> Inspector::Connect() {
    impl_->Settle();
    // As in `New`: ours first, so that out of memory is a null, then V8's.
    std::unique_ptr<InspectorSession> made;
    try {
        made.reset(new InspectorSession(std::make_unique<InspectorSession::Impl>(*impl_, *impl_->client)));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    {
        const v8::HandleScope scope(impl_->owner->impl().isolate);
        made->impl_->session = impl_->inspector->connect(CONTEXT_GROUP, made->impl_.get(), v8_inspector::StringView(),
                                                         v8_inspector::V8Inspector::kFullyTrusted,
                                                         v8_inspector::V8Inspector::kNotWaitingForDebugger);
    }
    if (made->impl_->session == nullptr) {
        return nullptr;
    }
    return made;
}

std::shared_ptr<InspectorDispatcher> Inspector::Dispatcher() const noexcept {
    return impl_->dispatcher;
}

InspectorDispatcher::InspectorDispatcher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

InspectorDispatcher::~InspectorDispatcher() = default;

bool InspectorDispatcher::RequestDispatch(JobCallback callback, CallbackData data) noexcept {
    if (callback == nullptr) {
        return false;
    }
    Impl& shared = *impl_;
    const std::scoped_lock guard(shared.mutex);
    if (shared.owner == nullptr) {
        return false;
    }
    try {
        shared.queue.emplace_back(callback, data);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (!shared.wakePending) {
        // Both, and whichever arrives first drains: an interrupt reaches a
        // script that is running and never fires while the isolate is idle, and
        // a job is the other way round. Asked for under the lock, which is what
        // keeps the isolate alive until they have been. The job first, because
        // it is the one that can be refused, and a request with no job behind
        // it would wait for a script that may never run.
        Isolate& owner = *shared.owner;
        if (!owner.PostJob(&DispatchJob, {})) {
            shared.queue.pop_back();
            return false;
        }
        owner.impl().isolate->RequestInterrupt(&DispatchInterrupt, &owner);
        shared.wakePending = true;
    }
    return true;
}

InspectorSession::InspectorSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

InspectorSession::~InspectorSession() {
    Inspector::Impl& inspector = *impl_->inspector;
    std::erase(inspector.stopping, impl_.get());
    if (inspector.notifying > 0) {
        // Inside a notification: gone as far as the client can tell, and V8's
        // half put aside until nothing of V8's is mid-send. See `notifying`.
        impl_->client = nullptr;
        try {
            inspector.doomed.push_back(std::move(impl_));
            inspector.WakeToSettle();
            return;
        } catch (const std::bad_alloc&) {
            // Nowhere to keep it, so it goes now, as it always used to.
        }
    }
    const v8::HandleScope scope(impl_->owner->impl().isolate);
    impl_->session.reset();
}

void InspectorSession::DispatchProtocolMessage(std::string_view message) {
    Isolate& owner = *impl_->owner;
    Inspector::Impl& inspector = *impl_->inspector;
    inspector.Settle();
    const v8::HandleScope scope(owner.impl().isolate);
    // A stopped isolate runs no script until the stop is cancelled, and a
    // `Runtime.evaluate` is script like any other - but it goes straight to
    // V8, past every gate this backend keeps, and V8 forgot the stop the moment
    // its unwind finished. Arming V8's own termination again makes whatever
    // the dispatch runs stop at its first check, and the answer DevTools gets
    // says so; `CancelTerminateExecution` disarms it with the rest.
    if (owner.impl().terminating.load(std::memory_order_acquire)) {
        owner.impl().isolate->TerminateExecution();
    }
    // Nothing of `this` is touched once the dispatch has started: a callback
    // inside it may destroy the session. The inspector outlives the dispatch.
    impl_->session->dispatchProtocolMessage(
        v8_inspector::StringView(reinterpret_cast<const uint8_t*>(message.data()), message.size()));
    inspector.Settle();
}

void InspectorSession::Resume() {
    if (impl_->stopped) {
        return;
    }
    impl_->inspector->Settle();
    const v8::HandleScope scope(impl_->owner->impl().isolate);
    impl_->session->resume();
}

void InspectorSession::Stop() {
    if (impl_->stopped) {
        return;
    }
    impl_->stopped = true;
    Inspector::Impl& inspector = *impl_->inspector;
    if (inspector.notifying > 0) {
        // As for destruction, and for the same reason: the debugger agent a
        // stop tears down may be the one sending. Stopped as far as this API
        // is concerned now, and in V8 once the send is done.
        try {
            inspector.stopping.push_back(impl_.get());
            inspector.WakeToSettle();
            return;
        } catch (const std::bad_alloc&) {
            // Nowhere to keep it, so it happens now, as it always used to.
        }
    }
    const v8::HandleScope scope(impl_->owner->impl().isolate);
    impl_->session->stop();
}

}  // namespace ub
