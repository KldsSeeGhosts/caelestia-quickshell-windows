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
	const auto reason = pinRefusalReason(result, elevated);
	if (!reason.isEmpty()) tracker->reportActionRefusal(QStringLiteral("pin"), reason, elevated);
}

template <typename Tracker>
void reportToggleFloatingDisabled(Tracker* tracker, bool elevated) {
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

} // namespace qs::hyprland::ipc
