// Binary data: ArrayBuffer is a bytearray, and unibind.TypedArray / DataView
// are typed windows onto one - made from C++ or from script, read from either.

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "data_support.h"

using py_test::AllBytes;
using py_test::Bytes;
using py_test::Eval;
using py_test::EvalAs;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::EvalTruth;
using py_test::Fixture;
using py_test::Give;
using py_test::Run;

// --- ArrayBuffer ---------------------------------------------------------------

TEST_CASE("binary: an ArrayBuffer holds a copy of the bytes, zero-filled after them") {
    Fixture f;
    const auto input = Bytes({1, 2, 3});
    auto buffer = ub::ArrayBuffer::New(f.context, 6);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 6);
    CHECK(AllBytes(*buffer) == Bytes({0, 0, 0, 0, 0, 0}));

    auto copied = ub::ArrayBuffer::New(f.context, std::span<const std::byte>(input));
    REQUIRE(copied.has_value());
    CHECK(AllBytes(*copied) == input);
    CHECK(copied->Is<ub::Object>());
    CHECK_FALSE(copied->Is<ub::ArrayBufferView>());

    // A copy out truncates to the destination and says how much it wrote.
    std::array<std::byte, 2> small{};
    CHECK(ub::CopyBytes(*copied, small) == 2);
    CHECK(small[1] == std::byte{2});
    std::array<std::byte, 8> large{};
    CHECK(ub::CopyBytes(*copied, large) == 3);

    if (!Give(f.context, "buf", *copied)) {
        return;
    }
    CHECK(EvalTruth(f.context, "type(buf) is bytearray and buf == b'\\x01\\x02\\x03'"));
}

TEST_CASE("binary: a buffer too large to allocate is empty, with nothing thrown") {
    Fixture f;
    ub::TryCatch tc(f.iso());
    CHECK_FALSE(ub::ArrayBuffer::New(f.context, std::numeric_limits<std::size_t>::max()).has_value());
    // Half the address space: more than any process can have, on x86 as on x64.
    CHECK_FALSE(ub::ArrayBuffer::New(f.context, std::numeric_limits<std::size_t>::max() / 2 + 1).has_value());
    CHECK_FALSE(tc.HasCaught());
}

TEST_CASE("binary: a bytearray and a bytes made by script are ArrayBuffers") {
    Fixture f;
    const auto mutable_ = EvalAs<ub::ArrayBuffer>(f.context, "bytearray(b'abc')");
    CHECK(AllBytes(mutable_) == Bytes({'a', 'b', 'c'}));
    const auto readOnly = EvalAs<ub::ArrayBuffer>(f.context, "b'xyz!'");
    CHECK(ub::ByteLength(readOnly) == 4);
    CHECK(AllBytes(readOnly) == Bytes({'x', 'y', 'z', '!'}));
    const auto empty = EvalAs<ub::ArrayBuffer>(f.context, "bytearray()");
    CHECK(ub::ByteLength(empty) == 0);
}

// --- TypedArray from C++ ---------------------------------------------------------

TEST_CASE("binary: a TypedArray made from a span carries its elements and type") {
    Fixture f;
    const std::array<std::int32_t, 3> values{1, -2, 2147483647};
    auto view = ub::TypedArray::New(f.context, std::span<const std::int32_t>(values));
    REQUIRE(view.has_value());
    CHECK(ub::GetElementType(*view) == ub::ElementType::Int32);
    CHECK(ub::Length(*view) == 3);
    CHECK(ub::ByteOffset(*view) == 0);
    CHECK(view->Is<ub::TypedArray>());
    CHECK(view->Is<ub::ArrayBufferView>());
    CHECK_FALSE(view->Is<ub::DataView>());
    CHECK_FALSE(view->Is<ub::ArrayBuffer>());

    std::array<std::int32_t, 3> out{};
    CHECK(ub::CopyElements(*view, std::span<std::int32_t>(out)) == 3);
    CHECK(out == values);
    // A different element type is refused, not converted.
    std::array<std::uint32_t, 3> wrong{};
    CHECK(ub::CopyElements(*view, std::span<std::uint32_t>(wrong)) == 0);
    // A short destination takes what fits.
    std::array<std::int32_t, 2> two{};
    CHECK(ub::CopyElements(*view, std::span<std::int32_t>(two)) == 2);
    CHECK(two[1] == -2);

    auto buffer = ub::GetBuffer(f.context, *view);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 12);

    if (!Give(f.context, "v", *view)) {
        return;
    }
    CHECK(EvalTruth(f.context, "import unibind\ntype(v) is unibind.TypedArray and v.type == 'int32'"));
    CHECK(EvalTruth(f.context, "v.tolist() == [1, -2, 2147483647]"));
}

