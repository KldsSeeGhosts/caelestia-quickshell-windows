#pragma once

#include <qtenvironmentvariables.h>

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

} // namespace qs::crash
