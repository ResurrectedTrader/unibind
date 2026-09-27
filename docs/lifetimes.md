# Handle lifetimes

The central design decision of `unibind`. Everything else in the API is downstream
of it, so it is written down before a single header.

## 1. The question

An embedding API has to hand the caller something that names a JavaScript
value. Call it a handle. The handle has to survive across calls into the
engine, be passed to other functions, be *returned* to a caller, and be stored
in an ordinary C++ variable — and while it is alive the garbage collector must
not free the value it names, nor leave the handle pointing at a stale address
if the collector moves things.

V8's answer is `Local<T>`: a pointer to a slot in a bump-allocated block owned
by the innermost `HandleScope`. Handles die wholesale when the scope closes;
`EscapableHandleScope::Escape` copies one slot into the parent scope so a
function can return a handle.

That answer is not portable. It assumes a scope you can allocate arbitrarily
many handles into, on demand, at a stable address. SpiderMonkey provides no
such thing.

The question this document answers: **what is the smallest handle model that
both engines can implement honestly, that still lets a function return a
handle to its caller?**

## 2. What each engine actually gives us

### V8 15.6

- `Local<T>` is a pointer to an `Address` slot inside the current
  `HandleScope`'s block. Copying a `Local` is copying a pointer; the value is
  reached with one load.
- `HandleScope` is a stack RAII object, but its storage is a pooled block list
  owned by the isolate. Allocating a handle is a bump of a pointer plus an
  occasional block allocation, amortized across scopes.
- Scopes nest LIFO. A `Local` is invalid after its scope closes.
- `EscapableHandleScope::Escape(Local<T>)` copies the value into a slot the
  scope reserved in its *parent* before returning.
- `Global<T>` / `Persistent<T>` is an off-stack root, movable, storable in
  containers, explicitly (or RAII-) reset.
- V8 also moves objects (scavenger, compacting major GC), but handles are
  indirections through a slot the collector rewrites, so a `Local` stays valid
  across a GC.

### SpiderMonkey 153

- The root type is `JS::Rooted<T>`, which is `MOZ_RAII`: it links itself onto
  the context's rooted list in its constructor and unlinks in its destructor.
  It is *stack only* and *strictly LIFO*. It cannot be moved, cannot be copied,
  cannot be put in a container, and cannot outlive its enclosing block. There
  is no allocate-N-of-these-on-demand facility.
- `JS::Handle<T>` is a borrowed, immutable reference to an existing root —
  it roots nothing itself.
- Values are passed and returned **by value** (`JS::Value`, 8 bytes,
  NaN-boxed). An API that returns a `JS::Value` has rooted nothing: it is the
  receiver's job to root it before the next thing that can GC.
- The collector **moves** objects (generational nursery plus compaction), so a
  `JS::Value` copied out of a root into an un-rooted local is not merely
  possibly-freed, it is possibly-*stale*. This is the harder half of the
  constraint: a leak you might get away with, a moved pointer you never do.
- `JS::RootedVector<T>` is `Rooted<StackGCVector<T>>`: one stack root that
  traces a growable vector of `T`. Verified in this build
  (`include/js/GCVector.h:382`), with an inline capacity of 8 elements
  (`StackGCVector : GCVector<T, 8>`, line 223) before it touches the heap.
  **This is the load-bearing fact of the whole design.**
- `JS::PersistentRooted<T>` is an off-stack root on a per-runtime linked list.
  Heap-allocatable, movable. The header warns it is a *root*: a cycle through
  one leaks unconditionally.

### The shape of a third engine

QuickJS and Duktape refcount instead of tracing, and never move. There, a
handle must own a reference and release it deterministically, and copying a
handle is a refcount bump. That is a *stricter* release discipline and a
*looser* address discipline than either engine here. Section 12 said what the
chosen model would cost such a backend, before one existed.