TEST_CASE("binary: every element type crosses from C++ to script intact") {
    Fixture f;
    const auto make = [&f]<class T>(std::initializer_list<T> list) {
        const std::vector<T> values(list);
        auto view = ub::TypedArray::New(f.context, std::span<const T>(values));
        REQUIRE(view.has_value());
        CHECK(ub::GetElementType(*view) == ub::ElementTypeOf<T>);
        CHECK(ub::Length(*view) == values.size());
        std::vector<T> back(values.size());
        CHECK(ub::CopyElements(*view, std::span<T>(back)) == values.size());
        CHECK(back == values);
        return *view;
    };
    const auto i8 = make(std::initializer_list<std::int8_t>{-128, 127, 0});
    const auto u8 = make(std::initializer_list<std::uint8_t>{0, 255});
    const auto i16 = make(std::initializer_list<std::int16_t>{-32768, 32767});
    const auto u16 = make(std::initializer_list<std::uint16_t>{0, 65535});
    const auto i32 = make(std::initializer_list<std::int32_t>{INT32_MIN, INT32_MAX});
    const auto u32 = make(std::initializer_list<std::uint32_t>{0, UINT32_MAX});
    const auto f32 = make(std::initializer_list<float>{1.5F, -0.0F, std::numeric_limits<float>::infinity()});
    const auto f64 = make(std::initializer_list<double>{0.1, -1e308});
    const auto i64 = make(std::initializer_list<std::int64_t>{INT64_MIN, INT64_MAX});
    const auto u64 = make(std::initializer_list<std::uint64_t>{0, UINT64_MAX});

    if (!Give(f.context, "i8", i8) || !Give(f.context, "u8", u8) || !Give(f.context, "i16", i16) ||
        !Give(f.context, "u16", u16) || !Give(f.context, "i32", i32) || !Give(f.context, "u32", u32) ||
        !Give(f.context, "f32", f32) || !Give(f.context, "f64", f64) || !Give(f.context, "i64", i64) ||
        !Give(f.context, "u64", u64)) {
        return;
    }
    CHECK(EvalTruth(f.context, "i8.tolist() == [-128, 127, 0] and i8.type == 'int8'"));
    CHECK(EvalTruth(f.context, "u8.tolist() == [0, 255]"));
    CHECK(EvalTruth(f.context, "i16.tolist() == [-32768, 32767]"));
    CHECK(EvalTruth(f.context, "u16.tolist() == [0, 65535]"));
    CHECK(EvalTruth(f.context, "i32.tolist() == [-2**31, 2**31 - 1]"));
    CHECK(EvalTruth(f.context, "u32.tolist() == [0, 2**32 - 1]"));
    CHECK(EvalTruth(f.context,
                    "import math\nf32[0] == 1.5 and math.copysign(1, f32[1]) == -1 and f32[2] == float('inf')"));
    CHECK(EvalTruth(f.context, "f64.tolist() == [0.1, -1e308]"));
    CHECK(EvalTruth(f.context, "i64.tolist() == [-2**63, 2**63 - 1] and i64.type == 'bigint64'"));
    CHECK(EvalTruth(f.context, "u64.tolist() == [0, 2**64 - 1]"));
}

TEST_CASE("binary: a view that would not fit is refused, not clamped") {
    Fixture f;
    auto buffer = ub::ArrayBuffer::New(f.context, 16);
    REQUIRE(buffer.has_value());
    ub::TryCatch tc(f.iso());
    const auto view = [&](ub::ElementType type, std::size_t offset, std::size_t length) {
        return ub::TypedArray::New(f.context, type, *buffer, offset, length).has_value();
    };
    CHECK(view(ub::ElementType::Uint8, 0, 16));
    CHECK(view(ub::ElementType::Int32, 4, 3));
    CHECK(view(ub::ElementType::Float64, 16, 0));
    CHECK_FALSE(view(ub::ElementType::Uint8, 0, 17));
    CHECK_FALSE(view(ub::ElementType::Int32, 4, 4));
    CHECK_FALSE(view(ub::ElementType::Uint8, 17, 0));
    CHECK_FALSE(view(ub::ElementType::Int32, 2, 1));  // misaligned
    CHECK_FALSE(view(ub::ElementType::Float16, 1, 1));
    // A length whose byte count would wrap is refused before it is multiplied.
    CHECK_FALSE(view(ub::ElementType::Float64, 0, (std::numeric_limits<std::size_t>::max() / 8) + 3));
    CHECK_FALSE(view(ub::ElementType::Uint8, std::numeric_limits<std::size_t>::max(), 1));

    CHECK(ub::DataView::New(f.context, *buffer, 3, 13).has_value());
    CHECK_FALSE(ub::DataView::New(f.context, *buffer, 3, 14).has_value());
    CHECK_FALSE(ub::DataView::New(f.context, *buffer, 17, 0).has_value());
    CHECK_FALSE(ub::DataView::New(f.context, *buffer, std::numeric_limits<std::size_t>::max(), 2).has_value());
    CHECK_FALSE(tc.HasCaught());
}

