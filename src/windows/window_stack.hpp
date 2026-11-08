#pragma once

#include <functional>

#include <qlist.h>
#include <qrect.h>
#include <qscreen.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtypes.h>
#include <qvariant.h>

#include "system_surface.hpp"

// A one-shot snapshot of the application windows from the topmost to the
// bottom one, with frame bounds in Qt logical global coordinates, one piece
// per screen the window covers.
//
// A window that straddles monitors of different scale has no single correct
// logical rectangle: Qt places each screen in logical space with its own scale.
// The physical frame is therefore cut at the monitor borders and each cut is
// mapped through its own monitor's transform.
//
// The Hyprland toplevel list and the WindowTracker have no stacking order.
// A screenshot picker needs "the topmost window under the cursor", so this
// asks Windows once, when the picker opens, instead of tracking order live.
// EnumWindows walks the top-level windows from the topmost down, which is the
// order returned here. Nothing is cached, hooked or kept between calls.
namespace qs::windows::window_stack {

// One screen as the mapping needs it.
struct Monitor {
	// The QML-facing name from the screen identity registry, the string
	// ShellScreen.name reports. Not QScreen::name(): that repeats for identical
	// monitors and can stay stale when Qt reuses a QScreen for another monitor.
	QString screen;
	// The monitor rectangle in physical virtual-desktop pixels.
	QRect physical;
	// QScreen::geometry(), the screen in Qt logical global coordinates.
	QRect logical;
	qreal devicePixelRatio = 1;

	[[nodiscard]] bool operator==(const Monitor&) const = default;
};

// The part of a window on one screen, in that screen's logical coordinates.
struct Piece {
	QString screen;
	QRect rect;

	[[nodiscard]] bool operator==(const Piece&) const = default;
};

struct Entry {
	// The window handle in lowercase hex. This is the address the Hyprland
	// toplevel of the same window reports, and nothing else in it is meaningful.
	QString id;
	// Empty for a window without a caption. It is still listed.
	QString title;
	// DWMWA_EXTENDED_FRAME_BOUNDS, the visible frame without the invisible resize
	// border, cut at the monitor borders. One piece per monitor it overlaps, in
	// the order Qt lists the screens.
	QList<Piece> pieces;

	[[nodiscard]] bool operator==(const Entry&) const = default;
};

// How a QScreen becomes a Monitor. The defaults are the production behaviour.
struct ScreenSource {
	// The QML-facing name. Defaults to qs::windows::screenName.
	std::function<QString(QScreen*)> name;
	// The monitor rectangle in physical pixels, empty when unknown. Defaults to
	// the native monitor of the screen.
	std::function<QRect(QScreen*)> physical;
};

// The screens as Monitors, in the order given. A screen without a physical
// rectangle is skipped.
[[nodiscard]] QList<Monitor>
monitorsFor(const QList<QScreen*>& screens, const ScreenSource& source = ScreenSource());

// What a snapshot asks of the process. The defaults are the production
// behaviour. A fixture replaces them to create its windows in the test
// process and to map without real monitors.
struct Hooks {
	// Windows of this process are never listed.
	std::function<bool(HWND)> ownProcess;
	// The screens, read once per snapshot. A window that overlaps none of them
	// is dropped.
	std::function<QList<Monitor>()> monitors;
};

[[nodiscard]] QString windowId(HWND hwnd);

// Whether an observed window is a listed application window. Visible, a root
// window, not cloaked (another virtual desktop or a suspended app), not a
// Windows shell surface (the desktop, taskbars, Start, the task switcher) and
// not a tooltip or popup (tool windows and no-activate windows, unless the
// application forces a taskbar button with WS_EX_APPWINDOW).
//
// A window without a title is listed: not every application window has a
// native caption. Owned dialogs are listed too: a dialog is a window the user
// may want to capture, and leaving it out would make its owner behind it win
// the hit test.
[[nodiscard]] bool isStackWindow(const system_surface::Identity& identity);

// The physical frame as logical global coordinates on one monitor, with the
// mapping TrackedWindow::refreshState uses.
[[nodiscard]] QRect logicalFrame(
    const QRect& physicalFrame,
    const QRect& physicalMonitor,
    const QRect& logicalScreen,
    qreal devicePixelRatio
);

// The frame cut at each monitor border, each cut mapped through its own
// monitor. Empty when the frame overlaps no monitor.
[[nodiscard]] QList<Piece> logicalPieces(const QRect& physicalFrame, const QList<Monitor>& monitors);

[[nodiscard]] QList<Entry> snapshot();
[[nodiscard]] QList<Entry> snapshot(const Hooks& hooks);

// For QML: a list of {id, title, rects} objects, topmost first. `rects` is a
// list of {screen, rect} where `rect` has x, y, width and height.
[[nodiscard]] QVariantList toVariantList(const QList<Entry>& entries);

} // namespace qs::windows::window_stack
