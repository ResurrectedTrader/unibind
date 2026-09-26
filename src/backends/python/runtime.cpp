// The runtime area of the CPython backend: stopping a script from outside,
// interrupts, the job queues and the promises they drain, and the heap and
// stack budgets. See internal.h for the model this sits in.
//
// Three facts about this CPython shape everything here:
//
//   * **It has no terminate, and no interrupt either.** What it has is the
//     *pending call*: a per-interpreter queue that any thread may add to and
//     that the interpreter's own thread drains at its next eval-breaker check
//     - every loop back-edge, every function entry, most calls. A pending call
//     that returns -1 with an exception set raises that exception at the check.
//     Both `TerminateExecution` and `RequestInterrupt` are built on one such
//     call, `Service`, queued at most once at a time.
//   * **A promise is an `asyncio.Future`, and the engine's job queue is an
//     asyncio event loop** - one per isolate, never run forever. `PumpJobs`
//     drains what the loop has ready without ever blocking in it, which is
//     exactly the microtask checkpoint `unibind/isolate.h` describes. Scripts
//     use asyncio as they would anywhere else.
//   * **Its allocator is ours to wrap.** `PyMem_SetAllocator` accepts hooks
//     before the interpreter comes up; a small header on every block lets a
//     byte count follow the block to whichever thread frees it, which is how a
//     heap limit and heap statistics are possible at all.

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

#include "internal.h"

// Exported by the (static) interpreter but declared only in its internal
// headers, which drag in half of CPython's private build configuration. The
// signature is 3.14's (3.13's too), from Include/internal/pycore_ceval.h: per
// interpreter, callable from any thread without the GIL - it takes the queue's
// own lock. `flags` is a bit set: `_Py_PENDING_MAINTHREADONLY` (1) is for the
// main interpreter's signal machinery and `_Py_PENDING_RAWFREE` (2) frees `arg`
// after the call; neither is wanted here. The result is
// `_Py_add_pending_call_result`: 0 queued, -1 the queue was full.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-identifier"
#endif
extern "C" int _PyEval_AddPendingCall(PyInterpreterState* interp, int (*func)(void*), void* arg, int flags);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {
/// `_Py_ADD_PENDING_SUCCESS`.
constexpr int PENDING_CALL_QUEUED = 0;
}  // namespace

namespace ub::detail {

// ---------------------------------------------------------------------------
// Heap accounting
//
// Every block the MEM and OBJ domains hand out carries a 16-byte header in
// front of it: the account it is charged to and its size. The account is the
// allocating thread's isolate - one isolate per thread, so a thread-local is
// the whole attribution - and the header is what lets a free, on any thread
// and at any time, credit the right one. 16 bytes keeps the block the
// allocator returned 16-aligned, which is what both CPython's small-object
// allocator and the CRT's `malloc` promise.
//
// The RAW domain is left alone. It is what the interpreter uses for its own
// thread and interpreter states and for everything it allocates without the
// GIL, and a refusal there is one CPython treats as fatal rather than as a
// `MemoryError`. It is not where a script's objects live.
// ---------------------------------------------------------------------------

/// One isolate's heap figures and budget.
///
/// **Never freed.** A block charged to an account can be freed after its
/// isolate is gone - an object the interpreter's teardown leaves behind, freed
/// at process exit - and that free has to find the account still there. So
/// accounts are pooled instead: one whose isolate has gone and whose count has
/// reached zero is charged by no live block, can be credited by no future free,
/// and is handed to the next isolate made.
struct HeapAccount {
    std::atomic<std::size_t> used{0};
    std::atomic<std::size_t> peak{0};
    /// 0 is no limit.
    std::atomic<std::size_t> limit{0};
    /// An allocation was refused at this limit and has not been forgiven yet;
    /// see `OverLimit`. Cleared by a free that brings the count well back
    /// under, or by the limit being raised.
    std::atomic<bool> crossed{false};
    std::atomic<bool> retired{false};

    // The isolate's thread only.
    Isolate* isolate = nullptr;
    std::size_t initialLimit = 0;
    HeapLimitCallback callback = nullptr;
    CallbackData data;
};

namespace {

struct alignas(16) BlockHeader {
    HeapAccount* owner;  ///< null: allocated by a thread with no isolate
    std::size_t size;    ///< what was asked of the underlying allocator, header included
};
static_assert(sizeof(BlockHeader) == 16, "the header must keep blocks 16-aligned");

constexpr std::size_t HEADER = sizeof(BlockHeader);
constexpr std::size_t MAX_REQUEST = static_cast<std::size_t>(PY_SSIZE_T_MAX) - HEADER;

/// One wrapped domain: the allocator it had before, which does the work.
struct HookedDomain {
    PyMemAllocatorEx original{};
};

HookedDomain gMemDomain;
HookedDomain gObjDomain;
bool gHooksInstalled = false;

std::mutex gAccountMutex;
std::vector<HeapAccount*> gAccounts;  // never shrinks; see HeapAccount

/// The account this thread's allocations are charged to: its isolate's, from
/// `IsolateRuntimeSetup` to `IsolateRuntimeTeardown`.
thread_local HeapAccount* tAccount = nullptr;
/// Set while a heap-limit callback or the fault handler runs, so that what
/// they allocate - they should not, but an embedder's callback is the
/// embedder's - is neither refused nor able to recurse into them.
thread_local bool tInHeapHook = false;

void Charge(HeapAccount& account, std::size_t bytes) noexcept {
    const std::size_t now = account.used.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    std::size_t peak = account.peak.load(std::memory_order_relaxed);
    while (now > peak && !account.peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
}

void Credit(HeapAccount& account, std::size_t bytes) noexcept {
    // The hysteresis first, because after the subtraction the account may be
    // pooled and handed to another isolate: this thread must not touch it
    // again. An eighth of the limit below it is "well back under".
    if (account.crossed.load(std::memory_order_relaxed)) {
        const std::size_t limit = account.limit.load(std::memory_order_relaxed);
        const std::size_t used = account.used.load(std::memory_order_relaxed);
        if (used - bytes < limit - limit / 8) {
            account.crossed.store(false, std::memory_order_relaxed);
        }
    }
    account.used.fetch_sub(bytes, std::memory_order_acq_rel);
}

/// `EngineFault::OutOfMemory`, reported without allocating: the message is a
/// literal and the report lives on the stack. `unibind/isolate.h` forbids the
/// handler to allocate, and the reason is right here - the heap it would
/// allocate from is the one that just said no.
void ReportOutOfMemory(Isolate* isolate) noexcept {
    const EngineFaultCallback handler = PlatformFaultHandler();
    if (handler == nullptr) {
        return;
    }
    EngineFaultReport report;
    report.fault = EngineFault::OutOfMemory;
    report.isolate = isolate;
    report.message = "the isolate's Python heap reached its heapLimitBytes";
    handler(report, PlatformFaultData());
}

/// An allocation of `bytes` would take the account past its limit.
///
/// Once per crossing, the heap-limit callback is asked first and the ceiling
/// it answers is adopted - clamped up, never down, as `unibind/isolate.h`
/// says. If the allocation still does not fit it is refused, which CPython
/// turns into a `MemoryError` the script can catch, and the fault is reported.
/// A crossing lasts until the count falls well back under (`Credit`) or the
/// ceiling is raised, so that a script failing allocation after allocation in
/// one `except MemoryError` loop is one report, not thousands.
///
/// Nothing is collected first. CPython runs its collector only at points
/// where every object is consistent, and inside an allocation is not one.
[[nodiscard]] bool OverLimit(HeapAccount& account, std::size_t bytes) noexcept {
    if (account.crossed.load(std::memory_order_relaxed)) {
        return false;
    }
    tInHeapHook = true;
    std::size_t limit = account.limit.load(std::memory_order_relaxed);
    if (account.callback != nullptr && account.isolate != nullptr) {
        const std::size_t answer = account.callback(*account.isolate, limit, account.initialLimit, account.data);
        if (answer > limit) {
            limit = answer;
            account.limit.store(limit, std::memory_order_relaxed);
        }
    }
    const bool fits = account.used.load(std::memory_order_relaxed) + bytes <= limit;
    if (!fits) {
        account.crossed.store(true, std::memory_order_relaxed);
        ReportOutOfMemory(account.isolate);
    }
    tInHeapHook = false;
    return fits;
}

[[nodiscard]] bool Admit(HeapAccount& account, std::size_t bytes) noexcept {
    const std::size_t limit = account.limit.load(std::memory_order_relaxed);
    if (limit == 0 || tInHeapHook) {
        return true;
    }
    const std::size_t used = account.used.load(std::memory_order_relaxed);
    if (used <= limit && bytes <= limit - used) {
        return true;
    }
    return OverLimit(account, bytes);
}

[[nodiscard]] void* Finish(void* base, HeapAccount* owner, std::size_t bytes) noexcept {
    if (base == nullptr) {
        return nullptr;
    }
    auto* header = static_cast<BlockHeader*>(base);
    header->owner = owner;
    header->size = bytes;
    if (owner != nullptr) {
        Charge(*owner, bytes);
    }
    return header + 1;
}

void* HookMalloc(void* ctx, std::size_t size) {
    const HookedDomain& domain = *static_cast<HookedDomain*>(ctx);
    if (size > MAX_REQUEST) {
        return nullptr;
    }
    const std::size_t bytes = size + HEADER;
    HeapAccount* owner = tAccount;
    if (owner != nullptr && !Admit(*owner, bytes)) {
        return nullptr;
    }
    return Finish(domain.original.malloc(domain.original.ctx, bytes), owner, bytes);
}

void* HookCalloc(void* ctx, std::size_t count, std::size_t elsize) {
    const HookedDomain& domain = *static_cast<HookedDomain*>(ctx);
    if (elsize != 0 && count > MAX_REQUEST / elsize) {
        return nullptr;
    }
    const std::size_t bytes = count * elsize + HEADER;
    HeapAccount* owner = tAccount;
    if (owner != nullptr && !Admit(*owner, bytes)) {
        return nullptr;
    }
    return Finish(domain.original.calloc(domain.original.ctx, 1, bytes), owner, bytes);
}

void* HookRealloc(void* ctx, void* block, std::size_t size) {
    if (block == nullptr) {
        return HookMalloc(ctx, size);
    }
    const HookedDomain& domain = *static_cast<HookedDomain*>(ctx);
    if (size > MAX_REQUEST) {
        return nullptr;
    }
    BlockHeader* header = static_cast<BlockHeader*>(block) - 1;
    HeapAccount* owner = header->owner;
    const std::size_t before = header->size;
    const std::size_t after = size + HEADER;
    // Growth is budgeted only on the owner's own thread: a block charged to
    // one isolate and grown on another thread is the interpreter's business,
    // and refusing it there would be refusing the wrong heap.
    if (owner != nullptr && after > before && owner == tAccount && !Admit(*owner, after - before)) {
        return nullptr;
    }
    void* moved = domain.original.realloc(domain.original.ctx, header, after);
    if (moved == nullptr) {
        return nullptr;
    }
    header = static_cast<BlockHeader*>(moved);
    header->size = after;
    if (owner != nullptr) {
        if (after > before) {
            Charge(*owner, after - before);
        } else if (before > after) {
            Credit(*owner, before - after);
        }
    }
    return header + 1;
}

void HookFree(void* ctx, void* block) {
    if (block == nullptr) {
        return;
    }
    const HookedDomain& domain = *static_cast<HookedDomain*>(ctx);
    BlockHeader* header = static_cast<BlockHeader*>(block) - 1;
    if (header->owner != nullptr) {
        Credit(*header->owner, header->size);
    }
    domain.original.free(domain.original.ctx, header);
}

[[nodiscard]] HeapAccount* AcquireAccount() noexcept {
    const std::lock_guard<std::mutex> lock(gAccountMutex);
    for (HeapAccount* account : gAccounts) {
        if (account->retired.load(std::memory_order_acquire) && account->used.load(std::memory_order_acquire) == 0) {
            account->retired.store(false, std::memory_order_relaxed);
            account->peak.store(0, std::memory_order_relaxed);
            account->limit.store(0, std::memory_order_relaxed);
            account->crossed.store(false, std::memory_order_relaxed);
            account->isolate = nullptr;
            account->initialLimit = 0;
            account->callback = nullptr;
            account->data = {};
            return account;
        }
    }
    auto* account = new (std::nothrow) HeapAccount();
    if (account == nullptr) {
        return nullptr;
    }
    try {
        gAccounts.push_back(account);
    } catch (const std::bad_alloc&) {
        delete account;
        return nullptr;
    }
    return account;
}

/// The commit limit - physical memory plus page file - which no heap in this
/// process can exceed and so bounds `usedBytes` honestly when no
/// `heapLimitBytes` was asked for.
[[nodiscard]] std::uint64_t SystemMemoryCeiling() noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0 && status.ullTotalPageFile != 0) {
        return status.ullTotalPageFile;
    }
    return std::numeric_limits<std::uint64_t>::max();
}

}  // namespace