TEST_CASE("binary: views made in C++ over one buffer see each other's writes") {
    Fixture f;
    const auto zeros = Bytes({0, 0, 0, 0, 0, 0, 0, 0});
    auto buffer = ub::ArrayBuffer::New(f.context, std::span<const std::byte>(zeros));
    REQUIRE(buffer.has_value());
    auto bytes = ub::TypedArray::New(f.context, ub::ElementType::Uint8, *buffer, 0, 8);
    auto words = ub::TypedArray::New(f.context, ub::ElementType::Uint16, *buffer, 2, 2);
    auto data = ub::DataView::New(f.context, *buffer, 4, 4);
    REQUIRE(bytes.has_value());
    REQUIRE(words.has_value());
    REQUIRE(data.has_value());
    CHECK(ub::ByteOffset(ub::Local<ub::ArrayBufferView>(*words)) == 2);
    CHECK(ub::ByteLength(ub::Local<ub::ArrayBufferView>(*words)) == 4);
    CHECK(ub::ByteLength(ub::Local<ub::ArrayBufferView>(*data)) == 4);
    CHECK(ub::ByteOffset(ub::Local<ub::ArrayBufferView>(*data)) == 4);

    if (!Give(f.context, "b", *bytes) || !Give(f.context, "w", *words) || !Give(f.context, "d", *data) ||
        !Give(f.context, "buf", *buffer)) {
        return;
    }
    Run(f.context, "w[0] = 0x0102\nd.setUint32(0, 0xAABBCCDD)\nbuf[7] ^= 0xFF");
    std::array<std::uint8_t, 8> out{};
    CHECK(ub::CopyElements(*bytes, std::span<std::uint8_t>(out)) == 8);
    // Little-endian through the Uint16Array, big-endian through the DataView
    // (its default), and the last byte flipped through the buffer itself.
    CHECK(out == std::array<std::uint8_t, 8>{0, 0, 2, 1, 0xAA, 0xBB, 0xCC, 0xDD ^ 0xFF});
    CHECK(AllBytes(ub::Local<ub::ArrayBufferView>(*data)) == Bytes({0xAA, 0xBB, 0xCC, 0x22}));
    // The buffer handed back is the same object script holds.
    auto again = ub::GetBuffer(f.context, *bytes);
    REQUIRE(again.has_value());
    CHECK(again->StrictEquals(*buffer));
}

// --- TypedArray from script ------------------------------------------------------

TEST_CASE("binary: a TypedArray made by script reads the same from C++") {
    Fixture f;
    Run(f.context, "import unibind\nbuf = bytearray(range(10))\nv = unibind.TypedArray('uint16', buf, 2, 3)");
    const auto view = EvalAs<ub::TypedArray>(f.context, "v");
    CHECK(ub::GetElementType(view) == ub::ElementType::Uint16);
    CHECK(ub::Length(view) == 3);
    CHECK(ub::ByteOffset(view) == 2);
    const ub::Local<ub::ArrayBufferView> any = view;
    CHECK(ub::ByteLength(any) == 6);
    CHECK(ub::ByteOffset(any) == 2);
    CHECK(AllBytes(any) == Bytes({2, 3, 4, 5, 6, 7}));
    std::array<std::uint16_t, 3> elements{};
    CHECK(ub::CopyElements(view, std::span<std::uint16_t>(elements)) == 3);
    CHECK(elements == std::array<std::uint16_t, 3>{0x0302, 0x0504, 0x0706});

    // The buffer under it is script's bytearray - the whole of it.
    auto buffer = ub::GetBuffer(f.context, view);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 10);
    CHECK(buffer->StrictEquals(Eval(f.context, "buf")));
    auto viaAny = ub::GetBuffer(f.context, any);
    REQUIRE(viaAny.has_value());
    CHECK(viaAny->StrictEquals(*buffer));
}

TEST_CASE("binary: a DataView made by script reads the same from C++") {
    Fixture f;
    Run(f.context, "import unibind\nd = unibind.DataView(bytearray(b'abcdefgh'), 1, 5)");
    const auto view = EvalAs<ub::DataView>(f.context, "d");
    CHECK(view.Is<ub::ArrayBufferView>());
    CHECK_FALSE(view.Is<ub::TypedArray>());
    const ub::Local<ub::ArrayBufferView> any = view;
    CHECK(ub::ByteOffset(any) == 1);
    CHECK(ub::ByteLength(any) == 5);
    CHECK(AllBytes(any) == Bytes({'b', 'c', 'd', 'e', 'f'}));
    std::array<std::byte, 3> three{};
    CHECK(ub::CopyBytes(any, three) == 3);
    CHECK(three[2] == std::byte{'d'});
    auto buffer = ub::GetBuffer(f.context, any);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 8);
}