One does now, and it is CPython rather than a JavaScript engine: reference
counted, with a cycle collector on top, and objects that never move. Section 12
says what was built, and `docs/python.md` the rest.

## 3. The shared truth

Strip the two engines down to what is actually common:

1. A root is a thing the collector traces. Roots can be created and destroyed,
   in LIFO order, on the stack.
2. A root can hold **more than one value** — SpiderMonkey's `RootedVector`,
   V8's handle block — and the number can grow after the root exists.
3. Nothing portable lets you hand out a *stable address* of an individual
   rooted value. V8 does (its slots don't move), SpiderMonkey does not (the
   vector's buffer reallocates on growth, and `Rooted` itself may not be
   copied or relocated by us).
4. Copying the value out of a root into an unrooted place is always wrong.

Point 2 is the opening. Point 3 is what kills the V8 design.

## 4. The model

> **A handle is a (frame, index) pair into a stack-owned array of rooted
> slots. Scopes nest LIFO; escaping copies a slot into the parent frame; a
> value that must outlive all frames is moved into a separately rooted
> `Global<T>`.**

Concretely:

```cpp
class HandleScope {                 // stack-only, LIFO, opens a frame
    explicit HandleScope(Isolate&);
    ~HandleScope();
    void* operator new(std::size_t) = delete;   // enforced, not just documented
    alignas(...) std::byte storage_[UNIBIND_FRAME_STORAGE_SIZE];  // backend's frame, in place
};

template <class T>
class Local {                        // {detail::Frame*, uint32_t}: 8 bytes on
    detail::Frame* frame_;           // x86, 16 on x64 - see section 6
    detail::SlotIndex index_;
};

class EscapableHandleScope : public HandleScope {
    template <class T> Local<T> Escape(Local<T>);   // copy slot into parent frame
};

template <class T>
class Global {                       // one off-stack root; move-only; no scope needed
    Global(Isolate&, Local<T>);
    Local<T> Get(Isolate&) const;    // materialize into the current frame
};
```

A frame is a growable array of rooted slots whose *storage lives inside the
`HandleScope` object*, i.e. on the stack. The `HandleScope` is the root; the
slots are what it traces. `Local` names a slot by ordinal, so nothing depends
on the slot's address staying put.

The isolate keeps a pointer to the innermost open frame, so value-creating
functions (`String::New(isolate, ...)`) allocate into "the current frame", the
same way V8 allocates into the current `HandleScope`. That is why the API
takes an `Isolate&` and not a frame — frames are never named by callers.

### Why an index and not a pointer

A pointer to a slot is one load cheaper and is what V8 does natively. It is
also unimplementable on SpiderMonkey without giving up exact stack rooting:
the only stack root that holds a growable set of values is `RootedVector`,
whose buffer reallocates, so an address taken before an append dangles after
it. The alternatives are a chunked root list (N roots, one per chunk, which
`Rooted` cannot express off-stack) or `PersistentRooted` chunks (off-stack,
heap-allocated, list-linked — pays a malloc and gives up the stack discipline
the engine is built around).

An index costs one extra load and buys the exact-rooting implementation.

### Why the frame pointer is in the handle

A handle could be just an index into a single isolate-wide slot stack, with
frames as watermarks — 4 bytes instead of 8, and one fewer indirection. But an
isolate-wide growable rooted array is exactly the thing SpiderMonkey cannot
root from the stack: it would have to be a `PersistentRootedVector`, i.e. an
off-stack root, and the "exact, stack-ordered rooting" property is gone. The
frame pointer is the price of keeping every root on the stack.

### What escaping actually does

`Escape` moves the value out of the closing frame into the parent frame and
returns a handle to its new slot. Both frames are live roots at the moment of
the handover, so the value is never momentarily unrooted. That is not an
accident; it is the reason escape is a scope method and not a free function.

On SpiderMonkey this is a `JS::Value` copy between two `RootedVector`s and
costs nothing.