// ---------------------------------------------------------------------------
// Per-isolate state
// ---------------------------------------------------------------------------

struct RuntimeInterrupt {
    InterruptCallback callback = nullptr;
    CallbackData data;
};

struct RuntimePostedJob {
    JobCallback callback = nullptr;
    CallbackData data;
};

struct RuntimeState {
    Isolate* isolate = nullptr;
    PyInterpreterState* interp = nullptr;

    /// `Service` is in the interpreter's pending-call queue. That queue holds
    /// 32 calls for the whole interpreter and refuses the 33rd, so this one is
    /// never queued twice.
    std::atomic<bool> scheduled{false};
    /// Teardown has begun: nothing is accepted, `Service` does nothing on the
    /// isolate's own thread.
    std::atomic<bool> closed{false};

    /// The isolate's thread, by its OS id - which is how `Service` and the
    /// monitoring callback, run on whichever of the interpreter's threads
    /// trips the check first, tell it from a thread a script started.
    DWORD ownerThread = 0;
    /// Stop every thread of the interpreter but the isolate's own: set by
    /// `~Isolate` while it waits for the threads a script started
    /// (`StopScriptThreads`). Unlike `terminating` it is honoured after
    /// `closed`, and needs nothing from the isolate, so that it goes on
    /// stopping threads an isolate had to abandon; see `~Isolate`.
    std::atomic<bool> stopThreads{false};
    /// `unibind.Terminated`, strong, for raising on a thread that cannot find
    /// the isolate's `Types`. Dropped when the last thread has been waited
    /// for - or never, if the interpreter is abandoned with threads in it.
    PyObject* threadStop = nullptr;

    std::mutex interruptMutex;
    std::deque<RuntimeInterrupt> interrupts;

    std::mutex jobMutex;
    std::deque<RuntimePostedJob> jobs;
    std::multimap<std::chrono::steady_clock::time_point, RuntimePostedJob> delayedJobs;

    // The event loop, and the parts of it the pump drives directly. All null
    // when asyncio could not be imported, and then there are no promises.
    // Strong references, dropped at teardown.
    PyObject* support = nullptr;         ///< the Python half, below: a dict
    PyObject* loop = nullptr;            ///< an `asyncio.SelectorEventLoop`
    PyObject* ready = nullptr;           ///< `loop._ready`: the microtask queue
    PyObject* scheduledTimers = nullptr; ///< `loop._scheduled`: its timers
    PyObject* runOnce = nullptr;         ///< `loop._run_once`, bound
    PyObject* setRunningLoop = nullptr;  ///< `asyncio.events._set_running_loop`
    PyObject* futureType = nullptr;      ///< `asyncio.Future`

    /// The `sys.monitoring` tool this isolate claimed, or -1, and whether its
    /// events are switched on - only while a stop is in force; see `ArmStop`.
    int monitoringTool = -1;
    bool monitoringArmed = false;

    HeapAccount* account = nullptr;
    /// The lowest stack address native code on this isolate's thread may reach
    /// before recursion is refused; see `IsolateRuntimeSetup`.
    std::uintptr_t stackFloor = 0;
};

void DestroyRuntimeState(RuntimeState* state) noexcept {
    if (state == nullptr) {
        return;
    }
    // After the interpreter has ended: nothing here may touch Python. The
    // queues and the loop went at teardown, and the account outlives us on
    // purpose.
    if (state->account != nullptr) {
        state->account->isolate = nullptr;
        state->account->callback = nullptr;
        state->account->retired.store(true, std::memory_order_release);
    }
    delete state;
}