TEST_CASE("binary: element conversions follow JavaScript at every boundary") {
    Fixture f;
    Run(f.context, R"PY(
import math, struct
from unibind import TypedArray

def put(t, v):
    a = TypedArray(t, 1)
    a[0] = v
    return a[0]

def same(a, b):
    return (math.isnan(a) and math.isnan(b)) or (a == b and math.copysign(1, a) == math.copysign(1, b))

nan, inf = float('nan'), float('inf')
)PY");
    // Integers wrap modulo 2^N, from ints of any size and from truncated floats.
    CHECK(EvalTruth(f.context,
                    "[put('int8', v) for v in (127, 128, 255, 256, -129, 2**70 + 5)] == [127, -128, -1, 0, 127, 5]"));
    CHECK(EvalTruth(f.context, "[put('int8', v) for v in (1.9, -1.9, nan, inf, -inf, -0.0)] == [1, -1, 0, 0, 0, 0]"));
    CHECK(EvalTruth(f.context, "[put('uint8', v) for v in (-1, 256, 257.99, 2**64 + 1)] == [255, 0, 1, 1]"));
    CHECK(
        EvalTruth(f.context, "[put('int16', v) for v in (32767, 32768, 65535, -32769)] == [32767, -32768, -1, 32767]"));
    CHECK(EvalTruth(f.context, "[put('uint16', v) for v in (-1, 65536, 70000)] == [65535, 0, 4464]"));
    CHECK(EvalTruth(f.context,
                    "[put('int32', v) for v in (2**31, 2**32 + 7, 4294967295.0, 1e20, -2**31 - 1)] == "
                    "[-2**31, 7, -1, 1661992960, 2**31 - 1]"));
    CHECK(EvalTruth(f.context,
                    "[put('uint32', v) for v in (-1, 2**32, 1e20, -1.5)] == [2**32 - 1, 0, 1661992960, 2**32 - 1]"));
    CHECK(EvalTruth(f.context, "put('uint8', True) == 1 and put('int32', False) == 0"));
    // Uint8Clamped clamps, and rounds a half to the even neighbour.
    CHECK(EvalTruth(f.context, "[put('uint8clamped', v) for v in (300, -5, 2**100, -2**100)] == [255, 0, 255, 0]"));
    CHECK(EvalTruth(
        f.context,
        "[put('uint8clamped', v) for v in (0.5, 1.5, 2.5, 3.49, 3.51, 253.5, 254.5, 254.6, nan, inf, -inf)] == "
        "[0, 2, 2, 3, 4, 254, 254, 255, 0, 255, 0]"));
    // Float32 rounds to nearest; past the last float it overflows as rounding would.
    CHECK(EvalTruth(f.context, "put('float32', 0.1) == struct.unpack('f', struct.pack('f', 0.1))[0]"));
    CHECK(EvalTruth(f.context, "[put('float32', v) for v in (1e40, -1e40)] == [inf, -inf]"));
    CHECK(
        EvalTruth(f.context, "put('float32', 3.4028235e38) == struct.unpack('f', struct.pack('f', 3.4028235e38))[0]"));
    // FLT_MAX + half an ulp is the tie: below it FLT_MAX, at it even - which is infinity.
    CHECK(EvalTruth(f.context, "put('float32', 2.0**128 - 2.0**103 - 2.0**80) == 3.4028234663852886e38"));
    CHECK(EvalTruth(f.context, "put('float32', 2.0**128 - 2.0**103) == inf"));
    CHECK(EvalTruth(f.context, "same(put('float32', -0.0), -0.0) and same(put('float32', nan), nan)"));
    CHECK(EvalTruth(f.context, "put('float32', 2**200) == inf and put('float32', 3) == 3.0"));
    // Float64 is exact.
    CHECK(EvalTruth(f.context, "all(same(put('float64', v), v) for v in (0.1, -0.0, nan, inf, 5e-324))"));
    CHECK(EvalTruth(f.context, "put('float64', 2**53 + 1) == 2.0**53"));  // an int rounds, as a double must
    // Float16, as struct's 'e' packs it - until the range ends, where it is infinity.
    CHECK(EvalTruth(f.context,
                    "all(put('float16', v) == struct.unpack('e', struct.pack('e', v))[0] "
                    "for v in (1.0, 0.1, -2.5, 65504, 65519.99, 6e-8, 1e-8))"));
    CHECK(EvalTruth(f.context, "[put('float16', v) for v in (65520, 1e6, -1e6, inf)] == [inf, inf, -inf, inf]"));
    CHECK(EvalTruth(f.context, "same(put('float16', -0.0), -0.0) and same(put('float16', nan), nan)"));
    // The BigInt types take integers only, wrapping modulo 2^64.
    CHECK(EvalTruth(f.context,
                    "[put('bigint64', v) for v in (2**63, -1, 2**64 + 3, -2**63 - 1)] == [-2**63, -1, 3, 2**63 - 1]"));
    CHECK(EvalTruth(f.context, "[put('biguint64', v) for v in (-1, 2**64, 2**100 + 9)] == [2**64 - 1, 0, 9]"));
    CHECK(py_test::EvalError(f.context, "put('bigint64', 1.0)").starts_with("TypeError"));
    // A string is not a number here, as it is not to array.array.
    CHECK(py_test::EvalError(f.context, "put('int8', '5')").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "put('float64', None)").starts_with("TypeError"));
    // __index__ and __float__ are asked.
    CHECK(EvalTruth(f.context, R"PY(
class I:
    def __index__(self): return 300
class F:
    def __float__(self): return 2.75
put('uint8', I()) == 44 and put('float64', F()) == 2.75 and put('int8', F()) == 2
)PY"));
}

