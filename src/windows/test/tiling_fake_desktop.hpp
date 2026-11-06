#pragma once

// Compile-time desktop boundary for the *production* tiling.cpp. No native
// tracker, virtual desktop, util implementation, user32 or DWM import is linked
// into this fixture. The Qt application uses the offscreen QPA plugin.
#include <cmath>
#include <limits>
#include <utility>

#include <qguiapplication.h>
#include <qhash.h>
#include <qpointer.h>
#include <qscreen.h>
#include <qt_windows.h>

#include "../accessor_state.hpp"
#include "../desktop_index.hpp"
#include "../focus_monitor.hpp"
#include "../fullscreen_geometry.hpp"
#include "../window_style_snapshot.hpp"

// Redirect at the translation-unit boundary, including calls from constructor rescue.
#define GetWindowRect fakeGetWindowRect
#define DwmGetWindowAttribute fakeDwmGetWindowAttribute
#define DwmSetWindowAttribute fakeDwmSetWindowAttribute
#define SetWindowPos fakeSetWindowPos
#define IsIconic fakeIsIconic
#define IsZoomed fakeIsZoomed
#define IsHungAppWindow fakeIsHungAppWindow
#define IsWindow fakeIsWindow
#define GetWindow fakeGetWindow
#define IsWindowVisible fakeIsWindowVisible
#define GetWindowLongPtrW fakeGetWindowLongPtrW
#define GetClassNameW fakeGetClassNameW
#define GetWindowDpiAwarenessContext fakeGetWindowDpiAwarenessContext
#define GetAwarenessFromDpiAwarenessContext fakeGetAwarenessFromDpiAwarenessContext
#define GetDpiForWindow fakeGetDpiForWindow
#define SetThreadDpiAwarenessContext fakeSetThreadDpiAwarenessContext
#define GetWindowPlacement fakeGetWindowPlacement
#define SetWindowPlacement fakeSetWindowPlacement
#define GetForegroundWindow fakeGetForegroundWindow
#define GetCursorPos fakeGetCursorPos
#define CreateMutexW fakeCreateMutexW
#define GetLastError fakeGetLastError
#define CloseHandle fakeCloseHandle
#define ReleaseMutex fakeReleaseMutex
#define OpenMutexW fakeOpenMutexW
#define GlobalFindAtomW fakeGlobalFindAtomW
#define GlobalAddAtomW fakeGlobalAddAtomW
#define SetPropW fakeSetPropW
#define RemovePropW fakeRemovePropW
#define GetPropW fakeGetPropW
#define EnumWindows fakeEnumWindows
#define MonitorFromRect fakeMonitorFromRect
#define MonitorFromWindow fakeMonitorFromWindow
#define MonitorFromPoint fakeMonitorFromPoint
#define GetSystemMetrics fakeGetSystemMetrics

namespace qs::windows {

struct FakeWindow {
	enum class Cloak { None, Shell, App };

