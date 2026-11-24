#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <qscopeguard.h>
#include <qtest.h>

#include "../services/systray/seed.hpp"
#include "../services/systray/worker_stop.hpp"
#include "../window_identity_policy.hpp"

namespace qs::windows::services::systray {
QImage imageFromIcon(HICON) { return {}; }
} // namespace qs::windows::services::systray

using namespace qs::windows;
using namespace qs::windows::services::systray;

namespace {

class ScopedHandle {
public:
	explicit ScopedHandle(HANDLE handle): handle(handle) {}
	~ScopedHandle() {
		if (this->handle != nullptr) CloseHandle(this->handle);
	}
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle& operator=(ScopedHandle&&) = delete;

	[[nodiscard]] HANDLE get() const { return this->handle; }
	explicit operator bool() const { return this->handle != nullptr; }

private:
	HANDLE handle;
};

struct BlockedWorkerState {
	std::atomic<bool> started = false;
	std::atomic<bool> finished = false;
};

class WedgedObserverBackend: public TrayNotifyBackend {
public:
	WedgedObserverBackend(HANDLE unregisterStarted, HANDLE release)
	    : unregisterStarted(unregisterStarted)
	    , release(release) {}

	Registration registerCallback(Callback callback) override {
		const auto lock = std::lock_guard(this->mutex);
		this->callback = std::move(callback);
		return {S_OK, 17, true};
	}

	void unregisterCallback() override {
		SetEvent(this->unregisterStarted);
		WaitForSingleObject(this->release, INFINITE);
		this->unregisterFinished.store(true, std::memory_order_release);
	}

	void fireLateCallback() {
		Callback callback;
		{
			const auto lock = std::lock_guard(this->mutex);
			callback = this->callback;
		}
		const TrayNotifyItem item {};
		if (callback) callback(2, &item);
	}

	std::atomic<bool> unregisterFinished = false;

private:
	HANDLE unregisterStarted;
	HANDLE release;
	std::mutex mutex;
	Callback callback;
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
		const auto release = ScopedHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		QVERIFY(bool(release));

		std::thread worker([state, release = release.get()] {
			state->started.store(true);
			WaitForSingleObject(release, INFINITE);
			state->finished.store(true);
		});
		const auto join = qScopeGuard([&] {
			if (worker.joinable()) {
				SetEvent(release.get());
				worker.join();
			}
		});

		QTRY_VERIFY(state->started.load());
		SetEvent(release.get());
		QCOMPARE(stopWorker(worker), WorkerStopResult::Joined);
		QVERIFY(state->finished.load());
	}

	void wedgedWorkerDetachesWithSafeState() {
		auto state = std::make_shared<BlockedWorkerState>();
		auto weakState = std::weak_ptr<BlockedWorkerState>(state);
		const auto release = ScopedHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		QVERIFY(bool(release));

		std::thread worker([state, release = release.get()] {
			state->started.store(true);
			WaitForSingleObject(release, INFINITE);
			state->finished.store(true);
		});
		const auto cleanup = qScopeGuard([&] {
			SetEvent(release.get());
			if (worker.joinable()) worker.join();
		});
		QTRY_VERIFY(state->started.load());

		auto started = std::chrono::steady_clock::now();
		QCOMPARE(stopWorker(worker), WorkerStopResult::Detached);
		auto elapsed = std::chrono::steady_clock::now() - started;
		QVERIFY(elapsed >= TRAY_WORKER_STOP_TIMEOUT);
		QVERIFY(!worker.joinable());

		// The detached thread still owns the state after the caller drops its
		// reference. Releasing the blocked call must not touch freed memory.
		state.reset();
		SetEvent(release.get());
		QTRY_VERIFY_WITH_TIMEOUT(weakState.expired(), 10000);
	}

	void destroyedObserverSuppressesLateWorkerCallback() {
		const auto unregisterStarted = ScopedHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		const auto release = ScopedHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		QVERIFY(bool(unregisterStarted));
		QVERIFY(bool(release));

		std::shared_ptr<WedgedObserverBackend> backend;
		auto observer = std::make_unique<TrayNotifyObserver>([&] {
			backend = std::make_shared<WedgedObserverBackend>(unregisterStarted.get(), release.get());
			return backend;
		});
		const auto cleanup = qScopeGuard([&] {
			SetEvent(release.get());
			if (observer) observer->stop();
		});
		std::atomic<int> delivered = 0;

		observer->start([&delivered](TrayIconMessage) { delivered.fetch_add(1); }, {});
		QTRY_COMPARE(observer->state(), ComObserverState::Live);

		auto started = std::chrono::steady_clock::now();
		observer->stop();
		auto elapsed = std::chrono::steady_clock::now() - started;
		QVERIFY(elapsed >= TRAY_WORKER_STOP_TIMEOUT);
		QCOMPARE(WaitForSingleObject(unregisterStarted.get(), 0), DWORD(WAIT_OBJECT_0));

		observer.reset();
		backend->fireLateCallback();
		QCOMPARE(delivered.load(), 0);

		SetEvent(release.get());
		QTRY_VERIFY_WITH_TIMEOUT(backend->unregisterFinished.load(std::memory_order_acquire), 10000);
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
		        [responding] { return responding; },
		        isFixtureShellHost,
		        isFixtureShellClass
		    ),
		    expected
		);
	}

	void shellWindowsSkipResponsivenessProbe() {
		auto probed = false;
		const auto query = [&] {
			probed = true;
			return true;
		};

		QVERIFY(!shouldQueryWindowPropertyStore(
		    QStringLiteral("explorer.exe"),
		    QStringLiteral("CabinetWClass"),
		    query,
		    isFixtureShellHost,
		    isFixtureShellClass
		));
		QVERIFY(!probed);
		QVERIFY(shouldQueryWindowPropertyStore(
		    QStringLiteral("foreign.exe"),
		    QStringLiteral("CabinetWClass"),
		    query,
		    isFixtureShellHost,
		    isFixtureShellClass
		));
		QVERIFY(probed);
	}
};

QTEST_GUILESS_MAIN(ExplorerHangHardeningTest)
#include "explorer_hang_hardening_test.moc"