TEST_CASE("binary: what script writes is what C++ reads, for every element type") {
    Fixture f;
    Run(f.context, "from unibind import TypedArray");
    const auto read = [&f]<class T>(std::string_view source, std::initializer_list<T> expected) {
        const auto view = EvalAs<ub::TypedArray>(f.context, source);
        std::vector<T> out(ub::Length(view));
        CHECK(ub::CopyElements(view, std::span<T>(out)) == out.size());
        CHECK(out == std::vector<T>(expected));
    };
    read("TypedArray('int8', [127, 128, -129])", std::initializer_list<std::int8_t>{127, -128, 127});
    read("TypedArray('uint8', [255, 256, -1])", std::initializer_list<std::uint8_t>{255, 0, 255});
    read("TypedArray('int16', [32768])", std::initializer_list<std::int16_t>{-32768});
    read("TypedArray('uint16', [-2])", std::initializer_list<std::uint16_t>{65534});
    read("TypedArray('int32', [2**31, 1e20])", std::initializer_list<std::int32_t>{INT32_MIN, 1661992960});
    read("TypedArray('uint32', [-1])", std::initializer_list<std::uint32_t>{UINT32_MAX});
    read("TypedArray('float32', [0.5, 1e40])",
         std::initializer_list<float>{0.5F, std::numeric_limits<float>::infinity()});
    read("TypedArray('float64', [0.1, -2.5])", std::initializer_list<double>{0.1, -2.5});
    read("TypedArray('bigint64', [2**63, -5])", std::initializer_list<std::int64_t>{INT64_MIN, -5});
    read("TypedArray('biguint64', [-1])", std::initializer_list<std::uint64_t>{UINT64_MAX});

    // Uint8Clamped is its own element type: a uint8_t copy is a type mismatch.
    const auto clamped = EvalAs<ub::TypedArray>(f.context, "TypedArray('Uint8ClampedArray', [1.5, 300])");
    CHECK(ub::GetElementType(clamped) == ub::ElementType::Uint8Clamped);
    std::array<std::uint8_t, 2> none{};
    CHECK(ub::CopyElements(clamped, std::span<std::uint8_t>(none)) == 0);
    CHECK(AllBytes(ub::Local<ub::ArrayBufferView>(clamped)) == Bytes({2, 255}));

    // Float16 has no C++ element type; its bytes are IEEE binary16.
    const auto half = EvalAs<ub::TypedArray>(f.context, "TypedArray('float16', [1.0, -2.0, 65520])");
    CHECK(ub::GetElementType(half) == ub::ElementType::Float16);
    CHECK(ub::Length(half) == 3);
    CHECK(AllBytes(ub::Local<ub::ArrayBufferView>(half)) == Bytes({0x00, 0x3C, 0x00, 0xC0, 0x00, 0x7C}));
}

