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
/// Written against `ub::` alone, like the rest of the suite - except, on the V8
/// backend, a **raw V8 twin** of every row that has one: the same operation
/// written against V8 directly, on the same isolate and realm, measured right
/// after the `ub::` row it pairs with. The difference is what unibind costs
/// over the engine itself. It is built into the V8 backend's benchmark only
/// (`UNIBIND_BENCH_V8_BASELINE`, set by tests/CMakeLists.txt), and it goes in
/// the CSV's `raw_ns` column.

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

#ifdef UNIBIND_BENCH_V8_BASELINE
#include <v8-fast-api-calls.h>
#include <v8.h>

#include "unibind/interop/v8.h"
#endif

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
    /// The raw V8 twin's median, on the V8 backend, for a row that has one.
    std::optional<double> raw;
    /// A row with no `ub::` side: measured against V8 alone, for comparison
    /// with a neighbouring row. `nanoseconds` and `raw` are then the same.
    bool rawOnly = false;
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

#ifdef UNIBIND_BENCH_V8_BASELINE
// ---------------------------------------------------------------------------
// The same, written against V8 directly
//
// Each is what a V8 embedder would write for the `ub::` callback above it, and
// does the same work: a method and an accessor find their native through an
// internal field of a receiver a `v8::Signature` has already checked, which is
// V8's own way to type a receiver and what `Class<T>` does by other means.
// ---------------------------------------------------------------------------

constexpr int kRawNativeField = 0;

Counter* RawCounterOf(v8::Local<v8::Object> self) {
    return static_cast<Counter*>(
        self->GetAlignedPointerFromInternalField(kRawNativeField, v8::kEmbedderDataTypeTagDefault));
}

void RawAdd(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    const v8::Maybe<std::int32_t> a = info[0]->Int32Value(context);
    const v8::Maybe<std::int32_t> b = info[1]->Int32Value(context);
    if (a.IsJust() && b.IsJust()) {
        info.GetReturnValue().Set(a.FromJust() + b.FromJust());
    }
}

void RawCallee(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(info.Length());
}

void RawIncrement(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Counter* self = RawCounterOf(info.This());
    ++self->value;
    info.GetReturnValue().Set(self->value);
}

void RawReadValue(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(RawCounterOf(info.This())->value);
}

void RawWriteValue(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const v8::Maybe<std::int32_t> value = info[0]->Int32Value(info.GetIsolate()->GetCurrentContext());
    if (value.IsJust()) {
        RawCounterOf(info.This())->value = value.FromJust();
    }
}

void RawDataRead(v8::Local<v8::Name> /*property*/, const v8::PropertyCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(RawCounterOf(info.Holder())->value);
}

void RawDataWrite(v8::Local<v8::Name> /*property*/, v8::Local<v8::Value> value,
                  const v8::PropertyCallbackInfo<v8::Boolean>& info) {
    const v8::Maybe<std::int32_t> asInt = value->Int32Value(info.GetIsolate()->GetCurrentContext());
    if (asInt.IsJust()) {
        RawCounterOf(info.Holder())->value = asInt.FromJust();
    }
}

v8::Intercepted RawAnswerOne(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    if (!property->IsString()) {
        return v8::Intercepted::kNo;
    }
    info.GetReturnValue().Set(1);
    return v8::Intercepted::kYes;
}

