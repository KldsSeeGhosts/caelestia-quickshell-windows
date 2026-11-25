#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <qabstractnativeeventfilter.h>
#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qset.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

class QThreadPool;
class QTimer;

namespace qs::windows {
class ScreenIdentityRegistry;
}

namespace qs::windows::sys {

enum class BrightnessMethod : quint8 {
	Unresolved,
	Wmi,
	Lcd,
	Ddc,
	SdrWhite,
	Software,
};

struct BrightnessRoute {
	BrightnessMethod method = BrightnessMethod::Unresolved;
	QString wmiInstance;
	QString wmiMethodsPath;
	// This screen is the only active built-in panel, so the global \\.\LCD
	// device provably addresses it. Every \\.\LCD write, including the
	// fallback after a failed WMI write, requires it.
	bool builtIn = false;
	bool lcdTried = false;
	LUID adapterId {};
	UINT32 targetId = 0;
};

class BrightnessBackend;

namespace brightness_detail {

using EventClock = std::chrono::steady_clock;
constexpr auto EVENT_HEALTHY_RESET = std::chrono::seconds(30);
constexpr DWORD EVENT_POLL_WAIT_MS = 1000;
constexpr DWORD EVENT_JOIN_TIMEOUT_MS = 2000;
constexpr int EVENT_BACKOFF_CAP = 5;
constexpr DWORD EVENT_RETRY_BASE_MS = 5000;

[[nodiscard]] inline bool healthyRunResetsFailures(EventClock::duration healthyFor) {
	return healthyFor >= EVENT_HEALTHY_RESET;
}

[[nodiscard]] inline DWORD eventRetryDelayMs(int consecutiveFailures) {
	const auto factor = std::clamp(consecutiveFailures, 1, EVENT_BACKOFF_CAP);
	return EVENT_RETRY_BASE_MS * static_cast<DWORD>(factor);
}


constexpr UINT WM_APP_BRIGHTNESS_EVENT = WM_APP + 0x51;
constexpr size_t WMI_EVENT_QUEUE_CAP = 128;

struct WmiBrightnessEvent {
	std::wstring instanceName;
	int percent = -1;
};

// Pure C++ state shared with the detachable WMI worker. It deliberately has no
// Qt member or callback: the GUI owner drains events and turns them into Qt
// signals on the GUI thread.
struct SharedWmiState {
	std::shared_ptr<void> stopEvent;
	std::atomic<bool> cancelled {false};
	std::atomic<bool> finished {false};
	mutable std::mutex mutex;
	HWND wakeHwnd = nullptr; // Guarded by mutex and cleared before window destruction.
	std::deque<WmiBrightnessEvent> events;
	std::deque<std::wstring> warnings;
};

[[nodiscard]] inline std::shared_ptr<SharedWmiState> createSharedWmiState() {
	auto state = std::make_shared<SharedWmiState>();
	auto* event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (event == nullptr) return nullptr;
	state->stopEvent = std::shared_ptr<void>(event, [](void* handle) { CloseHandle(handle); });
	return state;
}

inline void clearWmiWakeWindow(const std::shared_ptr<SharedWmiState>& state) {
	if (state == nullptr) return;
	auto lock = std::lock_guard(state->mutex);
	state->wakeHwnd = nullptr;
}

inline void setWmiWakeWindow(const std::shared_ptr<SharedWmiState>& state, HWND hwnd) {
	if (state == nullptr) return;
	auto lock = std::lock_guard(state->mutex);
	state->wakeHwnd = hwnd;
}

inline bool queueWmiBrightnessEvent(
    const std::shared_ptr<SharedWmiState>& state,
    std::wstring instanceName,
    int percent
) {
	if (state == nullptr) return false;
	HWND wake = nullptr;
	{
		auto lock = std::lock_guard(state->mutex);
		if (state->cancelled.load(std::memory_order_acquire)) return false;
		if (state->events.size() >= WMI_EVENT_QUEUE_CAP) return false;
		state->events.push_back({std::move(instanceName), percent});
		wake = state->wakeHwnd;
	}
	if (wake != nullptr) PostMessageW(wake, WM_APP_BRIGHTNESS_EVENT, 0, 0);
	return true;
}

inline bool queueWmiWarning(const std::shared_ptr<SharedWmiState>& state, std::wstring warning) {
	if (state == nullptr) return false;
	HWND wake = nullptr;
	{
		auto lock = std::lock_guard(state->mutex);
		if (state->cancelled.load(std::memory_order_acquire)) return false;
		if (state->warnings.size() >= WMI_EVENT_QUEUE_CAP) return false;
		state->warnings.push_back(std::move(warning));
		wake = state->wakeHwnd;
	}
	if (wake != nullptr) PostMessageW(wake, WM_APP_BRIGHTNESS_EVENT, 0, 0);
	return true;
}

inline bool popWmiBrightnessEvent(
    const std::shared_ptr<SharedWmiState>& state,
    WmiBrightnessEvent& event
) {
	auto lock = std::lock_guard(state->mutex);
	if (state->events.empty()) return false;
	event = std::move(state->events.front());
	state->events.pop_front();
	return true;
}

inline bool popWmiWarning(const std::shared_ptr<SharedWmiState>& state, std::wstring& warning) {
	auto lock = std::lock_guard(state->mutex);
	if (state->warnings.empty()) return false;
	warning = std::move(state->warnings.front());
	state->warnings.pop_front();
	return true;
}

// Owns the watcher thread and shares RAII ownership of its stop event with that
// worker. stop() cancels and joins a finitely polling worker with a bounded wait;
// if the wait times out, the detached worker still owns a valid event handle and
// exits without touching owner storage.
class EventWatchThread {
public:
	EventWatchThread() = default;
	~EventWatchThread();
	Q_DISABLE_COPY_MOVE(EventWatchThread);

