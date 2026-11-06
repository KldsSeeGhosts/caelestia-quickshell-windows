#pragma once

#include <vector>

#include <qt_windows.h>

#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qproperty.h>
#include <qrect.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "fullscreen_geometry.hpp"
#include "window_style_snapshot.hpp"

namespace qs::windows {

class WindowTracker;
class VirtualDesktops;

struct WindowEvent {
	DWORD event = 0;
	HWND hwnd = nullptr;
	DWORD time = 0;
	// Strict order of arrival from the hook thread. The event time is only a
	// millisecond tick and two events raised in the same millisecond share it.
	quint64 sequence = 0;
};

class TrackedWindow: public QObject {
	Q_OBJECT;

public:
	[[nodiscard]] HWND hwnd() const { return this->mHwnd; }
	[[nodiscard]] quint64 address() const;
	[[nodiscard]] QString addressHex() const;
	[[nodiscard]] TrackedWindow* owner() const;

	// clang-format off
	[[nodiscard]] QBindable<QString> bindableTitle() { return &this->bTitle; }
	[[nodiscard]] QBindable<QString> bindableAppId() { return &this->bAppId; }
	[[nodiscard]] QBindable<QString> bindableExePath() { return &this->bExePath; }
	[[nodiscard]] QBindable<quint32> bindablePid() { return &this->bPid; }
	[[nodiscard]] QBindable<QScreen*> bindableScreen() { return &this->bScreen; }
	[[nodiscard]] QBindable<QRect> bindableRect() { return &this->bRect; }
	[[nodiscard]] QBindable<bool> bindableMinimized() { return &this->bMinimized; }
	[[nodiscard]] QBindable<bool> bindableMaximized() { return &this->bMaximized; }
	[[nodiscard]] QBindable<bool> bindableFullscreen() { return &this->bFullscreen; }
	[[nodiscard]] QBindable<bool> bindableCloaked() { return &this->bCloaked; }
	[[nodiscard]] QBindable<bool> bindableAppCloaked() { return &this->bAppCloaked; }
	[[nodiscard]] QBindable<bool> bindableActivated() { return &this->bActivated; }
	[[nodiscard]] QBindable<qint32> bindableDesktop() { return &this->bDesktop; }
	// clang-format on

	[[nodiscard]] QString title() const { return this->bTitle.value(); }
	[[nodiscard]] QString appId() const { return this->bAppId.value(); }
	[[nodiscard]] QString exePath() const { return this->bExePath.value(); }
	[[nodiscard]] quint32 pid() const { return this->bPid.value(); }
	// Process and thread of the frame window itself, recorded when the tracker
	// adopted it (pid() is the core process for a UWP frame). A consumer that
	// keeps the HWND can compare these to reject a recycled handle.
	[[nodiscard]] quint32 frameProcessId() const { return this->mFrameProcessId; }
	[[nodiscard]] quint32 frameThreadId() const { return this->mFrameThreadId; }
	[[nodiscard]] QScreen* screen() const { return this->bScreen.value(); }
	[[nodiscard]] QRect rect() const { return this->bRect.value(); }
	[[nodiscard]] bool minimized() const { return this->bMinimized.value(); }
	[[nodiscard]] bool maximized() const { return this->bMaximized.value(); }
	[[nodiscard]] bool fullscreen() const { return this->bFullscreen.value(); }
	// Visibility on the current desktop, separate from the fullscreen episode.
	// A cloaked window keeps its fullscreen latch for the tiler but must not
	// hide the shell or count as a foreground application.
	[[nodiscard]] bool cloaked() const { return this->bCloaked.value(); }
	// True for DWM_CLOAKED_APP / DWM_CLOAKED_INHERITED only. A shell cloak is
	// another virtual desktop and keeps the normal mapped state.
	[[nodiscard]] bool appCloaked() const { return this->bAppCloaked.value(); }
	// What the tiler treats like a minimized window: an app-cloaked window is
	// invisible but keeps its fullscreen episode and latch.
	[[nodiscard]] bool hidden() const { return this->minimized() || this->appCloaked(); }
	[[nodiscard]] bool activated() const { return this->bActivated.value(); }
	[[nodiscard]] qint32 desktop() const { return this->bDesktop.value(); }
	[[nodiscard]] bool isUwpFrame() const { return this->uwpFrame; }

	// True when the shared foreground policy accepted this one request.
	bool activate();
	void close();
	void setMinimized(bool minimized);
	void setMaximized(bool maximized);
	void setFullscreen(bool fullscreen);
	void fullscreenOn(QScreen* screen);
	bool moveToDesktop(qsizetype index);
	void moveTo(const QPoint& logical);

signals:
	void closed();
	void titleChanged();
	void appIdChanged();
	void exePathChanged();
	void pidChanged();
	void screenChanged();
	void rectChanged();
	void minimizedChanged();
	void maximizedChanged();
	void fullscreenChanged();
	void cloakedChanged();
	void appCloakedChanged();
	void styleChanged();
	void activatedChanged();
	void desktopChanged();

private:
	friend class WindowTracker;

	void forgetScreen(QScreen* screen);

	explicit TrackedWindow(WindowTracker* tracker, HWND hwnd);

	void refreshIdentity();
	void refreshTitle();
	void refreshState();
	void refreshDesktop(bool askShell);

