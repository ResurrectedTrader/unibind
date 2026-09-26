// Objects: `unibind.Object`, the JavaScript-shaped property bag, and every
// property operation of the backend interface.
//
// A Python object already has properties - attributes, or items - so the
// question this file answers is which of them an `ub::Object` operation means,
// and where Python has nothing to offer, what stands in:
//
//   * **`unibind.Object`** carries what an ordinary Python object cannot: an
//     ordered own-property table, per-property attributes (ReadOnly, DontEnum,
//     DontDelete), accessor properties that run native code, a [[Prototype]]
//     chain that lookup walks, and the interceptor a template may put on it.
//     Every object a template or a class makes is one - their types derive
//     from it - and so is `Object::New`.
//   * **A dict** is its items, keyed as the backend keys everything (see
//     `NormalizeKey`): `o["3"]` and `o[3]` are one property. A realm's global
//     object is a dict, so this is also how the embedder reads and writes a
//     realm's globals.
//   * **A list** is an Array: its indices, and a `length`. A tuple is a
//     read-only one.
//   * **Anything else** is its attributes: `getattr`, `setattr`, `hasattr`,
//     `delattr`, with an index key spelled as its digits.
//
// What a `unibind.Object` looks like from Python is the other half of this
// file, and the rule is that it should be natural there: `o.x`, `o["x"]`,
// `o[0]`, `"x" in o`, `del o.x`, `for k in o`, `len(o)`, and a repr that looks
// like the object literal it would be in JavaScript. The places where Python
// and JavaScript disagree are decided below, where they arise, and each says
// which way it went.

#include <algorithm>
#include <cstring>
#include <string>

#include "bindings.h"

