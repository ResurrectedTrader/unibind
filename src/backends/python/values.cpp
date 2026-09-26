// Values: what a Python object is in `ub::` terms, the primitives, equality,
// conversions, strings, symbols, externals and errors.
//
// The mapping, which docs/python.md argues for at length:
//
//   undefined  None                 Number   int (|n| <= 2^53) or float
//   null       unibind.null         BigInt   int beyond 2^53
//   Boolean    bool                 String   str
//   Symbol     unibind.Symbol       Array    list (tuple, read-only)
//   Function   anything callable    Object   everything else
//   External   unibind.External

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "internal.h"

namespace ub::detail {

namespace {

constexpr double MAX_SAFE_INTEGER = 9007199254740992.0;  // 2^53

[[nodiscard]] bool IsType(PyObject* value, PyTypeObject* type) noexcept {
    return type != nullptr && PyObject_TypeCheck(value, type);
}

/// An `int` that is not a `bool`.
[[nodiscard]] bool IsPlainInt(PyObject* value) noexcept {
    return PyLong_Check(value) && !PyBool_Check(value);
}

/// Whether an int is beyond the range a JavaScript Number holds exactly,
/// which is what makes it a BigInt here.
[[nodiscard]] bool IsBigInt(PyObject* value) noexcept {
    if (!IsPlainInt(value)) {
        return false;
    }
    int overflow = 0;
    const long long n = PyLong_AsLongLongAndOverflow(value, &overflow);
    if (overflow != 0) {
        return true;
    }
    return n > 9007199254740992LL || n < -9007199254740992LL;
}

[[nodiscard]] bool IsNumber(PyObject* value) noexcept {
    return PyFloat_Check(value) || (IsPlainInt(value) && !IsBigInt(value));
}

[[nodiscard]] bool IsPrimitive(Isolate::Impl& state, PyObject* value) noexcept {
    return value == Py_None || value == state.types.nullValue || PyBool_Check(value) || PyLong_Check(value) ||
           PyFloat_Check(value) || PyUnicode_Check(value) || IsType(value, state.types.symbol);
}

[[nodiscard]] double AsDouble(PyObject* number) noexcept {
    if (PyFloat_Check(number)) {
        return PyFloat_AS_DOUBLE(number);
    }
    const double value = PyLong_AsDouble(number);
    if (value == -1.0 && PyErr_Occurred() != nullptr) {
        // Too big for a double: that is what a double does with it.
        PyErr_Clear();
        int sign = 0;
        (void)PyLong_GetSign(number, &sign);  // cannot fail: an int
        return sign < 0 ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    return value;
}

/// ToInt32 of an int of any size: its low 32 bits, as two's complement.
[[nodiscard]] std::int32_t IntToInt32(PyObject* value) noexcept {
    int overflow = 0;
    const long long n = PyLong_AsLongLongAndOverflow(value, &overflow);
    if (overflow == 0) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<unsigned long long>(n)));
    }
    PyObject* mask = PyLong_FromUnsignedLong(0xFFFFFFFFUL);
    PyObject* low = mask != nullptr ? PyNumber_And(value, mask) : nullptr;
    Py_XDECREF(mask);
    if (low == nullptr) {
        PyErr_Clear();
        return 0;
    }
    const unsigned long bits = PyLong_AsUnsignedLong(low);
    Py_DECREF(low);
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
}

void AppendUtf8(std::string& out, Py_UCS4 c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

}  // namespace

