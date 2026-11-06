#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <winrt/Windows.UI.Notifications.Management.h>

namespace qs::windows::services::notifications {

class SystemNotificationAccess: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Unknown = 0,
		Unspecified = 1,
		Allowed = 2,
		Denied = 3,
		Unavailable = 4,
	};
	Q_ENUM(Enum);

	Q_INVOKABLE static QString
	toString(qs::windows::services::notifications::SystemNotificationAccess::Enum value);
};

struct ToastSnapshot {
	quint32 id = 0;
	QString aumid;
	QString appName;
	QString logoPath;
	QString summary;
	QString body;
};

class ToastMirrorWorker;

class ToastMirror: public QObject {
	Q_OBJECT;

public:
	explicit ToastMirror(QObject* parent = nullptr);
	~ToastMirror() override;
	Q_DISABLE_COPY_MOVE(ToastMirror);

	void setEnabled(bool enabled);
	void setAccessObservation(bool enabled);
	void requestAccess();
	void refreshAccessStatus();
	void removeToast(quint32 id);

	[[nodiscard]] SystemNotificationAccess::Enum access() const { return this->mAccess; }
	[[nodiscard]] bool isActive() const { return this->mActive; }
	// Process-lifetime latch set only by the explicit requestAccess() call. A
	// reloaded QML layer reconciles with it instead of re-asking Windows.
	[[nodiscard]] bool accessRequested() const { return this->mAccessRequested; }

signals:
	void toastAdded(const qs::windows::services::notifications::ToastSnapshot& toast);
	void toastRemoved(quint32 id);
	void accessChanged();
	void activeChanged();
	void accessRequestedChanged();

private:
	friend class ToastMirrorWorker;

	void workerState(SystemNotificationAccess::Enum access, bool active);
	void resolveAccess(
	    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status
	);
	void completeAccessRequest(
	    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status,
	    bool failed
	);

	QThread mThread;
	ToastMirrorWorker* mWorker = nullptr;
	SystemNotificationAccess::Enum mAccess = SystemNotificationAccess::Unknown;
	bool mActive = false;
	bool mAccessRequested = false;
	// One prompt at a time: the async request may outlive a reloaded QML layer
	// that asks again.
	bool mAccessPrompting = false;
};

} // namespace qs::windows::services::notifications
