#include <QtTest/QtTest>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

#include <qbytearray.h>
#include <qcoreapplication.h>
#include <qjsondocument.h>
#include <qlist.h>
#include <qlogging.h>
#include <qobject.h>
#include <qscopeguard.h>
#include <qsignalspy.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qthread.h>
#include <qtypes.h>
#include <quuid.h>
#include <qvariant.h>

#include "../../core/desktopentry.hpp"
#include "../../core/iconimageprovider.hpp"
#include "../../core/instanceinfo.hpp"
#include "../../core/logcat.hpp"
#include "../desktopentry_backend.hpp"
#include "../window_tracker.hpp"
#include "services/notifications/notification.hpp"
#include "services/notifications/server.hpp"
#include "services/notifications/session_bus_lock.hpp"
#include "services/notifications/toast_mirror.hpp"
#include "services/notifications/toast_worker.hpp"

// Link shims: this test compiles only the notification service sources, so the
// platform singletons they reference resolve to inert stubs. Every code path
// that could reach the real implementations is additionally faked through the
// consent seams below, so the test never queries the real listener, prompts,
// activates an app or removes a real Windows notification.
void qs::log::initLogCategoryLevel(const char* name, QtMsgType defaultLevel) {}
DesktopEntryManager* DesktopEntryManager::instance() { return nullptr; }
DesktopEntry* DesktopEntryManager::byId(const QString& id) { return nullptr; }
QString qs::windows::WindowsDesktopEntryBackend::parsingNameForId(const QString& id) {
	return QString();
}
QString qs::windows::WindowsDesktopEntryBackend::idForParsingName(const QString& token) {
	return QString();
}
void qs::windows::WindowsDesktopEntryBackend::launch(
    const QString& token,
    const QString& workingDirectory
) {}
qs::windows::WindowTracker* qs::windows::WindowTracker::instance() { return nullptr; }
bool qs::windows::TrackedWindow::activate() { return true; }
QString IconImageProvider::requestString(
    const QString& icon,
    const QString& path,
    const QString& fallback
) {
	return icon;
}

namespace {

using SeamStatus = qs::windows::services::notifications::ToastListenerSeam::AccessStatus;

// Fake listener state. The prompt fake models Windows asking the user and the
// user allowing; every other transition is driven directly by the test.
SeamStatus fakeStatus = SeamStatus::Unspecified;
bool fakeListenerAvailable = true;
bool fakeStatusReadable = true;
int fakeStatusQueries = 0;
int fakePrompts = 0;
int fakeRemovals = 0;
QThread* fakeRequestThread = nullptr;
QList<QString> fakeActivations;

SeamStatus fakeGetAccessStatus() {
	return fakeStatus;
}

// Captures the lock-failure warning without letting it into the fixture output,
// so the row can prove the Win32 code is reported, not only the state.
QStringList lockMessages;
QtMessageHandler previousMessageHandler = nullptr;

void captureLockMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
	if (type == QtWarningMsg || type == QtCriticalMsg) lockMessages.append(message);
}

// Counts every read attempt, including the failed ones the backstop retries.
bool fakeGetAccessStatusSucceeds() {
	fakeStatusQueries++;
	return fakeStatusReadable;
}

SeamStatus fakeRequestAccess() {
	fakePrompts++;
	fakeRequestThread = QThread::currentThread();
	fakeStatus = SeamStatus::Allowed;
	return fakeStatus;
}

// Asynchronous prompt fixture: the test owns completion, like the OS dialog.
void (*fakeAsyncComplete)(void* context, SeamStatus status) = nullptr;
void* fakeAsyncContext = nullptr;

void fakeRequestAccessAsync(void (*complete)(void* context, SeamStatus status), void* context) {
	fakePrompts++;
	fakeRequestThread = QThread::currentThread();
	fakeAsyncComplete = complete;
	fakeAsyncContext = context;
}

void completeFakeRequest(SeamStatus status) {
	auto complete = fakeAsyncComplete;
	auto* context = fakeAsyncContext;
	fakeAsyncComplete = nullptr;
	fakeAsyncContext = nullptr;
	if (complete != nullptr) complete(context, status);
}

// AppCapability.AccessChanged fixture: the test fires the OS event.
void (*fakeAccessChangedCallback)(void* context) = nullptr;
void* fakeAccessChangedContext = nullptr;

void fakeWatchAccessChanged(void (*callback)(void* context), void* context) {
	fakeAccessChangedCallback = callback;
	fakeAccessChangedContext = context;
}

void fakeUnwatchAccessChanged() {
	fakeAccessChangedCallback = nullptr;
	fakeAccessChangedContext = nullptr;
}

void triggerFakeAccessChanged() {
	if (fakeAccessChangedCallback != nullptr) {
		fakeAccessChangedCallback(fakeAccessChangedContext);
	}
}

// Forwarding fixture: a test-owned session-bus window that answers WM_COPYDATA.
// The unique class name also keeps a real shell's bus window unreachable from
// this process, so no fixture can forward to the live desktop.
std::wstring fakeBusClassName;
std::wstring fakeBusMutexName;
QByteArray fakeBusPayload;
LRESULT fakeBusWindowResult = 4242;

LRESULT CALLBACK fakeBusWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_COPYDATA) {
		const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam); // NOLINT
		fakeBusPayload = QByteArray(
		    static_cast<const char*>(data->lpData),
		    static_cast<qsizetype>(data->cbData)
		);
		return fakeBusWindowResult;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void fakeRemoveToast(quint32 id) { fakeRemovals++; }

bool fakeListenerAvailableFn() { return fakeListenerAvailable; }

bool fakeActivateApp(const QString& aumid, const QString& desktopEntry) {
	fakeActivations.append(aumid);
	return true;
}

bool fakeHasWindowFor(const QString& aumid) { return false; }

} // namespace

