#include "toast_mirror.hpp"

#include <qcoreapplication.h>
#include <qdeadlinetimer.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qobject.h>
#include <qpointer.h>
#include <qthread.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/base.h>

#include "../../../core/logcat.hpp"
#include "toast_worker.hpp"

namespace qs::windows::services::notifications {

using winrt::Windows::UI::Notifications::Management::UserNotificationListener;
using winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;

namespace {
QS_LOGGING_CATEGORY(logToastAccess, "quickshell.windows.notifications.access", QtWarningMsg);

uint32_t codeOf(const winrt::hresult_error& e) { return static_cast<uint32_t>(e.code().value); }
} // namespace

QString SystemNotificationAccess::toString(SystemNotificationAccess::Enum value) {
	switch (value) {
	case SystemNotificationAccess::Unknown: return "Unknown";
	case SystemNotificationAccess::Unspecified: return "Unspecified";
	case SystemNotificationAccess::Allowed: return "Allowed";
	case SystemNotificationAccess::Denied: return "Denied";
	case SystemNotificationAccess::Unavailable: return "Unavailable";
	default: return "Invalid system notification access";
	}
}

ToastMirror::ToastMirror(QObject* parent): QObject(parent) {
	this->mWorker = new ToastMirrorWorker(this);
	this->mWorker->moveToThread(&this->mThread);

	QObject::connect(&this->mThread, &QThread::started, this->mWorker, &ToastMirrorWorker::start);
	QObject::connect(&this->mThread, &QThread::finished, this->mWorker, &ToastMirrorWorker::shutdown);

	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
		this->mThread.quit();
		if (!this->mThread.wait(QDeadlineTimer(2000))) {
			qWarning() << "Notification listener thread still busy at exit, leaving it behind";
		}
	});

	this->mThread.setObjectName(QStringLiteral("ToastMirror"));
	this->mThread.start();
}

ToastMirror::~ToastMirror() {
	this->mThread.quit();
	this->mThread.wait();
	delete this->mWorker;
}

void ToastMirror::setEnabled(bool enabled) {
	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker, enabled] { worker->setEnabled(enabled); },
	    Qt::QueuedConnection
	);
}

void ToastMirror::setAccessObservation(bool enabled) {
	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker, enabled] { worker->setAccessObservation(enabled); },
	    Qt::QueuedConnection
	);
}

// The only path that may raise the Windows access prompt. It also latches
// accessRequested for the lifetime of the process so a reloaded QML layer
// cannot issue a second request.
//
// Microsoft requires UserNotificationListener.RequestAccessAsync on the UI
// thread, so the prompt is started here, on the GUI thread, and completed
// asynchronously; the worker MTA only receives the resolved status. Blocking
// here (a .get()) would freeze the shell while the user decides.
void ToastMirror::requestAccess() {
	if (!this->mAccessRequested) {
		this->mAccessRequested = true;
		emit this->accessRequestedChanged();
	}

	// A pending prompt already belongs to this process lifetime. Asking again
	// would stack consent dialogs after a QML reload.
	if (this->mAccessPrompting) return;
	this->mAccessPrompting = true;

	auto& seam = ToastListenerSeam::instance();
	if (seam.engaged()) {
		// Same contract as Windows: only Unspecified may prompt.
		auto status = seam.getAccessStatus != nullptr
		                ? seam.getAccessStatus()
		                : UserNotificationListenerAccessStatus::Unspecified;
		if (status == UserNotificationListenerAccessStatus::Unspecified) {
			if (seam.requestAccessAsync != nullptr) {
				// The fixture owns completion, exactly like the OS dialog.
				auto* pending = new QPointer<ToastMirror>(this);
				seam.requestAccessAsync(
				    [](void* context, UserNotificationListenerAccessStatus result) {
					    auto* guard = static_cast<QPointer<ToastMirror>*>(context);
					    auto frontend = *guard;
					    delete guard;
					    QMetaObject::invokeMethod(
					        QCoreApplication::instance(),
					        [frontend, result] {
						        auto* self = frontend.data();
						        if (self == nullptr) return;
						        self->completeAccessRequest(result, false);
					        },
					        Qt::QueuedConnection
					    );
				    },
				    pending
				);
				return;
			}
			if (seam.requestAccess != nullptr) status = seam.requestAccess();
		}
		this->completeAccessRequest(status, false);
		return;
	}

	auto self = QPointer<ToastMirror>(this);

	try {
		auto listener =
		    winrt::Windows::UI::Notifications::Management::UserNotificationListener::Current();
		auto status = listener.GetAccessStatus();

		if (status != UserNotificationListenerAccessStatus::Unspecified) {
			this->completeAccessRequest(status, false);
			return;
		}

		listener.RequestAccessAsync().Completed(
		    [self](const auto& operation, winrt::Windows::Foundation::AsyncStatus) {
			    auto status = UserNotificationListenerAccessStatus::Unspecified;
			    auto failed = false;

			    try {
				    status = operation.GetResults();
			    } catch (const winrt::hresult_error& e) {
				    failed = true;
				    qCWarning(logToastAccess)
				        << "Notification access request failed:" << Qt::hex << codeOf(e);
			    }

			    // The operation completes on a WinRT thread. Hop to the GUI
			    // thread through the application object so a dying ToastMirror
			    // is never dereferenced off-thread.
			    QMetaObject::invokeMethod(
			        QCoreApplication::instance(),
			        [self, status, failed] {
				        auto* frontend = self.data();
				        if (frontend == nullptr) return;
				        frontend->completeAccessRequest(status, failed);
			        },
			        Qt::QueuedConnection
			    );
		    }
		);
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastAccess) << "Notification access request could not start:" << Qt::hex
		                          << codeOf(e);
		// A request that never started has no access answer; reconcile with
		// GetAccessStatus instead of inventing one.
		this->completeAccessRequest(UserNotificationListenerAccessStatus::Unspecified, true);
	}
}

void ToastMirror::completeAccessRequest(
    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status,
    bool failed
) {
	this->mAccessPrompting = false;
	if (failed) this->refreshAccessStatus();
	else this->resolveAccess(status);
}

// Hands a UI-thread consent result to the worker, which owns all listener
// state and never prompts.
void ToastMirror::resolveAccess(
    winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status
) {
	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker, status] { worker->accessResolved(status); },
	    Qt::QueuedConnection
	);
}

// Non-prompting status query: reads GetAccessStatus without ever calling
// RequestAccessAsync, so callers can observe consent changes safely.
void ToastMirror::refreshAccessStatus() {
	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker] { worker->refreshAccessStatus(); },
	    Qt::QueuedConnection
	);
}

void ToastMirror::removeToast(quint32 id) {
	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker, id] { worker->removeToast(id); },
	    Qt::QueuedConnection
	);
}

void ToastMirror::workerState(SystemNotificationAccess::Enum access, bool active) {
	if (access != this->mAccess) {
		this->mAccess = access;
		emit this->accessChanged();
	}

	if (active != this->mActive) {
		this->mActive = active;
		emit this->activeChanged();
	}
}

} // namespace qs::windows::services::notifications