namespace ub::detail {

namespace {

constexpr const char* ACCESSOR_CAPSULE = "unibind.accessor";

/// Longest [[Prototype]] chain a lookup walks. `SetPrototype` refuses a cycle,
/// so this is a guard against a chain long enough to be a mistake rather than
/// against a loop.
constexpr int MAX_CHAIN = 100000;

/// Most elements a list is grown to by one index write or `Array::New`. A
/// JavaScript array may be sparse - `new Array(3e9)` is holes and costs
/// nothing - and a Python list may not: every element is a pointer, so an
/// array of 2^32 - 1 holes is 32 GiB of `None`. Past this the operation is
/// refused with a RangeError rather than attempted, because attempting it
/// can succeed on a machine with enough swap and then page the process to
/// death.
constexpr std::uint64_t MAX_DENSE_LENGTH = std::uint64_t{1} << 26;

[[nodiscard]] Isolate* IsolateOrRaise() noexcept {
    Isolate* isolate = CurrentIsolate();
    if (isolate == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "unibind: no isolate on this thread");
    }
    return isolate;
}

[[nodiscard]] ObjectInstance* AsInstance(PyObject* object) noexcept {
    return reinterpret_cast<ObjectInstance*>(object);
}

/// `__name__`-shaped: the names Python's own protocols look up on an instance,
/// which an interceptor is not asked about (see `ObjectGetAttr`).
[[nodiscard]] bool IsDunder(PyObject* name) noexcept {
    if (!PyUnicode_Check(name)) {
        return false;
    }
    const Py_ssize_t length = PyUnicode_GET_LENGTH(name);
    if (length < 5) {
        return false;
    }
    const int kind = PyUnicode_KIND(name);
    const void* data = PyUnicode_DATA(name);
    return PyUnicode_READ(kind, data, 0) == '_' && PyUnicode_READ(kind, data, 1) == '_' &&
           PyUnicode_READ(kind, data, length - 1) == '_' && PyUnicode_READ(kind, data, length - 2) == '_';
}

/// The well-known symbol a dunder key stands for, if it is one. A template's
/// `Symbol.iterator` method is stored under `__iter__` (see `NormalizeKey`),
/// and own-key listings hand it back as the symbol it was declared as.
[[nodiscard]] PyObject* WellKnownFor(Isolate& isolate, PyObject* key) noexcept {
    if (!PyUnicode_Check(key)) {
        return nullptr;
    }
    for (PyObject* symbol : isolate.impl().types.wellKnown) {
        if (symbol == nullptr) {
            continue;
        }
        PyObject* dunder = reinterpret_cast<SymbolObject*>(symbol)->dunder;
        if (dunder != nullptr && PyUnicode_Compare(key, dunder) == 0) {
            return symbol;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The own-property table
//
// `properties` maps a normalized key to its value, in insertion order - which
// is JavaScript's order for string keys and for symbols; integer keys are
// sorted when listed, as the language lists them. `meta` records attributes
// only where they are not the default, so an object nobody declared anything
// unusual on has none.
// ---------------------------------------------------------------------------

/// The attribute bits recorded for `key`: 0 for an ordinary data property.
[[nodiscard]] long AttributesOf(const ObjectInstance* object, PyObject* key) noexcept {
    if (object->meta == nullptr) {
        return 0;
    }
    PyObject* bits = PyDict_GetItemWithError(object->meta, key);  // borrowed
    if (bits == nullptr) {
        PyErr_Clear();  // an int key or a str key: hashing cannot fail
        return 0;
    }
    return PyLong_AsLong(bits);
}

[[nodiscard]] bool SetAttributes(ObjectInstance* object, PyObject* key, long attributes) noexcept {
    if (attributes == 0) {
        if (object->meta != nullptr && PyDict_DelItem(object->meta, key) != 0) {
            if (PyErr_ExceptionMatches(PyExc_KeyError) == 0) {
                return false;
            }
            PyErr_Clear();
        }
        return true;
    }
    if (object->meta == nullptr) {
        object->meta = PyDict_New();
        if (object->meta == nullptr) {
            return false;
        }
    }
    PyObject* bits = PyLong_FromLong(attributes);
    if (bits == nullptr) {
        return false;
    }
    const int stored = PyDict_SetItem(object->meta, key, bits);
    Py_DECREF(bits);
    return stored == 0;
}

/// Own lookup: 1 with `*value` a new reference and `*attributes` its bits, 0
/// if absent, -1 with an exception pending.
[[nodiscard]] int OwnLookup(ObjectInstance* object, PyObject* key, PyObject** value, long* attributes) noexcept {
    PyObject* found = PyDict_GetItemWithError(object->properties, key);  // borrowed
    if (found == nullptr) {
        return PyErr_Occurred() != nullptr ? -1 : 0;
    }
    *value = Py_NewRef(found);
    *attributes = AttributesOf(object, key);
    return 1;
}

[[nodiscard]] const CallbackRecord* RecordOf(PyObject* capsule) noexcept {
    auto* record = static_cast<CallbackRecord*>(PyCapsule_GetPointer(capsule, ACCESSOR_CAPSULE));
    if (record == nullptr) {
        PyErr_Clear();
    }
    return record;
}

/// Text to name a key by in an error message: its `str()`.
[[nodiscard]] std::string KeyText(PyObject* key) {
    PyObject* text = PyObject_Str(key);
    if (text == nullptr) {
        PyErr_Clear();
        return "?";
    }
    std::string out = Utf8Of(text);
    Py_DECREF(text);
    return out;
}

// ---------------------------------------------------------------------------
// The property algorithms
//
// Each is ECMAScript's ordinary one, over the table above, with a template's
// interceptor consulted first where the object has one. `hooks` is false only
// for Python's own protocol lookups (see `ObjectGetAttr`).
// ---------------------------------------------------------------------------

enum class Outcome : std::uint8_t {
    Done,
    Absent,   ///< delete: there was nothing to delete
    Refused,  ///< a read-only property, a getter with no setter, a permanent property
    Failed,   ///< an exception is pending
};

/// [[Get]]: the receiver's own property, else its prototype's, and so on. An
/// accessor found anywhere on the way runs with the receiver as `This()` and
/// the object it was found on as `Holder()`.
[[nodiscard]] PyObject* GetChain(Isolate& isolate, ObjectInstance* receiver, PyObject* key, bool hooks,
                                 bool* found) noexcept {
    *found = false;
    // Held while it is being asked about: a hook runs arbitrary code, which
    // may swap the prototype out from under the walk.
    PyObject* holder = Py_NewRef(reinterpret_cast<PyObject*>(receiver));
    for (int depth = 0; holder != nullptr && depth < MAX_CHAIN; ++depth) {
        ObjectInstance* object = AsInstance(holder);
        if (hooks) {
            if (TemplateRec* shape = ShapeOf(object); shape != nullptr && HasHandlerFor(shape, key)) {
                PyObject* answer = nullptr;
                const Hook hook =
                    InterceptGet(isolate, shape, key, reinterpret_cast<PyObject*>(receiver), holder, &answer);
                if (hook != Hook::Declined) {
                    Py_DECREF(holder);
                    *found = hook == Hook::Handled;
                    return answer;
                }
            }
        }
        PyObject* value = nullptr;
        long attributes = 0;
        const int own = OwnLookup(object, key, &value, &attributes);
        if (own < 0) {
            Py_DECREF(holder);
            return nullptr;
        }
        if (own > 0) {
            *found = true;
            if ((attributes & ATTR_ACCESSOR) != 0) {
                const CallbackRecord* record = RecordOf(value);
                PyObject* result =
                    record != nullptr && record->getter != nullptr
                        ? RunAccessorGetter(isolate, *record, key, reinterpret_cast<PyObject*>(receiver), holder)
                        : Py_NewRef(Py_None);
                Py_DECREF(value);
                Py_DECREF(holder);
                return result;
            }
            Py_DECREF(holder);
            return value;
        }
        PyObject* next = Py_XNewRef(object->prototype);
        Py_DECREF(holder);
        holder = next;
    }
    Py_XDECREF(holder);
    return Py_NewRef(Py_None);
}

/// [[Set]], OrdinarySet: the receiver's setter hook; else the first property
/// of that name along the chain decides - an accessor's setter runs, a
/// read-only data property refuses, a writable one is written (on the
/// receiver, whichever object it was found on) - and with none, an own data
/// property is made.
///
/// A setter hook is asked only about a write made on the object itself, as
/// V8 asks it; a write through something inheriting from an intercepted
/// object is the ordinary set.
[[nodiscard]] Outcome SetChain(Isolate& isolate, ObjectInstance* receiver, PyObject* key, PyObject* value, bool hooks,
                               bool* getterOnly) noexcept {
    *getterOnly = false;
    auto* self = reinterpret_cast<PyObject*>(receiver);
    if (hooks) {
        if (TemplateRec* shape = ShapeOf(receiver); shape != nullptr && HasHandlerFor(shape, key)) {
            switch (InterceptSet(isolate, shape, key, value, self)) {
                case Hook::Handled:
                    return Outcome::Done;
                case Hook::Failed:
                    return Outcome::Failed;
                case Hook::Declined:
                    break;
            }
        }
    }
    PyObject* holder = Py_NewRef(self);
    for (int depth = 0; holder != nullptr && depth < MAX_CHAIN; ++depth) {
        ObjectInstance* object = AsInstance(holder);
        PyObject* existing = nullptr;
        long attributes = 0;
        const int own = OwnLookup(object, key, &existing, &attributes);
        if (own < 0) {
            Py_DECREF(holder);
            return Outcome::Failed;
        }
        if (own > 0) {
            if ((attributes & ATTR_ACCESSOR) != 0) {
                const CallbackRecord* record = RecordOf(existing);
                Py_DECREF(existing);
                if (record == nullptr || record->setter == nullptr) {
                    Py_DECREF(holder);
                    *getterOnly = true;
                    return Outcome::Refused;
                }
                const bool ran = RunAccessorSetter(isolate, *record, key, value, self, holder);
                Py_DECREF(holder);
                return ran ? Outcome::Done : Outcome::Failed;
            }
            Py_DECREF(existing);
            if ((attributes & static_cast<long>(PropertyAttribute::ReadOnly)) != 0) {
                // Inherited or own, a read-only data property refuses the
                // write: the language's rule, which keeps a frozen prototype
                // from being shadowed by assignment.
                Py_DECREF(holder);
                return Outcome::Refused;
            }
            if (holder == self) {
                Py_DECREF(holder);
                return PyDict_SetItem(receiver->properties, key, value) == 0 ? Outcome::Done : Outcome::Failed;
            }
            break;  // a writable data property up the chain: shadow it
        }
        PyObject* next = Py_XNewRef(object->prototype);
        Py_DECREF(holder);
        holder = next;
    }
    Py_XDECREF(holder);
    return PyDict_SetItem(receiver->properties, key, value) == 0 ? Outcome::Done : Outcome::Failed;
}

[[nodiscard]] Outcome DeleteOwn(Isolate& isolate, ObjectInstance* object, PyObject* key, bool hooks) noexcept {
    if (hooks) {
        if (TemplateRec* shape = ShapeOf(object); shape != nullptr && HasHandlerFor(shape, key)) {
            bool answer = false;
            switch (InterceptDelete(isolate, shape, key, reinterpret_cast<PyObject*>(object), &answer)) {
                case Hook::Handled:
                    return answer ? Outcome::Done : Outcome::Refused;
                case Hook::Failed:
                    return Outcome::Failed;
                case Hook::Declined:
                    break;
            }
        }
    }
    PyObject* existing = nullptr;
    long attributes = 0;
    const int own = OwnLookup(object, key, &existing, &attributes);
    if (own < 0) {
        return Outcome::Failed;
    }
    if (own == 0) {
        return Outcome::Absent;
    }
    Py_DECREF(existing);
    if ((attributes & static_cast<long>(PropertyAttribute::DontDelete)) != 0) {
        return Outcome::Refused;
    }
    if (PyDict_DelItem(object->properties, key) != 0 || !SetAttributes(object, key, 0)) {
        return Outcome::Failed;
    }
    return Outcome::Done;
}

/// [[GetOwnProperty]] through the hooks: the query hook, then the getter -
/// what the SpiderMonkey backend asks, so that a getter on its own is enough
/// to make a key look like an own property. 1, 0 or -1 (thrown).
[[nodiscard]] int HasOwn(Isolate& isolate, ObjectInstance* object, PyObject* key, bool hooks) noexcept {
    if (hooks) {
        if (TemplateRec* shape = ShapeOf(object); shape != nullptr && HasHandlerFor(shape, key)) {
            PropertyAttribute attributes = PropertyAttribute::None;
            const Hook queried = InterceptQuery(isolate, shape, key, reinterpret_cast<PyObject*>(object), &attributes);
            if (queried != Hook::Declined) {
                return queried == Hook::Handled ? 1 : -1;
            }
            PyObject* answer = nullptr;
            const Hook got = InterceptGet(isolate, shape, key, reinterpret_cast<PyObject*>(object),
                                          reinterpret_cast<PyObject*>(object), &answer);
            Py_XDECREF(answer);
            if (got != Hook::Declined) {
                return got == Hook::Handled ? 1 : -1;
            }
        }
    }
    return PyDict_Contains(object->properties, key);
}

[[nodiscard]] int HasChain(Isolate& isolate, ObjectInstance* receiver, PyObject* key, bool hooks) noexcept {
    PyObject* holder = Py_NewRef(reinterpret_cast<PyObject*>(receiver));
    for (int depth = 0; holder != nullptr && depth < MAX_CHAIN; ++depth) {
        const int own = HasOwn(isolate, AsInstance(holder), key, hooks);
        if (own != 0) {
            Py_DECREF(holder);
            return own;
        }
        PyObject* next = Py_XNewRef(AsInstance(holder)->prototype);
        Py_DECREF(holder);
        holder = next;
    }
    Py_XDECREF(holder);
    return 0;
}

/// The attributes of the property `key` names, found along the chain as
/// `Get` would find it. 1 with `*out` set, 0 absent, -1 thrown.
[[nodiscard]] int AttributesChain(Isolate& isolate, ObjectInstance* receiver, PyObject* key,
                                  PropertyAttribute* out) noexcept {
    PyObject* holder = Py_NewRef(reinterpret_cast<PyObject*>(receiver));
    for (int depth = 0; holder != nullptr && depth < MAX_CHAIN; ++depth) {
        ObjectInstance* object = AsInstance(holder);
        if (TemplateRec* shape = ShapeOf(object); shape != nullptr && HasHandlerFor(shape, key)) {
            const Hook queried = InterceptQuery(isolate, shape, key, holder, out);
            if (queried != Hook::Declined) {
                Py_DECREF(holder);
                return queried == Hook::Handled ? 1 : -1;
            }
            PyObject* answer = nullptr;
            const Hook got = InterceptGet(isolate, shape, key, reinterpret_cast<PyObject*>(receiver), holder, &answer);
            Py_XDECREF(answer);
            if (got != Hook::Declined) {
                Py_DECREF(holder);
                *out = PropertyAttribute::None;
                return got == Hook::Handled ? 1 : -1;
            }
        }
        PyObject* value = nullptr;
        long attributes = 0;
        const int own = OwnLookup(object, key, &value, &attributes);
        if (own != 0) {
            Py_XDECREF(value);
            Py_DECREF(holder);
            *out = static_cast<PropertyAttribute>(attributes & ATTRIBUTE_MASK);
            return own;
        }
        PyObject* next = Py_XNewRef(object->prototype);
        Py_DECREF(holder);
        holder = next;
    }
    Py_XDECREF(holder);
    return 0;
}

/// Own keys in the language's order - array indices ascending, then strings in
/// insertion order, then symbols in insertion order - with the keys an
/// enumerator claims merged in, each key once. New list, or null (thrown).
///
/// A key an enumerator claims is enumerable unless the query hook says it is
/// not, which is how V8 decides it for `Object.keys` too.
[[nodiscard]] PyObject* OwnKeys(Isolate& isolate, ObjectInstance* object, KeyFilter filter, bool hooks) noexcept {
    PyObject* indices = PyList_New(0);
    PyObject* strings = PyList_New(0);
    PyObject* symbols = PyList_New(0);
    PyObject* seen = PySet_New(nullptr);
    PyObject* own = PyDict_Keys(object->properties);
    PyObject* claimed = nullptr;
    PyObject* result = nullptr;

    const auto add = [&](PyObject* key) -> bool {
        const int already = PySet_Contains(seen, key);
        if (already != 0) {
            return already > 0;
        }
        if (PySet_Add(seen, key) != 0) {
            return false;
        }
        if (PyLong_Check(key)) {
            return PyList_Append(indices, key) == 0;
        }
        if (PyObject* symbol = WellKnownFor(isolate, key); symbol != nullptr) {
            return PyList_Append(symbols, symbol) == 0;
        }
        if (IsSymbol(isolate, key)) {
            return PyList_Append(symbols, key) == 0;
        }
        return PyList_Append(strings, key) == 0;
    };

    bool ok = indices != nullptr && strings != nullptr && symbols != nullptr && seen != nullptr && own != nullptr;
    for (Py_ssize_t i = 0; ok && i < PyList_GET_SIZE(own); ++i) {
        PyObject* key = PyList_GET_ITEM(own, i);
        const long attributes = AttributesOf(object, key);
        if (!filter.includeNonEnumerable && (attributes & static_cast<long>(PropertyAttribute::DontEnum)) != 0) {
            continue;
        }
        ok = add(key);
    }

    TemplateRec* shape = hooks ? ShapeOf(object) : nullptr;
    if (ok && shape != nullptr &&
        ((shape->hasNamed && shape->named.enumerator != nullptr) ||
         (shape->hasIndexed && shape->indexed.enumerator != nullptr))) {
        claimed = PyList_New(0);
        ok = claimed != nullptr && InterceptEnumerate(isolate, shape, reinterpret_cast<PyObject*>(object), claimed);
        for (Py_ssize_t i = 0; ok && claimed != nullptr && i < PyList_GET_SIZE(claimed); ++i) {
            PyObject* key = PyList_GET_ITEM(claimed, i);
            if (!filter.includeNonEnumerable) {
                const bool hasQuery = PyLong_Check(key) ? (shape->hasIndexed && shape->indexed.query != nullptr)
                                                        : (shape->hasNamed && shape->named.query != nullptr);
                if (hasQuery) {
                    PropertyAttribute attributes = PropertyAttribute::None;
                    const Hook queried =
                        InterceptQuery(isolate, shape, key, reinterpret_cast<PyObject*>(object), &attributes);
                    if (queried == Hook::Failed) {
                        ok = false;
                        break;
                    }
                    if (queried == Hook::Handled && HasAttribute(attributes, PropertyAttribute::DontEnum)) {
                        continue;
                    }
                }
            }
            ok = add(key);
        }
    }

    if (ok && PyList_Sort(indices) == 0) {
        result = PySequence_Concat(indices, strings);
        if (result != nullptr && filter.includeSymbols) {
            PyObject* all = PySequence_Concat(result, symbols);
            Py_SETREF(result, all);
        }
    }
    Py_XDECREF(indices);
    Py_XDECREF(strings);
    Py_XDECREF(symbols);
    Py_XDECREF(seen);
    Py_XDECREF(own);
    Py_XDECREF(claimed);
    return result;
}

/// DefineOwnProperty with ECMAScript's one rule that matters here: a
/// permanent (DontDelete) property may not be changed, beyond a writable one
/// being given a new value or made read-only. 1 defined, 0 refused, -1 thrown.
[[nodiscard]] int DefineChecked(ObjectInstance* object, PyObject* key, PyObject* value, long attributes) noexcept {
    PyObject* existing = nullptr;
    long current = 0;
    const int own = OwnLookup(object, key, &existing, &current);
    if (own < 0) {
        return -1;
    }
    if (own > 0) {
        const bool permanent = (current & static_cast<long>(PropertyAttribute::DontDelete)) != 0;
        const bool sameValue = existing == value;
        Py_DECREF(existing);
        if (permanent) {
            const long readOnly = static_cast<long>(PropertyAttribute::ReadOnly);
            if ((current & ATTR_ACCESSOR) != 0) {
                return 0;
            }
            if ((current & readOnly) != 0 && (attributes != current || !sameValue)) {
                return 0;
            }
            // Writable: may keep its attributes or lose writability, nothing else.
            if (attributes != current && attributes != (current | readOnly)) {
                return 0;
            }
        }
    }
    return DefineRaw(object, key, value, attributes) ? 1 : -1;
}

// ---------------------------------------------------------------------------
// Ordinary Python objects
// ---------------------------------------------------------------------------

/// A key as an attribute name: a string as itself, an index as its digits.
/// Null - with nothing pending - for a symbol, which no attribute can be named.
[[nodiscard]] PyObject* AttributeName(Isolate& isolate, PyObject* key) noexcept {
    if (PyUnicode_Check(key)) {
        return Py_NewRef(key);
    }
    if (IsSymbol(isolate, key)) {
        return nullptr;
    }
    return PyObject_Str(key);
}

[[nodiscard]] bool IsLengthKey(PyObject* key) noexcept {
    return PyUnicode_Check(key) && PyUnicode_CompareWithASCIIString(key, "length") == 0;
}

/// A normalized int key as a list position; false for anything else.
[[nodiscard]] bool AsPosition(PyObject* key, Py_ssize_t* position) noexcept {
    if (!PyLong_Check(key)) {
        return false;
    }
    *position = PyLong_AsSsize_t(key);
    if (*position == -1 && PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        return false;
    }
    return *position >= 0;
}

[[nodiscard]] bool RangeCheck(Isolate& isolate, std::uint64_t length) noexcept {
    if (length > MAX_DENSE_LENGTH) {
        RaiseError(isolate, ErrorKind::RangeError, "an array this long is more than this backend will allocate");
        return false;
    }
    return true;
}

/// JavaScript's array write: past the end grows the array, the gap in between
/// filled with `None`, which is what a hole reads as.
[[nodiscard]] bool ListSet(Isolate& isolate, PyObject* list, Py_ssize_t position, PyObject* value) noexcept {
    const Py_ssize_t size = PyList_GET_SIZE(list);
    if (position < size) {
        return PyList_SetItem(list, position, Py_NewRef(value)) == 0;
    }
    if (!RangeCheck(isolate, static_cast<std::uint64_t>(position) + 1)) {
        return false;
    }
    for (Py_ssize_t i = size; i < position; ++i) {
        if (PyList_Append(list, Py_None) != 0) {
            return false;
        }
    }
    return PyList_Append(list, value) == 0;
}

[[nodiscard]] bool ListSetLength(Isolate& isolate, PyObject* list, PyObject* value) noexcept {
    const Py_ssize_t length = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (length == -1 && PyErr_Occurred() != nullptr) {
        return false;
    }
    if (length < 0) {
        RaiseError(isolate, ErrorKind::RangeError, "Invalid array length");
        return false;
    }
    const Py_ssize_t size = PyList_GET_SIZE(list);
    if (length <= size) {
        return PyList_SetSlice(list, length, size, nullptr) == 0;
    }
    if (!RangeCheck(isolate, static_cast<std::uint64_t>(length))) {
        return false;
    }
    for (Py_ssize_t i = size; i < length; ++i) {
        if (PyList_Append(list, Py_None) != 0) {
            return false;
        }
    }
    return true;
}

/// `__dict__`, if the object has a mapping there. New reference or null, with
/// nothing pending either way.
[[nodiscard]] PyObject* InstanceDict(PyObject* object) noexcept {
    PyObject* dict = PyObject_GetAttrString(object, "__dict__");
    if (dict == nullptr) {
        PyErr_Clear();
        return nullptr;
    }
    if (PyMapping_Check(dict) == 0) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

/// Whether an ordinary object's attribute is enumerable: one whose name starts
/// with an underscore is, by the only convention Python has, not for listing.
[[nodiscard]] bool EnumerableName(PyObject* name) noexcept {
    return !(PyUnicode_Check(name) && PyUnicode_GET_LENGTH(name) > 0 &&
             PyUnicode_READ_CHAR(name, 0) == static_cast<Py_UCS4>('_'));
}

/// The keys of the backend's own bookkeeping in a realm's globals - the realm
/// capsule, the template cache - which are not the embedder's globals.
[[nodiscard]] bool IsInternalKey(PyObject* key) noexcept {
    if (!PyUnicode_Check(key)) {
        return false;
    }
    const char* text = PyUnicode_AsUTF8(key);
    if (text == nullptr) {
        PyErr_Clear();
        return false;
    }
    return std::strncmp(text, "__unibind_", 10) == 0;
}

[[nodiscard]] Outcome SetAny(Isolate& isolate, PyObject* object, PyObject* key, PyObject* value) noexcept {
    if (IsObjectInstance(isolate, object)) {
        bool getterOnly = false;
        return SetChain(isolate, AsInstance(object), key, value, true, &getterOnly);
    }
    if (PyDict_Check(object)) {
        return PyObject_SetItem(object, key, value) == 0 ? Outcome::Done : Outcome::Failed;
    }
    if (PyList_Check(object)) {
        Py_ssize_t position = 0;
        if (AsPosition(key, &position)) {
            return ListSet(isolate, object, position, value) ? Outcome::Done : Outcome::Failed;
        }
        if (IsLengthKey(key)) {
            return ListSetLength(isolate, object, value) ? Outcome::Done : Outcome::Failed;
        }
    }
    if (PyTuple_Check(object)) {
        Py_ssize_t position = 0;
        if (AsPosition(key, &position) || IsLengthKey(key)) {
            return Outcome::Refused;  // a tuple is a frozen array
        }
    }
    PyObject* name = AttributeName(isolate, key);
    if (name == nullptr) {
        return PyErr_Occurred() != nullptr ? Outcome::Failed : Outcome::Refused;
    }
    const int set = PyObject_SetAttr(object, name, value);
    Py_DECREF(name);
    return set == 0 ? Outcome::Done : Outcome::Failed;
}

/// 1, 0, or -1 (thrown).
[[nodiscard]] int HasOwnAny(Isolate& isolate, PyObject* object, PyObject* key) noexcept {
    if (IsObjectInstance(isolate, object)) {
        return HasOwn(isolate, AsInstance(object), key, true);
    }
    if (PyDict_Check(object)) {
        return PyDict_Contains(object, key);
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        Py_ssize_t position = 0;
        if (AsPosition(key, &position)) {
            return position < PySequence_Size(object) ? 1 : 0;
        }
        if (IsLengthKey(key)) {
            return 1;
        }
    }
    PyObject* name = AttributeName(isolate, key);
    if (name == nullptr) {
        return PyErr_Occurred() != nullptr ? -1 : 0;
    }
    PyObject* dict = InstanceDict(object);
    int has = 0;
    if (dict != nullptr) {
        has = PySequence_Contains(dict, name);
        Py_DECREF(dict);
    }
    Py_DECREF(name);
    return has;
}

[[nodiscard]] int HasAny(Isolate& isolate, PyObject* object, PyObject* key) noexcept {
    if (IsObjectInstance(isolate, object)) {
        return HasChain(isolate, AsInstance(object), key, true);
    }
    if (PyDict_Check(object)) {
        return PyDict_Contains(object, key);
    }
    bool found = false;
    PyObject* value = GetAny(isolate, object, key, &found);
    if (value == nullptr) {
        return -1;
    }
    Py_DECREF(value);
    return found ? 1 : 0;
}

/// An ordinary object's own keys: a dict's (less the backend's own), a list's
/// indices, anything else's instance attributes.
[[nodiscard]] PyObject* OwnKeysAny(Isolate& isolate, PyObject* object, KeyFilter filter) noexcept {
    PyObject* keys = PyList_New(0);
    if (keys == nullptr) {
        return nullptr;
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        const Py_ssize_t size = PySequence_Size(object);
        for (Py_ssize_t i = 0; i < size; ++i) {
            PyObject* index = PyLong_FromSsize_t(i);
            if (index == nullptr || PyList_Append(keys, index) != 0) {
                Py_XDECREF(index);
                Py_DECREF(keys);
                return nullptr;
            }
            Py_DECREF(index);
        }
        if (filter.includeNonEnumerable) {
            PyObject* length = PyUnicode_FromString("length");
            const int appended = length == nullptr ? -1 : PyList_Append(keys, length);
            Py_XDECREF(length);
            if (appended != 0) {
                Py_DECREF(keys);
                return nullptr;
            }
        }
        return keys;
    }

    const bool isDict = PyDict_Check(object) != 0;
    PyObject* source = isDict ? PyDict_Keys(object) : nullptr;
    if (!isDict) {
        PyObject* dict = InstanceDict(object);
        if (dict == nullptr) {
            return keys;  // no instance attributes: no own keys
        }
        source = PyMapping_Keys(dict);
        Py_DECREF(dict);
    }
    if (source == nullptr) {
        Py_DECREF(keys);
        return nullptr;
    }
    PyObject* symbols = PyList_New(0);
    bool ok = symbols != nullptr;
    for (Py_ssize_t i = 0; ok && i < PyList_GET_SIZE(source); ++i) {
        PyObject* raw = PyList_GET_ITEM(source, i);
        if (IsInternalKey(raw)) {
            continue;
        }
        if (!isDict && !filter.includeNonEnumerable && !EnumerableName(raw)) {
            continue;
        }
        if (PyObject* symbol = WellKnownFor(isolate, raw); symbol != nullptr || IsSymbol(isolate, raw)) {
            ok = PyList_Append(symbols, symbol != nullptr ? symbol : raw) == 0;
            continue;
        }
        PyObject* key = NormalizeKey(isolate, raw);
        ok = key != nullptr && PyList_Append(keys, key) == 0;
        Py_XDECREF(key);
    }
    Py_DECREF(source);
    if (ok && filter.includeSymbols) {
        const Py_ssize_t size = PyList_GET_SIZE(keys);
        ok = PyList_SetSlice(keys, size, size, symbols) == 0;
    }
    Py_XDECREF(symbols);
    if (!ok) {
        Py_DECREF(keys);
        return nullptr;
    }
    return keys;
}

// ---------------------------------------------------------------------------
// The backend-interface operations share one prologue: the gate every entry
// into Python code needs, and the realm the operation was given.
// ---------------------------------------------------------------------------

class ApiOp {
   public:
    explicit ApiOp(const Context& context) noexcept
        : isolate_(&OwnerOf(context)), gate_(*isolate_), realm_(*isolate_, context.rec()) {}

    [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }

    [[nodiscard]] bool Open() const noexcept { return gate_.Open(); }

    /// `NormalizeKey` of a slot. New reference or null (thrown).
    [[nodiscard]] PyObject* Key(Slot key) const noexcept { return NormalizeKey(*isolate_, Resolve(key)); }
    [[nodiscard]] PyObject* Index(std::uint32_t index) const noexcept {
        PyObject* raw = PyLong_FromUnsignedLong(index);
        if (raw == nullptr) {
            return nullptr;
        }
        // 2^32 - 1 is not an array index, and becomes the string it spells.
        PyObject* key = NormalizeKey(*isolate_, raw);
        Py_DECREF(raw);
        return key;
    }

   private:
    Isolate* isolate_;
    ScriptGate gate_;
    RealmScope realm_;
};

// ---------------------------------------------------------------------------
// unibind.Object, as Python sees it
// ---------------------------------------------------------------------------

/// Give the native a class instance carries back, exactly once: here, on the
/// isolate's thread, unless teardown already has. An instance that dies on a
/// thread a script started leaves its native in `liveNatives`, for `~Isolate`
/// to give back on the isolate's thread: a native is the embedder's object,
/// and is never destroyed on a thread the embedder did not make.
void ReleaseBox(ObjectInstance* object) noexcept {
    NativeBox* box = std::exchange(object->box, nullptr);
    if (box == nullptr) {
        return;
    }
    Isolate* isolate = CurrentIsolate();
    if (isolate == nullptr) {
        return;
    }
    Isolate::Impl& impl = isolate->impl();
    if (impl.nativesReleased || impl.liveNatives.erase(box) == 0) {
        // Given back already, by `~Isolate`: the pointer is dangling now and
        // must not be touched.
        return;
    }
    // A native's destructor is the embedder's code, and may release a Global
    // or a Context of its own - into the interpreter, in the middle of a
    // deallocation that may itself be in the middle of an exception.
    PyObject* pending = PyErr_GetRaisedException();
    box->destroy(box);
    if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();
    }
    if (pending != nullptr) {
        PyErr_SetRaisedException(pending);
    }
}

void ObjectDealloc(PyObject* self) {
    ObjectInstance* object = AsInstance(self);
    PyTypeObject* type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_BEGIN(self, ObjectDealloc) if (object->weaklist != nullptr) {
        PyObject_ClearWeakRefs(self);
    }
    ReleaseBox(object);
    Py_CLEAR(object->properties);
    Py_CLEAR(object->meta);
    Py_CLEAR(object->prototype);
    type->tp_free(self);
    Py_DECREF(type);
    Py_TRASHCAN_END
}

int ObjectTraverse(PyObject* self, visitproc visit, void* arg) {
    ObjectInstance* object = AsInstance(self);
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(object->properties);
    Py_VISIT(object->meta);
    Py_VISIT(object->prototype);
    return 0;
}

/// Breaks what a cycle can run through. The property table is emptied rather
/// than dropped, so that an object cleared but not yet deallocated - one a
/// weak reference callback still reaches - is an empty object rather than a
/// broken one. The native stays until the deallocation, which gives it back.
int ObjectClear(PyObject* self) {
    ObjectInstance* object = AsInstance(self);
    if (object->properties != nullptr) {
        PyDict_Clear(object->properties);
    }
    if (object->meta != nullptr) {
        PyDict_Clear(object->meta);
    }
    Py_CLEAR(object->prototype);
    return 0;
}

/// Whether `name` is declared by a Python class between `type` and the first
/// type in its MRO that a template made (or `unibind.Object` itself) - that
/// is, by a Python subclass, whose members override the prototype chain.
[[nodiscard]] bool DeclaredBySubclass(Isolate& isolate, PyTypeObject* type, PyObject* name) noexcept {
    PyObject* mro = type->tp_mro;
    if (mro == nullptr || !PyTuple_Check(mro)) {
        return false;
    }
    PyTypeObject* base = isolate.impl().types.object;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
        auto* entry = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro, i));
        if (entry == base || IsTemplateMadeType(isolate, entry)) {
            return false;
        }
        if (entry->tp_dict != nullptr && PyDict_Contains(entry->tp_dict, name) == 1) {
            return true;
        }
    }
    return false;
}

/// `o.name`.
///
/// Python's data descriptors on the type come first - `__class__`,
/// `__proto__`, a `property` a Python subclass declared - as they do for any
/// Python object; then the JavaScript lookup, own properties and the
/// prototype chain, with the object's interceptor asked first; then the
/// ordinary attribute lookup, which finds methods a Python subclass defines
/// and raises AttributeError for a name nothing has.
///
/// A native function found by this lookup comes back bound to the object, so
/// `o.method()` calls it with `This()` the object, as `o.method()` does in
/// JavaScript. A Python function stored as a property does not - it is a value
/// in a table, as it would be in a dict.
///
/// A dunder name is not put to the interceptor. Python's machinery looks such
/// names up on instances constantly - `copy`, `pickle`, `inspect`, a bare
/// `hasattr(o, "__len__")` - and a catch-all interceptor, which is the reason
/// interceptors exist, would otherwise answer every one of them.
PyObject* ObjectGetAttr(PyObject* self, PyObject* name) {
    if (!PyUnicode_Check(name)) {
        return PyObject_GenericGetAttr(self, name);
    }
    PyObject* descriptor = _PyType_Lookup(Py_TYPE(self), name);  // borrowed
    if (descriptor != nullptr && Py_TYPE(descriptor)->tp_descr_set != nullptr) {
        return PyObject_GenericGetAttr(self, name);
    }
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    PyObject* key = NormalizeKey(*isolate, name);
    if (key == nullptr) {
        return nullptr;
    }
    // A member a Python subclass declares overrides the template's prototype,
    // as a subclass's method overrides its base's in Python: `class
    // Loud(Counter): def increment(self)` must be what `Loud().increment()`
    // runs. Only an own property of the instance comes before it, as an
    // instance attribute does in Python.
    if (DeclaredBySubclass(*isolate, Py_TYPE(self), name)) {
        PyObject* properties = AsInstance(self)->properties;
        const int own = properties != nullptr ? PyDict_Contains(properties, key) : 0;
        if (own <= 0) {
            Py_DECREF(key);
            if (own < 0) {
                return nullptr;
            }
            return PyObject_GenericGetAttr(self, name);
        }
    }
    bool found = false;
    PyObject* value = GetChain(*isolate, AsInstance(self), key, !IsDunder(name), &found);
    Py_DECREF(key);
    if (value == nullptr) {
        return nullptr;
    }
    if (found) {
        if (IsNativeFunction(*isolate, value)) {
            PyObject* bound = BindFunction(*isolate, value, self);
            Py_DECREF(value);
            return bound;
        }
        return value;
    }
    Py_DECREF(value);
    // A mirror of a prototype method, kept on the class for `super()`, is not
    // a member of its own: the prototype chain just said the name is absent.
    if (MirroredOnly(*isolate, Py_TYPE(self), name)) {
        PyErr_Format(PyExc_AttributeError, "'%.100s' object has no attribute '%U'", Py_TYPE(self)->tp_name, name);
        return nullptr;
    }
    return PyObject_GenericGetAttr(self, name);
}

/// Report a write, a delete, that the object refused. From Python there is no
/// sloppy mode to drop it silently in, so it is the TypeError strict mode
/// JavaScript throws.
void RaiseRefusedSet(PyObject* key, bool getterOnly) noexcept {
    const std::string text = KeyText(key);
    if (getterOnly) {
        PyErr_Format(PyExc_TypeError, "Cannot set property %s of object which has only a getter", text.c_str());
    } else {
        PyErr_Format(PyExc_TypeError, "Cannot assign to read only property '%s' of object", text.c_str());
    }
}

void RaiseRefusedDelete(PyObject* key) noexcept {
    PyErr_Format(PyExc_TypeError, "Cannot delete property '%s' of object", KeyText(key).c_str());
}

/// `o.name = value`, `del o.name`, and their subscript forms. `missing` is the
/// exception a delete of an absent key raises: Python's, AttributeError or
/// KeyError, where JavaScript's `delete` would answer true - the one place
/// the Python spelling keeps Python's meaning, because `del` of something that
/// is not there is a mistake in Python code and nothing else says so.
int StoreOrDelete(PyObject* self, PyObject* rawKey, PyObject* value, PyObject* missing) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return -1;
    }
    PyObject* key = NormalizeKey(*isolate, rawKey);
    if (key == nullptr) {
        return -1;
    }
    const bool hooks = !(missing == PyExc_AttributeError && IsDunder(rawKey));
    int status = 0;
    if (value == nullptr) {
        switch (DeleteOwn(*isolate, AsInstance(self), key, hooks)) {
            case Outcome::Done:
                break;
            case Outcome::Absent:
                PyErr_SetObject(missing, rawKey);
                status = -1;
                break;
            case Outcome::Refused:
                RaiseRefusedDelete(key);
                status = -1;
                break;
            case Outcome::Failed:
                status = -1;
                break;
        }
    } else {
        bool getterOnly = false;
        switch (SetChain(*isolate, AsInstance(self), key, value, hooks, &getterOnly)) {
            case Outcome::Done:
            case Outcome::Absent:
                break;
            case Outcome::Refused:
                RaiseRefusedSet(key, getterOnly);
                status = -1;
                break;
            case Outcome::Failed:
                status = -1;
                break;
        }
    }
    Py_DECREF(key);
    return status;
}

