/// \file
/// An interactive Python prompt over unibind's CPython backend: every input is
/// a `ub::Evaluate` in one realm, whose globals hold the host bindings from
/// `host.cpp`. Also runs a file, a `-c` string, or the bundled `demo.py`.
///
/// Three threads, and exactly one of them touches the isolate:
///
///   * **The main thread** owns the isolate and does everything with it:
///     evaluates input, prints results, and pumps jobs - after every input,
///     and every few milliseconds while it waits for the next one, so that
///     `host.set_timeout` fires at the prompt.
///   * **A reader thread** blocks reading stdin, one line when asked for one.
///     Reading on a thread of its own is what lets the main thread keep
///     pumping while a human thinks.
///   * **The console's control thread** - Windows runs a Ctrl-C handler on a
///     thread of its own - and an optional **watchdog** (`--timeout`). Both
///     stop a running script with `Isolate::TerminateExecution`, the one call
///     the API allows from another thread; Ctrl-Break asks the running script
///     where it is with `RequestInterrupt`, the other one.
///
/// The main thread then calls `CancelTerminateExecution` back at the prompt:
/// a stopped isolate stays stopped until it is told otherwise.

#include <windows.h>

#include <crtdbg.h>
// for `_set_abort_behavior`, a CRT extension that the C header declares and <cstdlib> does not promise.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "host.h"
#include "unibind/unibind.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// --- output -----------------------------------------------------------------------
//
// Python's `print` and this program write to the same two file descriptors
// through two different buffers. Flushing after every write on this side, and
// line-buffering Python's (see `TOOLS_SOURCE`), is what keeps their output in
// the order it happened when both go to a pipe.

template <class... Args>
void Out(std::format_string<Args...> format, Args&&... args) {
    std::print(stdout, format, std::forward<Args>(args)...);
    std::fflush(stdout);
}

template <class... Args>
void Err(std::format_string<Args...> format, Args&&... args) {
    std::fflush(stdout);
    std::print(stderr, format, std::forward<Args>(args)...);
    std::fflush(stderr);
}

// =====================================================================================
// Stopping a script from another thread
// =====================================================================================

enum class StopReason { None, Interrupt, Timeout };

/// What the other threads - the console's Ctrl-C handler and the watchdog -
/// know about the main thread: whether it is running Python right now, and
/// what to do about it.
///
/// The one rule it exists to keep: **`TerminateExecution` only while Python is
/// running.** A stop requested while idle is remembered and would kill the
/// *next* input, so the check and the stop happen under one lock, and the main
/// thread takes the same lock to say it has finished - after which no stop can
/// arrive for the run that just ended.
class Supervisor {
   public:
    void Attach(ub::Isolate* isolate) {
        const std::scoped_lock lock(mutex_);
        isolate_ = isolate;
    }

    void SetTimeout(std::optional<double> seconds) {
        const std::scoped_lock lock(mutex_);
        timeout_ = seconds;
        StartWatchdogLocked();
    }
    [[nodiscard]] std::optional<double> Timeout() {
        const std::scoped_lock lock(mutex_);
        return timeout_;
    }

