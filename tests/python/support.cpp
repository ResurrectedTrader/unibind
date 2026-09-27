#include "support.h"

#include <atomic>

namespace py_test {

namespace {

std::unique_ptr<ub::Isolate> MakeIsolate() {
    auto isolate = ub::Isolate::New();
    REQUIRE(isolate != nullptr);
    return isolate;
}

ub::Context MakeContext(ub::Isolate& isolate) {
    auto context = ub::Context::New(isolate);
    REQUIRE(context.has_value());
    return *std::move(context);
}

}  // namespace

Fixture::Fixture() : isolate(MakeIsolate()), scope(*isolate), context(MakeContext(*isolate)), entered(context) {}

ub::Local<ub::Value> Eval(const ub::Context& context, std::string_view source) {
    ub::Isolate& isolate = context.GetIsolate();
    ub::EscapableHandleScope scope(isolate);
    ub::TryCatch tc(isolate);
    auto result = ub::Evaluate(context, source, {.resourceName = "test.py"});
    if (!result) {
        const std::string message = tc.HasCaught() ? tc.Message(context).value_or("<no message>") : "<nothing caught>";
        FAIL_CHECK("evaluating `" << source << "` threw: " << message);
        return scope.Escape(ub::Undefined(isolate));
    }
    return scope.Escape(*result);
}

std::int32_t EvalInt(const ub::Context& context, std::string_view source) {
    const auto value = Eval(context, source);
    const auto number = value.To<ub::Integer>();
    REQUIRE(number.has_value());
    return number->Int32Value();
}

double EvalNumber(const ub::Context& context, std::string_view source) {
    const auto value = Eval(context, source);
    const auto number = value.To<ub::Number>();
    REQUIRE(number.has_value());
    return number->NumberValue();
}

std::string EvalText(const ub::Context& context, std::string_view source) {
    return TextOf(context, Eval(context, source));
}

bool EvalTruth(const ub::Context& context, std::string_view source) {
    const auto value = Eval(context, source);
    const auto boolean = value.To<ub::Boolean>();
    REQUIRE(boolean.has_value());
    return boolean->BooleanValue();
}

std::string EvalError(const ub::Context& context, std::string_view source) {
    ub::Isolate& isolate = context.GetIsolate();
    const ub::HandleScope scope(isolate);
    ub::TryCatch tc(isolate);
    const auto result = ub::Evaluate(context, source, {.resourceName = "test.py"});
    CHECK_FALSE(result.has_value());
    REQUIRE(tc.HasCaught());
    return tc.Message(context).value_or("");
}

ub::Local<ub::String> Str(ub::Isolate& isolate, std::string_view utf8) {
    auto string = ub::String::NewFromUtf8(isolate, utf8);
    REQUIRE(string.has_value());
    return *string;
}

std::string TextOf(const ub::Context& context, const ub::Local<ub::Value>& value) {
    if (auto string = value.To<ub::String>()) {
        return string->Utf8Value();
    }
    auto converted = value.ToString(context);
    REQUIRE(converted.has_value());
    return converted->Utf8Value();
}

}  // namespace py_test

namespace py_test {

namespace {
std::atomic<std::uint64_t> gOutOfMemoryReports{0};
}  // namespace

void OnEngineFault(const ub::EngineFaultReport& report, ub::CallbackData /*data*/) {
    if (report.fault == ub::EngineFault::OutOfMemory) {
        gOutOfMemoryReports.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t OutOfMemoryReports() noexcept {
    return gOutOfMemoryReports.load(std::memory_order_relaxed);
}

}  // namespace py_test
