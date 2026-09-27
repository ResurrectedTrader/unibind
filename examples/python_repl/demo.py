# The unibind Python REPL demo: every binding in host.cpp, exercised from Python.
#
#     unibind_python_repl --demo
#
# Everything named here - host, Counter, Vec2, Shape, Circle, Rect - is C++,
# registered through the ub:: API before this file runs. Each section prints
# what it does and asserts what it expects, so this file is also a smoke test
# (CTest: example.python_repl.demo). A failed assert is a traceback and exit
# code 1.
#
# It uses top-level `await`, so the whole file evaluates to a promise - an
# asyncio Task - that the program settles by pumping the isolate's jobs.

import asyncio
import unibind


def section(title):
    print()
    print(f"== {title} ==")


# ---------------------------------------------------------------------------
section("host: an ObjectTemplate of constants and native functions")
# ---------------------------------------------------------------------------

print("host.version =", host.version)            # a constant, declared ReadOnly
print("host.backend =", host.backend)
assert host.backend == "python"
try:
    host.version = "2.0"
except TypeError as error:
    print("host.version is read-only:", error)
else:
    raise AssertionError("a ReadOnly constant took a write")

host.log("host.log prints", 1, 2.5, [3], None)   # str() of each, from C++
t0 = host.now()
assert isinstance(t0, float) and t0 >= 0

# A native that throws: an ordinary Python exception, catchable as one.
try:
    host.throw_type_error("thrown from C++")
except TypeError as error:
    print("caught:", repr(error))
    assert str(error) == "thrown from C++"


# ---------------------------------------------------------------------------
section("Function::New with a script value as its data")
# ---------------------------------------------------------------------------

hello = host.make_greeter("Hello")    # one native callback ...
howdy = host.make_greeter("Howdy")    # ... two functions, each with its own data
print(hello("world"), "/", howdy("partner"))
assert hello("world") == "Hello, world!"
assert howdy() == "Howdy, world!"


# ---------------------------------------------------------------------------
section("native calling back into Python (Function::Call)")
# ---------------------------------------------------------------------------

squares = host.map([1, 2, 3, 4], lambda x: x * x)
print("host.map([1, 2, 3, 4], square) =", squares)
assert squares == [1, 4, 9, 16]
assert host.map(["a", "b"], str.upper) == ["A", "B"]

# An exception raised by the callback passes back out through the native.
def boom(x):
    raise KeyError(x)

try:
    host.map([7], boom)
except KeyError as error:
    print("the callback's KeyError came through the native:", error)


# ---------------------------------------------------------------------------
section("Counter: a Class<T> with methods, accessors, statics and iteration")
# ---------------------------------------------------------------------------

c = Counter(10, 5)                       # Construct<&NewCounter>
print("Counter(10, 5).increment() =", c.increment())
assert c.value == 15
c.increment(100)                         # an explicit step
c.value = 3                              # an accessor with a setter
assert c.value == 3
print("iterating a Counter at 3:", list(c))   # its Symbol.iterator method
assert list(c) == [0, 1, 2]
assert [x * 2 for x in Counter(4)] == [0, 2, 4, 6]
c.reset()
assert c.value == 0

try:
    c.step = 0                           # the setter validates
except ValueError as error:              # a RangeError is a ValueError
    print("step = 0 refused:", type(error).__name__, error)
try:
    Counter(1, -1)                       # so does the constructor
except ValueError:
    pass
else:
    raise AssertionError("a negative step was accepted")

print("Counter.MAX_STEP =", Counter.MAX_STEP, "/ alive now:", Counter.alive())
before = Counter.alive()
temporary = [Counter() for _ in range(3)]
assert Counter.alive() == before + 3
del temporary                            # the natives go with their wrappers
assert Counter.alive() == before

# A Python subclass of a native class keeps the native, and adds to it.
class LabelledCounter(Counter):
    def bump_twice(self):
        self.increment()
        return self.increment()

    @property
    def label(self):
        return f"counter at {self.value}"

labelled = LabelledCounter(0, 2)
print("LabelledCounter(0, 2).bump_twice() =", labelled.bump_twice(), "/", labelled.label)
assert labelled.value == 4 and isinstance(labelled, Counter)


# ---------------------------------------------------------------------------
section("shared ownership: Class<T>::Wrap over a native C++ also holds")
# ---------------------------------------------------------------------------

a = host.shared_counter("hits")          # two wrappers ...
b = host.shared_counter("hits")          # ... over one native
a.increment()
a.increment()
print("a.value =", a.value, "/ b.value =", b.value)
assert b.value == 2
print("host.registry() =", host.registry())
assert host.registry()["hits"]["owners"] == 3   # the registry, a and b
del a, b
assert host.registry()["hits"]["owners"] == 1   # the registry alone
assert host.shared_counter("hits").value == 2   # and the native is still there


# ---------------------------------------------------------------------------
section("Vec2: methods that make new instances, and checked unwrapping")
# ---------------------------------------------------------------------------

v = Vec2(3, 4)
w = v.add(Vec2(1, -1)).scale(2)
print(v.describe(), "+ Vec2(1, -1), scaled by 2 =", w.describe())
assert (w.x, w.y) == (8, 6)
assert v.length() == 5
assert v.dot(Vec2(1, 0)) == 3
assert v.sub(v).length() == 0
try:
    v.add("not a vector")                # Class<Vec2>::Unwrap answers null
except TypeError as error:
    print("v.add('not a vector'):", error)


# ---------------------------------------------------------------------------
section("Shape, Circle, Rect: FunctionTemplates with Inherit and HasInstance")
# ---------------------------------------------------------------------------

