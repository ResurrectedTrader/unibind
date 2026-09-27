# Run as a file by the "example.python_repl.script" test:
#   unibind_python_repl script.py one two
import sys

print("argv:", sys.argv)
assert sys.argv[1:] == ["one", "two"]
assert host.env.mode == "script"

c = Counter()
for _ in range(3):
    c.increment()
print("counted to", c.value)

v = Vec2(3, 4)
print("vector length", int(v.length()))
