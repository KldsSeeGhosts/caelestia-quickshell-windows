#include <memory>
#include <utility>

#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qpixmap.h>
#include <qprocess.h>
#include <qt_windows.h>
#include <qtest.h>
#include <qvariant.h>
#include <shellapi.h>

#include "../services/systray/host.hpp"
#include "../supervisor_bridge.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace qs::windows::startup {
void afterFirstFrame(QObject* context, std::function<void()> callback) {
	QMetaObject::invokeMethod(context, std::move(callback), Qt::QueuedConnection);
}
} // namespace qs::windows::startup

namespace qs::windows {
QPixmap iconForKey(const QString&, const QSize&) { return {}; }

void requestShellIcon(
    const QString&,
    const QSize&,
    std::function<bool()> cancelled,
    std::function<void(QImage)> done
) {
	if (!cancelled || !cancelled()) done({});
}
} // namespace qs::windows

namespace qs::redact {
QString logId(const QString& value) { return value; }
} // namespace qs::redact

namespace qs::windows::services::systray {
namespace {
ComObserverState observerResult = ComObserverState::Live;
bool observerCanRetry = false;
std::function<void(ComObserverState)> observerStateChanged;
int observerStarts = 0;
int observerResyncs = 0;
DWORD observerThreadId = 0;
bool observerRetryable = false;
ComObserverState observerState = ComObserverState::Inactive;
QList<bool> hookStarts;
QList<bool> hookModeChanges;
} // namespace

void TrayHook::start(
    TrayIconSink,
    std::function<void()>,
    bool brief,
    std::function<void(TrayHookState)> stateChanged
) {
	hookStarts.append(brief);
	stateChanged(brief ? TrayHookState::RunningBrief : TrayHookState::Running);
}

void TrayHook::setBrief(bool brief) { hookModeChanges.append(brief); }
void TrayHook::stop() {}
bool TrayHook::running() { return false; }
QVariantMap TrayHook::capability() { return {}; }
void TrayHook::announceTo(std::vector<HWND>) {}

TrayNotifyObserver::TrayNotifyObserver(BackendFactory) {}
TrayNotifyObserver::~TrayNotifyObserver() = default;

void TrayNotifyObserver::start(TrayIconSink, std::function<void(ComObserverState)> onState) {
	observerStarts++;
	observerStateChanged = std::move(onState);
	observerThreadId = GetCurrentThreadId();
	observerStateChanged(ComObserverState::Connecting);
	observerRetryable = observerCanRetry;
	observerState = observerResult;
	observerStateChanged(observerResult);
}

void TrayNotifyObserver::resync() {
	if (observerThreadId == 0) return;
	observerResyncs++;
	observerStateChanged(ComObserverState::Connecting);
	observerRetryable = observerCanRetry;
	observerState = observerResult;
	observerStateChanged(observerResult);
}

void TrayNotifyObserver::stop() {
	observerThreadId = 0;
	observerRetryable = false;
	observerState = ComObserverState::Inactive;
	observerStateChanged = {};
}

ComObserverState TrayNotifyObserver::state() const { return observerState; }
bool TrayNotifyObserver::canRetry() const { return observerRetryable; }
} // namespace qs::windows::services::systray

using namespace qs::windows::services::systray;

class TrayHostTest: public QObject {
	Q_OBJECT

private slots:
	void init() {
		qputenv("QS_WINDOWS_TRAY_HOOK", "0");
		observerResult = ComObserverState::Live;
		observerCanRetry = false;
		observerStarts = 0;
		observerResyncs = 0;
		observerThreadId = 0;
		observerRetryable = false;
		observerState = ComObserverState::Inactive;
		hookStarts.clear();
		hookModeChanges.clear();
	}
	void cleanup() { qunsetenv("QS_WINDOWS_TRAY_HOOK"); }

	void transportByObserverOutcome_data();
	void transportByObserverOutcome();
	void briefStopsRetryOnLiveOrStub_data();
	void briefStopsRetryOnLiveOrStub();
	void explorerRestartResyncsObserver();

	// An owner that exits without NIM_DELETE must lose its icon right away.
	// The old implementation only noticed on its 10 s prune timer.
	void ownerExitRemovesItsIcon();
	// Every icon owns one process-handle wait, and removing the icon releases
	// it. A leak here would show up as hundreds of handles after the loop.
	void manyAddRemoveCyclesDoNotLeakWaitHandles();

private:
	static TrayIconMessage iconMessage(HWND hwnd, UINT uid);
	static HWND createTestWindow();
	static QString ownerHelperPath();
	static DWORD processHandleCount();
};

