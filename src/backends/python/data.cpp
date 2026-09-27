// Binary data, structured clone and the compiled-code cache: the three things
// in the contract that are about bytes rather than about objects.
//
// **Binary data.** An `ArrayBuffer` is a `bytearray` (or, read-only, a `bytes`)
// - values.cpp decided that, and it is the right call: it is Python's own
// mutable byte string, every Python library already takes one, and it copies in
// and out in one `memcpy`. What Python has no word for is a *typed window* onto
// one, so this file makes two: `unibind.TypedArray` and `unibind.DataView`.
// Each holds a strong reference to its buffer and an offset and a length, and
// reads and writes the buffer's storage directly, with JavaScript's element
// conversions.
//
// Python has no detaching, but it has something with the same consequence:
// script can *resize* a bytearray under a view (`del buf[:]`, `buf.extend`).
// A resize may move the storage and may leave the view's window hanging off the
// end, so a view never keeps a pointer: every access re-reads the buffer's
// current storage and size, and a window that no longer fits reads as a view
// over nothing - length and offset zero, as backend.h asks of a detached one.
// Element conversion can run Python code (`__index__`, `__float__`), and that
// code can resize the buffer too, so a write converts first and looks at the
// buffer afterwards, never the other way round.
//
// **Structured clone** is `marshal` over a tagged graph this file builds: the
// value becomes a flat list of nodes, each a small tuple of a tag and plain
// data, with children named by their index in the list. Indices are what make
// shared references and cycles survive - a list that holds itself is a node
// whose child is its own index - and flatness is what keeps both sides free of
// recursion, so a deeply nested value costs heap, not C stack. `pickle` was not
// an option: loading a pickle calls whatever the pickle names. Loading this
// runs nothing - `marshal` only builds the handful of plain types, and every
// node is checked for exactly the shape the writer gives it before anything is
// built from it.
//
// **The code cache** is `marshal` too, of the pair of code objects a
// `ScriptRec` holds. That is CPython's own compiled-code format, the one `.pyc`
// files are, and it is exactly as portable as a `.pyc`: one interpreter
// version, one bytecode magic number. `marshal.loads` of a malformed code
// object can crash the interpreter, so a blob is checked before `marshal` sees
// a byte of it; see `FrameBlob`.

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>

#include "internal.h"

// After Python.h, which internal.h includes: marshal.h does not include it.
#include <marshal.h>

namespace ub::detail {

/// Nothing per isolate beyond the types, which live in `Types` because they are
/// Python objects and must be dropped before the interpreter ends - which this
/// struct outlives. Kept, empty, so that the area has the same shape as the
/// others and a future cache has somewhere to go.
struct DataState {};

void DestroyDataState(DataState* state) noexcept {
    delete state;
}

namespace {

// ===========================================================================
// Element types
// ===========================================================================

/// What the backend needs to know about each element type, indexed by
/// `ElementType`. `format` is the `struct`-module character the buffer protocol
/// describes an element with; native byte order, as a typed array's is.
struct ElementInfo {
    const char* name;    ///< what `TypedArray.type` says, and what the constructor takes
    const char* format;  ///< buffer-protocol format string
};

constexpr ElementInfo ELEMENTS[] = {
    {"int8", "b"},   {"uint8", "B"},   {"uint8clamped", "B"}, {"int16", "h"},    {"uint16", "H"},    {"int32", "i"},
    {"uint32", "I"}, {"float32", "f"}, {"float64", "d"},      {"bigint64", "q"}, {"biguint64", "Q"}, {"float16", "e"},
};
constexpr int ELEMENT_TYPE_COUNT = static_cast<int>(sizeof(ELEMENTS) / sizeof(ELEMENTS[0]));
static_assert(ELEMENT_TYPE_COUNT == static_cast<int>(ElementType::Float16) + 1,
              "ELEMENTS must have one entry per ElementType, in the enum's order");
// The formats name C types; these are the widths they have to be.
static_assert(sizeof(int) == 4 && sizeof(long long) == 8, "buffer-protocol formats assume 32-bit int");

[[nodiscard]] const ElementInfo& InfoOf(ElementType type) noexcept {
    return ELEMENTS[static_cast<int>(type)];
}

[[nodiscard]] Py_ssize_t SizeOf(ElementType type) noexcept {
    return static_cast<Py_ssize_t>(ElementSize(type));
}

constexpr bool NATIVE_LITTLE = std::endian::native == std::endian::little;

/// The exception to raise for an index or length out of range: `unibind.RangeError`,
/// a ValueError, where there is an isolate on this thread to find it in.
[[nodiscard]] PyObject* RangeErrorClass() noexcept {
    Isolate* isolate = CurrentIsolate();
    if (isolate != nullptr && isolate->impl().types.rangeError != nullptr) {
        return isolate->impl().types.rangeError;
    }
    return PyExc_ValueError;
}

/// A number as JavaScript's ToNumber would see it, for the float element types
/// and for the integer ones given a float. Python's rules for what counts as a
/// number, though: an int, a float, or something with `__index__` or
/// `__float__` - not a string, which `array.array` refuses too.
[[nodiscard]] bool ElementNumber(PyObject* value, double* out) noexcept {
    if (PyFloat_Check(value)) {
        *out = PyFloat_AS_DOUBLE(value);
        return true;
    }
    PyObject* integer = nullptr;
    if (PyLong_Check(value)) {
        integer = Py_NewRef(value);
    } else if (PyIndex_Check(value) != 0) {
        integer = PyNumber_Index(value);
        if (integer == nullptr) {
            return false;
        }
    }
    if (integer != nullptr) {
        const double d = PyLong_AsDouble(integer);
        if (d == -1.0 && PyErr_Occurred() != nullptr) {
            // Beyond a double: which is infinity, as a double has it.
            PyErr_Clear();
            int sign = 0;
            (void)PyLong_GetSign(integer, &sign);  // cannot fail: an int
            *out = sign < 0 ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        } else {
            *out = d;
        }
        Py_DECREF(integer);
        return true;
    }
    PyNumberMethods* number = Py_TYPE(value)->tp_as_number;
    if (number == nullptr || number->nb_float == nullptr) {
        PyErr_Format(PyExc_TypeError, "a TypedArray element must be a number, not '%.200s'", Py_TYPE(value)->tp_name);
        return false;
    }
    PyObject* converted = PyNumber_Float(value);
    if (converted == nullptr) {
        return false;
    }
    *out = PyFloat_AS_DOUBLE(converted);
    Py_DECREF(converted);
    return true;
}

/// ToInt64's bits of a double: truncated towards zero and reduced modulo 2^64,
/// with NaN and the infinities as zero. Every narrower ToIntN / ToUintN is the
/// low N bits of this, because 2^N divides 2^64.
[[nodiscard]] std::uint64_t WrapDouble(double d) noexcept {
    if (!std::isfinite(d)) {
        return 0;
    }
    // fmod is exact, and its result is an integer strictly inside (-2^64, 2^64),
    // so both casts below are of a value the target type holds.
    const double r = std::fmod(std::trunc(d), 18446744073709551616.0);
    if (r < 0) {
        return std::uint64_t{0} - static_cast<std::uint64_t>(-r);
    }
    return static_cast<std::uint64_t>(r);
}

/// The low 64 bits of an int of any size, two's complement: `PyLong`'s mask
/// conversion, which wraps rather than raising.
[[nodiscard]] bool WrapInteger(PyObject* value, std::uint64_t* out) noexcept {
    PyObject* integer = PyNumber_Index(value);
    if (integer == nullptr) {
        return false;
    }
    const unsigned long long bits = PyLong_AsUnsignedLongLongMask(integer);
    Py_DECREF(integer);
    if (bits == static_cast<unsigned long long>(-1) && PyErr_Occurred() != nullptr) {
        return false;
    }
    *out = bits;
    return true;
}

/// JavaScript's ToUint8Clamp: NaN is 0, the range clamps, and a half rounds to
/// the even neighbour. Spelled out rather than left to `nearbyint`, whose
/// answer depends on a rounding mode the host may have changed.
[[nodiscard]] std::uint8_t ClampDouble(double d) noexcept {
    if (std::isnan(d) || d <= 0) {
        return 0;
    }
    if (d >= 255) {
        return 255;
    }
    const double floor = std::floor(d);
    const double fraction = d - floor;
    auto result = static_cast<std::uint8_t>(floor);
    if (fraction > 0.5 || (fraction == 0.5 && (result & 1) != 0)) {
        ++result;
    }
    return result;
}

/// A double rounded to the nearest float, overflowing to infinity past the
/// point where rounding would, as JavaScript's `Math.fround` does. A plain cast
/// is undefined behaviour for a value beyond `FLT_MAX`.
[[nodiscard]] float RoundToFloat(double d) noexcept {
    if (std::isfinite(d) && std::fabs(d) > static_cast<double>(FLT_MAX)) {
        // Half an ulp above FLT_MAX is the tie, and it rounds to even - which
        // is infinity, FLT_MAX's significand being odd.
        const double tie = static_cast<double>(FLT_MAX) + 0x1p103;
        const float magnitude = std::fabs(d) >= tie ? std::numeric_limits<float>::infinity() : FLT_MAX;
        return d < 0 ? -magnitude : magnitude;
    }
    return static_cast<float>(d);
}

/// A double as a binary16, native byte order. `PyFloat_Pack2` rounds half to
/// even as IEEE does, but raises OverflowError where IEEE (and Float16Array)
/// give infinity; 65520 is the first value that rounds past 65504.
void PackHalf(double d, unsigned char* out) noexcept {
    if (std::isfinite(d) && std::fabs(d) >= 65520.0) {
        const std::uint16_t bits = d < 0 ? 0xFC00 : 0x7C00;
        std::memcpy(out, &bits, 2);
        return;
    }
    if (PyFloat_Pack2(d, reinterpret_cast<char*>(out), NATIVE_LITTLE ? 1 : 0) != 0) {
        // Unreachable after the range check; zero rather than garbage if not.
        PyErr_Clear();
        std::memset(out, 0, 2);
    }
}

/// Convert `value` to one element of `type`, written to `out` in native byte
/// order. False with an exception pending if `value` is not a number. May run
/// Python code - `__index__`, `__float__` - so it is always called before the
/// destination buffer is looked at.
[[nodiscard]] bool PackElement(ElementType type, PyObject* value, unsigned char* out) noexcept {
    switch (type) {
        case ElementType::BigInt64:
        case ElementType::BigUint64: {
            // A BigInt array takes an integer and nothing else - JavaScript
            // refuses a Number there, and a float here is the same mistake.
            std::uint64_t bits = 0;
            if (!WrapInteger(value, &bits)) {
                return false;
            }
            std::memcpy(out, &bits, 8);
            return true;
        }
        case ElementType::Int8:
        case ElementType::Uint8:
        case ElementType::Int16:
        case ElementType::Uint16:
        case ElementType::Int32:
        case ElementType::Uint32: {
            std::uint64_t bits = 0;
            if (PyLong_Check(value)) {
                // Exact at any size: ToInt32(2**70 + 5) is 5, and a double
                // would have lost the 5.
                if (!WrapInteger(value, &bits)) {
                    return false;
                }
            } else {
                double d = 0;
                if (!ElementNumber(value, &d)) {
                    return false;
                }
                bits = WrapDouble(d);
            }
            // Little-endian truncation is the low bytes, which is where the
            // value is on a little-endian host; on a big-endian one it is the
            // last bytes, so narrow by value rather than by memcpy.
            switch (type) {
                case ElementType::Int8:
                case ElementType::Uint8: {
                    const auto narrow = static_cast<std::uint8_t>(bits);
                    std::memcpy(out, &narrow, 1);
                    break;
                }
                case ElementType::Int16:
                case ElementType::Uint16: {
                    const auto narrow = static_cast<std::uint16_t>(bits);
                    std::memcpy(out, &narrow, 2);
                    break;
                }
                default: {
                    const auto narrow = static_cast<std::uint32_t>(bits);
                    std::memcpy(out, &narrow, 4);
                    break;
                }
            }
            return true;
        }
        case ElementType::Uint8Clamped: {
            if (PyLong_Check(value)) {
                int overflow = 0;
                const long long n = PyLong_AsLongLongAndOverflow(value, &overflow);
                if (n == -1 && PyErr_Occurred() != nullptr) {
                    return false;
                }
                if (overflow != 0) {
                    *out = overflow > 0 ? 255 : 0;
                } else {
                    *out = static_cast<std::uint8_t>(std::clamp(n, 0LL, 255LL));
                }
                return true;
            }
            double d = 0;
            if (!ElementNumber(value, &d)) {
                return false;
            }
            *out = ClampDouble(d);
            return true;
        }
        case ElementType::Float32: {
            double d = 0;
            if (!ElementNumber(value, &d)) {
                return false;
            }
            const float f = RoundToFloat(d);
            std::memcpy(out, &f, 4);
            return true;
        }
        case ElementType::Float64: {
            double d = 0;
            if (!ElementNumber(value, &d)) {
                return false;
            }
            std::memcpy(out, &d, 8);
            return true;
        }
        case ElementType::Float16: {
            double d = 0;
            if (!ElementNumber(value, &d)) {
                return false;
            }
            PackHalf(d, out);
            return true;
        }
    }
    PyErr_SetString(PyExc_SystemError, "unibind: unknown element type");
    return false;
}

/// One element of `type` at `in`, native byte order, as a Python value: an int
/// for the integer types, a float for the float ones. New reference.
[[nodiscard]] PyObject* UnpackElement(ElementType type, const unsigned char* in) noexcept {
    switch (type) {
        case ElementType::Int8: {
            std::int8_t v = 0;
            std::memcpy(&v, in, 1);
            return PyLong_FromLong(v);
        }
        case ElementType::Uint8:
        case ElementType::Uint8Clamped:
            return PyLong_FromLong(*in);
        case ElementType::Int16: {
            std::int16_t v = 0;
            std::memcpy(&v, in, 2);
            return PyLong_FromLong(v);
        }
        case ElementType::Uint16: {
            std::uint16_t v = 0;
            std::memcpy(&v, in, 2);
            return PyLong_FromLong(v);
        }
        case ElementType::Int32: {
            std::int32_t v = 0;
            std::memcpy(&v, in, 4);
            return PyLong_FromLong(v);
        }
        case ElementType::Uint32: {
            std::uint32_t v = 0;
            std::memcpy(&v, in, 4);
            return PyLong_FromUnsignedLong(v);
        }
        case ElementType::Float32: {
            float v = 0;
            std::memcpy(&v, in, 4);
            return PyFloat_FromDouble(static_cast<double>(v));
        }
        case ElementType::Float64: {
            double v = 0;
            std::memcpy(&v, in, 8);
            return PyFloat_FromDouble(v);
        }
        case ElementType::BigInt64: {
            std::int64_t v = 0;
            std::memcpy(&v, in, 8);
            return PyLong_FromLongLong(v);
        }
        case ElementType::BigUint64: {
            std::uint64_t v = 0;
            std::memcpy(&v, in, 8);
            return PyLong_FromUnsignedLongLong(v);
        }
        case ElementType::Float16:
            return PyFloat_FromDouble(PyFloat_Unpack2(reinterpret_cast<const char*>(in), NATIVE_LITTLE ? 1 : 0));
    }
    PyErr_SetString(PyExc_SystemError, "unibind: unknown element type");
    return nullptr;
}

/// `"int32"`, `"Int32"`, `"Int32Array"` and `"int32array"` all name Int32:
/// the Python spelling and JavaScript's constructor name are both natural to
/// type, and refusing either would be pedantry.
[[nodiscard]] bool ParseElementType(PyObject* name, ElementType* out) noexcept {
    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "TypedArray type must be a str such as 'int32', not '%.200s'",
                     Py_TYPE(name)->tp_name);
        return false;
    }
    const char* text = PyUnicode_AsUTF8(name);
    if (text == nullptr) {
        return false;
    }
    std::string key;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c != '_') {
            key.push_back(static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c));
        }
    }
    if (key.size() > 5 && key.ends_with("array")) {
        key.resize(key.size() - 5);
    }
    for (int i = 0; i < ELEMENT_TYPE_COUNT; ++i) {
        if (key == ELEMENTS[i].name) {
            *out = static_cast<ElementType>(i);
            return true;
        }
    }
    PyErr_Format(PyExc_ValueError,
                 "unknown TypedArray type %R: expected one of int8, uint8, uint8clamped, int16, uint16, int32, "
                 "uint32, float32, float64, bigint64, biguint64, float16",
                 name);
    return false;
}

