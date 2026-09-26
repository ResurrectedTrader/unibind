// The spine of the CPython backend: the platform, the isolate, handle frames,
// roots, realms, scripts and exception handlers. See internal.h for the model
// and docs/python.md for the reasons.

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "internal.h"
#include "stdlib_frozen.h"

namespace ub {

namespace {

/// The process's one platform. CPython is initialised once and never again
/// (`Platform` is a scope around the whole program), so this is plain static
/// state rather than an object anyone could make twice.
struct PlatformState {
    bool initialized = false;
    bool constructed = false;
    /// The main interpreter's thread state, detached after start-up: the main
    /// interpreter owns process-wide machinery and never runs a script.
    PyThreadState* mainThread = nullptr;
    EngineFaultCallback onFault = nullptr;
    CallbackData faultData;
    /// Interpreters isolates had to leave behind, with a thread of a script's
    /// still inside a call that had not returned (`~Isolate`). Ended at
    /// `~Platform` if their threads have finished by then; CPython will not
    /// finalise while one is left.
    struct Abandoned {
        PyThreadState* tstate = nullptr;
        detail::RuntimeState* runtime = nullptr;
    };
    std::mutex abandonedMutex;
    std::vector<Abandoned> abandoned;
    std::atomic<bool> abandonedUnrecorded{false};
};

PlatformState gPlatform;

thread_local Isolate* tCurrentIsolate = nullptr;

/// The directory the running program is in, or empty if it cannot be told.
std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return std::filesystem::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

/// A directory holding the pure-Python standard library, when one is given:
/// `UNIBIND_PYTHON_HOME` (that directory or its `Lib`), then a `python-stdlib`
/// directory beside the program. The first that holds `os.py` wins. Either one
/// overrides the embedded standard library, so a developer can run against
/// sources on disk; without either, the embedded one is used.
///
/// A backend built without the embedded standard library
/// (UNIBIND_PYTHON_EMBED_STDLIB off) looks in one more place, last: where it
/// was when the backend was built.
std::wstring FindStandardLibraryDirectory() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto holds = [&ec](const fs::path& dir) { return !dir.empty() && fs::exists(dir / L"os.py", ec); };

    wchar_t* env = nullptr;
    std::size_t envLength = 0;
    if (_wdupenv_s(&env, &envLength, L"UNIBIND_PYTHON_HOME") == 0 && env != nullptr) {
        const fs::path dir(env);
        std::free(env);
        if (holds(dir)) {
            return dir.wstring();
        }
        if (holds(dir / L"Lib")) {
            return (dir / L"Lib").wstring();
        }
    }

    const fs::path exe = ExecutableDirectory();
    if (!exe.empty() && holds(exe / L"python-stdlib")) {
        return (exe / L"python-stdlib").wstring();
    }

#if defined(UNIBIND_PYTHON_DEFAULT_STDLIB)
    const fs::path baked(UNIBIND_PYTHON_DEFAULT_STDLIB);
    if (holds(baked)) {
        return baked.wstring();
    }
#endif
    return {};
}

void ReportPlatformFault(EngineFault fault, std::string_view message) noexcept {
    if (gPlatform.onFault == nullptr) {
        return;
    }
    EngineFaultReport report;
    report.fault = fault;
    report.isolate = tCurrentIsolate;
    report.message = message;
    gPlatform.onFault(report, gPlatform.faultData);
}

}  // namespace

// --- platform ------------------------------------------------------------------

Platform::Platform(const PlatformOptions& options) {
    assert(!gPlatform.constructed && "a second Platform while one exists, or after one was destroyed");
    gPlatform.constructed = true;
    gPlatform.onFault = options.onEngineFault;
    gPlatform.faultData = options.engineFaultData;

    if (!detail::PlatformRuntimeSetup() || !detail::RegisterModule()) {
        return;
    }

    // A directory on disk if one was given, and the embedded standard library
    // otherwise. The frozen-module table is installed only in the second case:
    // CPython consults it before `sys.path`, so with it in place a directory
    // would never be read.
    const std::wstring stdlib = FindStandardLibraryDirectory();
    if (stdlib.empty()) {
#if defined(UNIBIND_PYTHON_EMBED_STDLIB)
        PyImport_FrozenModules = detail::frozen_stdlib::kTable.modules;
#else
        ReportPlatformFault(EngineFault::Fatal, "the Python standard library could not be found");
        return;
#endif
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    // An embedded engine: no site-packages, no signal handlers (the host owns
    // Ctrl+C), no .pyc files written next to the standard library, and nothing
    // read from the environment or the command line.
    config.site_import = 0;
    config.user_site_directory = 0;
    config.install_signal_handlers = 0;
    config.write_bytecode = 0;
    config.parse_argv = 0;
    config.pathconfig_warnings = 0;
    config.module_search_paths_set = 1;

    // `home` is what `sys.prefix` says. From a directory it is the directory's
    // parent, as for an installed CPython, and `sys.path` is the directory. With
    // the standard library embedded it is the program's own directory, and
    // `sys.path` is empty: nothing is read from disk, and nothing on disk is
    // looked for.
    PyStatus status = PyStatus_Ok();
    if (!stdlib.empty()) {
        const std::filesystem::path lib(stdlib);
        status = PyConfig_SetString(&config, &config.home, lib.parent_path().c_str());
        if (PyStatus_Exception(status) == 0) {
            status = PyWideStringList_Append(&config.module_search_paths, stdlib.c_str());
        }
    } else if (const std::filesystem::path exe = ExecutableDirectory(); !exe.empty()) {
        status = PyConfig_SetString(&config, &config.home, exe.c_str());
    }
    if (PyStatus_Exception(status) == 0) {
        status = PyConfig_SetString(&config, &config.program_name, L"unibind");
    }
    if (PyStatus_Exception(status) == 0) {
        status = Py_InitializeFromConfig(&config);
    }
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status) != 0) {
        ReportPlatformFault(EngineFault::Fatal, status.err_msg != nullptr ? status.err_msg : "Py_InitializeFromConfig");
        return;
    }

    gPlatform.mainThread = PyEval_SaveThread();
    gPlatform.initialized = true;
}

