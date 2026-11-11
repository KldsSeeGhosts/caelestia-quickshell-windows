// An app launch that never returns must not keep qs.exe alive after quit.
//
// Activation and ShellExecuteExW can block for as long as the shell or the
// launched app decides. ~QCoreApplication waits for QThreadPool::globalInstance()
// without a bound, so a stuck launch there kept the old shell process running
// after an update asked it to quit: it still owned the desktop session and the
// update failed. Launches go through runLauncher, whose pool nothing waits for
// at exit. Each case runs a child copy of this executable that blocks one task
// forever and then returns from main.

#include <cstdio>

#include <qcoreapplication.h>
#include <qfile.h>
#include <qlogging.h>
#include <qprocess.h>
#include <qsemaphore.h>
#include <qtest.h>
#include <qthreadpool.h>
#include <qt_windows.h>

#include "util.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace {

HANDLE gNever = nullptr; // NOLINT

int runBlockedChild(bool global) {
	int argc = 1;
	char name[] = "launcher_exit_child";
	char* argv[] = {name, nullptr};
	QCoreApplication app(argc, argv);

	gNever = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	QSemaphore started;
	auto task = [&started] {
		started.release();
		WaitForSingleObject(gNever, INFINITE);
	};

	if (global) {
		QThreadPool::globalInstance()->start(task);
	} else {
		qs::windows::runLauncher(task);
	}

	started.acquire();
	std::printf("started\n");
	std::fflush(stdout);
	return 0; // Destroys the application while the task is still blocked.
}

} // namespace

class TestLauncherExit: public QObject {
	Q_OBJECT

private slots:
	void blockedLaunchDoesNotHoldExit() {
		QProcess child;
		child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--launcher-blocked")});
		QVERIFY(child.waitForStarted(5000));
		QVERIFY(child.waitForFinished(10000));
		QCOMPARE(child.exitStatus(), QProcess::NormalExit);
		QCOMPARE(child.exitCode(), 0);
		QVERIFY(child.readAllStandardOutput().contains("started"));
	}

	void blockedGlobalPoolTaskHoldsExit() {
		// Negative control: the same task on the global pool keeps the child
		// alive after main returns. This is the hang the launch path avoids.
		QProcess child;
		child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--global-blocked")});
		QVERIFY(child.waitForStarted(5000));
		QByteArray output;
		QTRY_VERIFY_WITH_TIMEOUT((output += child.readAllStandardOutput()).contains("started"), 5000);
		QVERIFY(!child.waitForFinished(3000));
		child.kill();
		QVERIFY(child.waitForFinished(5000));
	}

	void appLaunchUsesTheLauncherPool() {
		// Source guard for the production launch path.
		QFile source(QStringLiteral(QS_WINDOWS_SRC "/desktopentry_backend.cpp"));
		QVERIFY(source.open(QIODevice::ReadOnly));
		auto text = source.readAll();
		QVERIFY(!text.contains("QThreadPool::globalInstance()"));
		QVERIFY(text.contains("runLauncher("));
	}

	void settingsLaunchesUseTheLauncherPool_data() {
		QTest::addColumn<QString>("file");
		QTest::addColumn<int>("calls");
		QTest::newRow("wallpaper") << QStringLiteral("wallpaper") << 1;
		QTest::newRow("notifications") << QStringLiteral("notification_settings") << 1;
		QTest::newRow("network") << QStringLiteral("network") << 2;
		QTest::newRow("wifi-fallbacks") << QStringLiteral("network_wifi") << 2;
	}

	void settingsLaunchesUseTheLauncherPool() {
		QFETCH(QString, file);
		QFETCH(int, calls);
		QFile source(QStringLiteral(QS_WINDOWS_SRC "/system/") + file + ".cpp");
		QVERIFY(source.open(QIODevice::ReadOnly));
		auto text = source.readAll();
		QVERIFY(!text.contains("QThreadPool::globalInstance()"));
		QCOMPARE(text.count("runLauncher("), calls);
	}
};

int main(int argc, char** argv) {
	if (argc > 1 && qstrcmp(argv[1], "--launcher-blocked") == 0) return runBlockedChild(false);
	if (argc > 1 && qstrcmp(argv[1], "--global-blocked") == 0) return runBlockedChild(true);

	QCoreApplication app(argc, argv);
	TestLauncherExit test;
	return QTest::qExec(&test, argc, argv);
}

#include "launcher_exit_test.moc"