    /// The main thread is about to run Python.
    void BeginRun() {
        const std::scoped_lock lock(mutex_);
        running_ = true;
        reason_ = StopReason::None;
        ++generation_;
        if (timeout_) {
            deadline_ =
                Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(*timeout_));
        }
        wake_.notify_all();
    }

    /// The main thread has finished; answers why it was stopped, if it was.
    StopReason EndRun() {
        const std::scoped_lock lock(mutex_);
        running_ = false;
        return std::exchange(reason_, StopReason::None);
    }

    /// Ctrl-C: stop the script if one is running, otherwise remember it for
    /// the prompt (the reader thread sees its read cut short, too).
    void Interrupt() {
        const std::scoped_lock lock(mutex_);
        if (!StopLocked(StopReason::Interrupt)) {
            idleInterrupt_ = true;
        }
    }

    /// Ctrl-Break: ask the running script where it is, without stopping it.
    void Where() {
        const std::scoped_lock lock(mutex_);
        if (running_ && isolate_ != nullptr) {
            (void)isolate_->RequestInterrupt(&PrintWhere, {});
        }
    }

    /// Was Ctrl-C pressed while no script ran? Clears it.
    [[nodiscard]] bool TakeIdleInterrupt() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(idleInterrupt_, false);
    }

    ~Supervisor() {
        watchdog_.request_stop();
        wake_.notify_all();
    }

   private:
    /// Requires the lock. True if a running script was told to stop.
    bool StopLocked(StopReason reason) {
        if (!running_ || isolate_ == nullptr) {
            return false;
        }
        if (reason_ == StopReason::None) {
            reason_ = reason;
            isolate_->TerminateExecution();
        }
        return true;
    }

    /// An interrupt callback: on the isolate's thread, between two bytecodes
    /// of whatever was running. It may read - capture a stack - and must not
    /// run script, which is all it does.
    static void PrintWhere(ub::Isolate& isolate, ub::CallbackData /*data*/) {
        const std::vector<ub::StackFrame> frames = ub::CaptureStackFrames(isolate, 8);
        std::string text = "\n[Ctrl-Break] the script is running at:\n";
        for (const ub::StackFrame& frame : frames) {
            text += std::format("  {} ({}:{})\n", frame.functionName.empty() ? "<module>" : frame.functionName,
                                frame.scriptName, frame.lineNumber);
        }
        Err("{}", text);
    }

    /// The watchdog: sleeps until a run's deadline, and stops the run if it
    /// is still the same one when the deadline comes.
    void StartWatchdogLocked() {
        if (!timeout_ || watchdog_.joinable()) {
            return;
        }
        watchdog_ = std::jthread([this](const std::stop_token& stop) {
            std::unique_lock lock(mutex_);
            while (!stop.stop_requested()) {
                if (!running_ || !timeout_) {
                    wake_.wait(lock, stop, [this] { return running_ && timeout_.has_value(); });
                    continue;
                }
                const std::uint64_t watching = generation_;
                const bool ended = wake_.wait_until(lock, stop, deadline_, [this, watching] {
                    return !running_ || generation_ != watching || !timeout_;
                });
                if (!ended && !stop.stop_requested() && running_ && generation_ == watching) {
                    (void)StopLocked(StopReason::Timeout);
                    // Wait for this run to end before watching the next one.
                    wake_.wait(lock, stop, [this, watching] { return !running_ || generation_ != watching; });
                }
            }
        });
    }

    std::mutex mutex_;
    std::condition_variable_any wake_;
    ub::Isolate* isolate_ = nullptr;
    bool running_ = false;
    bool idleInterrupt_ = false;
    StopReason reason_ = StopReason::None;
    std::uint64_t generation_ = 0;
    std::optional<double> timeout_;
    Clock::time_point deadline_;
    std::jthread watchdog_;
};

/// Static, so it is alive for as long as the console can call the handler.
Supervisor gSupervisor;

BOOL WINAPI OnConsoleControl(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
            gSupervisor.Interrupt();
            return TRUE;  // handled: do not end the process
        case CTRL_BREAK_EVENT:
            gSupervisor.Where();
            return TRUE;
        default:
            return FALSE;  // closing the window, logging off: the default
    }
}

// =====================================================================================
// Reading lines on a thread of their own
// =====================================================================================

/// One line of input, or the reason there is none.
struct InputEvent {
    enum class Kind { Line, Interrupt, Eof };
    Kind kind = Kind::Eof;
    std::string text;
};

/// Reads stdin one line at a time, and only when asked - so that it is never
/// blocked in a read while a script runs, where Ctrl-C would cut the read
/// short and look like input.
class LineReader {
   public:
    LineReader() : console_(IsConsole()) {
        thread_ = std::jthread([this](const std::stop_token& stop) { Loop(stop); });
    }

    [[nodiscard]] bool Interactive() const noexcept { return console_; }

    /// Ask for the next line; `Poll` delivers it.
    void Request() {
        const std::scoped_lock lock(mutex_);
        if (!requested_ && !ready_) {
            requested_ = true;
            wake_.notify_all();
        }
    }

    /// The line asked for, if it has arrived within `wait`.
    [[nodiscard]] std::optional<InputEvent> Poll(std::chrono::milliseconds wait) {
        std::unique_lock lock(mutex_);
        if (!wake_.wait_for(lock, wait, [this] { return ready_.has_value(); })) {
            return std::nullopt;
        }
        return std::exchange(ready_, std::nullopt);
    }

   private:
    static bool IsConsole() {
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
    }