TEST_CASE("binary: the script-side TypedArray behaves as a sequence") {
    Fixture f;
    Run(f.context, "import unibind, struct\nfrom unibind import TypedArray, DataView");
    CHECK(EvalTruth(
        f.context, "a = TypedArray('int32', [1, 2, 3, 4])\nlen(a) == 4 and a.length == 4 and list(a) == [1, 2, 3, 4]"));
    CHECK(EvalTruth(f.context, "a[-1] == 4 and a[0] == 1 and 3 in a and 5 not in a"));
    CHECK(EvalTruth(f.context,
                    "a.byteLength == 16 and a.byteOffset == 0 and a.BYTES_PER_ELEMENT == 4 and a.type == 'int32'"));
    CHECK(EvalTruth(f.context, "type(a.buffer) is bytearray and len(a.buffer) == 16"));
    CHECK(py_test::EvalError(f.context, "a[4]").starts_with("IndexError"));
    CHECK(py_test::EvalError(f.context, "a[-5] = 1").starts_with("IndexError"));
    CHECK(py_test::EvalError(f.context, "del a[0]").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "a['x']").starts_with("TypeError"));
    // A slice is a copy; subarray() shares.
    CHECK(EvalTruth(f.context,
                    "s = a[1:3]\ns[0] = 99\ns.tolist() == [99, 3] and a[1] == 2 and s.buffer is not a.buffer"));
    CHECK(EvalTruth(f.context, "a[::-2].tolist() == [4, 2]"));
    CHECK(EvalTruth(f.context,
                    "sub = a.subarray(1, -1)\nsub[0] = 42\na[1] == 42 and sub.byteOffset == 4 and len(sub) == 2"));
    CHECK(EvalTruth(f.context, "len(a.subarray(3, 1)) == 0 and a.subarray(-2).tolist() == [3, 4]"));
    // Slice assignment converts every element, and needs exactly the slice's length.
    CHECK(EvalTruth(f.context, "a[0:2] = [7.9, 2**32 + 8]\na.tolist()[:2] == [7, 8]"));
    CHECK(py_test::EvalError(f.context, "a[0:2] = [1, 2, 3]").starts_with("ValueError"));
    CHECK(EvalTruth(f.context, "a[:] = a[::-1]\na.tolist() == [4, 3, 8, 7]"));
    // Overlapping source and target read before they write.
    CHECK(EvalTruth(f.context, "a[1:] = a.subarray(0, 3)\na.tolist() == [4, 4, 3, 8]"));
    CHECK(EvalText(f.context, "repr(TypedArray('uint8', [1, 2]))") == "unibind.TypedArray('uint8', [1, 2])");
    // Construction: a length, an iterable, a copy of another view, a view over a buffer.
    CHECK(EvalTruth(f.context, "TypedArray('float64', 3).tolist() == [0.0, 0.0, 0.0]"));
    CHECK(EvalTruth(f.context, "TypedArray('int8', range(3)).tolist() == [0, 1, 2]"));
    CHECK(EvalTruth(f.context, "TypedArray('int8').tolist() == [] and len(TypedArray('uint8', None)) == 0"));
    CHECK(EvalTruth(f.context, "c = TypedArray('uint8', TypedArray('float32', [1.5, 300]))\nc.tolist() == [1, 44]"));
    CHECK(EvalTruth(
        f.context,
        "b = bytearray(8)\nv = TypedArray('Int16Array', b, byteOffset=2)\nlen(v) == 3 and v.byteOffset == 2"));
    CHECK(EvalTruth(f.context, "v[0] = -1\nb[2:4] == b'\\xff\\xff'"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int32', bytearray(8), 2)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int32', bytearray(7))").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int32', bytearray(8), 4, 2)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int32', bytearray(8), 12)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int32', bytearray(8), -4)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int32', -1)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int33', 1)").starts_with("ValueError"));
    CHECK(py_test::EvalError(f.context, "TypedArray(4, 1)").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int8', [1], 0)").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "TypedArray('int8', ['x'])").starts_with("TypeError"));
    CHECK(EvalTruth(f.context, "issubclass(unibind.RangeError, ValueError)"));
    // A view over bytes is read-only.
    CHECK(EvalTruth(f.context, "r = TypedArray('uint8', b'\\x01\\x02')\nr.tolist() == [1, 2]"));
    CHECK(py_test::EvalError(f.context, "r[0] = 5").starts_with("TypeError"));
    // The buffer protocol shows the view's own window, typed.
    CHECK(EvalTruth(f.context,
                    "m = memoryview(TypedArray('int16', bytearray(b'\\x00\\x00\\x01\\x00\\x02\\x00'), 2))\n"
                    "m.format == 'h' and m.itemsize == 2 and m.tolist() == [1, 2] and not m.readonly"));
    CHECK(EvalTruth(f.context, "bytes(TypedArray('uint16', [0x0102])) == b'\\x02\\x01'"));
    CHECK(EvalTruth(f.context, "struct.unpack_from('<i', TypedArray('int32', [-7]))[0] == -7"));
    CHECK(EvalTruth(f.context, "memoryview(r).readonly"));
    CHECK(EvalTruth(f.context, "w = TypedArray('uint8', 2)\nmemoryview(w)[1] = 9\nw[1] == 9"));
    CHECK(EvalTruth(f.context, "import weakref\nweakref.ref(w)() is w"));
}

