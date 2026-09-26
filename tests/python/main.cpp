#define DOCTEST_CONFIG_IMPLEMENT
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <crtdbg.h>
#include <doctest/doctest.h>
// for `_set_abort_behavior`, a CRT extension that the C header declares and <cstdlib> does not promise.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>

#include "support.h"
#include "unibind/unibind.h"

namespace {

/// A crash in this suite must fail the run, not wait for someone to click. A
/// debug CRT answers `abort()` and a failed debug-heap check with a modal
/// dialog, and Windows Error Reporting answers an unhandled fault with another;
/// either one leaves the process blocked on a desktop nobody is watching - a
/// CTest timeout at best. Everything goes to stderr instead, and the process
/// dies with an exit code CTest reports.
void DieQuietly() noexcept {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        (void)kind;  // the two calls below are nothing in a release CRT
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
}

}  // namespace

int main(int argc, char** argv) {
    DieQuietly();
    // Installed for the whole run - a Platform's fault handler is fixed for its
    // life - and counted in support.cpp for the cases that need to see one.
    ub::PlatformOptions options;
    options.onEngineFault = &py_test::OnEngineFault;
    const ub::Platform platform(options);
    doctest::Context context(argc, argv);
    return context.run();
}
