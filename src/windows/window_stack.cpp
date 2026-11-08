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

QRect mapThroughNearestScreen(HWND hwnd, const QRect& physical) {
	auto* monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	auto* screen = screenForMonitor(monitor);
	auto rects = monitorRects(monitor);
	if (screen == nullptr || !rects.valid) return {};

	return logicalFrame(physical, rects.monitor, screen->geometry(), screen->devicePixelRatio());
}

struct Walk {
	const Hooks* hooks = nullptr;
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

	auto logical = walk.hooks->toLogical(hwnd, physical);
	if (logical.isEmpty()) return TRUE;

	walk.entries->append({.id = windowId(hwnd), .title = std::move(identity.title), .rect = logical});
	return TRUE;
}

} // namespace

QString windowId(HWND hwnd) {
	return QString::number(static_cast<quint64>(reinterpret_cast<quintptr>(hwnd)), 16);
}

bool isStackWindow(const system_surface::Identity& identity) {
	if (!identity.visible || !identity.root || identity.child || identity.cloaked) return false;
	if (!identity.hasTitle) return false;
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

QList<Entry> snapshot() { return snapshot(Hooks()); }

QList<Entry> snapshot(const Hooks& requested) {
	auto hooks = requested;
	if (!hooks.ownProcess) hooks.ownProcess = &isOwnProcessWindow;
	if (!hooks.toLogical) hooks.toLogical = &mapThroughNearestScreen;

	auto entries = QList<Entry>();
	auto walk = Walk {.hooks = &hooks, .entries = &entries};
	EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&walk));
	return entries;
}

QVariantList toVariantList(const QList<Entry>& entries) {
	auto list = QVariantList();
	list.reserve(entries.size());

	for (const auto& entry: entries) {
		list.append(QVariantMap {
		    {QStringLiteral("id"), entry.id},
		    {QStringLiteral("title"), entry.title},
		    {QStringLiteral("rect"), QRectF(entry.rect)},
		});
	}

	return list;
}

} // namespace qs::windows::window_stack