int ObjectSetAttr(PyObject* self, PyObject* name, PyObject* value) {
    if (PyUnicode_Check(name)) {
        PyObject* descriptor = _PyType_Lookup(Py_TYPE(self), name);  // borrowed
        if (descriptor != nullptr && Py_TYPE(descriptor)->tp_descr_set != nullptr) {
            return PyObject_GenericSetAttr(self, name, value);
        }
    }
    return StoreOrDelete(self, name, value, PyExc_AttributeError);
}

/// `o[key]`: the JavaScript lookup under any key - a str, an int, a Symbol -
/// and KeyError for one that is not there, as a Python mapping answers. What
/// comes back is the value as stored: a native function is not bound, as
/// `o.__dict__["m"]` is not.
PyObject* ObjectGetItem(PyObject* self, PyObject* rawKey) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    PyObject* key = NormalizeKey(*isolate, rawKey);
    if (key == nullptr) {
        return nullptr;
    }
    bool found = false;
    PyObject* value = GetChain(*isolate, AsInstance(self), key, true, &found);
    Py_DECREF(key);
    if (value != nullptr && !found) {
        Py_DECREF(value);
        PyErr_SetObject(PyExc_KeyError, rawKey);
        return nullptr;
    }
    return value;
}

int ObjectSetItem(PyObject* self, PyObject* key, PyObject* value) {
    return StoreOrDelete(self, key, value, PyExc_KeyError);
}