    void Loop(const std::stop_token& stop) {
        while (true) {
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [this] { return requested_; })) {
                    return;
                }
            }
            InputEvent event = console_ ? ReadConsoleLine() : ReadPipedLine();
            const std::scoped_lock lock(mutex_);
            requested_ = false;
            ready_ = std::move(event);
            wake_.notify_all();
        }
    }

    /// From a console: `ReadConsoleW`, which is what tells Ctrl-C (the read is
    /// aborted) apart from end of input (Ctrl-Z Enter), and reads Unicode.
    static InputEvent ReadConsoleLine() {
        std::wstring line;
        wchar_t buffer[512];
        while (true) {
            DWORD read = 0;
            if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), buffer, 512, &read, nullptr)) {
                return {.kind = GetLastError() == ERROR_OPERATION_ABORTED ? InputEvent::Kind::Interrupt
                                                                          : InputEvent::Kind::Eof,
                        .text = {}};
            }
            if (read == 0) {
                return {.kind = GetLastError() == ERROR_OPERATION_ABORTED ? InputEvent::Kind::Interrupt
                                                                          : InputEvent::Kind::Eof,
                        .text = {}};
            }
            line.append(buffer, read);
            if (line.ends_with(L'\n')) {
                break;
            }
        }
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) {
            line.pop_back();
        }
        if (line.starts_with(L'\x1a')) {
            return {.kind = InputEvent::Kind::Eof, .text = {}};  // Ctrl-Z Enter
        }
        return {.kind = InputEvent::Kind::Line, .text = Narrow(line)};
    }

    /// From a pipe or a file: bytes, taken as UTF-8. A byte order mark at the
    /// very start - PowerShell writes one when it pipes text - is not input.
    InputEvent ReadPipedLine() {
        std::string line;
        if (!std::getline(std::cin, line)) {
            return {.kind = InputEvent::Kind::Eof, .text = {}};
        }
        if (std::exchange(firstLine_, false) && line.starts_with("\xEF\xBB\xBF")) {
            line.erase(0, 3);
        }
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        return {.kind = InputEvent::Kind::Line, .text = std::move(line)};
    }

   public:
    static std::string Narrow(std::wstring_view wide) {
        if (wide.empty()) {
            return {};
        }
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), size, nullptr,
                            nullptr);
        return narrow;
    }

   private:
    const bool console_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool requested_ = false;
    std::optional<InputEvent> ready_;
    bool firstLine_ = true;  // the reader thread's own
    std::jthread thread_;    // last: it starts in the constructor and uses the rest
};

// =====================================================================================
// The session: one realm for the user, one for the REPL's own helpers
// =====================================================================================

/// Pure Python the REPL needs and the API has no call for: deciding whether
/// input is a complete statement yet (`codeop`, exactly as Python's own REPL
/// does - with top-level await allowed, as the asyncio REPL does), reading a
/// `SystemExit`, setting `sys.argv`, and flushing Python's output.
///
/// It runs in a realm of its own, so none of it shows up in the user's
/// globals. A value may be used with any realm of its isolate, so its
/// functions are called on the user's values freely.
constexpr std::string_view TOOLS_SOURCE = R"PY(
import ast, codeop, sys

_compiler = codeop.CommandCompiler()
_compiler.compiler.flags |= ast.PyCF_ALLOW_TOP_LEVEL_AWAIT

def is_complete(source):
    try:
        return _compiler(source, "<stdin>", "single") is not None
    except (SyntaxError, ValueError, OverflowError):
        return True   # complete, and wrong: evaluating it reports the error

def exit_code(error):
    if not isinstance(error, SystemExit):
        return None
    if error.code is None:
        return 0
    if isinstance(error.code, int):
        return error.code
    print(error.code, file=sys.stderr)
    return 1

def set_argv(argv):
    sys.argv = list(argv)

def flush(*_):
    for stream in (sys.stdout, sys.stderr):
        if stream is not None:
            stream.flush()

for _stream in (sys.stdout, sys.stderr):
    if _stream is not None and hasattr(_stream, "reconfigure"):
        _stream.reconfigure(line_buffering=True)
)PY";

/// How one input ended.
struct Outcome {
    enum class Kind { Ok, Threw, Stopped, Exit };
    Kind kind = Kind::Ok;
    int exitCode = 0;
};

class Session {
   public:
    Session(ub::Isolate& isolate, ub::Context user, ub::Context tools, repl::Host& host)
        : isolate_(isolate), user_(std::move(user)), tools_(std::move(tools)), host_(host) {}

