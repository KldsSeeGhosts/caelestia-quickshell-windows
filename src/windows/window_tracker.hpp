#pragma once
#include <thread>

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
#include "fullscreen_ownership.hpp"
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

namespace window_action_detail {

[[nodiscard]] inline bool shouldReportFailure(DWORD error, bool) {
	return error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_WINDOW_HANDLE;
}

[[nodiscard]] inline QString refusalReason(DWORD error, bool elevated) {
	if (error == ERROR_INVALID_WINDOW_HANDLE) return QStringLiteral("stale-window");
	if (error != ERROR_ACCESS_DENIED) return {};
	return elevated ? QStringLiteral("elevated-window") : QStringLiteral("access-denied");
}

[[nodiscard]] inline QString errorText(const QString& action, const QString& reason) {
	return action + QLatin1Char(':') + reason;
}

[[nodiscard]] inline bool trackedOwnerMatches(
    bool isWindow,
    DWORD process,
    DWORD thread,
    DWORD expectedProcess,
    DWORD expectedThread
) {
	return isWindow && process == expectedProcess && thread == expectedThread;
}

[[nodiscard]] inline QString trackedOwnerFailureReason(bool ownerMatches) {
	return ownerMatches ? QString() : QStringLiteral("stale-window");
}

[[nodiscard]] inline QString fullscreenFailureReason(bool accessDenied, bool windowAlive, bool elevated) {
	if (accessDenied) return refusalReason(ERROR_ACCESS_DENIED, elevated);
	if (!windowAlive) return QStringLiteral("stale-window");
	return {};
}

} // namespace window_action_detail

namespace window_fullscreen_detail {

// Records genuine native access denials separately from ownership-policy
// failures. GetLastError is reset immediately before each delegated native
// operation so a stale thread error cannot become a refusal.
class ErrorTrackingOps final: public fullscreen_ownership::Ops {
public:
	void resetAccessDenied() { this->mAccessDenied = false; }
	[[nodiscard]] bool accessDenied() const { return this->mAccessDenied; }

	bool read(HWND hwnd, fullscreen_ownership::Snapshot& snapshot) override {
		return this->invoke([&] { return this->native.read(hwnd, snapshot); });
	}
	bool mark(HWND hwnd, const fullscreen_ownership::Identity& identity, const void* token) override {
		return this->invoke([&] { return this->native.mark(hwnd, identity, token); });
	}
	bool matches(HWND hwnd, const fullscreen_ownership::Identity& identity, const void* token) override {
		return this->invoke([&] { return this->native.matches(hwnd, identity, token); });
	}
	void unmark(HWND hwnd, const fullscreen_ownership::Identity& identity, const void* token) override {
		this->native.unmark(hwnd, identity, token);
	}
	bool style(
	    HWND hwnd,
	    const fullscreen_ownership::Identity& identity,
	    const void* token,
	    LONG_PTR value
	) override {
		return this->invoke([&] { return this->native.style(hwnd, identity, token, value); });
	}
	bool placement(
	    HWND hwnd,
	    const fullscreen_ownership::Identity& identity,
	    const void* token,
	    const WINDOWPLACEMENT& value
	) override {
		return this->invoke([&] { return this->native.placement(hwnd, identity, token, value); });
	}
	bool frame(
	    HWND hwnd,
	    const fullscreen_ownership::Identity& identity,
	    const void* token,
	    const RECT* rect
	) override {
		return this->invoke([&] { return this->native.frame(hwnd, identity, token, rect); });
	}

private:
	template <typename Fn> bool invoke(Fn&& fn) {
		SetLastError(ERROR_SUCCESS);
		auto ok = fn();
		if (!ok && GetLastError() == ERROR_ACCESS_DENIED) this->mAccessDenied = true;
		return ok;
	}

	fullscreen_ownership::NativeOps native;
	bool mAccessDenied = false;
};

} // namespace window_fullscreen_detail

class TrackedWindow: public QObject {
	Q_OBJECT;

public:
	~TrackedWindow() override;
#ifdef QS_TEST
	static TrackedWindow* createForTest(WindowTracker* tracker, HWND hwnd) {
		return new TrackedWindow(tracker, hwnd);
	}
	void setElevatedForTest(bool elevated) { this->bElevated = elevated; }
#endif
	[[nodiscard]] HWND hwnd() const { return this->mHwnd; }
	[[nodiscard]] quint64 address() const;
	[[nodiscard]] QString addressHex() const;
	[[nodiscard]] TrackedWindow* owner() const;

