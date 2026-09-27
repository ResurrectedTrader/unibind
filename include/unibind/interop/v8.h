#pragma once

/// The V8 objects under a `ub::Isolate` and a `ub::Context`, for the few things
/// only V8 can do and this library does not abstract. The Chrome DevTools
/// inspector, which is what first needed these, is not one of them any more:
/// `unibind/inspector.h` offers it on every backend that has one, and a program
/// that uses it compiles against no engine header at all.
///
/// **This is the one header whose functions only one backend defines.** It
/// compiles without V8's headers - the V8 types are forward-declared, so
/// including it costs nothing and breaks no backend-neutral object - but a
/// program that *calls* these links only against `unibind_backend_v8`. That is
/// the point: the code that needs V8 goes in an object or library that is linked
/// only into the V8 build, and a SpiderMonkey build links something else in its
/// place. Nothing else in unibind is shaped like this, and nothing else should
/// be.
///
/// What comes back is borrowed, and the usual V8 rules apply to it, not
/// unibind's:
///
///   * The isolate lives exactly as long as the `ub::Isolate`. Do not dispose
///     it, do not enter or exit it - unibind already has - and use it only on
///     the isolate's own thread.
///   * `V8Context` makes a `v8::Local`, so the caller must have a
///     `v8::HandleScope` open. A `ub::HandleScope` is not one.
///   * Anything V8 does through these - running script, a nested message loop
///     at a breakpoint, a pause that lasts minutes - is invisible to unibind and
///     leaves its handles and frames as they were, which is what makes it safe.
///     What it must not do is make or free unibind handles from inside a V8
///     callback unibind did not install.
///   * Inside a unibind callback, enter a realm with `ub::ContextScope`, not
///     with V8's `Context::Enter` or `Context::Scope`. The backend finds a
///     callback's realm (`GetContext()`) when first asked, and settles it
///     before a `ContextScope` changes the current realm - which V8's own
///     calls, invisible to it, do not do. Ask before entering, or enter the
///     unibind way.
///
/// The translation unit that uses these includes V8's headers itself, with
/// `V8_GN_HEADER` defined (so the layout matches the library), and includes
/// `<v8-fast-api-calls.h>` before `<v8.h>`: under the MSVC STL, `v8.h` alone
/// leaves the `CFunction` that `v8-template.h` spans over incomplete.

namespace v8 {
class Isolate;
class Context;
template <class T>
class Local;
}  // namespace v8

namespace ub {

class Isolate;
class Context;

namespace interop {

/// The `v8::Isolate` a `ub::Isolate` runs on. Never null for a live isolate.
[[nodiscard]] v8::Isolate* V8Isolate(Isolate& isolate) noexcept;

/// The `v8::Context` a `ub::Context` names. Needs an open `v8::HandleScope`;
/// empty if the context is.
[[nodiscard]] v8::Local<v8::Context> V8Context(const Context& context) noexcept;

}  // namespace interop
}  // namespace ub
