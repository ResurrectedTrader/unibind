/// \file
/// The bindings: one small piece of C++ per feature of the binding API, each
/// reachable from the REPL. Read it top to bottom as a tour; `InstallHost` at
/// the bottom is where each piece is declared and put where script can see it.
///
/// Conventions every callback here follows, because the API asks for them:
///
///   * **An empty optional means "it threw, or could not be made"**, and the
///     callback returns at once without calling further into the engine. The
///     exception is already pending and reaches the Python that called us.
///   * `info.GetContext()` is the realm to make values in and read through -
///     not a context captured earlier.
///   * Python's `None` arrives as `undefined`, so an argument that was left
///     out and one passed as `None` are the same thing, and both take the
///     default.

#include "host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>
#include <numeric>
#include <print>
#include <thread>
#include <utility>
#include <vector>

namespace repl {

namespace {

// --- small conveniences ------------------------------------------------------------

/// The host a callback was declared with. Every `host.*` function carries it as
/// its `CallbackData`; the typed class callbacks, which have no data slot of
/// their own, find it through the isolate's embedder pointer instead - both are
/// shown, and both are checked: the wrong type comes back as null.
Host* HostOf(const ub::CallbackContextBase& info) {
    if (Host* host = info.Data<Host>()) {
        return host;
    }
    return info.GetIsolate().GetEmbedderData<Host>();
}

/// A string handle, or empty with nothing pending if the engine could not make one.
std::optional<ub::Local<ub::String>> Str(ub::Isolate& isolate, std::string_view utf8) {
    return ub::String::NewFromUtf8(isolate, utf8);
}

/// The UTF-8 text of any value - `str()` of it, for Python. Empty if the
/// conversion threw.
std::optional<std::string> TextOf(const ub::Context& context, const ub::Local<ub::Value>& value) {
    const auto text = value.ToString(context);
    if (!text) {
        return std::nullopt;
    }
    return text->Utf8Value();
}

/// Argument `index` as a number, or `fallback` when it was left out (or None).
std::optional<double> NumberArg(const ub::CallbackInfo& info, std::uint32_t index, double fallback) {
    if (info[index].IsUndefined()) {
        return fallback;
    }
    return info[index].ToNumber(info.GetContext());
}

std::optional<std::int32_t> IntArg(const ub::CallbackInfo& info, std::uint32_t index, std::int32_t fallback) {
    if (info[index].IsUndefined()) {
        return fallback;
    }
    return info[index].ToInt32(info.GetContext());
}

/// Sets `object[key] = value`. False if it threw.
template <class T>
bool Put(const ub::Context& context, const ub::Local<ub::Object>& object, std::string_view key,
         const ub::Local<T>& value) {
    return object.Set(context, key, value).value_or(false);
}

bool PutText(const ub::Context& context, const ub::Local<ub::Object>& object, std::string_view key,
             std::string_view text) {
    const auto string = Str(context.GetIsolate(), text);
    return string && Put(context, object, key, *string);
}

/// The key of a named interceptor call as text, or empty for a symbol key.
std::optional<std::string> KeyOf(const ub::Local<ub::Name>& property) {
    if (const auto string = property.To<ub::String>()) {
        return string->Utf8Value();
    }
    return std::nullopt;
}

// =================================================================================
// host.*: plain functions on an ObjectTemplate
// =================================================================================

/// `host.log(*args)`: the classic first binding. Each argument is converted the
/// way `str()` would, which may run script - a `__str__` - and so may throw.
void Log(const ub::CallbackInfo& info) {
    std::string line;
    for (std::uint32_t i = 0; i < info.Length(); ++i) {
        const auto text = TextOf(info.GetContext(), info[i]);
        if (!text) {
            return;  // the conversion threw; it is pending and reaches the caller
        }
        line += (i == 0 ? "" : " ") + *text;
    }
    std::println("[host.log] {}", line);
    std::fflush(stdout);
}

/// `host.now()`: seconds since the host was installed, as a float.
void Now(const ub::CallbackInfo& info) {
    const Host* host = HostOf(info);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - host->started;
    info.GetReturnValue().Set(elapsed.count());
}

/// `host.throw_type_error(message)`: native code throwing. The TypeError is
/// made when this returns, and Python sees an ordinary `TypeError`.
void ThrowTypeError(const ub::CallbackInfo& info) {
    const auto message = TextOf(info.GetContext(), info[0]);
    if (!message) {
        return;
    }
    info.ThrowTypeError(*message);
}

/// `host.sleep(seconds)`: a native that blocks - the one thing a watchdog
/// cannot interrupt (`Isolate::TerminateExecution` reaches script, not C++).
/// So it polls `IsExecutionTerminating()` and returns early by itself, which
/// is what every native that can block for long enough to matter has to do.
/// True if it slept the whole time, False if it was told to stop.
void Sleep(const ub::CallbackInfo& info) {
    const auto seconds = NumberArg(info, 0, 0.0);
    if (!seconds) {
        return;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(0.0, *seconds));
    while (std::chrono::steady_clock::now() < until) {
        if (info.GetIsolate().IsExecutionTerminating()) {
            info.GetReturnValue().Set(false);
            return;  // and call nothing else in the engine on the way out
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    info.GetReturnValue().Set(true);
}

/// `host.map(items, fn)`: native calling back into Python. `fn` is any Python
/// callable - a lambda, a builtin, a bound method - and each call can throw,
/// in which case the exception is already pending and we stop.
void Map(const ub::CallbackInfo& info) {
    const ub::Context& context = info.GetContext();
    const auto items = info[0].To<ub::Array>();  // a Python list is an Array
    const auto fn = info[1].To<ub::Function>();  // and any callable a Function
    if (!items || !fn) {
        info.ThrowTypeError("map(items, fn) expects a list and a callable");
        return;
    }
    const std::uint32_t length = items->Length();
    const auto out = ub::Array::New(context, length);
    if (!out) {
        return;
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        const auto item = items->Get(context, i);
        if (!item) {
            return;
        }
        const std::array<ub::Local<ub::Value>, 1> arguments{*item};
        const auto mapped = fn->Call(context, ub::Undefined(info.GetIsolate()), arguments);
        if (!mapped || !out->Set(context, i, *mapped).value_or(false)) {
            return;  // fn raised: let it reach the caller unchanged
        }
    }
    info.GetReturnValue().Set(*out);
}

/// `host.make_greeter(greeting)`: `Function::New` with a *script value* as its
/// data. One native callback, `Greet`, serves every greeter; each function
/// made here carries its own greeting, and the engine keeps it alive exactly
/// as long as the function.
void Greet(const ub::CallbackInfo& info) {
    const auto greeting = TextOf(info.GetContext(), info.Data());
    const auto name = info[0].IsUndefined() ? std::optional<std::string>("world") : TextOf(info.GetContext(), info[0]);
    if (!greeting || !name) {
        return;
    }
    (void)info.GetReturnValue().Set(std::format("{}, {}!", *greeting, *name));
}

void MakeGreeter(const ub::CallbackInfo& info) {
    const auto greeting = info[0].ToString(info.GetContext());
    if (!greeting) {
        return;
    }
    if (const auto greeter = ub::Function::New(info.GetContext(), &Greet, *greeting)) {
        info.GetReturnValue().Set(*greeter);
    }
}

// --- binary data and structured clone ----------------------------------------------

/// `host.bytes(n)` is a `Uint8Array` of 0, 1, ... n-1; `host.bytes(text)` is
/// the UTF-8 encoding of `text`. The bytes are copied into the engine.
void Bytes(const ub::CallbackInfo& info) {
    std::vector<std::uint8_t> bytes;
    if (const auto text = info[0].To<ub::String>()) {
        const std::string utf8 = text->Utf8Value();
        bytes.assign(utf8.begin(), utf8.end());
    } else {
        const auto count = IntArg(info, 0, 0);
        if (!count) {
            return;
        }
        if (*count < 0 || *count > (1 << 20)) {
            info.Throw(ub::ErrorKind::RangeError, "bytes(n) wants 0 <= n <= 1048576");
            return;
        }
        bytes.resize(static_cast<std::size_t>(*count));
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(i);
        }
    }
    if (const auto view = ub::TypedArray::New(info.GetContext(), std::span<const std::uint8_t>(bytes))) {
        info.GetReturnValue().Set(*view);
    }
}

/// `host.checksum(view)`: the other direction - copy the bytes of any typed
/// array or DataView out, and add them up.
void Checksum(const ub::CallbackInfo& info) {
    const auto view = info[0].To<ub::ArrayBufferView>();
    if (!view) {
        info.ThrowTypeError("checksum() expects a TypedArray or a DataView");
        return;
    }
    std::vector<std::byte> bytes(ub::ByteLength(*view));
    bytes.resize(ub::CopyBytes(*view, bytes));
    std::uint32_t sum = 0;
    for (const std::byte b : bytes) {
        sum += static_cast<std::uint32_t>(b);
    }
    info.GetReturnValue().Set(sum);
}

/// `host.clone(value)`: write `value` down with the structured-clone algorithm
/// and build it again - the way a value moves to another isolate, done here in
/// one realm so the result can be compared with the original. A function or a
/// class instance will not clone; `Serialize` then fails with a DataCloneError
/// pending, which is what the caller sees.
void Clone(const ub::CallbackInfo& info) {
    const auto blob = ub::Serialize(info.GetContext(), info[0]);
    if (!blob) {
        return;
    }
    if (const auto copy = ub::Deserialize(info.GetContext(), *blob)) {
        info.GetReturnValue().Set(*copy);
    }
}

// --- the stack, the heap, and the collector ------------------------------------------

/// `host.stack()`: who called us - `CaptureStackFrames`, innermost first, as a
/// list of `{function, script, line}` objects.
void Stack(const ub::CallbackInfo& info) {
    const ub::Context& context = info.GetContext();
    const std::vector<ub::StackFrame> frames = ub::CaptureStackFrames(info.GetIsolate(), 16);
    const auto list = ub::Array::New(context, 0);
    if (!list) {
        return;
    }
    for (std::uint32_t i = 0; i < frames.size(); ++i) {
        const auto frame = ub::Object::New(context);
        if (!frame || !PutText(context, *frame, "function", frames[i].functionName) ||
            !PutText(context, *frame, "script", frames[i].scriptName) ||
            !Put(context, *frame, "line", ub::Integer::New(info.GetIsolate(), frames[i].lineNumber)) ||
            !list->Set(context, i, *frame).value_or(false)) {
            return;
        }
    }
    info.GetReturnValue().Set(*list);
}

/// `host.heap()`: `GetHeapStatistics` as a Python dict. The figures one engine
/// reports and another does not are optional, and left out rather than zero.
void Heap(const ub::CallbackInfo& info) {
    Host* host = HostOf(info);
    const ub::Context& context = info.GetContext();
    ub::Isolate& isolate = info.GetIsolate();
    // A native constructing a script type: `dict()`, through `NewInstance`.
    const auto dict = host->dictType.Get(isolate).NewInstance(context);
    if (!dict) {
        return;
    }
    const ub::HeapStatistics stats = isolate.GetHeapStatistics();
    const auto put = [&](std::string_view key, std::optional<std::uint64_t> value) {
        return !value || Put(context, *dict, key, ub::Number::New(isolate, static_cast<double>(*value)));
    };
    if (put("used_bytes", stats.usedBytes) && put("total_bytes", stats.totalBytes) &&
        put("limit_bytes", stats.limitBytes) && put("physical_bytes", stats.physicalBytes) &&
        put("external_bytes", stats.externalBytes) && put("malloced_bytes", stats.mallocedBytes) &&
        put("peak_malloced_bytes", stats.peakMallocedBytes)) {
        info.GetReturnValue().Set(*dict);
    }
}

/// `host.gc()`: ask for a full collection, and answer with the bytes in use
/// afterwards. The engine may treat it as a hint.
void Gc(const ub::CallbackInfo& info) {
    info.GetIsolate().RequestGarbageCollection();
    info.GetReturnValue().Set(info.GetIsolate().GetHeapStatistics().usedBytes);
}

// --- jobs: timers and promises --------------------------------------------------------
//
// Both are *posted work*: `PostJob`/`PostDelayedJob` queue a callback that runs
// on the isolate's thread at the next `PumpJobs` - which the REPL calls after
// every input and while it waits at the prompt. A job is not inside any call,
// so it opens its own `HandleScope`, enters a realm, and catches what it runs.

/// Runs a `host.set_timeout` callback. The job owns the timer from here on:
/// taking it out of the table first means a callback that clears itself, or
/// sets a new timer, sees a consistent table.
void FireTimer(ub::Isolate& isolate, ub::CallbackData data) {
    auto* timer = data.As<Host::Timer>();
    if (timer == nullptr) {
        return;
    }
    Host& host = *timer->host;
    auto owned = host.timers.extract(timer->id);
    if (owned.empty() || owned.mapped()->cancelled) {
        return;
    }
    const ub::HandleScope scope(isolate);
    const ub::ContextScope entered(host.context);
    const ub::TryCatch caught(isolate);
    const ub::Local<ub::Function> callback = owned.mapped()->callback.Get(isolate);
    if (!callback.Call(host.context, ub::Undefined(isolate)) && caught.HasCaught() && !caught.HasTerminated()) {
        ReportUncaught(host.context, caught, "a set_timeout callback");
    }
}

/// `host.set_timeout(fn, seconds)`: call `fn()` once, no sooner than `seconds`
/// from now, at a pump. Answers an id for `clear_timeout`.
void SetTimeout(const ub::CallbackInfo& info) {
    Host* host = HostOf(info);
    const auto callback = info[0].To<ub::Function>();
    const auto seconds = NumberArg(info, 1, 0.0);
    if (!callback) {
        info.ThrowTypeError("set_timeout(fn, seconds) expects a callable");
        return;
    }
    if (!seconds) {
        return;
    }
    // A `Global` keeps the function alive after this call's frame is gone.
    auto timer = std::make_unique<Host::Timer>();
    timer->host = host;
    timer->id = host->nextId++;
    timer->callback = ub::Global<ub::Function>(info.GetIsolate(), *callback);
    Host::Timer& queued = *timer;
    host->timers.emplace(queued.id, std::move(timer));
    if (!info.GetIsolate().PostDelayedJob(&FireTimer, ub::CallbackData::For(queued), *seconds)) {
        host->timers.erase(queued.id);
        info.Throw(ub::ErrorKind::Error, "set_timeout: the isolate refused the job");
        return;
    }
    info.GetReturnValue().Set(queued.id);
}

/// `host.clear_timeout(id)`: True if the timer had not fired yet. The job is
/// already queued and cannot be taken back, so it is marked, and does nothing
/// when it runs - and the callback is let go now rather than then.
void ClearTimeout(const ub::CallbackInfo& info) {
    Host* host = HostOf(info);
    const auto id = info[0].ToUint32(info.GetContext());
    if (!id) {
        return;
    }
    const auto found = host->timers.find(*id);
    const bool live = found != host->timers.end() && !found->second->cancelled;
    if (live) {
        found->second->cancelled = true;
        found->second->callback.Reset();
    }
    info.GetReturnValue().Set(live);
}

/// Settles a `host.fetch_later` promise. Its continuations - the `await` that
/// is waiting on it - run in the same pump, before the next posted job.
void SettleLater(ub::Isolate& isolate, ub::CallbackData data) {
    auto* pending = data.As<Host::Pending>();
    if (pending == nullptr) {
        return;
    }
    Host& host = *pending->host;
    auto owned = host.pending.extract(pending->id);
    if (owned.empty()) {
        return;
    }
    const ub::HandleScope scope(isolate);
    const ub::ContextScope entered(host.context);
    const ub::TryCatch caught(isolate);
    const ub::Local<ub::Promise> promise = owned.mapped()->promise.Get(isolate);
    const ub::Local<ub::Value> value = owned.mapped()->value.Get(isolate);
    const auto settled =
        owned.mapped()->reject ? ub::Reject(host.context, promise, value) : ub::Resolve(host.context, promise, value);
    if (!settled && caught.HasCaught() && !caught.HasTerminated()) {
        ReportUncaught(host.context, caught, "settling a fetch_later promise");
    }
}

/// `host.fetch_later(value, seconds)` / `host.fail_later(reason, seconds)`: a
/// promise - an `asyncio.Future` on this backend - that native settles from a
/// posted job, the way an embedder settles one when its I/O completes. So
/// `await host.fetch_later(42)` works, top-level await included.
void Later(const ub::CallbackInfo& info, bool reject) {
    Host* host = HostOf(info);
    const auto seconds = NumberArg(info, 1, 0.0);
    if (!seconds) {
        return;
    }
    const auto promise = ub::Promise::New(info.GetContext());
    if (!promise) {
        if (!info.GetIsolate().HasPendingException()) {
            info.Throw(ub::ErrorKind::Error, "this build has no promises");
        }
        return;
    }
    auto pending = std::make_unique<Host::Pending>();
    pending->host = host;
    pending->id = host->nextId++;
    pending->promise = ub::Global<ub::Promise>(info.GetIsolate(), *promise);
    pending->value = ub::Global<ub::Value>(info.GetIsolate(), info[0]);
    pending->reject = reject;
    Host::Pending& queued = *pending;
    host->pending.emplace(queued.id, std::move(pending));
    if (!info.GetIsolate().PostDelayedJob(&SettleLater, ub::CallbackData::For(queued), *seconds)) {
        host->pending.erase(queued.id);
        info.Throw(ub::ErrorKind::Error, "fetch_later: the isolate refused the job");
        return;
    }
    info.GetReturnValue().Set(*promise);
}

void FetchLater(const ub::CallbackInfo& info) {
    Later(info, false);
}
void FailLater(const ub::CallbackInfo& info) {
    Later(info, true);
}

// =================================================================================
// host.session: accessors on a plain object (`Object::SetAccessor`)
// =================================================================================

/// `host.session.inputs`: how many inputs the REPL has evaluated. Read-only -
/// there is no setter, so Python assigning it is a TypeError.
void ReadInputs(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(HostOf(info)->inputs);
}

/// `host.session.prompt`: the REPL's primary prompt, readable and writable.
void ReadPrompt(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    (void)info.GetReturnValue().Set(HostOf(info)->prompt);
}

void WritePrompt(const ub::Local<ub::Name>& /*property*/, const ub::Local<ub::Value>& value,
                 const ub::PropertyCallbackInfo& info) {
    if (const auto text = TextOf(info.GetContext(), value)) {
        HostOf(info)->prompt = *text;
    }
}

// =================================================================================
// host.env: a named interceptor over a C++ map
// =================================================================================
//
// Five hooks make the map look like an object's own properties from Python:
// attribute and subscript reads (getter), writes (setter), `in` (query), `del`
// (deleter), and `dir()`/`list()`/`len()` (enumerator). Each returns "not
// intercepted" for keys it does not know, and the ordinary lookup carries on.

ub::Intercepted EnvGet(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const Host* host = HostOf(info);
    const auto key = KeyOf(property);
    const auto found = key ? host->env.find(*key) : host->env.end();
    if (found == host->env.end()) {
        return ub::Intercepted::No;
    }
    (void)info.GetReturnValue().Set(found->second.value);
    return ub::Intercepted::Yes;
}

ub::Intercepted EnvSet(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                       const ub::PropertyCallbackInfo& info) {
    Host* host = HostOf(info);
    const auto key = KeyOf(property);
    if (!key) {
        return ub::Intercepted::No;  // a symbol key: an ordinary property
    }
    const auto found = host->env.find(*key);
    if (found != host->env.end() && found->second.locked) {
        info.ThrowTypeError(std::format("env.{} is locked", *key));
        return ub::Intercepted::Yes;  // a hook that throws has intercepted
    }
    const auto text = TextOf(info.GetContext(), value);
    if (!text) {
        return ub::Intercepted::Yes;  // str(value) threw; that is the answer
    }
    host->env[*key].value = *text;
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> EnvQuery(const ub::Local<ub::Name>& property,
                                              const ub::PropertyCallbackInfo& info) {
    const Host* host = HostOf(info);
    const auto key = KeyOf(property);
    const auto found = key ? host->env.find(*key) : host->env.end();
    if (found == host->env.end()) {
        return std::nullopt;
    }
    return found->second.locked ? ub::PropertyAttribute::ReadOnly | ub::PropertyAttribute::DontDelete
                                : ub::PropertyAttribute::None;
}

std::optional<bool> EnvDelete(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Host* host = HostOf(info);
    const auto key = KeyOf(property);
    const auto found = key ? host->env.find(*key) : host->env.end();
    if (found == host->env.end()) {
        return std::nullopt;
    }
    if (found->second.locked) {
        return false;  // a refused delete: Python's `del` raises TypeError
    }
    host->env.erase(found);
    return true;
}

std::optional<ub::Local<ub::Array>> EnvKeys(const ub::PropertyCallbackInfo& info) {
    const Host* host = HostOf(info);
    const auto keys = ub::Array::New(info.GetContext(), 0);
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t at = 0;
    for (const auto& entry : host->env) {
        const auto name = Str(info.GetIsolate(), entry.first);
        if (!name || !keys->Set(info.GetContext(), at++, *name).value_or(false)) {
            return std::nullopt;
        }
    }
    return keys;
}

// =================================================================================
// host.registers: an indexed interceptor - a virtual array of eight integers
// =================================================================================

ub::Intercepted RegisterGet(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    const Host* host = HostOf(info);
    if (index >= host->registers.size()) {
        return ub::Intercepted::No;
    }
    info.GetReturnValue().Set(host->registers[index]);
    return ub::Intercepted::Yes;
}

ub::Intercepted RegisterSet(std::uint32_t index, const ub::Local<ub::Value>& value,
                            const ub::PropertyCallbackInfo& info) {
    Host* host = HostOf(info);
    if (index >= host->registers.size()) {
        info.Throw(ub::ErrorKind::RangeError, std::format("there are only {} registers", host->registers.size()));
        return ub::Intercepted::Yes;
    }
    if (const auto number = value.ToInt32(info.GetContext())) {
        host->registers[index] = *number;
    }
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> RegisterQuery(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    if (index >= HostOf(info)->registers.size()) {
        return std::nullopt;
    }
    return ub::PropertyAttribute::DontDelete;
}

/// `del host.registers[i]` clears the register rather than removing it.
std::optional<bool> RegisterDelete(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    Host* host = HostOf(info);
    if (index >= host->registers.size()) {
        return std::nullopt;
    }
    host->registers[index] = 0;
    return true;
}

std::optional<ub::Local<ub::Array>> RegisterKeys(const ub::PropertyCallbackInfo& info) {
    const auto keys = ub::Array::New(info.GetContext(), 0);
    if (!keys) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < HostOf(info)->registers.size(); ++i) {
        if (!keys->Set(info.GetContext(), i, ub::Integer::NewFromUnsigned(info.GetIsolate(), i)).value_or(false)) {
            return std::nullopt;
        }
    }
    return keys;
}

// =================================================================================
// Counter: a Class<T> - constructable, methods, accessors, a static, an
// iterator, and instances made from C++ that share their native
// =================================================================================

/// `Counter(start=0, step=1)`. Returning null after throwing refuses the
/// construction; no instance without a native ever reaches script.
std::unique_ptr<Counter> NewCounter(const ub::CallbackInfo& info) {
    const auto start = IntArg(info, 0, 0);
    const auto step = IntArg(info, 1, 1);
    if (!start || !step) {
        return nullptr;
    }
    if (*step <= 0) {
        info.Throw(ub::ErrorKind::RangeError, "a counter's step must be positive");
        return nullptr;
    }
    return std::make_unique<Counter>(*start, *step);
}

/// `c.increment(by=c.step)` - answers the new value.
void CounterIncrement(Counter& self, const ub::CallbackInfo& info) {
    const auto by = IntArg(info, 0, self.step);
    if (!by) {
        return;
    }
    self.value += *by;
    info.GetReturnValue().Set(self.value);
}

void CounterReset(Counter& self, const ub::CallbackInfo& /*info*/) {
    self.value = 0;
}

void CounterValue(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void CounterSetValue(Counter& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    if (const auto number = value.ToInt32(info.GetContext())) {
        self.value = *number;
    }
}

void CounterStep(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.step);
}

void CounterSetStep(Counter& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    const auto number = value.ToInt32(info.GetContext());
    if (!number) {
        return;
    }
    if (*number <= 0) {
        info.Throw(ub::ErrorKind::RangeError, "a counter's step must be positive");
        return;
    }
    self.step = *number;
}

/// `Counter.alive()`: a static takes a plain callback - there is no instance.
void CounterAlive(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(Counter::alive);
}

/// `c[Symbol.iterator]()`, which is what `for x in c` calls: a fresh iterator
/// object, itself a class instance whose native is its position. The backend
/// adapts its JavaScript-style `next()` into Python's iteration protocol.
void CounterIterate(Counter& self, const ub::CallbackInfo& info) {
    const Host* host = HostOf(info);
    if (!host->countUpClass) {
        return;
    }
    const auto iterator =
        host->countUpClass->Wrap(info.GetContext(), std::make_shared<CountUp>(CountUp{.next = 0, .end = self.value}));
    if (iterator) {
        info.GetReturnValue().Set(*iterator);
    }
}

/// `next()`, answering `{done, value}` as the iterator protocol asks.
void CountUpNext(CountUp& self, const ub::CallbackInfo& info) {
    const ub::Context& context = info.GetContext();
    const auto result = ub::Object::New(context);
    if (!result) {
        return;
    }
    const bool done = self.next >= self.end;
    if (!Put(context, *result, "done", ub::Boolean::New(info.GetIsolate(), done)) ||
        !Put(context, *result, "value", ub::Integer::New(info.GetIsolate(), done ? 0 : self.next++))) {
        return;
    }
    info.GetReturnValue().Set(*result);
}

/// `host.shared_counter(name)`: a wrapper over a native the *host* also owns.
/// Every call for the same name wraps the same native again, so two wrappers
/// see each other's changes, and the native lives until the last share - the
/// registry's or any wrapper's - is gone.
void SharedCounter(const ub::CallbackInfo& info) {
    Host* host = HostOf(info);
    const auto name = TextOf(info.GetContext(), info[0]);
    if (!name || !host->counterClass) {
        return;
    }
    auto& share = host->registry[*name];
    if (share == nullptr) {
        share = std::make_shared<Counter>(0, 1);
    }
    if (const auto wrapper = host->counterClass->Wrap(info.GetContext(), share)) {
        info.GetReturnValue().Set(*wrapper);
    }
}

/// `host.registry()`: what the C++ side sees - each shared counter's value,
/// and how many owners it has (the registry itself, plus one per live wrapper).
void Registry(const ub::CallbackInfo& info) {
    Host* host = HostOf(info);
    const ub::Context& context = info.GetContext();
    ub::Isolate& isolate = info.GetIsolate();
    const auto dict = host->dictType.Get(isolate).NewInstance(context);
    if (!dict) {
        return;
    }
    for (const auto& [name, share] : host->registry) {
        const auto entry = ub::Object::New(context);
        if (!entry || !Put(context, *entry, "value", ub::Integer::New(isolate, share->value)) ||
            !Put(context, *entry, "owners", ub::Integer::New(isolate, static_cast<std::int32_t>(share.use_count()))) ||
            !Put(context, *dict, name, *entry)) {
            return;
        }
    }
    info.GetReturnValue().Set(*dict);
}

// =================================================================================
// Vec2: a Class<T> whose methods make new instances
// =================================================================================

std::unique_ptr<Vec2> NewVec2(const ub::CallbackInfo& info) {
    const auto x = NumberArg(info, 0, 0.0);
    const auto y = NumberArg(info, 1, 0.0);
    if (!x || !y) {
        return nullptr;
    }
    return std::make_unique<Vec2>(Vec2{.x = *x, .y = *y});
}

void Vec2X(Vec2& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.x);
}
void Vec2Y(Vec2& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.y);
}

/// Hands script a new Vec2. `Wrap` makes an instance without running the
/// script constructor.
void ReturnVec2(const ub::CallbackInfo& info, Vec2 value) {
    const Host* host = HostOf(info);
    if (!host->vec2Class) {
        return;
    }
    if (const auto made = host->vec2Class->Wrap(info.GetContext(), std::make_shared<Vec2>(value))) {
        info.GetReturnValue().Set(*made);
    }
}

/// The other operand, checked: `Class<T>::Unwrap` answers null for anything
/// that is not a Vec2 - never an unchecked cast.
const Vec2* OtherVec2(const ub::CallbackInfo& info, std::string_view method) {
    const Vec2* other = ub::Class<Vec2>::Unwrap(info[0]);
    if (other == nullptr) {
        info.ThrowTypeError(std::format("Vec2.{}() expects a Vec2", method));
    }
    return other;
}

void Vec2Add(Vec2& self, const ub::CallbackInfo& info) {
    if (const Vec2* other = OtherVec2(info, "add")) {
        ReturnVec2(info, {.x = self.x + other->x, .y = self.y + other->y});
    }
}

void Vec2Sub(Vec2& self, const ub::CallbackInfo& info) {
    if (const Vec2* other = OtherVec2(info, "sub")) {
        ReturnVec2(info, {.x = self.x - other->x, .y = self.y - other->y});
    }
}

void Vec2Dot(Vec2& self, const ub::CallbackInfo& info) {
    if (const Vec2* other = OtherVec2(info, "dot")) {
        info.GetReturnValue().Set((self.x * other->x) + (self.y * other->y));
    }
}

void Vec2Scale(Vec2& self, const ub::CallbackInfo& info) {
    if (const auto k = NumberArg(info, 0, 1.0)) {
        ReturnVec2(info, {.x = self.x * *k, .y = self.y * *k});
    }
}

void Vec2Length(Vec2& self, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(std::hypot(self.x, self.y));
}

void Vec2Describe(Vec2& self, const ub::CallbackInfo& info) {
    (void)info.GetReturnValue().Set(std::format("Vec2({}, {})", self.x, self.y));
}

// =================================================================================
// Shape, Circle, Rect: FunctionTemplates with Inherit and HasInstance
// =================================================================================
//
// No native state: an instance keeps its dimensions as ordinary properties,
// set by the constructor callback on `info.This()` - the instance being made.
// `Circle` and `Rect` inherit `Shape`, so their instances have Shape's
// `describe()`, and `describe()` calls `this.area()`, which each subtype
// defines on its own prototype.

void ConstructShape(const ub::CallbackInfo& info) {
    (void)PutText(info.GetContext(), info.This(), "name", "shape");
}

void ConstructCircle(const ub::CallbackInfo& info) {
    const auto radius = NumberArg(info, 0, 1.0);
    if (!radius) {
        return;
    }
    if (*radius < 0) {
        info.Throw(ub::ErrorKind::RangeError, "a circle's radius cannot be negative");
        return;
    }
    const ub::Context& context = info.GetContext();
    (void)(PutText(context, info.This(), "name", "circle") &&
           Put(context, info.This(), "radius", ub::Number::New(info.GetIsolate(), *radius)));
}

void ConstructRect(const ub::CallbackInfo& info) {
    const auto width = NumberArg(info, 0, 1.0);
    const auto height = NumberArg(info, 1, 1.0);
    if (!width || !height) {
        return;
    }
    const ub::Context& context = info.GetContext();
    (void)(PutText(context, info.This(), "name", "rect") &&
           Put(context, info.This(), "width", ub::Number::New(info.GetIsolate(), *width)) &&
           Put(context, info.This(), "height", ub::Number::New(info.GetIsolate(), *height)));
}

/// Reads a numeric property of the receiver.
std::optional<double> NumberProperty(const ub::CallbackInfo& info, std::string_view key) {
    const auto value = info.This().Get(info.GetContext(), key);
    return value ? value->ToNumber(info.GetContext()) : std::nullopt;
}

void ShapeArea(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(0.0);
}

void CircleArea(const ub::CallbackInfo& info) {
    if (const auto r = NumberProperty(info, "radius")) {
        info.GetReturnValue().Set(std::numbers::pi * *r * *r);
    }
}

void RectArea(const ub::CallbackInfo& info) {
    const auto w = NumberProperty(info, "width");
    const auto h = w ? NumberProperty(info, "height") : std::nullopt;
    if (h) {
        info.GetReturnValue().Set(*w * *h);
    }
}

/// `shape.describe()`: a method on the parent's prototype calling one that
/// the child's prototype overrides - dynamic dispatch through script.
void ShapeDescribe(const ub::CallbackInfo& info) {
    const ub::Context& context = info.GetContext();
    const auto name = info.This().Get(context, "name");
    const auto areaMethod = info.This().Get(context, "area");
    if (!name || !areaMethod) {
        return;
    }
    const auto area = areaMethod->To<ub::Function>();
    if (!area) {
        info.ThrowTypeError("this shape has no area()");
        return;
    }
    const auto measured = area->Call(context, info.This());
    const auto text = TextOf(context, *name);
    const auto number = measured ? measured->ToNumber(context) : std::nullopt;
    if (text && number) {
        (void)info.GetReturnValue().Set(std::format("{} with area {:.2f}", *text, *number));
    }
}

/// `host.kind_of(value)`: which of the three templates claim `value`, asked
/// with `FunctionTemplate::HasInstance`. It asks what *made* the object, not
/// its prototype chain - and a Circle is a Shape too, through `Inherit`.
void KindOf(const ub::CallbackInfo& info) {
    const Host* host = HostOf(info);
    const ub::Context& context = info.GetContext();
    const auto kinds = ub::Array::New(context, 0);
    if (!kinds) {
        return;
    }
    std::uint32_t at = 0;
    for (const auto& [name, type] : {std::pair{"Circle", &host->circleType}, std::pair{"Rect", &host->rectType},
                                     std::pair{"Shape", &host->shapeType}}) {
        if (!*type) {
            return;
        }
        const auto is = (*type)->HasInstance(context, info[0]);
        if (!is) {
            return;
        }
        if (*is) {
            const auto string = Str(info.GetIsolate(), name);
            if (!string || !kinds->Set(context, at++, *string).value_or(false)) {
                return;
            }
        }
    }
    info.GetReturnValue().Set(*kinds);
}

// --- what :bindings prints ---------------------------------------------------------------

constexpr BindingDoc DOCS[] = {
    {"host.version, host.backend", "constants on the host ObjectTemplate"},
    {"host.log(*args)", "print its arguments' str(), from C++"},
    {"host.now()", "seconds since start, as a float"},
    {"host.sleep(seconds)", "a blocking native that polls IsExecutionTerminating"},
    {"host.map(items, fn)", "C++ calling a Python callable for each item (Function::Call)"},
    {"host.make_greeter(greeting)", "a function carrying a script value as its data"},
    {"host.bytes(n | text)", "a Uint8Array made in C++ (TypedArray::New)"},
    {"host.checksum(view)", "sum of a typed array's bytes, copied out (CopyBytes)"},
    {"host.clone(value)", "structured clone: Serialize, then Deserialize"},
    {"host.stack()", "CaptureStackFrames as a list of {function, script, line}"},
    {"host.heap(), host.gc()", "heap statistics as a dict; ask for a collection"},
    {"host.throw_type_error(msg)", "native code throwing a TypeError"},
    {"host.set_timeout(fn, s)", "call fn() after s seconds (PostDelayedJob + Global<Function>)"},
    {"host.clear_timeout(id)", "cancel a timer that has not fired"},
    {"host.fetch_later(v, s)", "a Promise (asyncio.Future) resolved with v from a posted job"},
    {"host.fail_later(e, s)", "the same, rejected with e"},
    {"host.shared_counter(name)", "a Counter co-owned with a C++ registry (Class<T>::Wrap)"},
    {"host.registry()", "the C++ registry's view: value and owner count per counter"},
    {"host.kind_of(value)", "which of Circle/Rect/Shape claim it (FunctionTemplate::HasInstance)"},
    {"host.env", "a named interceptor over a C++ map: get, set, in, del, dir()"},
    {"host.registers", "an indexed interceptor: eight integers"},
    {"host.session", "a plain object with accessors: inputs (read-only), prompt"},
    {"Counter(start, step)", "Class<Counter>: increment(), reset(), value, step, alive(), iteration"},
    {"Vec2(x, y)", "Class<Vec2>: x, y, add(), sub(), dot(), scale(), length(), describe()"},
    {"Shape, Circle(r), Rect(w, h)", "FunctionTemplates; Circle and Rect Inherit Shape"},
};

}  // namespace

// ==========================================================================================
// Declaring it all, and putting it where script can see it
// ==========================================================================================

Host::Host(ub::Isolate& isolate, ub::Context context) : isolate(isolate), context(std::move(context)) {
    env["app"] = {.value = "unibind_python_repl", .locked = true};
    env["mode"] = {.value = "interactive"};
    env["greeting"] = {.value = "hello"};
}

Host::~Host() {
    // Unhook the embedder pointer: an isolate outliving this must not hand a
    // callback a dangling Host.
    isolate.StoreEmbedderData({});
}

std::size_t Host::Outstanding() const noexcept {
    return pending.size() + static_cast<std::size_t>(std::ranges::count_if(
                                timers, [](const auto& entry) { return !entry.second->cancelled; }));
}

bool InstallHost(Host& host) {
    ub::Isolate& isolate = host.isolate;
    const ub::Context& context = host.context;
    const ub::HandleScope scope(isolate);
    const ub::ContextScope entered(context);
    const ub::TryCatch caught(isolate);
    const auto fail = [&caught, &context](std::string_view what) {
        std::println(stderr, "could not install the host: {} ({})", what,
                     caught.HasCaught() ? caught.Message(context).value_or("?") : "nothing thrown");
        return false;
    };

    // The typed class callbacks have no data slot; they find the host here.
    isolate.SetEmbedderData(host);
    const ub::CallbackData data = ub::CallbackData::For(host);

    // --- classes -------------------------------------------------------------------
    //
    // Shape first, then use: a class or template is fixed at its first
    // instantiation (unibind/template.h), so everything is declared before
    // anything is instantiated below.

    const auto counter = ub::Class<Counter>::New(isolate, "Counter");
    counter.Construct<&NewCounter>();
    counter.Method<&CounterIncrement>("increment");
    counter.Method<&CounterReset>("reset");
    counter.Accessor<&CounterValue, &CounterSetValue>("value");
    counter.Accessor<&CounterStep, &CounterSetStep>("step");
    counter.StaticMethod("alive", &CounterAlive);
    counter.StaticValue("MAX_STEP", ub::Constant(std::int32_t{1000}));
    counter.SymbolMethod<&CounterIterate>(ub::WellKnownSymbol::Iterator);
    host.counterClass = counter;

    // No `Construct`: script cannot make one, only get one from `iter(counter)`.
    const auto countUp = ub::Class<CountUp>::New(isolate, "CounterIterator");
    countUp.Method<&CountUpNext>("next");
    host.countUpClass = countUp;

    const auto vec2 = ub::Class<Vec2>::New(isolate, "Vec2");
    vec2.Construct<&NewVec2>();
    vec2.Accessor<&Vec2X>("x");
    vec2.Accessor<&Vec2Y>("y");
    vec2.Method<&Vec2Add>("add");
    vec2.Method<&Vec2Sub>("sub");
    vec2.Method<&Vec2Dot>("dot");
    vec2.Method<&Vec2Scale>("scale");
    vec2.Method<&Vec2Length>("length");
    vec2.Method<&Vec2Describe>("describe");
    host.vec2Class = vec2;

    // --- function templates with inheritance ---------------------------------------

    const auto shape = ub::FunctionTemplate::New(isolate, &ConstructShape);
    shape.SetClassName("Shape");
    shape.PrototypeTemplate().Set("area", &ShapeArea);
    shape.PrototypeTemplate().Set("describe", &ShapeDescribe);
    shape.Set("SIDES_OF_A_CIRCLE", ub::Constant(std::int32_t{0}));

    const auto circle = ub::FunctionTemplate::New(isolate, &ConstructCircle);
    circle.SetClassName("Circle");
    circle.Inherit(shape);
    circle.PrototypeTemplate().Set("area", &CircleArea);

    const auto rect = ub::FunctionTemplate::New(isolate, &ConstructRect);
    rect.SetClassName("Rect");
    rect.Inherit(shape);
    rect.PrototypeTemplate().Set("area", &RectArea);

    host.shapeType = shape;
    host.circleType = circle;
    host.rectType = rect;

    // --- the interceptor objects ---------------------------------------------------

    const auto env = ub::ObjectTemplate::New(isolate);
    env.SetHandler(ub::NamedPropertyHandler{.getter = &EnvGet,
                                            .setter = &EnvSet,
                                            .query = &EnvQuery,
                                            .deleter = &EnvDelete,
                                            .enumerator = &EnvKeys,
                                            .data = data});

    const auto registers = ub::ObjectTemplate::New(isolate);
    registers.SetHandler(ub::IndexedPropertyHandler{.getter = &RegisterGet,
                                                    .setter = &RegisterSet,
                                                    .query = &RegisterQuery,
                                                    .deleter = &RegisterDelete,
                                                    .enumerator = &RegisterKeys,
                                                    .data = data});

    // --- host: an ObjectTemplate of constants, functions and nested objects ---------

    const auto hostShape = ub::ObjectTemplate::New(isolate);
    hostShape.Set("version", ub::Constant("unibind_python_repl 1.0"), ub::PropertyAttribute::ReadOnly);
    hostShape.Set("backend", ub::Constant(ub::Platform::BackendName()), ub::PropertyAttribute::ReadOnly);
    hostShape.Set("log", &Log, data);
    hostShape.Set("now", &Now, data);
    hostShape.Set("sleep", &Sleep, data);
    hostShape.Set("map", &Map, data);
    hostShape.Set("make_greeter", &MakeGreeter, data);
    hostShape.Set("bytes", &Bytes, data);
    hostShape.Set("checksum", &Checksum, data);
    hostShape.Set("clone", &Clone, data);
    hostShape.Set("stack", &Stack, data);
    hostShape.Set("heap", &Heap, data);
    hostShape.Set("gc", &Gc, data);
    hostShape.Set("throw_type_error", &ThrowTypeError, data);
    hostShape.Set("set_timeout", &SetTimeout, data);
    hostShape.Set("clear_timeout", &ClearTimeout, data);
    hostShape.Set("fetch_later", &FetchLater, data);
    hostShape.Set("fail_later", &FailLater, data);
    hostShape.Set("shared_counter", &SharedCounter, data);
    hostShape.Set("registry", &Registry, data);
    hostShape.Set("kind_of", &KindOf, data);
    // Nested templates become nested objects, made along with the host.
    hostShape.Set("env", env, ub::PropertyAttribute::ReadOnly);
    hostShape.Set("registers", registers, ub::PropertyAttribute::ReadOnly);

    // --- instantiate ----------------------------------------------------------------

    const auto hostObject = hostShape.NewInstance(context);
    if (!hostObject) {
        return fail("the host object");
    }

    // host.session: a *plain* object, with accessors added to it after the
    // fact - `Object::SetAccessor`, for an object no template made.
    const auto session = ub::Object::New(context);
    if (!session || !session->SetAccessor(context, "inputs", &ReadInputs, nullptr, data).value_or(false) ||
        !session->SetAccessor(context, "prompt", &ReadPrompt, &WritePrompt, data).value_or(false) ||
        !Put(context, *hostObject, "session", *session)) {
        return fail("host.session");
    }

    // Python's own `dict`, kept so that callbacks can make real dicts.
    const auto dictType = ub::Evaluate(context, "dict");
    const auto dictFunction = dictType ? dictType->To<ub::Function>() : std::nullopt;
    if (!dictFunction) {
        return fail("the dict type");
    }
    host.dictType = ub::Global<ub::Function>(isolate, *dictFunction);

    // Types become globals: a class's constructor is a Python type.
    const auto global = context.GlobalObject();
    const auto counterType = counter.GetConstructor(context);
    const auto vec2Type = vec2.GetConstructor(context);
    const auto shapeType = shape.GetFunction(context);
    const auto circleType = circle.GetFunction(context);
    const auto rectType = rect.GetFunction(context);
    if (!counterType || !vec2Type || !shapeType || !circleType || !rectType) {
        return fail("the types");
    }
    if (!Put(context, global, "host", *hostObject) || !Put(context, global, "Counter", *counterType) ||
        !Put(context, global, "Vec2", *vec2Type) || !Put(context, global, "Shape", *shapeType) ||
        !Put(context, global, "Circle", *circleType) || !Put(context, global, "Rect", *rectType)) {
        return fail("the globals");
    }
    return true;
}

std::span<const BindingDoc> BindingDocs() noexcept {
    return DOCS;
}

std::string ExceptionText(const ub::Context& context, const ub::TryCatch& caught) {
    // The traceback is Python's own text - what a human reads. Without one
    // (a value thrown from native code, say) the message is all there is.
    const auto trace = caught.StackTrace(context);
    std::string text = trace && !trace->empty() ? *trace : caught.Message(context).value_or("<no message>");
    while (text.ends_with('\n')) {
        text.pop_back();
    }
    return text;
}

void ReportUncaught(const ub::Context& context, const ub::TryCatch& caught, std::string_view where) {
    const std::string text = ExceptionText(context, caught);
    std::fflush(stdout);
    std::println(stderr, "Exception in {}:\n{}", where, text);
    std::fflush(stderr);
}

}  // namespace repl
