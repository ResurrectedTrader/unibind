#pragma once
/// \file
/// What every case in the CPython backend's suite may lean on.
///
/// The same discipline as tests/support/harness.h: nothing here or in a case
/// names a CPython type or includes a CPython header. The suite is written
/// against the public `ub::` API, with Python as the script language - which is
/// the whole of what makes it this backend's suite rather than the shared one.

#include <doctest/doctest.h>

#include <memory>
#include <ostream>
#include <string>
#include <string_view>

#include "unibind/unibind.h"

namespace py_test {

/// An isolate, a scope, a realm, and the realm entered: what nearly every case
/// starts from. Members are declared in destruction order's reverse, so the
/// realm and the scope go before the isolate, as `unibind/isolate.h` requires.
struct Fixture {
    Fixture();

    [[nodiscard]] ub::Isolate& iso() const noexcept { return *isolate; }

    std::unique_ptr<ub::Isolate> isolate;
    ub::HandleScope scope;
    ub::Context context;
    ub::ContextScope entered;
};

/// Evaluate Python source in `context`. A trailing expression statement is the
/// result; a script without one evaluates to None (undefined). Fails the case
/// - with the exception's message - if it throws.
[[nodiscard]] ub::Local<ub::Value> Eval(const ub::Context& context, std::string_view source);

[[nodiscard]] std::int32_t EvalInt(const ub::Context& context, std::string_view source);
[[nodiscard]] double EvalNumber(const ub::Context& context, std::string_view source);
[[nodiscard]] std::string EvalText(const ub::Context& context, std::string_view source);
[[nodiscard]] bool EvalTruth(const ub::Context& context, std::string_view source);

/// Evaluate source that is expected to throw, and hand back the exception's
/// message (`TryCatch::Message`). Fails the case if nothing is thrown.
[[nodiscard]] std::string EvalError(const ub::Context& context, std::string_view source);

/// A string handle, failing the case if it cannot be made.
[[nodiscard]] ub::Local<ub::String> Str(ub::Isolate& isolate, std::string_view utf8);

/// The UTF-8 text of any value: a string's own, anything else's `str()`.
[[nodiscard]] std::string TextOf(const ub::Context& context, const ub::Local<ub::Value>& value);

/// Put `value` in the realm's globals under `name`.
template <class T>
void Expose(const ub::Context& context, std::string_view name, const ub::Local<T>& value) {
    REQUIRE(context.GlobalObject().Set(context, name, value).value_or(false));
}

}  // namespace py_test

namespace py_test {

/// The Platform's engine-fault handler for the whole suite (main.cpp). It
/// counts what arrives, per kind, and allocates nothing.
void OnEngineFault(const ub::EngineFaultReport& report, ub::CallbackData data);

/// How many `EngineFault::OutOfMemory` reports have arrived, from any isolate.
[[nodiscard]] std::uint64_t OutOfMemoryReports() noexcept;

}  // namespace py_test