void TrayHostTest::transportByObserverOutcome_data() {
	QTest::addColumn<QByteArray>("mode");
	QTest::addColumn<int>("state");
	QTest::addColumn<QString>("transport");
	QTest::addColumn<bool>("brief");
	QTest::addColumn<bool>("promoted");
	QTest::newRow("com-live") << QByteArray("com") << int(ComObserverState::Live)
	                          << QStringLiteral("com-hybrid") << true << false;
	QTest::newRow("com-stub") << QByteArray("com") << int(ComObserverState::StubNoOp)
	                          << QStringLiteral("mirror") << true << true;
	QTest::newRow("com-unavailable") << QByteArray("com") << int(ComObserverState::Unavailable)
	                                 << QStringLiteral("mirror") << true << true;
	QTest::newRow("brief-live") << QByteArray("brief") << int(ComObserverState::Live)
	                            << QStringLiteral("brief") << true << false;
	QTest::newRow("brief-stub") << QByteArray("brief") << int(ComObserverState::StubNoOp)
	                            << QStringLiteral("brief") << true << false;
	QTest::newRow("brief-permanent-failure")
	    << QByteArray("brief") << int(ComObserverState::Unavailable) << QStringLiteral("brief")
	    << true << false;
	QTest::newRow("default") << QByteArray("") << int(ComObserverState::StubNoOp)
	                         << QStringLiteral("mirror") << false << false;
	QTest::newRow("disabled") << QByteArray("0") << int(ComObserverState::Inactive)
	                          << QStringLiteral("disabled") << false << false;
}

void TrayHostTest::transportByObserverOutcome() {
	QFETCH(QByteArray, mode);
	QFETCH(int, state);
	QFETCH(QString, transport);
	QFETCH(bool, brief);
	QFETCH(bool, promoted);
	qputenv("QS_WINDOWS_TRAY_HOOK", mode);
	observerResult = static_cast<ComObserverState>(state);
	const auto disabled = observerResult == ComObserverState::Inactive;
	auto host = std::unique_ptr<TrayHost>(TrayHost::createForTest());

	QTRY_COMPARE(
	    host->hookCapability().value("comObserver").toString(),
	    comObserverStateName(observerResult)
	);
	QCOMPARE(host->hookCapability().value("transport").toString(), transport);
	QCOMPARE(
	    host->hookCapability().value("availability").toString(),
	    disabled ? "disabled" : "available"
	);
	QCOMPARE(host->hookCapability().value("reason").toString(), disabled ? "hook-disabled" : "");
	QCOMPARE(observerStarts, disabled ? 0 : 1);
	QCOMPARE(hookStarts, disabled ? QList<bool>() : QList<bool> {brief});
	QCOMPARE(hookModeChanges, promoted ? QList<bool> {false} : QList<bool>());
	QVERIFY(!host->pollActiveForTest());
}

void TrayHostTest::briefStopsRetryOnLiveOrStub_data() {
	QTest::addColumn<int>("state");
	QTest::newRow("live") << int(ComObserverState::Live);
	QTest::newRow("stub") << int(ComObserverState::StubNoOp);
}

void TrayHostTest::briefStopsRetryOnLiveOrStub() {
	QFETCH(int, state);
	qputenv("QS_WINDOWS_TRAY_HOOK", "brief");
	observerResult = ComObserverState::Unavailable;
	observerCanRetry = true;
	auto host = std::unique_ptr<TrayHost>(TrayHost::createForTest());
	QTRY_VERIFY(host->pollActiveForTest());

	observerResult = static_cast<ComObserverState>(state);
	QTRY_COMPARE(
	    host->hookCapability().value("comObserver").toString(),
	    comObserverStateName(observerResult)
	);
	QCOMPARE(observerResyncs, 1);
	QVERIFY(!host->pollActiveForTest());
	QVERIFY(hookModeChanges.isEmpty());
}

void TrayHostTest::explorerRestartResyncsObserver() {
	qputenv("QS_WINDOWS_TRAY_HOOK", "com");
	observerResult = ComObserverState::StubNoOp;
	auto host = std::unique_ptr<TrayHost>(TrayHost::createForTest());
	QTRY_COMPARE(host->hookCapability().value("comObserver").toString(), "stub-noop");
	QCOMPARE(observerResyncs, 0);

	observerResult = ComObserverState::Live;
	emit qs::windows::SupervisorBridge::instance() -> explorerSettled(1234, 5678, 1, true);
	QCOMPARE(observerResyncs, 0);

	emit qs::windows::SupervisorBridge::instance() -> explorerSettled(1234, 5679, 2, true);
	QCOMPARE(observerResyncs, 1);
	QTRY_COMPARE(host->hookCapability().value("comObserver").toString(), "live");
	QCOMPARE(host->hookCapability().value("transport").toString(), "com-hybrid");
	const QList<bool> expected {false, true};
	QCOMPARE(hookModeChanges, expected);
}

