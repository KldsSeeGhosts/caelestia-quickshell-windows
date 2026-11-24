#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <qtest.h>

#include "../services/systray/worker_stop.hpp"
#include "../window_identity_policy.hpp"

using namespace qs::windows;
using namespace qs::windows::services::systray;

namespace {

struct BlockedWorkerState {
	std::atomic<bool> started = false;
	std::atomic<bool> finished = false;
};

bool isFixtureShellHost(const QString& processName) {
	return processName.compare(QStringLiteral("ShellExperienceHost.exe"), Qt::CaseInsensitive) == 0;
}

bool isFixtureShellClass(const QString& windowClass) {
	return windowClass.compare(QStringLiteral("Shell_TrayWnd"), Qt::CaseInsensitive) == 0;
}

} // namespace

class ExplorerHangHardeningTest: public QObject {
	Q_OBJECT

private slots:
	void finishedWorkerJoins() {
		auto state = std::make_shared<BlockedWorkerState>();
		auto* release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		QVERIFY(release != nullptr);

		std::thread worker([state, release] {
			state->started.store(true);
			WaitForSingleObject(release, INFINITE);
			state->finished.store(true);
		});

		QTRY_VERIFY(state->started.load());
		SetEvent(release);
		QCOMPARE(stopWorker(worker), WorkerStopResult::Joined);
		QVERIFY(state->finished.load());
		CloseHandle(release);
	}

	void wedgedWorkerDetachesWithSafeState() {
		auto state = std::make_shared<BlockedWorkerState>();
		auto weakState = std::weak_ptr<BlockedWorkerState>(state);
		auto* release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		QVERIFY(release != nullptr);

		std::thread worker([state, release] {
			state->started.store(true);
			WaitForSingleObject(release, INFINITE);
			state->finished.store(true);
		});
		QTRY_VERIFY(state->started.load());

		auto started = std::chrono::steady_clock::now();
		QCOMPARE(stopWorker(worker), WorkerStopResult::Detached);
		auto elapsed = std::chrono::steady_clock::now() - started;
		QVERIFY(elapsed >= TRAY_WORKER_STOP_TIMEOUT);
		QVERIFY(elapsed < TRAY_WORKER_STOP_TIMEOUT + std::chrono::milliseconds(1000));
		QVERIFY(!worker.joinable());

		// The detached thread still owns the state after the caller drops its
		// reference. Releasing the blocked call must not touch freed memory.
		state.reset();
		SetEvent(release);
		QTRY_VERIFY(weakState.expired());
		CloseHandle(release);
	}

	void windowPropertyStoreGuard_data() {
		QTest::addColumn<QString>("processName");
		QTest::addColumn<QString>("windowClass");
		QTest::addColumn<bool>("responding");
		QTest::addColumn<bool>("expected");

		QTest::newRow("explorer-responsive") << "explorer.exe" << "CabinetWClass" << true << false;
		QTest::newRow("explorer-unresponsive") << "explorer.exe" << "Shell_TrayWnd" << false << false;
		QTest::newRow("shell-host") << "ShellExperienceHost.exe" << "Windows.UI.Core.CoreWindow" << true
		                            << false;
		QTest::newRow("shell-class") << "foreign.exe" << "Shell_TrayWnd" << true << false;
		QTest::newRow("foreign-responsive") << "foreign.exe" << "CabinetWClass" << true << true;
		QTest::newRow("foreign-unresponsive") << "foreign.exe" << "CabinetWClass" << false << false;
	}

	void windowPropertyStoreGuard() {
		QFETCH(QString, processName);
		QFETCH(QString, windowClass);
		QFETCH(bool, responding);
		QFETCH(bool, expected);

		QCOMPARE(
		    shouldQueryWindowPropertyStore(
		        processName,
		        windowClass,
		        responding,
		        isFixtureShellHost,
		        isFixtureShellClass
		    ),
		    expected
		);
	}
};

QTEST_GUILESS_MAIN(ExplorerHangHardeningTest)
#include "explorer_hang_hardening_test.moc"
