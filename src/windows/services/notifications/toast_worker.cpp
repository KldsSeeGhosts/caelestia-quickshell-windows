#include "toast_worker.hpp"

#include <winerror.h>

#include <qbytearray.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qimage.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qstandardpaths.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>

#include "../../../core/logcat.hpp"
#include "../../redact.hpp"
#include "toast_mirror.hpp"

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;
using winrt::Windows::ApplicationModel::AppInfo;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapability;

namespace qs::windows::services::notifications {

namespace {
QS_LOGGING_CATEGORY(logToastMirror, "quickshell.windows.notifications.mirror", QtWarningMsg);

constexpr int POLL_INTERVAL_MS = 2000;
// AppCapability.AccessChanged for the userNotificationListener capability is
// the primary consent-change signal. The capability object is readable from
// this unpackaged shell (Create succeeds and CheckAccess returns the same
// listener consent), but event delivery is not guaranteed for every Settings
// change on every build, so the non-prompting GetAccessStatus backstop below
// remains as a fallback. Poll cheaply while the mirror suppresses Windows'
// own banners so a missed event cannot leave them off, and slowly otherwise.
constexpr int ACTIVE_ACCESS_CHECK_INTERVAL_MS = 2000;
constexpr int ACCESS_CHECK_INTERVAL_MS = 10000;
constexpr int EVENT_COALESCE_MS = 150;
constexpr float LOGO_SIZE = 96;
constexpr uint32_t MAX_LOGO_BYTES = 4 * 1024 * 1024;

QString toQString(const winrt::hstring& value) {
	return QString::fromWCharArray(value.c_str(), static_cast<qsizetype>(value.size()));
}

uint32_t codeOf(const winrt::hresult_error& e) { return static_cast<uint32_t>(e.code().value); }

SystemNotificationAccess::Enum mapAccess(UserNotificationListenerAccessStatus status) {
	switch (status) {
	case UserNotificationListenerAccessStatus::Allowed: return SystemNotificationAccess::Allowed;
	case UserNotificationListenerAccessStatus::Denied: return SystemNotificationAccess::Denied;
	default: return SystemNotificationAccess::Unspecified;
	}
}

QString logoCacheDir() {
	auto dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
	         + QStringLiteral("/notification-icons");
	QDir().mkpath(dir);
	return dir;
}
} // namespace

int ToastMirrorWorker::testAccessCheckIntervalMs = 0;

ToastListenerSeam& ToastListenerSeam::instance() {
	static auto* seam = new ToastListenerSeam(); // NOLINT
	return *seam;
}

ToastMirrorWorker::ToastMirrorWorker(ToastMirror* frontend)
    : mGate(std::make_shared<NativeCallbackGate<ToastMirrorWorker>>(this)), mFrontend(frontend) {}

ToastMirrorWorker::~ToastMirrorWorker() = default;

void ToastMirrorWorker::start() {
	if (!ToastListenerSeam::instance().engaged()) {
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			this->mApartment = true;
		} catch (const winrt::hresult_error& e) {
			qCWarning(logToastMirror) << "init_apartment(multi_threaded) failed:" << Qt::hex << codeOf(e);
		}
	}

	this->mPollTimer = new QTimer(this);
	this->mPollTimer->setInterval(POLL_INTERVAL_MS);
	QObject::connect(this->mPollTimer, &QTimer::timeout, this, [this] { this->resync(true); });

	this->mAccessTimer = new QTimer(this);
	this->mAccessTimer->setInterval(ACCESS_CHECK_INTERVAL_MS);
	QObject::connect(this->mAccessTimer, &QTimer::timeout, this, &ToastMirrorWorker::checkAccess);

	this->mEventTimer = new QTimer(this);
	this->mEventTimer->setSingleShot(true);
	this->mEventTimer->setInterval(EVENT_COALESCE_MS);
	QObject::connect(this->mEventTimer, &QTimer::timeout, this, [this] { this->resync(true); });
}

void ToastMirrorWorker::shutdown() {
	this->mGate->close();
	this->mEnabled = false;
	this->mObserve = false;
	this->stop();
	this->stopWatchingAccess();
	this->mListener = nullptr;

	if (this->mApartment) {
		winrt::uninit_apartment();
		this->mApartment = false;
	}
}

void ToastMirrorWorker::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	// Enabling (mirroring consent or ownership acquisition) only reads the
	// current access status; the OS prompt is reserved for the explicit
	// requestAccess() operation.
	if (enabled) {
		this->watchAccessChanges();
		this->begin();
	} else {
		this->stop();
		// Active mirroring is withdrawn, but the opt-in observer keeps watching
		// access for a re-grant unless the feature itself is off.
		if (!this->mObserve) this->stopWatchingAccess();
		this->restartAccessBackstop();
		this->report();
	}
}