namespace {

[[nodiscard]] RuntimeState* RuntimeOf(Isolate& isolate) noexcept {
    return isolate.impl().runtime.get();
}

// ---------------------------------------------------------------------------
// Termination and interrupts
// ---------------------------------------------------------------------------

int Service(void* arg);

/// Queue `Service` unless it already is. Any thread. `evenIfClosed` is for the
/// stop `~Isolate` puts on the threads a script started, which outlives
/// `closed`.
void Schedule(RuntimeState& runtime, bool evenIfClosed = false) noexcept {
    if (!evenIfClosed && runtime.closed.load(std::memory_order_acquire)) {
        return;
    }
    if (!runtime.scheduled.exchange(true)) {
        // Refused only when the interpreter's queue is full, and nothing else
        // in a sub-interpreter queues pending calls: signals and the legacy
        // `Py_AddPendingCall` both go to the main interpreter. Should it ever
        // happen the request is not lost - a stop is still enforced at every
        // gate and an interrupt waits for the next `Schedule` - but a running
        // loop would not be reached.
        if (_PyEval_AddPendingCall(runtime.interp, &Service, &runtime, /*flags=*/0) != PENDING_CALL_QUEUED) {
            runtime.scheduled.store(false);
        }
    }
}

/// Run the requested interrupts: in order, once each, and until none are
/// waiting - so one asked for by a callback runs in this pass. A stop in force
/// holds the rest back for the first check after the cancel.
///
/// Each callback gets a `HandleScope` of its own, which is what V8 gives it,
/// and runs with no `TryCatch` in reach: an interrupt is not part of the code
/// it interrupted, so a failure inside it must not be handed to a handler the
/// interrupted code opened. Whatever it leaves pending is cleared - it runs
/// between two bytecodes of unrelated code, and an exception left there would
/// be raised by code that never threw it.
void RunInterrupts(RuntimeState& runtime) noexcept {
    Isolate& isolate = *runtime.isolate;
    Isolate::Impl& impl = isolate.impl();
    TryCatchState* handler = std::exchange(impl.tryCatch, nullptr);
    for (;;) {
        if (Terminating(isolate)) {
            break;
        }
        RuntimeInterrupt next;
        {
            const std::lock_guard<std::mutex> lock(runtime.interruptMutex);
            if (runtime.interrupts.empty()) {
                break;
            }
            next = runtime.interrupts.front();
            runtime.interrupts.pop_front();
        }
        {
            const HandleScope scope(isolate);
            next.callback(isolate, next.data);
        }
        if (PyErr_Occurred() != nullptr) {
            PyErr_Clear();
        }
    }
    impl.tryCatch = handler;
}

/// The pending call. Runs on the isolate's thread at an eval-breaker check,
/// which is between two bytecodes of running Python - never while the thread
/// is idle, because nothing checks then.
///
/// **How a stop is made to stick.** CPython's pending calls are one-shot, and
/// a single `Terminated` raised into a script that says `except BaseException:
/// pass` would be swallowed. So while a stop is in force this re-queues itself
/// before raising: the eval breaker stays tripped, and the very next check -
/// the loop's back-edge, the next function entered, the `finally` block's own
/// loop - raises `Terminated` again. Nothing a script writes gets past a check
/// without meeting it, and a loop cannot be written without one. It costs
/// nothing when no stop is in force: the call is not queued then.
///
/// `sys.monitoring` could do the same per line, and is not needed: every
/// construct that can run for long - a loop, a call, a recursion - already
/// passes an eval-breaker check, and the straight-line code between two checks
/// ends by itself. What no check reaches is a single long-running builtin -
/// `sum(range(10**12))` - which is native code, and runs to completion as a
/// native callback does.
/// `sys.monitoring` events, while a stop is in force: every new line and every
/// call raises `Terminated` again.
///
/// The pending call alone stops every loop and every Python function entered,
/// but two things run between its checks: straight-line code, such as a
/// `finally:` block whose body is a statement or two, and a call to a builtin
/// or a native - CPython checks after such a call returns, not before it. Both
/// would let a script that was told to stop do one more thing, and on V8 a
/// stopped script's `finally` does not run at all. A LINE and a CALL event
/// close both gaps. PEP 669 instruments code only while events are set, so
/// this costs nothing until a stop and is switched off again by the cancel.
///
/// The tool id is claimed once per isolate at setup; 3 and 4 are the ids
/// CPython leaves unassigned. If a script's own debugger holds both, the stop
/// still works - by the pending call alone, with the two gaps above.
///
/// The callback is the interpreter's, not the thread's: it fires on every
/// thread that runs Python in this interpreter, including threads a script
/// started. It is bound to the isolate's `RuntimeState` rather than finding the
/// isolate through the thread, so that a stop in force - the embedder's, or the
/// one `~Isolate` puts on those threads - stops them too.
constexpr const char* RUNTIME_CAPSULE = "unibind.runtime";

/// Whether a stop applies to the thread asking. The isolate's own thread
/// honours `terminating` until teardown closes the runtime; every other thread
/// honours it too, and `stopThreads` besides, which outlives the isolate.
[[nodiscard]] bool StopApplies(RuntimeState& runtime) noexcept {
    const bool closed = runtime.closed.load(std::memory_order_acquire);
    if (!closed && Terminating(*runtime.isolate)) {
        return true;
    }
    return GetCurrentThreadId() != runtime.ownerThread && runtime.stopThreads.load(std::memory_order_acquire);
}

PyObject* MonitoringCallback(PyObject* self, PyObject* const* /*args*/, Py_ssize_t /*count*/) {
    auto* runtime = static_cast<RuntimeState*>(PyCapsule_GetPointer(self, RUNTIME_CAPSULE));
    if (runtime == nullptr) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    if (runtime->threadStop != nullptr && StopApplies(*runtime)) {
        PyErr_SetNone(runtime->threadStop);
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyMethodDef monitoringCallbackDef = {"_stop", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(&MonitoringCallback)),
                                     METH_FASTCALL, "unibind: raises Terminated while a stop is in force."};

constexpr int MONITORING_TOOLS[] = {4, 3};

/// `sys.monitoring.events.<name>`, or -1.
[[nodiscard]] long MonitoringEvent(PyObject* monitoring, const char* name) noexcept {
    PyObject* events = PyObject_GetAttrString(monitoring, "events");
    PyObject* value = events == nullptr ? nullptr : PyObject_GetAttrString(events, name);
    const long event = value == nullptr ? -1 : PyLong_AsLong(value);
    Py_XDECREF(value);
    Py_XDECREF(events);
    if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        return -1;
    }
    return event;
}

/// Claim a tool id and register the callback for it, with no events set.
void ClaimMonitoring(RuntimeState& runtime) noexcept {
    PyObject* monitoring = PySys_GetObject("monitoring");  // borrowed
    if (monitoring == nullptr) {
        return;
    }
    const long line = MonitoringEvent(monitoring, "LINE");
    const long call = MonitoringEvent(monitoring, "CALL");
    PyObject* self = PyCapsule_New(&runtime, RUNTIME_CAPSULE, nullptr);
    PyObject* callback = self == nullptr ? nullptr : PyCFunction_NewEx(&monitoringCallbackDef, self, nullptr);
    Py_XDECREF(self);
    if (line < 0 || call < 0 || callback == nullptr) {
        Py_XDECREF(callback);
        PyErr_Clear();
        return;
    }
    for (const int tool : MONITORING_TOOLS) {
        PyObject* holder = PyObject_CallMethod(monitoring, "get_tool", "i", tool);
        const bool free = holder == Py_None;
        Py_XDECREF(holder);
        if (!free) {
            PyErr_Clear();
            continue;
        }
        PyObject* claimed = PyObject_CallMethod(monitoring, "use_tool_id", "is", tool, "unibind");
        PyObject* onLine = claimed == nullptr ? nullptr
                                              : PyObject_CallMethod(monitoring, "register_callback", "ilO", tool, line,
                                                                    callback);
        PyObject* onCall = onLine == nullptr ? nullptr
                                             : PyObject_CallMethod(monitoring, "register_callback", "ilO", tool, call,
                                                                   callback);
        const bool ok = onCall != nullptr;
        Py_XDECREF(claimed);
        Py_XDECREF(onLine);
        Py_XDECREF(onCall);
        if (ok) {
            runtime.monitoringTool = tool;
            break;
        }
        PyErr_Clear();
    }
    Py_DECREF(callback);
    PyErr_Clear();
}

/// Switch the tool's events on or off. On the isolate's thread; keeps what is
/// pending, because it is called from the middle of an unwind.
void SetMonitoring(RuntimeState& runtime, bool armed) noexcept {
    if (runtime.monitoringTool < 0 || runtime.monitoringArmed == armed) {
        return;
    }
    PyObject* monitoring = PySys_GetObject("monitoring");  // borrowed
    if (monitoring == nullptr) {
        return;
    }
    const long events = armed ? MonitoringEvent(monitoring, "LINE") | MonitoringEvent(monitoring, "CALL") : 0;
    PyObject* pending = PyErr_GetRaisedException();
    PyObject* set = PyObject_CallMethod(monitoring, "set_events", "il", runtime.monitoringTool, events);
    if (set != nullptr) {
        runtime.monitoringArmed = armed;
    }
    Py_XDECREF(set);
    PyErr_Clear();
    if (pending != nullptr) {
        PyErr_SetRaisedException(pending);
    }
}

/// `Service` on a thread a script started. Interrupts belong to the isolate's
/// thread (`Isolate::RequestInterrupt`), so they are left queued, and the call
/// is queued again so that the isolate's thread finds them at its next check -
/// until it does, a check on this thread comes back here, which costs a lock
/// and nothing else. A stop is raised here as on the isolate's thread.
int ServiceScriptThread(RuntimeState& runtime) noexcept {
    if (runtime.threadStop != nullptr && StopApplies(runtime)) {
        Schedule(runtime, /*evenIfClosed=*/true);
        SetMonitoring(runtime, true);
        PyErr_SetNone(runtime.threadStop);
        return -1;
    }
    if (!runtime.closed.load(std::memory_order_acquire)) {
        bool waiting = false;
        {
            const std::lock_guard<std::mutex> lock(runtime.interruptMutex);
            waiting = !runtime.interrupts.empty();
        }
        if (waiting) {
            Schedule(runtime);
        }
    }
    return 0;
}

int Service(void* arg) {
    auto& runtime = *static_cast<RuntimeState*>(arg);
    // Cleared first, so that a request made from here on queues us again.
    runtime.scheduled.store(false);
    // Pending calls run on whichever of the interpreter's threads checks first.
    if (GetCurrentThreadId() != runtime.ownerThread) {
        return ServiceScriptThread(runtime);
    }
    if (runtime.closed.load(std::memory_order_acquire)) {
        // A stop `~Isolate` put on the script's threads outlives `closed`;
        // keep it queued for them.
        if (runtime.stopThreads.load(std::memory_order_acquire)) {
            Schedule(runtime, /*evenIfClosed=*/true);
        }
        return 0;
    }
    Isolate& isolate = *runtime.isolate;
    if (!Terminating(isolate)) {
        RunInterrupts(runtime);
        if (!Terminating(isolate)) {
            if (runtime.stopThreads.load(std::memory_order_acquire)) {
                Schedule(runtime, /*evenIfClosed=*/true);
            }
            return 0;
        }
    }
    Schedule(runtime);
    SetMonitoring(runtime, true);
    PyErr_SetNone(isolate.impl().types.terminated);
    return -1;
}

/// `sys.unraisablehook` for the isolate: `Terminated` reaching the top of a
/// finalizer - a `__del__`, a coroutine being closed - or of a thread a script
/// started, while a stop is in force, is the stop working, not an error to
/// print. Everything else goes to the default hook. A builtin rather than a
/// Python function on purpose: a Python one would meet `Terminated` itself on
/// entry, while a stop is in force, which is exactly when it is needed. Bound
/// to `unibind.Terminated` (`self`), because it runs on those threads too,
/// where there is no isolate to look the type up in.
PyObject* UnraisableHook(PyObject* self, PyObject* unraisable) {
    if (self != nullptr) {
        PyObject* value = PyObject_GetAttrString(unraisable, "exc_value");
        if (value == nullptr) {
            PyErr_Clear();
        } else {
            const bool stop = PyErr_GivenExceptionMatches(value, self) != 0;
            Py_DECREF(value);
            if (stop) {
                Py_RETURN_NONE;
            }
        }
    }
    PyObject* fallback = PySys_GetObject("__unraisablehook__");  // borrowed
    if (fallback == nullptr) {
        Py_RETURN_NONE;
    }
    return PyObject_CallOneArg(fallback, unraisable);
}

PyMethodDef unraisableHookDef = {"_unraisablehook", &UnraisableHook, METH_O,
                                 "sys.unraisablehook for a unibind isolate: a stop is not an error."};

/// The loop's exception handler: says nothing. A pump is not a call and has
/// nowhere to put an exception (`unibind/isolate.h`), and an unhandled
/// rejection is not reported on either engine - so "Task exception was never
/// retrieved" and "Task was destroyed but it is pending" are asyncio saying
/// what this API has decided not to. A builtin, for the same reason as the
/// unraisable hook: it must not itself meet a stop on entry.
PyObject* QuietExceptionHandler(PyObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}

PyMethodDef quietHandlerDef = {"_quiet_exception_handler", &QuietExceptionHandler, METH_VARARGS,
                               "The isolate's event-loop exception handler: a pump has nowhere to put one."};

// ---------------------------------------------------------------------------
// Promises and the event loop
//
// A promise is an `asyncio.Future` of the isolate's own loop, and a task - a
// coroutine being driven - is one too, so both are `ub::Promise`. Settling one
// schedules its callbacks on the loop; nothing runs them but `PumpJobs`,
// which is where `unibind/isolate.h` says continuations run.
//
// The loop is a `SelectorEventLoop`, made when the isolate is and set as the
// thread's current loop, so that `asyncio.get_event_loop()` at the top of a
// script finds it rather than making one nobody drains. It is never run
// forever: see `DrainLoop`.
// ---------------------------------------------------------------------------

constexpr const char* RUNTIME_SOURCE = R"PY(
import asyncio as _asyncio
import asyncio.events as _events
import weakref as _weakref

# Futures locked in to another awaitable: resolved, not yet settled, as a
# JavaScript promise that was resolved with a thenable.
_following = _weakref.WeakSet()

import threading as _threading

# The isolate's loop is the thread's current loop, and stays it. `asyncio.run`
# makes a loop of its own and, closing it, sets the current loop to None -
# after which `get_event_loop()`, `Future()` and `ensure_future()` in the next
# script would raise "There is no current event loop". So the policy falls back
# to the isolate's loop, on the isolate's thread, whenever none is set: a
# script that sets one of its own still gets that one. Through asyncio's
# private `_get_event_loop_policy`/`_set_event_loop_policy`, which asyncio
# itself uses: the public pair warns from 3.14, for the policy system's
# removal in 3.16.
_isolate_loop = None
_isolate_thread = None

class _Policy(type(_events._get_event_loop_policy())):
    def get_event_loop(self):
        loop = _isolate_loop
        if (self._local._loop is None and loop is not None and not loop.is_closed()
                and _threading.get_ident() == _isolate_thread):
            self.set_event_loop(loop)
        return super().get_event_loop()

def new_loop(quiet):
    global _isolate_loop, _isolate_thread
    loop = _asyncio.SelectorEventLoop()
    loop.set_exception_handler(quiet)
    _events._set_event_loop_policy(_Policy())
    _asyncio.set_event_loop(loop)
    _isolate_loop = loop
    _isolate_thread = _threading.get_ident()
    return loop

def _follow(source, target):
    _following.discard(target)
    if target.done():
        return
    if source.cancelled():
        target.cancel()
        return
    error = source.exception()
    if error is not None:
        target.set_exception(error)
    else:
        target.set_result(source.result())

def resolve(loop, future, value):
    if future.done() or future in _following:
        return False
    if value is future:
        future.set_exception(TypeError("a promise cannot be resolved with itself"))
    elif _asyncio.isfuture(value) or _asyncio.iscoroutine(value) or hasattr(type(value), "__await__"):
        source = _asyncio.ensure_future(value, loop=loop)
        _following.add(future)
        source.add_done_callback(lambda done, target=future: _follow(done, target))
    else:
        future.set_result(value)
    return True

def reject(future, error):
    if future.done() or future in _following:
        return False
    future.set_exception(error)
    return True

def close_loop(loop):
    global _isolate_loop
    # Dropped, not run: nothing is stepped again. Closing each coroutine
    # keeps an unstarted one from warning that it was never awaited, and a
    # suspended one gets the GeneratorExit its garbage collection would have
    # given it anyway.
    for task in _asyncio.all_tasks(loop):
        task._log_destroy_pending = False
        try:
            task.get_coro().close()
        except BaseException:
            pass
    loop.close()
    _isolate_loop = None
    _asyncio.set_event_loop(None)
)PY";

