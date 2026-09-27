/// \file
/// The benchmark: the same workloads on whichever engine this program links.
///
/// Two halves. **Script-side** rows are one `Evaluate` of a loop that runs N
/// iterations of an operation, reported per iteration and, beside that, net of
/// an empty loop's cost - the operation rather than the loop around it. The
/// loop is the only engine-dependent thing in the program: each workload is a
/// `{JavaScript, Python}` pair of sources, and `Platform::BackendName()` picks
/// which one runs. **C++-side** rows are the API itself, called from C++: a
/// property read, a call into script, a compile, a realm, an isolate, and the
/// handle operations the model of docs/lifetimes.md charges for.
///
/// Every measurement is calibrated to take about `--target-ms` (150 ms by
/// default), run once more untimed at that size, then timed `--repetitions`
/// times (5); the median is what is reported. Every script loop returns a value
/// the program checks, so that a loop a JIT could prove dead cannot become a
/// loop that does nothing and still be reported. `--csv` prints the rows in the
/// form `tests/bench/Compare.cmake` merges; see README "What it costs".
///
/// Numbers are only comparable against each other on the same machine.
/// Written against `ub::` alone, like the rest of the suite.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "unibind/unibind.h"

namespace {

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct Options {
    double targetMilliseconds = 150.0;
    int repetitions = 5;
    bool csv = false;
};

enum class Side : std::uint8_t { Script, Native };

struct Result {
    Side side = Side::Native;
    std::string id;
    std::string label;
    double nanoseconds = 0.0;
    /// Script rows only: `nanoseconds` less the empty loop's.
    std::optional<double> net;
    std::uint64_t iterations = 0;
    /// The slowest repetition over the fastest - how far to trust the median.
    double spread = 0.0;
};

/// Runs `n` operations and says whether they all did what they should.
using Body = std::function<bool(std::uint64_t)>;

/// Nanoseconds for one call of `body(n)`, or empty if it failed.
std::optional<double> TimeOnce(const Body& body, std::uint64_t n) {
    const auto start = std::chrono::steady_clock::now();
    const bool ok = body(n);
    const auto finish = std::chrono::steady_clock::now();
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

/// The median of `repetitions` timed runs of `body`, each sized to take about
/// the target. The calibration runs are the warm-up: on V8 and SpiderMonkey
/// they are what tiers a loop up to the optimising compiler, so the timed runs
/// see the code the engine settled on, not its first attempt.
std::optional<Result> Measure(const Options& options, Side side, std::string_view id, std::string_view label,
                              const Body& body, std::uint64_t firstGuess, std::uint64_t ceiling) {
    const double target = options.targetMilliseconds * 1e6;
    std::uint64_t n = std::max<std::uint64_t>(firstGuess, 1);
    for (;;) {
        const auto elapsed = TimeOnce(body, n);
        if (!elapsed) {
            return std::nullopt;
        }
        if (*elapsed >= target / 20.0 || n >= ceiling) {
            const double scaled = static_cast<double>(n) * target / std::max(*elapsed, 1.0);
            n = std::clamp<std::uint64_t>(static_cast<std::uint64_t>(scaled), 1, ceiling);
            break;
        }
        // Grow towards a twentieth of the target, at most a hundredfold a step,
        // so that an operation much slower than the guess is not run a hundred
        // times more often than it needed to be.
        const double factor = std::clamp(target / 20.0 / std::max(*elapsed, 1.0) * 1.5, 2.0, 100.0);
        n = std::min<std::uint64_t>(static_cast<std::uint64_t>(static_cast<double>(n) * factor), ceiling);
    }
    if (!TimeOnce(body, n)) {
        return std::nullopt;
    }

    std::vector<double> perOperation;
    for (int r = 0; r < std::max(options.repetitions, 1); ++r) {
        const auto elapsed = TimeOnce(body, n);
        if (!elapsed) {
            return std::nullopt;
        }
        perOperation.push_back(*elapsed / static_cast<double>(n));
    }
    std::ranges::sort(perOperation);
    const std::size_t count = perOperation.size();
    const double median =
        count % 2 == 1 ? perOperation[count / 2] : (perOperation[(count / 2) - 1] + perOperation[count / 2]) / 2.0;
    Result result;
    result.side = side;
    result.id = id;
    result.label = label;
    result.nanoseconds = median;
    result.iterations = n;
    result.spread = perOperation.front() > 0.0 ? perOperation.back() / perOperation.front() : 0.0;
    return result;
}

/// Kept out of the optimiser's reach without a compiler-specific barrier.
volatile std::int32_t g_sink = 0;

// ---------------------------------------------------------------------------
// What script is given to call
// ---------------------------------------------------------------------------

struct Counter {
    std::int32_t value = 0;
};

std::unique_ptr<Counter> MakeCounter(const ub::CallbackInfo& info) {
    auto made = std::make_unique<Counter>();
    if (info.Length() > 0) {
        made->value = info[0].ToInt32(info.GetContext()).value_or(0);
    }
    return made;
}

void Increment(Counter& self, const ub::CallbackInfo& info) {
    ++self.value;
    info.GetReturnValue().Set(self.value);
}

void ReadValue(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void WriteValue(Counter& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    if (const auto asInt = value.ToInt32(info.GetContext())) {
        self.value = *asInt;
    }
}

/// `add(a, b)`, the smallest native function that reads its arguments.
void Add(const ub::CallbackInfo& info) {
    const auto& context = info.GetContext();
    const auto a = info[0].ToInt32(context);
    const auto b = info[1].ToInt32(context);
    if (a && b) {
        info.GetReturnValue().Set(*a + *b);
    }
}

void NativeCallee(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(static_cast<std::int32_t>(info.Length()));
}

/// A catch-all that answers 1 for every string key: the hook's own cost, with
/// nothing of an embedder's lookup behind it.
ub::Intercepted AnswerOne(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    if (!property.IsString()) {
        return ub::Intercepted::No;
    }
    info.GetReturnValue().Set(std::int32_t{1});
    return ub::Intercepted::Yes;
}

// ---------------------------------------------------------------------------
// Script-side workloads
// ---------------------------------------------------------------------------

/// One loop, in each language. Each becomes a function `NAME(n)`:
///
///     function NAME(n) { PRE let s = 0; for (let i = 0; i < n; ++i) { BODY } return RESULT; }
///
///     def NAME(n):
///         PRE
///         s = 0
///         for i in range(n):
///             BODY
///         return RESULT
///
/// so every value the loop touches is a local in both - a Python global is a
/// dictionary lookup and a JavaScript one a context-slot load, and neither is
/// what a row is about. Every body is the baseline's `s += 1` with the
/// operation put in, so the difference from the baseline is the operation -
/// except the string row, whose net figure also carries a length check.
struct Source {
    std::string_view pre;
    std::string_view body;
    std::string_view result = "s";
};

struct ScriptWorkload {
    std::string_view id;
    std::string_view label;
    Source js;
    Source py;
    /// What the loop returns, per iteration; checked on every run.
    double perIterationJs = 1.0;
    double perIterationPy = 1.0;
};

// NOLINTBEGIN(cert-err58-cpp)
constexpr std::string_view kJsSetup = R"(
var scriptObject = {x: 1};
var writeTarget = {x: 0};
function inc(x) { return x + 1; }
var items = [1, 1, 1, 1, 1, 1, 1, 1];
var shapes = [];
for (let k = 0; k < 8; ++k) shapes.push({x: 1});
var small = {a: 1, b: 'two', c: [1, 2, 3]};
)";

constexpr std::string_view kPySetup = R"(
import json

class Plain:
    def __init__(self, x):
        self.x = x

script_object = Plain(1)
write_target = Plain(0)

def inc(x):
    return x + 1

items = [1, 1, 1, 1, 1, 1, 1, 1]
shapes = [Plain(1) for _ in range(8)]
small = {'a': 1, 'b': 'two', 'c': [1, 2, 3]}
)";

// `JSON.stringify` of `small` is 29 characters; Python's `json.dumps`, with its
// default separators, 36.
constexpr std::array kScriptWorkloads{
    ScriptWorkload{.id = "empty-loop",
                   .label = "empty loop (baseline)",
                   .js = {.pre = "", .body = "s += 1;"},
                   .py = {.pre = "pass", .body = "s += 1"}},
    ScriptWorkload{.id = "script-object-read",
                   .label = "read a property of a script object",
                   .js = {.pre = "const o = scriptObject;", .body = "s += o.x;"},
                   .py = {.pre = "o = script_object", .body = "s += o.x"}},
    ScriptWorkload{.id = "script-object-write",
                   .label = "write a property of a script object",
                   .js = {.pre = "const o = writeTarget;", .body = "o.x = i; s += 1;"},
                   .py = {.pre = "o = write_target", .body = "o.x = i; s += 1"}},
    ScriptWorkload{.id = "native-object-read",
                   .label = "read a property of an Object::New object",
                   .js = {.pre = "const o = nativeObject;", .body = "s += o.x;"},
                   .py = {.pre = "o = native_object", .body = "s += o.x"}},
    ScriptWorkload{.id = "script-call",
                   .label = "call a script function",
                   .js = {.pre = "const f = inc;", .body = "s = f(s);"},
                   .py = {.pre = "f = inc", .body = "s = f(s)"}},
    ScriptWorkload{.id = "native-accessor",
                   .label = "read a native accessor (Class<T>)",
                   .js = {.pre = "const c = counter; c.value = 1;", .body = "s += c.value;"},
                   .py = {.pre = "c = counter; c.value = 1", .body = "s += c.value"}},
    ScriptWorkload{.id = "native-method",
                   .label = "call a native method (Class<T>)",
                   .js = {.pre = "const c = counter; c.value = 0;", .body = "s = c.increment();"},
                   .py = {.pre = "c = counter; c.value = 0", .body = "s = c.increment()"}},
    ScriptWorkload{.id = "native-function",
                   .label = "call a native function",
                   .js = {.pre = "const f = add;", .body = "s = f(s, 1);"},
                   .py = {.pre = "f = add", .body = "s = f(s, 1)"}},
    ScriptWorkload{.id = "interceptor-read",
                   .label = "read through a named interceptor",
                   .js = {.pre = "const h = intercepted;", .body = "s += h.one;"},
                   .py = {.pre = "h = intercepted", .body = "s += h.one"}},
    ScriptWorkload{.id = "native-construct",
                   .label = "construct a native class instance",
                   .js = {.pre = "const C = Counter;", .body = "new C(1); s += 1;"},
                   .py = {.pre = "C = Counter", .body = "C(1); s += 1"}},
    ScriptWorkload{.id = "array-read",
                   .label = "read an array element (a[i & 7])",
                   .js = {.pre = "const a = items;", .body = "s += a[i & 7];"},
                   .py = {.pre = "a = items", .body = "s += a[i & 7]"}},
    // The row above with a property read added. The script-object row reads
    // one object, and a JIT hoists that load out of the loop; eight objects of
    // one shape, one per iteration, leave a load it has to do - so this row
    // less the one above is a property read the JIT could not remove.
    ScriptWorkload{.id = "script-object-read-varying",
                   .label = "read a property of one of 8 script objects (a[i & 7].x)",
                   .js = {.pre = "const a = shapes;", .body = "s += a[i & 7].x;"},
                   .py = {.pre = "a = shapes", .body = "s += a[i & 7].x"}},
    ScriptWorkload{.id = "typed-array-read",
                   .label = "read a typed array element (a[i & 7])",
                   .js = {.pre = "const a = typed;", .body = "s += a[i & 7];"},
                   .py = {.pre = "a = typed", .body = "s += a[i & 7]"}},
    // A fresh string every 1024 characters: one string grown for all N would be
    // hundreds of megabytes by the time the calibration was done, which is a
    // test of the engine's heap limit rather than of appending.
    ScriptWorkload{
        .id = "string-append",
        .label = "append to a string (a new one every 1024)",
        .js = {.pre = "let t = '';",
               .body = "t += 'x'; if (t.length === 1024) { s += 1024; t = ''; }",
               .result = "s + t.length"},
        .py = {.pre = "t = ''", .body = "t += 'x'\nif len(t) == 1024: s += 1024; t = ''", .result = "s + len(t)"}},
    ScriptWorkload{.id = "json",
                   .label = "JSON.stringify / json.dumps of a small object",
                   .js = {.pre = "const o = small;", .body = "s += JSON.stringify(o).length;"},
                   .py = {.pre = "o = small; d = json.dumps", .body = "s += len(d(o))"},
                   .perIterationJs = 29.0,
                   .perIterationPy = 36.0},
};
// NOLINTEND(cert-err58-cpp)

std::string FunctionName(std::string_view id) {
    std::string name = "bench_";
    for (const char c : id) {
        name += c == '-' ? '_' : c;
    }
    return name;
}

std::string Define(bool python, const ScriptWorkload& workload) {
    const std::string name = FunctionName(workload.id);
    if (python) {
        const Source& s = workload.py;
        // A body of several lines is indented as the loop's.
        std::string body(s.body);
        for (std::size_t at = body.find('\n'); at != std::string::npos; at = body.find('\n', at + 1)) {
            body.insert(at + 1, "        ");
        }
        return "def " + name + "(n):\n    " + std::string(s.pre) + "\n    s = 0\n    for i in range(n):\n        " +
               body + "\n    return " + std::string(s.result) + "\n";
    }
    const Source& s = workload.js;
    return "function " + name + "(n) { " + std::string(s.pre) + " let s = 0; for (let i = 0; i < n; ++i) { " +
           std::string(s.body) + " } return " + std::string(s.result) + "; }\n";
}

/// Exposed under a name each language would spell: `camelCase` to JavaScript,
/// `snake_case` to Python. Only the two-word names differ.
bool Expose(const ub::Context& context, bool python, std::string_view js, std::string_view py,
            const ub::Local<ub::Value>& value) {
    return context.GlobalObject().Set(context, python ? py : js, value).value_or(false);
}

void Report(const char* what, const ub::Context& context, ub::TryCatch& handler) {
    std::fprintf(stderr, "bench: %s failed: %s\n", what,
                 handler.HasCaught() ? handler.Message(context).value_or("(no message)").c_str() : "(nothing thrown)");
}

bool RunScriptSide(const Options& options, ub::Isolate& isolate, const ub::Context& context, bool python,
                   std::vector<Result>& results) {
    ub::TryCatch handler(isolate);
    if (!ub::Evaluate(context, python ? kPySetup : kJsSetup)) {
        Report("the script setup", context, handler);
        return false;
    }

    // What the loops reach that C++ made.
    auto nativeObject = ub::Object::New(context);
    auto add = ub::Function::New(context, &Add);
    const auto counterClass = ub::Class<Counter>::New(isolate, "Counter");
    counterClass.Construct<&MakeCounter>();
    counterClass.Method<&Increment>("increment");
    counterClass.Accessor<&ReadValue, &WriteValue>("value");
    auto counterType = counterClass.GetConstructor(context);
    const auto interceptorTemplate = ub::ObjectTemplate::New(isolate);
    interceptorTemplate.SetHandler(ub::NamedPropertyHandler{.getter = &AnswerOne});
    auto intercepted = interceptorTemplate.NewInstance(context);
    constexpr std::array<std::int32_t, 8> kOnes{1, 1, 1, 1, 1, 1, 1, 1};
    auto typed = ub::TypedArray::New(context, std::span<const std::int32_t>(kOnes));
    if (!nativeObject || !add || !counterType || !intercepted || !typed) {
        std::fprintf(stderr, "bench: could not make what the script side calls\n");
        return false;
    }
    const std::array<ub::Local<ub::Value>, 1> startAtZero{ub::Integer::New(isolate, 0)};
    auto counter = counterType->NewInstance(context, startAtZero);
    if (!counter || !nativeObject->Set(context, "x", ub::Integer::New(isolate, 1)).value_or(false) ||
        !Expose(context, python, "nativeObject", "native_object", *nativeObject) ||
        !Expose(context, python, "add", "add", *add) || !Expose(context, python, "Counter", "Counter", *counterType) ||
        !Expose(context, python, "counter", "counter", *counter) ||
        !Expose(context, python, "intercepted", "intercepted", *intercepted) ||
        !Expose(context, python, "typed", "typed", *typed)) {
        std::fprintf(stderr, "bench: could not expose what the script side calls\n");
        return false;
    }

    for (const ScriptWorkload& workload : kScriptWorkloads) {
        if (!ub::Evaluate(context, Define(python, workload))) {
            Report(std::string(workload.id).c_str(), context, handler);
            return false;
        }
        const std::string name = FunctionName(workload.id);
        const double perIteration = python ? workload.perIterationPy : workload.perIterationJs;
        const Body body = [&](std::uint64_t n) {
            ub::HandleScope inner(isolate);
            const auto value = ub::Evaluate(context, name + "(" + std::to_string(n) + ")");
            if (!value) {
                Report(name.c_str(), context, handler);
                return false;
            }
            const auto answer = value->ToNumber(context);
            const double expected = perIteration * static_cast<double>(n);
            if (!answer || *answer != expected) {
                std::fprintf(stderr, "bench: %s(%llu) returned %.17g, not %.17g\n", name.c_str(),
                             static_cast<unsigned long long>(n), answer.value_or(-1.0), expected);
                return false;
            }
            return true;
        };
        // The loop counters stay small integers in both languages - a Smi in
        // V8, one digit in CPython - with the ceiling at 2^29.
        auto result = Measure(options, Side::Script, workload.id, workload.label, body, 1000, std::uint64_t{1} << 29U);
        if (!result) {
            return false;
        }
        results.push_back(std::move(*result));
    }

    const double baseline = results.front().nanoseconds;
    for (Result& result : results) {
        if (result.side == Side::Script && result.id != "empty-loop") {
            result.net = result.nanoseconds - baseline;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// C++-side workloads
// ---------------------------------------------------------------------------

/// Before any other isolate exists, so that this is the cost of one isolate
/// alone rather than of one beside another.
bool RunIsolateLifecycle(const Options& options, std::vector<Result>& results) {
    const Body body = [](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            auto isolate = ub::Isolate::New();
            if (isolate == nullptr) {
                std::fprintf(stderr, "bench: could not create an isolate\n");
                return false;
            }
        }
        return true;
    };
    auto result = Measure(options, Side::Native, "isolate-new", "Isolate::New + destroy", body, 1, 100000);
    if (!result) {
        return false;
    }
    results.push_back(std::move(*result));
    return true;
}

bool RunNativeSide(const Options& options, ub::Isolate& isolate, const ub::Context& context, bool python,
                   std::vector<Result>& results) {
    auto object = ub::Object::New(context);
    auto key = ub::String::New(isolate, "property");
    auto xKey = ub::String::New(isolate, "x");
    auto function = ub::Function::New(context, &NativeCallee);
    auto scriptObject = ub::Evaluate(context, python ? "script_object" : "scriptObject");
    auto scriptFunction = ub::Evaluate(context, "inc");
    if (!object || !key || !xKey || !function || !scriptObject || !scriptFunction || !scriptObject->IsObject()) {
        std::fprintf(stderr, "bench: could not set the C++ side up\n");
        return false;
    }
    auto scriptObjectAsObject = scriptObject->To<ub::Object>();
    auto inc = scriptFunction->To<ub::Function>();
    if (!scriptObjectAsObject || !inc || !object->Set(context, *key, ub::Integer::New(isolate, 1)).value_or(false)) {
        return false;
    }

    const auto add = [&](std::string_view id, std::string_view label, const Body& body, std::uint64_t firstGuess,
                         std::uint64_t ceiling) {
        auto result = Measure(options, Side::Native, id, label, body, firstGuess, ceiling);
        if (!result) {
            std::fprintf(stderr, "bench: %s failed\n", std::string(id).c_str());
            return false;
        }
        results.push_back(std::move(*result));
        return true;
    };
    constexpr std::uint64_t kMany = std::uint64_t{1} << 32U;

    const bool ok =
        add(
            "object-new", "Object::New",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto made = ub::Object::New(context);
                    g_sink += made ? 1 : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "get-by-handle", "Object::Get of an Object::New object by key handle",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto value = object->Get(context, *key);
                    g_sink += value ? value->To<ub::Integer>()->Int32Value() : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "get-by-name", "Object::Get of an Object::New object by string_view",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto value = object->Get(context, "property");
                    g_sink += value ? 1 : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "get-script-object", "Object::Get of a script object by key handle",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto value = scriptObjectAsObject->Get(context, *xKey);
                    g_sink += value ? value->To<ub::Integer>()->Int32Value() : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "set-by-handle", "Object::Set of an Object::New object by key handle",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    const bool set =
                        object->Set(context, *key, ub::Integer::New(isolate, static_cast<std::int32_t>(i & 0xffU)))
                            .value_or(false);
                    g_sink += set ? 1 : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "call-native", "Function::Call of a native function",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto result = function->Call(context, context.GlobalObject());
                    g_sink += result ? 1 : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "call-script", "Function::Call of a script function",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    const std::array<ub::Local<ub::Value>, 1> arguments{
                        ub::Integer::New(isolate, static_cast<std::int32_t>(i & 0xffU))};
                    auto result = inc->Call(context, ub::Undefined(isolate), arguments);
                    if (!result) {
                        return false;
                    }
                    g_sink += result->To<ub::Integer>() ? 1 : 0;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "evaluate", "Evaluate(\"1 + 1\") - compile and run",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto result = ub::Evaluate(context, "1 + 1");
                    if (!result) {
                        return false;
                    }
                    g_sink += 1;
                }
                return true;
            },
            100, kMany) &&
        [&] {
            auto script = ub::Script::Compile(context, "1 + 1");
            if (!script) {
                return false;
            }
            return add(
                "script-run", "Script::Run of \"1 + 1\" compiled once",
                [&](std::uint64_t n) {
                    for (std::uint64_t i = 0; i < n; ++i) {
                        const ub::HandleScope inner(isolate);
                        auto result = script->Run(context);
                        if (!result) {
                            return false;
                        }
                        g_sink += 1;
                    }
                    return true;
                },
                1000, kMany);
        }() &&
        add(
            "context-new", "Context::New + destroy",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    auto made = ub::Context::New(isolate);
                    if (!made) {
                        return false;
                    }
                }
                return true;
            },
            10, kMany) &&
        add(
            "frame", "open and close a HandleScope",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope inner(isolate);
                    g_sink += 1;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "handle-create", "create a handle",
            [&](std::uint64_t n) {
                // A fresh frame every 1024 handles, so this measures appending to a
                // frame rather than one frame growing without bound.
                std::uint64_t made = 0;
                while (made < n) {
                    const ub::HandleScope inner(isolate);
                    const std::uint64_t chunk = std::min<std::uint64_t>(1024, n - made);
                    for (std::uint64_t i = 0; i < chunk; ++i) {
                        g_sink += ub::Integer::New(isolate, static_cast<std::int32_t>(i)).Int32Value();
                    }
                    made += chunk;
                }
                return true;
            },
            1000, kMany) &&
        add(
            "handle-read", "read a handle",
            [&](std::uint64_t n) {
                const ub::HandleScope inner(isolate);
                const auto value = ub::Integer::New(isolate, 3);
                for (std::uint64_t i = 0; i < n; ++i) {
                    g_sink += value.Int32Value();
                }
                return true;
            },
            1000, kMany) &&
        add(
            "handle-escape", "escape a handle",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::HandleScope outer(isolate);
                    ub::EscapableHandleScope inner(isolate);
                    g_sink += inner.Escape(ub::Integer::New(isolate, 1)).Int32Value();
                }
                return true;
            },
            1000, kMany) &&
        add(
            "global", "create and release a Global",
            [&](std::uint64_t n) {
                for (std::uint64_t i = 0; i < n; ++i) {
                    const ub::Global<ub::Object> root(isolate, *object);
                    g_sink += root.IsEmpty() ? 0 : 1;
                }
                return true;
            },
            1000, kMany);
    return ok;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void PrintTable(const std::vector<Result>& results, std::string_view backend) {
    std::printf("\nbenchmark backend=%s\n", std::string(backend).c_str());
    for (const Side side : {Side::Script, Side::Native}) {
        std::printf("\n%-50s %12s %12s %12s %8s\n",
                    side == Side::Script ? "script-side, per iteration" : "C++-side, per operation", "ns", "net ns",
                    "iterations", "spread");
        for (const Result& result : results) {
            if (result.side != side) {
                continue;
            }
            std::printf("%-50s %12.2f ", result.label.c_str(), result.nanoseconds);
            if (result.net) {
                std::printf("%12.2f ", *result.net);
            } else {
                std::printf("%12s ", "");
            }
            std::printf("%12llu %8.2f\n", static_cast<unsigned long long>(result.iterations), result.spread);
        }
    }
    std::printf("\n");
}