Platform::~Platform() {
    if (!gPlatform.initialized) {
        return;
    }
    // Interpreters isolates left behind: ended now if their threads have, and
    // if one still has a thread inside a call that never returned, CPython
    // cannot be finalised at all - `Py_FinalizeEx` is a fatal error with a
    // sub-interpreter left - so it is left as it is, for the process to end.
    bool finalizable = !gPlatform.abandonedUnrecorded.load();
    {
        const std::lock_guard<std::mutex> lock(gPlatform.abandonedMutex);
        for (const PlatformState::Abandoned& left : gPlatform.abandoned) {
            if (!detail::EndAbandonedInterpreter(left.tstate, left.runtime)) {
                finalizable = false;
            }
        }
        gPlatform.abandoned.clear();
    }
    if (!finalizable) {
        gPlatform.initialized = false;
        return;
    }
    PyEval_RestoreThread(gPlatform.mainThread);
    Py_FinalizeEx();
    gPlatform.mainThread = nullptr;
    gPlatform.initialized = false;
}

bool Platform::IsInitialized() noexcept {
    return gPlatform.initialized;
}

std::string_view Platform::BackendName() noexcept {
    return "python";
}

std::string_view Platform::BackendVersion() noexcept {
    return PY_VERSION;
}

std::optional<std::uint32_t> Platform::WorkerThreads() noexcept {
    // CPython starts no threads of its own: collection, compilation and
    // everything else happen on the thread that asked.
    return 0U;
}

namespace detail {

EngineFaultCallback PlatformFaultHandler() noexcept {
    return gPlatform.onFault;
}
CallbackData PlatformFaultData() noexcept {
    return gPlatform.faultData;
}

Isolate* CurrentIsolate() noexcept {
    return tCurrentIsolate;
}

}  // namespace detail

// --- isolate -------------------------------------------------------------------

Isolate::Isolate(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::unique_ptr<Isolate> Isolate::New(const IsolateOptions& options) {
    if (!Platform::IsInitialized() || tCurrentIsolate != nullptr) {
        return nullptr;
    }

    // A GIL of its own, its own object allocator, and none of the things an
    // embedded engine must not do to its host process.
    PyInterpreterConfig config{};
    config.use_main_obmalloc = 0;
    config.allow_fork = 0;
    config.allow_exec = 0;
    config.allow_threads = 1;
    config.allow_daemon_threads = 0;
    config.check_multi_interp_extensions = 1;
    config.gil = PyInterpreterConfig_OWN_GIL;

    PyThreadState* tstate = nullptr;
    const PyStatus status = Py_NewInterpreterFromConfig(&tstate, &config);
    if (PyStatus_Exception(status) != 0 || tstate == nullptr) {
        return nullptr;
    }

    auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl());
    std::unique_ptr<Isolate> isolate;
    if (impl) {
        isolate.reset(new (std::nothrow) Isolate(std::move(impl)));
    }
    if (!isolate) {
        Py_EndInterpreter(tstate);
        return nullptr;
    }
    Impl& state = isolate->impl();
    state.self = isolate.get();
    state.tstate = tstate;
    state.interp = PyThreadState_GetInterpreter(tstate);
    tCurrentIsolate = isolate.get();

    if (!detail::InstallRealmWatcher(*isolate) || !detail::ImportModule(*isolate) ||
        !detail::IsolateRuntimeSetup(*isolate, options)) {
        PyErr_Clear();
        detail::IsolateRuntimeTeardown(*isolate);
        detail::ReleaseScriptThreadStop(*isolate);
        detail::ReleaseTypes(*isolate);
        detail::ForgetRealms(*isolate);
        Py_EndInterpreter(tstate);
        state.tstate = nullptr;
        tCurrentIsolate = nullptr;
        return nullptr;
    }
    return isolate;
}