    /// Runs `TOOLS_SOURCE` in the tools realm and keeps what it defined.
    bool Init() {
        const ub::HandleScope scope(isolate_);
        const ub::ContextScope entered(tools_);
        ub::TryCatch caught(isolate_);
        if (!ub::Evaluate(tools_, TOOLS_SOURCE, {.resourceName = "<repl tools>"})) {
            Err("the REPL's helpers failed: {}\n", caught.Message(tools_).value_or("?"));
            return false;
        }
        const auto function = [&](std::string_view name, ub::Global<ub::Function>& into) {
            const auto value = tools_.GlobalObject().Get(tools_, name);
            const auto asFunction = value ? value->To<ub::Function>() : std::nullopt;
            if (asFunction) {
                into = ub::Global<ub::Function>(isolate_, *asFunction);
            }
            return asFunction.has_value();
        };
        // `repr` is a builtin: a Python callable is a `ub::Function` like any other.
        const auto repr = ub::Evaluate(tools_, "repr");
        const auto reprFunction = repr ? repr->To<ub::Function>() : std::nullopt;
        if (!reprFunction) {
            return false;
        }
        repr_ = ub::Global<ub::Function>(isolate_, *reprFunction);
        return function("is_complete", isComplete_) && function("exit_code", exitCode_) &&
               function("set_argv", setArgv_) && function("flush", flush_);
    }

    /// Is `source` a whole statement, or does the prompt say `... ` next?
    [[nodiscard]] bool IsComplete(std::string_view source) {
        const ub::HandleScope scope(isolate_);
        const auto text = ub::String::NewFromUtf8(isolate_, source);
        if (!text) {
            return true;
        }
        const auto answer = CallTool(isComplete_, *text);
        return !answer || answer->ToBoolean(tools_).value_or(true);
    }

    void SetArgv(std::span<const std::string> argv) {
        const ub::HandleScope scope(isolate_);
        const ub::ContextScope entered(tools_);
        const auto list = ub::Array::New(tools_, 0);
        if (!list) {
            return;
        }
        for (std::uint32_t i = 0; i < argv.size(); ++i) {
            const auto item = ub::String::NewFromUtf8(isolate_, argv[i]);
            if (!item || !list->Set(tools_, i, *item).value_or(false)) {
                return;
            }
        }
        (void)CallTool(setArgv_, *list);
    }

    /// Evaluate `source` in the user's realm. With `interactive`, print the
    /// result the way Python's REPL does and keep it in `_`.
    Outcome Execute(std::string_view source, std::string_view name, bool interactive) {
        Outcome outcome;
        {
            const ub::HandleScope scope(isolate_);
            const ub::ContextScope entered(user_);
            ub::TryCatch caught(isolate_);
            std::optional<ub::Local<ub::Value>> result;
            StopReason stopped = StopReason::None;
            {
                Running running;
                result = ub::Evaluate(user_, source, {.resourceName = name});
                stopped = running.End();
            }
            // Top-level `await` makes the whole input a coroutine: `Evaluate`
            // answers a promise - an asyncio Task - that the pump settles.
            if (result && result->IsPromise()) {
                result = Await(*result->To<ub::Promise>(), interactive, stopped);
            }
            if (!result) {
                outcome = Report(caught, stopped);
            } else if (interactive && !result->IsUndefined()) {
                // Python's REPL keeps the last result in `_`.
                (void)user_.GlobalObject().Set(user_, "_", *result);
                const auto text = Repr(*result);
                if (text) {
                    Out("{}\n", *text);
                }
            }
        }
        Recover();
        if (outcome.kind == Outcome::Kind::Ok) {
            Pump();  // timers and promises the input started
        }
        Flush();
        return outcome;
    }

    /// Run what is queued - promise continuations and posted jobs, timers
    /// whose time has come. Reports anything that stopped it.
    void Pump() {
        StopReason stopped = StopReason::None;
        {
            Running running;
            isolate_.PumpJobs();
            stopped = running.End();
        }
        if (stopped != StopReason::None || isolate_.IsExecutionTerminating()) {
            ReportStop(stopped);
        }
        Recover();
    }

    /// Pump for `seconds`, so that timers due in that window fire.
    void PumpFor(double seconds) {
        const auto until =
            Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        do {
            Pump();
            if (gSupervisor.TakeIdleInterrupt()) {
                Out("KeyboardInterrupt\n");
                return;
            }
            std::this_thread::sleep_for(5ms);
        } while (Clock::now() < until);
        Pump();
    }