	QRect frame {40, 60, 600, 400};
	// Keep the restored rectangle in screen coordinates, like GetWindowRect.
	// The placement API below models Win32's workspace-coordinate boundary.
	QRect normal = frame;
	QRect lastPlacement;
	HANDLE parked = nullptr;
	QString appId = QStringLiteral("editor");
	QString title = QStringLiteral("Editor");
	QString className = QStringLiteral("TestWindow");
	bool own = false;
	bool elevated = false;
	bool minimized = false;
	bool maximized = false;
	bool fullscreen = false;
	// DWM cloaking is a visibility change. The tracker keeps the fullscreen
	// episode and its latch; only shell/foreground consumers look at this. A
	// shell cloak is another virtual desktop; an app cloak hides the window.
	Cloak cloak = Cloak::None;
	bool hung = false;
	bool ignoreMoves = false;
	DWORD moveError = 0;
	QSize minimumSize;
	QSize maximumSize;
	bool exists = true;
	// Index into FakeDesktop::screens, or -1 for the fixture's primary screen.
	int monitor = -1;
	// DPI_AWARENESS_* of the window's own process: 0 unaware, 1 system aware,
	// 2 per monitor aware. A virtualized (unaware) window reports its rectangle
	// on a 96-DPI grid that `dpiScale` stretches to physical pixels.
	quint32 dpiAwareness = 2;
	qreal dpiScale = 1.0;
	LONG_PTR style = WS_THICKFRAME;
	LONG_PTR exStyle = 0;
	int moves = 0;
	int placements = 0;
	COLORREF borderColour = 0xffffffff;
	int borderWrites = 0;
};

struct MonitorRects {
	QRect monitor;
	QRect work;
	bool valid = false;
};

struct FakeDesktop {
	QHash<HWND, FakeWindow> windows;
	QList<MonitorRects> monitors {{{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true}};
	// Monitor index -> live QScreen. Empty while a fixture only needs the
	// primary screen.
	QList<QPointer<QScreen>> screens;
	// What the Hyprland compatibility layer last published as focusedmon.
	FocusMonitor focus;
	QPoint cursor;
	qint64 now = 10000;
	bool mutexHeld = false;
	bool mutexOpen = false;
	QString mutexName;
	DWORD error = 0;
	int rescues = 0;
	// DPI awareness of the calling thread. The real tiler asks for physical
	// pixels around a virtualized window; a test can start from the virtualized
	// grid the placement bug was seen from.
	quint32 threadAwareness = 2;
	int awarenessSwitches = 0;
	bool borderSupported = true;
	HWND active = nullptr;
};

inline FakeDesktop fakeDesktop;
inline qint64 tilingNow() { return fakeDesktop.now; }
inline bool borderColoursSupported() { return fakeDesktop.borderSupported; }

inline RECT toRECT(const QRect& r) { return {r.x(), r.y(), r.x() + r.width(), r.y() + r.height()}; }
inline QRect toQRect(const RECT& r) { return {r.left, r.top, r.right - r.left, r.bottom - r.top}; }

// Scale a rectangle between physical pixels and a window's virtualized 96-DPI
// grid, the way Windows draws a DPI-unaware window.
inline QRect scaleRect(const QRect& rect, qreal factor) {
	auto scale = [factor](int value) { return static_cast<int>(std::lround(value * factor)); };
	return {scale(rect.x()), scale(rect.y()), scale(rect.width()), scale(rect.height())};
}

// The compositor rounds a virtualized window onto whole units of its own grid,
// so a physical request lands up to one physical pixel away.
inline QRect snapRect(const QRect& rect, qreal scale) {
	if (scale <= 0.0) return rect;
	auto snap = [scale](int value) {
		return static_cast<int>(std::lround(std::lround(value / scale) * scale));
	};
	return {snap(rect.x()), snap(rect.y()), snap(rect.width()), snap(rect.height())};
}

inline quint32 fakeAwarenessOf(DPI_AWARENESS_CONTEXT context) {
	if (context == DPI_AWARENESS_CONTEXT_UNAWARE) return DPI_AWARENESS_UNAWARE;
	if (context == DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) return DPI_AWARENESS_SYSTEM_AWARE;
	return DPI_AWARENESS_PER_MONITOR_AWARE;
}

inline DPI_AWARENESS_CONTEXT fakeContextOf(quint32 awareness) {
	if (awareness == DPI_AWARENESS_UNAWARE) return DPI_AWARENESS_CONTEXT_UNAWARE;
	if (awareness == DPI_AWARENESS_SYSTEM_AWARE) return DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
	return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
}
inline HMONITOR MonitorFromRect(const RECT* native, DWORD flags) {
	auto rect = toQRect(*native);
	qsizetype best = 0;
	qint64 bestArea = 0;
	qint64 bestDistance = std::numeric_limits<qint64>::max();
	for (qsizetype i = 0; i < fakeDesktop.monitors.size(); i++) {
		auto monitor = fakeDesktop.monitors.at(i).monitor;
		auto overlap = rect.intersected(monitor);
		auto area = qint64(overlap.width()) * overlap.height();
		auto delta = rect.center()
		           - QPoint(
		                 qBound(monitor.left(), rect.center().x(), monitor.right()),
		                 qBound(monitor.top(), rect.center().y(), monitor.bottom())
		           );
		auto distance = qint64(delta.x()) * delta.x() + qint64(delta.y()) * delta.y();
		if (area > bestArea || (area == bestArea && distance < bestDistance)) {
			best = i;
			bestArea = area;
			bestDistance = distance;
		}
	}
	if (bestArea == 0 && flags == MONITOR_DEFAULTTONULL) return nullptr;
	return reinterpret_cast<HMONITOR>(best + 1);
}
inline MonitorRects monitorRects(HMONITOR monitor) {
	auto index = reinterpret_cast<quintptr>(monitor);
	if (index == 0 || index > quintptr(fakeDesktop.monitors.size())) return {};
	return fakeDesktop.monitors.at(index - 1);
}
inline FakeWindow& fakeWindow(HWND h) {
	if (!fakeDesktop.windows.contains(h)) qFatal("Attempt to access a non-fixture HWND");
	return fakeDesktop.windows[h];
}
inline bool isOwnProcessWindow(HWND h) { return fakeWindow(h).own; }
inline bool isMoreElevated(HWND h) { return fakeWindow(h).elevated; }
inline BOOL GetWindowRect(HWND h, RECT* r) {
	auto& window = fakeWindow(h);
	if (!window.exists) {
		fakeDesktop.error = ERROR_INVALID_WINDOW_HANDLE;
		return FALSE;
	}
	auto rect = window.frame;
	// A window that is not per-monitor aware reports its rectangle on its own
	// scaled grid unless the reader is per-monitor aware too. This is the
	// mismatch that made every placement look wrong.
	if (window.dpiAwareness != DPI_AWARENESS_PER_MONITOR_AWARE
	    && fakeDesktop.threadAwareness != DPI_AWARENESS_PER_MONITOR_AWARE)
	{
		rect = scaleRect(rect, 1.0 / window.dpiScale);
	}
	*r = toRECT(rect);
	return TRUE;
}
inline HRESULT DwmGetWindowAttribute(HWND h, DWORD, void* r, DWORD size) {
	if (size != sizeof(RECT)) qFatal("Unexpected native DWM query in fixture");
	// The extended frame bounds stay in physical pixels, whatever the caller.
	*static_cast<RECT*>(r) = toRECT(fakeWindow(h).frame);
	return S_OK;
}
inline HRESULT DwmSetWindowAttribute(HWND h, DWORD attribute, const void* value, DWORD size) {
	if (attribute != 34 || size != sizeof(COLORREF)) qFatal("Unexpected native DWM write in fixture");
	auto& window = fakeWindow(h);
	window.borderColour = *static_cast<const COLORREF*>(value);
	window.borderWrites++;
	return S_OK;
}
inline BOOL SetWindowPos(HWND h, HWND, int x, int y, int w, int height, UINT) {
	auto& window = fakeWindow(h);
	window.moves++;
	if (window.moveError != 0 || !window.exists) {
		fakeDesktop.error = window.exists ? window.moveError : ERROR_INVALID_WINDOW_HANDLE;
		return FALSE;
	}
	if (!window.ignoreMoves) {
		auto rect = QRect(x, y, w, height);
		if (window.dpiAwareness != DPI_AWARENESS_PER_MONITOR_AWARE
		    && fakeDesktop.threadAwareness != DPI_AWARENESS_PER_MONITOR_AWARE)
		{
			// The request is taken on the window's virtualized grid, so the
			// physical result drifts away from the requested tile.
			rect = scaleRect(rect, window.dpiScale);
		} else if (window.dpiAwareness != DPI_AWARENESS_PER_MONITOR_AWARE) {
			rect = snapRect(rect, window.dpiScale);
		}
		rect.setSize(rect.size().expandedTo(window.minimumSize));
		if (window.maximumSize.isValid()) rect.setSize(rect.size().boundedTo(window.maximumSize));
		window.frame = rect;
	}
	return TRUE;
}
inline DPI_AWARENESS_CONTEXT GetWindowDpiAwarenessContext(HWND h) {
	return fakeContextOf(fakeWindow(h).dpiAwareness);
}
inline DPI_AWARENESS GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT context) {
	return static_cast<DPI_AWARENESS>(fakeAwarenessOf(context));
}
inline UINT GetDpiForWindow(HWND h) {
	auto& window = fakeWindow(h);
	if (window.dpiAwareness == DPI_AWARENESS_PER_MONITOR_AWARE) {
		return static_cast<UINT>(std::lround(96.0 * window.dpiScale));
	}
	return 96; // A virtualized window is told it runs at 96 DPI.
}
inline DPI_AWARENESS_CONTEXT SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT context) {
	auto previous = fakeDesktop.threadAwareness;
	fakeDesktop.threadAwareness = fakeAwarenessOf(context);
	fakeDesktop.awarenessSwitches++;
	return fakeContextOf(previous);
}
inline BOOL IsIconic(HWND h) { return fakeWindow(h).minimized; }
inline BOOL IsZoomed(HWND h) { return fakeWindow(h).maximized; }
inline BOOL IsHungAppWindow(HWND h) { return fakeWindow(h).hung; }
inline BOOL IsWindow(HWND h) {
	return fakeDesktop.windows.contains(h) && fakeWindow(h).exists;
}
inline HWND GetWindow(HWND h, UINT) {
	fakeWindow(h);
	return nullptr;
}
inline BOOL IsWindowVisible(HWND h) {
	fakeWindow(h);
	return TRUE;
}
inline LONG_PTR GetWindowLongPtrW(HWND h, int index) {
	return index == GWL_STYLE ? fakeWindow(h).style : fakeWindow(h).exStyle;
}
inline int GetClassNameW(HWND h, LPWSTR buffer, int size) {
	auto name = fakeWindow(h).className.left(size - 1);
	name.toWCharArray(buffer);
	buffer[name.size()] = 0;
	return static_cast<int>(name.size());
}
inline BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT* p) {
	auto& window = fakeWindow(h);
	auto native = toRECT(window.normal);
	auto rects = monitorRects(MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST));
	auto offset = (window.exStyle & WS_EX_TOOLWINDOW)
	                ? QPoint()
	                : rects.work.topLeft() - rects.monitor.topLeft();
	p->rcNormalPosition = toRECT(window.normal.translated(-offset));
	p->showCmd =
	    window.minimized ? SW_SHOWMINIMIZED : (window.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
	return TRUE;
}
inline BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT* p) {
	auto& window = fakeWindow(h);
	window.placements++;
	window.lastPlacement = toQRect(p->rcNormalPosition);
	auto rects = monitorRects(MonitorFromRect(&p->rcNormalPosition, MONITOR_DEFAULTTONEAREST));
	auto offset = (window.exStyle & WS_EX_TOOLWINDOW)
	                ? QPoint()
	                : rects.work.topLeft() - rects.monitor.topLeft();
	window.normal = window.lastPlacement.translated(offset);
	window.minimized = p->showCmd == SW_SHOWMINIMIZED;
	window.maximized = p->showCmd == SW_SHOWMAXIMIZED;
	return TRUE;
}
inline HWND GetForegroundWindow() { return nullptr; }
inline BOOL GetCursorPos(POINT* p) {
	*p = {fakeDesktop.cursor.x(), fakeDesktop.cursor.y()};
	return TRUE;
}
// The default drag threshold; no other metric is read by the tiler.
inline int GetSystemMetrics(int index) {
	if (index != SM_CXDRAG && index != SM_CYDRAG) qFatal("Unexpected system metric in fixture");
	return 4;
}
inline HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR name) {
	fakeDesktop.mutexName = QString::fromWCharArray(name);
	fakeDesktop.error = fakeDesktop.mutexHeld ? ERROR_ALREADY_EXISTS : 0;
	fakeDesktop.mutexOpen = true;
	return reinterpret_cast<HANDLE>(1);
}
inline DWORD GetLastError() { return fakeDesktop.error; }
inline BOOL CloseHandle(HANDLE) {
	fakeDesktop.mutexOpen = false;
	return TRUE;
}
inline BOOL ReleaseMutex(HANDLE) { return TRUE; }
inline HANDLE OpenMutexW(DWORD, BOOL, LPCWSTR) {
	fakeDesktop.rescues++;
	return nullptr;
}
inline ATOM GlobalFindAtomW(LPCWSTR) { return 1; }
inline ATOM GlobalAddAtomW(LPCWSTR) { return 1; }
inline BOOL SetPropW(HWND h, LPCWSTR, HANDLE value) {
	fakeWindow(h).parked = value;
	return TRUE;
}
inline HANDLE RemovePropW(HWND h, LPCWSTR) { return std::exchange(fakeWindow(h).parked, nullptr); }
inline HANDLE GetPropW(HWND h, LPCWSTR) { return fakeWindow(h).parked; }
inline BOOL EnumWindows(WNDENUMPROC callback, LPARAM param) {
	for (auto h: fakeDesktop.windows.keys()) {
		if (!callback(h, param)) return FALSE;
	}
	return TRUE;
}
inline HMONITOR MonitorFromWindow(HWND h, DWORD flags) {
	auto& window = fakeWindow(h);
	auto native = toRECT(window.minimized || window.maximized ? window.normal : window.frame);
	return MonitorFromRect(&native, flags);
}
inline HMONITOR MonitorFromPoint(POINT point, DWORD flags) {
	auto rect = RECT {point.x, point.y, point.x + 1, point.y + 1};
	return MonitorFromRect(&rect, flags);
}
inline HMONITOR monitorForScreen(QScreen* screen) {
	for (qsizetype i = 0; i < fakeDesktop.screens.size(); i++) {
		if (fakeDesktop.screens.at(i) == screen) return reinterpret_cast<HMONITOR>(i + 1);
	}
	return reinterpret_cast<HMONITOR>(1);
}
inline QScreen* screenForMonitor(HMONITOR monitor) {
	auto index = reinterpret_cast<quintptr>(monitor);
	if (index == 0 || index > quintptr(fakeDesktop.screens.size())) return nullptr;
	return fakeDesktop.screens.at(index - 1).data();
}
inline QString screenName(QScreen* screen) { return screen ? screen->name() : QString(); }
inline QScreen* focusedScreen() {
	auto* screen = fakeDesktop.focus.screen();
	return screen != nullptr ? screen : QGuiApplication::primaryScreen();
}