/// `key in o`: JavaScript's `in`, own or inherited.
int ObjectContains(PyObject* self, PyObject* rawKey) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return -1;
    }
    PyObject* key = NormalizeKey(*isolate, rawKey);
    if (key == nullptr) {
        return -1;
    }
    const int has = HasChain(*isolate, AsInstance(self), key, true);
    Py_DECREF(key);
    return has;
}

/// `len(o)`: how many enumerable own keys it has - `Object.keys(o).length`,
/// which is what iterating it visits.
Py_ssize_t ObjectLength(PyObject* self) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return -1;
    }
    PyObject* keys = OwnKeys(*isolate, AsInstance(self), KeyFilter{}, true);
    if (keys == nullptr) {
        return -1;
    }
    const Py_ssize_t length = PyList_GET_SIZE(keys);
    Py_DECREF(keys);
    return length;
}

/// Always true, as every object is in JavaScript. Without this an object with
/// no enumerable keys would be falsy through `len`, and `if o:` would mean
/// something no JavaScript reader expects.
int ObjectBool(PyObject* /*self*/) {
    return 1;
}

/// `iter(o)`.
///
/// An object with a `Symbol.iterator` method - one a template or a class
/// declared, stored under `__iter__` - is iterated by calling it, with the
/// object as its receiver, and adapting whatever comes back: a Python
/// iterator as itself, a JavaScript-style iterator object (`next()` answering
/// `{done, value}`) through an adapter, anything else iterable through
/// `iter()`. See `AdaptIterator`.
///
/// Any other object iterates over its enumerable own keys, as a dict does.
PyObject* ObjectIter(PyObject* self) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    PyObject* key = PyUnicode_InternFromString("__iter__");
    if (key == nullptr) {
        return nullptr;
    }
    bool found = false;
    PyObject* method = GetChain(*isolate, AsInstance(self), key, true, &found);
    Py_DECREF(key);
    if (method == nullptr) {
        return nullptr;
    }
    if (found && PyCallable_Check(method) != 0) {
        PyObject* produced = CallWithReceiver(*isolate, method, self, nullptr, 0);
        Py_DECREF(method);
        if (produced == nullptr) {
            return nullptr;
        }
        return AdaptIterator(*isolate, produced);
    }
    Py_DECREF(method);
    PyObject* keys = OwnKeys(*isolate, AsInstance(self), KeyFilter{}, true);
    if (keys == nullptr) {
        return nullptr;
    }
    PyObject* iterator = PyObject_GetIter(keys);
    Py_DECREF(keys);
    return iterator;
}