std::string Utf8Of(PyObject* string) {
    if (string == nullptr || !PyUnicode_Check(string)) {
        return {};
    }
    Py_ssize_t size = 0;
    if (const char* utf8 = PyUnicode_AsUTF8AndSize(string, &size); utf8 != nullptr) {
        return {utf8, static_cast<std::size_t>(size)};
    }
    // A lone surrogate, which a str may hold and UTF-8 may not. Write it as
    // U+FFFD, as V8 writes an unpaired surrogate out of a JavaScript string.
    PyErr_Clear();
    std::string out;
    const Py_ssize_t length = PyUnicode_GET_LENGTH(string);
    const int kind = PyUnicode_KIND(string);
    const void* data = PyUnicode_DATA(string);
    for (Py_ssize_t i = 0; i < length; ++i) {
        const Py_UCS4 c = PyUnicode_READ(kind, data, i);
        AppendUtf8(out, (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : c);
    }
    return out;
}

PyObject* TextString(std::string_view utf8) noexcept {
    return PyUnicode_DecodeUTF8(utf8.data(), static_cast<Py_ssize_t>(utf8.size()), "replace");
}

bool IsSymbol(Isolate& isolate, PyObject* value) noexcept {
    return IsType(value, isolate.impl().types.symbol);
}

// --- inspection ------------------------------------------------------------------

ValueKind KindOf(Slot slot) noexcept {
    PyObject* value = Resolve(slot);
    Isolate::Impl& state = IsolateFor(slot).impl();
    if (value == Py_None) {
        return ValueKind::Undefined;
    }
    if (value == state.types.nullValue) {
        return ValueKind::Null;
    }
    if (PyBool_Check(value)) {
        return ValueKind::Boolean;
    }
    if (PyLong_Check(value)) {
        return IsBigInt(value) ? ValueKind::BigInt : ValueKind::Number;
    }
    if (PyFloat_Check(value)) {
        return ValueKind::Number;
    }
    if (PyUnicode_Check(value)) {
        return ValueKind::String;
    }
    if (IsType(value, state.types.symbol)) {
        return ValueKind::Symbol;
    }
    if (IsType(value, state.types.external)) {
        return ValueKind::External;
    }
    if (PyList_Check(value) || PyTuple_Check(value)) {
        return ValueKind::Array;
    }
    if (PyCallable_Check(value) != 0) {
        return ValueKind::Function;
    }
    return ValueKind::Object;
}

bool IsType(Slot slot, TypeCode type) noexcept {
    PyObject* value = Resolve(slot);
    Isolate::Impl& state = IsolateFor(slot).impl();
    const Types& types = state.types;
    switch (type) {
        case TypeCode::Value:
            return true;
        case TypeCode::Primitive:
            return IsPrimitive(state, value);
        case TypeCode::Boolean:
            return PyBool_Check(value);
        case TypeCode::Number:
            return IsNumber(value);
        case TypeCode::Integer: {
            if (IsPlainInt(value)) {
                int overflow = 0;
                const long long n = PyLong_AsLongLongAndOverflow(value, &overflow);
                return overflow == 0 && n >= INT32_MIN && n <= INT32_MAX;
            }
            if (PyFloat_Check(value)) {
                const double d = PyFloat_AS_DOUBLE(value);
                return d >= INT32_MIN && d <= INT32_MAX && std::trunc(d) == d && !(d == 0 && std::signbit(d));
            }
            return false;
        }
        case TypeCode::Name:
            return PyUnicode_Check(value) || IsType(value, types.symbol);
        case TypeCode::String:
            return PyUnicode_Check(value);
        case TypeCode::Symbol:
            return IsType(value, types.symbol);
        case TypeCode::BigInt:
            return IsBigInt(value);
        case TypeCode::Object:
            return !IsPrimitive(state, value) && !IsType(value, types.external);
        case TypeCode::Array:
            return PyList_Check(value) || PyTuple_Check(value);
        case TypeCode::Function:
            return !IsPrimitive(state, value) && !IsType(value, types.external) && PyCallable_Check(value) != 0;
        case TypeCode::ArrayBuffer:
            return PyByteArray_Check(value) || PyBytes_Check(value);
        case TypeCode::ArrayBufferView:
            return IsType(value, types.typedArray) || IsType(value, types.dataView);
        case TypeCode::TypedArray:
            return IsType(value, types.typedArray);
        case TypeCode::DataView:
            return IsType(value, types.dataView);
        case TypeCode::Promise:
            return IsType(value, types.promise);
        case TypeCode::External:
            return IsType(value, types.external);
    }
    return false;
}

bool StrictEqualsObjects(Isolate& isolate, PyObject* lhs, PyObject* rhs) noexcept {
    const bool lhsNumber = PyFloat_Check(lhs) || IsPlainInt(lhs);
    const bool rhsNumber = PyFloat_Check(rhs) || IsPlainInt(rhs);
    if (lhsNumber && rhsNumber) {
        if (IsPlainInt(lhs) && IsPlainInt(rhs)) {
            // Exact, at any size - and ints run no user code to compare.
            const int equal = PyObject_RichCompareBool(lhs, rhs, Py_EQ);
            return equal == 1;
        }
        return AsDouble(lhs) == AsDouble(rhs);  // NaN is not itself; +0 is -0
    }
    if (lhsNumber != rhsNumber) {
        return false;
    }
    if (PyUnicode_Check(lhs) && PyUnicode_Check(rhs)) {
        return PyUnicode_Compare(lhs, rhs) == 0;
    }
    (void)isolate;
    // Everything else is an object or a singleton: identity.
    return lhs == rhs;
}

bool SameValueObjects(Isolate& isolate, PyObject* lhs, PyObject* rhs) noexcept {
    if (PyFloat_Check(lhs) || PyFloat_Check(rhs)) {
        if (!(PyFloat_Check(lhs) || IsPlainInt(lhs)) || !(PyFloat_Check(rhs) || IsPlainInt(rhs))) {
            return false;
        }
        const double a = AsDouble(lhs);
        const double b = AsDouble(rhs);
        if (std::isnan(a) && std::isnan(b)) {
            return true;
        }
        return a == b && std::signbit(a) == std::signbit(b);
    }
    return StrictEqualsObjects(isolate, lhs, rhs);
}

bool StrictEquals(Slot lhs, Slot rhs) noexcept {
    return StrictEqualsObjects(IsolateFor(lhs), Resolve(lhs), Resolve(rhs));
}

bool SameValue(Slot lhs, Slot rhs) noexcept {
    return SameValueObjects(IsolateFor(lhs), Resolve(lhs), Resolve(rhs));
}

std::optional<bool> LooseEquals(const Context& context, Slot lhs, Slot rhs) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    PyObject* a = Resolve(lhs);
    PyObject* b = Resolve(rhs);
    // JavaScript's one cross-kind rule worth keeping: null == undefined.
    const auto nullish = [&isolate](PyObject* v) { return v == Py_None || IsNull(isolate, v); };
    if (nullish(a) || nullish(b)) {
        return nullish(a) && nullish(b);
    }
    const int equal = PyObject_RichCompareBool(a, b, Py_EQ);
    if (equal < 0) {
        return std::nullopt;
    }
    return equal == 1;
}