On V8 it is not, and this is the one place where writing the backend changed
the design rather than confirming it. A `v8::Local` is a *pointer into the
innermost open scope's block*, so copying one into the parent's slot array
copies a pointer into storage that is about to be released: the handle survives
the copy and the value does not. V8 hands a value across a scope boundary in
exactly one cheap way, `EscapableHandleScope`, which reserves a slot in the
parent *when it is constructed* and can therefore be used **once**.

Two consequences, both deliberate:

1. **Escapability is decided when a scope is opened, not when `Escape` is
   called.** `HandleScope` and `EscapableHandleScope` are separate types
   because an engine may have to reserve the parent slot up front. V8 does;
   SpiderMonkey does not care.
2. **The first escape from a scope is free; each one after that costs a global
   root.** The API does not inherit V8's one-escape-per-scope rule - a function
   that returns two handles is an ordinary thing to write, and SpiderMonkey can
   do it for nothing. Instead the V8 backend spends the reserved slot on the
   first escape, and for each later one parks the value in a `v8::Global`,
   reserves an empty parent slot, and fills it in once its own V8 scope has
   closed and the parent's is current again. The price shows up in the cost
   table rather than in a rule the caller has to remember.

A function that wants to return a handle therefore looks like V8:

```cpp
ub::Local<ub::Object> MakePoint(ub::Isolate& iso, const ub::Context& ctx) {
    ub::EscapableHandleScope scope(iso);
    auto obj = ub::Object::New(ctx).value();
    ...
    return scope.Escape(obj);
}
```

and the caller's own `HandleScope` owns the result.

### Where `Global<T>` fits

Anything that must outlive every frame — a cached constructor, a callback
stored on the embedder side, a value in a container, a member of a
long-lived C++ object — is a `Global<T>`: move-only, no copy (copy would be a
second root and is spelled `Global::Duplicate()` so it is visible), reset on
destruction. It is `v8::Global` on V8 and a heap-allocated
`JS::PersistentRooted` node on SpiderMonkey. It costs one allocation and one
list link on creation, which is why it is a distinct type rather than the
default: the hot path must not pay it.

`Global<T>::Get` materializes into the current frame, so reading a global
costs one slot append.

## 5. The rules these impose on callers

Normative. Tests assert them; checked builds diagnose the ones that can be
caught.

1. **A `HandleScope` is a stack object.** `operator new` is deleted, so it
   cannot be heap-allocated, and it must not be a member of a heap object.
   Scopes on one isolate close in reverse order of opening.
2. **A `Local<T>` is valid only while the frame that created it is open.**
   Using one afterwards is undefined behaviour. This is the same rule as V8's,
   with the same consequences.
3. **A `Local<T>` may be freely copied, passed, and stored in containers *for
   the lifetime of its frame*.** This is strictly more than SpiderMonkey's
   `Rooted` allows, and is the main ergonomic win of the indirection.
4. **To return a handle, escape it.** A bare `return local;` out of a function
   that opened its own `HandleScope` returns a dangling handle. There is no way
   to make that a compile error without making every handle carry its scope in
   its type; checked builds catch it at the point of use (§9).
5. **To keep a value past all scopes, move it into a `Global<T>`.** Nothing
   else outlives a frame — and a `Global<T>` outlives every frame but **not
   its isolate**: it holds one of that isolate's roots, so let it go before the
   isolate does. The same goes for the other two things that outlive a frame,
   `Context` and `Script`. See decision 27 in `docs/status.md`.
6. **Never copy a value out of the API into your own storage.** There is no
   way to do this — `Local` holds no engine value, only a slot ordinal — and
   that is deliberate: on a moving collector, a copied-out value is stale the
   moment the collector runs.
7. **A handle belongs to one isolate and one thread.** Crossing either is
   undefined; checked builds assert.
8. **Handles are not released individually.** Both engines are traced; there is
   no per-value release call and the API deliberately has none. A frame
   releases its slots wholesale when it closes.