class VirtualDesktops: public AccessorState {
	Q_OBJECT;

public:
	struct Desktop {
		GUID id {};
		QString name;
	};
	using AccessorState::setAccessorLoaded;
	using AccessorState::setPinExports;
	const QList<Desktop>& desktops() const {
		static QList<Desktop> list {Desktop {}};
		return list;
	}
	qsizetype count() const { return 1; }
	qsizetype currentIndex() const { return 0; }
	qsizetype indexOf(const GUID& id) const { return desktopIndexOf(this->desktops(), id); }
	static QString guidToString(const GUID&) { return QStringLiteral("fake-desktop"); }
signals:
	void desktopsChanged();
};

class TrackedWindow: public QObject {
	Q_OBJECT;

public:
	explicit TrackedWindow(quintptr id): handle(reinterpret_cast<HWND>(id)) {}
	HWND hwnd() const { return handle; }
	QString addressHex() const { return QString::number(reinterpret_cast<quintptr>(handle), 16); }
	QString appId() const { return fakeWindow(handle).appId; }
	QString exePath() const { return appId() + QStringLiteral(".exe"); }
	QString title() const { return fakeWindow(handle).title; }
	QRect rect() const { return fakeWindow(handle).frame; }
	QScreen* screen() const {
		auto index = fakeWindow(handle).monitor;
		if (index >= 0 && index < fakeDesktop.screens.size())
			return fakeDesktop.screens.at(index).data();
		return QGuiApplication::primaryScreen();
	}
	bool minimized() const { return fakeWindow(handle).minimized; }
	bool maximized() const { return fakeWindow(handle).maximized; }
	bool fullscreen() const { return fakeWindow(handle).fullscreen; }
	bool cloaked() const { return fakeWindow(handle).cloak != FakeWindow::Cloak::None; }
	bool appCloaked() const { return fakeWindow(handle).cloak == FakeWindow::Cloak::App; }
	bool hidden() const { return this->minimized() || this->appCloaked(); }
	void refreshState() {
		auto& native = fakeWindow(handle);
		// Production refresh keeps the fullscreen episode and latch across a
		// cloak change. The fixture stores that episode directly, so a cloak
		// refresh must not fold visibility into `fullscreen`.
		if (styleSnapshot.update(native.style, native.exStyle)) emit this->styleChanged();
	}
	// Derive the episode from geometry the way the fixed tracker does. DWM
	// cloaking is deliberately not an input: a cloak is not a fullscreen exit.
	void refreshFullscreenEpisode() {
		auto& native = fakeWindow(handle);
		auto rects = monitorRects(monitorForScreen(screen()));
		auto detection = fullscreen_geometry::detect(
		    native.minimized,
		    native.maximized,
		    native.frame,
		    rects.monitor,
		    1,
		    this->fullscreenLatch
		);
		this->fullscreenLatch = detection.latch;
		if (detection.fullscreen == native.fullscreen) return;
		native.fullscreen = detection.fullscreen;
		emit this->fullscreenChanged();
	}
	int desktop() const { return 0; }
	void activate() { qFatal("Unexpected focus mutation in fixture"); }
signals:
	void minimizedChanged();
	void appCloakedChanged();
	void maximizedChanged();
	void fullscreenChanged();
	void styleChanged();
	void desktopChanged();
	void screenChanged();
	void titleChanged();
	void rectChanged();

private:
	HWND handle;
	WindowStyleSnapshot styleSnapshot;
	fullscreen_geometry::Latch fullscreenLatch;
};

