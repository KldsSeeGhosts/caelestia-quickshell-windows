#pragma once

#include <optional>
#include <utility>

#include <qt_windows.h>
#include <qtypes.h>

namespace qs::windows {

// The documented foreground lock is a permission, not an API to work around.
// SetForegroundWindow allows a process to activate a window only while it is
// the foreground process, received the last input event, or no process owns the
// foreground; otherwise Windows refuses and flashes the taskbar button instead.
// The shell's right to activate comes from an input event it received: a click
// on one of its own surfaces, or a hotkey Windows delivered to it. A QML show,
// an IPC request, a workspace switch or a timer carries no such right.
//
// A WH_KEYBOARD_LL hook is the exception. When the hook swallows a key Windows
// never delivered it to the shell, so the "received the last input event" rule
// does not apply even though the shell saw the key. Lone-modifier taps such as
// the default lone-Super launcher can only use that hook path (keyboard_hook.hpp
// explains why). The maintainer accepted the old focus-forcing technique for
// exactly that source as a Windows limitation, not as a general activation
// path: the shared architecture notes
enum class ActivationSource : quint8 {
	// A key swallowed by the WH_KEYBOARD_LL hook. No documented foreground
	// permission, so a refused activation may use the accepted limitation.
	Hook,
	// WM_HOTKEY from RegisterHotKey. Windows delivered the key to the shell,
	// so SetForegroundWindow is permitted once, synchronously.
	Hotkey,
	// A click on a shell surface. The click is the input event the shell
	// received, so SetForegroundWindow is permitted once, synchronously.
	Click,
	// IPC or any request without a shell input event. Clean path only; a
	// refusal is reported.
	Ipc,
};

// Only a swallowed hook key may use the accepted escalation. Everything else
// relies on the documented permission or reports the refusal.
[[nodiscard]] constexpr bool mayForceForeground(ActivationSource source) {
	return source == ActivationSource::Hook;
}

// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow
enum class ForegroundHandoff {
	// SetForegroundWindow is the only call; the process owns no right to hand
	// over, or the target already belongs to this process.
	None,
	// This process owns the foreground and is handing it to another process.
	// Grant that process the documented AllowSetForegroundWindow handoff first.
	AllowTarget,
};

// AllowSetForegroundWindow is a handoff from the process that owns the
// foreground to a different process. Granting the calling process itself is
// meaningless, and granting a process while the caller does not own the
// foreground would be granting a right the caller does not have.
[[nodiscard]] constexpr ForegroundHandoff foregroundHandoff(
    bool callerOwnsForeground,
    DWORD targetPid,
    DWORD callerPid
) {
	if (!callerOwnsForeground || targetPid == 0 || targetPid == callerPid) {
		return ForegroundHandoff::None;
	}

	return ForegroundHandoff::AllowTarget;
}

// A panel that stops accepting keyboard focus gives the foreground back to
// the application that had it before the panel took it. Windows' foreground
// lock only permits that while the shell still owns the foreground, and the
// remembered window may have closed, been minimized or hidden, belong to
// the shell itself, or be on another monitor. A panel opened on a different
// display must not send keyboard focus back across displays as it closes.
// Any of those falls back to the close path's same-monitor handoff.
[[nodiscard]] constexpr bool shouldRestoreRememberedForeground(
    bool shellOwnsForeground,
    bool windowLive,
    bool windowIsOwnProcess,
    bool sameMonitor
) {
	return shellOwnsForeground && windowLive && !windowIsOwnProcess && sameMonitor;
}

// Unknown or removed monitors are not interchangeable with another display.
// Use native monitor identity, never a label or enumeration ordinal.
[[nodiscard]] constexpr bool sameFocusReturnMonitor(HMONITOR panel, HMONITOR candidate) {
	return panel != nullptr && candidate == panel;
}

// Carries the origin of the current dispatch while the hotkey manager invokes
// the action that may open a panel. The marker is save/restore scoped, so a
// binding that fires another bind cannot leak its source outwards.
class ActivationDispatchScope {
public:
	explicit ActivationDispatchScope(ActivationSource source);
	~ActivationDispatchScope();
	ActivationDispatchScope(const ActivationDispatchScope&) = delete;
	ActivationDispatchScope& operator=(const ActivationDispatchScope&) = delete;
	ActivationDispatchScope(ActivationDispatchScope&&) = delete;
	ActivationDispatchScope& operator=(ActivationDispatchScope&&) = delete;

private:
	std::optional<ActivationSource> previous;
};

// The source of the dispatch currently running, if it was started by a key the
// shell received. Activation requests outside a dispatch are IPC or state
// changes, never a claim to a permission the shell does not have.
[[nodiscard]] std::optional<ActivationSource> currentActivationSource();

// The source of one open request whose first show is deferred. A window or grab
// created from QML during a dispatch only becomes visible after Reloadable's
// zero-delay reload, when the dispatch scope is gone. capture() runs where the
// open is requested, take() runs once at the show that fulfils it. The owner
// also clear()s it when its reload ends, so an open that never shows cannot
// hand a hook source to a later, unrelated activation.
class PendingActivation {
public:
	// Remember the running dispatch's source. Outside a dispatch nothing is
	// recorded, so the show falls back to the state it happens in.
	void capture() {
		if (auto source = currentActivationSource()) this->source = source;
	}

	void clear() { this->source.reset(); }

	// Consumes the captured source: a second call returns nothing.
	[[nodiscard]] std::optional<ActivationSource> take() {
		return std::exchange(this->source, std::nullopt);
	}

private:
	std::optional<ActivationSource> source;
};

} // namespace qs::windows