Isolate::~Isolate() {
    Impl& state = *impl_;
    assert(tCurrentIsolate == this && "an isolate is destroyed on the thread that made it");

    if (state.tstate != nullptr) {
        PyErr_Clear();
        // Threads a script started go first, while everything they could
        // still reach is there: stopped, and waited for (runtime.cpp,
        // "Threads a script started").
        bool alone = detail::StopScriptThreads(*this, /*wait=*/true);
        detail::IsolateRuntimeTeardown(*this);
        detail::IsolateBindingsTeardown(*this);
        PyErr_Clear();

        // Give every native back while the interpreter is still alive: a
        // native's destructor may release a `Global` or a `Context` of its own,
        // and that has to happen against a live interpreter. Collect first, so
        // that garbage gives its natives back through the ordinary path.
        PyGC_Collect();
        PyErr_Clear();
        // One at a time, each out of the set before it is destroyed. A
        // native's destructor can run script - releasing its last reference
        // to a Python object runs that object's `__del__` - and that script can
        // reach another instance still waiting here, or the one going now:
        // `GetNativeBox` answers only for boxes still in the set while this
        // runs, so it never hands out one already destroyed, and a native made
        // meanwhile joins the set and goes too.
        state.nativesTearingDown = true;
        while (!state.liveNatives.empty()) {
            const auto next = state.liveNatives.begin();
            detail::NativeBox* box = *next;
            state.liveNatives.erase(next);
            box->destroy(box);
            PyErr_Clear();
        }
        state.nativesReleased = true;
        PyErr_Clear();

#if UNIBIND_HANDLE_CHECKS && !defined(NDEBUG)
        // docs/status.md decision 27: a Context, Script or Global the embedder
        // still holds is two faults at once. Diagnose it where it happens -
        // which is here, once every native has been given back: a native may
        // hold a realm and a root of its own, and giving them back is what
        // destroying it is for (unibind/isolate.h), not a violation.
        assert(state.embedderRefs == 0 && "a Context, Script or Global outlived its Isolate");
#endif

        detail::ReleaseTypes(*this);
        // A finalizer that ran just now may have started a thread too.
        alone = alone ? detail::StopScriptThreads(*this, /*wait=*/true) : detail::StopScriptThreads(*this, false);
        if (alone) {
            detail::ReleaseScriptThreadStop(*this);
            Py_EndInterpreter(state.tstate);
            // A realm whose dictionary the interpreter never freed.
            for (const auto& realm : state.realms) {
                delete realm.second;
            }
            state.realms.clear();
        } else {
            // A thread is still inside a call that has not returned - a lock
            // nobody will release, a read nothing will answer - and ending the
            // interpreter under it is a fatal error. So the interpreter is
            // left behind instead: nothing in it can reach this isolate any
            // more, the stop stays on its threads for when their calls
            // return, and this thread lets go of it and its GIL. `~Platform`
            // ends it, if its threads have ended by then.
            detail::ForgetRealms(*this);
            PlatformState::Abandoned left{.tstate = state.tstate, .runtime = detail::AbandonRuntime(*this)};
            (void)PyEval_SaveThread();
            try {
                const std::lock_guard<std::mutex> lock(gPlatform.abandonedMutex);
                gPlatform.abandoned.push_back(left);
            } catch (...) {
                // Not recorded: it can never be ended, and so nor can CPython.
                gPlatform.abandonedUnrecorded = true;
            }
        }
        state.tstate = nullptr;
    }
    tCurrentIsolate = nullptr;
}

// A member because the API is one; what it reads is the error indicator of the
// thread state attached to this thread, which is this isolate's from `New` to
// `~Isolate` (decision 11), so there is nothing on the object to consult.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool Isolate::HasPendingException() const noexcept {
    return PyErr_Occurred() != nullptr;
}

void Isolate::ThrowError(ErrorKind kind, std::string_view message) {
    detail::RaiseError(*this, kind, message);
}

void Isolate::StoreEmbedderData(CallbackData data) noexcept {
    impl_->embedder = data;
}

CallbackData Isolate::LoadEmbedderData() const noexcept {
    return impl_->embedder;
}