// --- reading primitives ------------------------------------------------------------

bool BooleanValue(Slot value) noexcept {
    return Resolve(value) == Py_True;
}

double NumberValue(Slot value) noexcept {
    PyObject* number = Resolve(value);
    if (PyFloat_Check(number) || PyLong_Check(number)) {
        return AsDouble(number);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::int32_t Int32Value(Slot value) noexcept {
    PyObject* number = Resolve(value);
    if (PyLong_Check(number)) {
        return IntToInt32(number);
    }
    if (PyFloat_Check(number)) {
        return NumberToInt32(PyFloat_AS_DOUBLE(number));
    }
    return 0;
}

std::size_t Utf8Length(Slot string) noexcept {
    try {
        return Utf8Of(Resolve(string)).size();
    } catch (const std::bad_alloc&) {
        return 0;
    }
}

std::size_t WriteUtf8(Slot string, std::span<char> out) noexcept {
    std::string text;
    try {
        text = Utf8Of(Resolve(string));
    } catch (const std::bad_alloc&) {
        return 0;
    }
    std::size_t n = std::min(text.size(), out.size());
    // Truncate at a code point boundary: never end on a continuation byte's
    // lead without its tail.
    if (n < text.size()) {
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) {
            --n;
        }
    }
    std::memcpy(out.data(), text.data(), n);
    return n;
}

std::string ToStdString(Slot string) {
    return Utf8Of(Resolve(string));
}

std::optional<std::string> SymbolDescription(Slot symbol) {
    auto* object = reinterpret_cast<SymbolObject*>(Resolve(symbol));
    if (object->description == nullptr || object->description == Py_None) {
        return std::nullopt;
    }
    return Utf8Of(object->description);
}

// --- conversion ------------------------------------------------------------------

std::optional<bool> ToBoolean(const Context& context, Slot value) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    PyObject* object = Resolve(value);
    if (IsNull(isolate, object)) {
        return false;
    }
    // Python's truth, not JavaScript's: an empty list is false here.
    const int truth = PyObject_IsTrue(object);
    if (truth < 0) {
        return std::nullopt;
    }
    return truth == 1;
}