// ===========================================================================
// Buffers and windows
// ===========================================================================

/// Whether `object` can be the buffer under a view: a bytearray, or - read
/// only - a bytes. The two things values.cpp calls an ArrayBuffer.
[[nodiscard]] bool IsBuffer(PyObject* object) noexcept {
    return object != nullptr && (PyByteArray_Check(object) || PyBytes_Check(object));
}

/// A buffer's storage as it is *now*. A bytearray may have been resized - and
/// its storage moved - since the last time anyone looked, so nothing keeps one
/// of these across a call that could run Python code.
struct Storage {
    unsigned char* data = nullptr;
    Py_ssize_t size = 0;
    bool writable = false;
};

[[nodiscard]] Storage StorageOf(PyObject* buffer) noexcept {
    if (buffer != nullptr && PyByteArray_Check(buffer)) {
        return {reinterpret_cast<unsigned char*>(PyByteArray_AS_STRING(buffer)), PyByteArray_GET_SIZE(buffer), true};
    }
    if (buffer != nullptr && PyBytes_Check(buffer)) {
        return {reinterpret_cast<unsigned char*>(PyBytes_AS_STRING(buffer)), PyBytes_GET_SIZE(buffer), false};
    }
    return {};
}

/// The bytes `[offset, offset + length)` of `buffer`, if the buffer still holds
/// all of them. A null `data` - with `ok` false - is a view that fell off the
/// end of a buffer script shrank: Python's detached buffer.
struct Window {
    unsigned char* data = nullptr;
    bool writable = false;
    bool ok = false;
};

[[nodiscard]] Window WindowOf(PyObject* buffer, Py_ssize_t offset, Py_ssize_t length) noexcept {
    const Storage storage = StorageOf(buffer);
    if (buffer == nullptr || offset < 0 || length < 0 || offset > storage.size || length > storage.size - offset) {
        return {};
    }
    return {storage.data + offset, storage.writable, true};
}

// ===========================================================================
// unibind.TypedArray
// ===========================================================================

struct TypedArrayObject {
    PyObject_HEAD PyObject* buffer;  ///< bytearray or bytes; strong. Null only after tp_clear.
    Py_ssize_t byteOffset;
    Py_ssize_t length;  ///< in elements, as made - the live length may be zero
    ElementType type;
    Py_ssize_t shape;   ///< `length`, and `stride` the element size: what an
    Py_ssize_t stride;  ///< exported Py_buffer points its shape and strides at
    PyObject* weaklist;
};

struct DataViewObject {
    PyObject_HEAD PyObject* buffer;  ///< bytearray or bytes; strong. Null only after tp_clear.
    Py_ssize_t byteOffset;
    Py_ssize_t byteLength;
    PyObject* weaklist;
};

[[nodiscard]] TypedArrayObject* AsTypedArray(PyObject* object) noexcept {
    return reinterpret_cast<TypedArrayObject*>(object);
}
[[nodiscard]] DataViewObject* AsDataView(PyObject* object) noexcept {
    return reinterpret_cast<DataViewObject*>(object);
}

[[nodiscard]] Window WindowOf(const TypedArrayObject* self) noexcept {
    return WindowOf(self->buffer, self->byteOffset, self->length * SizeOf(self->type));
}
[[nodiscard]] Window WindowOf(const DataViewObject* self) noexcept {
    return WindowOf(self->buffer, self->byteOffset, self->byteLength);
}

/// Elements the view can reach now: its length, or zero once its buffer has
/// shrunk from under it.
[[nodiscard]] Py_ssize_t LiveLength(const TypedArrayObject* self) noexcept {
    return WindowOf(self).ok ? self->length : 0;
}

/// Make a view object of `type` over `buffer`. The caller has checked the
/// window fits. New reference, or null with an exception pending.
[[nodiscard]] PyObject* NewTypedArrayObject(PyTypeObject* type, ElementType element, PyObject* buffer,
                                            Py_ssize_t byteOffset, Py_ssize_t length) noexcept {
    PyObject* object = type->tp_alloc(type, 0);
    if (object == nullptr) {
        return nullptr;
    }
    TypedArrayObject* self = AsTypedArray(object);
    self->buffer = Py_NewRef(buffer);
    self->byteOffset = byteOffset;
    self->length = length;
    self->type = element;
    self->shape = length;
    self->stride = SizeOf(element);
    self->weaklist = nullptr;
    return object;
}

[[nodiscard]] PyObject* NewDataViewObject(PyTypeObject* type, PyObject* buffer, Py_ssize_t byteOffset,
                                          Py_ssize_t byteLength) noexcept {
    PyObject* object = type->tp_alloc(type, 0);
    if (object == nullptr) {
        return nullptr;
    }
    DataViewObject* self = AsDataView(object);
    self->buffer = Py_NewRef(buffer);
    self->byteOffset = byteOffset;
    self->byteLength = byteLength;
    self->weaklist = nullptr;
    return object;
}

/// A non-negative index argument - a byte offset, a length - as Py_ssize_t.
/// RangeError for a negative one or one too big to be any buffer's.
[[nodiscard]] bool IndexArgument(PyObject* value, const char* what, Py_ssize_t* out) noexcept {
    PyObject* integer = PyNumber_Index(value);
    if (integer == nullptr) {
        return false;
    }
    int overflow = 0;
    const long long n = PyLong_AsLongLongAndOverflow(integer, &overflow);
    Py_DECREF(integer);
    if (n == -1 && PyErr_Occurred() != nullptr) {
        return false;
    }
    if (overflow != 0 || n < 0 || n > PY_SSIZE_T_MAX) {
        PyErr_Format(RangeErrorClass(), "%s out of range", what);
        return false;
    }
    *out = static_cast<Py_ssize_t>(n);
    return true;
}

/// A new bytearray of `size` bytes, contents unspecified. New reference, or
/// null with `MemoryError` pending.
///
/// Never `PyByteArray_FromStringAndSize(nullptr, size)`: in CPython (3.12, and
/// still 3.14), when the storage cannot be had, that frees the half-made object
/// before it has set `ob_exports`, and `bytearray`'s deallocator reads the uninitialised
/// field - "SystemError: deallocated bytearray object has exported buffers",
/// printed as unraisable whenever the stale memory happens to be positive. An
/// empty bytearray is made whole, and growing it fails cleanly.
[[nodiscard]] PyObject* NewByteArray(Py_ssize_t size) noexcept {
    PyObject* buffer = PyByteArray_FromStringAndSize(nullptr, 0);
    if (buffer != nullptr && size > 0 && PyByteArray_Resize(buffer, size) != 0) {
        Py_DECREF(buffer);
        return nullptr;
    }
    return buffer;
}

/// A new, private, zero-filled bytearray of `count` elements of `type`.
[[nodiscard]] PyObject* NewZeroedBuffer(ElementType type, Py_ssize_t count) noexcept {
    const Py_ssize_t size = SizeOf(type);
    if (count > PY_SSIZE_T_MAX / size) {
        PyErr_Format(RangeErrorClass(), "TypedArray length %zd is too large", count);
        return nullptr;
    }
    PyObject* buffer = NewByteArray(count * size);
    if (buffer != nullptr && count > 0) {
        std::memset(PyByteArray_AS_STRING(buffer), 0, static_cast<std::size_t>(count * size));
    }
    return buffer;
}

/// `TypedArray(type, source=None, byteOffset=None, length=None)`.
///
/// What `source` is decides what is made, as it does for JavaScript's
/// constructors:
///
///   * a bytearray or bytes: a view over *that* buffer, sharing it, from
///     `byteOffset` (default 0, a multiple of the element size) for `length`
///     elements (default: to the end, which must then be a whole number of
///     elements). A bytes makes a read-only view.
///   * an int: that many zero elements in a new buffer.
///   * None: an empty array.
///   * any other iterable - a list, a range, another TypedArray: its elements,
///     converted, in a new buffer. A copy, never a view.
///
/// `byteOffset` and `length` apply to a buffer source only.
PyObject* TypedArrayNew(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static const char* keywords[] = {"type", "source", "byteOffset", "length", nullptr};
    PyObject* name = nullptr;
    PyObject* source = Py_None;
    PyObject* offsetArgument = Py_None;
    PyObject* lengthArgument = Py_None;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "O|OOO:TypedArray", const_cast<char**>(keywords), &name, &source,
                                    &offsetArgument, &lengthArgument) == 0) {
        return nullptr;
    }
    ElementType element = ElementType::Uint8;
    if (!ParseElementType(name, &element)) {
        return nullptr;
    }
    const Py_ssize_t size = SizeOf(element);

    if (IsBuffer(source)) {
        const Py_ssize_t available = StorageOf(source).size;
        Py_ssize_t offset = 0;
        if (offsetArgument != Py_None && !IndexArgument(offsetArgument, "byteOffset", &offset)) {
            return nullptr;
        }
        if (offset % size != 0) {
            PyErr_Format(RangeErrorClass(), "start offset of a %s TypedArray should be a multiple of %zd",
                         InfoOf(element).name, size);
            return nullptr;
        }
        if (offset > available) {
            PyErr_Format(RangeErrorClass(), "start offset %zd is outside the bounds of the buffer", offset);
            return nullptr;
        }
        Py_ssize_t length = 0;
        if (lengthArgument == Py_None) {
            if ((available - offset) % size != 0) {
                PyErr_Format(RangeErrorClass(), "byte length of a %s TypedArray should be a multiple of %zd",
                             InfoOf(element).name, size);
                return nullptr;
            }
            length = (available - offset) / size;
        } else {
            if (!IndexArgument(lengthArgument, "length", &length)) {
                return nullptr;
            }
            if (length > (available - offset) / size) {
                PyErr_Format(RangeErrorClass(), "invalid TypedArray length %zd", length);
                return nullptr;
            }
        }
        return NewTypedArrayObject(type, element, source, offset, length);
    }

    if (offsetArgument != Py_None || lengthArgument != Py_None) {
        PyErr_SetString(PyExc_TypeError, "TypedArray: byteOffset and length apply only to a bytearray or bytes source");
        return nullptr;
    }

    if (source == Py_None || (PyLong_Check(source) && !PyBool_Check(source))) {
        Py_ssize_t count = 0;
        if (source != Py_None && !IndexArgument(source, "length", &count)) {
            return nullptr;
        }
        PyObject* buffer = NewZeroedBuffer(element, count);
        if (buffer == nullptr) {
            return nullptr;
        }
        PyObject* view = NewTypedArrayObject(type, element, buffer, 0, count);
        Py_DECREF(buffer);
        return view;
    }

    // Any other iterable: its elements, converted, in a buffer of our own. The
    // list is a snapshot, so a source that changes while its elements convert
    // (a generator, or an `__index__` that mutates it) cannot move under us.
    PyObject* items = PySequence_List(source);
    if (items == nullptr) {
        return nullptr;
    }
    const Py_ssize_t count = PyList_GET_SIZE(items);
    PyObject* buffer = NewZeroedBuffer(element, count);
    if (buffer == nullptr) {
        Py_DECREF(items);
        return nullptr;
    }
    // The buffer is not reachable from anywhere but here until the view is
    // made, so converting straight into it is safe: nothing an element's
    // `__index__` does can resize it.
    auto* data = reinterpret_cast<unsigned char*>(PyByteArray_AS_STRING(buffer));
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (!PackElement(element, PyList_GET_ITEM(items, i), data + (i * size))) {
            Py_DECREF(items);
            Py_DECREF(buffer);
            return nullptr;
        }
    }
    Py_DECREF(items);
    PyObject* view = NewTypedArrayObject(type, element, buffer, 0, count);
    Py_DECREF(buffer);
    return view;
}

