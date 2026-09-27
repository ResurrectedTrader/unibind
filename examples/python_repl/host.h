#pragma once
/// \file
/// What the REPL's Python can reach: the `host` object, the classes `Counter`
/// and `Vec2`, and the template types `Shape`, `Circle` and `Rect`.
///
/// Everything here is written against the public `ub::` API and nothing else,
/// so it reads the same as it would under any backend - it is Python only
/// because the scripts that call it are. `host.cpp` is the part worth reading:
/// it is a tour of the binding API, one small binding per feature.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "unibind/unibind.h"

namespace repl {

// --- the natives script gets to hold -----------------------------------------

/// A counter with a step. Script makes them with `Counter(start, step)`, and
/// the host hands out *shared* ones by name through `host.shared_counter` - the
/// same native behind several wrappers, co-owned with `Host::registry`.
struct Counter {
    Counter(std::int32_t start, std::int32_t step) : value(start), step(step) { ++alive; }
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&) = delete;
    Counter& operator=(Counter&&) = delete;
    ~Counter() { --alive; }

    std::int32_t value = 0;
    std::int32_t step = 1;

    /// How many natives exist right now, whoever owns them. `Counter.alive()`.
    static inline std::int32_t alive = 0;
};

/// What `iter(counter)` walks: 0, 1, ... up to the counter's value.
struct CountUp {
    std::int32_t next = 0;
    std::int32_t end = 0;
};

/// A 2-D vector, immutable from script: every operation makes a new one.
struct Vec2 {
    double x = 0;
    double y = 0;
};

// --- the host's own state --------------------------------------------------------

/// Everything the bindings share. One per realm the host is installed in, and
/// it must go **before** the isolate: it holds `Global`s and a `Context`, and
/// `unibind/isolate.h` is clear that nothing an isolate handed out outlives it.
struct Host {
    Host(ub::Isolate& isolate, ub::Context context);

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) = delete;
    Host& operator=(Host&&) = delete;
    ~Host();

    ub::Isolate& isolate;
    /// The realm the host lives in. A posted job runs outside any call and
    /// with no realm entered, so it enters this one to do its work.
    ub::Context context;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    // What `host.session` shows and the REPL reads back.
    std::uint32_t inputs = 0;
    std::string prompt = ">>> ";

    /// `host.shared_counter(name)`: natives the host owns a share of, and
    /// hands script more shares of.
    std::map<std::string, std::shared_ptr<Counter>, std::less<>> registry;

    /// `host.env`: a string store behind a named interceptor.
    struct EnvEntry {
        std::string value;
        bool locked = false;  ///< neither written nor deleted from script
    };
    std::map<std::string, EnvEntry, std::less<>> env;

    /// `host.registers`: eight integers behind an indexed interceptor.
    std::array<std::int32_t, 8> registers{};

    /// A `host.set_timeout` that has not fired yet. The posted job carries a
    /// pointer to this, so it lives in `timers` until the job has run - a
    /// `clear_timeout` only marks it.
    struct Timer {
        Host* host = nullptr;
        std::uint32_t id = 0;
        ub::Global<ub::Function> callback;
        bool cancelled = false;
    };
    std::map<std::uint32_t, std::unique_ptr<Timer>> timers;

    /// A `host.fetch_later` promise that the host has not settled yet.
    struct Pending {
        Host* host = nullptr;
        std::uint32_t id = 0;
        ub::Global<ub::Promise> promise;
        ub::Global<ub::Value> value;
        bool reject = false;
    };
    std::map<std::uint32_t, std::unique_ptr<Pending>> pending;
    std::uint32_t nextId = 1;

    // Declarations. They belong to the isolate and are only *handles* here; a
    // callback that has to make an instance - `Vec2.add` returns a new Vec2 -
    // finds them through this struct.
    std::optional<ub::Class<Counter>> counterClass;
    std::optional<ub::Class<CountUp>> countUpClass;
    std::optional<ub::Class<Vec2>> vec2Class;
    std::optional<ub::FunctionTemplate> shapeType;
    std::optional<ub::FunctionTemplate> circleType;
    std::optional<ub::FunctionTemplate> rectType;

    /// Python's `dict`, so that `host.heap()` can answer with a real dict
    /// rather than a JavaScript-shaped object.
    ub::Global<ub::Function> dictType;

    /// Timers and promises the host still owes script: what a script run
    /// waits on before the program exits.
    [[nodiscard]] std::size_t Outstanding() const noexcept;
};

/// Declares every binding and installs them into `host.context`'s globals.
/// False - with a line on stderr - if anything could not be made.
[[nodiscard]] bool InstallHost(Host& host);

/// One line per thing the REPL's `:bindings` lists.
struct BindingDoc {
    std::string_view name;
    std::string_view summary;
};
[[nodiscard]] std::span<const BindingDoc> BindingDocs() noexcept;

/// What `caught` holds, as a human reads it: Python's traceback, or the
/// message when there is none. No trailing newline.
[[nodiscard]] std::string ExceptionText(const ub::Context& context, const ub::TryCatch& caught);

/// Prints what a job threw - there is nobody to hand it to.
void ReportUncaught(const ub::Context& context, const ub::TryCatch& caught, std::string_view where);

}  // namespace repl