namespace detail {

// --- frames ------------------------------------------------------------------

Frame::~Frame() {
    for (std::uint32_t i = count; i > 0; --i) {
        const std::uint32_t own = i - 1;
        PyObject* value = own < UNIBIND_FRAME_INLINE_SLOTS ? inlineSlots[own] : spill[own - UNIBIND_FRAME_INLINE_SLOTS];
        Py_DECREF(value);
    }
    ::operator delete(static_cast<void*>(spill), std::nothrow);
#if UNIBIND_HANDLE_CHECKS
    // A closed frame's epoch is one no handle of it carries; see the
    // SpiderMonkey frame for why this store is volatile.
    *static_cast<volatile std::uint32_t*>(&epoch) = ~epoch;
#endif
}

SlotIndex Frame::Push(PyObject* value) noexcept {
    if (count < UNIBIND_FRAME_INLINE_SLOTS) {
        inlineSlots[count] = value;
    } else {
        const std::uint32_t spilled = count - UNIBIND_FRAME_INLINE_SLOTS;
        if (spilled == spillCapacity) {
            const std::uint32_t grown = spillCapacity == 0 ? 16 : spillCapacity * 2;
            auto* bigger = static_cast<PyObject**>(::operator new(sizeof(PyObject*) * grown, std::nothrow));
            if (bigger == nullptr) {
                Py_DECREF(value);
                return NO_SLOT;
            }
            if (spill != nullptr) {
                std::memcpy(static_cast<void*>(bigger), static_cast<const void*>(spill),
                            sizeof(PyObject*) * spillCapacity);
                ::operator delete(static_cast<void*>(spill), std::nothrow);
            }
            spill = bigger;
            spillCapacity = grown;
        }
        spill[spilled] = value;
    }
    ++count;
    return argCount + count - 1;
}

Slot Push(Isolate& isolate, PyObject* value) noexcept {
    if (value == nullptr) {
        return Slot{};
    }
    Frame* frame = isolate.impl().current;
    assert(frame != nullptr && "a value was created with no HandleScope open");
    if (frame == nullptr) {
        Py_DECREF(value);
        return Slot{};
    }
    const SlotIndex index = frame->Push(value);
    if (index == Frame::NO_SLOT) {
        return NoSlot();
    }
    return MakeSlot(*frame, index);
}

Frame& OpenFrame(Isolate& isolate, FrameStorage& storage, bool /*escapable*/) noexcept {
    Isolate::Impl& state = isolate.impl();
    auto* frame = ::new (static_cast<void*>(storage.bytes)) Frame(isolate, state.current, nullptr, 0);
    state.current = frame;
    return *frame;
}

void CloseFrame(Frame& frame) noexcept {
    Isolate::Impl& state = frame.owner->impl();
    assert(state.current == &frame && "HandleScopes must close in reverse order of opening");
    state.current = frame.parent;
    frame.~Frame();
}

Slot EscapeSlot(Frame& closing, Slot value) noexcept {
    Frame* parent = closing.parent;
    if (parent == nullptr || value.IsEmpty()) {
        return Slot{};
    }
    const SlotIndex index = parent->Push(Py_NewRef(Resolve(value)));
    if (index == Frame::NO_SLOT) {
        return NoSlot();
    }
    return MakeSlot(*parent, index);
}

Isolate& IsolateOf(Frame& frame) noexcept {
    return *frame.owner;
}

Frame* CurrentFrame(Isolate& isolate) noexcept {
    return isolate.impl().current;
}

// --- globals -------------------------------------------------------------------

// The reference is taken only once the node exists: a `new (std::nothrow)`
// that fails never evaluates its initialiser, so a reference taken there would
// not have been taken at all, and releasing it on failure would release one
// the value's other owners hold.
GlobalNode* MakeGlobal(Isolate& isolate, Slot value) {
    auto* node = new (std::nothrow) GlobalNode{&isolate, nullptr};
    if (node == nullptr) {
        return nullptr;
    }
    node->value = Py_NewRef(Resolve(value));
    ++isolate.impl().embedderRefs;
    return node;
}

GlobalNode* DuplicateGlobal(GlobalNode* node) {
    if (node == nullptr) {
        return nullptr;
    }
    auto* copy = new (std::nothrow) GlobalNode{node->owner, nullptr};
    if (copy == nullptr) {
        return nullptr;
    }
    copy->value = Py_NewRef(node->value);
    ++node->owner->impl().embedderRefs;
    return copy;
}

void ReleaseGlobal(GlobalNode* node) noexcept {
    if (node == nullptr) {
        return;
    }
    --node->owner->impl().embedderRefs;
    Py_DECREF(node->value);
    delete node;
}

Slot GlobalToSlot(Isolate& isolate, GlobalNode* node) noexcept {
    return PushBorrowed(isolate, node->value);
}

namespace {
[[nodiscard]] bool Comparable(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return lhs != nullptr && rhs != nullptr && lhs->owner == rhs->owner;
}
}  // namespace

bool GlobalStrictEquals(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return Comparable(lhs, rhs) && StrictEqualsObjects(*lhs->owner, lhs->value, rhs->value);
}

bool GlobalSameValue(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return Comparable(lhs, rhs) && SameValueObjects(*lhs->owner, lhs->value, rhs->value);
}

bool GlobalStrictEqualsSlot(const GlobalNode* lhs, Slot rhs) noexcept {
    return lhs != nullptr && !rhs.IsEmpty() && lhs->owner == &IsolateFor(rhs) &&
           StrictEqualsObjects(*lhs->owner, lhs->value, Resolve(rhs));
}

bool GlobalSameValueSlot(const GlobalNode* lhs, Slot rhs) noexcept {
    return lhs != nullptr && !rhs.IsEmpty() && lhs->owner == &IsolateFor(rhs) &&
           SameValueObjects(*lhs->owner, lhs->value, Resolve(rhs));
}

// --- realms --------------------------------------------------------------------

namespace {

/// Where an interpreter's dictionary (`PyInterpreterState_GetDict`) keeps the
/// isolate it belongs to, for code that runs on a thread with no isolate of
/// its own - a thread a script started - and has to find it.
constexpr const char* ISOLATE_KEY = "unibind.isolate";
constexpr const char* ISOLATE_CAPSULE = "unibind.isolate";

/// The isolate whose interpreter is running on this thread, or null.
[[nodiscard]] Isolate* IsolateOfThisInterpreter() noexcept {
    PyInterpreterState* interp = PyInterpreterState_Get();
    if (Isolate* here = CurrentIsolate(); here != nullptr && here->impl().interp == interp) {
        return here;
    }
    PyObject* dict = PyInterpreterState_GetDict(interp);                                      // borrowed
    PyObject* capsule = dict == nullptr ? nullptr : PyDict_GetItemString(dict, ISOLATE_KEY);  // borrowed
    if (capsule == nullptr || PyCapsule_IsValid(capsule, ISOLATE_CAPSULE) == 0) {
        return nullptr;
    }
    return static_cast<Isolate*>(PyCapsule_GetPointer(capsule, ISOLATE_CAPSULE));
}

/// The isolate's dictionary watcher. Every realm's globals are watched, and
/// the one event that matters is the last: the dictionary is going, and its
/// realm goes with it. That can only happen once the embedder holds no
/// `Context` for it, because while one does the record holds the dictionary.
///
/// Called for every change to a watched dictionary - every global a script
/// assigns - so everything but a deallocation is turned away first thing.
int RealmWatcher(PyDict_WatchEvent event, PyObject* dict, PyObject* /*key*/, PyObject* /*value*/) {
    if (event != PyDict_EVENT_DEALLOCATED) {
        return 0;
    }
    Isolate* isolate = IsolateOfThisInterpreter();
    if (isolate == nullptr) {
        return 0;
    }
    Isolate::Impl& state = isolate->impl();
    const auto found = state.realms.find(dict);
    if (found == state.realms.end()) {
        return 0;
    }
    ContextRec* rec = found->second;
    assert(rec->refs == 0 && "a realm's dictionary went while a Context still held it");
    state.realms.erase(found);
    if (state.entered == rec) {
        state.entered = nullptr;
    }
    // Nothing in the record is a reference: its dictionary was borrowed.
    delete rec;
    return 0;
}

}  // namespace

bool InstallRealmWatcher(Isolate& isolate) noexcept {
    Isolate::Impl& state = isolate.impl();
    PyObject* dict = PyInterpreterState_GetDict(state.interp);  // borrowed
    PyObject* capsule = PyCapsule_New(&isolate, ISOLATE_CAPSULE, nullptr);
    const bool stored = dict != nullptr && capsule != nullptr && PyDict_SetItemString(dict, ISOLATE_KEY, capsule) == 0;
    Py_XDECREF(capsule);
    if (!stored) {
        return false;
    }
    state.realmWatcher = PyDict_AddWatcher(&RealmWatcher);
    return state.realmWatcher >= 0;
}

void ForgetRealms(Isolate& isolate) noexcept {
    Isolate::Impl& state = isolate.impl();
    // Nothing may find its way back to this isolate from here on: not a
    // dictionary going, and not a thread asking the interpreter for it.
    if (state.realmWatcher >= 0) {
        if (PyDict_ClearWatcher(state.realmWatcher) != 0) {
            PyErr_Clear();
        }
        state.realmWatcher = -1;
    }
    if (PyObject* dict = PyInterpreterState_GetDict(state.interp); dict != nullptr) {
        if (PyDict_DelItemString(dict, ISOLATE_KEY) != 0) {
            PyErr_Clear();
        }
    }
    state.entered = nullptr;
    for (const auto& realm : state.realms) {
        delete realm.second;
    }
    state.realms.clear();
}

ContextRec* NewContext(Isolate& isolate) {
    Isolate::Impl& state = isolate.impl();
    if (state.realmWatcher < 0) {
        return nullptr;
    }
    PyObject* globals = PyDict_New();
    if (globals == nullptr) {
        PyErr_Clear();
        return nullptr;
    }
    auto* rec = new (std::nothrow) ContextRec{&isolate, globals, 1, nullptr};
    if (rec == nullptr) {
        Py_DECREF(globals);
        return nullptr;
    }
    PyObject* builtins = PyImport_ImportModule("builtins");
    PyObject* name = PyUnicode_FromString("__main__");
    const bool ok = builtins != nullptr && name != nullptr &&
                    PyDict_SetItemString(globals, "__builtins__", builtins) == 0 &&
                    PyDict_SetItemString(globals, "__name__", name) == 0;
    Py_XDECREF(builtins);
    Py_XDECREF(name);
    bool registered = false;
    if (ok) {
        try {
            registered = state.realms.emplace(globals, rec).second;
        } catch (const std::bad_alloc&) {
        }
    }
    // Watched only once it is in the map, so that the watcher never sees a
    // realm dictionary it cannot find.
    if (!registered || PyDict_Watch(state.realmWatcher, globals) != 0) {
        PyErr_Clear();
        if (registered) {
            state.realms.erase(globals);
        }
        Py_DECREF(globals);
        delete rec;
        return nullptr;
    }
    ++state.embedderRefs;
    return rec;
}

void RetainContext(ContextRec* rec) noexcept {
    if (rec == nullptr) {
        return;
    }
    if (rec->refs++ == 0) {
        Py_INCREF(rec->globals);
    }
    ++rec->owner->impl().embedderRefs;
}

void ReleaseContext(ContextRec* rec) noexcept {
    if (rec == nullptr) {
        return;
    }
    --rec->owner->impl().embedderRefs;
    if (--rec->refs == 0) {
        // Last statement on purpose: if nothing else holds the dictionary,
        // this frees it, and its capsule frees `rec`.
        Py_DECREF(rec->globals);
    }
}

Isolate& ContextIsolate(const Context& context) noexcept {
    return OwnerOf(context);
}

Slot ContextGlobalObject(const Context& context) noexcept {
    return PushBorrowed(OwnerOf(context), GlobalsOf(context));
}

void ContextEnter(const Context& context, ContextScopeState& storage) noexcept {
    Isolate& isolate = OwnerOf(context);
    auto* state =
        ::new (static_cast<void*>(&storage)) ContextScopeState{&isolate, isolate.impl().entered, context.rec()};
    RetainContext(state->rec);
    isolate.impl().entered = state->rec;
}

void ContextLeave(ContextScopeState& state) noexcept {
    Isolate::Impl& impl = state.owner->impl();
    impl.entered = state.previous;
    ContextRec* rec = state.rec;
    state.~ContextScopeState();
    ReleaseContext(rec);
}

ContextRec* RealmOfGlobals(Isolate& isolate, PyObject* globals) noexcept {
    const auto found = isolate.impl().realms.find(globals);
    return found == isolate.impl().realms.end() ? nullptr : found->second;
}

ContextRec* CurrentRealm(Isolate& isolate) noexcept {
    if (PyFrameObject* frame = PyEval_GetFrame(); frame != nullptr) {
        PyObject* globals = PyFrame_GetGlobals(frame);
        ContextRec* rec = RealmOfGlobals(isolate, globals);
        Py_XDECREF(globals);
        if (rec != nullptr) {
            return rec;
        }
    }
    return isolate.impl().entered;
}

// --- scripts -------------------------------------------------------------------

bool CompileSource(Isolate& isolate, std::string_view source, const ScriptOrigin& origin, PyObject** body,
                   PyObject** tail) noexcept {
    *body = nullptr;
    *tail = nullptr;
    PyObject* compile = PyDict_GetItemString(TypesOf(isolate).support, "compile_script");  // borrowed
    if (compile == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "unibind: support code missing");
        return false;
    }
    // A resource name ends at its first NUL (unibind/types.h).
    std::string_view resource = origin.resourceName;
    if (const std::size_t nul = resource.find('\0'); nul != std::string_view::npos) {
        resource = resource.substr(0, nul);
    }
    PyObject* text = PyUnicode_DecodeUTF8(source.data(), static_cast<Py_ssize_t>(source.size()), "replace");
    PyObject* name = TextString(resource);
    PyObject* line = PyLong_FromLong(origin.lineOffset);
    PyObject* result = nullptr;
    if (text != nullptr && name != nullptr && line != nullptr) {
        result = PyObject_CallFunctionObjArgs(compile, text, name, line, nullptr);
    }
    Py_XDECREF(text);
    Py_XDECREF(name);
    Py_XDECREF(line);
    if (result == nullptr) {
        return false;
    }
    if (!PyTuple_Check(result) || PyTuple_GET_SIZE(result) != 2) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_RuntimeError, "unibind: compile_script answered something other than a pair");
        return false;
    }
    *body = Py_NewRef(PyTuple_GET_ITEM(result, 0));
    PyObject* second = PyTuple_GET_ITEM(result, 1);
    *tail = second == Py_None ? nullptr : Py_NewRef(second);
    Py_DECREF(result);
    return true;
}

