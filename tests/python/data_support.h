#pragma once
/// \file
/// What the binary-data, structured-clone and code-cache cases share. The same
/// discipline as support.h: the public `ub::` API only, Python as the script.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "support.h"

namespace py_test {

/// Put `value` in the realm's globals under `name`, for script to look at.
///
/// This is the one direction these cases cannot take without the objects area
/// (`Object::Set` on the globals dictionary): a value made in C++ reaches script
/// through it and nothing else. Where that is not there yet, the case says so
/// and stops here rather than failing - everything it checked up to this point
/// still counts - and once it is there the rest runs. Script-made values read
/// back into C++ need nothing but `Evaluate`, so most cases are written that
/// way round.
template <class T>
[[nodiscard]] bool Give(const ub::Context& context, std::string_view name, const ub::Local<T>& value) {
    ub::TryCatch tc(context.GetIsolate());
    if (context.GlobalObject().Set(context, name, value).value_or(false)) {
        return true;
    }
    // Said once, not once per case: it is one fact about the build.
    static bool said = false;
    if (!std::exchange(said, true)) {
        MESSAGE("Object::Set on the globals is not available: cases stop where C++ would hand script a value");
    }
    return false;
}

/// Bytes, for comparing a copy out with what was put in.
[[nodiscard]] inline std::vector<std::byte> Bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

/// All of a buffer's bytes.
[[nodiscard]] inline std::vector<std::byte> AllBytes(const ub::Local<ub::ArrayBuffer>& buffer) {
    std::vector<std::byte> out(ub::ByteLength(buffer));
    CHECK(ub::CopyBytes(buffer, out) == out.size());
    return out;
}

/// All of a view's bytes.
[[nodiscard]] inline std::vector<std::byte> AllBytes(const ub::Local<ub::ArrayBufferView>& view) {
    std::vector<std::byte> out(ub::ByteLength(view));
    CHECK(ub::CopyBytes(view, out) == out.size());
    return out;
}

/// Evaluate `source` and narrow the result to `T`, failing the case if it is
/// not one.
template <class T>
[[nodiscard]] ub::Local<T> EvalAs(const ub::Context& context, std::string_view source) {
    const auto value = Eval(context, source);
    auto narrowed = value.template To<T>();
    REQUIRE(narrowed.has_value());
    return *narrowed;
}

/// Evaluate `source` for its effect, failing the case if it throws.
inline void Run(const ub::Context& context, std::string_view source) {
    (void)Eval(context, source);
}

/// Narrow `value` to `T`, failing the case if it is not one.
template <class T, class U>
[[nodiscard]] ub::Local<T> Narrow(const ub::Local<U>& value) {
    auto narrowed = value.template To<T>();
    REQUIRE(narrowed.has_value());
    return *narrowed;
}

}  // namespace py_test
