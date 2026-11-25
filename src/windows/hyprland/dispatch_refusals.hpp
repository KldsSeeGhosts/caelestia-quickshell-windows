#pragma once

#include <qstring.h>

#include "pin_support.hpp"

namespace qs::hyprland::ipc {

[[nodiscard]] inline QString pinRefusalReason(PinDispatchResult result, bool elevated) {
	if (result == PinDispatchResult::Unsupported) {
		return QStringLiteral("Window pinning is not available on this system");
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

[[nodiscard]] inline QString toggleFloatingRefusalReason(bool elevated) {
	return elevated ? QStringLiteral("Windows prevents tiling an elevated window")
	                : QStringLiteral("Window cannot change floating state");
}

template <typename Tracker>
void reportToggleFloatingFailure(Tracker* tracker, bool elevated) {
	tracker->reportActionRefusal(
	    QStringLiteral("togglefloating"), toggleFloatingRefusalReason(elevated), elevated
	);
}

} // namespace qs::hyprland::ipc