namespace {

/// ToNumber, in Python's terms where they exist and JavaScript's where Python
/// has none: None is NaN, null is 0, a string is parsed (and NaN when it does
/// not parse), and anything with `__float__` or `__index__` is asked.
std::optional<double> NumberOf(Isolate& isolate, PyObject* object) {
    if (object == Py_None) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (IsNull(isolate, object)) {
        return 0.0;
    }
    if (PyBool_Check(object)) {
        return object == Py_True ? 1.0 : 0.0;
    }
    if (PyLong_Check(object) || PyFloat_Check(object)) {
        return AsDouble(object);
    }
    if (IsSymbol(isolate, object)) {
        PyErr_SetString(PyExc_TypeError, "cannot convert a Symbol to a number");
        return std::nullopt;
    }
    if (PyUnicode_Check(object)) {
        PyObject* stripped = PyObject_CallMethod(object, "strip", nullptr);
        if (stripped == nullptr) {
            return std::nullopt;
        }
        std::optional<double> result;
        if (PyUnicode_GET_LENGTH(stripped) == 0) {
            result = 0.0;
        } else if (PyObject* parsed = PyFloat_FromString(stripped); parsed != nullptr) {
            result = PyFloat_AS_DOUBLE(parsed);
            Py_DECREF(parsed);
        } else {
            PyErr_Clear();
            // `0x10`, `0o17`, `0b11` - the prefixed forms JavaScript accepts.
            PyObject* asInt = PyLong_FromUnicodeObject(stripped, 0);
            if (asInt != nullptr) {
                result = AsDouble(asInt);
                Py_DECREF(asInt);
            } else {
                PyErr_Clear();
                result = std::numeric_limits<double>::quiet_NaN();
            }
        }
        Py_DECREF(stripped);
        return result;
    }
    PyObject* converted = PyNumber_Float(object);
    if (converted == nullptr) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) != 0) {
            PyErr_Clear();
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::nullopt;
    }
    const double result = PyFloat_AS_DOUBLE(converted);
    Py_DECREF(converted);
    return result;
}

}  // namespace

std::optional<double> ToNumber(const Context& context, Slot value) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    return NumberOf(isolate, Resolve(value));
}

std::optional<std::int32_t> ToInt32(const Context& context, Slot value) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    PyObject* object = Resolve(value);
    if (IsPlainInt(object)) {
        return IntToInt32(object);
    }
    const std::optional<double> number = NumberOf(isolate, object);
    if (!number) {
        return std::nullopt;
    }
    return NumberToInt32(*number);
}

std::optional<std::uint32_t> ToUint32(const Context& context, Slot value) {
    const std::optional<std::int32_t> bits = ToInt32(context, value);
    if (!bits) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*bits);
}

std::optional<Slot> ToJsString(const Context& context, Slot value) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    PyObject* object = Resolve(value);
    if (PyUnicode_Check(object)) {
        return PushBorrowed(isolate, object);
    }
    return PushOrNothing(isolate, PyObject_Str(object));
}

std::optional<Slot> ToJsObject(const Context& context, Slot value) {
    Isolate& isolate = OwnerOf(context);
    const ScriptGate gate(isolate);
    if (!gate.Open()) {
        return std::nullopt;
    }
    PyObject* object = Resolve(value);
    if (object == Py_None || IsNull(isolate, object)) {
        PyErr_SetString(PyExc_TypeError, "cannot convert None or null to an object");
        return std::nullopt;
    }
    // Everything in Python already is one.
    return PushBorrowed(isolate, object);
}

// --- making values ---------------------------------------------------------------

Slot MakeUndefined(Isolate& isolate) noexcept {
    return PushBorrowed(isolate, Py_None);
}