void TypedArrayDealloc(PyObject* object) {
    PyTypeObject* type = Py_TYPE(object);
    PyObject_GC_UnTrack(object);
    if (AsTypedArray(object)->weaklist != nullptr) {
        PyObject_ClearWeakRefs(object);
    }
    Py_CLEAR(AsTypedArray(object)->buffer);
    type->tp_free(object);
    Py_DECREF(type);
}

int TypedArrayTraverse(PyObject* object, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(object));
    Py_VISIT(AsTypedArray(object)->buffer);
    return 0;
}

int TypedArrayClear(PyObject* object) {
    // A view whose buffer the collector took reads as detached from here on;
    // every accessor treats a null buffer as a window that does not fit.
    Py_CLEAR(AsTypedArray(object)->buffer);
    return 0;
}

Py_ssize_t TypedArrayLength(PyObject* object) {
    return LiveLength(AsTypedArray(object));
}

/// Element `index` (already made non-negative), or IndexError.
PyObject* TypedArrayItem(PyObject* object, Py_ssize_t index) {
    const TypedArrayObject* self = AsTypedArray(object);
    const Window window = WindowOf(self);
    if (!window.ok || index < 0 || index >= self->length) {
        PyErr_SetString(PyExc_IndexError, "TypedArray index out of range");
        return nullptr;
    }
    return UnpackElement(self->type, window.data + (index * SizeOf(self->type)));
}

/// Store `value` at element `index`, which may be negative from the end.
[[nodiscard]] int StoreItem(TypedArrayObject* self, Py_ssize_t index, PyObject* value) {
    unsigned char packed[8];
    // Convert first: it may run Python code, and that code may resize the buffer.
    if (!PackElement(self->type, value, packed)) {
        return -1;
    }
    const Window window = WindowOf(self);
    const Py_ssize_t length = window.ok ? self->length : 0;
    if (index < 0) {
        index += length;
    }
    if (index < 0 || index >= length) {
        PyErr_SetString(PyExc_IndexError, "TypedArray assignment index out of range");
        return -1;
    }
    if (!window.writable) {
        PyErr_SetString(PyExc_TypeError, "this TypedArray is over a bytes object and is read-only");
        return -1;
    }
    std::memcpy(window.data + (index * SizeOf(self->type)), packed, static_cast<std::size_t>(SizeOf(self->type)));
    return 0;
}

int TypedArraySetItem(PyObject* object, Py_ssize_t index, PyObject* value) {
    if (value == nullptr) {
        PyErr_SetString(PyExc_TypeError, "TypedArray elements cannot be deleted: its length is fixed");
        return -1;
    }
    return StoreItem(AsTypedArray(object), index, value);
}

/// `view[i]` and `view[a:b:c]`. A slice is a copy in a new buffer - what
/// JavaScript's `slice()` and `array.array` both give; `subarray()` is the
/// sharing form.
PyObject* TypedArraySubscript(PyObject* object, PyObject* key) {
    TypedArrayObject* self = AsTypedArray(object);
    if (PyIndex_Check(key) != 0) {
        Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred() != nullptr) {
            return nullptr;
        }
        if (index < 0) {
            index += LiveLength(self);
        }
        return TypedArrayItem(object, index);
    }
    if (!PySlice_Check(key)) {
        PyErr_Format(PyExc_TypeError, "TypedArray indices must be integers or slices, not %.200s",
                     Py_TYPE(key)->tp_name);
        return nullptr;
    }
    Py_ssize_t start = 0;
    Py_ssize_t stop = 0;
    Py_ssize_t step = 0;
    if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
        return nullptr;
    }
    const Py_ssize_t count = PySlice_AdjustIndices(LiveLength(self), &start, &stop, step);
    PyObject* buffer = NewZeroedBuffer(self->type, count);
    if (buffer == nullptr) {
        return nullptr;
    }
    // Nothing since `LiveLength` has run Python code, so the window still holds.
    const Window window = WindowOf(self);
    const Py_ssize_t size = SizeOf(self->type);
    auto* out = reinterpret_cast<unsigned char*>(PyByteArray_AS_STRING(buffer));
    for (Py_ssize_t i = 0; i < count && window.ok; ++i) {
        std::memcpy(out + (i * size), window.data + ((start + (i * step)) * size), static_cast<std::size_t>(size));
    }
    PyObject* copy = NewTypedArrayObject(Py_TYPE(object), self->type, buffer, 0, count);
    Py_DECREF(buffer);
    return copy;
}

/// `view[i] = v` and `view[a:b:c] = iterable`. A slice assignment must supply
/// exactly as many elements as the slice covers: a typed array cannot grow or
/// shrink, so the list-like alternative has nothing to mean.
int TypedArrayAssign(PyObject* object, PyObject* key, PyObject* value) {
    TypedArrayObject* self = AsTypedArray(object);
    if (value == nullptr) {
        PyErr_SetString(PyExc_TypeError, "TypedArray elements cannot be deleted: its length is fixed");
        return -1;
    }
    if (PyIndex_Check(key) != 0) {
        const Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred() != nullptr) {
            return -1;
        }
        return StoreItem(self, index, value);
    }
    if (!PySlice_Check(key)) {
        PyErr_Format(PyExc_TypeError, "TypedArray indices must be integers or slices, not %.200s",
                     Py_TYPE(key)->tp_name);
        return -1;
    }
    Py_ssize_t start = 0;
    Py_ssize_t stop = 0;
    Py_ssize_t step = 0;
    if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
        return -1;
    }
    // A snapshot of the source first: assigning a view to an overlapping view
    // of the same buffer must read everything before it writes anything.
    PyObject* items = PySequence_List(value);
    if (items == nullptr) {
        return -1;
    }
    const Py_ssize_t count = PyList_GET_SIZE(items);
    const Py_ssize_t size = SizeOf(self->type);
    std::vector<unsigned char> packed;
    try {
        packed.resize(static_cast<std::size_t>(count * size));
    } catch (const std::bad_alloc&) {
        Py_DECREF(items);
        PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (!PackElement(self->type, PyList_GET_ITEM(items, i), packed.data() + (i * size))) {
            Py_DECREF(items);
            return -1;
        }
    }
    Py_DECREF(items);
    // Only now, after every conversion has run, is the buffer looked at.
    const Py_ssize_t covered = PySlice_AdjustIndices(LiveLength(self), &start, &stop, step);
    if (covered != count) {
        PyErr_Format(PyExc_ValueError, "cannot assign %zd elements to a TypedArray slice of %zd: its length is fixed",
                     count, covered);
        return -1;
    }
    const Window window = WindowOf(self);
    if (count > 0 && !window.writable) {
        PyErr_SetString(PyExc_TypeError, "this TypedArray is over a bytes object and is read-only");
        return -1;
    }
    for (Py_ssize_t i = 0; i < count; ++i) {
        std::memcpy(window.data + ((start + (i * step)) * size), packed.data() + (i * size),
                    static_cast<std::size_t>(size));
    }
    return 0;
}

PyObject* TypedArrayToList(PyObject* object, PyObject* /*unused*/) {
    const TypedArrayObject* self = AsTypedArray(object);
    const Py_ssize_t length = LiveLength(self);
    PyObject* list = PyList_New(length);
    if (list == nullptr) {
        return nullptr;
    }
    // Unpacking allocates but runs no Python code that could touch the buffer,
    // except the collector - which frees, and never resizes, a bytearray.
    const Window window = WindowOf(self);
    for (Py_ssize_t i = 0; i < length; ++i) {
        PyObject* item = UnpackElement(self->type, window.data + (i * SizeOf(self->type)));
        if (item == nullptr) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, i, item);
    }
    return list;
}

/// `subarray(begin=0, end=None)`: JavaScript's, a view of the same buffer with
/// negative indices counted from the end and the range clamped.
PyObject* TypedArraySubarray(PyObject* object, PyObject* args, PyObject* kwds) {
    static const char* keywords[] = {"begin", "end", nullptr};
    PyObject* beginArgument = Py_None;
    PyObject* endArgument = Py_None;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "|OO:subarray", const_cast<char**>(keywords), &beginArgument,
                                    &endArgument) == 0) {
        return nullptr;
    }
    const TypedArrayObject* self = AsTypedArray(object);
    if (self->buffer == nullptr) {
        PyErr_SetString(PyExc_TypeError, "TypedArray has no buffer");
        return nullptr;
    }
    const auto relative = [](PyObject* argument, Py_ssize_t fallback, Py_ssize_t length, Py_ssize_t* out) {
        if (argument == Py_None) {
            *out = fallback;
            return true;
        }
        Py_ssize_t n = PyNumber_AsSsize_t(argument, nullptr);  // clamps instead of raising
        if (n == -1 && PyErr_Occurred() != nullptr) {
            return false;
        }
        if (n < 0) {
            n = std::max<Py_ssize_t>(n + length, 0);
        }
        *out = std::min(n, length);
        return true;
    };
    Py_ssize_t begin = 0;
    Py_ssize_t end = 0;
    if (!relative(beginArgument, 0, self->length, &begin) || !relative(endArgument, self->length, self->length, &end)) {
        return nullptr;
    }
    const Py_ssize_t count = std::max<Py_ssize_t>(end - begin, 0);
    return NewTypedArrayObject(Py_TYPE(object), self->type, self->buffer,
                               self->byteOffset + (begin * SizeOf(self->type)), count);
}

PyObject* TypedArrayRepr(PyObject* object) {
    const TypedArrayObject* self = AsTypedArray(object);
    if (!WindowOf(self).ok) {
        return PyUnicode_FromFormat("unibind.TypedArray('%s', <out of bounds>)", InfoOf(self->type).name);
    }
    PyObject* list = TypedArrayToList(object, nullptr);
    if (list == nullptr) {
        return nullptr;
    }
    PyObject* text = PyUnicode_FromFormat("unibind.TypedArray('%s', %R)", InfoOf(self->type).name, list);
    Py_DECREF(list);
    return text;
}

PyObject* TypedArrayGetBuffer(PyObject* object, void* /*closure*/) {
    PyObject* buffer = AsTypedArray(object)->buffer;
    return Py_NewRef(buffer != nullptr ? buffer : Py_None);
}
PyObject* TypedArrayGetByteOffset(PyObject* object, void* /*closure*/) {
    const TypedArrayObject* self = AsTypedArray(object);
    return PyLong_FromSsize_t(WindowOf(self).ok ? self->byteOffset : 0);
}
PyObject* TypedArrayGetByteLength(PyObject* object, void* /*closure*/) {
    const TypedArrayObject* self = AsTypedArray(object);
    return PyLong_FromSsize_t(LiveLength(self) * SizeOf(self->type));
}
PyObject* TypedArrayGetLength(PyObject* object, void* /*closure*/) {
    return PyLong_FromSsize_t(LiveLength(AsTypedArray(object)));
}
PyObject* TypedArrayGetType(PyObject* object, void* /*closure*/) {
    return PyUnicode_FromString(InfoOf(AsTypedArray(object)->type).name);
}
PyObject* TypedArrayGetBytesPerElement(PyObject* object, void* /*closure*/) {
    return PyLong_FromSsize_t(SizeOf(AsTypedArray(object)->type));
}

/// The buffer protocol: `memoryview(view)`, `bytes(view)`, `struct.unpack_from`
/// and every C library that takes a buffer see the view's own window, typed.
/// While one is exported the bytearray underneath has an export too, so it
/// cannot be resized until it is released - CPython's rule, and the right one.
int TypedArrayGetBufferProc(PyObject* object, Py_buffer* view, int flags) {
    TypedArrayObject* self = AsTypedArray(object);
    if (!WindowOf(self).ok) {
        PyErr_SetString(PyExc_BufferError, "TypedArray is out of bounds of its buffer");
        return -1;
    }
    auto* inner = static_cast<Py_buffer*>(PyMem_Malloc(sizeof(Py_buffer)));
    if (inner == nullptr) {
        PyErr_NoMemory();
        return -1;
    }
    if (PyObject_GetBuffer(self->buffer, inner, (flags & PyBUF_WRITABLE) != 0 ? PyBUF_WRITABLE : PyBUF_SIMPLE) != 0) {
        PyMem_Free(inner);
        return -1;
    }
    const Py_ssize_t bytes = self->length * SizeOf(self->type);
    if (self->byteOffset + bytes > inner->len) {
        PyBuffer_Release(inner);
        PyMem_Free(inner);
        PyErr_SetString(PyExc_BufferError, "TypedArray is out of bounds of its buffer");
        return -1;
    }
    view->buf = static_cast<char*>(inner->buf) + self->byteOffset;
    view->obj = Py_NewRef(object);
    view->len = bytes;
    view->itemsize = SizeOf(self->type);
    view->readonly = inner->readonly;
    view->ndim = 1;
    view->format = (flags & PyBUF_FORMAT) != 0 ? const_cast<char*>(InfoOf(self->type).format) : nullptr;
    view->shape = (flags & PyBUF_ND) == PyBUF_ND ? &self->shape : nullptr;
    view->strides = (flags & PyBUF_STRIDES) == PyBUF_STRIDES ? &self->stride : nullptr;
    view->suboffsets = nullptr;
    view->internal = inner;
    return 0;
}

void TypedArrayReleaseBufferProc(PyObject* /*object*/, Py_buffer* view) {
    auto* inner = static_cast<Py_buffer*>(view->internal);
    if (inner != nullptr) {
        PyBuffer_Release(inner);
        PyMem_Free(inner);
    }
}

