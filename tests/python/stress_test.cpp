// Stress cases: too slow, or too hard on the machine, for every run. Skipped
// unless asked for by name: `unibind_python_tests -tc="stress:*" --no-skip`.

#include <windows.h>

#include <psapi.h>

#include <cstdio>

#include "support.h"

TEST_CASE("stress: process memory over many isolates made and destroyed" * doctest::skip()) {
    // What an isolate that is gone still costs the process. Each is made,
    // used the way the lifetime case uses one, and destroyed; the process's
    // private bytes are printed every 50. A figure that climbs by the same
    // amount every batch is a leak; one that levels off is the heap's own
    // high-water mark. CPython 3.12 kept a sub-interpreter's arenas, and this
    // climbed by about 9.5 MB an isolate; 3.14 frees them - provided the
    // interpreter ends with no block still allocated - and it levels off at a
    // few megabytes (docs/python.md, section 11).
    const auto privateBytes = [] {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters));
        return static_cast<long long>(counters.PrivateUsage);
    };
    const long long start = privateBytes();
    for (int made = 1; made <= 1000; ++made) {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        {
            const ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            REQUIRE(context.has_value());
            const ub::ContextScope entered(*context);
            CHECK(py_test::EvalInt(*context, "len([str(i) for i in range(10000)])") == 10000);
        }
        isolate.reset();
        if (made % 50 == 0) {
            std::printf("stress: %d isolates made and destroyed, private bytes +%lld KB\n", made,
                        (privateBytes() - start) / 1024);
            std::fflush(stdout);
        }
    }
}

