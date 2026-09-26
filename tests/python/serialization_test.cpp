// Structured clone: Serialize writes a value graph down, Deserialize builds it
// again - in another realm, another isolate, another thread.
//
// A clone's encoding is canonical (one graph, one blob), so the sharpest check
// of a round trip needs no script at all: serialize the clone and compare the
// bytes. Equal bytes mean the same values *and* the same shape - every shared
// reference and every cycle where it was.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include "data_support.h"

using py_test::Eval;
using py_test::EvalAs;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Fixture;
using py_test::Give;
using py_test::Run;
using py_test::TextOf;

namespace {

using Blob = std::vector<std::uint8_t>;

[[nodiscard]] Blob Encode(const ub::Context& context, const ub::Local<ub::Value>& value) {
    ub::TryCatch tc(context.GetIsolate());
    auto blob = ub::Serialize(context, value);
    if (!blob) {
        FAIL_CHECK("Serialize failed: " << (tc.HasCaught() ? tc.Message(context).value_or("?") : "nothing caught"));
        return {};
    }
    CHECK_FALSE(tc.HasCaught());
    return *std::move(blob);
}

[[nodiscard]] ub::Local<ub::Value> Decode(const ub::Context& context, const Blob& blob) {
    ub::TryCatch tc(context.GetIsolate());
    auto value = ub::Deserialize(context, blob);
    if (!value) {
        FAIL_CHECK("Deserialize failed: " << (tc.HasCaught() ? tc.Message(context).value_or("?") : "nothing caught"));
        return ub::Undefined(context.GetIsolate());
    }
    return *value;
}

/// Why `Serialize` refused `source`'s value: the caught message.
[[nodiscard]] std::string Refusal(const ub::Context& context, std::string_view source) {
    const auto value = Eval(context, source);
    ub::TryCatch tc(context.GetIsolate());
    CHECK_FALSE(ub::Serialize(context, value).has_value());
    REQUIRE(tc.HasCaught());
    return tc.Message(context).value_or("");
}

/// Clone `source`'s value into a second realm of the same isolate and check the
/// clone re-serializes to the same bytes and - unless it prints its address -
/// prints the same. The clone is left
/// in the second realm's globals as `clone` and the source as `src`, where the
/// objects area can put them there.
struct RoundTrip {
    RoundTrip(Fixture& f, std::string_view source, bool comparePrinted = true) : other(*ub::Context::New(f.iso())) {
        original = Eval(f.context, source);
        blob = Encode(f.context, original);
        REQUIRE_FALSE(blob.empty());
        clone = Decode(other, blob);
        CHECK(Encode(other, clone) == blob);
        if (comparePrinted) {
            CHECK(TextOf(other, clone) == TextOf(f.context, original));
        }
        exposed = Give(other, "clone", clone) && Give(other, "src", original);
    }

    ub::Context other;
    ub::Local<ub::Value> original;
    ub::Local<ub::Value> clone;
    Blob blob;
    bool exposed = false;
};

}  // namespace

TEST_CASE("clone: every primitive round-trips exactly") {
    Fixture f;
    Run(f.context, "import unibind");
    for (const char* source : {"None",
                               "unibind.null",
                               "True",
                               "False",
                               "0",
                               "-1",
                               "2**53 + 1",
                               "2**200",
                               "-(2**200)",
                               "1.5",
                               "float('inf')",
                               "float('-inf')",
                               "5e-324",
                               "''",
                               "'text'",
                               "'emoji \\U0001F600 and \\u00e9'",
                               "'lone \\ud800 surrogate'",
                               "b''",
                               "b'\\x00\\xff'",
                               "bytearray(b'\\x01\\x02')"}) {
        const std::string where = source;
        CAPTURE(where);
        const RoundTrip trip(f, source);
        CHECK(trip.clone.Kind() == trip.original.Kind());
        if (trip.exposed) {
            CHECK(EvalTruth(trip.other, "type(clone) is type(src) and clone == src"));
        }
    }
}

TEST_CASE("clone: NaN and negative zero keep their bits") {
    Fixture f;
    const RoundTrip nan(f, "float('nan')");
    CHECK(std::isnan(py_test::Narrow<ub::Number>(nan.clone).NumberValue()));
    const RoundTrip negativeZero(f, "-0.0");
    const double zero = py_test::Narrow<ub::Number>(negativeZero.clone).NumberValue();
    CHECK(zero == 0.0);
    CHECK(std::signbit(zero));
}