9. **A frame that cannot grow yields an empty handle, never a value.** Adding a
   slot is fallible - on SpiderMonkey `RootedVector::append` reports OOM, and
   nothing portable makes it infallible - so every value-making operation can
   come back empty. An empty `Local` is not `undefined`: `IsEmpty()` is true,
   reading it is a programming error that checked builds diagnose, and the
   engine additionally reports the condition however it reports allocation
   failure. See below.

### Running out of slots

This is the one failure mode the handle model cannot make impossible, so it is
worth saying exactly what it does.

The alternative designs were: make every maker return `Maybe<Slot>`, or declare
exhaustion fatal. `Maybe` everywhere is the honest-looking answer and the wrong
one - it would turn `ub::Undefined(isolate)`, `Context::GlobalObject()`,
`Global<T>::Get()` and `info.This()` into things that must be unwrapped at
every call site, forever, for a condition that essentially never happens. Fatal
is what V8 does to itself and is not something a *library* should promise on an
engine that can report instead.

The empty handle is neither: it costs nothing at a call site that ignores it,
it is one `IsEmpty()` call for a caller that cares, and - the part that
matters - it cannot be confused with a result. A slot that read as `undefined`
would make running out of memory indistinguishable from a property that was not
there - and **nothing that can fail may return something that looks like it
succeeded**, which is the rule this whole API is built to keep. `Local` already
had `IsEmpty()`; this rule is what it is *for*.

## 6. What it costs

Per operation, against each engine's native API, with LTO on (see §10 — without
LTO every one of these is additionally a non-inlined call).

**The figures below are x86**, which is the architecture everything here was
first measured in. The handle is a pointer and a 32-bit ordinal, so the pointer
is what moves it:

| | x86 | x64 |
|---|---|---|
| `sizeof(Local<T>)`, `UNIBIND_HANDLE_CHECKS=0` | 8 | 16 |
| `sizeof(Local<T>)`, `UNIBIND_HANDLE_CHECKS=1` | 12 | 16 |

So on x64 **the checked build is the same size as the unchecked one**: the
epoch lands in padding the handle was already paying for, and the diagnosis in
§9 is free. Nothing assumes either number — `Local` is `sizeof(detail::Slot)`
by `static_assert`, and the suite prints what it came out as.

| Operation | V8 native | `unibind` on V8 | SpiderMonkey native | `unibind` on SpiderMonkey |
|---|---|---|---|---|
| `sizeof(handle)` | 4 (`Local`) | 8 | 8 (`Value`) + 12-ish per `Rooted` node | 8 |
| copy a handle | copy 4 bytes | copy 8 bytes | *impossible* (`Rooted` is immovable) | copy 8 bytes |
| create a handle | bump pointer | append to frame vector: bounds check + store | construct `Rooted`: 2 stores + list link | append: bounds check + store |
| read the value | 1 load | 2 loads (frame → buffer base, then index) | 1 load | 2 loads |
| open a scope | block bookkeeping | same + frame init (no allocation below 8 handles on SM, below the inline capacity on V8) | n/a | `RootedVector` ctor: 1 list link |
| escape a handle | 1 slot copy | first: 1 slot copy; each later one: a global root | n/a | 1 `Value` copy into parent |
| `Global` create | allocate a global slot | same | n/a | `PersistentRooted` node: 1 malloc + 1 list link |

The honest summary: **one extra load per value access and one extra
pointer-sized word per handle, against V8's native API; nothing at all against
SpiderMonkey's, which cannot express the operation in the first place.** A
`v8::Local` is one pointer and a `ub::Local` is a pointer plus an ordinal, so
the difference is 4 bytes on x86 and 8 on x64. No
allocation per handle, no virtual call, no `std::function`, no RTTI, no
refcount traffic.

The one place the model is *cheaper* than the native API is SpiderMonkey: N
values in one `RootedVector` is one list link, where N `Rooted`s are N.

## 7. How each backend implements a frame