/// A key as an object literal would spell it: a bare identifier, a number, a
/// quoted string, a `[Symbol(...)]`.
[[nodiscard]] PyObject* LiteralKey(Isolate& isolate, PyObject* key) noexcept {
    if (PyLong_Check(key)) {
        return PyObject_Str(key);
    }
    if (PyObject* symbol = WellKnownFor(isolate, key); symbol != nullptr || IsSymbol(isolate, key)) {
        return PyUnicode_FromFormat("[%R]", symbol != nullptr ? symbol : key);
    }
    if (PyUnicode_Check(key) && PyUnicode_IsIdentifier(key) == 1) {
        return Py_NewRef(key);
    }
    return PyObject_Repr(key);
}

/// `{x: 1, y: 'two'}`, and `Widget {name: 'w'}` for an instance of a template's
/// type - the shape a JavaScript console prints. Enumerable own properties
/// only, accessors as `[Getter/Setter]` rather than run: a repr that ran native
/// code would make a debugger's variable view do things.
PyObject* ObjectRepr(PyObject* self) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    const int entered = Py_ReprEnter(self);
    if (entered != 0) {
        return entered > 0 ? PyUnicode_FromString("{...}") : nullptr;
    }
    ObjectInstance* object = AsInstance(self);
    PyObject* keys = OwnKeys(*isolate, object, KeyFilter{.includeSymbols = true}, false);
    PyObject* parts = PyList_New(0);
    bool ok = keys != nullptr && parts != nullptr;
    for (Py_ssize_t i = 0; ok && i < PyList_GET_SIZE(keys); ++i) {
        PyObject* listed = PyList_GET_ITEM(keys, i);
        // Symbols come back as the symbol; the table keys a well-known one
        // by its dunder.
        PyObject* key = NormalizeKey(*isolate, listed);
        PyObject* value = nullptr;
        long attributes = 0;
        const int own = key != nullptr ? OwnLookup(object, key, &value, &attributes) : -1;
        PyObject* shown = nullptr;
        if (own > 0) {
            if ((attributes & ATTR_ACCESSOR) != 0) {
                const CallbackRecord* record = RecordOf(value);
                const bool get = record != nullptr && record->getter != nullptr;
                const bool set = record != nullptr && record->setter != nullptr;
                const char* label = "[Setter]";
                if (get && set) {
                    label = "[Getter/Setter]";
                } else if (get) {
                    label = "[Getter]";
                }
                shown = PyUnicode_FromString(label);
            } else {
                shown = PyObject_Repr(value);
            }
        }
        PyObject* name = own > 0 && shown != nullptr ? LiteralKey(*isolate, listed) : nullptr;
        PyObject* part = name != nullptr ? PyUnicode_FromFormat("%U: %U", name, shown) : nullptr;
        ok = own == 0 || (part != nullptr && PyList_Append(parts, part) == 0);
        Py_XDECREF(part);
        Py_XDECREF(name);
        Py_XDECREF(shown);
        Py_XDECREF(value);
        Py_XDECREF(key);
    }
    PyObject* result = nullptr;
    if (ok) {
        PyObject* separator = PyUnicode_FromString(", ");
        PyObject* body = separator != nullptr ? PyUnicode_Join(separator, parts) : nullptr;
        Py_XDECREF(separator);
        if (body != nullptr) {
            if (Py_TYPE(self) == isolate->impl().types.object) {
                result = PyUnicode_FromFormat("{%U}", body);
            } else {
                result = PyUnicode_FromFormat("%s {%U}", _PyType_Name(Py_TYPE(self)), body);
            }
            Py_DECREF(body);
        }
    }
    Py_XDECREF(keys);
    Py_XDECREF(parts);
    Py_ReprLeave(self);
    return result;
}

