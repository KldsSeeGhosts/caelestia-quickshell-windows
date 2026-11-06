#pragma once

#include <windows.h>

#include <functional>
#include <memory>

#include <qcontainerfwd.h>
#include <qhash.h>
#include <qobject.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../../core/model.hpp"
#include "notification.hpp"
#include "session_bus_lock.hpp"
#include "toast_mirror.hpp"

namespace qs::windows::services::notifications {

struct NotificationServerSupport {
	bool persistence = false;
	bool body = true;
	bool bodyMarkup = false;
	bool bodyHyperlinks = false;
	bool bodyImages = false;
	bool actions = false;
	bool actionIcons = false;
	bool image = false;
	bool inlineReply = false;
	QVector<QString> extraHints;
};

// A holder's lease on the process-lifetime consent observation. Claiming takes
// the next generation; releasing (explicitly or on destruction) switches
// observation off through the same generation check as any other write, so a
// stale lease destroyed after a newer holder claimed is refused and the newer
// holder keeps observing. Tying the lease to the owner's lifetime means
// observation cannot outlive the object that asked for it, whatever order the
// objects around it are destroyed in. Releasing twice is a no-op.
class ObservationLease {
public:
	ObservationLease() = default;
	~ObservationLease();
	ObservationLease(const ObservationLease&) = delete;
	ObservationLease& operator=(const ObservationLease&) = delete;
	ObservationLease(ObservationLease&&) = delete;
	ObservationLease& operator=(ObservationLease&&) = delete;

	quint32 claim();
	void release();
	[[nodiscard]] quint32 generation() const { return this->mGeneration; }

private:
	quint32 mGeneration = 0;
};

class NotificationServer: public QObject {
	Q_OBJECT;

public:
	static NotificationServer* instance();

	void switchGeneration(bool reEmit, const std::function<void()>& clearHook);
	ObjectModel<Notification>* trackedNotifications();
	void deleteNotification(Notification* notification, NotificationCloseReason::Enum reason);

	quint32 notify(
	    const QString& appName,
	    quint32 replacesId,
	    const QString& appIcon,
	    const QString& summary,
	    const QString& body,
	    const QStringList& actions,
	    const QVariantMap& hints,
	    qint32 expireTimeout
	);

	// Local replacement of a mirrored record is refused (returns zero), never
	// converted into a mutation of its Windows toast. Local replacements keep
	// their existing behavior.
	quint32 notifySend(const QString& summary, const QString& body, const QStringList& args);

	void invokeAction(Notification* notification, const QString& identifier);

	// Activates the source app of a mirrored toast without the implicit
	// Dismissed auto-close of the generic action invoke path. Removing the real
	// Windows toast stays a separate, explicit dismissal decision.
	[[nodiscard]] bool activateMirrored(quint32 id);

	void setMirrorEnabled(bool enabled);
	// Independent of active mirroring: keeps the non-prompting consent
	// observer (AppCapability event plus backstop) alive while the feature is
	// opted in, so a re-grant is noticed with the notification centre closed.
	//
	// Observation lives as long as this process, not as long as the QML object
	// that asked for it, so it is owned by generation. A holder claims a
	// generation, and only the holder of the latest one can change the state:
	// an outgoing holder's late disable (service teardown, reload) is refused
	// instead of switching off its replacement's observation. A claim does not
	// change the state; setSystemAccessObservation returns whether the write
	// came from the current holder and was applied.
	[[nodiscard]] quint32 claimAccessObservation();
	bool setSystemAccessObservation(bool enabled, quint32 generation);
	void requestSystemAccess();
	// Non-prompting access query (GetAccessStatus only).
	void refreshAccessStatus();
	[[nodiscard]] SystemNotificationAccess::Enum systemAccess() const;
	[[nodiscard]] bool isSystemMirrorActive() const;
	// Process-lifetime latch: true once an explicit request was issued.
	[[nodiscard]] bool accessRequested() const;
	// "owner" once this process holds the session bus mutex, "contended" while
	// another shell owns it, "error" when the lock could not be initialised,
	// "unknown" before a claim was attempted.
	[[nodiscard]] QString ownership() const { return this->mOwnership; }

	NotificationServerSupport support;

#ifdef QS_NOTIFICATION_TEST_SEAMS
	// Test seams, compiled only into the test target (the compile definition is
	// set by the test CMake alone), so a production build has no member to
	// redirect behaviour with. When set, the first two replace app activation
	// and window lookup so tests never activate, launch or focus a real
	// application. The bus pair moves the session-bus window class and mutex to
	// names unique to a test run, so a fixture can acquire ownership and forward
	// without reaching, or being reached by, a live shell.
	static bool (*testActivateApp)(const QString& aumid, const QString& desktopEntry);
	static bool (*testHasWindowFor)(const QString& aumid);
	static const wchar_t* testBusClass;
	static const wchar_t* testBusMutex;
#endif

signals:
	void notification(qs::windows::services::notifications::Notification* notification);
	void actionInvoked(quint32 id, const QString& action);
	void systemAccessChanged();
	void systemMirrorActiveChanged();
	void accessRequestedChanged();
	void ownershipChanged();

private:
	friend class NotificationMirrorConsentTest;

	explicit NotificationServer() = default;

	quint32 deliver(
	    quint32 reservedId,
	    quint32 replacesId,
	    const QString& appName,
	    const QString& appIcon,
	    const QString& summary,
	    const QString& body,
	    const QStringList& actions,
	    const QVariantMap& hints,
	    qint32 expireTimeout,
	    quint32 toastId = 0,
	    const QString& aumid = QString()
	);

	[[nodiscard]] bool hasReceiver() const;

	void claimSession();
	void acquireOwnership();
	void sessionOwnershipChanged(SessionBusLock::Result result, DWORD error);
	void setOwnership(const QString& ownership);
	quint32 forwardToOwner(const QString& summary, const QString& body, const QStringList& args);
	static LRESULT CALLBACK busWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	bool mOwner = false;
	bool mMirrorWanted = false;
	bool mObservationWanted = false;
	// Latest observation generation handed out; zero is never a valid holder.
	quint32 mObservationGeneration = 0;
	std::unique_ptr<SessionBusLock> mSessionLock;
	QString mOwnership = QStringLiteral("unknown");
	HWND busWindow = nullptr;

	ToastMirror* mirror();
	void onToastAdded(const ToastSnapshot& toast);
	void onToastRemoved(quint32 toastId);
	static void activateApp(const QString& aumid, const QString& desktopEntry);

	quint32 nextId = 1;
	QHash<quint32, Notification*> idMap;
	QHash<quint32, Notification*> toastMap;
	ObjectModel<Notification> mNotifications {this};
	ToastMirror* mMirror = nullptr;
	QList<std::function<void()>> pendingSends;
	bool sendScheduled = false;
};

} // namespace qs::windows::services::notifications