/// A function from the Python half. Borrowed, or null.
[[nodiscard]] PyObject* Helper(RuntimeState& runtime, const char* name) noexcept {
    return runtime.support == nullptr ? nullptr : PyDict_GetItemString(runtime.support, name);
}

[[nodiscard]] bool IsFuture(RuntimeState& runtime, PyObject* object) noexcept {
    return runtime.futureType != nullptr &&
           PyObject_TypeCheck(object, reinterpret_cast<PyTypeObject*>(runtime.futureType)) != 0;
}

/// Make the isolate's loop, and the promise type with it. False with an
/// exception pending if asyncio is not there to make one - which is not an
/// isolate that cannot be made, only one without promises.
[[nodiscard]] bool MakeLoop(Isolate& isolate, RuntimeState& runtime) noexcept {
    runtime.support = PyDict_New();
    if (runtime.support == nullptr) {
        return false;
    }
    PyObject* builtins = PyImport_ImportModule("builtins");
    if (builtins == nullptr || PyDict_SetItemString(runtime.support, "__builtins__", builtins) != 0) {
        Py_XDECREF(builtins);
        return false;
    }
    Py_DECREF(builtins);
    // Named "<unibind>", so its frames are left out of every stack the
    // backend reports, as module.cpp's are.
    PyObject* code = Py_CompileString(RUNTIME_SOURCE, "<unibind>", Py_file_input);
    if (code == nullptr) {
        return false;
    }
    PyObject* ran = PyEval_EvalCode(code, runtime.support, runtime.support);
    Py_DECREF(code);
    if (ran == nullptr) {
        return false;
    }
    Py_DECREF(ran);

    PyObject* quiet = PyCFunction_NewEx(&quietHandlerDef, nullptr, nullptr);
    if (quiet == nullptr) {
        return false;
    }
    runtime.loop = PyObject_CallOneArg(Helper(runtime, "new_loop"), quiet);
    Py_DECREF(quiet);
    if (runtime.loop == nullptr) {
        return false;
    }
    runtime.ready = PyObject_GetAttrString(runtime.loop, "_ready");
    runtime.scheduledTimers = PyObject_GetAttrString(runtime.loop, "_scheduled");
    runtime.runOnce = PyObject_GetAttrString(runtime.loop, "_run_once");
    PyObject* asyncio = PyImport_ImportModule("asyncio");
    if (asyncio != nullptr) {
        runtime.futureType = PyObject_GetAttrString(asyncio, "Future");
        PyObject* events = PyObject_GetAttrString(asyncio, "events");
        if (events != nullptr) {
            runtime.setRunningLoop = PyObject_GetAttrString(events, "_set_running_loop");
            Py_DECREF(events);
        }
        Py_DECREF(asyncio);
    }
    if (runtime.ready == nullptr || runtime.scheduledTimers == nullptr || runtime.runOnce == nullptr ||
        runtime.futureType == nullptr || runtime.setRunningLoop == nullptr || !PyType_Check(runtime.futureType)) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_ImportError, "unibind: asyncio is not the shape this backend drives");
        }
        return false;
    }
    // Tasks derive from it, so a task is a `ub::Promise` too.
    isolate.impl().types.promise = reinterpret_cast<PyTypeObject*>(Py_NewRef(runtime.futureType));
    return true;
}