/// One row per measurement. A comma or a semicolon in a label is written as a
/// space, so nothing needs quoting and `Compare.cmake` can split a row as it is.
void PrintCsv(const std::vector<Result>& results, std::string_view backend) {
    std::printf("backend,side,id,label,ns,net_ns,iterations,spread\n");
    for (const Result& result : results) {
        std::string label = result.label;
        std::ranges::replace_if(label, [](char c) { return c == ',' || c == ';'; }, ' ');
        std::printf("%s,%s,%s,%s,%.3f,", std::string(backend).c_str(),
                    result.side == Side::Script ? "script" : "native", result.id.c_str(), label.c_str(),
                    result.nanoseconds);
        if (result.net) {
            std::printf("%.3f", *result.net);
        }
        std::printf(",%llu,%.3f\n", static_cast<unsigned long long>(result.iterations), result.spread);
    }
}

int Run(const Options& options) {
    const bool python = ub::Platform::BackendName() == "python";
    std::vector<Result> native;
    if (!RunIsolateLifecycle(options, native)) {
        return 1;
    }

    auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        std::fprintf(stderr, "bench: could not create an isolate\n");
        return 1;
    }
    std::vector<Result> results;
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            std::fprintf(stderr, "bench: could not create a context\n");
            return 1;
        }
        const ub::ContextScope entered(*context);
        if (!RunScriptSide(options, *isolate, *context, python, results) ||
            !RunNativeSide(options, *isolate, *context, python, results)) {
            return 1;
        }
    }
    results.insert(results.end(), native.begin(), native.end());

    if (options.csv) {
        PrintCsv(results, ub::Platform::BackendName());
    } else {
        PrintTable(results, ub::Platform::BackendName());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        const bool hasValue = i + 1 < arguments.size();
        if (argument == "--csv") {
            options.csv = true;
        } else if (argument == "--quick") {
            // For CTest: that every workload runs and answers, not how fast.
            options.targetMilliseconds = 5.0;
            options.repetitions = 1;
        } else if (argument == "--target-ms" && hasValue) {
            options.targetMilliseconds = std::max(std::strtod(arguments[++i], nullptr), 1.0);
        } else if (argument == "--repetitions" && hasValue) {
            options.repetitions = std::max(std::atoi(arguments[++i]), 1);
        } else {
            std::fprintf(stderr, "usage: unibind_bench [--csv] [--quick] [--target-ms N] [--repetitions N]\n");
            return 2;
        }
    }

    const ub::Platform platform;
    return Run(options);
}