class WindowTracker: public QObject {
	Q_OBJECT;

public:
	static WindowTracker* instance() {
		static WindowTracker tracker;
		return &tracker;
	}
	QList<TrackedWindow*> fixtures;
	VirtualDesktops fakeDesktops;
	const QList<TrackedWindow*>& windows() const { return fixtures; }
	VirtualDesktops* desktops() { return &fakeDesktops; }
	TrackedWindow* activeWindow() const {
		for (auto* window: fixtures) {
			if (window->hwnd() == fakeDesktop.active) return window;
		}
		return nullptr;
	}
	TrackedWindow* windowFor(HWND) const { return nullptr; }
	QScreen* screenFor(HMONITOR monitor) const {
		auto* screen = screenForMonitor(monitor);
		return screen != nullptr ? screen : QGuiApplication::primaryScreen();
	}
	void refreshForeground() { qFatal("Unexpected foreground refresh in fixture"); }
	// Production publishes every EVENT_SYSTEM_FOREGROUND on the raw stream and
	// only then updates the active window, so a foreground event for the window
	// that is already active changes nothing. Deliver both halves in that order.
	void deliverForegroundEvent(HWND hwnd) {
		auto* previous = this->activeWindow();
		emit this->foregroundEvent(hwnd, static_cast<quint32>(fakeDesktop.now), 0);
		fakeDesktop.active = hwnd;
		if (this->activeWindow() != previous) emit this->activeWindowChanged();
	}
signals:
	void windowAdded(TrackedWindow*);
	void windowRemoved(TrackedWindow*);
	void activeWindowChanged();
	void foregroundEvent(HWND hwnd, quint32 time, quint64 sequence);
	void moveSizeStarted(TrackedWindow*, bool super);
	void moveSizeEnded(TrackedWindow*);
	void movingLocationChanged(TrackedWindow*);
};

} // namespace qs::windows
