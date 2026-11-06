#pragma once

#include <qvariant.h>

namespace qs::windows::services::systray {

enum class TrayHookState {
	Disabled,
	Starting,
	Running,
	Elevated,
	WindowCreationFailed,
	Stopped,
};

inline QVariantMap trayHookCapability(TrayHookState state) {
	switch (state) {
	case TrayHookState::Running: return {{"availability", "available"}, {"reason", ""}};
	case TrayHookState::Disabled: return {{"availability", "disabled"}, {"reason", "hook-disabled"}};
	case TrayHookState::Starting:
		return {{"availability", "unavailable"}, {"reason", "hook-starting"}};
	case TrayHookState::Elevated: return {{"availability", "unavailable"}, {"reason", "elevated"}};
	case TrayHookState::WindowCreationFailed:
		return {{"availability", "unavailable"}, {"reason", "window-creation-failed"}};
	case TrayHookState::Stopped: return {{"availability", "unavailable"}, {"reason", "hook-stopped"}};
	}
	Q_UNREACHABLE();
}

} // namespace qs::windows::services::systray
