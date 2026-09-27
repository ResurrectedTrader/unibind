// The inspector, on an engine that has none - CPython included.
//
// CPython's debugging surface is `sys.monitoring` and the pdb/debugpy family
// built on it, which speak the Debug Adapter Protocol if they speak anything; a
// Chrome DevTools inspector is not something this backend can honestly offer.
//
// So `Inspector::Supported()` is false and `New` makes nothing, and every other
// member is defined anyway. That is what `unibind/inspector.h` promises and it
// is the point: a program compiled once against the headers links against this
// backend, asks at run time, and is told no. Nothing below can be reached - no
// `Inspector` or `InspectorSession` is ever made here - so each body does
// nothing rather than something that would have to be tested.

#include "unibind/inspector.h"

#include <memory>
#include <string_view>
#include <utility>

namespace ub {

struct Inspector::Impl {};
struct InspectorSession::Impl {};
struct InspectorDispatcher::Impl {};

bool Inspector::Supported() noexcept {
    return false;
}

std::unique_ptr<Inspector> Inspector::New(Isolate& /*isolate*/, InspectorClient& /*client*/) {
    return nullptr;
}

Inspector::Inspector(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Inspector::~Inspector() = default;

// The members below are declared on the object because on an engine with an
// inspector they need one; here there is never an object to call them on.
// NOLINTBEGIN(readability-convert-member-functions-to-static)
void Inspector::ContextCreated(const Context& /*context*/, std::string_view /*name*/) {}
void Inspector::ContextDestroyed(const Context& /*context*/) {}
std::unique_ptr<InspectorSession> Inspector::Connect() {
    return nullptr;
}
std::shared_ptr<InspectorDispatcher> Inspector::Dispatcher() const noexcept {
    return nullptr;
}

InspectorDispatcher::InspectorDispatcher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InspectorDispatcher::~InspectorDispatcher() = default;
bool InspectorDispatcher::RequestDispatch(JobCallback /*callback*/, CallbackData /*data*/) noexcept {
    return false;
}

InspectorSession::InspectorSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InspectorSession::~InspectorSession() = default;
void InspectorSession::DispatchProtocolMessage(std::string_view /*message*/) {}
void InspectorSession::Resume() {}
void InspectorSession::Stop() {}
// NOLINTEND(readability-convert-member-functions-to-static)

}  // namespace ub
