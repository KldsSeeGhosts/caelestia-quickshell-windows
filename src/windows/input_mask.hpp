#pragma once

#include <thread>

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qpointer.h>
#include <qregion.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qwindow.h>

namespace qs::windows {

void noteHookDelay(bool keyboard, DWORD eventTime);

class InputMaskTracker: public QObject {
	Q_OBJECT;

public:
	~InputMaskTracker() override;
	Q_DISABLE_COPY_MOVE(InputMaskTracker);

	static InputMaskTracker* instance();

	void setMask(QWindow* window, const QRegion& region);
	void remove(QWindow* window);

	void refresh();

	void acquireButtonEvents();
	void releaseButtonEvents();

	void acquireCursorEvents();
	void releaseCursorEvents();

	void acquireMonitorEvents();
	void releaseMonitorEvents();
	void resetCursorMonitor();

	void acquireHook();
	void releaseHook();
	[[nodiscard]] bool hookActive() const { return this->hookRunning; }

	// Records that a hook saw input late and arms the report window. Called from
	// both input hook threads, so it only posts a wakeup for the tracker's
	// thread. While nothing is late no report timer runs at all.
	static void requestLateReport();

#ifdef QS_TEST
	struct LateReportState {
		bool armed = false;
		quint32 mouse = 0;
		quint32 keys = 0;
	};

	// Test seam: the report window's recording state, observable without a
	// message window, an installed hook or a tracker instance.
	static LateReportState lateReportStateForTesting();

	static LRESULT CALLBACK dispatchMouseHookForTesting(int code, WPARAM message, LPARAM data) {
		return mouseHookProc(code, message, data);
	}
#endif

signals:
	void buttonPressed(QPoint position, quint32 time);
	void cursorMoved(QPoint position);
	void cursorMonitorChanged(quintptr monitor);

private:
	explicit InputMaskTracker(QObject* parent);

	struct Entry {
		QPointer<QWindow> window;
		QRegion region;
	};

	void evaluate(POINT cursor);
	void publish();
	void emitCursor(QPoint position);
	void onCursorTrail();
	void updateHookState();
	bool startHook();
	void stopHook();
	void onHookFailed(DWORD error, quint32 generation);
	void onCursorMoved();
	void onButtonPressed(QPoint position, quint32 time);
	void reportLateInput();

	static void startLateReport();
	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);
	static void hookThreadMain(quint32 generation);

	QList<Entry> entries;
	HWND messageWindow = nullptr;
	std::thread hookThread;
	bool hookRunning = false;
	bool hookFailed = false;
	quint32 hookGeneration = 0;
	int cursorWatchers = 0;
	int monitorWatchers = 0;
	int hookHolders = 0;
	QPoint lastCursor;
	QTimer pollTimer;
	QTimer cursorTrailTimer;
	QTimer lateReportTimer;
};

} // namespace qs::windows