PyGetSetDef typedArrayGetSet[] = {
    {"buffer", &TypedArrayGetBuffer, nullptr, "The bytearray (or bytes) this view looks at.", nullptr},
    {"byteOffset", &TypedArrayGetByteOffset, nullptr, "Where the view starts in its buffer; 0 if out of bounds.",
     nullptr},
    {"byteLength", &TypedArrayGetByteLength, nullptr, "Bytes the view covers; 0 if out of bounds.", nullptr},
    {"length", &TypedArrayGetLength, nullptr, "Elements the view covers; 0 if out of bounds. Same as len().", nullptr},
    {"type", &TypedArrayGetType, nullptr, "The element type's name: 'int32', 'float64', ...", nullptr},
    {"BYTES_PER_ELEMENT", &TypedArrayGetBytesPerElement, nullptr, "The element size in bytes.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMethodDef typedArrayMethods[] = {
    {"tolist", &TypedArrayToList, METH_NOARGS, "The elements as a list of int or float."},
    {"subarray", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(&TypedArraySubarray)),
     METH_VARARGS | METH_KEYWORDS,
     "subarray(begin=0, end=None): a view of the same buffer over elements [begin, end)."},
    {nullptr, nullptr, 0, nullptr},
};

PyMemberDef typedArrayMembers[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(TypedArrayObject, weaklist), Py_READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr},
};

constexpr const char* TYPED_ARRAY_DOC =
    "TypedArray(type, source=None, byteOffset=None, length=None)\n\n"
    "A typed window onto a bytearray: JavaScript's Int8Array ... Float64Array, BigInt64Array, Float16Array.\n"
    "`type` is 'int8', 'uint8', 'uint8clamped', 'int16', 'uint16', 'int32', 'uint32', 'float32', 'float64',\n"
    "'bigint64', 'biguint64' or 'float16' (JavaScript's names, like 'Int32Array', work too).\n\n"
    "source a bytearray/bytes: a view sharing it, from byteOffset for length elements (bytes: read-only).\n"
    "source an int: that many zeros. Any other iterable: its elements, converted, in a new buffer.\n\n"
    "Elements convert as JavaScript's do: integer types wrap, uint8clamped clamps and rounds half to even,\n"
    "float32/float16 round to nearest; the bigint types take ints only. A slice is a copy; subarray() shares.";

PyType_Slot typedArraySlots[] = {
    {Py_tp_new, reinterpret_cast<void*>(&TypedArrayNew)},
    {Py_tp_dealloc, reinterpret_cast<void*>(&TypedArrayDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&TypedArrayTraverse)},
    {Py_tp_clear, reinterpret_cast<void*>(&TypedArrayClear)},
    {Py_tp_repr, reinterpret_cast<void*>(&TypedArrayRepr)},
    {Py_tp_getset, typedArrayGetSet},
    {Py_tp_methods, typedArrayMethods},
    {Py_tp_members, typedArrayMembers},
    {Py_sq_length, reinterpret_cast<void*>(&TypedArrayLength)},
    {Py_sq_item, reinterpret_cast<void*>(&TypedArrayItem)},
    {Py_sq_ass_item, reinterpret_cast<void*>(&TypedArraySetItem)},
    {Py_mp_length, reinterpret_cast<void*>(&TypedArrayLength)},
    {Py_mp_subscript, reinterpret_cast<void*>(&TypedArraySubscript)},
    {Py_mp_ass_subscript, reinterpret_cast<void*>(&TypedArrayAssign)},
    {Py_bf_getbuffer, reinterpret_cast<void*>(&TypedArrayGetBufferProc)},
    {Py_bf_releasebuffer, reinterpret_cast<void*>(&TypedArrayReleaseBufferProc)},
    {Py_tp_doc, const_cast<char*>(TYPED_ARRAY_DOC)},
    {0, nullptr},
};

PyType_Spec typedArraySpec = {
    "unibind.TypedArray",
    sizeof(TypedArrayObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    typedArraySlots,
};

// ===========================================================================
// unibind.DataView
// ===========================================================================

/// `DataView(buffer, byteOffset=0, byteLength=None)`: a window of bytes with
/// no element type, read and written a typed value at a time at any offset,
/// in either byte order.
PyObject* DataViewNew(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static const char* keywords[] = {"buffer", "byteOffset", "byteLength", nullptr};
    PyObject* buffer = nullptr;
    PyObject* offsetArgument = Py_None;
    PyObject* lengthArgument = Py_None;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "O|OO:DataView", const_cast<char**>(keywords), &buffer, &offsetArgument,
                                    &lengthArgument) == 0) {
        return nullptr;
    }
    if (!IsBuffer(buffer)) {
        PyErr_Format(PyExc_TypeError, "DataView needs a bytearray or bytes, not '%.200s'", Py_TYPE(buffer)->tp_name);
        return nullptr;
    }
    const Py_ssize_t available = StorageOf(buffer).size;
    Py_ssize_t offset = 0;
    if (offsetArgument != Py_None && !IndexArgument(offsetArgument, "byteOffset", &offset)) {
        return nullptr;
    }
    if (offset > available) {
        PyErr_Format(RangeErrorClass(), "start offset %zd is outside the bounds of the buffer", offset);
        return nullptr;
    }
    Py_ssize_t length = available - offset;
    if (lengthArgument != Py_None) {
        if (!IndexArgument(lengthArgument, "byteLength", &length)) {
            return nullptr;
        }
        if (length > available - offset) {
            PyErr_Format(RangeErrorClass(), "invalid DataView length %zd", length);
            return nullptr;
        }
    }
    return NewDataViewObject(type, buffer, offset, length);
}

void DataViewDealloc(PyObject* object) {
    PyTypeObject* type = Py_TYPE(object);
    PyObject_GC_UnTrack(object);
    if (AsDataView(object)->weaklist != nullptr) {
        PyObject_ClearWeakRefs(object);
    }
    Py_CLEAR(AsDataView(object)->buffer);
    type->tp_free(object);
    Py_DECREF(type);
}

int DataViewTraverse(PyObject* object, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(object));
    Py_VISIT(AsDataView(object)->buffer);
    return 0;
}

int DataViewClear(PyObject* object) {
    Py_CLEAR(AsDataView(object)->buffer);
    return 0;
}

/// Where a get or set of `size` bytes at `offset` lands, checked in
/// JavaScript's order: a view over nothing is a TypeError, an offset past the
/// view's end a RangeError.
[[nodiscard]] unsigned char* DataViewTarget(const DataViewObject* self, Py_ssize_t offset, Py_ssize_t size,
                                            bool write) noexcept {
    const Window window = WindowOf(self);
    if (!window.ok) {
        PyErr_SetString(PyExc_TypeError, "DataView is out of bounds of its buffer");
        return nullptr;
    }
    if (offset > self->byteLength - size) {
        PyErr_SetString(RangeErrorClass(), "offset is outside the bounds of the DataView");
        return nullptr;
    }
    if (write && !window.writable) {
        PyErr_SetString(PyExc_TypeError, "this DataView is over a bytes object and is read-only");
        return nullptr;
    }
    return window.data + offset;
}

/// Swap `bytes` in place when the byte order asked for is not the host's.
void ToOrder(unsigned char* bytes, Py_ssize_t size, bool littleEndian) noexcept {
    if (littleEndian != NATIVE_LITTLE) {
        std::reverse(bytes, bytes + size);
    }
}

/// `getInt32(byteOffset, littleEndian=False)` and its siblings. Big-endian by
/// default, as JavaScript's are.
template <ElementType Type>
PyObject* DataViewGet(PyObject* object, PyObject* args, PyObject* kwds) {
    static const char* keywords[] = {"byteOffset", "littleEndian", nullptr};
    PyObject* offsetArgument = nullptr;
    int littleEndian = 0;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "O|p", const_cast<char**>(keywords), &offsetArgument, &littleEndian) ==
        0) {
        return nullptr;
    }
    Py_ssize_t offset = 0;
    if (!IndexArgument(offsetArgument, "byteOffset", &offset)) {
        return nullptr;
    }
    const Py_ssize_t size = SizeOf(Type);
    const unsigned char* source = DataViewTarget(AsDataView(object), offset, size, false);
    if (source == nullptr) {
        return nullptr;
    }
    unsigned char bytes[8];
    std::memcpy(bytes, source, static_cast<std::size_t>(size));
    ToOrder(bytes, size, littleEndian != 0);
    return UnpackElement(Type, bytes);
}

/// `setInt32(byteOffset, value, littleEndian=False)` and its siblings.
template <ElementType Type>
PyObject* DataViewSet(PyObject* object, PyObject* args, PyObject* kwds) {
    static const char* keywords[] = {"byteOffset", "value", "littleEndian", nullptr};
    PyObject* offsetArgument = nullptr;
    PyObject* value = nullptr;
    int littleEndian = 0;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "OO|p", const_cast<char**>(keywords), &offsetArgument, &value,
                                    &littleEndian) == 0) {
        return nullptr;
    }
    Py_ssize_t offset = 0;
    if (!IndexArgument(offsetArgument, "byteOffset", &offset)) {
        return nullptr;
    }
    const Py_ssize_t size = SizeOf(Type);
    unsigned char bytes[8];
    // The value converts before the buffer is looked at; see the file comment.
    if (!PackElement(Type, value, bytes)) {
        return nullptr;
    }
    ToOrder(bytes, size, littleEndian != 0);
    unsigned char* target = DataViewTarget(AsDataView(object), offset, size, true);
    if (target == nullptr) {
        return nullptr;
    }
    std::memcpy(target, bytes, static_cast<std::size_t>(size));
    Py_RETURN_NONE;
}

PyObject* DataViewGetBuffer(PyObject* object, void* /*closure*/) {
    PyObject* buffer = AsDataView(object)->buffer;
    return Py_NewRef(buffer != nullptr ? buffer : Py_None);
}
PyObject* DataViewGetByteOffset(PyObject* object, void* /*closure*/) {
    const DataViewObject* self = AsDataView(object);
    return PyLong_FromSsize_t(WindowOf(self).ok ? self->byteOffset : 0);
}
PyObject* DataViewGetByteLength(PyObject* object, void* /*closure*/) {
    const DataViewObject* self = AsDataView(object);
    return PyLong_FromSsize_t(WindowOf(self).ok ? self->byteLength : 0);
}

PyObject* DataViewRepr(PyObject* object) {
    const DataViewObject* self = AsDataView(object);
    if (!WindowOf(self).ok) {
        return PyUnicode_FromString("unibind.DataView(<out of bounds>)");
    }
    return PyUnicode_FromFormat("unibind.DataView(byteOffset=%zd, byteLength=%zd)", self->byteOffset, self->byteLength);
}

// A keyword-taking method is stored as a PyCFunction; the cast goes through
// void(*)() so that it is a deliberate one.
template <ElementType Type>
[[nodiscard]] PyCFunction Getter() noexcept {
    return reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(&DataViewGet<Type>));
}
template <ElementType Type>
[[nodiscard]] PyCFunction Setter() noexcept {
    return reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(&DataViewSet<Type>));
}

constexpr int VIEW_FLAGS = METH_VARARGS | METH_KEYWORDS;

PyMethodDef dataViewMethods[] = {
    {"getInt8", Getter<ElementType::Int8>(), VIEW_FLAGS, "getInt8(byteOffset)"},
    {"getUint8", Getter<ElementType::Uint8>(), VIEW_FLAGS, "getUint8(byteOffset)"},
    {"getInt16", Getter<ElementType::Int16>(), VIEW_FLAGS, "getInt16(byteOffset, littleEndian=False)"},
    {"getUint16", Getter<ElementType::Uint16>(), VIEW_FLAGS, "getUint16(byteOffset, littleEndian=False)"},
    {"getInt32", Getter<ElementType::Int32>(), VIEW_FLAGS, "getInt32(byteOffset, littleEndian=False)"},
    {"getUint32", Getter<ElementType::Uint32>(), VIEW_FLAGS, "getUint32(byteOffset, littleEndian=False)"},
    {"getFloat16", Getter<ElementType::Float16>(), VIEW_FLAGS, "getFloat16(byteOffset, littleEndian=False)"},
    {"getFloat32", Getter<ElementType::Float32>(), VIEW_FLAGS, "getFloat32(byteOffset, littleEndian=False)"},
    {"getFloat64", Getter<ElementType::Float64>(), VIEW_FLAGS, "getFloat64(byteOffset, littleEndian=False)"},
    {"getBigInt64", Getter<ElementType::BigInt64>(), VIEW_FLAGS, "getBigInt64(byteOffset, littleEndian=False)"},
    {"getBigUint64", Getter<ElementType::BigUint64>(), VIEW_FLAGS, "getBigUint64(byteOffset, littleEndian=False)"},
    {"setInt8", Setter<ElementType::Int8>(), VIEW_FLAGS, "setInt8(byteOffset, value)"},
    {"setUint8", Setter<ElementType::Uint8>(), VIEW_FLAGS, "setUint8(byteOffset, value)"},
    {"setInt16", Setter<ElementType::Int16>(), VIEW_FLAGS, "setInt16(byteOffset, value, littleEndian=False)"},
    {"setUint16", Setter<ElementType::Uint16>(), VIEW_FLAGS, "setUint16(byteOffset, value, littleEndian=False)"},
    {"setInt32", Setter<ElementType::Int32>(), VIEW_FLAGS, "setInt32(byteOffset, value, littleEndian=False)"},
    {"setUint32", Setter<ElementType::Uint32>(), VIEW_FLAGS, "setUint32(byteOffset, value, littleEndian=False)"},
    {"setFloat16", Setter<ElementType::Float16>(), VIEW_FLAGS, "setFloat16(byteOffset, value, littleEndian=False)"},
    {"setFloat32", Setter<ElementType::Float32>(), VIEW_FLAGS, "setFloat32(byteOffset, value, littleEndian=False)"},
    {"setFloat64", Setter<ElementType::Float64>(), VIEW_FLAGS, "setFloat64(byteOffset, value, littleEndian=False)"},
    {"setBigInt64", Setter<ElementType::BigInt64>(), VIEW_FLAGS, "setBigInt64(byteOffset, value, littleEndian=False)"},
    {"setBigUint64", Setter<ElementType::BigUint64>(), VIEW_FLAGS,
     "setBigUint64(byteOffset, value, littleEndian=False)"},
    {nullptr, nullptr, 0, nullptr},
};