TEST_CASE("clone: a lone surrogate survives, where UTF-8 could not carry it") {
    Fixture f;
    const RoundTrip trip(f, "'a\\udc80b'");
    // Distinguishable from the U+FFFD any UTF-8 path would have made of it.
    const RoundTrip replaced(f, "'a\\ufffdb'");
    CHECK(trip.blob != replaced.blob);
    if (trip.exposed) {
        CHECK(EvalTruth(trip.other, "clone == 'a\\udc80b' and len(clone) == 3"));
    }
}

TEST_CASE("clone: containers round-trip, keys of every clonable kind included") {
    Fixture f;
    Run(f.context, "import unibind");
    for (const char* source : {"[]", "()", "{}", "[1, 'two', 3.0, None, [4, (5, 6)]]", "(1,)",
                               "{'a': 1, 2: 'b', (3, 'c'): [4], None: 5, 1.5: 6, b'k': 7, True: 8, unibind.null: 9}",
                               "{'nested': {'deeper': {'deepest': [bytearray(3)]}}}"}) {
        const std::string where = source;
        CAPTURE(where);
        const RoundTrip trip(f, source);
        if (trip.exposed) {
            // () is a singleton, and a clone of it is that same ()
            CHECK(EvalTruth(trip.other,
                            "type(clone) is type(src) and clone == src and (clone is not src or clone == ())"));
        }
    }
}

TEST_CASE("clone: shared references stay shared and cycles stay cycles") {
    Fixture f;
    Run(f.context, R"PY(
shared = [1, 2]
pair = [shared, shared, {'again': shared}]
loop = []
loop.append(loop)
selfish = {}
selfish['me'] = selfish
through_tuple = ([],)
through_tuple[0].append(through_tuple)
distinct = [[1, 2], [1, 2]]
)PY");
    const RoundTrip pair(f, "pair");
    const RoundTrip loop(f, "loop");
    const RoundTrip selfish(f, "selfish");
    const RoundTrip tuple(f, "through_tuple");
    const RoundTrip distinct(f, "distinct");
    // The same values unshared are a different graph, and a different blob.
    CHECK(distinct.blob != RoundTrip(f, "[shared, shared]").blob);
    CHECK(TextOf(loop.other, loop.clone) == "[[...]]");
    if (!pair.exposed || !loop.exposed || !selfish.exposed || !tuple.exposed) {
        return;
    }
    CHECK(EvalTruth(pair.other, "clone[0] is clone[1] is clone[2]['again'] and clone[0] is not src[0]"));
    CHECK(EvalTruth(loop.other, "clone[0] is clone"));
    CHECK(EvalTruth(selfish.other, "clone['me'] is clone"));
    CHECK(EvalTruth(tuple.other, "clone[0][0] is clone and type(clone) is tuple"));
    CHECK(EvalTruth(distinct.other, "clone[0] is not clone[1] and clone[0] == clone[1]"));
}