Slot MakeNull(Isolate& isolate) noexcept {
    return PushBorrowed(isolate, isolate.impl().types.nullValue);
}

Slot MakeBoolean(Isolate& isolate, bool value) noexcept {
    return PushBorrowed(isolate, value ? Py_True : Py_False);
}

Slot MakeNumber(Isolate& isolate, double value) noexcept {
    // A Number with an integral value is an int here, so that script can
    // index, slice and `range` with it; everything else is a float. -0 stays
    // a float, because an int cannot hold its sign.
    if (std::isfinite(value) && std::trunc(value) == value && std::fabs(value) <= MAX_SAFE_INTEGER &&
        !(value == 0 && std::signbit(value))) {
        return Push(isolate, PyLong_FromDouble(value));
    }
    return Push(isolate, PyFloat_FromDouble(value));
}

Slot MakeInteger(Isolate& isolate, std::int32_t value) noexcept {
    return Push(isolate, PyLong_FromLong(value));
}

Slot MakeUnsigned(Isolate& isolate, std::uint32_t value) noexcept {
    return Push(isolate, PyLong_FromUnsignedLong(value));
}

std::optional<Slot> MakeString(Isolate& isolate, std::string_view utf8) {
    PyObject* string = PyUnicode_DecodeUTF8(utf8.data(), static_cast<Py_ssize_t>(utf8.size()), "strict");
    if (string == nullptr) {
        // Not UTF-8: no string, and no exception either - `String::New`
        // answers empty, and `NewFromUtf8` is the lossy form.
        if (PyErr_ExceptionMatches(PyExc_UnicodeDecodeError) != 0) {
            PyErr_Clear();
        }
        return std::nullopt;
    }
    return PushOrNothing(isolate, string);
}

namespace {

[[nodiscard]] PyObject* NewSymbol(Isolate& isolate, PyObject* description) noexcept {
    PyTypeObject* type = isolate.impl().types.symbol;
    PyObject* args = description == nullptr ? PyTuple_New(0) : PyTuple_Pack(1, description);
    if (args == nullptr) {
        return nullptr;
    }
    PyObject* symbol = PyObject_Call(reinterpret_cast<PyObject*>(type), args, nullptr);
    Py_DECREF(args);
    return symbol;
}

}  // namespace

std::optional<Slot> MakeSymbol(Isolate& isolate, std::optional<std::string_view> description) {
    PyObject* text = nullptr;
    if (description) {
        text = TextString(*description);
        if (text == nullptr) {
            return std::nullopt;
        }
    }
    PyObject* symbol = NewSymbol(isolate, text);
    Py_XDECREF(text);
    return PushOrNothing(isolate, symbol);
}

std::optional<Slot> MakeSymbolFor(Isolate& isolate, std::string_view key) {
    PyObject* registry = isolate.impl().types.symbolRegistry;
    PyObject* text = TextString(key);
    if (text == nullptr) {
        return std::nullopt;
    }
    PyObject* existing = PyDict_GetItemWithError(registry, text);  // borrowed
    if (existing != nullptr) {
        Py_DECREF(text);
        return PushBorrowed(isolate, existing);
    }
    if (PyErr_Occurred() != nullptr) {
        Py_DECREF(text);
        return std::nullopt;
    }
    PyObject* symbol = NewSymbol(isolate, text);
    if (symbol == nullptr || PyDict_SetItem(registry, text, symbol) != 0) {
        Py_DECREF(text);
        Py_XDECREF(symbol);
        return std::nullopt;
    }
    Py_DECREF(text);
    return PushOrNothing(isolate, symbol);
}

std::optional<Slot> GetWellKnownSymbol(Isolate& isolate, WellKnownSymbol which) {
    PyObject* symbol = isolate.impl().types.wellKnown[static_cast<int>(which)];
    if (symbol == nullptr) {
        return std::nullopt;
    }
    return PushBorrowed(isolate, symbol);
}

std::optional<Slot> MakeExternal(Isolate& isolate, CallbackData data) {
    PyTypeObject* type = isolate.impl().types.external;
    auto* external = PyObject_New(ExternalObject, type);
    if (external == nullptr) {
        return std::nullopt;
    }
    external->data = data;
    return PushOrNothing(isolate, reinterpret_cast<PyObject*>(external));
}