void DropLoop(RuntimeState& runtime) noexcept {
    Py_CLEAR(runtime.ready);
    Py_CLEAR(runtime.scheduledTimers);
    Py_CLEAR(runtime.runOnce);
    Py_CLEAR(runtime.setRunningLoop);
    Py_CLEAR(runtime.futureType);
    Py_CLEAR(runtime.loop);
    Py_CLEAR(runtime.support);
}

[[nodiscard]] Py_ssize_t LengthOf(PyObject* container) noexcept {
    const Py_ssize_t length = PyObject_Length(container);
    if (length < 0) {
        PyErr_Clear();
        return 0;
    }
    return length;
}

/// What `run_forever` sets up around its iterations - done here in C, with no
/// Python frame in between, so that a stop cannot land half-way through it and
/// leave the loop believing it is still running. `run_forever` itself cannot
/// be used for exactly that reason: its `finally` is Python, and a sticky stop
/// raises again at the first call in it.
struct RunningLoop {
    explicit RunningLoop(RuntimeState& runtime) noexcept : runtime_(runtime) {
        PyObject* ident = PyLong_FromUnsignedLong(PyThread_get_thread_ident());
        if (ident != nullptr) {
            (void)PyObject_SetAttrString(runtime.loop, "_thread_id", ident);
            Py_DECREF(ident);
        }
        // `_stopping` makes every iteration poll with a zero timeout: ready
        // work runs, due timers fall into it, and nothing waits on one that
        // is not due - a delayed timer never blocks a pump.
        (void)PyObject_SetAttrString(runtime.loop, "_stopping", Py_True);
        PyObject* set = PyObject_CallOneArg(runtime.setRunningLoop, runtime.loop);
        Py_XDECREF(set);
        // Async generators finalized on this loop, as `run_forever` arranges.
        hooks_ = PySys_GetObject("get_asyncgen_hooks") == nullptr ? nullptr : CallSys("get_asyncgen_hooks", nullptr);
        PyObject* firstiter = PyObject_GetAttrString(runtime.loop, "_asyncgen_firstiter_hook");
        PyObject* finalizer = PyObject_GetAttrString(runtime.loop, "_asyncgen_finalizer_hook");
        if (firstiter != nullptr && finalizer != nullptr) {
            PyObject* args = PyTuple_Pack(2, firstiter, finalizer);
            if (args != nullptr) {
                Py_XDECREF(CallSys("set_asyncgen_hooks", args));
                Py_DECREF(args);
            }
        }
        Py_XDECREF(firstiter);
        Py_XDECREF(finalizer);
        PyErr_Clear();
    }

    ~RunningLoop() {
        PyObject* pending = PyErr_GetRaisedException();
        (void)PyObject_SetAttrString(runtime_.loop, "_stopping", Py_False);
        (void)PyObject_SetAttrString(runtime_.loop, "_thread_id", Py_None);
        PyObject* set = PyObject_CallOneArg(runtime_.setRunningLoop, Py_None);
        Py_XDECREF(set);
        if (hooks_ != nullptr) {
            Py_XDECREF(CallSys("set_asyncgen_hooks", hooks_));
            Py_DECREF(hooks_);
        }
        PyErr_Clear();
        if (pending != nullptr) {
            PyErr_SetRaisedException(pending);
        }
    }

    RunningLoop(const RunningLoop&) = delete;
    RunningLoop& operator=(const RunningLoop&) = delete;
    RunningLoop(RunningLoop&&) = delete;
    RunningLoop& operator=(RunningLoop&&) = delete;

   private:
    /// A function of `sys`, which are builtins: no Python frame, no check.
    [[nodiscard]] static PyObject* CallSys(const char* name, PyObject* args) noexcept {
        PyObject* function = PySys_GetObject(name);  // borrowed
        if (function == nullptr) {
            return nullptr;
        }
        return args == nullptr ? PyObject_CallNoArgs(function) : PyObject_Call(function, args, nullptr);
    }

    RuntimeState& runtime_;
    PyObject* hooks_ = nullptr;
};

/// Throw away what the loop has ready: the microtasks queued behind a stop.
void DiscardReady(RuntimeState& runtime) noexcept {
    PyObject* pending = PyErr_GetRaisedException();
    PyObject* cleared = PyObject_CallMethod(runtime.ready, "clear", nullptr);
    Py_XDECREF(cleared);
    // Releasing the handles may have run finalizers, which may have met the
    // stop; none of it is anyone's to see.
    PyErr_Clear();
    Py_XDECREF(pending);
}