shapes = [Circle(1), Rect(2, 3), Shape()]
for shape in shapes:
    print(" ", shape.describe(), "- claimed by", host.kind_of(shape))
assert shapes[0].describe() == "circle with area 3.14"   # Shape's method, Circle's area()
assert host.kind_of(shapes[0]) == ["Circle", "Shape"]    # a Circle is a Shape too
assert host.kind_of(shapes[2]) == ["Shape"]
assert host.kind_of({"looks": "like a shape"}) == []
assert isinstance(shapes[1], Shape) and not isinstance(shapes[1], Circle)
assert Circle.__mro__[1] is Shape
total = sum(shape.area() for shape in shapes)
print(f"  total area: {total:.3f}")


# ---------------------------------------------------------------------------
section("host.env: a named interceptor over a C++ map")
# ---------------------------------------------------------------------------

env = host.env
print("keys:", list(env), "/ env.mode =", env.mode, "/ env['greeting'] =", env["greeting"])
assert env.mode == "demo"                # set by the program for --demo
env.color = "blue"                       # setter: stored in the C++ map, as str()
env["answer"] = 42
assert env.answer == "42"
assert "color" in env and "nothing" not in env   # query
assert "color" in dir(env)                       # enumerator
del env.color                                    # deleter
assert "color" not in env
try:
    del env.app                          # the deleter refuses a locked key
except TypeError as error:
    print("del env.app:", error)
try:
    env.app = "renamed"                  # and the setter refuses to write it
except TypeError as error:
    print("env.app = ...:", error)
print("len(env) =", len(env))


# ---------------------------------------------------------------------------
section("host.registers: an indexed interceptor")
# ---------------------------------------------------------------------------

registers = host.registers
registers[0] = 7
registers[3] = "12"                      # converted with ToInt32
print("registers:", [registers[i] for i in registers])
assert [registers[i] for i in registers] == [7, 0, 0, 12, 0, 0, 0, 0]
assert len(registers) == 8 and 3 in registers and 8 not in registers
del registers[0]                         # the deleter clears it
assert registers[0] == 0
try:
    registers[8] = 1
except ValueError as error:
    print("registers[8] = 1:", error)


# ---------------------------------------------------------------------------
section("host.session: Object::SetAccessor on a plain object")
# ---------------------------------------------------------------------------

print("host.session.prompt =", repr(host.session.prompt), "/ inputs =", host.session.inputs)
host.session.prompt = "demo> "           # the REPL would now prompt with this
assert host.session.prompt == "demo> "
host.session.prompt = ">>> "
try:
    host.session.inputs = 99             # a getter with no setter
except TypeError as error:
    print("inputs is read-only:", error)


# ---------------------------------------------------------------------------
section("binary data and structured clone")
# ---------------------------------------------------------------------------

data = host.bytes(8)                     # TypedArray::New from a C++ span
print(data, "checksum", host.checksum(data))
assert list(data) == list(range(8)) and host.checksum(data) == 28
utf8 = host.bytes("hé")
assert list(utf8) == [0x68, 0xC3, 0xA9]
assert host.checksum(unibind.TypedArray("uint16", [1, 256])) == 2   # bytes 01 00 00 01

original = {"name": "clone me", "items": [1, 2.5, None, unibind.null], "nested": {"t": (1, 2)}}
copy = host.clone(original)              # Serialize, then Deserialize
print("clone:", copy)
assert copy == original and copy is not original
assert copy["nested"] is not original["nested"]
try:
    host.clone({"f": print})             # functions do not clone
except unibind.DataCloneError as error:
    print("clone of a function:", error)


# ---------------------------------------------------------------------------
section("the stack, the heap, the collector")
# ---------------------------------------------------------------------------

def where_am_i():
    return host.stack()

frames = where_am_i()
print("host.stack() from where_am_i():", frames[0])
assert frames[0]["function"] == "where_am_i" and frames[0]["script"] == "demo.py"

stats = host.heap()
print("host.heap():", stats)
assert type(stats) is dict and stats["used_bytes"] > 0
assert stats["used_bytes"] <= stats["limit_bytes"]
print("host.gc() ->", host.gc(), "bytes in use")


# ---------------------------------------------------------------------------
section("posted jobs: timers and promises, with top-level await")
# ---------------------------------------------------------------------------

fired = []
host.set_timeout(lambda: fired.append("late"), 0.05)
host.set_timeout(lambda: fired.append("soon"), 0.01)
cancelled = host.set_timeout(lambda: fired.append("never"), 0.02)
assert host.clear_timeout(cancelled) is True
assert fired == []                       # nothing runs before a pump

value = await host.fetch_later(42)       # resolved from a posted job
print("await host.fetch_later(42) ->", value)
assert value == 42

await host.fetch_later(None, 0.1)        # a promise that settles after 0.1 s
print("timers fired, in due order:", fired)
assert fired == ["soon", "late"]

try:
    await host.fail_later(ValueError("rejected by C++"))
except ValueError as error:
    print("await host.fail_later(...) raised:", repr(error))

# The host's promises are asyncio Futures, so asyncio composes them.
results = await asyncio.gather(host.fetch_later("a", 0.02), host.fetch_later("b"), asyncio.sleep(0.01, "c"))
print("asyncio.gather(...) ->", results)
assert results == ["a", "b", "c"]


# ---------------------------------------------------------------------------
section("a blocking native that polls for a stop")
# ---------------------------------------------------------------------------

# Nothing stops this one, so it sleeps the whole time. Under --timeout or
# Ctrl-C it would notice IsExecutionTerminating() and return False.
assert host.sleep(0.01) is True

print()
print("demo: all sections passed")
