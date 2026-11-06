#pragma once

#include <qt_windows.h>
#include <functional>

#include <qpoint.h>
#include <qrect.h>
#include <qscreen.h>
#include <qtypes.h>
#include <qwindow.h>

#include "activation_policy.hpp"
#include "screen_identity.hpp"

namespace qs::windows {

struct MonitorRects {
	QRect monitor;
	QRect work;
	bool valid = false;
};

[[nodiscard]] HMONITOR monitorForScreen(QScreen* screen);
[[nodiscard]] QScreen* screenForMonitor(HMONITOR monitor);
[[nodiscard]] QScreen* focusedScreen();
// Latest-event-wins focused monitor, as Hyprland publishes it. Each returns
// true when the focused screen changed. The cursor only moves focus while
// focus-follows-mouse is enabled; a click and a foreground window always do.
[[nodiscard]] bool focusActiveWindow(QScreen* screen);
[[nodiscard]] bool focusCursorMonitor(QScreen* screen, bool focusFollowsMouse);
[[nodiscard]] bool focusClickedMonitor(QScreen* screen);
[[nodiscard]] bool focusFollowsMouseEnabled();
void prioritizeInputThread(int priority);
void optOutOfProcessPowerThrottling();
[[nodiscard]] MonitorRects monitorRects(HMONITOR monitor);

class ScreenMapper {
public:
	ScreenMapper(const QRect& logicalScreen, const QRect& physicalMonitor, qreal dpr)
	    : logicalOrigin(logicalScreen.topLeft())
	    , physicalOrigin(physicalMonitor.topLeft())
	    , dpr(dpr) {}

	[[nodiscard]] QPoint toLogical(const QPoint& physical) const;
	[[nodiscard]] QRect toLogical(const QRect& physical) const;
	[[nodiscard]] QPoint toPhysical(const QPoint& logical) const;
	[[nodiscard]] QRect toPhysical(const QRect& logical) const;
	[[nodiscard]] qint32 toPhysical(qint32 logicalLength) const;

private:
	QPoint logicalOrigin;
	QPoint physicalOrigin;
	qreal dpr;
};

[[nodiscard]] HWND hwndOf(const QWindow* window);
[[nodiscard]] QRect toQRect(const RECT& rect);
[[nodiscard]] RECT toRECT(const QRect& rect);

// The shell's single activation path, following the documented foreground
// lock (activation_policy.hpp). It asks SetForegroundWindow once and returns
// false when Windows refuses. The caller reports the refusal; nothing here
// retries, injects input or waits on a timer.
//
// A Click or Hotkey source has the input event the foreground lock recognizes.
// Hook has no such event because the WH_KEYBOARD_LL hook swallowed the key, so
// only that source may use the accepted limitation documented next to
// forceForegroundFromHook().
[[nodiscard]] bool requestForegroundWindow(HWND hwnd, ActivationSource source);

// The source for an activation that is not inside a hotkey dispatch. A click or
// key delivered to a shell surface leaves this process with the foreground, so
// it is Click; otherwise the request has no input permission and is Ipc.
[[nodiscard]] ActivationSource activationSourceForCurrentState();
void runLauncher(std::function<void()> task);

DWORD windowsBuild();

[[nodiscard]] bool isOwnProcessWindow(HWND hwnd);
[[nodiscard]] bool isMoreElevated(HWND hwnd);

inline constexpr const wchar_t* TRAY_HOOK_PROP = L"QuickshellTrayHook";
[[nodiscard]] bool isTrayHookWindow(HWND hwnd);

[[nodiscard]] HWND explorerTaskbarWindow();

class TrayHookYield {
public:
	TrayHookYield();
	~TrayHookYield();
	TrayHookYield(const TrayHookYield&) = delete;
	TrayHookYield& operator=(const TrayHookYield&) = delete;
	TrayHookYield(TrayHookYield&&) = delete;
	TrayHookYield& operator=(TrayHookYield&&) = delete;
};

[[nodiscard]] bool trayHookYielding();

// A yield moves the tray hook window behind Explorer for the duration of an
// AppBar round trip. That move belongs to this process, so the reorder hook
// (scoped to Explorer) never sees it; ask for one repair when the last yield
// ends.
[[nodiscard]] UINT trayHookRepairMessage();
void requestTrayHookRepair();

void markNonRude(HWND hwnd);

void applyPanelDwmAttributes(HWND hwnd);

void setExStyleBits(HWND hwnd, LONG_PTR bits, bool enabled);

} // namespace qs::windows
