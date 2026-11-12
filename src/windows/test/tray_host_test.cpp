#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qpixmap.h>
#include <qprocess.h>
#include <qt_windows.h>
#include <qtest.h>
#include <qvariant.h>
#include <shellapi.h>

#include "../services/systray/host.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace qs::windows::startup {
void afterFirstFrame(QObject*, std::function<void()>) {}
} // namespace qs::windows::startup

namespace qs::windows {
QPixmap iconForKey(const QString&, const QSize&) { return {}; }
} // namespace qs::windows

namespace qs::redact {
QString logId(const QString& value) { return value; }
} // namespace qs::redact

namespace qs::windows::services::systray {
void TrayHook::start(
    TrayIconSink,
    std::function<void()>,
    bool,
    std::function<void(TrayHookState)>
) {}

void TrayHook::stop() {}
bool TrayHook::running() { return false; }
QVariantMap TrayHook::capability() { return {}; }
void TrayHook::announceTo(std::vector<HWND>) {}
void seedFromExplorer(TrayIconSink, std::function<void(bool)>) {}
} // namespace qs::windows::services::systray

using namespace qs::windows::services::systray;

class TrayHostTest: public QObject {
	Q_OBJECT

private slots:
	void initTestCase() { qputenv("QS_WINDOWS_TRAY_HOOK", "0"); }
	void cleanupTestCase() { qunsetenv("QS_WINDOWS_TRAY_HOOK"); }

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