**V8.** The frame holds a `v8::HandleScope` - or a `v8::EscapableHandleScope`
if the scope was opened escapable - plus a small-buffer array of
`v8::Local<v8::Value>`, inline up to `UNIBIND_FRAME_INLINE_SLOTS` and on the heap
above that. Slot *i* is element *i*. Frame slots `[0, argc)` of a callback
frame are not stored at all: they are read straight out of the
`FunctionCallbackInfo`, which V8 has already rooted.

Escape is *not* an append to the parent's array, for the reason in section 4:
a `v8::Local` is a pointer into the innermost scope's block, so the copy would
outlive what it points at. The first escape spends the slot
`v8::EscapableHandleScope` reserved in the parent; later ones go through a
`v8::Global` that is materialised into the parent after this frame's V8 scope
has closed. The frame therefore constructs its V8 scope in place rather than
holding it as a member, so it can be destroyed before the deferred escapes are
written.

**SpiderMonkey.** The frame holds a `JS::RootedVector<JS::Value>` constructed
on the `JSContext`. Slot *i* is element *i*. `HandleScope` construction and
destruction are the `Rooted` link/unlink, which is exactly the LIFO discipline
the API already requires of callers, so the engine's assertion and the API's
rule are the same rule.

**CPython.** The frame holds strong references: `UNIBIND_FRAME_INLINE_SLOTS`
`PyObject*`s inline and a spill buffer from `::operator new(std::nothrow)` above
that. Slot *i* is element *i*, and a callback frame's slots `[0, argc)` are the
vectorcall argument array, which CPython holds for the call. Closing the frame
releases every reference it took; section 12 has the rest.

All three frames fit in the `HandleScope`'s inline byte storage, whose size comes
from the generated `unibind/config.h`; the backend `static_assert`s that its frame
fits. Too big is a compile error in the backend, never a silent heap fallback.

## 8. Callback arguments

A native callback receives its arguments as a contiguous array that the engine
has already rooted for the duration of the call — `FunctionCallbackInfo` on V8,
`JS::CallArgs` on SpiderMonkey (whose values live on the VM stack, rooted by
the interpreter). The callback's frame therefore *borrows* that region: slots
`[0, argc)` map to the engine's array, and slots from `argc` up are the frame's
own. `args[i]` is an ordinal like any other handle; the mapping is a branch in
the backend, never visible in the API, and it means a call with 8 arguments
that reads one of them copies one value, not eight.

This is why `Local` is (frame, index) rather than anything that assumes a
single uniform buffer: the indirection through the frame is what lets a frame
be partly borrowed.

## 9. What is checked

- LIFO scope closing, isolate/thread affinity of a handle, and index bounds:
  asserted in checked builds. `UNIBIND_HANDLE_CHECKS` is on by default in
  *every* configuration, because it is an ABI flag rather than a debug one
  (`UNIBIND_HANDLE_CHECKS_ENABLED` is the CMake option that sets it) — but the
  checks themselves are assertions, so they fire only where assertions are
  live. A Release build carries the epoch and does not test it.
- Use of a `Local` after its frame closed: caught in checked builds by an
  epoch word carried in the handle and compared against the frame's. A frame
  overwrites its epoch with one no handle carries as it closes, so the check
  does not depend on a later frame reusing the same storage - which an
  optimised build usually does and an unoptimised one usually does not. On x86
  the option changes `sizeof(Local)` from 8 to 12; on x64 it changes nothing,
  because the epoch fits in the padding (§6). It is an ABI change either way —
  which is why the flag lives in a **generated** `unibind/config.h` rather than
  being a per-target `-D`, so the whole build tree necessarily agrees.
- Not checked, in any build: a `Local` copied into a container that outlives
  the frame and read after a *new* frame reused the address with a matching
  epoch. The epoch counter is per-isolate and monotonic, so this requires
  2^32 frames; it is a theoretical hole, not a practical one.