void ToastMirrorWorker::setAccessObservation(bool enabled) {
	if (enabled == this->mObserve) return;
	this->mObserve = enabled;

	if (enabled) {
		this->watchAccessChanges();
		// Report the current consent immediately so the service can reconcile
		// observation and mirroring without waiting for a first event/tick.
		this->begin();
		return;
	}

	if (!this->mEnabled) {
		if (this->mAccessTimer) this->mAccessTimer->stop();
		this->stopWatchingAccess();
	}
}

void ToastMirrorWorker::accessResolved(UserNotificationListenerAccessStatus status) {
	// The prompt itself ran on the UI thread; applying its answer here keeps
	// every listener read and subscription on the worker thread.
	this->applyAccess(status);
}

void ToastMirrorWorker::refreshAccessStatus() { this->begin(); }

void ToastMirrorWorker::begin() {
	auto status = UserNotificationListenerAccessStatus::Unspecified;

	if (!this->readAccessStatus(status)) {
		this->failAccess();
		return;
	}

	this->applyAccess(status);
}

// A failed status read means consent cannot be confirmed. Treat that as not
// usable: withdraw mirroring and banner suppression now, but keep the
// non-prompting backstop running so a later successful read recovers.
void ToastMirrorWorker::failAccess() {
	this->stop();
	this->mAccess = SystemNotificationAccess::Unavailable;
	this->restartAccessBackstop();
	this->report();
}

void ToastMirrorWorker::applyAccess(UserNotificationListenerAccessStatus status) {
	this->mAccess = mapAccess(status);

	if (this->mAccess != SystemNotificationAccess::Allowed || !this->mEnabled) {
		this->stop();
		this->restartAccessBackstop();
		this->report();
		return;
	}

	if (!this->mActive) {
		auto faked = ToastListenerSeam::instance().engaged();
		if (!faked && !this->mEvents) {
			try {
				this->mChangedToken = this->mListener.NotificationChanged([gate = this->mGate](auto&&, auto&&) {
					gate->post([](ToastMirrorWorker* self) {
						if (self->mActive) self->mEventTimer->start();
					});
				});
				this->mEvents = true;
			} catch (const winrt::hresult_error& e) {
				qCInfo(logToastMirror) << "NotificationChanged unavailable (" << Qt::hex << codeOf(e)
				                       << "), polling every" << POLL_INTERVAL_MS << "ms";
			}
		}

		this->mActive = true;
		this->mKnown.clear();
		if (!faked) this->resync(false);

		if (!faked && this->mActive && !this->mEvents) this->mPollTimer->start();
	}

	// Keep the backstop alive while the mirror suppresses Windows' banners.
	// Revoking access makes the listener silently return nothing, so without
	// this check the shell would keep the banners off forever.
	if (this->mActive) this->restartAccessBackstop();

	this->report();
}

void ToastMirrorWorker::checkAccess() {
	if (!this->mEnabled && !this->mObserve) return;

	auto status = UserNotificationListenerAccessStatus::Unspecified;
	if (!this->readAccessStatus(status)) {
		this->failAccess();
		return;
	}

	if (mapAccess(status) != this->mAccess
	    || (status == UserNotificationListenerAccessStatus::Allowed && this->mEnabled && !this->mActive))
	{
		this->applyAccess(status);
	}
}

// AppCapability.AccessChanged is the primary signal; re-read the listener
// status instead of trusting a mapped capability enum.
void ToastMirrorWorker::accessChanged() { this->checkAccess(); }

void ToastMirrorWorker::watchAccessChanges() {
	if (this->mCapabilityEvents) return;

	auto& seam = ToastListenerSeam::instance();
	if (seam.engaged()) {
		if (seam.watchAccessChanged != nullptr) {
			seam.watchAccessChanged(
			    [](void* context) {
				    auto* self = static_cast<ToastMirrorWorker*>(context);
				    QMetaObject::invokeMethod(
				        self,
				        [self] { self->accessChanged(); },
				        Qt::QueuedConnection
				    );
			    },
			    this
			);
			this->mCapabilityEvents = true;
		}
		return;
	}

	if (!this->mApartment) return;

	try {
		this->mCapability = AppCapability::Create(L"userNotificationListener");
		this->mCapabilityToken =
		    this->mCapability.AccessChanged([gate = this->mGate](auto&&, auto&&) {
			    gate->post([](ToastMirrorWorker* self) { self->accessChanged(); });
		    });
		this->mCapabilityEvents = true;
	} catch (const winrt::hresult_error& e) {
		qCInfo(logToastMirror) << "AppCapability AccessChanged unavailable (" << Qt::hex
		                       << codeOf(e) << "), using the access backstop every"
		                       << (this->mActive ? ACTIVE_ACCESS_CHECK_INTERVAL_MS
		                                         : ACCESS_CHECK_INTERVAL_MS)
		                       << "ms";
	}
}