/// The microtask checkpoint: iterate the loop until nothing is ready, never
/// blocking. Each iteration runs what was ready when it began - so a callback
/// that schedules another one sees it run in the next iteration, still inside
/// this drain, as a continuation queued by a continuation does - and moves
/// timers that have fallen due into the ready queue.
///
/// A stop discards what is still ready - V8 empties its queue when a stop
/// lands in a checkpoint, and `unibind/isolate.h` promises that on both
/// backends - and leaves the timers, which are not microtasks yet. False when
/// a stop ended it.
[[nodiscard]] bool DrainLoop(Isolate& isolate, RuntimeState& runtime) noexcept {
    if (runtime.loop == nullptr) {
        return !Terminating(isolate);
    }
    if (LengthOf(runtime.ready) == 0 && LengthOf(runtime.scheduledTimers) == 0) {
        return !Terminating(isolate);
    }
    bool stopped = false;
    {
        const RunningLoop running(runtime);
        for (;;) {
            if (Terminating(isolate)) {
                stopped = true;
                break;
            }
            PyObject* ran = PyObject_CallNoArgs(runtime.runOnce);
            if (ran == nullptr) {
                // A stop, or something a callback raised that asyncio lets
                // through - `SystemExit`, `KeyboardInterrupt`. Either way the
                // pump has nowhere to put it.
                PyErr_Clear();
            }
            Py_XDECREF(ran);
            if (Terminating(isolate)) {
                stopped = true;
                break;
            }
            if (LengthOf(runtime.ready) == 0) {
                break;
            }
        }
    }
    if (stopped) {
        DiscardReady(runtime);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stack
// ---------------------------------------------------------------------------

/// CPython 3.14 guards native recursion by address: every entry into its
/// evaluation loop, and every C-level recursion (a `repr` of a nested object, a
/// call through `tp_call`), compares the stack pointer with the thread state's
/// *soft limit* and raises `RecursionError` below it. The limit sits two
/// margins above the *base* of the stack CPython is told about, and the first
/// margin is its room to raise the error in; below the *hard limit*, one margin
/// above the base, it gives up with a fatal error. By default the base is the
/// thread's own stack end; `PyUnstable_ThreadState_SetStackProtection` moves
/// it, and this is how an isolate's budget becomes CPython's limit.
///
/// The margin is `_PyOS_STACK_MARGIN_BYTES` in Include/internal/pycore_pythonrun.h:
/// 2048 pointers, and in a debug CPython 64 KB - the overlay port's patch 0105,
/// because MSVC's unoptimised evaluation loop spends up to about 58 KB between
/// two checks, more than upstream's 4096 pointers allow for. It is not in a
/// public header, so it is repeated here; the base is put two margins below
/// the floor, which makes the soft limit the floor itself.
#if defined(Py_DEBUG)
constexpr std::size_t CPYTHON_STACK_MARGIN = std::size_t{64} * 1024;
#else
constexpr std::size_t CPYTHON_STACK_MARGIN = std::size_t{2048} * sizeof(void*);
#endif

/// Kept free below the floor: CPython's two margins, which its base sits under,
/// and 64 KB past them for whatever a native does on the way out. A thread's
/// stack ends in a guard page and, past it, the process; this is the room
/// before that. 128 KB in a release build, 192 KB in a debug one.
constexpr std::size_t STACK_HEADROOM = std::max<std::size_t>(128 * 1024, 2 * CPYTHON_STACK_MARGIN + 64 * 1024);

/// Native recursion - a callback calling back into the engine, through Python
/// or not - is held at the same floor by every native entry: each asks
/// `StackExhausted` (bindings.cpp). A native calling a native never enters the
/// evaluation loop, so CPython's own check never sees it.

[[nodiscard]] std::uintptr_t StackPointer() noexcept {
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

}  // namespace

void RaiseStop(Isolate& isolate) noexcept {
    PyObject* terminated = isolate.impl().types.terminated;
    if (terminated != nullptr && PyErr_Occurred() == nullptr) {
        PyErr_SetNone(terminated);
    }
}

bool StackExhausted(Isolate& isolate) noexcept {
    RuntimeState* runtime = RuntimeOf(isolate);
    if (runtime == nullptr || runtime->stackFloor == 0 || StackPointer() > runtime->stackFloor) {
        return false;
    }
    PyErr_SetString(PyExc_RecursionError, "maximum recursion depth exceeded in a native call");
    return true;
}

// ---------------------------------------------------------------------------
// Area hooks
// ---------------------------------------------------------------------------

bool PlatformRuntimeSetup() noexcept {
    if (gHooksInstalled) {
        return true;
    }
    // Before `Py_InitializeFromConfig`, as `PyMem_SetAllocator` requires: a
    // block allocated by one allocator and freed by another is heap
    // corruption, so the hooks must see every block of their domains from
    // the first. Process-wide - allocators are not per interpreter - and so
    // the account is found per thread instead.
    PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &gMemDomain.original);
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &gObjDomain.original);
    PyMemAllocatorEx mem{&gMemDomain, &HookMalloc, &HookCalloc, &HookRealloc, &HookFree};
    PyMemAllocatorEx obj{&gObjDomain, &HookMalloc, &HookCalloc, &HookRealloc, &HookFree};
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &mem);
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &obj);
    gHooksInstalled = true;
    return true;
}

bool InitRuntimeTypes(Isolate& isolate, PyObject* /*module*/) noexcept {
    Isolate::Impl& impl = isolate.impl();
    if (impl.runtime == nullptr) {
        auto* state = new (std::nothrow) RuntimeState();
        if (state == nullptr) {
            PyErr_NoMemory();
            return false;
        }
        state->isolate = &isolate;
        state->interp = impl.interp;
        state->ownerThread = GetCurrentThreadId();
        impl.runtime.reset(state);
    }
    RuntimeState& runtime = *impl.runtime;
    if (runtime.threadStop == nullptr) {
        runtime.threadStop = Py_XNewRef(impl.types.terminated);
    }
    PyObject* hook = PyCFunction_NewEx(&unraisableHookDef, impl.types.terminated, nullptr);
    if (hook == nullptr) {
        return false;
    }
    const int set = PySys_SetObject("unraisablehook", hook);
    Py_DECREF(hook);
    return set == 0;
}

bool IsolateRuntimeSetup(Isolate& isolate, const IsolateOptions& options) noexcept {
    RuntimeState* runtime = RuntimeOf(isolate);
    if (runtime == nullptr) {
        return false;
    }

    // --- the loop ---
    //
    // Before the account is attached, so that importing asyncio is part of
    // the interpreter's start-up rather than the script's budget. Without
    // asyncio the isolate still works; it has no promises.
    if (!MakeLoop(isolate, *runtime)) {
        PyErr_Clear();
        DropLoop(*runtime);
    }

    ClaimMonitoring(*runtime);

    // --- heap ---
    HeapAccount* account = AcquireAccount();
    if (account == nullptr) {
        return false;
    }
    account->isolate = &isolate;
    account->initialLimit = options.heapLimitBytes;
    account->limit.store(options.heapLimitBytes, std::memory_order_relaxed);
    runtime->account = account;
    // From here the thread's allocations are this isolate's. What the
    // interpreter allocated coming up is not counted: it is not the script's,
    // and a limit is a budget for what the embedder's code does with the heap.
    tAccount = account;

    // --- stack ---
    //
    // "Measured from wherever `Isolate::New` was called", which is about here,
    // and held to the thread's real stack whatever was asked: a limit past the
    // end of the stack is no limit at all. 0 is the whole stack, less the
    // headroom, because CPython's own default assumes a bigger stack than a
    // thread gets by default on Windows.
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const std::uintptr_t here = StackPointer();
    const std::size_t available = here > low ? here - low : 0;
    const std::size_t usable = available > STACK_HEADROOM ? available - STACK_HEADROOM : 0;
    const std::size_t budget =
        options.stackLimitBytes != 0 ? std::min<std::size_t>(options.stackLimitBytes, usable) : usable;
    runtime->stackFloor = here - budget;
    // CPython's own guard is an address too: its soft limit goes at the same
    // floor, so a recursion through Python and one through natives both end
    // in a `RecursionError` there. Python-to-Python calls cost no native stack
    // and are held by the separate `sys.getrecursionlimit()`, which is left as
    // it is. The region runs from two margins below the floor to the top of
    // the thread's stack. The call refuses a region smaller than three
    // margins; the top is only what CPython reports "used" against, so a
    // region that would be smaller - a small stackLimitBytes near the top of
    // a fresh thread - is reported as three margins.
    const std::uintptr_t base = runtime->stackFloor - 2 * CPYTHON_STACK_MARGIN;
    const std::size_t region = std::max<std::size_t>(high - base, 3 * CPYTHON_STACK_MARGIN);
    return PyUnstable_ThreadState_SetStackProtection(isolate.impl().tstate, reinterpret_cast<void*>(base), region) == 0;
}