TEST_CASE("clone: typed arrays and data views keep their buffer, and share it") {
    Fixture f;
    Run(f.context, R"PY(
from unibind import TypedArray, DataView
buf = bytearray(range(16))
views = [TypedArray('int32', buf, 4, 2), DataView(buf, 2, 6), TypedArray('uint8', buf), buf]
kinds = [TypedArray(t, [1, 2]) for t in ('int8', 'uint8', 'uint8clamped', 'int16', 'uint16', 'int32',
                                         'uint32', 'float32', 'float64', 'bigint64', 'biguint64', 'float16')]
readonly = TypedArray('uint16', b'\x01\x02\x03\x04', 2)
)PY");
    const RoundTrip views(f, "views");
    const RoundTrip kinds(f, "kinds");
    const RoundTrip readonly(f, "readonly");
    const auto view = EvalAs<ub::TypedArray>(f.context, "views[0]");
    const RoundTrip single(f, "views[0]");
    const auto cloned = py_test::Narrow<ub::TypedArray>(single.clone);
    CHECK(ub::GetElementType(cloned) == ub::ElementType::Int32);
    CHECK(ub::Length(cloned) == 2);
    CHECK(ub::ByteOffset(cloned) == 4);
    std::array<std::int32_t, 2> before{};
    std::array<std::int32_t, 2> after{};
    CHECK(ub::CopyElements(view, std::span<std::int32_t>(before)) == 2);
    CHECK(ub::CopyElements(cloned, std::span<std::int32_t>(after)) == 2);
    CHECK(before == after);
    // The whole buffer travels with a view, not only its window.
    auto buffer = ub::GetBuffer(single.other, cloned);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 16);

    if (!views.exposed || !kinds.exposed || !readonly.exposed) {
        return;
    }
    CHECK(EvalTruth(views.other, "all(v.buffer is clone[3] for v in clone[:3]) and clone[3] is not src[3]"));
    CHECK(EvalTruth(views.other,
                    "clone[1].setUint16(0, 0xBEEF)\nclone[2][2:4].tolist() == [0xBE, 0xEF] and src[3][2] == 2"));
    CHECK(EvalTruth(views.other,
                    "clone[0].tolist() == src[0].tolist() and clone[1].byteOffset == 2 and clone[1].byteLength == 6"));
    CHECK(EvalTruth(kinds.other,
                    "[k.type for k in clone] == [k.type for k in src] and all(c.tolist() == s.tolist() for c, s in "
                    "zip(clone, src))"));
    CHECK(EvalTruth(readonly.other, "type(clone.buffer) is bytes and clone.tolist() == [0x0403]"));
}

TEST_CASE("clone: a TypedArray made in C++ round-trips") {
    Fixture f;
    const std::array<double, 3> values{0.5, -0.0, 1e300};
    auto view = ub::TypedArray::New(f.context, std::span<const double>(values));
    REQUIRE(view.has_value());
    const Blob blob = Encode(f.context, *view);
    const auto clone = py_test::Narrow<ub::TypedArray>(Decode(f.context, blob));
    std::array<double, 3> out{};
    CHECK(ub::CopyElements(clone, std::span<double>(out)) == 3);
    // Bit for bit, which is what makes -0.0 a case: it equals 0.0 as a double.
    const auto bits = [](double d) { return std::bit_cast<std::uint64_t>(d); };
    CHECK(std::ranges::equal(out, values, {}, bits, bits));
    CHECK_FALSE(clone.StrictEquals(*view));
}

TEST_CASE("clone: a view out of bounds of its shrunk buffer is refused") {
    Fixture f;
    Run(f.context, "from unibind import TypedArray\nb = bytearray(8)\nv = TypedArray('int32', b, 4)\ndel b[:]");
    CHECK(Refusal(f.context, "v").find("DataCloneError") != std::string::npos);
    CHECK(Refusal(f.context, "[1, v]").find("DataCloneError") != std::string::npos);
}

TEST_CASE("clone: what cannot be cloned fails the whole call, with DataCloneError pending") {
    Fixture f;
    Run(f.context, R"PY(
import unibind, collections
class Thing:
    pass
)PY");
    for (const char* source : {"lambda: 1", "len", "Thing", "Thing()", "unibind.Symbol('s')", "{1, 2}", "frozenset()",
                               "memoryview(b'x')", "collections.OrderedDict()", "[1, [2, [3, lambda: 4]]]",
                               "{'k': {'deeper': print}}", "{unibind.Symbol('key'): 1}", "type('S', (str,), {})('sub')",
                               "True.__class__", "range(3)", "iter([])", "1j", "Ellipsis"}) {
        const std::string where = source;
        CAPTURE(where);
        const std::string message = Refusal(f.context, source);
        CHECK(message.find("DataCloneError") != std::string::npos);
    }
    CHECK(Refusal(f.context, "lambda: 1").find("a function could not be cloned") != std::string::npos);
    CHECK(Refusal(f.context, "unibind.Symbol('s')").find("a Symbol could not be cloned") != std::string::npos);
    CHECK(Refusal(f.context, "Thing()").find("'Thing'") != std::string::npos);
    CHECK(EvalTruth(f.context, "issubclass(unibind.DataCloneError, unibind.Error)"));

    int data = 0;
    auto external = ub::External::New(f.iso(), data);
    REQUIRE(external.has_value());
    ub::TryCatch tc(f.iso());
    CHECK_FALSE(ub::Serialize(f.context, *external).has_value());
    REQUIRE(tc.HasCaught());
    CHECK(tc.Message(f.context).value_or("").find("an External could not be cloned") != std::string::npos);
}