void ToastMirrorWorker::stopWatchingAccess() {
	if (!this->mCapabilityEvents) return;

	auto& seam = ToastListenerSeam::instance();
	if (seam.engaged()) {
		if (seam.unwatchAccessChanged != nullptr) seam.unwatchAccessChanged();
	} else if (this->mCapability) {
		try {
			this->mCapability.AccessChanged(this->mCapabilityToken);
		} catch (const winrt::hresult_error&) {}
	}

	this->mCapabilityEvents = false;
	this->mCapability = nullptr;
}

void ToastMirrorWorker::restartAccessBackstop() {
	if (this->mAccessTimer == nullptr) return;

	if (!this->mEnabled && !this->mObserve) {
		this->mAccessTimer->stop();
		return;
	}

	auto interval = ToastMirrorWorker::testAccessCheckIntervalMs;
	if (interval <= 0) {
		interval = this->mActive ? ACTIVE_ACCESS_CHECK_INTERVAL_MS : ACCESS_CHECK_INTERVAL_MS;
	}

	this->mAccessTimer->setInterval(interval);
	this->mAccessTimer->start();
}

// Reads consent without prompting. Shared by enable, the backstop and the
// listener-operation guards so every caller sees the same seam behavior.
bool ToastMirrorWorker::readAccessStatus(UserNotificationListenerAccessStatus& status) {
	auto& seam = ToastListenerSeam::instance();
	if (seam.engaged()) {
		if (seam.listenerAvailable != nullptr && !seam.listenerAvailable()) return false;
		if (seam.getAccessStatusSucceeds != nullptr && !seam.getAccessStatusSucceeds()) return false;
		if (seam.getAccessStatus == nullptr) return false;
		status = seam.getAccessStatus();
		return true;
	}

	if (!this->mApartment) return false;

	try {
		if (!this->mListener) this->mListener = UserNotificationListener::Current();
		status = this->mListener.GetAccessStatus();
		return true;
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "GetAccessStatus failed:" << Qt::hex << codeOf(e);
		return false;
	}
}

void ToastMirrorWorker::stop() {
	if (this->mPollTimer) this->mPollTimer->stop();
	if (this->mEventTimer) this->mEventTimer->stop();
	if (this->mAccessTimer) this->mAccessTimer->stop();

	if (this->mEvents) {
		try {
			this->mListener.NotificationChanged(this->mChangedToken);
		} catch (const winrt::hresult_error&) {}
		this->mEvents = false;
	}

	for (auto id: std::as_const(this->mKnown)) {
		QMetaObject::invokeMethod(
		    this->mFrontend,
		    [frontend = this->mFrontend, id] { emit frontend->toastRemoved(id); },
		    Qt::QueuedConnection
		);
	}

	this->mKnown.clear();
	this->mActive = false;
}

void ToastMirrorWorker::resync(bool announce) {
	if (!this->mActive) return;
	if (ToastListenerSeam::instance().engaged()) return; // no real listener under the test seam

	// Never call the listener without confirming consent first. A revoked
	// grant makes GetNotifications return nothing or fail, and the shell must
	// stop suppressing Windows' banners at that point, not keep an empty mirror
	// alive.
	auto status = UserNotificationListenerAccessStatus::Unspecified;
	if (!this->readAccessStatus(status)) {
		this->failAccess();
		return;
	}
	if (status != UserNotificationListenerAccessStatus::Allowed) {
		this->applyAccess(status);
		return;
	}

	winrt::Windows::Foundation::Collections::IVectorView<UserNotification> toasts {nullptr};
	try {
		toasts = this->mListener.GetNotificationsAsync(NotificationKinds::Toast).get();
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "GetNotificationsAsync failed:" << Qt::hex << codeOf(e);
		this->stop();
		this->mAccess = e.code() == E_ACCESSDENIED ? SystemNotificationAccess::Denied
		                                           : SystemNotificationAccess::Unspecified;
		this->restartAccessBackstop();
		this->report();
		return;
	}

	auto current = QSet<quint32>();

	for (const auto& toast: toasts) {
		quint32 id = 0;
		try {
			id = toast.Id();
		} catch (const winrt::hresult_error&) {
			continue;
		}

		current.insert(id);
		if (!announce || this->mKnown.contains(id)) continue;

		auto snapshot = this->snapshotOf(toast);
		qCDebug(logToastMirror) << "New toast" << id << "from" << qs::windows::logId(snapshot.aumid);

		QMetaObject::invokeMethod(
		    this->mFrontend,
		    [frontend = this->mFrontend, snapshot] { emit frontend->toastAdded(snapshot); },
		    Qt::QueuedConnection
		);
	}

	for (auto id: std::as_const(this->mKnown)) {
		if (current.contains(id)) continue;
		qCDebug(logToastMirror) << "Toast" << id << "left the notification center";

		QMetaObject::invokeMethod(
		    this->mFrontend,
		    [frontend = this->mFrontend, id] { emit frontend->toastRemoved(id); },
		    Qt::QueuedConnection
		);
	}

	this->mKnown = current;
}