/// `dir(o)`: what `object.__dir__` finds, plus every string key along the
/// JavaScript chain and those an enumerator claims.
PyObject* ObjectDir(PyObject* self, PyObject* /*unused*/) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    PyObject* names = PyObject_CallMethod(reinterpret_cast<PyObject*>(&PyBaseObject_Type), "__dir__", "O", self);
    if (names == nullptr) {
        return nullptr;
    }
    PyObject* holder = Py_NewRef(self);
    bool first = true;
    for (int depth = 0; holder != nullptr && depth < MAX_CHAIN; ++depth) {
        PyObject* keys = OwnKeys(*isolate, AsInstance(holder), KeyFilter{.includeNonEnumerable = true}, first);
        first = false;
        bool ok = keys != nullptr;
        for (Py_ssize_t i = 0; ok && i < PyList_GET_SIZE(keys); ++i) {
            PyObject* key = PyList_GET_ITEM(keys, i);
            if (PyUnicode_Check(key)) {
                ok = PyList_Append(names, key) == 0;
            }
        }
        Py_XDECREF(keys);
        if (!ok) {
            Py_DECREF(holder);
            Py_DECREF(names);
            return nullptr;
        }
        PyObject* next = Py_XNewRef(AsInstance(holder)->prototype);
        Py_DECREF(holder);
        holder = next;
    }
    Py_XDECREF(holder);
    return names;
}

PyObject* ObjectGetProto(PyObject* self, void* /*closure*/) {
    PyObject* prototype = AsInstance(self)->prototype;
    return Py_NewRef(prototype != nullptr ? prototype : Py_None);
}

/// Set a [[Prototype]], refusing a cycle as the language does. `prototype` is
/// null for "none".
[[nodiscard]] bool AssignPrototype(Isolate& isolate, ObjectInstance* object, PyObject* prototype) noexcept {
    for (PyObject* walk = prototype; walk != nullptr; walk = AsInstance(walk)->prototype) {
        if (walk == reinterpret_cast<PyObject*>(object)) {
            PyErr_SetString(PyExc_TypeError, "Cyclic __proto__ value");
            return false;
        }
    }
    (void)isolate;
    Py_XSETREF(object->prototype, Py_XNewRef(prototype));
    return true;
}

/// `o.__proto__ = p`: a `unibind.Object`, or None (or `unibind.null`) for none.
int ObjectSetProto(PyObject* self, PyObject* value, void* /*closure*/) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return -1;
    }
    if (value == nullptr || value == Py_None || IsNull(*isolate, value)) {
        return AssignPrototype(*isolate, AsInstance(self), nullptr) ? 0 : -1;
    }
    if (!IsObjectInstance(*isolate, value)) {
        PyErr_SetString(PyExc_TypeError, "a prototype is a unibind.Object or None");
        return -1;
    }
    return AssignPrototype(*isolate, AsInstance(self), value) ? 0 : -1;
}

/// `Type(*args)`. For a template's type this is `new`: the construct path in
/// bindings.cpp. For `unibind.Object` itself, and a Python subclass of it,
/// an empty object that `__init__` then fills.
PyObject* ObjectNew(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return nullptr;
    }
    if (TemplateRec* tpl = TemplateOfType(*isolate, type); tpl != nullptr) {
        if (kwds != nullptr && PyDict_GET_SIZE(kwds) > 0) {
            PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", _PyType_Name(type));
            return nullptr;
        }
        return ConstructTemplate(*isolate, type, tpl, &PyTuple_GET_ITEM(args, 0),
                                 static_cast<std::size_t>(PyTuple_GET_SIZE(args)));
    }
    return reinterpret_cast<PyObject*>(NewObjectInstance(*isolate, type, nullptr));
}

/// `unibind.Object(mapping_or_pairs=None, **properties)`: an object with those
/// properties, set in order, as an object literal would make it. A template's
/// type takes its arguments in `ObjectNew`, and nothing here.
int ObjectInit(PyObject* self, PyObject* args, PyObject* kwds) {
    Isolate* isolate = IsolateOrRaise();
    if (isolate == nullptr) {
        return -1;
    }
    if (TemplateOfType(*isolate, Py_TYPE(self)) != nullptr) {
        return 0;
    }
    PyObject* source = nullptr;
    if (PyArg_UnpackTuple(args, "Object", 0, 1, &source) == 0) {
        return -1;
    }
    PyObject* items = PyDict_New();
    if (items == nullptr) {
        return -1;
    }
    int status = 0;
    if (source != nullptr && source != Py_None) {
        if (IsObjectInstance(*isolate, source)) {
            // Another object's enumerable own properties, read as `Get` reads
            // them: an accessor's getter runs, and the copy holds its value.
            PyObject* keys = OwnKeys(*isolate, AsInstance(source), KeyFilter{}, true);
            status = keys == nullptr ? -1 : 0;
            for (Py_ssize_t i = 0; status == 0 && i < PyList_GET_SIZE(keys); ++i) {
                PyObject* key = PyList_GET_ITEM(keys, i);
                bool found = false;
                PyObject* value = GetChain(*isolate, AsInstance(source), key, true, &found);
                status = value == nullptr || PyDict_SetItem(items, key, value) != 0 ? -1 : 0;
                Py_XDECREF(value);
            }
            Py_XDECREF(keys);
        } else if (PyDict_Check(source) || PyObject_HasAttrString(source, "keys") != 0) {
            status = PyDict_Merge(items, source, 1);
        } else {
            status = PyDict_MergeFromSeq2(items, source, 1);
        }
    }
    if (status == 0 && kwds != nullptr) {
        status = PyDict_Merge(items, kwds, 1);
    }
    Py_ssize_t position = 0;
    PyObject* rawKey = nullptr;
    PyObject* value = nullptr;
    while (status == 0 && PyDict_Next(items, &position, &rawKey, &value) != 0) {
        status = StoreOrDelete(self, rawKey, value, PyExc_KeyError);
    }
    Py_DECREF(items);
    return status;
}