TEST_CASE("binary: the script-side DataView reads and writes in either byte order") {
    Fixture f;
    Run(f.context, "from unibind import DataView, TypedArray\nbuf = bytearray(16)\nd = DataView(buf, 4)");
    CHECK(EvalTruth(f.context, "d.byteOffset == 4 and d.byteLength == 12 and d.buffer is buf"));
    CHECK(EvalTruth(f.context, "d.setUint16(0, 0x1234)\nbuf[4:6] == b'\\x12\\x34' and d.getUint16(0) == 0x1234"));
    CHECK(EvalTruth(
        f.context,
        "d.setUint16(0, 0x1234, True)\nbuf[4:6] == b'\\x34\\x12' and d.getUint16(0, littleEndian=True) == 0x1234"));
    CHECK(EvalTruth(
        f.context,
        "d.setInt32(2, -2)\nd.getInt32(2) == -2 and d.getUint32(2) == 2**32 - 2 and d.getInt32(2, True) == -16777217"));
    CHECK(EvalTruth(
        f.context,
        "d.setFloat32(0, 1.5, True)\nd.getFloat32(0, True) == 1.5 and bytes(buf[4:8]) == b'\\x00\\x00\\xc0\\x3f'"));
    CHECK(EvalTruth(f.context, "d.setFloat64(4, -0.1)\nd.getFloat64(4) == -0.1"));
    CHECK(EvalTruth(f.context, "d.setFloat16(0, 1.0)\nbuf[4:6] == b'\\x3c\\x00' and d.getFloat16(0) == 1.0"));
    CHECK(
        EvalTruth(f.context, "d.setBigInt64(4, -1)\nd.getBigUint64(4) == 2**64 - 1 and d.getBigInt64(4, True) == -1"));
    CHECK(EvalTruth(f.context,
                    "d.setBigUint64(4, 1, True)\nd.getBigUint64(4, True) == 1 and d.getBigUint64(4) == 2**56"));
    CHECK(EvalTruth(f.context, "d.setInt8(11, 200)\nd.getInt8(11) == -56 and d.getUint8(11) == 200"));
    // An offset past the view's end is a RangeError - including one the element
    // would only partly fit at - and a negative offset is too.
    CHECK(py_test::EvalError(f.context, "d.getUint8(12)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "d.getUint32(9)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "d.setUint32(9, 0)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "d.getUint8(-1)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "d.setBigInt64(0, 1.0)").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "DataView(buf, 17)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "DataView(buf, 4, 13)").starts_with("unibind.RangeError"));
    CHECK(py_test::EvalError(f.context, "DataView([1, 2])").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "DataView(b'ab').setInt8(0, 1)").starts_with("TypeError"));
    CHECK(EvalTruth(f.context, "DataView(b'\\x01\\x02').getUint16(0) == 0x0102"));
    // A DataView and a TypedArray over the same buffer see each other.
    CHECK(EvalTruth(f.context, "t = TypedArray('uint32', buf)\nd.setUint32(4, 7, True)\nt[2] == 7"));
}

// --- a buffer shrunk under its views -------------------------------------------------

TEST_CASE("binary: a view whose buffer shrank reads as a view over nothing") {
    Fixture f;
    Run(f.context, R"PY(
from unibind import TypedArray, DataView
buf = bytearray(range(16))
v = TypedArray('int32', buf, 4, 2)
d = DataView(buf, 8, 8)
whole = TypedArray('uint8', buf)
)PY");
    const auto view = EvalAs<ub::TypedArray>(f.context, "v");
    const auto data = EvalAs<ub::DataView>(f.context, "d");
    CHECK(ub::Length(view) == 2);
    CHECK(ub::ByteOffset(view) == 4);

    // Script takes the bytes away. Nothing reads past the end: every question
    // answers as for a detached buffer.
    Run(f.context, "del buf[4:]");
    const ub::Local<ub::ArrayBufferView> anyView = view;
    const ub::Local<ub::ArrayBufferView> anyData = data;
    CHECK(ub::Length(view) == 0);
    CHECK(ub::ByteOffset(view) == 0);
    CHECK(ub::ByteLength(anyView) == 0);
    CHECK(ub::ByteOffset(anyView) == 0);
    CHECK(ub::ByteLength(anyData) == 0);
    CHECK(ub::ByteOffset(anyData) == 0);
    std::array<std::int32_t, 2> elements{-1, -1};
    CHECK(ub::CopyElements(view, std::span<std::int32_t>(elements)) == 0);
    CHECK(elements[0] == -1);
    std::array<std::byte, 8> bytes{};
    CHECK(ub::CopyBytes(anyData, bytes) == 0);
    // The buffer itself is still there, four bytes long.
    auto buffer = ub::GetBuffer(f.context, view);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 4);
    // A view made over it now has to fit what it is now.
    CHECK_FALSE(ub::TypedArray::New(f.context, ub::ElementType::Uint8, *buffer, 0, 5).has_value());
    CHECK(ub::TypedArray::New(f.context, ub::ElementType::Uint8, *buffer, 0, 4).has_value());

    // Script sees the same thing, and a read or write is refused, not wild.
    CHECK(EvalTruth(f.context, "len(v) == 0 and v.byteLength == 0 and v.byteOffset == 0 and v.tolist() == []"));
    CHECK(EvalTruth(f.context, "len(whole) == 0 and list(whole) == [] and whole[:].tolist() == []"));
    CHECK(py_test::EvalError(f.context, "v[0]").starts_with("IndexError"));
    CHECK(py_test::EvalError(f.context, "v[0] = 1").starts_with("IndexError"));
    CHECK(py_test::EvalError(f.context, "d.getInt8(0)").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "d.setInt8(0, 1)").starts_with("TypeError"));
    CHECK(py_test::EvalError(f.context, "memoryview(v)").starts_with("BufferError"));
    CHECK(EvalText(f.context, "repr(v)") == "unibind.TypedArray('int32', <out of bounds>)");

    // Grown back, the views are whole again - over the new bytes.
    Run(f.context, "buf.extend(bytes(range(4, 16)))");
    CHECK(ub::Length(view) == 2);
    CHECK(ub::ByteOffset(view) == 4);
    CHECK(AllBytes(anyData) == Bytes({8, 9, 10, 11, 12, 13, 14, 15}));
    CHECK(EvalTruth(f.context, "v.tolist() == list(TypedArray('int32', bytes(range(4, 12))))"));
}