- Not checked: returning a `Local` from a function whose scope has closed, at
  the point of *return*. It is caught at the point of *use*.
- A `Context`, `Script` or `Global<T>` still alive when its isolate is
  destroyed: counted, and asserted in `~Isolate` in checked builds. It is
  diagnosed where the mistake is rather than where it later crashes — the
  release the embedder has not yet made would reset an engine handle against a
  disposed isolate. Rule and reasoning: decision 27 in `docs/status.md`.

## 10. Inlining, and why the headers declare rather than define

Public headers must not include an engine header (the build enforces it with a
target that compiles the public headers alone). So every operation that needs
an engine type is **declared** in `unibind/detail/backend.h` and **defined** in the
backend library. Without link-time optimization that is a real call per
operation — a few instructions of overhead, but a call.

With LTO the definitions are visible to the linker and inline back into the
caller, which is what makes the table in §6 true. `UNIBIND_LTO` is **off** in
this tree — so the benchmarks were measured without it and are a floor rather
than a ceiling — and the cost of that is documented rather than hidden: the API
shape does not change, the codegen does. A consumer who wants the table's
figures builds with `-DUNIBIND_LTO=ON` and compiles their own code with LTO too.

The alternative — an inline backend header pulled in by the public header —
buys inlining without LTO and loses the "no engine header" guarantee, which is
the one property that keeps a second engine possible. Not a trade we make.

## 11. Alternatives rejected

### A. V8's model directly: `Local<T>` as a pointer into a scope block

The reference design, and the reason this document exists. Requires a scope you
can allocate an unbounded number of *stably addressed* slots into. On
SpiderMonkey the only stack-rooted growable container reallocates its buffer,
so addresses do not survive an append. Implementable only by giving up exact
stack rooting (see C). **Rejected: fails constraint 1.**

### B. Handles as literal `JS::Rooted` equivalents (`UNIBIND_ROOTED(name, expr)`)

Mirror SpiderMonkey exactly: a handle is a named stack variable that roots one
value, spelled with a macro so the V8 backend can expand it to a plain `Local`.
Cheapest possible on SpiderMonkey, and *provably* correct there.

It fails the requirement that a function be able to **return** a handle: a
`Rooted` cannot be returned, cannot be moved, cannot be copied. Every factory
function would have to take an out-parameter (`MutableHandle`) — where
everything here that produces a value returns it, so a handle would be the one
thing a caller could not simply be handed — and no handle could be stored in a
container even within its own frame. The API would be a SpiderMonkey API with a
V8 vocabulary pasted on. **Rejected: fails expressiveness, and imports the worst ergonomics
of the stricter engine into the common API.**

### C. Chunked frame with stable slot addresses; handle is a pointer

Keep V8's pointer handle by making frame storage a list of fixed-size chunks
that never move. On SpiderMonkey each chunk needs its own root, and the number
of chunks is dynamic, so the roots must be off-stack: `PersistentRooted` per
chunk. That is a malloc and a list insertion per chunk, an off-stack root set
that the "exact, stack-ordered" property no longer describes, and a
distinctly worse story if a GC-during-allocation bug ever has to be debugged.

It buys back one load per value access. **Rejected: pays the engine's core
discipline for a load.** (Worth revisiting only if profiling ever shows the
second load mattering, and even then only as a SpiderMonkey-side variation
behind the same public handle type — which the index model permits and the
pointer model would not.)

### D. RAII owning handle with move semantics (one root per handle)

`Local<T>` becomes a move-only object that owns its own root: `v8::Global` on
V8, `JS::PersistentRooted` on SpiderMonkey. Returnable, storable, container-
friendly, no scopes at all, and on a *refcounted* engine it would be the obvious
answer, because there a handle copy costs an integer increment and nothing else
(§12).