PyGetSetDef dataViewGetSet[] = {
    {"buffer", &DataViewGetBuffer, nullptr, "The bytearray (or bytes) this view looks at.", nullptr},
    {"byteOffset", &DataViewGetByteOffset, nullptr, "Where the view starts in its buffer; 0 if out of bounds.",
     nullptr},
    {"byteLength", &DataViewGetByteLength, nullptr, "Bytes the view covers; 0 if out of bounds.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMemberDef dataViewMembers[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(DataViewObject, weaklist), Py_READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr},
};

constexpr const char* DATA_VIEW_DOC =
    "DataView(buffer, byteOffset=0, byteLength=None)\n\n"
    "JavaScript's DataView over a bytearray (or, read-only, a bytes): getInt8 ... getBigUint64 and\n"
    "setInt8 ... setBigUint64 at any byte offset, big-endian unless littleEndian=True.";

PyType_Slot dataViewSlots[] = {
    {Py_tp_new, reinterpret_cast<void*>(&DataViewNew)},
    {Py_tp_dealloc, reinterpret_cast<void*>(&DataViewDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&DataViewTraverse)},
    {Py_tp_clear, reinterpret_cast<void*>(&DataViewClear)},
    {Py_tp_repr, reinterpret_cast<void*>(&DataViewRepr)},
    {Py_tp_methods, dataViewMethods},
    {Py_tp_getset, dataViewGetSet},
    {Py_tp_members, dataViewMembers},
    {Py_tp_doc, const_cast<char*>(DATA_VIEW_DOC)},
    {0, nullptr},
};

PyType_Spec dataViewSpec = {
    "unibind.DataView", sizeof(DataViewObject), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    dataViewSlots,
};

// ===========================================================================
// Views, from the C++ side
// ===========================================================================

[[nodiscard]] bool IsTypedArray(Isolate& isolate, PyObject* object) noexcept {
    PyTypeObject* type = isolate.impl().types.typedArray;
    return type != nullptr && PyObject_TypeCheck(object, type);
}

[[nodiscard]] bool IsDataView(Isolate& isolate, PyObject* object) noexcept {
    PyTypeObject* type = isolate.impl().types.dataView;
    return type != nullptr && PyObject_TypeCheck(object, type);
}

/// Any view's buffer and its window in bytes, as declared.
struct ViewFields {
    PyObject* buffer = nullptr;  ///< borrowed
    Py_ssize_t byteOffset = 0;
    Py_ssize_t byteLength = 0;
};

[[nodiscard]] ViewFields FieldsOf(Slot slot) noexcept {
    PyObject* object = Resolve(slot);
    Isolate& isolate = IsolateFor(slot);
    if (IsTypedArray(isolate, object)) {
        const TypedArrayObject* self = AsTypedArray(object);
        return {self->buffer, self->byteOffset, self->length * SizeOf(self->type)};
    }
    if (IsDataView(isolate, object)) {
        const DataViewObject* self = AsDataView(object);
        return {self->buffer, self->byteOffset, self->byteLength};
    }
    return {};
}

[[nodiscard]] std::size_t CopyWindow(const ViewFields& fields, std::span<std::byte> out) noexcept {
    const Window window = WindowOf(fields.buffer, fields.byteOffset, fields.byteLength);
    if (!window.ok) {
        return 0;
    }
    const std::size_t count = std::min(static_cast<std::size_t>(fields.byteLength), out.size());
    if (count > 0) {
        std::memcpy(out.data(), window.data, count);
    }
    return count;
}

}  // namespace

// --- binary data: the contract ------------------------------------------------------

std::optional<Slot> MakeArrayBuffer(const Context& context, std::span<const std::byte> bytes, std::size_t byteLength) {
    Isolate& isolate = OwnerOf(context);
    if (bytes.size() > byteLength || byteLength > static_cast<std::size_t>(PY_SSIZE_T_MAX)) {
        return std::nullopt;
    }
    PyObject* buffer = NewByteArray(static_cast<Py_ssize_t>(byteLength));
    if (buffer == nullptr) {
        // "Empty, with nothing thrown": a length this engine cannot allocate is
        // an answer, not an exception.
        PyErr_Clear();
        return std::nullopt;
    }
    char* data = PyByteArray_AS_STRING(buffer);
    if (!bytes.empty()) {
        std::memcpy(data, bytes.data(), bytes.size());
    }
    if (byteLength > bytes.size()) {
        std::memset(data + bytes.size(), 0, byteLength - bytes.size());
    }
    return PushOrNothing(isolate, buffer);
}

std::size_t ArrayBufferByteLength(Slot buffer) noexcept {
    return static_cast<std::size_t>(StorageOf(Resolve(buffer)).size);
}

std::size_t ArrayBufferCopyOut(Slot buffer, std::span<std::byte> out) noexcept {
    const Storage storage = StorageOf(Resolve(buffer));
    const std::size_t count = std::min(static_cast<std::size_t>(storage.size), out.size());
    if (count > 0) {
        std::memcpy(out.data(), storage.data, count);
    }
    return count;
}

std::optional<Slot> MakeTypedArray(const Context& context, ElementType type, Slot buffer, std::size_t byteOffset,
                                   std::size_t length) {
    Isolate& isolate = OwnerOf(context);
    PyObject* target = Resolve(buffer);
    PyTypeObject* viewType = isolate.impl().types.typedArray;
    if (!IsBuffer(target) || viewType == nullptr || static_cast<int>(type) >= ELEMENT_TYPE_COUNT) {
        return std::nullopt;
    }
    // Every check is made here, and none of them throws: the header's answer
    // to a view that does not fit is empty, as SpiderMonkey's backend gives it.
    // The multiplication is guarded before it is made, so that a length that
    // wrapped cannot pass the bounds test.
    const auto available = static_cast<std::size_t>(StorageOf(target).size);
    const std::size_t size = ElementSize(type);
    if (length > available / size || byteOffset > available || length * size > available - byteOffset ||
        byteOffset % size != 0) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, NewTypedArrayObject(viewType, type, target, static_cast<Py_ssize_t>(byteOffset),
                                                      static_cast<Py_ssize_t>(length)));
}

ElementType TypedArrayElementType(Slot view) noexcept {
    PyObject* object = Resolve(view);
    if (!IsTypedArray(IsolateFor(view), object)) {
        return ElementType::Uint8;
    }
    return AsTypedArray(object)->type;
}

std::size_t TypedArrayLength(Slot view) noexcept {
    PyObject* object = Resolve(view);
    if (!IsTypedArray(IsolateFor(view), object)) {
        return 0;
    }
    return static_cast<std::size_t>(LiveLength(AsTypedArray(object)));
}

std::size_t TypedArrayByteOffset(Slot view) noexcept {
    return ArrayBufferViewByteOffset(view);
}

std::optional<Slot> TypedArrayBuffer(const Context& context, Slot view) {
    return ArrayBufferViewBuffer(context, view);
}

std::size_t TypedArrayCopyOut(Slot view, std::span<std::byte> out) noexcept {
    return ArrayBufferViewCopyOut(view, out);
}

std::size_t ArrayBufferViewByteLength(Slot view) noexcept {
    const ViewFields fields = FieldsOf(view);
    return WindowOf(fields.buffer, fields.byteOffset, fields.byteLength).ok
               ? static_cast<std::size_t>(fields.byteLength)
               : 0;
}

std::size_t ArrayBufferViewByteOffset(Slot view) noexcept {
    // Zero for a view over nothing as well as its length: backend.h, and V8.
    const ViewFields fields = FieldsOf(view);
    return WindowOf(fields.buffer, fields.byteOffset, fields.byteLength).ok
               ? static_cast<std::size_t>(fields.byteOffset)
               : 0;
}

std::optional<Slot> ArrayBufferViewBuffer(const Context& context, Slot view) {
    const ViewFields fields = FieldsOf(view);
    if (fields.buffer == nullptr) {
        return std::nullopt;
    }
    return PushBorrowed(OwnerOf(context), fields.buffer);
}

std::size_t ArrayBufferViewCopyOut(Slot view, std::span<std::byte> out) noexcept {
    return CopyWindow(FieldsOf(view), out);
}

std::optional<Slot> MakeDataView(const Context& context, Slot buffer, std::size_t byteOffset, std::size_t byteLength) {
    Isolate& isolate = OwnerOf(context);
    PyObject* target = Resolve(buffer);
    PyTypeObject* viewType = isolate.impl().types.dataView;
    if (!IsBuffer(target) || viewType == nullptr) {
        return std::nullopt;
    }
    const auto available = static_cast<std::size_t>(StorageOf(target).size);
    if (byteOffset > available || byteLength > available - byteOffset) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, NewDataViewObject(viewType, target, static_cast<Py_ssize_t>(byteOffset),
                                                    static_cast<Py_ssize_t>(byteLength)));
}

namespace {

// ===========================================================================
// Blob framing
// ===========================================================================
//
// Both kinds of blob are `marshal` output behind a header of this file's own.
// The header is checked in full before `marshal` reads a byte: a truncated
// blob, a flipped bit or a blob from another interpreter version is refused
// here, which matters most for code - `marshal.loads` trusts a code object's
// fields, and a damaged one can take the process down. The hash is FNV-1a, the
// same as unibind/script.h's frame uses: not a defence against a forger, which
// the contract does not ask for (a blob is the embedder's own bytes), but
// certain to catch the accident.

struct BlobHeader {
    std::uint32_t magic = 0;
    std::uint16_t format = 0;
    std::uint16_t marshalVersion = 0;
    std::uint32_t pythonVersion = 0;  ///< PY_VERSION_HEX: marshal's format is the interpreter's
    std::uint32_t bytecodeMagic = 0;  ///< the .pyc magic number; zero in a clone blob
    std::uint64_t payloadLength = 0;
    std::uint64_t payloadHash = 0;
};
static_assert(sizeof(BlobHeader) == 32, "BlobHeader is written as raw bytes; keep it free of padding");

constexpr std::uint32_t CLONE_MAGIC = 0x76704255;  // "UBpv": a Python value
constexpr std::uint32_t CODE_MAGIC = 0x63704255;   // "UBpc": Python code
constexpr std::uint16_t CLONE_FORMAT = 1;
constexpr std::uint16_t CODE_FORMAT = 1;

/// marshal's format for a clone payload: version 2, the last before marshal
/// began writing back-references. Those are chosen by reference count, so the
/// same value could be written two ways; without them a value graph has one
/// encoding, and re-serializing a clone gives back the bytes it came from. The
/// graph's own sharing is in the node indices, not in marshal's references.
/// Version 2 already writes a float as its eight bytes, so NaN and -0 are exact.
constexpr int CLONE_MARSHAL_VERSION = 2;

/// The bytecode magic number, which is what `marshal` would refuse code over
/// in a `.pyc`. Asked of the interpreter once and kept; zero if it could not
/// say, which then simply has to match itself.
[[nodiscard]] std::uint32_t BytecodeMagic() noexcept {
    static const std::uint32_t magic = [] {
        const long value = PyImport_GetMagicNumber();
        if (value == -1 && PyErr_Occurred() != nullptr) {
            PyErr_Clear();
            return std::uint32_t{0};
        }
        return static_cast<std::uint32_t>(value);
    }();
    return magic;
}

/// Frame `payload` (a `bytes`, borrowed) as a blob of this kind.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> FrameBlob(PyObject* payload, std::uint32_t magic,
                                                                 std::uint16_t format, int marshalVersion,
                                                                 std::uint32_t bytecodeMagic) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(PyBytes_AS_STRING(payload));
    const auto size = static_cast<std::size_t>(PyBytes_GET_SIZE(payload));
    const std::span<const std::uint8_t> body(bytes, size);
    BlobHeader header;
    header.magic = magic;
    header.format = format;
    header.marshalVersion = static_cast<std::uint16_t>(marshalVersion);
    header.pythonVersion = PY_VERSION_HEX;
    header.bytecodeMagic = bytecodeMagic;
    header.payloadLength = size;
    header.payloadHash = CodeCachePayloadHash(body);
    std::vector<std::uint8_t> blob(sizeof(BlobHeader) + size);
    std::memcpy(blob.data(), &header, sizeof(header));
    if (size > 0) {
        std::memcpy(blob.data() + sizeof(header), bytes, size);
    }
    return blob;
}

/// The payload of a blob of this kind, or empty if any field of the header
/// disagrees - including the length, so a blob with bytes appended is refused
/// as surely as one with bytes missing.
[[nodiscard]] std::span<const std::uint8_t> UnframeBlob(std::span<const std::uint8_t> blob, std::uint32_t magic,
                                                        std::uint16_t format, int marshalVersion,
                                                        std::uint32_t bytecodeMagic) noexcept {
    if (blob.size() < sizeof(BlobHeader)) {
        return {};
    }
    BlobHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    const std::span<const std::uint8_t> payload = blob.subspan(sizeof(BlobHeader));
    if (header.magic != magic || header.format != format || header.marshalVersion != marshalVersion ||
        header.pythonVersion != PY_VERSION_HEX || header.bytecodeMagic != bytecodeMagic ||
        header.payloadLength != payload.size() || payload.empty() ||
        header.payloadHash != CodeCachePayloadHash(payload)) {
        return {};
    }
    return payload;
}