/// The natives the raw instances carry. The benchmark's lifetime, so nothing
/// finalizes them.
Counter g_rawCounter;
Counter g_rawDataCounter;
#endif

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
    /// On V8, the JavaScript `pre` of the row's raw twin: the same loop over
    /// what `RunScriptSide` made with V8's own API. Empty for a row with no
    /// twin - one that never leaves script, and the construct row, whose cost
    /// is mostly the collector's.
    std::string_view rawJsPre;
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
                   .py = {.pre = "c = counter; c.value = 1", .body = "s += c.value"},
                   .rawJsPre = "const c = rawCounter; c.value = 1;"},
    ScriptWorkload{.id = "native-method",
                   .label = "call a native method (Class<T>)",
                   .js = {.pre = "const c = counter; c.value = 0;", .body = "s = c.increment();"},
                   .py = {.pre = "c = counter; c.value = 0", .body = "s = c.increment()"},
                   .rawJsPre = "const c = rawCounter; c.value = 0;"},
    ScriptWorkload{.id = "native-function",
                   .label = "call a native function",
                   .js = {.pre = "const f = add;", .body = "s = f(s, 1);"},
                   .py = {.pre = "f = add", .body = "s = f(s, 1)"},
                   .rawJsPre = "const f = rawAdd;"},
    ScriptWorkload{.id = "interceptor-read",
                   .label = "read through a named interceptor",
                   .js = {.pre = "const h = intercepted;", .body = "s += h.one;"},
                   .py = {.pre = "h = intercepted", .body = "s += h.one"},
                   .rawJsPre = "const h = rawIntercepted;"},
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