	WindowTracker* tracker;
	HWND mHwnd;
	bool uwpFrame = false;
	quint32 mFrameProcessId = 0;
	quint32 mFrameThreadId = 0;
	bool traceObserved = false;
	LONG_PTR traceStyle = 0;
	LONG_PTR traceExStyle = 0;
	QRect traceRect;
	WindowStyleSnapshot styleSnapshot;

	struct {
		bool active = false;
		LONG_PTR style = 0;
		RECT rect {};
	} fullscreenBackup;

	// Exact fullscreen episode: the frame and monitor that were classified
	// fullscreen. It bridges a display-mode or DPI change, where the monitor and
	// the window briefly disagree about their size, but never a different frame.
	fullscreen_geometry::Latch fullscreenLatch;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bTitle, &TrackedWindow::titleChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bAppId, &TrackedWindow::appIdChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bExePath, &TrackedWindow::exePathChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, quint32, bPid, &TrackedWindow::pidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QScreen*, bScreen, &TrackedWindow::screenChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QRect, bRect, &TrackedWindow::rectChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bMinimized, &TrackedWindow::minimizedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bMaximized, &TrackedWindow::maximizedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bFullscreen, &TrackedWindow::fullscreenChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bCloaked, &TrackedWindow::cloakedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bAppCloaked, &TrackedWindow::appCloakedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bActivated, &TrackedWindow::activatedChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(TrackedWindow, qint32, bDesktop, -1, &TrackedWindow::desktopChanged);
	// clang-format on
};

class WindowTracker: public QObject {
	Q_OBJECT;

public:
	static WindowTracker* instance();
	// Null until the first instance() call. A configuration setter uses this to
	// re-evaluate live windows without starting the tracker just to store a
	// list that no consumer needs yet.
	static WindowTracker* instanceIfCreated();

	// Sequence of the last event the hook thread queued, including events not
	// yet drained. A consumer can record it as an ordering boundary: any event
	// queued before the read carries a sequence at or below it.
	[[nodiscard]] quint64 eventSequence() const;

	void drainEvents();

	[[nodiscard]] const QList<TrackedWindow*>& windows() const { return this->mWindows; }
	[[nodiscard]] TrackedWindow* activeWindow() const { return this->mActive; }
	[[nodiscard]] TrackedWindow* windowFor(HWND hwnd) const { return this->byHwnd.value(hwnd); }
	[[nodiscard]] VirtualDesktops* desktops() const { return this->mDesktops; }

	[[nodiscard]] QScreen* screenFor(HMONITOR monitor) const;
	[[nodiscard]] static qsizetype screenIndex(QScreen* screen);

	void rescan();
	void refreshForeground();
	// The start or end of a Super drag, which moves the window without a
	// native move-size loop.
	void noteMoveSize(HWND hwnd, bool started);

signals:
	void windowAdded(TrackedWindow* window);
	void windowRemoved(TrackedWindow* window);
	void activeWindowChanged();
	void flushed();
	// `super` is a Super drag (noteMoveSize); otherwise the native loop.
	void moveSizeStarted(TrackedWindow* window, bool super);
	void moveSizeEnded(TrackedWindow* window);
	// Every location change of a window inside a move-size loop, before the
	// tracker's own coalesced refresh. Keep handlers cheap.
	void movingLocationChanged(TrackedWindow* window);
	void foregroundChanged(HWND hwnd, quint32 time);
	// Every EVENT_SYSTEM_FOREGROUND, in queue order. Unlike foregroundChanged,
	// this is emitted for a window that is already active, so a consumer can
	// see the user return to the retained application. `sequence` orders the
	// event against a boundary taken with eventSequence().
	void foregroundEvent(HWND hwnd, quint32 time, quint64 sequence);
	// A title change for the current foreground window or the retained active
	// application. The foreground observer uses this instead of sampling titles.
	void foregroundNameChanged(HWND hwnd);
	void stackingChanged();

private:
	explicit WindowTracker();
	~WindowTracker() override;
	Q_DISABLE_COPY_MOVE(WindowTracker);

	struct Dirty {
		bool eligibility = false;
		bool title = false;
		bool state = false;
		bool desktop = false;
		bool askShell = false;
	};

	void startEventThread();
	void onEvent(DWORD event, HWND hwnd, DWORD time, quint64 sequence);
	void moveSize(HWND hwnd, bool started, bool super);
	void schedule();
	void flush();

	bool isEligible(HWND hwnd) const;
	void addWindow(HWND hwnd);
	void removeWindow(TrackedWindow* window);
	bool sweepDestroyed();
	void updateActive();
	void setActive(TrackedWindow* window);
	void updateScreens();
	void onScreensChanged();
	void onDesktopsChanged();

	QList<TrackedWindow*> mWindows;
	QHash<HWND, TrackedWindow*> byHwnd;
	TrackedWindow* mActive = nullptr;
	VirtualDesktops* mDesktops = nullptr;

	QHash<HWND, Dirty> dirty;
	QSet<HWND> candidates;
	QSet<HWND> moving;
	bool foregroundDirty = false;
	QTimer flushTimer;
	QTimer sweepTimer;

	QHash<HMONITOR, QScreen*> screensByMonitor;
	std::vector<WindowEvent> spareEvents;
};

} // namespace qs::windows