ScriptRec* NewScriptRec(Isolate& isolate, PyObject* body, PyObject* tail, std::string_view name) noexcept {
    ScriptRec* rec = nullptr;
    try {
        rec = new ScriptRec{&isolate, body, tail, false, std::string(name)};
    } catch (const std::bad_alloc&) {
        Py_XDECREF(body);
        Py_XDECREF(tail);
        return nullptr;
    }
    ++isolate.impl().embedderRefs;
    return rec;
}

ScriptRec* CompileScript(const Context& context, std::string_view source, const ScriptOrigin& origin,
                         CompileOptions /*options*/) {
    // CPython compiles a module in full - there is no lazy function body to
    // leave for later - so an eager compile is the only compile there is.
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return nullptr;
    }
    PyObject* body = nullptr;
    PyObject* tail = nullptr;
    if (!CompileSource(isolate, source, origin, &body, &tail)) {
        return nullptr;
    }
    return NewScriptRec(isolate, body, tail, origin.resourceName);
}

void ReleaseScript(ScriptRec* script) noexcept {
    if (script == nullptr) {
        return;
    }
    --script->owner->impl().embedderRefs;
    Py_XDECREF(script->body);
    Py_XDECREF(script->tail);
    delete script;
}

std::optional<Slot> RunScript(const Context& context, ScriptRec* script) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open() || script == nullptr) {
        return std::nullopt;
    }
    PyObject* globals = GlobalsOf(context);

    // A script that uses `await` at its top level compiles to coroutine code,
    // and evaluating it makes a coroutine rather than running anything. Such
    // a script evaluates to a promise of its completion value, which
    // `PumpJobs` settles - the shape a module with top-level await has in
    // JavaScript, and the only one that fits a synchronous `Run`.
    const auto isCoroutine = [](PyObject* code) {
        return code != nullptr && (reinterpret_cast<PyCodeObject*>(code)->co_flags & CO_COROUTINE) != 0;
    };
    if (isCoroutine(script->body) || isCoroutine(script->tail)) {
        PyObject* sequence = PyDict_GetItemString(TypesOf(isolate).support, "run_async_script");  // borrowed
        PyObject* coroutine = PyObject_CallFunctionObjArgs(
            sequence, script->body, script->tail != nullptr ? script->tail : Py_None, globals, nullptr);
        if (coroutine == nullptr) {
            return std::nullopt;
        }
        return PushOrNothing(isolate, SpawnCoroutine(isolate, coroutine));
    }

    PyObject* ran = PyEval_EvalCode(script->body, globals, globals);
    if (ran == nullptr) {
        return std::nullopt;
    }
    Py_DECREF(ran);
    if (script->tail == nullptr) {
        return PushBorrowed(isolate, Py_None);
    }
    return PushOrNothing(isolate, PyEval_EvalCode(script->tail, globals, globals));
}

