// Isolates on many threads at once: each thread makes an isolate, a realm,
// evaluates something trivial and destroys it all, over and over, while the
// others do the same. Nothing is shared between them but CPython's runtime -
// which is exactly what this is about.

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "support.h"

using py_test::EvalInt;

namespace {

/// Rounds per thread, overridable for soak runs: `UNIBIND_CONCURRENCY_ROUNDS`.
int Rounds(int fallback) {
    char* text = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&text, &length, "UNIBIND_CONCURRENCY_ROUNDS") == 0 && text != nullptr) {
        const long rounds = std::strtol(text, nullptr, 10);
        std::free(text);
        if (rounds > 0) {
            return static_cast<int>(rounds);
        }
    }
    return fallback;
}

/// `threads` threads, released together, each running `rounds` isolates one
/// after another, every one evaluating `source` and checking it gave `expected`.
int Churn(int threads, int rounds, const char* source, int expected) {
    std::atomic<int> ready{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> running;
    running.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        running.emplace_back([&] {
            ready.fetch_add(1);
            while (ready.load() < threads) {
                std::this_thread::yield();
            }
            for (int round = 0; round < rounds; ++round) {
                auto isolate = ub::Isolate::New();
                if (!isolate) {
                    failures.fetch_add(1);
                    continue;
                }
                {
                    const ub::HandleScope scope(*isolate);
                    auto context = ub::Context::New(*isolate);
                    if (!context) {
                        failures.fetch_add(1);
                        continue;
                    }
                    const ub::ContextScope entered(*context);
                    if (EvalInt(*context, source) != expected) {
                        failures.fetch_add(1);
                    }
                }
                isolate.reset();
            }
        });
    }
    for (auto& thread : running) {
        thread.join();
    }
    return failures.load();
}

}  // namespace

TEST_CASE("concurrency: eight threads making, using and destroying isolates at once") {
    CHECK(Churn(8, Rounds(6), "1 + 1", 2) == 0);
}

TEST_CASE("concurrency: two threads making, using and destroying isolates at once") {
    CHECK(Churn(2, Rounds(10), "1 + 1", 2) == 0);
}

TEST_CASE("concurrency: sixteen threads running a little of everything at once") {
    // Imports, classes, closures, generators, exceptions and comprehensions:
    // the paths that specialise bytecode and fill caches.
    static const char* const SOURCE =
        "import json, re, collections\n"
        "class P:\n"
        "    def __init__(self, x): self.x = x\n"
        "    def get(self): return self.x\n"
        "def gen(n):\n"
        "    for i in range(n): yield P(i).get()\n"
        "try:\n"
        "    raise ValueError('x')\n"
        "except ValueError:\n"
        "    pass\n"
        "d = collections.Counter(re.findall(r'\\w', json.dumps({'a': [1, 2, 3]})))\n"
        "sum(gen(100)) - 4950 + len(d)";
    CHECK(Churn(16, Rounds(3), SOURCE, 4) == 0);
}