    /// Until the host has no timer or promise left to settle - what a script
    /// run waits for before the program exits, as Node waits for its loop.
    bool Drain() {
        while (host_.Outstanding() > 0) {
            Pump();
            if (gSupervisor.TakeIdleInterrupt()) {
                Err("KeyboardInterrupt: {} timer(s)/promise(s) still pending\n", host_.Outstanding());
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    void PrintHeap() {
        const ub::HeapStatistics stats = isolate_.GetHeapStatistics();
        const auto optional = [](std::optional<std::uint64_t> value) {
            return value ? std::format("{}", *value) : std::string("n/a");
        };
        Out("used {} B, total {} B, limit {} B, external {}, malloced {} (peak {})\n", stats.usedBytes,
            stats.totalBytes, stats.limitBytes, optional(stats.externalBytes), optional(stats.mallocedBytes),
            optional(stats.peakMallocedBytes));
    }

    void Collect() {
        const std::uint64_t before = isolate_.GetHeapStatistics().usedBytes;
        isolate_.RequestGarbageCollection();
        const std::uint64_t after = isolate_.GetHeapStatistics().usedBytes;
        Out("collected: {} B -> {} B in use\n", before, after);
    }

    repl::Host& GetHost() { return host_; }

   private:
    /// Brackets everything that runs Python, so a stop from another thread
    /// lands only while something is running.
    class Running {
       public:
        Running() { gSupervisor.BeginRun(); }
        ~Running() {
            if (!ended_) {
                (void)gSupervisor.EndRun();
            }
        }
        Running(const Running&) = delete;
        Running& operator=(const Running&) = delete;
        Running(Running&&) = delete;
        Running& operator=(Running&&) = delete;

        StopReason End() {
            ended_ = true;
            return gSupervisor.EndRun();
        }

       private:
        bool ended_ = false;
    };

    /// A stop stays in force until it is cancelled, whatever caused it - and
    /// it is cancelled here, back at the top with no native frame left.
    void Recover() {
        if (isolate_.IsExecutionTerminating()) {
            isolate_.CancelTerminateExecution();
        }
    }

    /// Settle `promise`, pumping, and answer its value - or empty with its
    /// exception pending, as if the input itself had thrown.
    std::optional<ub::Local<ub::Value>> Await(const ub::Local<ub::Promise>& promise, bool interactive,
                                              StopReason& stopped) {
        (void)gSupervisor.TakeIdleInterrupt();
        bool announced = !interactive;  // a script says nothing while it waits
        while (true) {
            {
                Running running;
                isolate_.PumpJobs();
                stopped = running.End();
            }
            if (stopped != StopReason::None || isolate_.IsExecutionTerminating()) {
                return std::nullopt;
            }
            if (ub::GetState(promise) != ub::PromiseState::Pending) {
                break;
            }
            if (!announced) {
                Out("<pending>\n");
                announced = true;
            }
            if (gSupervisor.TakeIdleInterrupt()) {
                Out("KeyboardInterrupt (the promise is still pending; it settles at a later pump)\n");
                return ub::Undefined(isolate_);
            }
            std::this_thread::sleep_for(5ms);
        }
        // An asyncio Future: `result()` answers the value or raises the
        // exception - so a rejection is reported exactly as a throw.
        const auto result = promise.Get(user_, "result");
        const auto method = result ? result->To<ub::Function>() : std::nullopt;
        if (!method) {
            return std::nullopt;
        }
        Running running;
        auto value = method->Call(user_, promise);
        stopped = running.End();
        return value;
    }

    /// Says why an evaluation produced nothing. Everything is read out of the
    /// handler first, and then it is `Reset`: a handler holding an exception
    /// is a `catch` block still open, and asking whether it was a `SystemExit`
    /// calls back into the engine.
    Outcome Report(ub::TryCatch& caught, StopReason stopped) {
        if (stopped != StopReason::None || caught.HasTerminated()) {
            ReportStop(stopped);
            return {.kind = Outcome::Kind::Stopped};
        }
        if (!caught.HasCaught()) {
            Err("the evaluation failed without an exception\n");
            return {.kind = Outcome::Kind::Threw};
        }
        const ub::Local<ub::Value> exception = caught.Exception();
        const std::string text = repl::ExceptionText(user_, caught);
        caught.Reset();
        // `raise SystemExit(3)` - or `sys.exit(3)` - leaves the program.
        if (const auto code = CallTool(exitCode_, exception); code && !code->IsUndefined()) {
            return {.kind = Outcome::Kind::Exit, .exitCode = code->ToInt32(user_).value_or(1)};
        }
        Flush();
        Err("{}\n", text);
        return {.kind = Outcome::Kind::Threw};
    }

    static void ReportStop(StopReason reason) {
        if (reason == StopReason::Timeout) {
            Err("TimeoutError: stopped by the watchdog after {} s\n", gSupervisor.Timeout().value_or(0));
        } else {
            Err("KeyboardInterrupt (terminated)\n");
        }
    }

    /// `repr(value)`, as Python's REPL prints a result.
    std::optional<std::string> Repr(const ub::Local<ub::Value>& value) {
        const auto text = CallTool(repr_, value);
        if (!text) {
            return std::nullopt;
        }
        const auto string = text->To<ub::String>();
        return string ? std::optional(string->Utf8Value()) : std::nullopt;
    }

    /// Calls one of the helpers with one argument, reporting what it throws.
    template <class T>
    std::optional<ub::Local<ub::Value>> CallTool(const ub::Global<ub::Function>& tool, const ub::Local<T>& argument) {
        const ub::ContextScope entered(tools_);
        ub::TryCatch caught(isolate_);
        const std::array<ub::Local<ub::Value>, 1> arguments{argument};
        std::optional<ub::Local<ub::Value>> result;
        StopReason stopped = StopReason::None;
        {
            Running running;
            result = tool.Get(isolate_).Call(tools_, ub::Undefined(isolate_), arguments);
            stopped = running.End();
        }
        if (!result) {
            (void)Report(caught, stopped);
        }
        return result;
    }

    void Flush() {
        const ub::HandleScope scope(isolate_);
        (void)CallTool(flush_, ub::Undefined(isolate_));
        std::fflush(stdout);
        std::fflush(stderr);
    }

    ub::Isolate& isolate_;
    ub::Context user_;
    ub::Context tools_;
    repl::Host& host_;
    ub::Global<ub::Function> repr_;
    ub::Global<ub::Function> isComplete_;
    ub::Global<ub::Function> exitCode_;
    ub::Global<ub::Function> setArgv_;
    ub::Global<ub::Function> flush_;
};

// =====================================================================================
// The REPL
// =====================================================================================

constexpr std::string_view HELP =
    R"(Enter Python; a blank line ends a block. The last expression's repr is printed and kept in _.
Top-level await works:  await host.fetch_later(42)

  :help               this text
  :bindings           what the host registered (see also: dir(host))
  :load FILE          run a file in this session
  :pump [SECONDS]     run pending jobs - and keep pumping for SECONDS, so timers fire
  :heap               heap statistics
  :gc                 ask for a collection
  :timeout [S|off]    stop any single input that runs longer than S seconds
  :quit               leave (so do Ctrl-Z Enter and raise SystemExit)

Ctrl-C stops a running script (TerminateExecution) or clears the line at the prompt.
Ctrl-Break prints where a running script is (RequestInterrupt), without stopping it.
)";

std::optional<std::string> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string text = std::move(contents).str();
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);  // a UTF-8 byte order mark
    }
    return text;
}

