#pragma once

#include "system_surface.hpp"

namespace qs::windows {

enum class ForegroundAction {
	// No application took the foreground: keep the one observed last.
	Keep,
	// Nothing is focused, or the foreground belongs to no application.
	Clear,
	// The foreground window is an application.
	Adopt,
};

// A foreground surface the bar can name. This is intentionally wider than
// `isApplicationWindow`: an owned dialog or popup is a real application surface
// even though it gets no taskbar button, and the active-window display must
// follow the dialog the user is using. A tool window without WS_EX_APPWINDOW,
// an untitled, hidden or cloaked window and Windows shell UI are not.
[[nodiscard]] inline bool isForegroundApplication(const system_surface::Identity& identity) {
	if (!identity.root || identity.child || !identity.visible) return false;
	if (identity.cloaked) return false;
	if (identity.toolWindow && !identity.appWindow) return false;
	if (!identity.hasTitle) return false;

	return !system_surface::isShellSurface(identity);
}

// What one foreground event means for the shell's active application. The
// shell keeps the last application while its own panels or a Windows shell
// overlay owns the foreground, the same continuity WindowTracker gives its
// active window. The desktop, a hidden window and any other non-application
// clear it; a real application is adopted even when the taskbar/tiling rules
// do not track it.
[[nodiscard]] inline ForegroundAction foregroundAction(
    const system_surface::Identity& identity,
    bool ownProcess
) {
	if (ownProcess) return ForegroundAction::Keep;
	if (system_surface::isDesktopSurface(identity)) return ForegroundAction::Clear;
	if (system_surface::isShellSurface(identity)) return ForegroundAction::Keep;

	return isForegroundApplication(identity) ? ForegroundAction::Adopt
	                                         : ForegroundAction::Clear;
}

// The application a shell surface hands the foreground back to when it stops
// taking focus. Foreground events carry an HWND, and the tracker drops
// null-HWND events before delivery, so a desktop or windowless foreground
// reaches a consumer only as the tracker's active application going away.
// Both paths clear the remembered application.
struct RememberedForeground {
	HWND hwnd = nullptr;

	void observe(HWND value, ForegroundAction action) {
		switch (action) {
		case ForegroundAction::Adopt: this->hwnd = value; break;
		case ForegroundAction::Clear: this->hwnd = nullptr; break;
		case ForegroundAction::Keep: break;
		}
	}

	// The tracker has no active application: the desktop, or nothing at all.
	void onActiveApplicationLost() { this->hwnd = nullptr; }
};

} // namespace qs::windows