TEST_CASE("clone: a plain unibind.Object clones its own properties into a fresh one") {
    Fixture f;
    if (!EvalTruth(f.context, "import unibind\nhasattr(unibind, 'Object')")) {
        MESSAGE("unibind.Object is not available; skipped");
        return;
    }
    Run(f.context, "o = unibind.Object()\no.name = 'x'\no.count = 3\no.inner = [1, o]");
    const RoundTrip trip(f, "o", false);
    if (!trip.exposed) {
        return;
    }
    CHECK(EvalTruth(trip.other, "import unibind\ntype(clone) is unibind.Object and clone is not src"));
    CHECK(EvalTruth(trip.other, "clone.name == 'x' and clone.count == 3 and clone.inner[1] is clone"));
}

TEST_CASE("clone: a blob moves to an isolate on another thread") {
    Blob blob;
    {
        Fixture a;
        Run(a.context, R"PY(
from unibind import TypedArray
buf = bytearray(b'shared bytes')
payload = {'text': 'héllo', 'big': 2**100, 'views': [TypedArray('uint8', buf), TypedArray('int16', buf, 2, 3)],
           'nested': [[1.5, None, True]]}
payload['self'] = payload
)PY");
        blob = Encode(a.context, Eval(a.context, "payload"));
    }
    // The isolate that wrote it is gone; the blob is only bytes.
    REQUIRE_FALSE(blob.empty());

    bool built = false;
    bool sameBytes = false;
    std::string text;
    std::string checks;
    std::thread worker([&] {
        auto isolate = ub::Isolate::New();
        if (!isolate) {
            return;
        }
        {
            ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            if (!context) {
                return;
            }
            ub::ContextScope entered(*context);
            ub::TryCatch tc(*isolate);
            auto value = ub::Deserialize(*context, blob);
            built = value.has_value();
            if (value) {
                auto again = ub::Serialize(*context, *value);
                sameBytes = again.has_value() && *again == blob;
                if (auto string = value->ToString(*context)) {
                    text = string->Utf8Value();
                }
                if (context->GlobalObject().Set(*context, "payload", *value).value_or(false)) {
                    auto result = ub::Evaluate(*context,
                                               "payload['self'] is payload and payload['views'][0].buffer is "
                                               "payload['views'][1].buffer and bytes(payload['views'][0]) == "
                                               "b'shared bytes' and payload['big'] == 2**100");
                    checks = result && result->IsTrue() ? "ok" : "failed";
                } else {
                    checks = "skipped";
                }
            }
        }
    });
    worker.join();
    CHECK(built);
    CHECK(sameBytes);
    CHECK(text.find("'text': 'h\xc3\xa9llo'") != std::string::npos);
    CHECK(text.find("'self': {...}") != std::string::npos);
    // "skipped": Object::Set is not there to put the value in script's hands.
    CHECK((checks == "ok" || checks == "skipped"));
}

TEST_CASE("clone: two isolates on two threads clone into each other at once") {
    // Each thread serializes in its own isolate and deserializes what the other
    // wrote: the blob is the only thing that crosses.
    Blob fromA;
    Blob fromB;
    std::atomic<int> ready{0};
    bool okA = false;
    bool okB = false;
    const auto run = [&ready](const char* source, Blob& mine, const Blob& theirs, bool& ok) {
        auto isolate = ub::Isolate::New();
        if (!isolate) {
            return;
        }
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return;
        }
        ub::ContextScope entered(*context);
        for (int round = 0; round < 50; ++round) {
            auto value = ub::Evaluate(*context, source);
            auto blob = value ? ub::Serialize(*context, *value) : std::nullopt;
            if (!blob) {
                return;
            }
            if (round == 0) {
                mine = *blob;
                ++ready;
                while (ready.load() < 2) {
                    std::this_thread::yield();
                }
            }
            auto clone = ub::Deserialize(*context, theirs);
            if (!clone) {
                return;
            }
        }
        ok = true;
    };
    std::thread a([&] { run("[list(range(100)), {'a': 'b'}]", fromA, fromB, okA); });
    std::thread b([&] { run("{'x': (1.5, b'bytes'), 'y': [None] * 10}", fromB, fromA, okB); });
    a.join();
    b.join();
    CHECK(okA);
    CHECK(okB);
}