Under a tracing collector it is a malloc, a list link, and an unlink *per
handle* — for handles whose median lifetime is a few instructions. It also
makes every intermediate value in an expression a heap node, and
`PersistentRooted`'s own header warns that persistent roots participate in leak
cycles. **Rejected on cost.** It survives, renamed, as `Global<T>`, which is
where that cost is worth paying.

### E. `shared_ptr`-style refcounted handle

Copy is an atomic increment, destruction a decrement, and a control block per
value. Everything wrong with D, plus atomics on a single-threaded hot path, and
it still needs an underlying root because neither engine refcounts. **Rejected.**

### F. Index into a single isolate-wide handle stack (no frame pointer)

4-byte handles, one indirection, frames are just watermarks — the classic
"handle table" design, and genuinely attractive. On SpiderMonkey the isolate-
wide array has to be a `PersistentRootedVector`: a single off-stack root. That
is legal, correct, and *cheap* — one root for the whole isolate.

It was rejected for two reasons. First, a stale handle silently reads a live
slot belonging to an unrelated frame, which is the worst failure mode of the
lot: no crash, no assert, wrong value. The frame pointer at least makes the
common stale case point at a dead frame. Second, it gives up stack rooting
wholesale, which is constraint 1's actual subject; a design that satisfies the
constraint only via the engine's escape hatch has not satisfied it.

**Rejected — but it is the fallback if the frame-pointer model ever proves too
big or too slow, and the public API would not change, only `Local`'s two
fields.** That is by itself a good sign about the boundary.

### G. Hybrid: scope handles that auto-promote to globals on escape

Return a `Global<T>` from any function that returns a handle, and let the
caller demote it. Correct, and no escape mechanism needed — but it puts an
allocation on every returning factory function, which is most of them.
**Rejected on cost**; the explicit `Escape` is 20 characters and no malloc.

## 12. A refcounted backend

This section used to be a prediction: that the model neither precludes a
refcounting engine nor pays for one, and what such a backend would cost. The
CPython backend (`src/backends/python/`, `docs/python.md`) is that backend, and
the prediction held line for line.

- **A frame is an array of owned references** - `PyObject*`, eight inline and a
  spill buffer above that - released in reverse when it closes. That is the scope
  semantics the API already specified; nothing in §4 or §5 changed.
- **Copying a `Local` copies an ordinal** and touches no reference count, so a
  handle is as cheap to pass around as on the tracing engines.
- **`Global<T>` is one owned reference**, and **`Escape` is one `Py_NewRef` into
  the parent** - free, and unbounded, since nothing about a refcounted object
  pins it to the frame that made it.
- **§8's borrowed argument region needed nothing**: a callback frame's slots
  `[0, argc)` are the vectorcall argument array, which CPython holds for the
  call, and the frame already knew where its borrowed prefix ends.
- **Rule 9 is reachable** because the spill buffer comes from
  `::operator new(std::nothrow)`, the lever the suites use to make a frame fail
  to grow; a frame that cannot grow releases the value it was handed and yields
  an empty handle with `MemoryError` pending.

The cost is the one predicted, and it is the engine's: one reference-count pair
per *slot* (not per handle), and a frame destructor that is a loop rather than a
no-op. No storage budget had to be raised. What CPython added that the
prediction did not need to think about is that objects never move, so §3's point
3 - no stable address - is moot there; the model's indirection costs that backend
one load it did not strictly need, which is the price of one model for three
engines.

## 13. Revisit triggers

Change this document, not just the code, if any of these happens:

- Profiling shows the second load in §6 is material on a real workload → §11.C.
- `sizeof(Local)` matters (a handle-dense container) → §11.F.
- A backend appears whose roots are neither stack-LIFO nor refcounted (a
  conservative-stack-scanning engine, say) → the frame abstraction is probably
  still right, but §7 gains a row and §6's table needs redoing.
- A caller is found that legitimately needs to escape a handle through more
  than one frame at once → today that is a `Global`, and if it becomes common
  the answer is a multi-escape, not a weakening of rule 2.