// ===========================================================================
// Structured clone
// ===========================================================================
//
// The payload is `marshal` of `(root, nodes)`: `nodes` a list of tuples, each
// `(tag, ...)`, and `root` the index of the value that was cloned. Children are
// indices into `nodes`, so the graph's shape - shared references, cycles - is
// written down exactly, and no level of it is nested inside another.
//
//   (UNDEFINED,)                         None
//   (NULL,)                              unibind.null
//   (BOOL, b)  (INT, n)  (FLOAT, x)      bool, int of any size, float (NaN, -0 exact)
//   (STR, s)                             str - lone surrogates too, marshal keeps them
//   (BYTES, b)  (BYTEARRAY, b)           a read-only and a mutable ArrayBuffer
//   (TYPED, type, buffer, offset, len)   TypedArray over node `buffer`
//   (VIEW, buffer, offset, len)          DataView over node `buffer`
//   (LIST, [i...])  (TUPLE, [i...])      list, tuple
//   (DICT, [k, v, k, v...])              dict, keys any clonable value
//   (OBJECT, [k, v, k, v...])            unibind.Object: own enumerable string-keyed properties

enum Tag : std::uint8_t {
    TAG_UNDEFINED,
    TAG_NULL,
    TAG_BOOL,
    TAG_INT,
    TAG_FLOAT,
    TAG_STR,
    TAG_BYTES,
    TAG_BYTEARRAY,
    TAG_TYPED,
    TAG_VIEW,
    TAG_LIST,
    TAG_TUPLE,
    TAG_DICT,
    TAG_OBJECT,
    TAG_COUNT,
};

/// `unibind.DataCloneError` - or TypeError, in the moment before the module
/// has made it.
[[nodiscard]] PyObject* CloneErrorClass(Isolate& isolate) noexcept {
    PyObject* type = isolate.impl().types.dataCloneError;
    return type != nullptr ? type : PyExc_TypeError;
}

void RaiseCloneError(Isolate& isolate, const char* message) noexcept {
    PyErr_SetString(CloneErrorClass(isolate), message);
}

/// Whether `object` is a plain `unibind.Object` - not a template's or a class's
/// instance, which derive from it and may carry a native that cannot be cloned.
[[nodiscard]] bool IsPlainObject(Isolate& isolate, PyObject* object) noexcept {
    PyTypeObject* type = isolate.impl().types.object;
    return type != nullptr && Py_TYPE(object) == type && reinterpret_cast<ObjectInstance*>(object)->box == nullptr;
}

/// Writes a value graph down as nodes. Iterative - a work list, not recursion -
/// so a list nested a million deep is a long loop, not a stack overflow.
class Encoder {
   public:
    Encoder(const Context& context, Isolate& isolate) noexcept : context_(context), isolate_(isolate) {}
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;
    ~Encoder() {
        for (PyObject* node : nodes_) {
            Py_XDECREF(node);
        }
        for (PyObject* held : held_) {
            Py_DECREF(held);
        }
    }

    /// The `marshal` payload for `root`, a new `bytes`; null with an exception
    /// pending if the graph holds something that does not clone.
    [[nodiscard]] PyObject* Encode(PyObject* root) {
        const Py_ssize_t rootIndex = Visit(root);
        if (rootIndex < 0) {
            return nullptr;
        }
        while (!pending_.empty()) {
            const auto [object, index] = pending_.back();
            pending_.pop_back();
            if (!Expand(object, index)) {
                return nullptr;
            }
        }
        if (!CheckViews()) {
            return nullptr;
        }
        PyObject* list = PyList_New(static_cast<Py_ssize_t>(nodes_.size()));
        if (list == nullptr) {
            return nullptr;
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), std::exchange(nodes_[i], nullptr));
        }
        PyObject* payload = Py_BuildValue("(nN)", rootIndex, list);
        if (payload == nullptr) {
            return nullptr;
        }
        PyObject* bytes = PyMarshal_WriteObjectToString(payload, CLONE_MARSHAL_VERSION);
        Py_DECREF(payload);
        return bytes;
    }

   private:
    /// A view written down, to be checked against its buffer once the whole
    /// graph is: a getter run later in the walk may have shrunk the buffer.
    struct ViewRecord {
        Py_ssize_t buffer;
        Py_ssize_t end;  ///< byteOffset + byteLength
    };

    /// The node index for `value`, assigning one - and writing a leaf node
    /// straight away, or queueing a container to be expanded - if it has none.
    /// Runs no Python code, so the caller's iteration cannot be disturbed.
    [[nodiscard]] Py_ssize_t Visit(PyObject* value) {
        if (const auto found = memo_.find(value); found != memo_.end()) {
            return found->second;
        }
        const Types& types = isolate_.impl().types;
        const auto index = static_cast<Py_ssize_t>(nodes_.size());
        PyObject* node = nullptr;
        bool container = false;
        if (value == Py_None) {
            node = Py_BuildValue("(i)", TAG_UNDEFINED);
        } else if (value == types.nullValue) {
            node = Py_BuildValue("(i)", TAG_NULL);
        } else if (PyBool_Check(value)) {
            node = Py_BuildValue("(iO)", TAG_BOOL, value);
        } else if (PyLong_CheckExact(value)) {
            node = Py_BuildValue("(iO)", TAG_INT, value);
        } else if (PyFloat_CheckExact(value)) {
            node = Py_BuildValue("(iO)", TAG_FLOAT, value);
        } else if (PyUnicode_CheckExact(value)) {
            node = Py_BuildValue("(iO)", TAG_STR, value);
        } else if (PyBytes_CheckExact(value)) {
            node = Py_BuildValue("(iO)", TAG_BYTES, value);
            sizes_[index] = PyBytes_GET_SIZE(value);
        } else if (PyByteArray_CheckExact(value)) {
            // Its bytes as they are now, which is when the walk reached it.
            node = Py_BuildValue("(iy#)", TAG_BYTEARRAY, PyByteArray_AS_STRING(value), PyByteArray_GET_SIZE(value));
            sizes_[index] = PyByteArray_GET_SIZE(value);
        } else if ((types.typedArray != nullptr && Py_TYPE(value) == types.typedArray) ||
                   (types.dataView != nullptr && Py_TYPE(value) == types.dataView) || PyList_CheckExact(value) ||
                   PyTuple_CheckExact(value) || PyDict_CheckExact(value) || IsPlainObject(isolate_, value)) {
            container = true;
        } else {
            // A function, a symbol, an External, a promise, a class instance -
            // or a subclass of a clonable type, whose class could not be
            // rebuilt from the data alone. The whole clone fails (value.h).
            const char* what = nullptr;
            if (PyCallable_Check(value) != 0) {
                what = "a function";
            } else if (IsSymbol(isolate_, value)) {
                what = "a Symbol";
            } else if (PyObject_TypeCheck(value, types.external) != 0) {
                what = "an External";
            }
            if (what != nullptr) {
                PyErr_Format(CloneErrorClass(isolate_), "%s could not be cloned", what);
            } else {
                PyErr_Format(CloneErrorClass(isolate_), "a '%.200s' could not be cloned", Py_TYPE(value)->tp_name);
            }
            return -1;
        }
        if (!container && node == nullptr) {
            return -1;
        }
        try {
            nodes_.push_back(node);
            held_.push_back(Py_NewRef(value));
            memo_.emplace(value, index);
            if (container) {
                pending_.emplace_back(value, index);
            }
        } catch (const std::bad_alloc&) {
            PyErr_NoMemory();
            return -1;
        }
        return index;
    }

    /// Visit every item of a snapshot of `items` (a list or tuple), as a list
    /// of indices. New reference, or null.
    [[nodiscard]] PyObject* VisitAll(PyObject* items) {
        const Py_ssize_t count = PySequence_Fast_GET_SIZE(items);
        PyObject* indices = PyList_New(count);
        if (indices == nullptr) {
            return nullptr;
        }
        for (Py_ssize_t i = 0; i < count; ++i) {
            const Py_ssize_t child = Visit(PySequence_Fast_GET_ITEM(items, i));
            PyObject* number = child < 0 ? nullptr : PyLong_FromSsize_t(child);
            if (number == nullptr) {
                Py_DECREF(indices);
                return nullptr;
            }
            PyList_SET_ITEM(indices, i, number);
        }
        return indices;
    }

    /// Write the node for a container reached by `Visit`.
    [[nodiscard]] bool Expand(PyObject* object, Py_ssize_t index) {
        const Types& types = isolate_.impl().types;
        PyObject* node = nullptr;
        if (PyList_CheckExact(object) || PyTuple_CheckExact(object)) {
            // A snapshot, so that nothing - not even a finaliser the collector
            // runs while this allocates - can change the list under the loop.
            PyObject* items = PySequence_Tuple(object);
            if (items == nullptr) {
                return false;
            }
            PyObject* indices = VisitAll(items);
            Py_DECREF(items);
            if (indices == nullptr) {
                return false;
            }
            node = Py_BuildValue("(iN)", PyList_CheckExact(object) ? TAG_LIST : TAG_TUPLE, indices);
        } else if (PyDict_CheckExact(object)) {
            PyObject* items = PyDict_Items(object);
            if (items == nullptr) {
                return false;
            }
            PyObject* flat = FlattenPairs(items);
            Py_DECREF(items);
            if (flat == nullptr) {
                return false;
            }
            PyObject* indices = VisitAll(flat);
            Py_DECREF(flat);
            if (indices == nullptr) {
                return false;
            }
            node = Py_BuildValue("(iN)", TAG_DICT, indices);
        } else if (Py_TYPE(object) == types.typedArray) {
            const TypedArrayObject* view = AsTypedArray(object);
            if (!WindowOf(view).ok) {
                RaiseCloneError(isolate_, "a TypedArray out of bounds of its buffer could not be cloned");
                return false;
            }
            const Py_ssize_t buffer = Visit(view->buffer);
            if (buffer < 0 || !Record(buffer, view->byteOffset + (view->length * SizeOf(view->type)))) {
                return false;
            }
            node = Py_BuildValue("(iinnn)", TAG_TYPED, static_cast<int>(view->type), buffer, view->byteOffset,
                                 view->length);
        } else if (Py_TYPE(object) == types.dataView) {
            const DataViewObject* view = AsDataView(object);
            if (!WindowOf(view).ok) {
                RaiseCloneError(isolate_, "a DataView out of bounds of its buffer could not be cloned");
                return false;
            }
            const Py_ssize_t buffer = Visit(view->buffer);
            if (buffer < 0 || !Record(buffer, view->byteOffset + view->byteLength)) {
                return false;
            }
            node = Py_BuildValue("(innn)", TAG_VIEW, buffer, view->byteOffset, view->byteLength);
        } else {
            PyObject* flat = ObjectPairs(object);
            if (flat == nullptr) {
                return false;
            }
            PyObject* indices = VisitAll(flat);
            Py_DECREF(flat);
            if (indices == nullptr) {
                return false;
            }
            node = Py_BuildValue("(iN)", TAG_OBJECT, indices);
        }
        if (node == nullptr) {
            return false;
        }
        nodes_[static_cast<std::size_t>(index)] = node;
        return true;
    }

    /// `[(k, v), ...]` as `[k, v, ...]`. New reference.
    [[nodiscard]] static PyObject* FlattenPairs(PyObject* pairs) {
        const Py_ssize_t count = PyList_GET_SIZE(pairs);
        PyObject* flat = PyList_New(count * 2);
        if (flat == nullptr) {
            return nullptr;
        }
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject* pair = PyList_GET_ITEM(pairs, i);
            PyList_SET_ITEM(flat, 2 * i, Py_NewRef(PyTuple_GET_ITEM(pair, 0)));
            PyList_SET_ITEM(flat, (2 * i) + 1, Py_NewRef(PyTuple_GET_ITEM(pair, 1)));
        }
        return flat;
    }

    /// A `unibind.Object`'s own enumerable string-keyed properties, read the
    /// way script reads them - through the objects area, so an accessor's
    /// getter runs, as structured clone's [[Get]] does - as `[k, v, ...]`.
    /// Symbol keys are skipped, as JavaScript's clone skips them. New
    /// reference, or null with an exception pending.
    [[nodiscard]] PyObject* ObjectPairs(PyObject* object) {
        const HandleScope scope(isolate_);
        const Slot self = PushBorrowed(isolate_, object);
        if (self.IsEmpty()) {
            return nullptr;
        }
        const std::optional<Slot> keys = GetOwnPropertyNames(context_, self, KeyFilter{});
        if (!keys) {
            if (PyErr_Occurred() == nullptr) {
                RaiseCloneError(isolate_, "the object's properties could not be listed");
            }
            return nullptr;
        }
        PyObject* list = PySequence_Fast(Resolve(*keys), "own property names are not a sequence");
        if (list == nullptr) {
            return nullptr;
        }
        PyObject* flat = PyList_New(0);
        for (Py_ssize_t i = 0; flat != nullptr && i < PySequence_Fast_GET_SIZE(list); ++i) {
            PyObject* key = PySequence_Fast_GET_ITEM(list, i);
            if (IsSymbol(isolate_, key)) {
                continue;
            }
            const Slot keySlot = PushBorrowed(isolate_, key);
            const std::optional<Slot> value = keySlot.IsEmpty() ? std::nullopt : GetProperty(context_, self, keySlot);
            if (!value) {
                if (PyErr_Occurred() == nullptr) {
                    RaiseCloneError(isolate_, "a property of the object could not be read");
                }
                Py_CLEAR(flat);
                break;
            }
            if (PyList_Append(flat, key) != 0 || PyList_Append(flat, Resolve(*value)) != 0) {
                Py_CLEAR(flat);
            }
        }
        Py_DECREF(list);
        return flat;
    }

    [[nodiscard]] bool Record(Py_ssize_t buffer, Py_ssize_t end) {
        try {
            views_.push_back({buffer, end});
        } catch (const std::bad_alloc&) {
            PyErr_NoMemory();
            return false;
        }
        return true;
    }

    /// Every view against its buffer's bytes as written. A mismatch means
    /// script shrank a buffer during the walk; the reader would refuse the
    /// blob, so refuse to write it.
    [[nodiscard]] bool CheckViews() {
        for (const ViewRecord& view : views_) {
            const auto size = sizes_.find(view.buffer);
            if (size == sizes_.end() || view.end > size->second) {
                RaiseCloneError(isolate_, "a view's buffer shrank while it was being cloned");
                return false;
            }
        }
        return true;
    }

    const Context& context_;
    Isolate& isolate_;
    std::vector<PyObject*> nodes_;                           ///< owned; null until expanded
    std::vector<PyObject*> held_;                            ///< owned: keeps every memo key alive
    std::unordered_map<PyObject*, Py_ssize_t> memo_;         ///< identity -> node index
    std::vector<std::pair<PyObject*, Py_ssize_t>> pending_;  ///< containers still to expand
    std::unordered_map<Py_ssize_t, Py_ssize_t> sizes_;       ///< buffer node -> bytes written
    std::vector<ViewRecord> views_;
};