void IsolateRuntimeTeardown(Isolate& isolate) noexcept {
    RuntimeState* runtime = RuntimeOf(isolate);
    if (runtime == nullptr) {
        return;
    }
    // Stop accepting first: from here a request is refused, and `Service`
    // does nothing if it is still queued - including when `Py_EndInterpreter`
    // runs the queue one last time, and including while the loop is closed
    // below, which is Python code and must not meet a stop left armed.
    runtime->closed.store(true, std::memory_order_release);

    // Dropped, not run: `unibind/isolate.h`. The embedder's `CallbackData`
    // is not ours to touch; an interrupt or a job owns nothing here.
    {
        const std::lock_guard<std::mutex> lock(runtime->interruptMutex);
        runtime->interrupts.clear();
    }
    {
        const std::lock_guard<std::mutex> lock(runtime->jobMutex);
        runtime->jobs.clear();
        runtime->delayedJobs.clear();
    }

    // A stop left armed must not reach the Python that closes the loop.
    SetMonitoring(*runtime, false);

    // The limit goes before the loop does: closing it, and the interpreter's
    // own teardown, allocate, and refusing them helps nobody. Frees keep
    // crediting the account.
    if (HeapAccount* account = runtime->account; account != nullptr) {
        account->limit.store(0, std::memory_order_relaxed);
        account->callback = nullptr;
    }

    if (runtime->loop != nullptr) {
        PyObject* closed = PyObject_CallOneArg(Helper(*runtime, "close_loop"), runtime->loop);
        Py_XDECREF(closed);
        PyErr_Clear();
    }
    DropLoop(*runtime);
    PyErr_Clear();

    if (runtime->account != nullptr && tAccount == runtime->account) {
        tAccount = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Threads a script started
//
// `allow_threads` is on - asyncio resolves names on a worker thread, and a
// stdlib without `threading` is not the stdlib - so a script can start threads
// in the isolate's interpreter, and `Py_EndInterpreter` must not run while one
// is alive: it is a fatal error ("not the last thread"), and for a
// `threading.Thread` CPython would first join it, forever if it never ends.
// So `~Isolate` stops them first and waits for them.
//
// They share the isolate's GIL, so they run only while the isolate's thread
// runs Python or sits in a blocking call; idle, the isolate holds the GIL and
// they wait. Stopping one is the ordinary stop, delivered to that thread: the
// pending call and the monitoring events fire on whichever thread checks.
// ---------------------------------------------------------------------------

namespace {

/// How long `~Isolate` waits for the threads a script started to end once
/// they have been told to stop. Stopped Python ends at its next check; this is
/// for a thread inside a blocking call - `time.sleep`, a socket read - which
/// sees the stop only when the call returns.
constexpr std::chrono::milliseconds THREAD_GRACE{2000};

[[nodiscard]] bool OtherThreads(Isolate& isolate) noexcept {
    const Isolate::Impl& impl = isolate.impl();
    // The list changes only under the runtime's own lock, and a thread
    // leaves it while holding the GIL - which this thread holds.
    for (PyThreadState* ts = PyInterpreterState_ThreadHead(impl.interp); ts != nullptr; ts = PyThreadState_Next(ts)) {
        if (ts != impl.tstate) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool StopScriptThreads(Isolate& isolate, bool wait) noexcept {
    if (!OtherThreads(isolate)) {
        return true;
    }
    RuntimeState* runtime = RuntimeOf(isolate);
    if (runtime == nullptr) {
        return false;
    }
    runtime->stopThreads.store(true, std::memory_order_release);
    Schedule(*runtime, /*evenIfClosed=*/true);
    SetMonitoring(*runtime, true);
    const auto deadline = std::chrono::steady_clock::now() + (wait ? THREAD_GRACE : std::chrono::milliseconds{0});
    bool alone = false;
    for (;;) {
        // The GIL goes for a moment each round: the threads need it to meet
        // the stop, and to finish.
        PyThreadState* self = PyEval_SaveThread();
        Sleep(1);
        PyEval_RestoreThread(self);
        alone = !OtherThreads(isolate);
        if (alone || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        Schedule(*runtime, /*evenIfClosed=*/true);
    }
    if (alone) {
        runtime->stopThreads.store(false, std::memory_order_release);
        if (!Terminating(isolate) || runtime->closed.load(std::memory_order_acquire)) {
            SetMonitoring(*runtime, false);
        }
    }
    return alone;
}

namespace {

/// Once no thread a script started is left: forget the stop that was for
/// them.
///
/// `Py_EndInterpreter` joins every non-daemon `threading.Thread` that is left,
/// and every one has ended by now - this runs only once the thread ending the
/// interpreter is its last. Under 3.12 the join went through a Python lock per
/// thread in `threading._shutdown_locks`, which a stop inside `Thread.join()`
/// could leave acquired, and this emptied the list; from 3.13 a thread is
/// joined through its C thread handle, which a stop cannot leave half-done.
void FinishWithScriptThreads(RuntimeState* runtime) noexcept {
    if (runtime != nullptr) {
        runtime->stopThreads.store(false, std::memory_order_release);
        Py_CLEAR(runtime->threadStop);
    }
}

}  // namespace

void ReleaseScriptThreadStop(Isolate& isolate) noexcept {
    FinishWithScriptThreads(RuntimeOf(isolate));
}

RuntimeState* AbandonRuntime(Isolate& isolate) noexcept {
    // Queued pending calls and the monitoring callback of the interpreter
    // being left behind point at this state; it stays, stopping whatever
    // those threads run once their blocking calls return.
    return isolate.impl().runtime.release();
}

bool EndAbandonedInterpreter(PyThreadState* tstate, RuntimeState* runtime) noexcept {
    PyEval_RestoreThread(tstate);
    bool alone = true;
    for (PyThreadState* ts = PyInterpreterState_ThreadHead(PyThreadState_GetInterpreter(tstate)); ts != nullptr;
         ts = PyThreadState_Next(ts)) {
        if (ts != tstate) {
            alone = false;
        }
    }
    if (!alone) {
        (void)PyEval_SaveThread();
        return false;
    }
    FinishWithScriptThreads(runtime);
    Py_EndInterpreter(tstate);
    DestroyRuntimeState(runtime);
    return true;
}

// ---------------------------------------------------------------------------
// Promises, from C++
// ---------------------------------------------------------------------------

PyObject* SpawnCoroutine(Isolate& isolate, PyObject* coroutine) noexcept {
    RuntimeState* runtime = RuntimeOf(isolate);
    if (runtime == nullptr || runtime->loop == nullptr) {
        // Closed rather than dropped, so that it does not also warn that it
        // was never awaited.
        PyObject* closed = PyObject_CallMethod(coroutine, "close", nullptr);
        Py_XDECREF(closed);
        Py_DECREF(coroutine);
        PyErr_SetString(PyExc_RuntimeError, "unibind: asyncio is not available, so there are no promises");
        return nullptr;
    }
    // A task starts at the next pump, not here: asyncio steps a new task from
    // the loop, and the loop runs only in `PumpJobs`.
    PyObject* task = PyObject_CallMethod(runtime->loop, "create_task", "O", coroutine);
    if (task == nullptr) {
        PyObject* pending = PyErr_GetRaisedException();
        PyObject* closed = PyObject_CallMethod(coroutine, "close", nullptr);
        Py_XDECREF(closed);
        PyErr_Clear();
        PyErr_SetRaisedException(pending);
    }
    Py_DECREF(coroutine);
    return task;
}

std::optional<Slot> MakePromise(const Context& context) {
    Isolate& isolate = OwnerOf(context);
    RuntimeState* runtime = RuntimeOf(isolate);
    if (Terminating(isolate) || runtime == nullptr || runtime->loop == nullptr) {
        return std::nullopt;
    }
    // `create_future` is C in the loop's base class when `_asyncio` is
    // there, but it is a method a subclass may override; keep what it might
    // raise away from the embedder's handler, as `Promise::New` says only
    // "empty".
    PyObject* future = PyObject_CallMethod(runtime->loop, "create_future", nullptr);
    if (future == nullptr) {
        PyErr_Clear();
        return std::nullopt;
    }
    return PushOrNothing(isolate, future);
}

namespace {

std::optional<bool> SettleFromEmbedder(const Context& context, Slot promise, Slot value, bool reject) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    RuntimeState* runtime = RuntimeOf(isolate);
    if (!gate.Open() || runtime == nullptr || runtime->loop == nullptr) {
        return std::nullopt;
    }
    PyObject* future = Resolve(promise);
    if (!IsFuture(*runtime, future)) {
        PyErr_SetString(PyExc_TypeError, "not a promise: an asyncio.Future was expected");
        return std::nullopt;
    }
    PyObject* answer = nullptr;
    if (reject) {
        // A reason may be any value; a future holds only an exception. So a
        // value that is not one travels wrapped in `unibind.Thrown`, which
        // `await` raises and a `TryCatch` unwraps back into the value - the
        // same wrapper `ub::Throw` uses. `StopIteration` is wrapped too:
        // a future refuses it, because a coroutine cannot raise it.
        PyObject* reason = Resolve(value);
        PyObject* error = nullptr;
        if (PyExceptionInstance_Check(reason) && !PyErr_GivenExceptionMatches(reason, PyExc_StopIteration)) {
            error = Py_NewRef(reason);
        } else {
            error = PyObject_CallOneArg(isolate.impl().types.thrown, reason);
        }
        if (error == nullptr) {
            return std::nullopt;
        }
        answer = PyObject_CallFunctionObjArgs(Helper(*runtime, "reject"), future, error, nullptr);
        Py_DECREF(error);
    } else {
        answer = PyObject_CallFunctionObjArgs(Helper(*runtime, "resolve"), runtime->loop, future, Resolve(value),
                                              nullptr);
    }
    if (answer == nullptr) {
        return std::nullopt;
    }
    const bool settled = answer == Py_True;
    Py_DECREF(answer);
    return settled;
}

}  // namespace

std::optional<bool> ResolvePromise(const Context& context, Slot promise, Slot value) {
    return SettleFromEmbedder(context, promise, value, false);
}

std::optional<bool> RejectPromise(const Context& context, Slot promise, Slot reason) {
    return SettleFromEmbedder(context, promise, reason, true);
}

PromiseState PromiseStateOf(Slot promise) noexcept {
    if (promise.IsEmpty()) {
        return PromiseState::Pending;
    }
    Isolate& isolate = IsolateFor(promise);
    RuntimeState* runtime = RuntimeOf(isolate);
    PyObject* future = Resolve(promise);
    if (runtime == nullptr || !IsFuture(*runtime, future)) {
        return PromiseState::Pending;
    }
    // Read through the future's own fields, not `done()` and `exception()`:
    // on asyncio's C future these are getters that run no Python, and this is
    // `noexcept` and may be asked while a stop is in force. A cancelled
    // future is a rejected one - awaiting it raises `CancelledError`.
    PyObject* pending = PyErr_GetRaisedException();
    PromiseState state = PromiseState::Pending;
    if (PyObject* text = PyObject_GetAttrString(future, "_state"); text != nullptr) {
        if (PyUnicode_Check(text) != 0 && PyUnicode_CompareWithASCIIString(text, "CANCELLED") == 0) {
            state = PromiseState::Rejected;
        } else if (PyUnicode_Check(text) != 0 && PyUnicode_CompareWithASCIIString(text, "FINISHED") == 0) {
            PyObject* error = PyObject_GetAttrString(future, "_exception");
            state = error != nullptr && error != Py_None ? PromiseState::Rejected : PromiseState::Fulfilled;
            Py_XDECREF(error);
        }
        Py_DECREF(text);
    }
    PyErr_Clear();
    if (pending != nullptr) {
        PyErr_SetRaisedException(pending);
    }
    return state;
}

}  // namespace ub::detail

namespace ub {

using detail::RuntimeState;

HeapStatistics Isolate::GetHeapStatistics() const noexcept {
    const RuntimeState* runtime = impl_->runtime.get();
    const detail::HeapAccount* account = runtime == nullptr ? nullptr : runtime->account;
    if (account == nullptr) {
        return HeapStatistics{.limitBytes = detail::SystemMemoryCeiling()};
    }
    const std::uint64_t used = account->used.load(std::memory_order_relaxed);
    const std::uint64_t peak = std::max<std::uint64_t>(account->peak.load(std::memory_order_relaxed), used);
    const std::size_t limit = account->limit.load(std::memory_order_relaxed);
    HeapStatistics stats;
    stats.usedBytes = used;
    // CPython's object allocator reserves arenas, but per interpreter and
    // with no public way to ask for the figure; what this heap holds is what
    // is charged to it, so that is the reservation too.
    stats.totalBytes = used;
    stats.limitBytes = limit != 0 ? std::max<std::uint64_t>(limit, used) : detail::SystemMemoryCeiling();
    // Everything charged here came through `malloc`-shaped hooks, so it is
    // exactly the "allocated for itself" figure, and the peak is exact.
    stats.mallocedBytes = used;
    stats.peakMallocedBytes = peak;
    return stats;
}

void Isolate::RequestGarbageCollection() noexcept {
    // Keep whatever is pending: a collection is not a call and must not eat
    // an exception the embedder has not looked at yet.
    PyObject* pending = PyErr_GetRaisedException();
    (void)PyGC_Collect();
    PyErr_Clear();
    if (pending != nullptr) {
        PyErr_SetRaisedException(pending);
    }
}

void Isolate::SetHeapLimitCallback(HeapLimitCallback callback, CallbackData data) noexcept {
    RuntimeState* runtime = impl_->runtime.get();
    if (runtime == nullptr || runtime->account == nullptr) {
        return;
    }
    runtime->account->callback = callback;
    runtime->account->data = data;
    runtime->account->crossed.store(false, std::memory_order_relaxed);
}

void Isolate::TerminateExecution() noexcept {
    // The flag first, then the pending call that makes running code look at
    // it. `Service` reads the flag when it runs, so a cancel that lands in
    // between leaves it nothing to do; flag-first means a gate on the
    // isolate's thread refuses from this instant.
    impl_->terminating.store(true, std::memory_order_release);
    if (RuntimeState* runtime = impl_->runtime.get(); runtime != nullptr) {
        detail::Schedule(*runtime);
    }
}

bool Isolate::IsExecutionTerminating() const noexcept {
    return impl_->terminating.load(std::memory_order_acquire);
}

void Isolate::CancelTerminateExecution() noexcept {
    impl_->terminating.store(false, std::memory_order_release);
    RuntimeState* runtime = impl_->runtime.get();
    if (runtime == nullptr) {
        return;
    }
    detail::SetMonitoring(*runtime, false);
    // Interrupts that waited out the stop run at the first check after this.
    // The re-armed pending call is usually still queued and will do it; this
    // makes sure of it.
    bool waiting = false;
    {
        const std::lock_guard<std::mutex> lock(runtime->interruptMutex);
        waiting = !runtime->interrupts.empty();
    }
    if (waiting) {
        detail::Schedule(*runtime);
    }
}

bool Isolate::RequestInterrupt(InterruptCallback callback, CallbackData data) noexcept {
    RuntimeState* runtime = impl_->runtime.get();
    if (callback == nullptr || runtime == nullptr || runtime->closed.load(std::memory_order_acquire)) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(runtime->interruptMutex);
        // Growing the queue can run out of memory, and this is noexcept:
        // refused, not `std::terminate`.
        try {
            runtime->interrupts.push_back({.callback = callback, .data = data});
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    detail::Schedule(*runtime);
    return true;
}

bool Isolate::PostJob(JobCallback callback, CallbackData data) noexcept {
    RuntimeState* runtime = impl_->runtime.get();
    if (callback == nullptr || runtime == nullptr || runtime->closed.load(std::memory_order_acquire)) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(runtime->jobMutex);
    // Never coalesced: the same callback posted twice runs twice.
    try {
        runtime->jobs.push_back({.callback = callback, .data = data});
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
    RuntimeState* runtime = impl_->runtime.get();
    if (callback == nullptr || runtime == nullptr || runtime->closed.load(std::memory_order_acquire)) {
        return false;
    }
    // Past what the steady clock can count the conversion would overflow into
    // the past, and a delay nothing will outlive would run at the next pump.
    // Half the room left is the cut-off, clear of rounding; beyond it the job
    // is due never.
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> delay(delayInSeconds);
    const std::chrono::duration<double> room(std::chrono::steady_clock::time_point::max() - now);
    const auto due = delay < room / 2 ? now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay)
                                      : std::chrono::steady_clock::time_point::max();
    const std::lock_guard<std::mutex> lock(runtime->jobMutex);
    try {
        // A multimap keeps equal keys in insertion order: two jobs due at the
        // same moment run in the order they were posted.
        runtime->delayedJobs.emplace(due, detail::RuntimePostedJob{.callback = callback, .data = data});
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void Isolate::PumpJobs() {
    RuntimeState* runtime = impl_->runtime.get();
    if (runtime == nullptr || detail::Terminating(*this)) {
        // Nothing runs while a stop is in force, and the queues wait for the
        // cancel.
        return;
    }
    // A pump is not a call: nothing it runs may hand an exception to a
    // `TryCatch` the embedder opened around it. Put back after.
    detail::TryCatchState* handler = std::exchange(impl_->tryCatch, nullptr);
    PyObject* pending = PyErr_GetRaisedException();

    for (;;) {
        // Continuations first, then one piece of posted work, then round
        // again - an event loop's microtask checkpoint after every task - so
        // posted work that settles a promise sees its continuations run before
        // the next piece of posted work does.
        if (!detail::DrainLoop(*this, *runtime)) {
            break;
        }
        detail::RuntimePostedJob job;
        {
            const std::lock_guard<std::mutex> lock(runtime->jobMutex);
            // Whatever has fallen due joins the back of the queue, in the
            // order it fell due, and runs in this pump.
            const auto now = std::chrono::steady_clock::now();
            auto& delayed = runtime->delayedJobs;
            while (!delayed.empty() && delayed.begin()->first <= now) {
                try {
                    runtime->jobs.push_back(delayed.begin()->second);
                } catch (const std::bad_alloc&) {
                    break;  // it stays delayed, and joins at a later pump
                }
                delayed.erase(delayed.begin());
            }
            if (runtime->jobs.empty()) {
                break;
            }
            job = runtime->jobs.front();
            runtime->jobs.pop_front();
        }
        // Not under the lock: a job may post more work. No realm entered: the
        // job opens its own, as `unibind/isolate.h` says.
        detail::ContextRec* realm = std::exchange(impl_->entered, nullptr);
        job.callback(*this, job.data);
        impl_->entered = realm;
        if (PyErr_Occurred() != nullptr) {
            PyErr_Clear();
        }
        if (detail::Terminating(*this)) {
            // A stop landed in the job. The posted work behind it waits for
            // the cancel; the continuations it queued do not - discarded, as
            // `unibind/isolate.h` promises and V8 does.
            if (runtime->ready != nullptr) {
                detail::DiscardReady(*runtime);
            }
            break;
        }
    }

    impl_->tryCatch = handler;
    if (pending != nullptr) {
        PyErr_SetRaisedException(pending);
    }
}

}  // namespace ub