void ToastMirrorWorker::removeToast(quint32 id) {
	this->mKnown.remove(id);

	auto& seam = ToastListenerSeam::instance();
	if (seam.engaged()) {
		if (seam.removeToast) seam.removeToast(id);
		return;
	}

	if (!this->mListener || !this->mActive) return;

	// Consent is checked before every listener operation, not only by the
	// backstop, so a revoked grant cannot keep touching the notification list.
	auto status = UserNotificationListenerAccessStatus::Unspecified;
	if (!this->readAccessStatus(status)) {
		this->failAccess();
		return;
	}
	if (status != UserNotificationListenerAccessStatus::Allowed) {
		this->applyAccess(status);
		return;
	}

	try {
		this->mListener.RemoveNotification(id);
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "RemoveNotification" << id << "failed:" << Qt::hex << codeOf(e);
	}
}

ToastSnapshot ToastMirrorWorker::snapshotOf(const UserNotification& toast) {
	auto snapshot = ToastSnapshot();
	snapshot.id = toast.Id();

	try {
		if (auto info = toast.AppInfo()) {
			snapshot.aumid = toQString(info.AppUserModelId());
			if (auto display = info.DisplayInfo()) snapshot.appName = toQString(display.DisplayName());
			snapshot.logoPath = this->logoFor(info, snapshot.aumid);
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logToastMirror) << "AppInfo of toast" << snapshot.id << "failed:" << Qt::hex
		                        << codeOf(e);
	}

	if (snapshot.appName.isEmpty()) snapshot.appName = snapshot.aumid;
	if (snapshot.appName.isEmpty()) snapshot.appName = QStringLiteral("Windows");

	try {
		auto notification = toast.Notification();
		auto visual = notification ? notification.Visual() : nullptr;

		if (visual) {
			auto binding = visual.GetBinding(KnownNotificationBindings::ToastGeneric());
			if (!binding && visual.Bindings().Size() != 0) binding = visual.Bindings().GetAt(0);

			if (binding) {
				auto lines = QStringList();
				for (const auto& text: binding.GetTextElements()) lines << toQString(text.Text());
				if (!lines.isEmpty()) snapshot.summary = lines.takeFirst();
				snapshot.body = lines.join('\n');
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logToastMirror) << "Reading toast" << snapshot.id << "failed:" << Qt::hex << codeOf(e);
	}

	return snapshot;
}

QString ToastMirrorWorker::logoFor(const AppInfo& info, const QString& aumid) {
	if (auto it = this->mLogos.constFind(aumid); it != this->mLogos.constEnd()) return *it;

	QString path;

	try {
		auto logo = info.DisplayInfo().GetLogo(winrt::Windows::Foundation::Size(LOGO_SIZE, LOGO_SIZE));
		auto stream = logo ? logo.OpenReadAsync().get() : nullptr;

		if (stream && stream.Size() > 0 && stream.Size() <= MAX_LOGO_BYTES) {
			auto size = static_cast<uint32_t>(stream.Size());
			auto reader = winrt::Windows::Storage::Streams::DataReader(stream);
			reader.LoadAsync(size).get();

			auto bytes = QByteArray(static_cast<qsizetype>(size), Qt::Uninitialized);
			reader.ReadBytes(
			    winrt::array_view<uint8_t>(
			        reinterpret_cast<uint8_t*>(bytes.data()),               // NOLINT
			        reinterpret_cast<uint8_t*>(bytes.data()) + bytes.size() // NOLINT
			    )
			);

			auto image = QImage();
			if (image.loadFromData(bytes)) {
				auto hash = QCryptographicHash::hash(aumid.toUtf8(), QCryptographicHash::Sha1).toHex();
				auto file = logoCacheDir() + '/' + QString::fromLatin1(hash) + QStringLiteral(".png");
				if (image.save(file, "PNG")) path = file;
				else
					qCWarning(logToastMirror) << "Could not save the logo of" << qs::windows::logId(aumid)
					                          << "to" << qs::windows::logPath(file);
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logToastMirror) << "GetLogo for" << qs::windows::logId(aumid) << "failed:" << Qt::hex
		                        << codeOf(e);
	}

	this->mLogos.insert(aumid, path);
	return path;
}

void ToastMirrorWorker::report() {
	QMetaObject::invokeMethod(
	    this->mFrontend,
	    [frontend = this->mFrontend, access = this->mAccess, active = this->mActive] {
		    frontend->workerState(access, active);
	    },
	    Qt::QueuedConnection
	);
}

} // namespace qs::windows::services::notifications