// --- exceptions ------------------------------------------------------------------

void ThrowValue(Isolate& isolate, Slot value) {
    RaiseValue(isolate, Resolve(value));
}

void ThrowError(Isolate& isolate, ErrorKind kind, std::string_view message) {
    RaiseError(isolate, kind, message);
}

bool HasPendingException(Isolate& /*isolate*/) noexcept {
    return PyErr_Occurred() != nullptr;
}

namespace {

/// Whether a frame belongs to the backend's own support code, which is an
/// implementation detail and not a frame of the embedder's script.
[[nodiscard]] bool IsSupportFrame(PyCodeObject* code) noexcept {
    const char* file = PyUnicode_AsUTF8(code->co_filename);
    if (file == nullptr) {
        PyErr_Clear();
        return false;
    }
    return std::strcmp(file, "<unibind>") == 0;
}

[[nodiscard]] StackFrame DescribeFrame(PyFrameObject* frame) {
    StackFrame out;
    PyCodeObject* code = PyFrame_GetCode(frame);
    const std::string name = Utf8Of(code->co_name);
    out.functionName = name == "<module>" ? std::string() : name;
    out.scriptName = Utf8Of(code->co_filename);
    out.lineNumber = PyFrame_GetLineNumber(frame);
    int line = 0;
    int column = 0;
    int endLine = 0;
    int endColumn = 0;
    if (PyCode_Addr2Location(code, PyFrame_GetLasti(frame), &line, &column, &endLine, &endColumn) != 0 && column >= 0) {
        out.columnNumber = column + 1;
    }
    Py_DECREF(code);
    return out;
}

}  // namespace