TrayIconMessage TrayHostTest::iconMessage(HWND hwnd, UINT uid) {
	TrayIconMessage message;
	message.message = NIM_ADD;
	message.hwnd = hwnd;
	message.uid = uid;
	// A known GUID keeps the fixture off the title lookup and the file icon.
	message.flags = NIF_TIP | NIF_GUID;
	message.tip = QStringLiteral("fixture");
	message.guid = QUuid("{7820ae76-23e3-4229-82c1-e41cb67d5b9c}");
	message.exePath = QStringLiteral("C:\\qs-tray-fixture\\owner.exe");
	return message;
}

HWND TrayHostTest::createTestWindow() {
	static const auto className = L"QsTrayHostTestWindow";

	WNDCLASSW windowClass {};
	windowClass.lpfnWndProc = DefWindowProcW;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = className;
	RegisterClassW(&windowClass);

	return CreateWindowExW(
	    0,
	    className,
	    L"tray host test",
	    WS_OVERLAPPEDWINDOW,
	    0,
	    0,
	    64,
	    64,
	    nullptr,
	    nullptr,
	    windowClass.hInstance,
	    nullptr
	);
}

QString TrayHostTest::ownerHelperPath() {
	return QCoreApplication::applicationDirPath() + QStringLiteral("/tray_owner_helper.exe");
}

DWORD TrayHostTest::processHandleCount() {
	DWORD count = 0;
	GetProcessHandleCount(GetCurrentProcess(), &count);
	return count;
}

void TrayHostTest::ownerExitRemovesItsIcon() {
	QProcess owner;
	owner.setProgram(ownerHelperPath());
	owner.start();
	QVERIFY2(owner.waitForStarted(5000), qPrintable(owner.errorString()));
	QVERIFY2(owner.waitForReadyRead(5000), qPrintable(owner.errorString()));

	auto line = owner.readLine().trimmed();
	auto ok = false;
	auto raw = line.toULongLong(&ok);
	QVERIFY2(ok, qPrintable(line));

	auto* hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(raw));
	QVERIFY(IsWindow(hwnd));

	auto* host = TrayHost::createForTest();
	host->applyForTest(iconMessage(hwnd, 1));
	QCOMPARE(host->items().size(), 1);
	QCOMPARE(host->ownerWaitCountForTest(), 1);

	QElapsedTimer elapsed;
	elapsed.start();
	owner.kill();
	QVERIFY(owner.waitForFinished(5000));

	QTRY_VERIFY_WITH_TIMEOUT(host->items().isEmpty(), 2000);
	QVERIFY2(elapsed.elapsed() < 2000, "the icon outlived its owner by more than 2 s");
	QCOMPARE(host->ownerWaitCountForTest(), 0);

	delete host;
}

void TrayHostTest::manyAddRemoveCyclesDoNotLeakWaitHandles() {
	auto window = createTestWindow();
	QVERIFY(window != nullptr);

	auto* host = TrayHost::createForTest();

	// The first cycle initializes the thread pool and Qt handles.
	host->applyForTest(iconMessage(window, 1));
	QCOMPARE(host->ownerWaitCountForTest(), 1);

	TrayIconMessage remove;
	remove.message = NIM_DELETE;
	remove.hwnd = window;
	remove.uid = 1;
	host->applyForTest(remove);
	QCOMPARE(host->ownerWaitCountForTest(), 0);

	auto baseline = processHandleCount();

	for (UINT uid = 2; uid < 202; uid++) {
		host->applyForTest(iconMessage(window, uid));
		QCOMPARE(host->ownerWaitCountForTest(), 1);

		remove.uid = uid;
		host->applyForTest(remove);
		QCOMPARE(host->ownerWaitCountForTest(), 0);
	}

	QCoreApplication::processEvents();
	auto after = processHandleCount();
	QVERIFY2(
	    after <= baseline + 8,
	    qPrintable(QStringLiteral("%1 handles leaked").arg(after - baseline))
	);

	delete host;
	DestroyWindow(window);
}

QTEST_GUILESS_MAIN(TrayHostTest)
#include "tray_host_test.moc"