TEST_CASE("clone: a damaged blob is refused, never misread or crashed on") {
    Fixture f;
    Run(f.context, R"PY(
from unibind import TypedArray, DataView
b = bytearray(range(8))
value = {'list': [1, 2.5, 'three', None], 'view': TypedArray('int16', b, 2, 2), 'dv': DataView(b), 'tuple': (b, 2**70)}
value['me'] = value
)PY");
    const Blob good = Encode(f.context, Eval(f.context, "value"));
    REQUIRE(good.size() > 40);
    CHECK(TextOf(f.context, Decode(f.context, good)).starts_with("{'list'"));

    const auto refused = [&f](const Blob& blob) {
        ub::TryCatch tc(f.iso());
        const bool none = !ub::Deserialize(f.context, blob).has_value();
        const bool caught = tc.HasCaught();
        const std::string message = caught ? tc.Message(f.context).value_or("") : "";
        return none && caught && message.find("DataCloneError") != std::string::npos;
    };

    CHECK(refused({}));
    CHECK(refused({1, 2, 3}));
    // Every truncation, and the blob with a byte appended.
    for (std::size_t length = 0; length < good.size(); ++length) {
        CAPTURE(length);
        CHECK(refused(Blob(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length))));
    }
    Blob longer = good;
    longer.push_back(0);
    CHECK(refused(longer));
    // Every single-bit flip, header and payload alike.
    for (std::size_t bit = 0; bit < good.size() * 8; ++bit) {
        Blob flipped = good;
        flipped[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
        CAPTURE(bit);
        CHECK(refused(flipped));
    }
    // Random garbage, some of it wearing the right magic.
    std::mt19937 random(12345);  // NOLINT(cert-msc32-c,cert-msc51-cpp): a failure has to be reproducible
    for (int round = 0; round < 500; ++round) {
        Blob noise(static_cast<std::size_t>(random() % 200));
        for (auto& byte : noise) {
            byte = static_cast<std::uint8_t>(random());
        }
        if (round % 2 == 0 && noise.size() >= 8) {
            std::memcpy(noise.data(), good.data(), 8);
        }
        CAPTURE(round);
        CHECK(refused(noise));
    }
    // And the good blob still reads after all that.
    CHECK(TextOf(f.context, Decode(f.context, good)) == TextOf(f.context, Eval(f.context, "value")));
}

