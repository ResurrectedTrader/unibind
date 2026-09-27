# Run by the "script-that-raises" tests: an uncaught exception is a traceback
# on stderr and exit code 1.

def inner():
    raise ValueError("raised on purpose")

def outer():
    inner()

print("about to raise")
outer()
print("never printed")
