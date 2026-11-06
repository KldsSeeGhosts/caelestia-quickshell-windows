#pragma once

#include <memory>

#include <qhash.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include "../../native_callback_gate.hpp"
#include "toast_mirror.hpp"

class QTimer;

namespace qs::windows::services::notifications {

// Test seam for the notification-mirror consent tests: when installed, every
// listener interaction is faked through these hooks so tests observe access
// queries and prompts without touching the real system notification listener.
// Production never installs the seam.
struct ToastListenerSeam {
	using AccessStatus =
	    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;

	bool (*listenerAvailable)() = nullptr;       // fake UserNotificationListener::Current()
	AccessStatus (*getAccessStatus)() = nullptr; // fake GetAccessStatus, never prompts
	AccessStatus (*requestAccess)() = nullptr;   // fake RequestAccessAsync, the OS prompt
	void (*removeToast)(quint32 id) = nullptr;   // fake RemoveNotification
	// Fakes a failed GetAccessStatus read (the production read can throw).
	bool (*getAccessStatusSucceeds)() = nullptr;
	// Fakes AppCapability.AccessChanged subscription; the test keeps the
	// callback and invokes it to model the OS notification.
	void (*watchAccessChanged)(void (*callback)(void* context), void* context) = nullptr;
	void (*unwatchAccessChanged)() = nullptr;
	// Fakes the asynchronous consent operation: the test receives the
	// completion and calls it when the user answers, including after the
	// frontend is destroyed.
	void (*requestAccessAsync)(void (*complete)(void* context, AccessStatus status), void* context) = nullptr;

	[[nodiscard]] bool engaged() const { return this->getAccessStatus != nullptr; }

	static ToastListenerSeam& instance();
};

class ToastMirrorWorker: public QObject {
	Q_OBJECT;

public:
	explicit ToastMirrorWorker(ToastMirror* frontend);
	~ToastMirrorWorker() override;
	Q_DISABLE_COPY_MOVE(ToastMirrorWorker);

	void setEnabled(bool enabled);
	// Separate from setEnabled: the non-prompting access backstop and the
	// AppCapability event stay active while the feature is opted in, even
	// when active mirroring is withdrawn for Denied/Unavailable access.
	void setAccessObservation(bool enabled);
	// The UI-thread prompt's result. Apply it on the worker so every listener
	// interaction stays on one thread.
	void accessResolved(
	    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status
	);
	void refreshAccessStatus();
	void removeToast(quint32 id);

	// Test seam: overrides both backstop intervals when non-zero. Production
	// leaves it zero.
	static int testAccessCheckIntervalMs;

public slots:
	void start();
	void shutdown();

private:
	void begin();
	void stop();
	void applyAccess(
	    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status
	);
	void checkAccess();
	void failAccess();
	void accessChanged();
	void watchAccessChanges();
	void stopWatchingAccess();
	[[nodiscard]] bool readAccessStatus(
	    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus& status
	);
	void restartAccessBackstop();
	void resync(bool announce);
	void report();

	[[nodiscard]] ToastSnapshot
	snapshotOf(const winrt::Windows::UI::Notifications::UserNotification& toast);
	QString logoFor(const winrt::Windows::ApplicationModel::AppInfo& info, const QString& aumid);

	std::shared_ptr<NativeCallbackGate<ToastMirrorWorker>> mGate;
	ToastMirror* mFrontend;
	bool mApartment = false;
	bool mEnabled = false;
	bool mObserve = false;

	winrt::Windows::UI::Notifications::Management::UserNotificationListener mListener {nullptr};
	winrt::event_token mChangedToken {};
	bool mEvents = false;

	// AppCapability.AccessChanged is the primary consent-change signal; the
	// backstop timer below is the fallback when the event is unavailable.
	winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapability mCapability {nullptr};
	winrt::event_token mCapabilityToken {};
	bool mCapabilityEvents = false;

	QTimer* mPollTimer = nullptr;
	QTimer* mAccessTimer = nullptr;
	QTimer* mEventTimer = nullptr;

	bool mBaselined = false;
	QSet<quint32> mKnown;
	QHash<QString, QString> mLogos;

	SystemNotificationAccess::Enum mAccess = SystemNotificationAccess::Unknown;
	bool mActive = false;
};

} // namespace qs::windows::services::notifications