std::string DefineJs(const std::string& name, const Source& s) {
    return "function " + name + "(n) { " + std::string(s.pre) + " let s = 0; for (let i = 0; i < n; ++i) { " +
           std::string(s.body) + " } return " + std::string(s.result) + "; }\n";
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
    return DefineJs(name, workload.js);
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

#ifdef UNIBIND_BENCH_V8_BASELINE
/// What the raw twins of the script-side rows call, made with V8's own API and
/// put on the realm's global: `rawAdd`, `rawCounter` (a class instance whose
/// `value` accessor and `increment` method live on its prototype, as
/// `Class<T>`'s do), `rawDataCounter` (the same accessor as a native data
/// property on the instance - V8's other shape, for comparison) and
/// `rawIntercepted`.
bool ExposeRawNatives(ub::Isolate& isolate, const ub::Context& context) {
    v8::Isolate* raw = ub::interop::V8Isolate(isolate);
    const v8::HandleScope scope(raw);
    const v8::Local<v8::Context> realm = ub::interop::V8Context(context);

    const v8::Local<v8::FunctionTemplate> counterClass = v8::FunctionTemplate::New(raw);
    counterClass->InstanceTemplate()->SetInternalFieldCount(kRawNativeField + 1);
    const v8::Local<v8::Signature> signature = v8::Signature::New(raw, counterClass);
    const v8::Local<v8::ObjectTemplate> prototype = counterClass->PrototypeTemplate();
    prototype->Set(raw, "increment",
                   v8::FunctionTemplate::New(raw, &RawIncrement, {}, signature, 0, v8::ConstructorBehavior::kThrow));
    prototype->SetAccessorProperty(
        v8::String::NewFromUtf8Literal(raw, "value"),
        v8::FunctionTemplate::New(raw, &RawReadValue, {}, signature, 0, v8::ConstructorBehavior::kThrow),
        v8::FunctionTemplate::New(raw, &RawWriteValue, {}, signature, 1, v8::ConstructorBehavior::kThrow));

    const v8::Local<v8::ObjectTemplate> dataShape = v8::ObjectTemplate::New(raw);
    dataShape->SetInternalFieldCount(kRawNativeField + 1);
    dataShape->SetNativeDataProperty(v8::String::NewFromUtf8Literal(raw, "value"), &RawDataRead, &RawDataWrite);

    const v8::Local<v8::ObjectTemplate> interceptorShape = v8::ObjectTemplate::New(raw);
    interceptorShape->SetHandler(v8::NamedPropertyHandlerConfiguration(&RawAnswerOne));

    v8::Local<v8::Function> add;
    v8::Local<v8::Function> constructor;
    v8::Local<v8::Object> counter;
    v8::Local<v8::Object> dataCounter;
    v8::Local<v8::Object> intercepted;
    if (!v8::Function::New(realm, &RawAdd, {}, 0, v8::ConstructorBehavior::kThrow).ToLocal(&add) ||
        !counterClass->GetFunction(realm).ToLocal(&constructor) || !constructor->NewInstance(realm).ToLocal(&counter) ||
        !dataShape->NewInstance(realm).ToLocal(&dataCounter) ||
        !interceptorShape->NewInstance(realm).ToLocal(&intercepted)) {
        return false;
    }
    counter->SetAlignedPointerInInternalField(kRawNativeField, &g_rawCounter, v8::kEmbedderDataTypeTagDefault);
    dataCounter->SetAlignedPointerInInternalField(kRawNativeField, &g_rawDataCounter, v8::kEmbedderDataTypeTagDefault);

    const v8::Local<v8::Object> global = realm->Global();
    const auto expose = [&](const char* name, v8::Local<v8::Value> value) {
        v8::Local<v8::String> key;
        return v8::String::NewFromUtf8(raw, name).ToLocal(&key) && global->Set(realm, key, value).FromMaybe(false);
    };
    return expose("rawAdd", add) && expose("rawCounter", counter) && expose("rawDataCounter", dataCounter) &&
           expose("rawIntercepted", intercepted);
}

/// One run of a raw twin's loop, `NAME(n)`, compiled and run through V8's own
/// API, and whether it returned what it should.
bool RunRawLoop(ub::Isolate& isolate, const ub::Context& context, const std::string& name, std::uint64_t n,
                double expected) {
    v8::Isolate* raw = ub::interop::V8Isolate(isolate);
    const v8::HandleScope scope(raw);
    const v8::Local<v8::Context> realm = ub::interop::V8Context(context);
    const v8::TryCatch handler(raw);
    const std::string call = name + "(" + std::to_string(n) + ")";
    v8::Local<v8::String> source;
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> value;
    if (!v8::String::NewFromUtf8(raw, call.c_str(), v8::NewStringType::kNormal, static_cast<int>(call.size()))
             .ToLocal(&source) ||
        !v8::Script::Compile(realm, source).ToLocal(&script) || !script->Run(realm).ToLocal(&value)) {
        std::fprintf(stderr, "bench: %s failed\n", call.c_str());
        return false;
    }
    const double answer = value->NumberValue(realm).FromMaybe(-1.0);
    if (answer != expected) {
        std::fprintf(stderr, "bench: %s returned %.17g, not %.17g\n", call.c_str(), answer, expected);
        return false;
    }
    return true;
}
#endif

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
#ifdef UNIBIND_BENCH_V8_BASELINE
    if (!ExposeRawNatives(isolate, context)) {
        std::fprintf(stderr, "bench: could not make what the raw V8 twins call\n");
        return false;
    }
#endif

    // The loops run to at most 2^29: the counters stay small integers in both
    // languages - a Smi in V8, one digit in CPython.
    constexpr std::uint64_t kLoopCeiling = std::uint64_t{1} << 29U;
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
        auto result = Measure(options, Side::Script, workload.id, workload.label, body, 1000, kLoopCeiling);
        if (!result) {
            return false;
        }
#ifdef UNIBIND_BENCH_V8_BASELINE
        // The raw twin: the same loop body over what `ExposeRawNatives` made,
        // measured right after, so both see the machine in the same state.
        const auto measureRaw = [&](const std::string& rawName, const Source& source,
                                    std::string_view id) -> std::optional<Result> {
            if (!ub::Evaluate(context, DefineJs(rawName, source))) {
                Report(rawName.c_str(), context, handler);
                return std::nullopt;
            }
            const Body rawBody = [&](std::uint64_t n) {
                return RunRawLoop(isolate, context, rawName, n, perIteration * static_cast<double>(n));
            };
            return Measure(options, Side::Script, id, workload.label, rawBody, 1000, kLoopCeiling);
        };
        if (!workload.rawJsPre.empty()) {
            const auto raw =
                measureRaw("raw_" + name, Source{.pre = workload.rawJsPre, .body = workload.js.body}, workload.id);
            if (!raw) {
                return false;
            }
            result->raw = raw->nanoseconds;
        }
        results.push_back(std::move(*result));
        // V8's other shape of native accessor, a data property on the
        // instance, which unibind does not use (see AccessorRecord in the V8
        // backend): raw only, beside the accessor row it compares with.
        if (workload.id == "native-accessor") {
            auto data = measureRaw("raw_native_accessor_data",
                                   Source{.pre = "const c = rawDataCounter; c.value = 1;", .body = workload.js.body},
                                   "native-accessor-data");
            if (!data) {
                return false;
            }
            data->label = "read a native data property (raw V8 only)";
            data->raw = data->nanoseconds;
            data->rawOnly = true;
            results.push_back(std::move(*data));
        }
#else
        results.push_back(std::move(*result));
#endif
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
#ifdef UNIBIND_BENCH_V8_BASELINE
    // Each with an allocator of its own, as `Isolate::New` makes one.
    const Body raw = [](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
                v8::ArrayBuffer::Allocator::NewDefaultAllocator());
            v8::Isolate::CreateParams params;
            params.array_buffer_allocator = allocator.get();
            v8::Isolate* isolate = v8::Isolate::New(params);
            if (isolate == nullptr) {
                return false;
            }
            isolate->Dispose();
        }
        return true;
    };
    const auto rawResult = Measure(options, Side::Native, "isolate-new", "Isolate::New + destroy", raw, 1, 100000);
    if (!rawResult) {
        return false;
    }
    result->raw = rawResult->nanoseconds;