TEST_CASE("clone: a well-framed payload that is not a clone is refused before anything is built") {
    // A forger with the header format - here, the test - can frame any marshal
    // data at all. Each of these is checked node by node and refused; none of
    // them may crash, and none may run anything.
    Fixture f;
    const Blob good = Encode(f.context, Eval(f.context, "[1]"));
    REQUIRE(good.size() > 32);
    const auto frame = [&good](const ub::Local<ub::ArrayBuffer>& marshalled) {
        std::vector<std::byte> payload(ub::ByteLength(marshalled));
        CHECK(ub::CopyBytes(marshalled, payload) == payload.size());
        Blob blob(good.begin(), good.begin() + 32);  // magic, format and versions as written
        const std::uint64_t length = payload.size();
        const std::uint64_t hash = ub::detail::CodeCachePayloadHash(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
        std::memcpy(blob.data() + 16, &length, 8);
        std::memcpy(blob.data() + 24, &hash, 8);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
        blob.insert(blob.end(), bytes, bytes + payload.size());
        return blob;
    };
    Run(f.context, "import marshal\nprobe = []\nclass Boom:\n    pass");
    // The frame itself is right: a hand-made payload of a legal graph reads.
    {
        const auto legal = EvalAs<ub::ArrayBuffer>(f.context, "marshal.dumps((0, [(10, [1, 1]), (5, 'hi')]), 2)");
        auto value = ub::Deserialize(f.context, frame(legal));
        REQUIRE(value.has_value());
        CHECK(TextOf(f.context, *value) == "['hi', 'hi']");
    }
    for (const char* source : {
             "marshal.dumps(None, 2)",
             "marshal.dumps((0,), 2)",
             "marshal.dumps((0, []), 2)",  // root out of range
             "marshal.dumps((1, [(0,)]), 2)",
             "marshal.dumps((-1, [(0,)]), 2)",
             "marshal.dumps((0, [(99,)]), 2)",                          // unknown tag
             "marshal.dumps((0, [(0, 1)]), 2)",                         // wrong arity
             "marshal.dumps((0, [(2, 1)]), 2)",                         // a bool that is an int
             "marshal.dumps((0, [(10, [5])]), 2)",                      // child out of range
             "marshal.dumps((0, [(10, (0,))]), 2)",                     // children not a list
             "marshal.dumps((0, [(11, [0])]), 2)",                      // a tuple holding itself
             "marshal.dumps((0, [(11, [1]), (11, [0])]), 2)",           // two tuples holding each other
             "marshal.dumps((0, [(12, [0])]), 2)",                      // odd pair list
             "marshal.dumps((0, [(12, [1, 1]), (10, [])]), 2)",         // unhashable key: TypeError, cleanly
             "marshal.dumps((0, [(8, 5, 1, 0, 1), (7, b'ab')]), 2)",    // view past its buffer
             "marshal.dumps((0, [(8, 3, 1, 1, 0), (7, b'abcd')]), 2)",  // misaligned view
             "marshal.dumps((0, [(8, 12, 1, 0, 0), (7, b'')]), 2)",     // no such element type
             "marshal.dumps((0, [(8, 1, 0, 0, 0)]), 2)",                // a view over a view
             "marshal.dumps((0, [(9, 1, 0, 3), (5, 'abc')]), 2)",       // a view over a string
             "marshal.dumps((0, [(13, [1, 1]), (10, [])]), 2)",         // an object key that is a list
             "marshal.dumps((0, [(5, b'bytes')]), 2)",                  // a str that is bytes
             "marshal.dumps((0, [(3, 1.5)]), 2)",                       // an int that is a float
             "marshal.dumps((0, [(4, compile('probe.append(1)', 'x', 'exec'))]))",  // code, never run
             "marshal.dumps(compile('probe.append(1)', 'x', 'exec'))",
         }) {
        const std::string where = source;
        CAPTURE(where);
        const auto marshalled = EvalAs<ub::ArrayBuffer>(f.context, source);
        ub::TryCatch tc(f.iso());
        CHECK_FALSE(ub::Deserialize(f.context, frame(marshalled)).has_value());
        CHECK(tc.HasCaught());
    }
    CHECK(EvalTruth(f.context, "probe == []"));
}

TEST_CASE("clone: deep nesting costs heap, not stack") {
    Fixture f;
    // Deep enough that recursion would blow the stack. Not as deep on x86,
    // where a graph this size and its blob take a real share of a 32-bit
    // address space the rest of the suite also needs.
    const std::string depth = sizeof(void*) == 8 ? "200000" : "50000";
    Run(f.context,
        "deep = []\ncursor = deep\nfor _ in range(" + depth + "):\n    cursor.append([])\n    cursor = cursor[0]");
    const Blob blob = Encode(f.context, Eval(f.context, "deep"));
    const auto clone = Decode(f.context, blob);
    CHECK(Encode(f.context, clone) == blob);
}

TEST_CASE("clone: a deserialized graph owes nothing to the realm or isolate that wrote it") {
    Blob blob;
    {
        Fixture a;
        blob = Encode(a.context, Eval(a.context, "{'k': [bytearray(b'abc'), 'v']}"));
    }
    Fixture b;
    ub::Global<ub::Value> kept;
    {
        ub::HandleScope scope(b.iso());
        auto other = ub::Context::New(b.iso());
        REQUIRE(other.has_value());
        kept = ub::Global<ub::Value>(b.iso(), Decode(*other, blob));
    }
    Run(b.context, "import gc\ngc.collect()");
    CHECK(TextOf(b.context, kept.Get(b.iso())) == "{'k': [bytearray(b'abc'), 'v']}");
    kept.Reset();
}