namespace qs::windows::services::notifications {

// Befriended by NotificationServer so the consent tests can drive the private
// ownership-acquisition and toast-arrival paths deterministically instead of
// the real session-bus mutex and Windows listener.
class NotificationMirrorConsentTest: public QObject {
	Q_OBJECT;

private slots:
	void initTestCase() {
		auto& seam = ToastListenerSeam::instance();
		seam.listenerAvailable = &fakeListenerAvailableFn;
		seam.getAccessStatus = &fakeGetAccessStatus;
		seam.getAccessStatusSucceeds = &fakeGetAccessStatusSucceeds;
		seam.requestAccess = &fakeRequestAccess;
		seam.removeToast = &fakeRemoveToast;
		seam.watchAccessChanged = &fakeWatchAccessChanged;
		seam.unwatchAccessChanged = &fakeUnwatchAccessChanged;

		NotificationServer::testActivateApp = &fakeActivateApp;
		NotificationServer::testHasWindowFor = &fakeHasWindowFor;

		// Unique per run: ownership acquisition creates the bus window, and a
		// live shell's window and mutex must stay unreachable from this process
		// (and this process's from the shell).
		const auto runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		fakeBusClassName = QStringLiteral("CaelestiaTestNotificationBus-%1").arg(runId).toStdWString();
		fakeBusMutexName = QStringLiteral("Local\\caelestia-test-notification-bus-%1").arg(runId).toStdWString();
		NotificationServer::testBusClass = fakeBusClassName.c_str();
		NotificationServer::testBusMutex = fakeBusMutexName.c_str();

		// The revocation backstop runs on a timer; keep the fixtures fast.
		ToastMirrorWorker::testAccessCheckIntervalMs = 25;

		// A non-shell.qml config never claims the session bus, so creating the
		// server singleton below has no effect outside this process.
		InstanceInfo::CURRENT.configPath = QStringLiteral("C:/fixture/consent-test.qml");
	}

	void cleanupTestCase() {
		ToastMirrorWorker::testAccessCheckIntervalMs = 0;
		auto& seam = ToastListenerSeam::instance();
		seam.listenerAvailable = nullptr;
		seam.getAccessStatus = nullptr;
		seam.getAccessStatusSucceeds = nullptr;
		seam.requestAccess = nullptr;
		seam.requestAccessAsync = nullptr;
		seam.removeToast = nullptr;
		seam.watchAccessChanged = nullptr;
		seam.unwatchAccessChanged = nullptr;
		NotificationServer::testActivateApp = nullptr;
		NotificationServer::testHasWindowFor = nullptr;
		NotificationServer::testBusClass = nullptr;
		NotificationServer::testBusMutex = nullptr;
		fakeBusClassName.clear();
		fakeBusMutexName.clear();
		fakeAccessChangedCallback = nullptr;
		fakeAccessChangedContext = nullptr;
	}

	// Finding 2: enabling the mirror only reads the access status. Even from
	// Unspecified — the only state where a prompt would be possible — no
	// request may be issued by an enable.
	void enableNeverPrompts() {
		const int promptsBefore = fakePrompts;
		fakeStatus = SeamStatus::Unspecified;

		ToastMirror mirror;
		QSignalSpy accessSpy(&mirror, &ToastMirror::accessChanged);
		QSignalSpy activeSpy(&mirror, &ToastMirror::activeChanged);

		mirror.setEnabled(true);
		QTRY_COMPARE_WITH_TIMEOUT(accessSpy.count(), 1, 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Unspecified);
		QVERIFY(!mirror.isActive());
		QCOMPARE(activeSpy.count(), 0);
		QCOMPARE(fakePrompts, promptsBefore);

		// Disabling again must not query or prompt either.
		mirror.setEnabled(false);
		QTRY_VERIFY(!mirror.isActive());
		QCOMPARE(fakePrompts, promptsBefore);
	}

	// Finding 2/5: the OS prompt belongs to the explicit request operation
	// only, and that request latches for the process lifetime.
	void explicitRequestPromptsOnce() {
		const int promptsBefore = fakePrompts;
		fakeStatus = SeamStatus::Unspecified;
		fakeRequestThread = nullptr;

		ToastMirror mirror;
		QSignalSpy requestedSpy(&mirror, &ToastMirror::accessRequestedChanged);
		QSignalSpy accessSpy(&mirror, &ToastMirror::accessChanged);

		QVERIFY(!mirror.accessRequested());
		mirror.requestAccess();
		QCOMPARE(requestedSpy.count(), 1);
		QVERIFY(mirror.accessRequested());

		// The request prompted exactly once and resolved to Allowed.
		QTRY_COMPARE_WITH_TIMEOUT(accessSpy.count(), 1, 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Allowed);
		QCOMPARE(fakePrompts, promptsBefore + 1);

		// Microsoft requires the prompt on the UI thread; the worker MTA only
		// receives the answer. The fixture calls from the application thread,
		// so the seam must see exactly that thread.
		QVERIFY(fakeRequestThread != nullptr);
		QCOMPARE(fakeRequestThread, QCoreApplication::instance()->thread());
		QCOMPARE(fakeRequestThread, QThread::currentThread());

		// A second explicit request from a non-Unspecified state never prompts.
		mirror.requestAccess();
		QCOMPARE(fakePrompts, promptsBefore + 1);
		QCOMPARE(requestedSpy.count(), 1);
		QVERIFY(mirror.accessRequested());

		// The server-level path behaves the same and latches process-wide.
		auto* server = NotificationServer::instance();
		const int serverPrompts = fakePrompts;
		fakeStatus = SeamStatus::Unspecified;
		server->requestSystemAccess();
		QTRY_COMPARE(fakePrompts, serverPrompts + 1);
		// The fake prompt resolves to Allowed, and the latch is observable.
		QTRY_COMPARE_WITH_TIMEOUT(server->systemAccess(), SystemNotificationAccess::Allowed, 5000);
		QVERIFY(server->accessRequested());
	}

	// Finding 4: refreshAccessStatus reads GetAccessStatus without ever
	// requesting, including while the mirror is enabled and denied.
	void refreshAccessStatusNeverPrompts() {
		const int promptsBefore = fakePrompts;
		fakeStatus = SeamStatus::Denied;

		ToastMirror mirror;
		QSignalSpy accessSpy(&mirror, &ToastMirror::accessChanged);

		// Denied while disabled: the QML denied-poll scenario.
		mirror.refreshAccessStatus();
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Denied, 5000);
		QCOMPARE(fakePrompts, promptsBefore);