std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

/// A `:command`. Answers an exit code if the REPL should end.
std::optional<int> Command(Session& session, std::string_view line) {
    const std::string_view command = Trim(line);
    const auto space = command.find(' ');
    const std::string_view verb = command.substr(0, space);
    const std::string_view argument = space == std::string_view::npos ? "" : Trim(command.substr(space));

    if (verb == ":quit" || verb == ":q" || verb == ":exit") {
        return 0;
    }
    if (verb == ":help") {
        Out("{}", HELP);
    } else if (verb == ":bindings") {
        for (const repl::BindingDoc& doc : repl::BindingDocs()) {
            Out("  {:<30} {}\n", doc.name, doc.summary);
        }
    } else if (verb == ":heap") {
        session.PrintHeap();
    } else if (verb == ":gc") {
        session.Collect();
    } else if (verb == ":pump") {
        double seconds = 0;
        if (!argument.empty()) {
            try {
                seconds = std::stod(std::string(argument));
            } catch (const std::exception&) {
                Err(":pump takes a number of seconds\n");
                return std::nullopt;
            }
        }
        session.PumpFor(seconds);
        Out("pumped; {} timer(s)/promise(s) outstanding\n", session.GetHost().Outstanding());
    } else if (verb == ":load") {
        const std::filesystem::path path(std::u8string(argument.begin(), argument.end()));
        const auto source = ReadWholeFile(path);
        if (!source) {
            Err("cannot read {}\n", argument);
            return std::nullopt;
        }
        const Outcome outcome = session.Execute(*source, argument, true);
        if (outcome.kind == Outcome::Kind::Exit) {
            return outcome.exitCode;
        }
    } else if (verb == ":timeout") {
        if (argument.empty()) {
            const auto timeout = gSupervisor.Timeout();
            Out("timeout: {}\n", timeout ? std::format("{} s", *timeout) : std::string("off"));
        } else if (argument == "off") {
            gSupervisor.SetTimeout(std::nullopt);
        } else {
            try {
                gSupervisor.SetTimeout(std::stod(std::string(argument)));
            } catch (const std::exception&) {
                Err(":timeout takes a number of seconds, or off\n");
            }
        }
    } else {
        Err("unknown command {} - try :help\n", verb);
    }
    return std::nullopt;
}