TEST_CASE("binary: a conversion that shrinks the buffer cannot write past its end") {
    Fixture f;
    Run(f.context, R"PY(
from unibind import TypedArray, DataView
buf = bytearray(8)
v = TypedArray('uint8', buf)
d = DataView(buf)
class Shrinker:
    def __index__(self):
        del buf[:]
        return 1
)PY");
    CHECK(py_test::EvalError(f.context, "v[7] = Shrinker()").starts_with("IndexError"));
    CHECK(py_test::EvalError(f.context, "v[0:1] = [Shrinker()]").starts_with("ValueError"));
    Run(f.context, "buf.extend(bytes(8))");
    CHECK(py_test::EvalError(f.context, "d.setUint8(7, Shrinker())").starts_with("TypeError"));
    CHECK(EvalInt(f.context, "len(buf)") == 0);
}

TEST_CASE("binary: an exported view pins its buffer's size") {
    Fixture f;
    Run(f.context,
        "from unibind import TypedArray\nbuf = bytearray(8)\nv = TypedArray('uint16', buf)\nm = memoryview(v)");
    CHECK(py_test::EvalError(f.context, "del buf[:]").starts_with("BufferError"));
    Run(f.context, "m.release()\ndel buf[:]");
    CHECK(EvalInt(f.context, "len(v)") == 0);
}

// --- lifetimes ------------------------------------------------------------------------

TEST_CASE("binary: a view keeps its buffer alive after every other reference is gone") {
    Fixture f;
    ub::Local<ub::TypedArray> view;
    {
        ub::EscapableHandleScope inner(f.iso());
        const auto input = Bytes({5, 6, 7, 8});
        auto buffer = ub::ArrayBuffer::New(f.context, std::span<const std::byte>(input));
        REQUIRE(buffer.has_value());
        auto made = ub::TypedArray::New(f.context, ub::ElementType::Uint8, *buffer, 1, 2);
        REQUIRE(made.has_value());
        view = inner.Escape(*made);
    }
    Run(f.context, "import gc\ngc.collect()");
    auto buffer = ub::GetBuffer(f.context, view);
    REQUIRE(buffer.has_value());
    CHECK(AllBytes(*buffer) == Bytes({5, 6, 7, 8}));
    CHECK(AllBytes(ub::Local<ub::ArrayBufferView>(view)) == Bytes({6, 7}));

    // And from script: the view is the only thing holding the bytearray.
    CHECK(EvalTruth(f.context, R"PY(
from unibind import TypedArray, DataView
def make():
    b = bytearray(b'\x01\x02\x03')
    return TypedArray('uint8', b, 1), DataView(b)
t, dv = make()
gc.collect()
t.tolist() == [2, 3] and t.buffer is dv.buffer and dv.getUint8(0) == 1
)PY"));
    // A view in a reference cycle is collected with it, and nothing breaks.
    CHECK(EvalTruth(f.context, R"PY(
import weakref
holder = [TypedArray('uint8', 4)]
holder.append(holder)
probe = weakref.ref(holder[0])
del holder
gc.collect()
probe() is None
)PY"));
}