	// clang-format off
	[[nodiscard]] QBindable<QString> bindableTitle() { return &this->bTitle; }
	[[nodiscard]] QBindable<QString> bindableAppId() { return &this->bAppId; }
	[[nodiscard]] QBindable<QString> bindableExePath() { return &this->bExePath; }
	[[nodiscard]] QBindable<QString> bindableAumid() { return &this->bAumid; }
	[[nodiscard]] QBindable<bool> bindableElevated() { return &this->bElevated; }
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
	// Full process image path. This aliases exePath so downstream window models
	// expose the explicit Hyprland-style name without a second cached copy.
	[[nodiscard]] QString imagePath() const { return this->exePath(); }
	// The real AppUserModelID when Windows supplies one. appId() may be a
	// process-name fallback and is not a substitute for this value.
	[[nodiscard]] QString aumid() const { return this->bAumid.value(); }
	[[nodiscard]] bool elevated() const { return this->bElevated.value(); }
	[[nodiscard]] QString lastActionError() const { return this->mLastActionError; }
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
	void imagePathChanged();
	void aumidChanged();
	void elevatedChanged();
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
	void lastActionErrorChanged();
	// Structured refusal for UI surfaces. reason is stable machine text such as
	// "elevated-window" or "access-denied", never a path or window title.
	void actionRefused(const QString& action, const QString& reason, bool elevated);

private:
	friend class WindowTracker;

	void forgetScreen(QScreen* screen);

	explicit TrackedWindow(WindowTracker* tracker, HWND hwnd);

	void refreshIdentity();
	void refreshTitle();
	void refreshState();
	void refreshDesktop(bool askShell);
	void releaseFullscreenOwnership();
	[[nodiscard]] bool hasCurrentOwner() const;
	bool ensureCurrentOwner(const QString& action);
	void reportActionRefused(const QString& action, const QString& reason);
	void reportActionFailure(const QString& action, DWORD error);
	void clearActionError();

	WindowTracker* tracker;
	QString mLastActionError;
	HWND mHwnd;
	bool uwpFrame = false;
	quint32 mFrameProcessId = 0;
	quint32 mFrameThreadId = 0;
	bool traceObserved = false;
	LONG_PTR traceStyle = 0;
	LONG_PTR traceExStyle = 0;
	QRect traceRect;
	WindowStyleSnapshot styleSnapshot;

	window_fullscreen_detail::ErrorTrackingOps fullscreenOps;
	fullscreen_ownership::Owner fullscreenOwner;
	bool endingTracking = false;

	// Exact fullscreen episode: the frame and monitor that were classified
	// fullscreen. It bridges a display-mode or DPI change, where the monitor and
	// the window briefly disagree about their size, but never a different frame.
	fullscreen_geometry::Latch fullscreenLatch;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bTitle, &TrackedWindow::titleChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bAppId, &TrackedWindow::appIdChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bExePath, &TrackedWindow::exePathChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, QString, bAumid, &TrackedWindow::aumidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(TrackedWindow, bool, bElevated, &TrackedWindow::elevatedChanged);
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
	Q_PROPERTY(QString lastActionError READ lastActionError NOTIFY lastActionErrorChanged);

public:
	static WindowTracker* instance();
#ifdef QS_TEST
	static WindowTracker* createForTest();
	static void destroyForTest(WindowTracker* tracker);
#endif
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
	[[nodiscard]] QString lastActionError() const { return this->mLastActionError; }

	[[nodiscard]] QScreen* screenFor(HMONITOR monitor) const;
	[[nodiscard]] static qsizetype screenIndex(QScreen* screen);

	void rescan();
	void refreshForeground();
	void reportActionRefusal(const QString& action, const QString& reason, bool elevated);
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
	void lastActionErrorChanged();
	// Broadcast form of TrackedWindow::actionRefused for shell-level toast UI.
	void windowActionRefused(const QString& action, const QString& reason, bool elevated);
	void tilingActiveChanged();

private:
	friend class TrackedWindow;

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
	void reportWindowActionRefused(
	    const TrackedWindow* window,
	    const QString& action,
	    const QString& reason,
	    bool elevated
	);
	void clearWindowActionError(const TrackedWindow* window);

	QList<TrackedWindow*> mWindows;
	QString mLastActionError;
	const TrackedWindow* mLastActionErrorWindow = nullptr;
	QHash<HWND, TrackedWindow*> byHwnd;
	TrackedWindow* mActive = nullptr;
	VirtualDesktops* mDesktops = nullptr;

	QHash<HWND, Dirty> dirty;
	QSet<HWND> candidates;
	mutable QSet<HWND> desktopCandidates;
	QSet<HWND> moving;
	bool foregroundDirty = false;
	QTimer flushTimer;
	QTimer sweepTimer;

	QHash<HMONITOR, QScreen*> screensByMonitor;
	std::vector<WindowEvent> spareEvents;
	HANDLE eventThreadStop = nullptr;
	std::thread eventThread;
};

} // namespace qs::windows