PyMethodDef objectMethods[] = {
    {"__dir__", &ObjectDir, METH_NOARGS, "Own and inherited property names, with Python's own."},
    {nullptr, nullptr, 0, nullptr},
};

PyGetSetDef objectGetSet[] = {
    {"__proto__", &ObjectGetProto, &ObjectSetProto,
     "The [[Prototype]]: another unibind.Object, or None. Lookup walks it.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMemberDef objectMembers[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(ObjectInstance, weaklist), Py_READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr},
};

PyType_Slot objectSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(&ObjectDealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(&ObjectTraverse)},
    {Py_tp_clear, reinterpret_cast<void*>(&ObjectClear)},
    {Py_tp_getattro, reinterpret_cast<void*>(&ObjectGetAttr)},
    {Py_tp_setattro, reinterpret_cast<void*>(&ObjectSetAttr)},
    {Py_tp_repr, reinterpret_cast<void*>(&ObjectRepr)},
    {Py_tp_iter, reinterpret_cast<void*>(&ObjectIter)},
    {Py_tp_new, reinterpret_cast<void*>(&ObjectNew)},
    {Py_tp_init, reinterpret_cast<void*>(&ObjectInit)},
    {Py_mp_subscript, reinterpret_cast<void*>(&ObjectGetItem)},
    {Py_mp_ass_subscript, reinterpret_cast<void*>(&ObjectSetItem)},
    {Py_mp_length, reinterpret_cast<void*>(&ObjectLength)},
    {Py_sq_contains, reinterpret_cast<void*>(&ObjectContains)},
    {Py_nb_bool, reinterpret_cast<void*>(&ObjectBool)},
    {Py_tp_methods, objectMethods},
    {Py_tp_getset, objectGetSet},
    {Py_tp_members, objectMembers},
    {Py_tp_doc, const_cast<char*>("A JavaScript-shaped object: ub::Object. Ordered own properties with attributes, "
                                  "accessors, and a [[Prototype]] chain.\n\n"
                                  "Object(mapping_or_pairs=None, **properties)")},
    {0, nullptr},
};

PyType_Spec objectSpec = {
    "unibind.Object", sizeof(ObjectInstance), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    objectSlots,
};

}  // namespace

// ---------------------------------------------------------------------------
// Shared with bindings.cpp
// ---------------------------------------------------------------------------

bool IsObjectInstance(Isolate& isolate, PyObject* value) noexcept {
    PyTypeObject* type = isolate.impl().types.object;
    return type != nullptr && PyObject_TypeCheck(value, type);
}

ObjectInstance* NewObjectInstance(Isolate& isolate, PyTypeObject* type, PyObject* prototype) noexcept {
    (void)isolate;
    PyObject* raw = type->tp_alloc(type, 0);
    if (raw == nullptr) {
        return nullptr;
    }
    ObjectInstance* object = AsInstance(raw);
    object->properties = PyDict_New();
    if (object->properties == nullptr) {
        Py_DECREF(raw);
        return nullptr;
    }
    object->prototype = Py_XNewRef(prototype);
    return object;
}

bool DefineRaw(ObjectInstance* object, PyObject* key, PyObject* value, long attributes) noexcept {
    return PyDict_SetItem(object->properties, key, value) == 0 && SetAttributes(object, key, attributes);
}

bool DefineAccessorRaw(ObjectInstance* object, PyObject* key, CallbackRecord* record,
                       PropertyAttribute attributes) noexcept {
    // An accessor has no [[Writable]]: a getter with no setter is the
    // read-only form, and ReadOnly is dropped, as both other backends drop it.
    const long bits =
        (static_cast<long>(attributes) & ~static_cast<long>(PropertyAttribute::ReadOnly) & ATTRIBUTE_MASK) |
        ATTR_ACCESSOR;
    PyObject* capsule = PyCapsule_New(record, ACCESSOR_CAPSULE, nullptr);
    if (capsule == nullptr) {
        return false;
    }
    const bool ok = DefineRaw(object, key, capsule, bits);
    Py_DECREF(capsule);
    return ok;
}

PyObject* GetAny(Isolate& isolate, PyObject* object, PyObject* key, bool* found) noexcept {
    *found = false;
    if (IsObjectInstance(isolate, object)) {
        return GetChain(isolate, AsInstance(object), key, true, found);
    }
    if (PyDict_CheckExact(object)) {
        PyObject* value = PyDict_GetItemWithError(object, key);  // borrowed
        if (value != nullptr) {
            *found = true;
            return Py_NewRef(value);
        }
        return PyErr_Occurred() != nullptr ? nullptr : Py_NewRef(Py_None);
    }
    if (PyDict_Check(object)) {
        // A dict subclass: through its own `__getitem__`, which may have a
        // `__missing__` or anything else a subclass is written for.
        PyObject* value = PyObject_GetItem(object, key);
        if (value != nullptr) {
            *found = true;
            return value;
        }
        if (PyErr_ExceptionMatches(PyExc_KeyError) != 0) {
            PyErr_Clear();
            return Py_NewRef(Py_None);
        }
        return nullptr;
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        Py_ssize_t position = 0;
        if (AsPosition(key, &position)) {
            if (position < PySequence_Size(object)) {
                *found = true;
                return PySequence_GetItem(object, position);
            }
            return Py_NewRef(Py_None);
        }
        if (IsLengthKey(key)) {
            *found = true;
            return PyLong_FromSsize_t(PySequence_Size(object));
        }
    }
    PyObject* name = AttributeName(isolate, key);
    if (name == nullptr) {
        return PyErr_Occurred() != nullptr ? nullptr : Py_NewRef(Py_None);
    }
    PyObject* value = PyObject_GetAttr(object, name);
    Py_DECREF(name);
    if (value != nullptr) {
        *found = true;
        return value;
    }
    if (PyErr_ExceptionMatches(PyExc_AttributeError) != 0) {
        PyErr_Clear();
        return Py_NewRef(Py_None);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The backend interface
// ---------------------------------------------------------------------------

std::optional<Slot> MakeObject(const Context& context) {
    Isolate& isolate = OwnerOf(context);
    PyTypeObject* type = isolate.impl().types.object;
    return PushOrNothing(isolate, reinterpret_cast<PyObject*>(NewObjectInstance(isolate, type, nullptr)));
}

std::optional<Slot> MakeArray(const Context& context, std::uint32_t length) {
    // A list of `None`, which is what a hole reads as - a Python list has no
    // holes. See MAX_DENSE_LENGTH for why a length past it is refused rather
    // than attempted.
    Isolate& isolate = OwnerOf(context);
    if (!RangeCheck(isolate, length)) {
        return std::nullopt;
    }
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(length));
    if (list == nullptr) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), Py_NewRef(Py_None));
    }
    return PushOrNothing(isolate, list);
}

std::optional<Slot> GetProperty(const Context& context, Slot object, Slot key) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    bool found = false;
    PyObject* value = GetAny(op.isolate(), Resolve(object), normalized, &found);
    Py_DECREF(normalized);
    return PushOrNothing(op.isolate(), value);
}

std::optional<Slot> GetIndex(const Context& context, Slot object, std::uint32_t index) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* key = op.Index(index);
    if (key == nullptr) {
        return std::nullopt;
    }
    bool found = false;
    PyObject* value = GetAny(op.isolate(), Resolve(object), key, &found);
    Py_DECREF(key);
    return PushOrNothing(op.isolate(), value);
}

namespace {

/// `Set`'s answer. A write a `unibind.Object` refused - a read-only property,
/// a getter with no setter - is true, as V8's `Object::Set` answers it in the
/// sloppy mode it runs in: the write is dropped, not reported. A tuple, which
/// cannot take the write at all, is false.
[[nodiscard]] std::optional<bool> SetAnswer(Isolate& isolate, PyObject* object, Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::Done:
        case Outcome::Absent:
            return true;
        case Outcome::Refused:
            return IsObjectInstance(isolate, object);
        case Outcome::Failed:
            break;
    }
    return std::nullopt;
}

}  // namespace

std::optional<bool> SetProperty(const Context& context, Slot object, Slot key, Slot value) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    const Outcome outcome = SetAny(op.isolate(), target, normalized, Resolve(value));
    Py_DECREF(normalized);
    return SetAnswer(op.isolate(), target, outcome);
}

std::optional<bool> SetIndex(const Context& context, Slot object, std::uint32_t index, Slot value) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* key = op.Index(index);
    if (key == nullptr) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    const Outcome outcome = SetAny(op.isolate(), target, key, Resolve(value));
    Py_DECREF(key);
    return SetAnswer(op.isolate(), target, outcome);
}