int Repl(Session& session) {
    LineReader reader;
    const bool echo = !reader.Interactive();  // piped: show the input, so the transcript reads like a session
    Out("Python {} on unibind ({}) - :help for help, :quit to leave\n", ub::Platform::BackendVersion(),
        ub::Platform::BackendName());

    std::string buffer;
    while (true) {
        Out("{}", buffer.empty() ? session.GetHost().prompt : std::string("... "));
        reader.Request();
        std::optional<InputEvent> event;
        // While a human thinks, keep the isolate's jobs moving: a timer set
        // at the prompt fires at the prompt.
        while (!(event = reader.Poll(20ms))) {
            session.Pump();
        }
        if (event->kind == InputEvent::Kind::Eof) {
            Out("\n");
            return 0;
        }
        if (event->kind == InputEvent::Kind::Interrupt) {
            (void)gSupervisor.TakeIdleInterrupt();
            Out("\nKeyboardInterrupt\n");
            buffer.clear();
            continue;
        }
        if (echo) {
            Out("{}\n", event->text);
        }
        if (buffer.empty()) {
            if (Trim(event->text).empty()) {
                continue;
            }
            if (Trim(event->text).starts_with(':')) {
                if (const auto code = Command(session, event->text)) {
                    return *code;
                }
                continue;
            }
            buffer = event->text;
        } else {
            buffer += "\n" + event->text;
        }
        if (!session.IsComplete(buffer)) {
            continue;
        }
        ++session.GetHost().inputs;
        const Outcome outcome = session.Execute(buffer, "<stdin>", true);
        buffer.clear();
        if (outcome.kind == Outcome::Kind::Exit) {
            return outcome.exitCode;
        }
    }
}

/// A file, a `-c` string or the demo: run it, wait for the timers and promises
/// it started, and answer an exit code.
int RunScript(Session& session, std::string_view source, std::string_view name) {
    const Outcome outcome = session.Execute(source, name, false);
    switch (outcome.kind) {
        case Outcome::Kind::Exit:
            return outcome.exitCode;
        case Outcome::Kind::Threw:
        case Outcome::Kind::Stopped:
            return 1;
        case Outcome::Kind::Ok:
            break;
    }
    return session.Drain() ? 0 : 1;
}

std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::filesystem::path(path).parent_path() : std::filesystem::path();
}

