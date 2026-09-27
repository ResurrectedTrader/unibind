#pragma once
/// \file
/// What the lifetime cases (lifetimes_test.cpp, teardown_test.cpp) share.
///
/// tests/support/allocations.cpp replaces the global allocation operators for
/// this binary (see tests/python/CMakeLists.txt), which is how the frame- and
/// root-exhaustion paths are reached and how the backend's own C++ heap is
/// measured. CPython allocates through `malloc`, so none of that counts it.

#include <cstdint>
#include <string>
#include <string_view>

#include "support.h"

namespace ub_test {
// tests/support/allocations.cpp
[[nodiscard]] long long OutstandingAllocations() noexcept;
void FailNextAllocations(long long count, long long skip = 0) noexcept;
long long StopFailingAllocations() noexcept;
}  // namespace ub_test

namespace py_lifetimes {

/// A realm that has imported what the scripts here lean on.
struct Fixture : py_test::Fixture {
    Fixture() { REQUIRE(ub::Evaluate(context, "import gc, sys, weakref, unibind").has_value()); }
};

inline void Run(const ub::Context& context, std::string_view source) {
    (void)py_test::Eval(context, source);
}

inline ub::Local<ub::Function> Native(const ub::Context& context, ub::FunctionCallback callback,
                                      ub::CallbackData data = {}) {
    auto function = ub::Function::New(context, callback, data);
    REQUIRE(function.has_value());
    return *function;
}

/// `sys.getrefcount` of a global, as the script sees it. Only differences
/// between two readings mean anything.
inline std::int32_t RefCount(const ub::Context& context, std::string_view name) {
    return py_test::EvalInt(context, "sys.getrefcount(" + std::string(name) + ")");
}

/// Arms `count` allocation failures for the length of a block, and reports how
/// many fired - zero means the replacement operators did not take and the case
/// proved nothing.
class FailAllocations {
   public:
    explicit FailAllocations(long long count, long long skip = 0) noexcept {
        ub_test::FailNextAllocations(count, skip);
    }
    ~FailAllocations() { (void)Stop(); }
    FailAllocations(const FailAllocations&) = delete;
    FailAllocations& operator=(const FailAllocations&) = delete;
    FailAllocations(FailAllocations&&) = delete;
    FailAllocations& operator=(FailAllocations&&) = delete;
    long long Stop() noexcept {
        if (!stopped_) {
            fired_ = ub_test::StopFailingAllocations();
            stopped_ = true;
        }
        return fired_;
    }

   private:
    bool stopped_ = false;
    long long fired_ = 0;
};

}  // namespace py_lifetimes
