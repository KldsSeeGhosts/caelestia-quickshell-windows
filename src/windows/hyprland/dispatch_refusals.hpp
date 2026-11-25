#pragma once

#include <qstring.h>

#include "../tiling_result.hpp"
#include "pin_support.hpp"

namespace qs::hyprland::ipc {

[[nodiscard]] inline QString pinRefusalReason(PinDispatchResult result, bool elevated) {
	if (result == PinDispatchResult::Unsupported) {
		return QStringLiteral("Window pinning is not available on this system");
	}
	if (result == PinDispatchResult::QueryFailed) {
		return QStringLiteral("Could not read the window pin state");
	}
	if (result == PinDispatchResult::Failed) {
		return elevated ? QStringLiteral("Windows refused to pin an elevated window")
		                : QStringLiteral("Failed to pin window");
	}
	return {};
}

template <typename Tracker>
void reportPinRefusal(Tracker* tracker, PinDispatchResult result, bool elevated) {
	if (tracker == nullptr) return;
	const auto reason = pinRefusalReason(result, elevated);
	if (!reason.isEmpty()) tracker->reportActionRefusal(QStringLiteral("pin"), reason, elevated);
}

template <typename Tracker>
void reportToggleFloatingDisabled(Tracker* tracker, bool elevated) {
	if (tracker == nullptr) return;
	tracker->reportActionRefusal(
	    QStringLiteral("togglefloating"), QStringLiteral("Tiling is disabled"), elevated
	);
}

[[nodiscard]] inline QString
toggleFloatingRefusalReason(qs::windows::FloatingChangeResult result, bool elevated) {
	if (result == qs::windows::FloatingChangeResult::ScopeExcluded) {
		return QStringLiteral("Window is excluded from tiling");
	}
	return elevated ? QStringLiteral("Windows prevents tiling an elevated window")
	                : QStringLiteral("Window cannot change floating state");
}

template <typename Tracker>
void reportToggleFloatingResult(
    Tracker* tracker,
    qs::windows::FloatingChangeResult result,
    bool elevated
) {
	if (tracker == nullptr) return;
	if (result == qs::windows::FloatingChangeResult::Changed
	    || result == qs::windows::FloatingChangeResult::Noop
	    || result == qs::windows::FloatingChangeResult::Deferred)
	{
		return;
	}
	tracker->reportActionRefusal(
	    QStringLiteral("togglefloating"), toggleFloatingRefusalReason(result, elevated), elevated
	);
}

[[nodiscard]] inline bool isWindowRejectionMoveCode(const QString& code) {
	return code.isEmpty() || code == QStringLiteral("permission-denied")
	    || code == QStringLiteral("command-rejected");
}

[[nodiscard]] inline QString moveRefusalReason(const QString& code, bool elevated) {
	if (elevated && isWindowRejectionMoveCode(code)) {
		return QStringLiteral("Windows refused to move an elevated window");
	}
	if (code == QStringLiteral("permission-denied")) {
		return QStringLiteral("Windows denied permission to move this window");
	}
	if (code == QStringLiteral("invalid-window")) {
		return QStringLiteral("The target window is no longer available");
	}
	if (code == QStringLiteral("invalid-target")) {
		return QStringLiteral("The target workspace is not available");
	}
	if (code == QStringLiteral("workspace-limit")) {
		return QStringLiteral("Workspace limit reached");
	}
	if (code == QStringLiteral("provider-timeout") || code == QStringLiteral("provider-busy")) {
		return QStringLiteral("Windows desktop control did not respond in time");
	}
	if (code == QStringLiteral("provider-disconnected")) {
		return QStringLiteral("Windows desktop control is restarting");
	}
	if (code == QStringLiteral("provider-unavailable")
	    || code == QStringLiteral("unsupported-build")
	    || code == QStringLiteral("operation-unsupported"))
	{
		return QStringLiteral("Moving windows between workspaces is not available on this system");
	}
	return QStringLiteral("Windows refused to move this window to that workspace");
}

template <typename Tracker>
void reportMoveRefusal(Tracker* tracker, const QString& code, bool elevated) {
	if (tracker == nullptr) return;
	const auto reportElevated = elevated && isWindowRejectionMoveCode(code);
	tracker->reportActionRefusal(
	    QStringLiteral("move"), moveRefusalReason(code, reportElevated), reportElevated
	);
}

} // namespace qs::hyprland::ipc