std::vector<StackFrame> CaptureStack(Isolate& /*isolate*/, std::uint32_t limit) {
    std::vector<StackFrame> frames;
    PyFrameObject* frame = PyEval_GetFrame();
    Py_XINCREF(frame);
    while (frame != nullptr && frames.size() < limit) {
        PyCodeObject* code = PyFrame_GetCode(frame);
        const bool support = IsSupportFrame(code);
        Py_DECREF(code);
        if (!support) {
            frames.push_back(DescribeFrame(frame));
        }
        PyFrameObject* back = PyFrame_GetBack(frame);
        Py_DECREF(frame);
        frame = back;
    }
    Py_XDECREF(frame);
    return frames;
}

void CatchPendingException(Isolate& isolate) noexcept {
    if (PyErr_Occurred() == nullptr) {
        return;
    }
    Isolate::Impl& state = isolate.impl();
    TryCatchState* handler = state.tryCatch;
    if (handler != nullptr && handler->nativeDepth == state.nativeDepth) {
        PyObject* exception = PyErr_GetRaisedException();
        Py_XSETREF(handler->exception, exception);
        handler->caught = true;
        handler->terminated =
            Terminating(isolate) ||
            (exception != nullptr && PyErr_GivenExceptionMatches(exception, state.types.terminated) != 0);
        return;
    }
    if (state.nativeDepth > 0) {
        // A native call is running and has no handler of its own at its depth:
        // the exception belongs to the Python code that called it, and goes
        // back there when the native returns.
        return;
    }
    // Nothing will ever take it. V8 reports an uncaught exception and lets it
    // go; so does this.
    PyErr_Clear();
}

void TryCatchOpen(Isolate& isolate, TryCatchState& storage) noexcept {
    Isolate::Impl& state = isolate.impl();
    auto* handler = ::new (static_cast<void*>(&storage)) TryCatchState{};
    handler->owner = &isolate;
    handler->prev = state.tryCatch;
    handler->nativeDepth = state.nativeDepth;
    if (PyErr_Occurred() != nullptr) {
        handler->outer = PyErr_GetRaisedException();
    }
    state.tryCatch = handler;
}

void TryCatchClose(TryCatchState& state) noexcept {
    Isolate& isolate = *state.owner;
    Isolate::Impl& impl = isolate.impl();
    // An exception raised since the last question is ours too.
    CatchPendingException(isolate);
    impl.tryCatch = state.prev;

    PyObject* exception = std::exchange(state.exception, nullptr);
    PyObject* outer = std::exchange(state.outer, nullptr);
    if (exception != nullptr && (state.terminated || state.rethrow)) {
        // A termination is never consumed, and a rethrow is asked for.
        PyErr_SetRaisedException(exception);
    } else {
        Py_XDECREF(exception);
    }
    if (outer != nullptr) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetRaisedException(outer);
        } else {
            Py_DECREF(outer);
        }
    }
    state.~TryCatchState();
    // What is pending now belongs to whoever is further out.
    CatchPendingException(isolate);
}

bool TryCatchHasCaught(const TryCatchState& state) noexcept {
    CatchPendingException(*state.owner);
    return state.caught;
}

bool TryCatchHasTerminated(const TryCatchState& state) noexcept {
    CatchPendingException(*state.owner);
    return state.caught && state.terminated;
}

Slot TryCatchException(const TryCatchState& state, Isolate& isolate) noexcept {
    CatchPendingException(isolate);
    if (!state.caught || state.terminated || state.exception == nullptr) {
        return Slot{};
    }
    return Push(isolate, UnwrapThrown(isolate, state.exception));
}

