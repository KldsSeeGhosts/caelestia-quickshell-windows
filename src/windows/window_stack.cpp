#include "window_stack.hpp"

#include <utility>

#include <dwmapi.h>
#include <qguiapplication.h>
#include <qlist.h>
#include <qmap.h>
#include <qrect.h>
#include <qscreen.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtypes.h>
#include <qvariant.h>

#include "system_surface.hpp"
#include "util.hpp"

namespace qs::windows::window_stack {

namespace {

QList<Monitor> qtScreens() {
	auto monitors = QList<Monitor>();

	for (auto* screen: QGuiApplication::screens()) {
		auto rects = monitorRects(monitorForScreen(screen));
		if (!rects.valid) continue;

		monitors.append(
		    {.screen = screen->name(),
		     .physical = rects.monitor,
		     .logical = screen->geometry(),
		     .devicePixelRatio = screen->devicePixelRatio()}
		);
	}

	return monitors;
}

struct Walk {
	const Hooks* hooks = nullptr;
	const QList<Monitor>* monitors = nullptr;
	QList<Entry>* entries = nullptr;
};

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param) {
	auto& walk = *reinterpret_cast<Walk*>(param); // NOLINT(performance-no-int-to-ptr)

	// The cheap checks run first. A typical desktop has a few hundred top-level
	// windows and a few dozen of them are visible.
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
	if (walk.hooks->ownProcess(hwnd)) return TRUE;
	if (system_surface::isCloaked(hwnd)) return TRUE;

	auto identity = system_surface::observe(hwnd);
	if (!isStackWindow(identity)) return TRUE;

	// The same frame as TrackedWindow::refreshState.
	RECT raw {};
	GetWindowRect(hwnd, &raw);
	RECT frame {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
		frame = raw;
	}

	auto physical = toQRect(frame);
	if (physical.isEmpty()) return TRUE;

	auto pieces = logicalPieces(physical, *walk.monitors);
	if (pieces.isEmpty()) return TRUE;

	walk.entries->append(
	    {.id = windowId(hwnd), .title = std::move(identity.title), .pieces = std::move(pieces)}
	);
	return TRUE;
}

} // namespace

QString windowId(HWND hwnd) {
	return QString::number(static_cast<quint64>(reinterpret_cast<quintptr>(hwnd)), 16);
}

bool isStackWindow(const system_surface::Identity& identity) {
	if (!identity.visible || !identity.root || identity.child || identity.cloaked) return false;
	if (system_surface::isShellSurface(identity)) return false;

	// A tooltip, a menu or an overlay is a tool window or never takes focus. An
	// application that wants a taskbar button says so with WS_EX_APPWINDOW. An
	// owned window is not excluded here, unlike in the taskbar rule.
	return identity.appWindow || (!identity.toolWindow && !identity.noActivate);
}

QRect logicalFrame(
    const QRect& physicalFrame,
    const QRect& physicalMonitor,
    const QRect& logicalScreen,
    qreal devicePixelRatio
) {
	return ScreenMapper(logicalScreen, physicalMonitor, devicePixelRatio).toLogical(physicalFrame);
}

QList<Piece> logicalPieces(const QRect& physicalFrame, const QList<Monitor>& monitors) {
	auto pieces = QList<Piece>();

	for (const auto& monitor: monitors) {
		auto part = physicalFrame.intersected(monitor.physical);
		if (part.isEmpty()) continue;

		auto rect = logicalFrame(part, monitor.physical, monitor.logical, monitor.devicePixelRatio);
		if (rect.isEmpty()) continue;

		pieces.append({.screen = monitor.screen, .rect = rect});
	}

	return pieces;
}

QList<Entry> snapshot() { return snapshot(Hooks()); }

QList<Entry> snapshot(const Hooks& requested) {
	auto hooks = requested;
	if (!hooks.ownProcess) hooks.ownProcess = &isOwnProcessWindow;
	if (!hooks.monitors) hooks.monitors = &qtScreens;

	auto monitors = hooks.monitors();
	auto entries = QList<Entry>();
	auto walk = Walk {.hooks = &hooks, .monitors = &monitors, .entries = &entries};
	EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&walk));
	return entries;
}

QVariantList toVariantList(const QList<Entry>& entries) {
	auto list = QVariantList();
	list.reserve(entries.size());

	for (const auto& entry: entries) {
		auto rects = QVariantList();
		rects.reserve(entry.pieces.size());

		for (const auto& piece: entry.pieces) {
			rects.append(QVariantMap {
			    {QStringLiteral("screen"), piece.screen},
			    {QStringLiteral("rect"), QRectF(piece.rect)},
			});
		}

		list.append(QVariantMap {
		    {QStringLiteral("id"), entry.id},
		    {QStringLiteral("title"), entry.title},
		    {QStringLiteral("rects"), rects},
		});
	}

	return list;
}

} // namespace qs::windows::window_stack