#endif
    results.push_back(std::move(*result));
    return true;
}

struct NativeWorkload {
    std::string_view id;
    std::string_view label;
    Body body;
    std::uint64_t firstGuess = 1000;
};

#ifdef UNIBIND_BENCH_V8_BASELINE
/// The raw twins' own objects, made with V8's API in the realm the `ub::` rows
/// use: an `Object::New` object with `property` set, the script's own object
/// and `inc`, a native function, and "1 + 1" compiled once. Roots, so that each
/// twin can open a scope of its own and take locals from them once, outside
/// its loop, as a V8 embedder would.
struct RawNative {
    v8::Global<v8::Object> object;
    v8::Global<v8::String> key;
    v8::Global<v8::String> xKey;
    v8::Global<v8::Object> scriptObject;
    v8::Global<v8::Function> function;
    v8::Global<v8::Function> inc;
    v8::Global<v8::Script> script;
};

bool SetUpRawNative(ub::Isolate& isolate, const ub::Context& context, RawNative& state) {
    v8::Isolate* raw = ub::interop::V8Isolate(isolate);
    const v8::HandleScope scope(raw);
    const v8::Local<v8::Context> realm = ub::interop::V8Context(context);
    const v8::Local<v8::Object> object = v8::Object::New(raw);
    const v8::Local<v8::String> key = v8::String::NewFromUtf8Literal(raw, "property");
    v8::Local<v8::Value> scriptObject;
    v8::Local<v8::Value> inc;
    v8::Local<v8::Function> function;
    v8::Local<v8::Script> script;
    if (!object->Set(realm, key, v8::Integer::New(raw, 1)).FromMaybe(false) ||
        !realm->Global()->Get(realm, v8::String::NewFromUtf8Literal(raw, "scriptObject")).ToLocal(&scriptObject) ||
        !realm->Global()->Get(realm, v8::String::NewFromUtf8Literal(raw, "inc")).ToLocal(&inc) ||
        !scriptObject->IsObject() || !inc->IsFunction() ||
        !v8::Function::New(realm, &RawCallee, {}, 0, v8::ConstructorBehavior::kThrow).ToLocal(&function) ||
        !v8::Script::Compile(realm, v8::String::NewFromUtf8Literal(raw, "1 + 1")).ToLocal(&script)) {
        return false;
    }
    state.object.Reset(raw, object);
    state.key.Reset(raw, key);
    state.xKey.Reset(raw, v8::String::NewFromUtf8Literal(raw, "x"));
    state.scriptObject.Reset(raw, scriptObject.As<v8::Object>());
    state.function.Reset(raw, function);
    state.inc.Reset(raw, inc.As<v8::Function>());
    state.script.Reset(raw, script);
    return true;
}