	using Cancelled = std::shared_ptr<std::atomic<bool>>;
	using Worker = std::function<void(HANDLE, const Cancelled&)>;
	bool start(const Worker& worker);
	void stop();
	[[nodiscard]] bool running() const;

private:
	struct StopState {
		explicit StopState(HANDLE event): event(event) {}
		~StopState() {
			if (this->event != nullptr) CloseHandle(this->event);
		}
		HANDLE event;
		std::atomic<bool> cancelled {false};
	};

	mutable std::mutex mutex;
	std::shared_ptr<StopState> stopState;
	std::thread thread;
	// Shared with the worker so the bounded-stop fallback can never leave a
	// detached thread writing into this object's storage.
	std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);
};

} // namespace brightness_detail

class Brightness
    : public QObject
    , public QAbstractNativeEventFilter {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

	// Strict route policy for writes, off by default. When false, behavior is
	// unchanged: an unresolved route is written as DDC/CI, a failed WMI write
	// retries \\.\LCD, a failed hardware write falls back to software gamma and
	// every terminal path reports ok = true. When true, setBrightness() only
	// writes through the screen's stored route: it rejects the write when the
	// route is unresolved, never retries \\.\LCD, never falls back to software
	// gamma, and reports the real outcome through brightnessSetFinished() and
	// brightnessWriteResult(). Software gamma is applied only through an
	// explicit setBrightness() call whose stored route already is software, or
	// through clearSoftwareDimming(). query()/probe() also stop dropping
	// software dimming this process applied earlier; use
	// clearSoftwareDimming() to request that explicitly.
	// Enabling strictRoutes is refused while a write is in flight, queued, or
	// being dispatched/completed; retry when idle. Once enabled it cannot be
	// disabled (attempts leave it true and warn once per controller).
	Q_PROPERTY(bool strictRoutes READ strictRoutes WRITE setStrictRoutes NOTIFY strictRoutesChanged);

public:
	explicit Brightness(QObject* parent = nullptr);
	~Brightness() override;
	Q_DISABLE_COPY_MOVE(Brightness);

	// Test-only construction against an injected backend (brightness_backend.hpp).
	// The backend is not owned and must outlive this object. Not reachable from QML.
	explicit Brightness(BrightnessBackend& backend, QObject* parent = nullptr);
	// Test-only construction with the same identity registry production uses.
	explicit Brightness(
	    BrightnessBackend& backend,
	    qs::windows::ScreenIdentityRegistry& identities,
	    QObject* parent = nullptr
	);
	bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

	[[nodiscard]] bool strictRoutes() const;
	void setStrictRoutes(bool strict);

	Q_INVOKABLE void query(const QString& screenName);

	// Runs exactly the same strict query as query() and additionally reports the
	// observation through queryFinished() with a request id. The id is
	// allocated when this call is made, on the calling (GUI) thread, is unique
	// and strictly increasing within the process (across controller
	// instances), and is returned immediately; the query itself still completes
	// asynchronously. queried() also fires for these calls, so existing
	// consumers keep observing every completed query.
	Q_INVOKABLE qint64 queryRequest(const QString& screenName);

	Q_INVOKABLE void probe(const QString& screenName);

	Q_INVOKABLE void refresh(const QString& screenName);

	Q_INVOKABLE void setBrightness(const QString& screenName, bool isDdc, qreal value);

	Q_INVOKABLE QString methodFor(const QString& screenName) const;

	// Explicit request to drop software gamma dimming this process applied to a
	// screen, even if its route changed or this controller was recreated.
	// No-op unless the backend retains dimming state for that screen.
	// Strict query()/probe() never do this on their own.
	Q_INVOKABLE void clearSoftwareDimming(const QString& screenName);

signals:
	void queried(const QString& screenName, bool available, bool isDdc, qreal brightness);
	// Correlates one queryRequest() call with its observation. While the
	// controller lives, emitted exactly once per queryRequest() call with that
	// call's id, on the GUI thread, immediately before the queried() for the
	// same completion. Never emitted for plain query() or probe() calls. Every
	// outcome reports: a resolved route, an unavailable or failed probe, an
	// unknown screen, and a request deferred by the startup gate. The id
	// travels with the request through that deferral, so completions may arrive
	// in any order and still pair each id with its own result. Destroying the
	// controller cancels its outstanding requests: they never report, and
	// consumers must treat a replaced or destroyed controller as cancelling
	// every id it issued.
	void queryFinished(
	    qint64 requestId,
	    const QString& screenName,
	    bool available,
	    bool isDdc,
	    qreal brightness
	);
	void brightnessSetFinished(const QString& screenName, bool ok);
	// Emitted once per setBrightness() call submitted with strictRoutes true.
	// Overlapping writes to the same screen are rejected with ok=false rather
	// than coalesced. Completion bookkeeping is finalized before emitting results,
	// so result handlers can immediately submit the next strict write.
	// value is clamped to [0, 1]; success means the backend accepted the write,
	// not that a subsequent hardware read-back confirmed the requested level.
	// route is "wmi", "lcd", "ddc", "software", or "unresolved" when the write
	// was rejected before dispatch because no route was stored for the screen.
	void brightnessWriteResult(
	    const QString& screenName,
	    qreal value,
	    bool ok,
	    const QString& route,
	    const QString& error
	);
	void routeChanged(const QString& screenName, const QString& method);
	void systemBrightnessChanged(const QString& screenName, qreal brightness);
	void strictRoutesChanged();
	void displayConfigurationChanged();

private:
	// Every per-screen map is keyed by the opaque stable target id, never by a
	// display label. Public methods still take the unique QML name for upstream
	// compatibility and resolve it through the identity registry once.
	// requestId is 0 for plain query()/probe() calls, which never report
	// through queryFinished(); a positive id is carried by every completion
	// path of queryRequest().
	void startProbe(const QString& screenName, bool readDdc, qint64 requestId);
	void startSet(const QString& key, const QString& name, qreal value, bool strict = false);
	void strictSet(const QString& key, const QString& name, qreal value);
	void finishSet(
	    const QString& key,
	    const QString& name,
	    BrightnessMethod method,
	    qreal value,
	    bool ok,
	    const QString& error,
	    bool strict,
	    quint64 lifetime
	);
	void useSoftware(const QString& key, const QString& name, qreal value, const QString& reason);
	void settle(const QString& key, const QString& name);
	void startReadBack(const QString& key, const QString& name);
	void onSystemBrightness(const QString& instanceName, int percent);
	[[nodiscard]] bool busy(const QString& key) const;
	[[nodiscard]] QString nameFor(const QString& key, const QString& fallback) const;
	// An id stops naming its screen (removal, or another identity): drop
	// everything keyed by it and invalidate work still running for it.
	void watchIdentities();
	void retire(const QString& key);
	[[nodiscard]] quint64 lifetimeOf(const QString& key) const;
	void ensureEventWatch();

	BrightnessBackend* backend = nullptr;
	qs::windows::ScreenIdentityRegistry* identities = nullptr;
	bool strictRoutesEnabled = false;
	bool strictRoutesDisableWarned = false;
	int legacyWriteDepth = 0;
	QThreadPool* pool = nullptr;
	QHash<QString, BrightnessRoute> routes;
	QHash<QString, qreal> pending;
	QSet<QString> inFlight;
	QHash<QString, qreal> lastValue;
	QHash<QString, quint32> generation;
	QHash<QString, QTimer*> settleTimers;
	// Bumped each time an id is retired. Workers capture it when they start and
	// are dropped on completion if it changed.
	QHash<QString, quint64> lifetimes;
};

} // namespace qs::windows::sys
