#pragma once

#include <qtenvironmentvariables.h>
#include <qt_windows.h>

namespace qs::crash {

// The crash handler starts a replacement shell after a crash by default. A
// supervisor that restarts the shell itself sets QS_DISABLE_CRASH_RELAUNCH
// instead of QS_DISABLE_CRASH_HANDLER: the minidump, the desktop cleanup and
// the report stay, and no second shell or dialog starts from here.
//
// Presence, not value, disables the relaunch, matching QS_DISABLE_CRASH_HANDLER.
// QS_DISABLE_CRASH_HANDLER keeps its meaning: it turns the whole handler off, so
// this variable never has to be set alongside it.
[[nodiscard]] inline bool crashRelaunchDisabled() {
	return qEnvironmentVariableIsSet("QS_DISABLE_CRASH_RELAUNCH");
}

// Gamma recovery performs a display topology query. A stack-overflowing thread
// may not have enough stack left for that work, so crash cleanup skips it there.
[[nodiscard]] inline bool crashGammaRestoreAllowed(DWORD exceptionCode) {
	return exceptionCode != EXCEPTION_STACK_OVERFLOW;
}

} // namespace qs::crash