CallbackData ExternalData(Slot external) noexcept {
    return reinterpret_cast<ExternalObject*>(Resolve(external))->data;
}

PyObject* NormalizeKey(Isolate& isolate, PyObject* key) noexcept {
    if (PyUnicode_Check(key)) {
        // A canonical array index - "0", "17", never "01" or "-1" - is the
        // integer it spells, so `o["3"]` and `o[3]` are one property, as in
        // JavaScript.
        const Py_ssize_t length = PyUnicode_GET_LENGTH(key);
        if (length > 0 && length <= 10) {
            const int kind = PyUnicode_KIND(key);
            const void* data = PyUnicode_DATA(key);
            bool digits = true;
            unsigned long long n = 0;
            for (Py_ssize_t i = 0; i < length && digits; ++i) {
                const Py_UCS4 c = PyUnicode_READ(kind, data, i);
                digits = c >= '0' && c <= '9';
                n = n * 10 + (c - '0');
            }
            const bool canonical = digits && (length == 1 || PyUnicode_READ(kind, data, 0) != '0');
            if (canonical && n < 4294967295ULL) {
                return PyLong_FromUnsignedLongLong(n);
            }
        }
        return Py_NewRef(key);
    }
    if (IsPlainInt(key)) {
        int overflow = 0;
        const long long n = PyLong_AsLongLongAndOverflow(key, &overflow);
        if (overflow == 0 && n >= 0 && n < 4294967295LL) {
            return Py_NewRef(key);
        }
        return PyObject_Str(key);
    }
    if (IsSymbol(isolate, key)) {
        auto* symbol = reinterpret_cast<SymbolObject*>(key);
        return Py_NewRef(symbol->dunder != nullptr ? symbol->dunder : key);
    }
    return PyObject_Str(key);
}

// --- errors ------------------------------------------------------------------------

PyObject* ErrorClass(Isolate& isolate, ErrorKind kind) noexcept {
    const Types& types = isolate.impl().types;
    switch (kind) {
        case ErrorKind::Error:
            return types.error;
        case ErrorKind::TypeError:
            return PyExc_TypeError;
        case ErrorKind::RangeError:
            return types.rangeError;
        case ErrorKind::ReferenceError:
            return PyExc_NameError;
        case ErrorKind::SyntaxError:
            return PyExc_SyntaxError;
    }
    return types.error;
}

PyObject* NewError(Isolate& isolate, ErrorKind kind, std::string_view message) noexcept {
    PyObject* text = TextString(message);
    if (text == nullptr) {
        return nullptr;
    }
    PyObject* error = PyObject_CallOneArg(ErrorClass(isolate, kind), text);
    Py_DECREF(text);
    return error;
}

void RaiseError(Isolate& isolate, ErrorKind kind, std::string_view message) noexcept {
    PyObject* error = NewError(isolate, kind, message);
    if (error != nullptr) {
        PyErr_SetRaisedException(error);
    }
}

void RaiseValue(Isolate& isolate, PyObject* value) noexcept {
    if (PyExceptionInstance_Check(value)) {
        PyErr_SetRaisedException(Py_NewRef(value));
        return;
    }
    PyObject* wrapped = PyObject_CallOneArg(isolate.impl().types.thrown, value);
    if (wrapped != nullptr) {
        PyErr_SetRaisedException(wrapped);
    }
}

PyObject* UnwrapThrown(Isolate& isolate, PyObject* exception) noexcept {
    if (PyObject_TypeCheck(exception, reinterpret_cast<PyTypeObject*>(isolate.impl().types.thrown))) {
        PyObject* value = PyObject_GetAttrString(exception, "value");
        if (value != nullptr) {
            return value;
        }
        PyErr_Clear();
    }
    return Py_NewRef(exception);
}

std::optional<Slot> MakeError(const Context& context, ErrorKind kind, std::string_view message) {
    Isolate& isolate = OwnerOf(context);
    return PushOrNothing(isolate, NewError(isolate, kind, message));
}

}  // namespace ub::detail
