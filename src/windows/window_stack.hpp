#pragma once

#include <functional>

#include <qlist.h>
#include <qrect.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtypes.h>
#include <qvariant.h>

#include "system_surface.hpp"

// A one-shot snapshot of the application windows from the topmost to the
// bottom one, with frame bounds in Qt logical global coordinates.
//
// The Hyprland toplevel list and the WindowTracker have no stacking order.
// A screenshot picker needs "the topmost window under the cursor", so this
// asks Windows once, when the picker opens, instead of tracking order live.
// EnumWindows walks the top-level windows from the topmost down, which is the
// order returned here. Nothing is cached, hooked or kept between calls.
namespace qs::windows::window_stack {

struct Entry {
	// The window handle in lowercase hex. This is the address the Hyprland
	// toplevel of the same window reports, and nothing else in it is meaningful.
	QString id;
	QString title;
	// DWMWA_EXTENDED_FRAME_BOUNDS, the visible frame without the invisible resize
	// border, mapped with the same ScreenMapper the WindowTracker uses.
	QRect rect;

	[[nodiscard]] bool operator==(const Entry&) const = default;
};

// What a snapshot asks of the process. The defaults are the production
// behaviour. A fixture replaces them to create its windows in the test
// process and to map without a real monitor.
struct Hooks {
	// Windows of this process are never listed.
	std::function<bool(HWND)> ownProcess;
	// Physical virtual-desktop frame to logical global coordinates. An empty
	// result drops the window, as for a monitor Qt has no screen for.
	std::function<QRect(HWND, const QRect& physical)> toLogical;
};

[[nodiscard]] QString windowId(HWND hwnd);

// Whether an observed window is a listed application window. Visible, a root
// window, not cloaked (another virtual desktop or a suspended app), with a title,
// not a Windows shell surface (the desktop, taskbars, Start, the task switcher)
// and not a tooltip or popup (tool windows and no-activate windows, unless the
// application forces a taskbar button with WS_EX_APPWINDOW).
//
// Owned dialogs are listed: a dialog is a window the user may want to capture,
// and leaving it out would make its owner behind it win the hit test.
[[nodiscard]] bool isStackWindow(const system_surface::Identity& identity);

// The physical frame as logical global coordinates, with the mapping
// TrackedWindow::refreshState uses.
[[nodiscard]] QRect logicalFrame(
    const QRect& physicalFrame,
    const QRect& physicalMonitor,
    const QRect& logicalScreen,
    qreal devicePixelRatio
);

[[nodiscard]] QList<Entry> snapshot();
[[nodiscard]] QList<Entry> snapshot(const Hooks& hooks);

// For QML: a list of {id, title, rect} objects, topmost first.
[[nodiscard]] QVariantList toVariantList(const QList<Entry>& entries);

} // namespace qs::windows::window_stack