std::optional<bool> DefineProperty(const Context& context, Slot object, Slot key, Slot value,
                                   PropertyAttribute attributes) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    std::optional<bool> answer;
    if (IsObjectInstance(op.isolate(), target)) {
        const int defined = DefineChecked(AsInstance(target), normalized, Resolve(value),
                                          static_cast<long>(attributes) & ATTRIBUTE_MASK);
        if (defined >= 0) {
            answer = defined > 0;
        }
    } else if (attributes != PropertyAttribute::None) {
        // Nothing else can keep an attribute: a dict item and an attribute are
        // writable, enumerable and deletable whatever was asked. Refusing says
        // so; storing the value and dropping the attributes would report a
        // read-only property that is not one.
        answer = false;
    } else {
        // Define bypasses setters - for an ordinary object, `__dict__` is
        // where that would go, but a `property` or `__slots__` may own the
        // name, so this is the attribute write and whatever it does.
        const Outcome outcome = SetAny(op.isolate(), target, normalized, Resolve(value));
        if (outcome != Outcome::Failed) {
            answer = outcome == Outcome::Done;
        }
    }
    Py_DECREF(normalized);
    return answer;
}

std::optional<bool> SetAccessorProperty(const Context& context, Slot object, std::string_view name,
                                        AccessorGetterCallback getter, AccessorSetterCallback setter, CallbackData data,
                                        PropertyAttribute attributes) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    if (!IsObjectInstance(op.isolate(), target)) {
        // An accessor is something only a `unibind.Object` can hold: a dict
        // item or an attribute cannot run code on read. Refused, not faked.
        return false;
    }
    PyObject* text = TextString(name);
    PyObject* key = text != nullptr ? NormalizeKey(op.isolate(), text) : nullptr;
    Py_XDECREF(text);
    if (key == nullptr) {
        return std::nullopt;
    }
    ObjectInstance* instance = AsInstance(target);
    const long current = AttributesOf(instance, key);
    std::optional<bool> answer;
    if ((current & static_cast<long>(PropertyAttribute::DontDelete)) != 0 &&
        PyDict_Contains(instance->properties, key) == 1) {
        answer = false;  // a permanent property is not replaced
    } else if (CallbackRecord* record = StoreAccessorRecord(op.isolate(), getter, setter, data); record != nullptr) {
        if (DefineAccessorRaw(instance, key, record, attributes)) {
            answer = true;
        }
    }
    Py_DECREF(key);
    return answer;
}

std::optional<bool> HasProperty(const Context& context, Slot object, Slot key) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    const int has = HasAny(op.isolate(), Resolve(object), normalized);
    Py_DECREF(normalized);
    if (has < 0) {
        return std::nullopt;
    }
    return has > 0;
}

std::optional<bool> HasOwnProperty(const Context& context, Slot object, Slot key) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    const int has = HasOwnAny(op.isolate(), Resolve(object), normalized);
    Py_DECREF(normalized);
    if (has < 0) {
        return std::nullopt;
    }
    return has > 0;
}

std::optional<bool> DeleteProperty(const Context& context, Slot object, Slot key) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    std::optional<bool> answer;
    if (IsObjectInstance(op.isolate(), target)) {
        switch (DeleteOwn(op.isolate(), AsInstance(target), normalized, true)) {
            case Outcome::Done:
            case Outcome::Absent:
                answer = true;  // deleting what is not there succeeds, as in the language
                break;
            case Outcome::Refused:
                answer = false;
                break;
            case Outcome::Failed:
                break;
        }
    } else if (PyDict_Check(target)) {
        if (PyObject_DelItem(target, normalized) == 0) {
            answer = true;
        } else if (PyErr_ExceptionMatches(PyExc_KeyError) != 0) {
            PyErr_Clear();
            answer = true;
        }
    } else if (Py_ssize_t position = 0; (PyList_Check(target) || PyTuple_Check(target)) &&
                                        (AsPosition(normalized, &position) || IsLengthKey(normalized))) {
        if (IsLengthKey(normalized) || PyTuple_Check(target)) {
            answer = false;  // not configurable, and a tuple is frozen
        } else if (position >= PyList_GET_SIZE(target)) {
            answer = true;
        } else {
            // `delete a[i]` leaves a hole and does not shift the rest: here,
            // `None` in its place.
            answer =
                PyList_SetItem(target, position, Py_NewRef(Py_None)) == 0 ? std::optional<bool>(true) : std::nullopt;
        }
    } else if (PyObject* name = AttributeName(op.isolate(), normalized); name != nullptr) {
        if (PyObject_DelAttr(target, name) == 0) {
            answer = true;
        } else if (PyErr_ExceptionMatches(PyExc_AttributeError) != 0) {
            PyErr_Clear();
            answer = true;
        } else if (PyErr_ExceptionMatches(PyExc_TypeError) != 0) {
            PyErr_Clear();  // a read-only attribute: the delete is refused
            answer = false;
        }
        Py_DECREF(name);
    } else if (PyErr_Occurred() == nullptr) {
        answer = true;  // a symbol names no attribute; nothing to delete
    }
    Py_DECREF(normalized);
    return answer;
}

std::optional<PropertyAttribute> GetPropertyAttributes(const Context& context, Slot object, Slot key) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* normalized = op.Key(key);
    if (normalized == nullptr) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    std::optional<PropertyAttribute> answer;
    if (IsObjectInstance(op.isolate(), target)) {
        PropertyAttribute attributes = PropertyAttribute::None;
        if (AttributesChain(op.isolate(), AsInstance(target), normalized, &attributes) > 0) {
            answer = attributes;
        }
    } else {
        // Whatever else it is, it has no attributes to keep - but an absent
        // property still answers empty rather than `None`, which would claim
        // an ordinary property that is not there (docs/status.md).
        const int has = HasAny(op.isolate(), target, normalized);
        if (has > 0) {
            const bool hidden = !PyDict_Check(target) && !EnumerableName(normalized);
            answer = hidden ? PropertyAttribute::DontEnum : PropertyAttribute::None;
        }
    }
    Py_DECREF(normalized);
    return answer;
}

std::optional<Slot> GetOwnPropertyNames(const Context& context, Slot object, KeyFilter filter) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    PyObject* keys = IsObjectInstance(op.isolate(), target) ? OwnKeys(op.isolate(), AsInstance(target), filter, true)
                                                            : OwnKeysAny(op.isolate(), target, filter);
    return PushOrNothing(op.isolate(), keys);
}

std::optional<Slot> GetPrototype(const Context& context, Slot object) {
    // Only a `unibind.Object` has a [[Prototype]] in the language's sense;
    // everything else answers null, the prototype of an object that has none,
    // rather than its Python type, which is a different thing that lookup here
    // never walks.
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    PyObject* prototype = IsObjectInstance(op.isolate(), target) ? AsInstance(target)->prototype : nullptr;
    return PushBorrowed(op.isolate(), prototype != nullptr ? prototype : op.isolate().impl().types.nullValue);
}

std::optional<bool> SetPrototype(const Context& context, Slot object, Slot prototype) {
    const ApiOp op(context);
    if (!op.Open()) {
        return std::nullopt;
    }
    PyObject* target = Resolve(object);
    PyObject* value = Resolve(prototype);
    if (!IsObjectInstance(op.isolate(), target)) {
        PyErr_Format(PyExc_TypeError, "cannot set the prototype of a %s", Py_TYPE(target)->tp_name);
        return std::nullopt;
    }
    if (IsNull(op.isolate(), value)) {
        value = nullptr;
    } else if (!IsObjectInstance(op.isolate(), value)) {
        PyErr_SetString(PyExc_TypeError, "Object prototype may only be a unibind.Object or null");
        return std::nullopt;
    }
    if (!AssignPrototype(op.isolate(), AsInstance(target), value)) {
        return std::nullopt;
    }
    return true;
}

std::uint32_t ArrayLength(Slot array) noexcept {
    PyObject* value = Resolve(array);
    Py_ssize_t size = 0;
    if (PyList_Check(value)) {
        size = PyList_GET_SIZE(value);
    } else if (PyTuple_Check(value)) {
        size = PyTuple_GET_SIZE(value);
    }
    // Compared as unsigned 64-bit: on x86 `Py_ssize_t` is 32 bits, and a
    // 0xFFFFFFFF there is -1, which every length is larger than.
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(size), 0xFFFFFFFFULL));
}

NativeBox* GetNativeBox(Slot object) noexcept {
    Isolate& isolate = IsolateFor(object);
    PyObject* value = Resolve(object);
    Isolate::Impl& impl = isolate.impl();
    if (impl.nativesReleased || !IsObjectInstance(isolate, value)) {
        return nullptr;
    }
    NativeBox* box = AsInstance(value)->box;
    // While `~Isolate` gives the natives back, one may already be gone while
    // its instance lives on: only a box still waiting its turn is real.
    if (box != nullptr && impl.nativesTearingDown && !impl.liveNatives.contains(box)) {
        return nullptr;
    }
    return box;
}

bool InitObjectTypes(Isolate& isolate, PyObject* module) noexcept {
    Types& types = isolate.impl().types;
    types.object = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &objectSpec, nullptr));
    if (types.object == nullptr) {
        return false;
    }
    return PyModule_AddObjectRef(module, "Object", reinterpret_cast<PyObject*>(types.object)) == 0;
}

}  // namespace ub::detail