namespace {

/// Call a helper from the support code with the caught exception. New
/// reference, or null - with nothing left pending - if it failed.
[[nodiscard]] PyObject* CallSupport(Isolate& isolate, const char* helper, PyObject* exception) noexcept {
    PyObject* function = PyDict_GetItemString(TypesOf(isolate).support, helper);  // borrowed
    if (function == nullptr) {
        return nullptr;
    }
    // Whatever was pending is not ours to disturb; the helper runs clean.
    PyObject* saved = PyErr_GetRaisedException();
    PyObject* result = PyObject_CallOneArg(function, exception);
    if (result == nullptr) {
        PyErr_Clear();
    }
    if (saved != nullptr) {
        PyErr_SetRaisedException(saved);
    }
    return result;
}

}  // namespace

std::optional<std::string> TryCatchMessage(const TryCatchState& state, const Context& context) {
    CatchPendingException(OwnerOf(context));
    if (!state.caught || state.terminated || state.exception == nullptr) {
        return std::nullopt;
    }
    PyObject* text = CallSupport(OwnerOf(context), "exception_message", state.exception);
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string message = PyUnicode_Check(text) ? Utf8Of(text) : std::string();
    Py_DECREF(text);
    return message;
}

std::optional<std::string> TryCatchStackTrace(const TryCatchState& state, const Context& context) {
    CatchPendingException(OwnerOf(context));
    if (!state.caught || state.terminated || state.exception == nullptr) {
        return std::nullopt;
    }
    PyObject* text = CallSupport(OwnerOf(context), "exception_stack", state.exception);
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string stack = PyUnicode_Check(text) ? Utf8Of(text) : std::string();
    Py_DECREF(text);
    return stack;
}

std::optional<std::vector<StackFrame>> TryCatchStackFrames(const TryCatchState& state, const Context& context) {
    CatchPendingException(OwnerOf(context));
    if (!state.caught || state.terminated || state.exception == nullptr) {
        return std::nullopt;
    }
    // Innermost first: a traceback runs from the outermost frame to the one
    // that raised, so collect it and turn it round.
    std::vector<StackFrame> frames;
    PyObject* traceback = PyException_GetTraceback(state.exception);
    for (PyObject* entry = traceback; entry != nullptr && entry != Py_None;) {
        auto* tb = reinterpret_cast<PyTracebackObject*>(entry);
        PyCodeObject* code = PyFrame_GetCode(tb->tb_frame);
        if (!IsSupportFrame(code)) {
            StackFrame frame;
            const std::string name = Utf8Of(code->co_name);
            frame.functionName = name == "<module>" ? std::string() : name;
            frame.scriptName = Utf8Of(code->co_filename);
            frame.lineNumber = tb->tb_lineno;
            int line = 0;
            int column = 0;
            int endLine = 0;
            int endColumn = 0;
            if (PyCode_Addr2Location(code, tb->tb_lasti, &line, &column, &endLine, &endColumn) != 0 && column >= 0) {
                frame.columnNumber = column + 1;
            }
            frames.push_back(std::move(frame));
        }
        Py_DECREF(code);
        entry = reinterpret_cast<PyObject*>(tb->tb_next);
    }
    Py_XDECREF(traceback);
    return std::vector<StackFrame>(frames.rbegin(), frames.rend());
}

std::optional<MessageLocation> TryCatchLocation(const TryCatchState& state, const Context& context) {
    Isolate& isolate = OwnerOf(context);
    CatchPendingException(isolate);
    if (!state.caught || state.terminated || state.exception == nullptr) {
        return std::nullopt;
    }
    // (script name, line, column, source line or None)
    PyObject* where = CallSupport(isolate, "exception_location", state.exception);
    if (where == nullptr || !PyTuple_Check(where) || PyTuple_GET_SIZE(where) != 4) {
        Py_XDECREF(where);
        return std::nullopt;
    }
    MessageLocation location;
    location.scriptName = Utf8Of(PyTuple_GET_ITEM(where, 0));
    location.lineNumber = static_cast<std::int32_t>(PyLong_AsLong(PyTuple_GET_ITEM(where, 1)));
    location.columnNumber = static_cast<std::int32_t>(PyLong_AsLong(PyTuple_GET_ITEM(where, 2)));
    if (PyObject* text = PyTuple_GET_ITEM(where, 3); text != Py_None) {
        location.sourceLine = Utf8Of(text);
    }
    Py_DECREF(where);
    if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();
    }
    return location;
}

void TryCatchReThrow(TryCatchState& state) noexcept {
    state.rethrow = true;
}

void TryCatchReset(TryCatchState& state) noexcept {
    CatchPendingException(*state.owner);
    Py_CLEAR(state.exception);
    state.caught = false;
    state.terminated = false;
    state.rethrow = false;
}

// --- build identity ----------------------------------------------------------------

std::string_view BackendBuildId() noexcept {
    // The interpreter's version and, more to the point, its bytecode magic
    // number: what `marshal` refuses a code object over is exactly what the
    // magic number changes for. Computed once; a string literal would have to
    // be kept in step with the engine by hand.
    static const std::string id = [] {
        std::string text = "python-" PY_VERSION "-";
        const long magic = PyImport_GetMagicNumber();
        text += std::to_string(magic);
        return text;
    }();
    return id;
}

}  // namespace detail

std::optional<Context> Context::New(Isolate& isolate) {
    detail::ContextRec* rec = detail::NewContext(isolate);
    if (rec == nullptr) {
        return std::nullopt;
    }
    Context context = Context::FromRec(rec);
    detail::ReleaseContext(rec);  // FromRec took its own reference
    return context;
}

}  // namespace ub