		// Recovery observed by the same non-prompting query while enabled.
		mirror.setEnabled(true);
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Denied, 5000);
		QVERIFY(!mirror.isActive());
		fakeStatus = SeamStatus::Allowed;
		mirror.refreshAccessStatus();
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Allowed);
		QCOMPARE(fakePrompts, promptsBefore);

		mirror.setEnabled(false);
		QTRY_VERIFY(!mirror.isActive());
	}

	// Contended ownership: notifySend hands the payload to the owner's bus
	// window and creates no local record.
	void forwardsWhileContended() {
		auto* server = NotificationServer::instance();
		QVERIFY(!server->mOwner);
		QVERIFY(!fakeBusClassName.empty());
		server->setOwnership(QStringLiteral("contended"));

		auto windowClass = WNDCLASSW {};
		windowClass.lpfnWndProc = &fakeBusWindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = fakeBusClassName.c_str();
		QVERIFY(RegisterClassW(&windowClass) != 0);
		auto* window = CreateWindowExW(
		    0,
		    windowClass.lpszClassName,
		    L"",
		    0,
		    0,
		    0,
		    0,
		    0,
		    HWND_MESSAGE,
		    nullptr,
		    windowClass.hInstance,
		    nullptr
		);
		QVERIFY(window != nullptr);

		fakeBusPayload.clear();
		QSignalSpy arrivals(server, &NotificationServer::notification);
		const auto id = server->notifySend(
		    QStringLiteral("Forwarded summary"),
		    QStringLiteral("Forwarded body"),
		    {QStringLiteral("-a"), QStringLiteral("Fixture")}
		);
		QCOMPARE(id, static_cast<quint32>(fakeBusWindowResult));
		QCOMPARE(arrivals.count(), 0);
		QVERIFY(server->pendingSends.isEmpty());

		const auto payload = QJsonDocument::fromJson(fakeBusPayload).array();
		QCOMPARE(payload.size(), 4);
		QCOMPARE(payload.at(0).toString(), QStringLiteral("Forwarded summary"));
		QCOMPARE(payload.at(1).toString(), QStringLiteral("Forwarded body"));
		QCOMPARE(payload.at(2).toString(), QStringLiteral("-a"));
		QCOMPARE(payload.at(3).toString(), QStringLiteral("Fixture"));

		DestroyWindow(window);
		UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
		server->setOwnership(QStringLiteral("unknown"));
	}

	// No owner window: forwarding fails and the send is delivered locally.
	void forwardingFailureFallsBackToLocal() {
		auto* server = NotificationServer::instance();
		QVERIFY(!server->mOwner);
		server->setOwnership(QStringLiteral("contended"));

		QVERIFY(server->pendingSends.isEmpty());
		const auto id = server->notifySend(QStringLiteral("Fallback summary"), QStringLiteral("Fallback body"), {});
		QVERIFY(id != 0);
		QCoreApplication::processEvents();
		QVERIFY(!server->hasReceiver());
		QCOMPARE(server->pendingSends.size(), 1);

		QStringList delivered;
		const auto connection = QObject::connect(
		    server,
		    &NotificationServer::notification,
		    this,
		    [&](Notification* notification) {
			    delivered.append(notification->bindableSummary().value());
		    }
		);
		server->notifySend(QStringLiteral("Trigger delivery"), QString(), {});
		QCoreApplication::processEvents();
		QVERIFY(delivered.contains(QStringLiteral("Fallback summary")));
		QVERIFY(delivered.contains(QStringLiteral("Trigger delivery")));
		QVERIFY(server->pendingSends.isEmpty());
		QObject::disconnect(connection);
		server->setOwnership(QStringLiteral("unknown"));
	}

	// #317: a Win32 lock-initialisation failure is its own state, never
	// reported as another shell owning the bus. The failure seam replaces the
	// real object creation, so no handle or memory exhaustion is involved.
	void lockInitialisationFailurePublishesError() {
		auto* server = NotificationServer::instance();
		const auto restorePath = qScopeGuard([] {
			InstanceInfo::CURRENT.configPath = QStringLiteral("C:/fixture/consent-test.qml");
		});
		const auto restoreSeam = qScopeGuard([] {
			SessionBusLock::testCreateObject = nullptr;
			qInstallMessageHandler(previousMessageHandler);
		});
		InstanceInfo::CURRENT.configPath = QStringLiteral("C:/fixture/shell.qml");

		SessionBusLock::testCreateObject = [](const wchar_t*, bool event) -> HANDLE {
			if (event) {
				SetLastError(ERROR_NOT_ENOUGH_MEMORY);
				return nullptr;
			}
			return nullptr;
		};
		lockMessages.clear();
		previousMessageHandler = qInstallMessageHandler(captureLockMessage);

		server->claimSession();

		// The initial pending wait is provisional; the failure must replace it.
		QCOMPARE(server->ownership(), QStringLiteral("contended"));
		QTRY_COMPARE_WITH_TIMEOUT(server->ownership(), QStringLiteral("error"), 5000);
		QVERIFY(!server->mOwner);
		QVERIFY2(
		    lockMessages.join('\n').contains(QStringLiteral("Win32 error 8")),
		    qPrintable(lockMessages.join('\n'))
		);
	}

	// #317: a failed lock must not forward either. Forwarding is allowed only
	// on confirmed contention; with ownership in error a notification must not
	// reach a bus window that happens to exist, and it is not handled locally.
	void lockInitErrorDoesNotForward() {
		auto* server = NotificationServer::instance();
		server->mSessionLock.reset();
		server->mOwner = false;
		server->setOwnership(QStringLiteral("unknown"));
		QVERIFY(!fakeBusClassName.empty());

		const auto restorePath = qScopeGuard([] {
			InstanceInfo::CURRENT.configPath = QStringLiteral("C:/fixture/consent-test.qml");
		});
		const auto restoreSeam = qScopeGuard([] { SessionBusLock::testCreateObject = nullptr; });
		InstanceInfo::CURRENT.configPath = QStringLiteral("C:/fixture/shell.qml");
		SessionBusLock::testCreateObject = [](const wchar_t*, bool event) -> HANDLE {
			if (event) {
				SetLastError(ERROR_NOT_ENOUGH_MEMORY);
				return nullptr;
			}
			return nullptr;
		};
		server->claimSession();
		QTRY_COMPARE_WITH_TIMEOUT(server->ownership(), QStringLiteral("error"), 5000);

		auto windowClass = WNDCLASSW {};
		windowClass.lpfnWndProc = &fakeBusWindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = fakeBusClassName.c_str();
		QVERIFY(RegisterClassW(&windowClass) != 0);
		auto* window = CreateWindowExW(
		    0,
		    windowClass.lpszClassName,
		    L"",
		    0,
		    0,
		    0,
		    0,
		    0,
		    HWND_MESSAGE,
		    nullptr,
		    windowClass.hInstance,
		    nullptr
		);
		QVERIFY(window != nullptr);

		fakeBusPayload.clear();
		const auto id = server->notifySend(
		    QStringLiteral("Refused summary"),
		    QStringLiteral("Refused body"),
		    {}
		);
		QCOMPARE(id, quint32(0));
		QVERIFY2(fakeBusPayload.isEmpty(), "error-state notifySend forwarded to the fake bus window");
		QVERIFY2(server->pendingSends.isEmpty(), "error-state notifySend was handled locally");
		QCOMPARE(server->ownership(), QStringLiteral("error"));

		DestroyWindow(window);
		UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
	}

	// Audited owner lock: the backend must publish real session-bus ownership,
	// not infer it from a named mutex merely existing.
	void serverPublishesOwnership() {
		auto* server = NotificationServer::instance();
		server->setOwnership(QStringLiteral("unknown"));
		QCOMPARE(server->ownership(), QStringLiteral("unknown"));

		QSignalSpy ownershipSpy(server, &NotificationServer::ownershipChanged);
		server->setOwnership(QStringLiteral("contended"));
		QCOMPARE(server->ownership(), QStringLiteral("contended"));

		server->sessionOwnershipChanged(SessionBusLock::Result::Owned, 0);
		QCOMPARE(server->ownership(), QStringLiteral("owner"));
		QVERIFY(server->mOwner);
		QCOMPARE(ownershipSpy.count(), 2);
	}

	// Finding 2: acquiring session ownership with a wanted mirror enables it
	// without prompting, including from Unspecified.
	void ownershipAcquisitionEnablesWithoutPrompting() {
		const int promptsBefore = fakePrompts;
		fakeStatus = SeamStatus::Unspecified;

		auto* server = NotificationServer::instance();
		QVERIFY(!server->isSystemMirrorActive());
		server->setMirrorEnabled(true);
		server->acquireOwnership();

		QTRY_COMPARE_WITH_TIMEOUT(server->systemAccess(), SystemNotificationAccess::Unspecified, 5000);
		QVERIFY(!server->isSystemMirrorActive());
		QCOMPARE(fakePrompts, promptsBefore);

		// The enabled mirror still never prompts on its own access timer.
		fakeStatus = SeamStatus::Denied;
		server->refreshAccessStatus();
		QTRY_COMPARE_WITH_TIMEOUT(server->systemAccess(), SystemNotificationAccess::Denied, 5000);
		QCOMPARE(fakePrompts, promptsBefore);

		fakeStatus = SeamStatus::Allowed;
		server->refreshAccessStatus();
		QTRY_VERIFY_WITH_TIMEOUT(server->isSystemMirrorActive(), 5000);
		QCOMPARE(fakePrompts, promptsBefore);
	}

	// Finding 1: activating a mirrored toast's app neither dismisses the
	// notification nor removes the real Windows toast, through the dedicated
	// activation entry and through the generic action invoke path. Local
	// notifications keep the original auto-close behavior.
	void mirroredActivationDoesNotDismissOrRemove() {
		const int promptsBefore = fakePrompts;
		const int removalsBefore = fakeRemovals;
		const int activationsBefore = fakeActivations.size();
		fakeStatus = SeamStatus::Allowed;

		auto* server = NotificationServer::instance();
		QVERIFY(server->isSystemMirrorActive());

		mLastNotification = nullptr;
		QObject::connect(
		    server,
		    &NotificationServer::notification,
		    this,
		    [this](Notification* notification) {
			    // The shell (and this test) keeps every delivered notification.
			    notification->setTracked(true);
			    this->mLastNotification = notification;
		    },
		    Qt::DirectConnection
		);

		auto snapshot = ToastSnapshot();
		snapshot.id = 42;
		snapshot.aumid = QStringLiteral("windows.defender.securitycenter");
		snapshot.appName = QStringLiteral("fixture-defender");
		snapshot.summary = QStringLiteral("Fixture toast");
		snapshot.body = QStringLiteral("Fixture body");

		emit server->mirror()->toastAdded(snapshot);

		auto* mirrored = mLastNotification;
		QVERIFY(mirrored != nullptr);
		QCOMPARE(mirrored->toastId(), quint32(42));
		QVERIFY(mirrored->isTracked());
		QCOMPARE(mirrored->bindableHints().value().value("x-windows-toast-id").toUInt(), quint32(42));
		QVERIFY(!mirrored->actions().isEmpty());
		QCOMPARE(mirrored->actions().first()->identifier(), QStringLiteral("default"));
		QCOMPARE(fakePrompts, promptsBefore);

		QSignalSpy closedSpy(mirrored, &Notification::closed);

		// The dedicated activation path activates the app and leaves both the
		// record and the real Windows toast alone.
		const auto mirroredId = mirrored->id();
		QVERIFY(server->activateMirrored(mirroredId));
		QCOMPARE(fakeActivations.size(), activationsBefore + 1);
		QCOMPARE(fakeActivations.last(), QStringLiteral("windows.defender.securitycenter"));

		QTest::qWait(150);
		QCOMPARE(closedSpy.count(), 0);
		QCOMPARE(fakeRemovals, removalsBefore);
		QVERIFY(mirrored->isTracked());
		QVERIFY(server->trackedNotifications()->valueList().contains(mirrored));
		QCOMPARE(fakePrompts, promptsBefore);

		// The generic action invoke path no longer auto-closes mirrored
		// records: its Dismissed close is what removed the real toast.
		mirrored->actions().first()->invoke();
		QCOMPARE(fakeActivations.size(), activationsBefore + 2);
		QTest::qWait(150);
		QCOMPARE(closedSpy.count(), 0);
		QCOMPARE(fakeRemovals, removalsBefore);
		QVERIFY(mirrored->isTracked());
		QCOMPARE(fakePrompts, promptsBefore);

		// Local notifications are not mirrored: activating them is refused and
		// their default action keeps the original auto-close.
		mLastNotification = nullptr;
		const auto localId = server->notify(
		    QStringLiteral("fixture-local"),
		    0,
		    QString(),
		    QStringLiteral("Local summary"),
		    QStringLiteral("Local body"),
		    {QStringLiteral("default"), QStringLiteral("Open")},
		    QVariantMap(),
		    5000
		);

		auto* local = mLastNotification;
		QVERIFY(local != nullptr);
		QCOMPARE(local->toastId(), quint32(0));
		QVERIFY(local->isTracked());
		QVERIFY(!server->activateMirrored(localId));
		QCOMPARE(fakeActivations.size(), activationsBefore + 2);

		QSignalSpy localClosedSpy(local, &Notification::closed);
		local->actions().first()->invoke();
		QCOMPARE(localClosedSpy.count(), 1);
		QCOMPARE(
		    qvariant_cast<NotificationCloseReason::Enum>(localClosedSpy.at(0).at(0)),
		    NotificationCloseReason::Dismissed
		);
		QCOMPARE(fakeRemovals, removalsBefore);

		// Explicit dismissal is the only path that removes the real toast.
		mirrored->dismiss();
		QCOMPARE(closedSpy.count(), 1);
		QTRY_COMPARE_WITH_TIMEOUT(fakeRemovals, removalsBefore + 1, 5000);
		QCOMPARE(fakePrompts, promptsBefore);

		QObject::disconnect(
		    server,
		    &NotificationServer::notification,
		    this,
		    nullptr
		);
	}

	// Local replacement must never overwrite the hints of a native toast.
	// The QML fixture separately proves local-only close after hostile hint
	// updates; here both backend entry points refuse before any mutation.
	void localReplacementOfMirroredRecordIsRefused() {
		const int removalsBefore = fakeRemovals;
		const int promptsBefore = fakePrompts;
		const int activationsBefore = fakeActivations.size();
		auto* server = NotificationServer::instance();
		mLastNotification = nullptr;
		const auto connection = QObject::connect(
		    server,
		    &NotificationServer::notification,
		    this,
		    [this](Notification* notification) {
			    notification->setTracked(true);
			    this->mLastNotification = notification;
		    }
		);
		QSignalSpy arrivalSpy(server, &NotificationServer::notification);
		ToastSnapshot snapshot;
		snapshot.id = 43;
		snapshot.aumid = QStringLiteral("fixture!App");
		snapshot.summary = QStringLiteral("Protected toast");
		snapshot.body = QStringLiteral("Protected body");
		emit server->mirror()->toastAdded(snapshot);
		auto* mirrored = mLastNotification;
		QVERIFY(mirrored != nullptr);
		const auto hintsBefore = mirrored->bindableHints().value();
		const auto id = mirrored->id();

		for (const auto& args: {
		         QStringList {QStringLiteral("-r"), QString::number(id)},
		         QStringList {QStringLiteral("--replace-id=%1").arg(id)}
		     })
		{
			QCOMPARE(server->notifySend("Local replacement", "Local body", args), quint32(0));
		}
		QCOMPARE(
		    server->notify("Local", id, "", "Local replacement", "Local body", {}, {}, 5000),
		    quint32(0)
		);
		// Exercise deliver's guard independently of notifySend's early check.
		QCOMPARE(
		    server->deliver(0, id, "Local", "", "Queued replacement", "Local body", {}, {}, 5000),
		    quint32(0)
		);
		QCoreApplication::processEvents();
		QCOMPARE(arrivalSpy.count(), 1);
		QCOMPARE(mirrored->bindableSummary().value(), snapshot.summary);
		QCOMPARE(mirrored->bindableHints().value(), hintsBefore);
		QCOMPARE(mirrored->toastId(), snapshot.id);

		// A local-only close uses CloseRequested, never the native Dismissed
		// path. No fake Windows toast removal may be requested.
		server->deleteNotification(mirrored, NotificationCloseReason::CloseRequested);
		QVERIFY(!server->idMap.contains(id));

		// Local-to-local replacement remains supported by the backend.
		const auto localId = server->notify("Local", 0, "", "Before", "", {}, {}, 5000);
		QCOMPARE(server->notifySend("After", "", {"-r", QString::number(localId)}), localId);
		QTRY_COMPARE(server->idMap.value(localId)->bindableSummary().value(), QStringLiteral("After"));
		server->idMap.value(localId)->dismiss();
		QTest::qWait(150);
		QCOMPARE(fakeRemovals, removalsBefore);
		QCOMPARE(fakePrompts, promptsBefore);
		QCOMPARE(fakeActivations.size(), activationsBefore);
		QObject::disconnect(connection);
	}

	void recursiveCloseIsIgnored() {
		auto* server = NotificationServer::instance();
		Notification* notification = nullptr;
		auto connection = QObject::connect(server, &NotificationServer::notification, this,
		    [&](Notification* value) { value->setTracked(true); notification = value; });
		auto id = server->notify("fixture", 0, "", "Close", "", {}, {}, 5000);
		QVERIFY(notification);
		int closed = 0;
		QObject::connect(notification, &Notification::closed, this, [&] {
			++closed;
			// A QML closed handler commonly drops tracking again.
			if (closed == 1) notification->dismiss();
		});
		notification->dismiss();
		QCOMPARE(closed, 1);
		QVERIFY(!server->idMap.contains(id));
		QVERIFY(!server->trackedNotifications()->valueList().contains(notification));
		QObject::disconnect(connection);
	}

	void pendingSendsAreBounded() {
		auto* server = NotificationServer::instance();
		QVERIFY(!server->hasReceiver());
		for (int i = 0; i < 512; ++i) server->notifySend(QString::number(i), "", {});
		QCOMPARE(server->pendingSends.size(), 128);
		QCoreApplication::processEvents();
		QCOMPARE(server->pendingSends.size(), 128);
		QStringList delivered;
		auto connection = QObject::connect(server, &NotificationServer::notification, this,
		    [&](Notification* value) { delivered.append(value->bindableSummary().value()); });
		server->notifySend("latest", "", {});
		QCoreApplication::processEvents();
		QCOMPARE(delivered.size(), 128);
		QCOMPARE(delivered.first(), QStringLiteral("385"));
		QCOMPARE(delivered.last(), QStringLiteral("latest"));
		QVERIFY(server->pendingSends.isEmpty());
		QObject::disconnect(connection);
	}

	// Finding 1: the prompt runs on the UI thread and the answer is applied by
	// the worker through a queued call, never by blocking the caller.
	void consentPromptRunsOnUiThreadAndResolvesOnWorker() {
		const int promptsBefore = fakePrompts;
		fakeStatus = SeamStatus::Unspecified;
		fakeRequestThread = nullptr;

		ToastMirror mirror;
		QCOMPARE(mirror.access(), SystemNotificationAccess::Unknown);

		mirror.requestAccess();
		QCOMPARE(fakePrompts, promptsBefore + 1);
		QCOMPARE(fakeRequestThread, QCoreApplication::instance()->thread());
		// The seam answered on the calling thread, but the listener state is
		// owned by the worker, so nothing is applied until the worker reports.
		QCOMPARE(mirror.access(), SystemNotificationAccess::Unknown);
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Allowed, 5000);
	}

	// Finding 2: the old code stopped the access timer after a grant, so a
	// revocation in Settings left the listener silent with banner suppression
	// on. The backstop now keeps querying GetAccessStatus and stops the mirror
	// as soon as consent is lost, without prompting.
	void revocationStopsMirrorWithoutPrompting() {
		const int promptsBefore = fakePrompts;
		fakeStatus = SeamStatus::Allowed;

		ToastMirror mirror;
		QSignalSpy activeSpy(&mirror, &ToastMirror::activeChanged);

		mirror.setEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Allowed);

		// The backstop keeps reading consent while the grant is in force.
		const int queriesBefore = fakeStatusQueries;
		QTRY_VERIFY_WITH_TIMEOUT(fakeStatusQueries > queriesBefore, 5000);

		fakeStatus = SeamStatus::Denied;
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Denied, 5000);
		QVERIFY(!mirror.isActive());
		QVERIFY(activeSpy.count() >= 1);
		QCOMPARE(fakePrompts, promptsBefore);

		// A re-grant read by the same non-prompting backstop restores the
		// mirror and therefore banner suppression.
		fakeStatus = SeamStatus::Allowed;
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Allowed);
		QCOMPARE(fakePrompts, promptsBefore);

		mirror.setEnabled(false);
		QTRY_VERIFY(!mirror.isActive());
	}

	// Finding 3: a held mutex is not ownership. The waiter stays unowned until
	// the owner releases, then takes over; destroying it hands over again.
	void mutexOwnershipWaitsForReleaseAndHandsOff() {
		const auto name = QStringLiteral("Local\\caelestia-consent-mutex-%1")
		                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
		                      .toStdWString();

		std::atomic<bool> started = false;
		std::atomic<bool> release = false;
		std::thread holder([&] {
			auto mutex = CreateMutexW(nullptr, TRUE, name.c_str());
			started.store(true, std::memory_order_release);
			while (!release.load(std::memory_order_acquire)) Sleep(5);
			ReleaseMutex(mutex);
			CloseHandle(mutex);
		});
		QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 5000);

		std::atomic<int> acquisitions = 0;
		{
			SessionBusLock lock(name, [&](SessionBusLock::Result result, DWORD) {
				if (result == SessionBusLock::Result::Owned) acquisitions.fetch_add(1, std::memory_order_release);
			});

			// The named mutex exists but another thread owns it; the old code
			// would already have treated this process as the owner.
			QTest::qWait(150);
			QCOMPARE(acquisitions.load(std::memory_order_acquire), 0);
			QVERIFY(!lock.owned());

			release.store(true, std::memory_order_release);
			QTRY_COMPARE_WITH_TIMEOUT(acquisitions.load(std::memory_order_acquire), 1, 5000);
			QVERIFY(lock.owned());
		}

		// Destruction released the real ownership; the next shell's waiter
		// takes over without a claim-retry interval.
		std::atomic<int> handovers = 0;
		{
			SessionBusLock lock(name, [&](SessionBusLock::Result result, DWORD) {
				if (result == SessionBusLock::Result::Owned) handovers.fetch_add(1, std::memory_order_release);
			});
			QTRY_COMPARE_WITH_TIMEOUT(handovers.load(std::memory_order_acquire), 1, 5000);
			QVERIFY(lock.owned());
		}

		holder.join();
	}

	// A previous shell that died while owning the mutex abandons it; the OS
	// reports WAIT_ABANDONED, which counts as acquired.
	void abandonedMutexCountsAsOwnership() {
		const auto name = QStringLiteral("Local\\caelestia-consent-mutex-%1")
		                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
		                      .toStdWString();

		HANDLE abandoned = nullptr;
		std::atomic<bool> started = false;
		std::thread holder([&] {
			abandoned = CreateMutexW(nullptr, TRUE, name.c_str());
			started.store(true, std::memory_order_release);
			// Exit without ReleaseMutex: the OS abandons the ownership.
		});
		QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 5000);
		holder.join();
		QVERIFY(abandoned != nullptr);

		std::atomic<int> acquisitions = 0;
		SessionBusLock lock(name, [&](SessionBusLock::Result result, DWORD) {
			if (result == SessionBusLock::Result::Owned) acquisitions.fetch_add(1, std::memory_order_release);
		});
		QTRY_COMPARE_WITH_TIMEOUT(acquisitions.load(std::memory_order_acquire), 1, 5000);
		QVERIFY(lock.owned());
		CloseHandle(abandoned);
	}

	// Blocking fix: a failed status read is not "still Allowed". The mirror and
	// banner suppression are withdrawn, and the backstop keeps reading so a
	// later success recovers without a prompt.
	void failedAccessReadWithdrawsSuppressionAndRecovers() {
		const int promptsBefore = fakePrompts;
		fakeStatusReadable = true;
		fakeStatus = SeamStatus::Allowed;

		ToastMirror mirror;
		mirror.setEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Allowed);

		fakeStatusReadable = false;
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Unavailable, 5000);
		QVERIFY(!mirror.isActive());

		// The non-prompting backstop keeps running while consent is unreadable.
		const int queriesBefore = fakeStatusQueries;
		QTRY_VERIFY_WITH_TIMEOUT(fakeStatusQueries > queriesBefore, 5000);
		QCOMPARE(fakePrompts, promptsBefore);

		fakeStatusReadable = true;
		fakeStatus = SeamStatus::Allowed;
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		QCOMPARE(mirror.access(), SystemNotificationAccess::Allowed);
		QCOMPARE(fakePrompts, promptsBefore);
		mirror.setEnabled(false);
	}

	// The AppCapability.AccessChanged event is the primary signal; with the
	// backstop interval parked, only the event can drive these transitions.
	void accessChangeEventRechecksWithoutPolling() {
		struct IntervalGuard {
			int saved;
			~IntervalGuard() { ToastMirrorWorker::testAccessCheckIntervalMs = this->saved; }
		} guard {ToastMirrorWorker::testAccessCheckIntervalMs};
		ToastMirrorWorker::testAccessCheckIntervalMs = 60000;
		fakeStatus = SeamStatus::Allowed;

		ToastMirror mirror;
		mirror.setEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);

		fakeStatus = SeamStatus::Denied;
		triggerFakeAccessChanged();
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Denied, 5000);
		QVERIFY(!mirror.isActive());

		fakeStatus = SeamStatus::Allowed;
		triggerFakeAccessChanged();
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		mirror.setEnabled(false);
	}

	// The subscribed capability event is the primary consent signal, so an
	// active mirror only needs the slow backstop for a missed event. The two
	// second poll is for the case where the subscription itself failed.
	void subscribedAccessEventSlowsTheActiveBackstop() {
		struct IntervalGuard {
			int saved;
			~IntervalGuard() { ToastMirrorWorker::testAccessCheckIntervalMs = this->saved; }
		} guard {ToastMirrorWorker::testAccessCheckIntervalMs};
		// Park the fixture override so the production intervals apply.
		ToastMirrorWorker::testAccessCheckIntervalMs = 0;

		fakeStatusReadable = true;
		fakeStatus = SeamStatus::Allowed;

		ToastMirror mirror;
		mirror.setEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);

		// `initTestCase` installs the AccessChanged seam, so the backstop must be
		// slow while mirroring: two idle seconds is inside the old 2 s poll.
		const int queriesBefore = fakeStatusQueries;
		QTest::qWait(2500);
		QCOMPARE(fakeStatusQueries, queriesBefore);

		// The event still applies a consent change immediately.
		fakeStatus = SeamStatus::Denied;
		triggerFakeAccessChanged();
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Denied, 5000);
		QVERIFY(!mirror.isActive());

		fakeStatus = SeamStatus::Allowed;
		triggerFakeAccessChanged();
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);
		mirror.setEnabled(false);
		QTRY_VERIFY(!mirror.isActive());
	}

	// Without a capability subscription nothing but the backstop reports a
	// consent change, so the active mirror keeps polling every two seconds.
	void failedAccessEventSubscriptionKeepsTheFastActiveBackstop() {
		struct IntervalGuard {
			int saved;
			~IntervalGuard() { ToastMirrorWorker::testAccessCheckIntervalMs = this->saved; }
		} guard {ToastMirrorWorker::testAccessCheckIntervalMs};
		ToastMirrorWorker::testAccessCheckIntervalMs = 0;

		auto& seam = ToastListenerSeam::instance();
		seam.watchAccessChanged = nullptr; // the subscription is unavailable
		fakeStatusReadable = true;
		fakeStatus = SeamStatus::Allowed;

		ToastMirror mirror;
		mirror.setEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(mirror.isActive(), 5000);

		const int queriesBefore = fakeStatusQueries;
		QTRY_VERIFY_WITH_TIMEOUT(fakeStatusQueries > queriesBefore, 5000);

		mirror.setEnabled(false);
		QTRY_VERIFY(!mirror.isActive());
		seam.watchAccessChanged = &fakeWatchAccessChanged;
	}

	// Observation is independent of active mirroring: the backstop watches a
	// denied grant and the following re-grant while mirroring is off.
	void observationSurvivesMirroringWithdrawal() {
		fakeStatusReadable = true;
		fakeStatus = SeamStatus::Allowed;

		ToastMirror mirror;
		mirror.setAccessObservation(true);
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Allowed, 5000);
		QVERIFY(!mirror.isActive());

		fakeStatus = SeamStatus::Denied;
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Denied, 5000);
		QVERIFY(!mirror.isActive());

		fakeStatus = SeamStatus::Allowed;
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Allowed, 5000);
		QVERIFY(!mirror.isActive());

		mirror.setAccessObservation(false);
	}

	// Real asynchronous prompt: the caller returns with no answer, a second
	// request does not stack a dialog, and completion is applied by the worker.
	void consentCompletesAsynchronously() {
		auto& seam = ToastListenerSeam::instance();
		seam.requestAccessAsync = &fakeRequestAccessAsync;
		fakeStatus = SeamStatus::Unspecified;
		fakeRequestThread = nullptr;
		const int promptsBefore = fakePrompts;

		ToastMirror mirror;
		mirror.requestAccess();
		QCOMPARE(fakePrompts, promptsBefore + 1);
		QCOMPARE(fakeRequestThread, QCoreApplication::instance()->thread());
		QCOMPARE(mirror.access(), SystemNotificationAccess::Unknown);

		// A pending prompt is not issued twice.
		mirror.requestAccess();
		QCOMPARE(fakePrompts, promptsBefore + 1);

		completeFakeRequest(SeamStatus::Allowed);
		QTRY_COMPARE_WITH_TIMEOUT(mirror.access(), SystemNotificationAccess::Allowed, 5000);
		seam.requestAccessAsync = nullptr;
	}

	// Destroying the frontend while the consent dialog is pending must not
	// leave a completion that touches freed memory.
	void destroyedFrontendWhilePromptPending() {
		auto& seam = ToastListenerSeam::instance();
		seam.requestAccessAsync = &fakeRequestAccessAsync;
		fakeStatus = SeamStatus::Unspecified;
		const int promptsBefore = fakePrompts;

		auto* mirror = new ToastMirror();
		mirror->requestAccess();
		QCOMPARE(fakePrompts, promptsBefore + 1);
		delete mirror;

		completeFakeRequest(SeamStatus::Allowed);
		QTest::qWait(50);
		seam.requestAccessAsync = nullptr;
	}

	// Test safety: acquiring ownership creates the bus window under the
	// run-unique class, never the production one, so this process cannot receive
	// a live shell's forwarded notifications nor be found by its forwarders.
	void ownershipUsesTheRunUniqueBusWindow() {
		auto* server = NotificationServer::instance();
		server->acquireOwnership();
		QVERIFY(server->mOwner);
		QVERIFY(server->busWindow != nullptr);

		QCOMPARE(
		    FindWindowExW(HWND_MESSAGE, nullptr, fakeBusClassName.c_str(), nullptr),
		    server->busWindow
		);
		// A live shell may own a production-class window; ours must not be it.
		QVERIFY(
		    FindWindowExW(HWND_MESSAGE, nullptr, L"QuickshellNotificationBus", nullptr)
		    != server->busWindow
		);

		wchar_t className[128] {};
		QVERIFY(GetClassNameW(server->busWindow, className, 128) > 0);
		QCOMPARE(std::wstring(className), fakeBusClassName);
	}

	// Observation outlives any QML object, so it is owned by generation: only
	// the latest holder can change it, an outgoing holder's late disable is
	// refused, and a replacement's explicit write always wins.
	void observationIsOwnedByGeneration() {
		auto* server = NotificationServer::instance();
		server->acquireOwnership();
		QVERIFY(server->mOwner);
		fakeStatus = SeamStatus::Allowed;

		// Isolate observation: with mirroring off, the access-change watch is
		// held by the observer alone.
		server->setMirrorEnabled(false);
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback == nullptr, 5000);

		// No holder yet: neither zero nor an unissued generation can write.
		QVERIFY(!server->setSystemAccessObservation(true, 0));
		QVERIFY(!server->setSystemAccessObservation(true, 12345));
		QVERIFY(!server->mObservationWanted);

		const auto first = server->claimAccessObservation();
		QVERIFY(first != 0);
		QVERIFY(server->setSystemAccessObservation(true, first));
		QVERIFY(server->mObservationWanted);
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback != nullptr, 5000);

		// A replacement claims the next generation. The outgoing holder is
		// destroyed late and tries to disable: refused, observation continues.
		const auto second = server->claimAccessObservation();
		QVERIFY(second != first);
		QVERIFY(!server->setSystemAccessObservation(false, first));
		QVERIFY(server->mObservationWanted);
		QTest::qWait(100);
		QVERIFY(fakeAccessChangedCallback != nullptr);

		// The replacement is the holder, so its explicit opt-out is honoured.
		QVERIFY(server->setSystemAccessObservation(false, second));
		QVERIFY(!server->mObservationWanted);
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback == nullptr, 5000);

		// An opted-out replacement supersedes an observing holder and stops it
		// even though it never enabled anything itself.
		const auto third = server->claimAccessObservation();
		QVERIFY(server->setSystemAccessObservation(true, third));
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback != nullptr, 5000);
		const auto fourth = server->claimAccessObservation();
		QVERIFY(server->setSystemAccessObservation(false, fourth));
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback == nullptr, 5000);
		QVERIFY(!server->setSystemAccessObservation(true, third));
		QTest::qWait(100);
		QVERIFY(!server->mObservationWanted);
		QVERIFY(fakeAccessChangedCallback == nullptr);

		// Leave the server as the following state expects: mirroring wanted.
		server->setMirrorEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(server->isSystemMirrorActive(), 5000);
	}

	// The QML facade holds its generation as a lease, so observation cannot
	// outlive the facade whatever order the objects around it are destroyed in.
	// The facade's destructor is exactly the lease's destructor.
	void observationLeaseFollowsFacadeLifetime() {
		auto* server = NotificationServer::instance();
		server->acquireOwnership();
		QVERIFY(server->mOwner);
		fakeStatus = SeamStatus::Allowed;
		server->setMirrorEnabled(false);
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback == nullptr, 5000);

		// Facade destroyed first while observing: observation goes off even
		// though no QML-side release ever ran.
		{
			auto lease = std::make_unique<ObservationLease>();
			const auto generation = lease->claim();
			QVERIFY(generation != 0);
			QCOMPARE(lease->generation(), generation);
			QVERIFY(server->setSystemAccessObservation(true, generation));
			QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback != nullptr, 5000);
			lease.reset();
		}
		QVERIFY(!server->mObservationWanted);
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback == nullptr, 5000);

		// A stale facade destroyed after a replacement facade claimed cannot
		// disable the new holder.
		auto stale = std::make_unique<ObservationLease>();
		QVERIFY(server->setSystemAccessObservation(true, stale->claim()));
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback != nullptr, 5000);

		auto current = std::make_unique<ObservationLease>();
		QVERIFY(current->claim() != stale->generation());
		QVERIFY(server->setSystemAccessObservation(true, current->generation()));

		stale.reset();
		QTest::qWait(100);
		QVERIFY(server->mObservationWanted);
		QVERIFY(fakeAccessChangedCallback != nullptr);

		// The QML-side release for the normal order, then the destructor:
		// releasing twice is a harmless no-op.
		QVERIFY(server->setSystemAccessObservation(false, current->generation()));
		QVERIFY(!server->mObservationWanted);
		QTRY_VERIFY_WITH_TIMEOUT(fakeAccessChangedCallback == nullptr, 5000);
		current->release();
		QCOMPARE(current->generation(), quint32(0));
		current.reset();
		QVERIFY(!server->mObservationWanted);
		QVERIFY(fakeAccessChangedCallback == nullptr);

		// Leave the server as the following state expects: mirroring wanted.
		server->setMirrorEnabled(true);
		QTRY_VERIFY_WITH_TIMEOUT(server->isSystemMirrorActive(), 5000);
	}

private:
	Notification* mLastNotification = nullptr;
};

} // namespace qs::windows::services::notifications

QTEST_GUILESS_MAIN(qs::windows::services::notifications::NotificationMirrorConsentTest)

#include "notification_mirror_consent_test.moc"