/// Reads nodes back into a value graph, after checking every one of them.
///
/// Nothing is built until the whole node list has been validated: each node a
/// tuple of exactly the arity and field types its tag has, every index in
/// range and pointing at the kind of node its field needs, every view inside
/// its buffer. After that, building cannot fail on a malformed blob - only on
/// memory, or on a dict key a forged blob made unhashable, which is an ordinary
/// TypeError. Then it builds in passes, so that no object is used before it is
/// complete in the way the use needs:
///
///   1. every leaf, and every container empty;
///   2. every view, over its buffer from pass 1;
///   3. every list and tuple filled - no hashing happens here, so a tuple can
///      be filled whatever its items are;
///   4. every dict and object filled - keys hash now, and every tuple a key
///      could be is complete.
class Decoder {
   public:
    Decoder(const Context& context, Isolate& isolate) noexcept : context_(context), isolate_(isolate) {}
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
    ~Decoder() {
        for (PyObject* object : built_) {
            Py_XDECREF(object);
        }
        Py_XDECREF(payload_);
    }

    /// The value, a new reference; null with an exception pending if the
    /// payload is not one `Encoder` could have written.
    [[nodiscard]] PyObject* Decode(std::span<const std::uint8_t> bytes) {
        payload_ = PyMarshal_ReadObjectFromString(reinterpret_cast<const char*>(bytes.data()),
                                                  static_cast<Py_ssize_t>(bytes.size()));
        if (payload_ == nullptr) {
            // marshal's own complaint says nothing the caller can use.
            PyErr_Clear();
            return Refuse("the blob is damaged");
        }
        if (!PyTuple_CheckExact(payload_) || PyTuple_GET_SIZE(payload_) != 2 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(payload_, 0)) || !PyList_CheckExact(PyTuple_GET_ITEM(payload_, 1))) {
            return Refuse("the blob is not a clone");
        }
        nodes_ = PyTuple_GET_ITEM(payload_, 1);
        count_ = PyList_GET_SIZE(nodes_);
        Py_ssize_t root = 0;
        if (!IndexOf(PyTuple_GET_ITEM(payload_, 0), &root) || !Validate() || !TuplesAcyclic()) {
            return Refuse("the blob is not a well-formed clone");
        }
        try {
            built_.assign(static_cast<std::size_t>(count_), nullptr);
        } catch (const std::bad_alloc&) {
            return PyErr_NoMemory();
        }
        if (!BuildLeaves() || !BuildViews() || !FillSequences() || !FillMappings()) {
            return nullptr;
        }
        return Py_NewRef(built_[static_cast<std::size_t>(root)]);
    }

   private:
    [[nodiscard]] PyObject* Refuse(const char* why) noexcept {
        RaiseCloneError(isolate_, why);
        return nullptr;
    }

    [[nodiscard]] PyObject* Node(Py_ssize_t i) const noexcept { return PyList_GET_ITEM(nodes_, i); }
    [[nodiscard]] int TagOf(Py_ssize_t i) const noexcept {
        return static_cast<int>(PyLong_AsLong(PyTuple_GET_ITEM(Node(i), 0)));
    }

    /// An exact int naming a node.
    [[nodiscard]] bool IndexOf(PyObject* value, Py_ssize_t* out) const noexcept {
        if (!PyLong_CheckExact(value)) {
            return false;
        }
        const Py_ssize_t n = PyLong_AsSsize_t(value);
        if (n == -1 && PyErr_Occurred() != nullptr) {
            PyErr_Clear();
            return false;
        }
        if (n < 0 || n >= count_) {
            return false;
        }
        *out = n;
        return true;
    }

    /// A non-negative exact int that fits in Py_ssize_t.
    [[nodiscard]] static bool SizeOfField(PyObject* value, Py_ssize_t* out) noexcept {
        if (!PyLong_CheckExact(value)) {
            return false;
        }
        const Py_ssize_t n = PyLong_AsSsize_t(value);
        if (n == -1 && PyErr_Occurred() != nullptr) {
            PyErr_Clear();
            return false;
        }
        *out = n;
        return n >= 0;
    }

    /// An exact list of node indices, of even length if `pairs`.
    [[nodiscard]] bool IndexList(PyObject* value, bool pairs) const noexcept {
        if (!PyList_CheckExact(value) || (pairs && PyList_GET_SIZE(value) % 2 != 0)) {
            return false;
        }
        Py_ssize_t ignored = 0;
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(value); ++i) {
            if (!IndexOf(PyList_GET_ITEM(value, i), &ignored)) {
                return false;
            }
        }
        return true;
    }

    /// The bytes a buffer node holds, or -1 if node `i` is not a buffer.
    [[nodiscard]] Py_ssize_t BufferSize(Py_ssize_t i) const noexcept {
        const int tag = TagOf(i);
        if (tag != TAG_BYTES && tag != TAG_BYTEARRAY) {
            return -1;
        }
        return PyBytes_GET_SIZE(PyTuple_GET_ITEM(Node(i), 1));
    }

    [[nodiscard]] bool Validate() const noexcept {
        // First the shape of every node, so that the cross-checks after may
        // read any node's tag and fields without a second look.
        for (Py_ssize_t i = 0; i < count_; ++i) {
            PyObject* node = Node(i);
            if (!PyTuple_CheckExact(node) || PyTuple_GET_SIZE(node) < 1 ||
                !PyLong_CheckExact(PyTuple_GET_ITEM(node, 0))) {
                return false;
            }
            int overflow = 0;
            const long tag = PyLong_AsLongAndOverflow(PyTuple_GET_ITEM(node, 0), &overflow);
            if (overflow != 0 || tag < 0 || tag >= TAG_COUNT) {
                return false;
            }
            const Py_ssize_t size = PyTuple_GET_SIZE(node);
            const auto field = [node](Py_ssize_t k) { return PyTuple_GET_ITEM(node, k); };
            bool ok = false;
            switch (static_cast<Tag>(tag)) {
                case TAG_UNDEFINED:
                case TAG_NULL:
                    ok = size == 1;
                    break;
                case TAG_BOOL:
                    ok = size == 2 && PyBool_Check(field(1));
                    break;
                case TAG_INT:
                    ok = size == 2 && PyLong_CheckExact(field(1));
                    break;
                case TAG_FLOAT:
                    ok = size == 2 && PyFloat_CheckExact(field(1));
                    break;
                case TAG_STR:
                    ok = size == 2 && PyUnicode_CheckExact(field(1));
                    break;
                case TAG_BYTES:
                case TAG_BYTEARRAY:
                    ok = size == 2 && PyBytes_CheckExact(field(1));
                    break;
                case TAG_TYPED:
                case TAG_VIEW: {
                    const Py_ssize_t base = tag == TAG_TYPED ? 2 : 1;
                    Py_ssize_t ignored = 0;
                    ok = size == base + 3 && IndexOf(field(base), &ignored) && SizeOfField(field(base + 1), &ignored) &&
                         SizeOfField(field(base + 2), &ignored);
                    if (ok && tag == TAG_TYPED) {
                        Py_ssize_t type = 0;
                        ok = SizeOfField(field(1), &type) && type < ELEMENT_TYPE_COUNT;
                    }
                    break;
                }
                case TAG_LIST:
                case TAG_TUPLE:
                    ok = size == 2 && IndexList(field(1), false);
                    break;
                case TAG_DICT:
                case TAG_OBJECT:
                    ok = size == 2 && IndexList(field(1), true);
                    break;
                case TAG_COUNT:
                    break;
            }
            if (!ok) {
                return false;
            }
        }
        // Then what the nodes say about each other.
        for (Py_ssize_t i = 0; i < count_; ++i) {
            PyObject* node = Node(i);
            const int tag = TagOf(i);
            if (tag == TAG_TYPED || tag == TAG_VIEW) {
                const Py_ssize_t base = tag == TAG_TYPED ? 2 : 1;
                const Py_ssize_t buffer = PyLong_AsSsize_t(PyTuple_GET_ITEM(node, base));
                const Py_ssize_t offset = PyLong_AsSsize_t(PyTuple_GET_ITEM(node, base + 1));
                const Py_ssize_t length = PyLong_AsSsize_t(PyTuple_GET_ITEM(node, base + 2));
                const Py_ssize_t available = BufferSize(buffer);
                Py_ssize_t element = 1;
                if (tag == TAG_TYPED) {
                    element = SizeOf(static_cast<ElementType>(PyLong_AsLong(PyTuple_GET_ITEM(node, 1))));
                }
                if (available < 0 || offset % element != 0 || offset > available ||
                    length > (available - offset) / element) {
                    return false;
                }
            } else if (tag == TAG_OBJECT) {
                // A property key is a string or an index; nothing else is one.
                PyObject* items = PyTuple_GET_ITEM(node, 1);
                for (Py_ssize_t k = 0; k < PyList_GET_SIZE(items); k += 2) {
                    const int keyTag = TagOf(PyLong_AsSsize_t(PyList_GET_ITEM(items, k)));
                    if (keyTag != TAG_STR && keyTag != TAG_INT) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /// Whether tuples reach themselves only through something mutable. The
    /// writer can never produce a tuple that holds itself through tuples alone
    /// - Python cannot make one - and hashing one would recurse without end, in
    /// C, so a blob that describes one is refused before anything is built.
    [[nodiscard]] bool TuplesAcyclic() const {
        // 0 unvisited, 1 on the current path, 2 done. Iterative depth-first.
        std::vector<std::uint8_t> state(static_cast<std::size_t>(count_), 0);
        std::vector<std::pair<Py_ssize_t, Py_ssize_t>> stack;  // (node, next item)
        for (Py_ssize_t start = 0; start < count_; ++start) {
            if (TagOf(start) != TAG_TUPLE || state[static_cast<std::size_t>(start)] != 0) {
                continue;
            }
            stack.emplace_back(start, 0);
            state[static_cast<std::size_t>(start)] = 1;
            while (!stack.empty()) {
                auto& [node, next] = stack.back();
                PyObject* items = PyTuple_GET_ITEM(Node(node), 1);
                if (next >= PyList_GET_SIZE(items)) {
                    state[static_cast<std::size_t>(node)] = 2;
                    stack.pop_back();
                    continue;
                }
                const Py_ssize_t child = PyLong_AsSsize_t(PyList_GET_ITEM(items, next++));
                if (TagOf(child) != TAG_TUPLE) {
                    continue;
                }
                if (state[static_cast<std::size_t>(child)] == 1) {
                    return false;
                }
                if (state[static_cast<std::size_t>(child)] == 0) {
                    state[static_cast<std::size_t>(child)] = 1;
                    stack.emplace_back(child, 0);
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool BuildLeaves() {
        const Types& types = isolate_.impl().types;
        for (Py_ssize_t i = 0; i < count_; ++i) {
            PyObject* node = Node(i);
            PyObject* object = nullptr;
            switch (static_cast<Tag>(TagOf(i))) {
                case TAG_UNDEFINED:
                    object = Py_NewRef(Py_None);
                    break;
                case TAG_NULL:
                    object = Py_NewRef(types.nullValue);
                    break;
                case TAG_BOOL:
                case TAG_INT:
                case TAG_FLOAT:
                case TAG_STR:
                case TAG_BYTES:
                    // Immutable, and made by marshal in this interpreter: the
                    // object itself is the value.
                    object = Py_NewRef(PyTuple_GET_ITEM(node, 1));
                    break;
                case TAG_BYTEARRAY:
                    object = PyByteArray_FromObject(PyTuple_GET_ITEM(node, 1));
                    break;
                case TAG_LIST:
                    object = PyList_New(0);
                    break;
                case TAG_TUPLE: {
                    // Filled with None rather than left null, so that nothing
                    // that sees it before pass 3 - the collector's traversal,
                    // say - sees a hole.
                    const Py_ssize_t size = PyList_GET_SIZE(PyTuple_GET_ITEM(node, 1));
                    object = PyTuple_New(size);
                    for (Py_ssize_t k = 0; object != nullptr && k < size; ++k) {
                        PyTuple_SET_ITEM(object, k, Py_NewRef(Py_None));
                    }
                    break;
                }
                case TAG_DICT:
                    object = PyDict_New();
                    break;
                case TAG_OBJECT: {
                    const HandleScope scope(isolate_);
                    const std::optional<Slot> made = MakeObject(context_);
                    if (made) {
                        object = Py_NewRef(Resolve(*made));
                    } else if (PyErr_Occurred() == nullptr) {
                        RaiseCloneError(isolate_, "an object could not be made");
                    }
                    break;
                }
                case TAG_TYPED:
                case TAG_VIEW:
                    continue;  // pass 2
                case TAG_COUNT:
                    break;
            }
            if (object == nullptr) {
                return false;
            }
            built_[static_cast<std::size_t>(i)] = object;
        }
        return true;
    }

    [[nodiscard]] bool BuildViews() {
        const Types& types = isolate_.impl().types;
        for (Py_ssize_t i = 0; i < count_; ++i) {
            PyObject* node = Node(i);
            const int tag = TagOf(i);
            if (tag != TAG_TYPED && tag != TAG_VIEW) {
                continue;
            }
            const Py_ssize_t base = tag == TAG_TYPED ? 2 : 1;
            PyObject* buffer = built_[static_cast<std::size_t>(PyLong_AsSsize_t(PyTuple_GET_ITEM(node, base)))];
            const Py_ssize_t offset = PyLong_AsSsize_t(PyTuple_GET_ITEM(node, base + 1));
            const Py_ssize_t length = PyLong_AsSsize_t(PyTuple_GET_ITEM(node, base + 2));
            PyObject* view = nullptr;
            if (tag == TAG_TYPED) {
                if (types.typedArray == nullptr) {
                    (void)Refuse("TypedArray is not available");
                    return false;
                }
                const auto element = static_cast<ElementType>(PyLong_AsLong(PyTuple_GET_ITEM(node, 1)));
                view = NewTypedArrayObject(types.typedArray, element, buffer, offset, length);
            } else {
                if (types.dataView == nullptr) {
                    (void)Refuse("DataView is not available");
                    return false;
                }
                view = NewDataViewObject(types.dataView, buffer, offset, length);
            }
            if (view == nullptr) {
                return false;
            }
            built_[static_cast<std::size_t>(i)] = view;
        }
        return true;
    }

    [[nodiscard]] PyObject* Built(PyObject* index) const noexcept {
        return built_[static_cast<std::size_t>(PyLong_AsSsize_t(index))];
    }

    [[nodiscard]] bool FillSequences() {
        for (Py_ssize_t i = 0; i < count_; ++i) {
            const int tag = TagOf(i);
            if (tag != TAG_LIST && tag != TAG_TUPLE) {
                continue;
            }
            PyObject* items = PyTuple_GET_ITEM(Node(i), 1);
            PyObject* target = built_[static_cast<std::size_t>(i)];
            for (Py_ssize_t k = 0; k < PyList_GET_SIZE(items); ++k) {
                PyObject* item = Built(PyList_GET_ITEM(items, k));
                if (tag == TAG_LIST) {
                    if (PyList_Append(target, item) != 0) {
                        return false;
                    }
                } else {
                    // A tuple this pass made and nothing has hashed: filling it
                    // in place is what CPython's own builders do.
                    PyObject* old = PyTuple_GET_ITEM(target, k);
                    PyTuple_SET_ITEM(target, k, Py_NewRef(item));
                    Py_DECREF(old);
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool FillMappings() {
        for (Py_ssize_t i = 0; i < count_; ++i) {
            const int tag = TagOf(i);
            if (tag != TAG_DICT && tag != TAG_OBJECT) {
                continue;
            }
            PyObject* items = PyTuple_GET_ITEM(Node(i), 1);
            PyObject* target = built_[static_cast<std::size_t>(i)];
            for (Py_ssize_t k = 0; k < PyList_GET_SIZE(items); k += 2) {
                PyObject* key = Built(PyList_GET_ITEM(items, k));
                PyObject* value = Built(PyList_GET_ITEM(items, k + 1));
                if (tag == TAG_DICT) {
                    if (PyDict_SetItem(target, key, value) != 0) {
                        return false;
                    }
                    continue;
                }
                // Through the objects area, which owns what a property is.
                const HandleScope scope(isolate_);
                const Slot objectSlot = PushBorrowed(isolate_, target);
                const Slot keySlot = PushBorrowed(isolate_, key);
                const Slot valueSlot = PushBorrowed(isolate_, value);
                if (objectSlot.IsEmpty() || keySlot.IsEmpty() || valueSlot.IsEmpty()) {
                    return false;
                }
                const std::optional<bool> set = SetProperty(context_, objectSlot, keySlot, valueSlot);
                if (!set || !*set) {
                    if (PyErr_Occurred() == nullptr) {
                        RaiseCloneError(isolate_, "a property could not be set");
                    }
                    return false;
                }
            }
        }
        return true;
    }

    const Context& context_;
    Isolate& isolate_;
    PyObject* payload_ = nullptr;  ///< owned: marshal's result
    PyObject* nodes_ = nullptr;    ///< borrowed from payload_
    Py_ssize_t count_ = 0;
    std::vector<PyObject*> built_;  ///< owned
};

}  // namespace

// --- structured clone: the contract -------------------------------------------------

std::optional<std::vector<std::uint8_t>> SerializeValue(const Context& context, Slot value) {
    Isolate& isolate = OwnerOf(context);
    // A unibind.Object's getters run during the walk; that is script.
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    PyObject* payload = nullptr;
    {
        Encoder encoder(context, isolate);
        payload = encoder.Encode(Resolve(value));
    }
    if (payload == nullptr) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> blob;
    try {
        blob = FrameBlob(payload, CLONE_MAGIC, CLONE_FORMAT, CLONE_MARSHAL_VERSION, 0);
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
    }
    Py_DECREF(payload);
    return blob;
}

std::optional<Slot> DeserializeValue(const Context& context, std::span<const std::uint8_t> blob) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> payload =
        UnframeBlob(blob, CLONE_MAGIC, CLONE_FORMAT, CLONE_MARSHAL_VERSION, 0);
    if (payload.empty()) {
        RaiseCloneError(isolate, "the blob is not a value this engine build serialized, or it is damaged");
        return std::nullopt;
    }
    PyObject* result = nullptr;
    {
        Decoder decoder(context, isolate);
        result = decoder.Decode(payload);
    }
    return PushOrNothing(isolate, result);
}

// --- code cache -------------------------------------------------------------------

namespace {

/// A resource name as `CompileSource` gives it to the compiler: up to its
/// first NUL (unibind/types.h), decoded lossily. New reference.
[[nodiscard]] PyObject* ResourceName(const ScriptOrigin& origin) noexcept {
    std::string_view resource = origin.resourceName;
    if (const std::size_t nul = resource.find('\0'); nul != std::string_view::npos) {
        resource = resource.substr(0, nul);
    }
    return TextString(resource);
}

/// Put `source` in `linecache` under `filename`, exactly as module.cpp's
/// `compile_script` does - a script compiled from a cache blob never went
/// through that, and without it a traceback could name a line but not quote
/// it. Best effort: a failure here costs a quoted line, not the compile.
void RegisterSourceLines(PyObject* filename, std::string_view source, int lineOffset) noexcept {
    PyObject* text = PyUnicode_DecodeUTF8(source.data(), static_cast<Py_ssize_t>(source.size()), "replace");
    PyObject* lines = text != nullptr ? PyUnicode_Splitlines(text, 1) : nullptr;
    if (lines != nullptr && lineOffset > 0) {
        PyObject* padding = PyList_New(0);
        PyObject* newline = PyUnicode_FromString("\n");
        for (int i = 0; padding != nullptr && newline != nullptr && i < lineOffset; ++i) {
            if (PyList_Append(padding, newline) != 0) {
                Py_CLEAR(padding);
            }
        }
        Py_XDECREF(newline);
        PyObject* joined = padding != nullptr ? PySequence_InPlaceConcat(padding, lines) : nullptr;
        Py_XDECREF(padding);
        Py_SETREF(lines, joined);
    }
    PyObject* linecache = lines != nullptr ? PyImport_ImportModule("linecache") : nullptr;
    PyObject* cache = linecache != nullptr ? PyObject_GetAttrString(linecache, "cache") : nullptr;
    PyObject* entry =
        cache != nullptr ? Py_BuildValue("(nOOO)", PyUnicode_GET_LENGTH(text), Py_None, lines, filename) : nullptr;
    if (entry == nullptr || PyObject_SetItem(cache, filename, entry) != 0) {
        PyErr_Clear();
    }
    Py_XDECREF(entry);
    Py_XDECREF(cache);
    Py_XDECREF(linecache);
    Py_XDECREF(lines);
    Py_XDECREF(text);
}

/// The code pair a blob holds, if it holds one for this script: a tuple of a
/// code object and a code object or None, both compiled under this script's
/// name. False - with nothing pending - for anything else.
[[nodiscard]] bool LoadCodePair(std::span<const std::uint8_t> blob, PyObject* filename, PyObject** body,
                                PyObject** tail) noexcept {
    const std::span<const std::uint8_t> payload =
        UnframeBlob(blob, CODE_MAGIC, CODE_FORMAT, Py_MARSHAL_VERSION, BytecodeMagic());
    if (payload.empty()) {
        return false;
    }
    PyObject* pair = PyMarshal_ReadObjectFromString(reinterpret_cast<const char*>(payload.data()),
                                                    static_cast<Py_ssize_t>(payload.size()));
    if (pair == nullptr) {
        PyErr_Clear();
        return false;
    }
    const auto named = [filename](PyObject* code) {
        const int same = PyObject_RichCompareBool(reinterpret_cast<PyCodeObject*>(code)->co_filename, filename, Py_EQ);
        if (same < 0) {
            PyErr_Clear();
        }
        return same == 1;
    };
    // The script.h frame keys the blob to its source already; checking the
    // shape - and the name the code was compiled under - here as well costs
    // nothing and keeps this function honest when it is called any other way.
    const bool ok = PyTuple_CheckExact(pair) && PyTuple_GET_SIZE(pair) == 2 &&
                    PyCode_Check(PyTuple_GET_ITEM(pair, 0)) && named(PyTuple_GET_ITEM(pair, 0)) &&
                    (PyTuple_GET_ITEM(pair, 1) == Py_None ||
                     (PyCode_Check(PyTuple_GET_ITEM(pair, 1)) && named(PyTuple_GET_ITEM(pair, 1))));
    if (!ok) {
        Py_DECREF(pair);
        return false;
    }
    *body = Py_NewRef(PyTuple_GET_ITEM(pair, 0));
    PyObject* second = PyTuple_GET_ITEM(pair, 1);
    *tail = second == Py_None ? nullptr : Py_NewRef(second);
    Py_DECREF(pair);
    return true;
}

}  // namespace

ScriptRec* CompileScriptWithCache(const Context& context, std::string_view source, const ScriptOrigin& origin,
                                  std::span<const std::uint8_t> codeCache, CompileOptions /*options*/) {
    // `options` has nothing to choose between: CPython compiles a module in
    // full or not at all, so every compile here is the eager one.
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return nullptr;
    }
    if (!codeCache.empty()) {
        PyObject* filename = ResourceName(origin);
        PyObject* body = nullptr;
        PyObject* tail = nullptr;
        if (filename == nullptr) {
            PyErr_Clear();
        } else if (LoadCodePair(codeCache, filename, &body, &tail)) {
            RegisterSourceLines(filename, source, origin.lineOffset);
            Py_DECREF(filename);
            ScriptRec* rec = NewScriptRec(isolate, body, tail, origin.resourceName);
            if (rec == nullptr) {
                PyErr_NoMemory();
                return nullptr;
            }
            rec->usedCache = true;
            return rec;
        }
        Py_XDECREF(filename);
    }
    // No blob, or one refused: compile the source as `CompileScript` does.
    PyObject* body = nullptr;
    PyObject* tail = nullptr;
    if (!CompileSource(isolate, source, origin, &body, &tail)) {
        return nullptr;
    }
    return NewScriptRec(isolate, body, tail, origin.resourceName);
}

bool ScriptUsedCodeCache(const ScriptRec* script) noexcept {
    return script != nullptr && script->usedCache;
}

std::optional<std::vector<std::uint8_t>> ScriptCreateCodeCache(const ScriptRec* script) {
    if (script == nullptr || script->body == nullptr) {
        return std::nullopt;
    }
    PyObject* pair = PyTuple_Pack(2, script->body, script->tail != nullptr ? script->tail : Py_None);
    PyObject* payload = pair != nullptr ? PyMarshal_WriteObjectToString(pair, Py_MARSHAL_VERSION) : nullptr;
    Py_XDECREF(pair);
    if (payload == nullptr) {
        // Nothing here is script, and nobody is waiting for an exception: an
        // empty answer is the whole report.
        PyErr_Clear();
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> blob =
        FrameBlob(payload, CODE_MAGIC, CODE_FORMAT, Py_MARSHAL_VERSION, BytecodeMagic());
    Py_DECREF(payload);
    return blob;
}

// --- the area hook ------------------------------------------------------------------

bool InitDataTypes(Isolate& isolate, PyObject* module) noexcept {
    Isolate::Impl& impl = isolate.impl();
    Types& types = impl.types;
    if (!impl.data) {
        impl.data.reset(new (std::nothrow) DataState());
        if (!impl.data) {
            PyErr_NoMemory();
            return false;
        }
    }
    types.typedArray = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &typedArraySpec, nullptr));
    types.dataView = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &dataViewSpec, nullptr));
    if (types.typedArray == nullptr || types.dataView == nullptr) {
        return false;
    }
    // JavaScript's DataCloneError, as a unibind.Error: what a clone that meets
    // a function - or a blob that is not a clone - raises.
    types.dataCloneError = PyErr_NewExceptionWithDoc(
        "unibind.DataCloneError", "A value that structured clone cannot copy, or a blob it cannot read.",
        types.error != nullptr ? types.error : PyExc_Exception, nullptr);
    if (types.dataCloneError == nullptr) {
        return false;
    }
    return PyModule_AddObjectRef(module, "TypedArray", reinterpret_cast<PyObject*>(types.typedArray)) == 0 &&
           PyModule_AddObjectRef(module, "DataView", reinterpret_cast<PyObject*>(types.dataView)) == 0 &&
           PyModule_AddObjectRef(module, "DataCloneError", types.dataCloneError) == 0;
}

}  // namespace ub::detail