/// `demo.py` is copied next to the program when it is built; the source tree's
/// copy is the fallback for a program run from somewhere else.
std::optional<std::filesystem::path> FindDemo() {
    for (const std::filesystem::path& candidate :
         {ExecutableDirectory() / "demo.py", std::filesystem::path(UNIBIND_REPL_DEMO_SOURCE)}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

constexpr std::string_view USAGE = R"(usage: unibind_python_repl [--timeout SECONDS] [--demo | -c CODE | FILE [ARGS...]]

  (nothing)       an interactive prompt
  FILE [ARGS]     run a Python file; sys.argv is [FILE, ARGS...]
  -c CODE         run CODE
  --demo          run the bundled demo.py, which exercises every binding
  --timeout S     stop any single evaluation that runs longer than S seconds

Exits 0 on success, 1 if the script raised or was stopped, or the SystemExit code.
)";

struct Options {
    enum class Mode { Repl, File, Code, Demo, Help };
    Mode mode = Mode::Repl;
    std::optional<double> timeout;
    std::string code;               ///< -c
    std::vector<std::string> argv;  ///< sys.argv for a file
};

std::optional<Options> Parse(std::span<const std::string> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
            return options;
        }
        if (arg == "--timeout" && i + 1 < args.size()) {
            try {
                options.timeout = std::stod(args[++i]);
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--demo") {
            options.mode = Options::Mode::Demo;
        } else if (arg == "-c" && i + 1 < args.size()) {
            options.mode = Options::Mode::Code;
            options.code = args[++i];
            options.argv = {"-c"};
            options.argv.insert(options.argv.end(), args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            return options;
        } else if (arg.starts_with('-')) {
            return std::nullopt;
        } else {
            options.mode = Options::Mode::File;
            options.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
            return options;
        }
    }
    return options;
}

int Run(const Options& options) {
    // One per process, before the first isolate and outliving the last.
    const ub::Platform platform;
    if (!ub::Platform::IsInitialized()) {
        Err("the Python engine did not come up - is the standard library where the program can find it? "
            "See examples/python_repl/README.md.\n");
        return 1;
    }
    const auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        Err("could not make an isolate\n");
        return 1;
    }
    gSupervisor.Attach(isolate.get());
    gSupervisor.SetTimeout(options.timeout);

    int code = 0;
    {
        // Everything that names the isolate's state lives in this block and
        // goes before the isolate does: the realms, the host's Globals, the
        // session's.
        const ub::HandleScope scope(*isolate);
        auto user = ub::Context::New(*isolate);
        auto tools = ub::Context::New(*isolate);
        if (!user || !tools) {
            Err("could not make a realm\n");
            return 1;
        }
        repl::Host host(*isolate, *user);
        if (!repl::InstallHost(host)) {
            return 1;
        }
        Session session(*isolate, *user, *tools, host);
        if (!session.Init()) {
            return 1;
        }

        switch (options.mode) {
            case Options::Mode::Repl:
                host.env["mode"].value = "interactive";
                session.SetArgv(std::vector<std::string>{""});
                code = Repl(session);
                break;
            case Options::Mode::Code:
                host.env["mode"].value = "command";
                session.SetArgv(options.argv);
                code = RunScript(session, options.code, "<string>");
                break;
            case Options::Mode::File:
            case Options::Mode::Demo: {
                const auto path = options.mode == Options::Mode::Demo
                                      ? FindDemo()
                                      : std::optional(std::filesystem::path(
                                            std::u8string(options.argv.front().begin(), options.argv.front().end())));
                const auto source = path ? ReadWholeFile(*path) : std::nullopt;
                if (!source) {
                    Err("cannot read {}\n", path ? path->string() : std::string("demo.py"));
                    code = 1;
                    break;
                }
                host.env["mode"].value = options.mode == Options::Mode::Demo ? "demo" : "script";
                const std::string name = options.mode == Options::Mode::Demo ? "demo.py" : options.argv.front();
                session.SetArgv(options.mode == Options::Mode::Demo ? std::vector<std::string>{name} : options.argv);
                code = RunScript(session, *source, name);
                break;
            }
            case Options::Mode::Help:
                break;
        }
    }
    gSupervisor.Attach(nullptr);
    return code;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // UTF-8 for what this program prints; Python writes to a console in UTF-16
    // on its own.
    SetConsoleOutputCP(CP_UTF8);
    // A process can start with Ctrl-C ignored - inherited from a parent that
    // made a new process group, as some shells do. Undo that first: this
    // program's Ctrl-C is how a runaway script is stopped.
    SetConsoleCtrlHandler(nullptr, FALSE);
    SetConsoleCtrlHandler(&OnConsoleControl, TRUE);
    // A crash reports on stderr and ends the process, rather than opening a
    // dialog box: this program also runs unattended, under CTest and in
    // scripts, where a box nobody clicks is a hang.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        (void)kind;  // the two calls below are nothing in a release CRT
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(LineReader::Narrow(argv[i]));
    }
    const auto options = Parse(args);
    if (!options || options->mode == Options::Mode::Help) {
        Out("{}", USAGE);
        return options ? 0 : 2;
    }
    return Run(*options);
}