/// The raw twin of each C++-side row that has one, under the row's id. Each
/// does what the `ub::` row's body does, a scope per operation included.
std::vector<NativeWorkload> RawNativeWorkloads(ub::Isolate& isolate, const ub::Context& context,
                                               const RawNative& state) {
    v8::Isolate* raw = ub::interop::V8Isolate(isolate);
    std::vector<NativeWorkload> twins;
    // Every one opens a scope for the locals it takes from the roots, once.
    const auto twin = [&](std::string_view id, auto loop) {
        twins.push_back(NativeWorkload{id, "", [raw, &context, loop](std::uint64_t n) {
                                           const v8::HandleScope outer(raw);
                                           return loop(ub::interop::V8Context(context), n);
                                       }});
    };
    twin("object-new", [raw](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            g_sink += v8::Object::New(raw).IsEmpty() ? 0 : 1;
        }
        return true;
    });
    const auto readInt = [](v8::Local<v8::Value> value) {
        return value->IsInt32() ? value.As<v8::Int32>()->Value() : 0;
    };
    twin("get-by-handle", [raw, &state, readInt](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Object> object = state.object.Get(raw);
        const v8::Local<v8::String> key = state.key.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            v8::Local<v8::Value> value;
            g_sink += object->Get(realm, key).ToLocal(&value) ? readInt(value) : 0;
        }
        return true;
    });
    twin("get-by-name", [raw, &state](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Object> object = state.object.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            v8::Local<v8::String> name;
            v8::Local<v8::Value> value;
            const bool ok = v8::String::NewFromUtf8(raw, "property", v8::NewStringType::kNormal, 8).ToLocal(&name) &&
                            object->Get(realm, name).ToLocal(&value);
            g_sink += ok ? 1 : 0;
        }
        return true;
    });
    twin("get-script-object", [raw, &state, readInt](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Object> object = state.scriptObject.Get(raw);
        const v8::Local<v8::String> key = state.xKey.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            v8::Local<v8::Value> value;
            g_sink += object->Get(realm, key).ToLocal(&value) ? readInt(value) : 0;
        }
        return true;
    });
    twin("set-by-handle", [raw, &state](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Object> object = state.object.Get(raw);
        const v8::Local<v8::String> key = state.key.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            const bool set =
                object->Set(realm, key, v8::Integer::New(raw, static_cast<std::int32_t>(i & 0xffU))).FromMaybe(false);
            g_sink += set ? 1 : 0;
        }
        return true;
    });
    twin("call-native", [raw, &state](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Function> function = state.function.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            g_sink += function->Call(realm, realm->Global(), 0, nullptr).IsEmpty() ? 0 : 1;
        }
        return true;
    });
    twin("call-script", [raw, &state](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Function> inc = state.inc.Get(raw);
        const v8::Local<v8::Value> receiver = v8::Undefined(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            std::array<v8::Local<v8::Value>, 1> arguments{v8::Integer::New(raw, static_cast<std::int32_t>(i & 0xffU))};
            v8::Local<v8::Value> result;
            if (!inc->Call(realm, receiver, 1, arguments.data()).ToLocal(&result)) {
                return false;
            }
            g_sink += result->IsInt32() ? 1 : 0;
        }
        return true;
    });
    twin("evaluate", [raw](v8::Local<v8::Context> realm, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            v8::Local<v8::String> source;
            v8::Local<v8::Script> script;
            v8::Local<v8::Value> result;
            if (!v8::String::NewFromUtf8(raw, "1 + 1", v8::NewStringType::kNormal, 5).ToLocal(&source) ||
                !v8::Script::Compile(realm, source).ToLocal(&script) || !script->Run(realm).ToLocal(&result)) {
                return false;
            }
            g_sink += 1;
        }
        return true;
    });
    twin("script-run", [raw, &state](v8::Local<v8::Context> realm, std::uint64_t n) {
        const v8::Local<v8::Script> script = state.script.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            v8::Local<v8::Value> result;
            if (!script->Run(realm).ToLocal(&result)) {
                return false;
            }
            g_sink += 1;
        }
        return true;
    });
    twin("context-new", [raw](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            if (v8::Context::New(raw).IsEmpty()) {
                return false;
            }
        }
        return true;
    });
    twin("frame", [raw](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(raw);
            g_sink += 1;
        }
        return true;
    });
    twin("handle-create", [raw](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        std::uint64_t made = 0;
        while (made < n) {
            const v8::HandleScope inner(raw);
            const std::uint64_t chunk = std::min<std::uint64_t>(1024, n - made);
            for (std::uint64_t i = 0; i < chunk; ++i) {
                g_sink += v8::Integer::New(raw, static_cast<std::int32_t>(i)).As<v8::Int32>()->Value();
            }
            made += chunk;
        }
        return true;
    });
    twin("handle-read", [raw](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        const v8::HandleScope inner(raw);
        const v8::Local<v8::Int32> value = v8::Integer::New(raw, 3).As<v8::Int32>();
        for (std::uint64_t i = 0; i < n; ++i) {
            g_sink += value->Value();
        }
        return true;
    });
    twin("handle-escape", [raw](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope outer(raw);
            v8::EscapableHandleScope inner(raw);
            g_sink += inner.Escape(v8::Integer::New(raw, 1)).As<v8::Int32>()->Value();
        }
        return true;
    });
    twin("global", [raw, &state](v8::Local<v8::Context> /*realm*/, std::uint64_t n) {
        const v8::Local<v8::Object> object = state.object.Get(raw);
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::Global<v8::Object> root(raw, object);
            g_sink += root.IsEmpty() ? 0 : 1;
        }
        return true;
    });
    return twins;
}
#endif

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

    auto script = ub::Script::Compile(context, "1 + 1");
    if (!script) {
        return false;
    }

    std::vector<NativeWorkload> workloads;
    workloads.push_back(NativeWorkload{"object-new", "Object::New",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope inner(isolate);
                                               auto made = ub::Object::New(context);
                                               g_sink += made ? 1 : 0;
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{"get-by-handle", "Object::Get of an Object::New object by key handle",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope inner(isolate);
                                               auto value = object->Get(context, *key);
                                               g_sink += value ? value->To<ub::Integer>()->Int32Value() : 0;
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{"get-by-name", "Object::Get of an Object::New object by string_view",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope inner(isolate);
                                               auto value = object->Get(context, "property");
                                               g_sink += value ? 1 : 0;
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{"get-script-object", "Object::Get of a script object by key handle",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope inner(isolate);
                                               auto value = scriptObjectAsObject->Get(context, *xKey);
                                               g_sink += value ? value->To<ub::Integer>()->Int32Value() : 0;
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{
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
        1000});
    workloads.push_back(NativeWorkload{"call-native", "Function::Call of a native function",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope inner(isolate);
                                               auto result = function->Call(context, context.GlobalObject());
                                               g_sink += result ? 1 : 0;
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{"call-script", "Function::Call of a script function",
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
                                       1000});
    workloads.push_back(NativeWorkload{"evaluate", "Evaluate(\"1 + 1\") - compile and run",
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
                                       100});
    workloads.push_back(NativeWorkload{"script-run", "Script::Run of \"1 + 1\" compiled once",
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
                                       1000});
    workloads.push_back(NativeWorkload{"context-new", "Context::New + destroy",
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
                                       10});
    workloads.push_back(NativeWorkload{"frame", "open and close a HandleScope",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope inner(isolate);
                                               g_sink += 1;
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(
        NativeWorkload{"handle-create", "create a handle",
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
                       1000});
    workloads.push_back(NativeWorkload{"handle-read", "read a handle",
                                       [&](std::uint64_t n) {
                                           const ub::HandleScope inner(isolate);
                                           const auto value = ub::Integer::New(isolate, 3);
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               g_sink += value.Int32Value();
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{"handle-escape", "escape a handle",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::HandleScope outer(isolate);
                                               ub::EscapableHandleScope inner(isolate);
                                               g_sink += inner.Escape(ub::Integer::New(isolate, 1)).Int32Value();
                                           }
                                           return true;
                                       },
                                       1000});
    workloads.push_back(NativeWorkload{"global", "create and release a Global",
                                       [&](std::uint64_t n) {
                                           for (std::uint64_t i = 0; i < n; ++i) {
                                               const ub::Global<ub::Object> root(isolate, *object);
                                               g_sink += root.IsEmpty() ? 0 : 1;
                                           }
                                           return true;
                                       },
                                       1000});

#ifdef UNIBIND_BENCH_V8_BASELINE
    RawNative rawState;
    if (!SetUpRawNative(isolate, context, rawState)) {
        std::fprintf(stderr, "bench: could not set the raw V8 twins up\n");
        return false;
    }
    const std::vector<NativeWorkload> twins = RawNativeWorkloads(isolate, context, rawState);
#endif

    constexpr std::uint64_t kMany = std::uint64_t{1} << 32U;
    for (const NativeWorkload& workload : workloads) {
        auto result =
            Measure(options, Side::Native, workload.id, workload.label, workload.body, workload.firstGuess, kMany);
        if (!result) {
            std::fprintf(stderr, "bench: %s failed\n", std::string(workload.id).c_str());
            return false;
        }
#ifdef UNIBIND_BENCH_V8_BASELINE
        // Right after the row it pairs with, so both see the machine alike.
        const auto twin = std::ranges::find(twins, workload.id, &NativeWorkload::id);
        if (twin != twins.end()) {
            const auto raw =
                Measure(options, Side::Native, workload.id, workload.label, twin->body, workload.firstGuess, kMany);
            if (!raw) {
                std::fprintf(stderr, "bench: the raw V8 twin of %s failed\n", std::string(workload.id).c_str());
                return false;
            }
            result->raw = raw->nanoseconds;
        }
#endif
        results.push_back(std::move(*result));
    }
    return true;
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
            if (result.side != side || result.rawOnly) {
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
    // What the binding costs over the engine itself, where a row has a raw twin.
    if (std::ranges::any_of(results, [](const Result& result) { return result.raw.has_value(); })) {
        std::printf("\n%-50s %12s %12s %12s %8s\n", "unibind over V8 itself", "ub:: ns", "raw V8 ns", "overhead ns",
                    "ratio");
        for (const Result& result : results) {
            if (!result.raw) {
                continue;
            }
            if (result.rawOnly) {
                std::printf("%-50s %12s %12.2f\n", result.label.c_str(), "", *result.raw);
                continue;
            }
            std::printf("%-50s %12.2f %12.2f %12.2f %7.2fx\n", result.label.c_str(), result.nanoseconds, *result.raw,
                        result.nanoseconds - *result.raw, *result.raw > 0.0 ? result.nanoseconds / *result.raw : 0.0);
        }
    }
    std::printf("\n");
}

/// One row per measurement. A comma or a semicolon in a label is written as a
/// space, so nothing needs quoting and `Compare.cmake` can split a row as it is.
/// `raw_ns` is the raw V8 twin's figure, on the V8 backend, and empty
/// elsewhere; a row measured against V8 alone has the side `raw`.
void PrintCsv(const std::vector<Result>& results, std::string_view backend) {
    std::printf("backend,side,id,label,ns,net_ns,iterations,spread,raw_ns\n");
    for (const Result& result : results) {
        std::string label = result.label;
        std::ranges::replace_if(label, [](char c) { return c == ',' || c == ';'; }, ' ');
        const char* side = result.side == Side::Script ? "script" : "native";
        if (result.rawOnly) {
            side = "raw";
        }
        std::printf("%s,%s,%s,%s,%.3f,", std::string(backend).c_str(), side, result.id.c_str(), label.c_str(),
                    result.nanoseconds);
        if (result.net && !result.rawOnly) {
            std::printf("%.3f", *result.net);
        }
        std::printf(",%llu,%.3f,", static_cast<unsigned long long>(result.iterations), result.spread);
        if (result.raw) {
            std::printf("%.3f", *result.raw);
        }
        std::printf("\n");
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
            options.repetitions = static_cast<int>(std::clamp(std::strtol(arguments[++i], nullptr, 10), 1L, 1000L));
        } else {
            std::fprintf(stderr, "usage: unibind_bench [--csv] [--quick] [--target-ms N] [--repetitions N]\n");
            return 2;
        }
    }

    const ub::Platform platform;
    return Run(options);
}
