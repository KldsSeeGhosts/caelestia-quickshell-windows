#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qsignalspy.h>
#include <qtest.h>
#include <qthread.h>
#include <wrl/client.h>

#include "erm625_desktop_fakes.hpp"
#include "native_desktop_accessor.hpp"

using namespace qs::windows;
using namespace qs::windows::native_desktop;
using namespace erm625;

namespace qs::windows {
DWORD windowsBuild() { return 26100; }

// QS_TEST-only friend. Select the native branch while the public test
// constructor keeps registry, real COM activation and window creation absent.
struct NativeDesktopFacadeTestHook {
	static void selectNative(VirtualDesktops& desktops) {
		desktops.nativeAccessor = true;
		desktops.testAvailability = DesktopAvailability::Disconnected;
	}
	static void observeSnapshots(VirtualDesktops& desktops) {
		desktops.bindNativeDesktopQueries();
	}
	static QVariantMap nativeCapability(VirtualDesktops& desktops) {
		const auto testMode = desktops.testMode;
		desktops.testMode = false;
		const auto capability = desktops.capability();
		desktops.testMode = testMode;
		return capability;
	}
	static bool pendingIsEmpty(const VirtualDesktops& desktops) {
		return !desktops.pendingSwitch && desktops.creationWaiters.isEmpty()
		    && !desktops.switchDeadline.isActive() && !desktops.creationTimer.isActive();
	}
	// Shortens the post-command observation budgets (1500 ms switch, 1000 ms
	// creation) for test runs. The budgets still gate "timed-out" outcomes the
	// same way; tests just do not pay the production grace period.
	static void setObservationBudget(VirtualDesktops& desktops, int milliseconds) {
		desktops.switchDeadline.setInterval(milliseconds);
		desktops.creationTimer.setInterval(milliseconds);
	}
	static int call(VirtualDesktops& desktops, int (*operation)()) {
		return desktops.call(-1, operation);
	}
};
} // namespace qs::windows
namespace qs::windows::startup {
void afterFirstFrame(QObject*, std::function<void()>) {}
} // namespace qs::windows::startup

namespace {
FakeView* outerFaultView = nullptr;

int localReleaseOutsideNativeGuard() {
	Microsoft::WRL::ComPtr<IUnknown> local;
	outerFaultView->AddRef();
	local.Attach(outerFaultView);
	return 0; // The injected Release SEH must reach VirtualDesktops::call.
}

void restoreEnvironment(const char* name, const QByteArray& previous) {
	if (previous.isNull()) qunsetenv(name);
	else qputenv(name, previous);
}

// QTest's QTRY_* re-polls a satisfied predicate in ~60 ms quanta. These waits
// poll at 1 ms instead; each keeps its original timeout budget and the
// QCOMPARE/verify immediately after keeps the assertion identical.
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs) {
	QElapsedTimer elapsed;
	elapsed.start();
	for (;;) {
		if (predicate()) return true;
		if (elapsed.elapsed() >= timeoutMs) return predicate();
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
		QThread::msleep(1);
	}
}

// The production recovery window is 30 s with a 250 ms first retry. Anchoring
// the injected age 40 ms before the deadline leaves every retry a real
// timer-driven attempt, just scheduled 40 ms out instead of 250+ ms.
constexpr qint64 RECOVERY_TEST_AGE = 30000 - 40;

void fastRecovery() {
	setNativeDesktopRecoveryAgeForTest([] { return RECOVERY_TEST_AGE; });
}
} // namespace

class Erm625NativeDesktopFacadeTest: public QObject {
	Q_OBJECT;
	Fixture old, fresh;
	VirtualDesktops* desktops = nullptr;
	VirtualDesktops::Accessor accessor;
	int acquisitions = 0;
	QByteArray previousHypr, previousDispatch;

	void install() {
		this->desktops->installAccessorForTest(this->accessor);
		// Match the native provider's disconnected reason when unloaded. During
		// operations testMode keeps refresh on the injected model, not registry.
		NativeDesktopFacadeTestHook::selectNative(*this->desktops);
	}

	void verifyUnavailable() {
		QVERIFY(!this->desktops->accessorLoaded());
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		// Exercise the real native capability projection too, without calling
		// refresh (which would read the live registry outside testMode).
		const auto capability = NativeDesktopFacadeTestHook::nativeCapability(*this->desktops);
		QCOMPARE(capability.value("availability").toString(), QStringLiteral("unavailable"));
		QCOMPARE(capability.value("reason").toString(), QStringLiteral("provider-disconnected"));
		QVERIFY(!capability.value("operations").toMap().value("switch").toBool());
	}

private slots:
	void init() {
		this->previousHypr = qgetenv("CAELESTIA_WINDOWS_HYPR");
		this->previousDispatch = qgetenv("CAELESTIA_WINDOWS_HYPR_DISPATCH");
		qputenv("CAELESTIA_WINDOWS_HYPR", "1");
		qputenv("CAELESTIA_WINDOWS_HYPR_DISPATCH", "1");
		this->acquisitions = 0;
		VirtualDesktops::TestHooks hooks;
		hooks.desktops = {{.id = this->old.one.id}, {.id = this->old.two.id}};
		this->desktops = VirtualDesktops::createForTest(std::move(hooks));
		NativeDesktopFacadeTestHook::selectNative(*this->desktops);
		setNativeDesktopFactoryForTest(
		    [this](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return (++this->acquisitions == 1 ? this->old : this->fresh).acquire(m, v, p);
		    }
		);
		fastRecovery();
		NativeDesktopFacadeTestHook::setObservationBudget(*this->desktops, 200);
		QVERIFY(loadNativeDesktopAccessor(this->accessor, this->desktops, [this] { this->install(); }));
		NativeDesktopFacadeTestHook::observeSnapshots(*this->desktops);
		this->install();
		QCoreApplication::processEvents();
	}
	void cleanup() {
		// Restore all process-global state and disarm every fault before any
		// assertion can return early and contaminate the following data row.
		restoreEnvironment("CAELESTIA_WINDOWS_HYPR", this->previousHypr);
		restoreEnvironment("CAELESTIA_WINDOWS_HYPR_DISPATCH", this->previousDispatch);
		outerFaultView = nullptr;
		setNativeDesktopFactoryForTest({});
		QVERIFY(waitUntil([] { return nativeDesktopWorkerCountForTest() == 0; }, 1000));
		QCOMPARE(nativeDesktopWorkerCountForTest(), 0);
		this->old.clearFaults();
		this->fresh.clearFaults();
		if (this->desktops) this->desktops->destroyForTest();
		this->desktops = nullptr;
		this->old.manager.switches = this->fresh.manager.switches = 0;
		this->old.manager.creates = this->fresh.manager.creates = 0;
		this->old.manager.moves = this->fresh.manager.moves = 0;
		this->old.manager.array.desktops = {&this->old.one, &this->old.two};
		this->fresh.manager.array.desktops = {&this->fresh.one, &this->fresh.two};
		this->old.manager.created = &this->old.three;
		this->fresh.manager.created = &this->fresh.three;
		this->old.publicApi.desktopId = this->old.one.id;
		this->fresh.publicApi.desktopId = this->fresh.one.id;
		this->old.manager.current = &this->old.one;
		this->fresh.manager.current = &this->fresh.one;
		this->old.manager.lastMovedDesktop = nullptr;
		this->fresh.manager.lastMovedDesktop = nullptr;
		QCOMPARE(this->old.manager.refs, 1UL);
		QCOMPARE(this->old.one.refs, 1UL);
		QCOMPARE(this->old.views.view.refs, 1UL);
		QCOMPARE(this->fresh.manager.refs, 1UL);
		QCOMPARE(this->fresh.one.refs, 1UL);
		QCOMPARE(qgetenv("CAELESTIA_WINDOWS_HYPR"), this->previousHypr);
		QCOMPARE(qgetenv("CAELESTIA_WINDOWS_HYPR_DISPATCH"), this->previousDispatch);
	}
	void switchFaultCannotReuseProviderAndRecovers() {
		QSignalSpy capabilities(this->desktops, &VirtualDesktops::capabilityChanged);
		QSignalSpy results(this->desktops, &VirtualDesktops::commandResult);
		this->old.manager.faultOnSwitch = true;
		QVERIFY(!this->desktops->switchTo(1));
		this->verifyUnavailable();
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("provider-disconnected")
		);
		QVERIFY(!this->desktops->switchTo(1));
		QCOMPARE(this->old.manager.switches, 1);
		QCOMPARE(this->acquisitions, 1);
		QCoreApplication::processEvents();
		QCOMPARE(capabilities.count(), 1);
		QCOMPARE(results.count(), 2);
		QVERIFY(waitUntil([this] { return this->desktops->accessorLoaded(); }, 1500));
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(this->acquisitions, 2);
		this->fresh.manager.current = &this->fresh.two;
		QVERIFY(this->desktops->switchTo(1));
		QCOMPARE(this->fresh.manager.switches, 1);
		QCOMPARE(this->old.manager.switches, 1);
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    5000
		));
		QCOMPARE(
		    this->desktops->lastCommand().value("outcome").toString(),
		    QStringLiteral("observed")
		);
	}
	void outerReleaseFaultInvalidates_data() {
		QTest::addColumn<bool>("unloaded");
		QTest::newRow("loaded-facade") << false;
		QTest::newRow("already-unloaded-facade") << true;
	}
	void outerReleaseFaultInvalidates() {
		QFETCH(bool, unloaded);
		if (unloaded) this->desktops->loseAccessorForTest();
		outerFaultView = &this->old.views.view;
		this->old.views.view.faultOnRelease = true;
		const auto result =
		    NativeDesktopFacadeTestHook::call(*this->desktops, &localReleaseOutsideNativeGuard);
		this->old.clearFaults();
		outerFaultView = nullptr;
		QCOMPARE(result, -1);
		this->verifyUnavailable();
		QCOMPARE(nativeDesktopLastError(), E_UNEXPECTED);
		QCOMPARE(this->old.views.view.refs, 1UL);
		QCOMPARE(this->old.manager.refs, 1UL);
		QVERIFY(!retryNativeDesktopAccessor());
		QCOMPARE(this->acquisitions, 1);
		QVERIFY(waitUntil([this] { return this->desktops->accessorLoaded(); }, 1500));
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(this->acquisitions, 2);
	}
	void pendingOperationsFailOnReadFault_data() {
		QTest::addColumn<bool>("creation");
		QTest::addColumn<bool>("outerRelease");
		QTest::newRow("pending-switch") << false << false;
		QTest::newRow("pending-create") << true << false;
		QTest::newRow("pending-switch-outer-release") << false << true;
		QTest::newRow("pending-create-outer-release") << true << true;
	}
	void pendingOperationsFailOnReadFault() {
		QFETCH(bool, creation);
		QFETCH(bool, outerRelease);
		QSignalSpy results(this->desktops, &VirtualDesktops::commandResult);
		int completions = 0;
		if (creation) {
			this->desktops->ensureCount(3, [&](bool ok) {
				QVERIFY(!ok);
				++completions;
			});
		} else {
			QVERIFY(this->desktops->switchTo(1));
		}
		const auto accepted = this->desktops->lastCommand();
		QCOMPARE(accepted.value("outcome").toString(), QStringLiteral("accepted"));
		QCOMPARE(
		    refreshNativeDesktopForTest(),
		    2
		); // Drain the command's refresh before arming a fault.
		if (outerRelease) {
			outerFaultView = &this->old.views.view;
			this->old.views.view.faultOnRelease = true;
			auto accessor = this->accessor;
			accessor.getDesktopCount = &localReleaseOutsideNativeGuard;
			this->desktops->installAccessorForTest(accessor);
			NativeDesktopFacadeTestHook::selectNative(*this->desktops);
		} else {
			this->old.manager.faultOnEnumeration = true;
		}
		this->desktops->refresh();
		QVERIFY(waitUntil([this] { return !this->desktops->accessorLoaded(); }, 200));
		QVERIFY(!this->desktops->accessorLoaded());
		this->old.clearFaults();
		outerFaultView = nullptr;
		this->verifyUnavailable();
		QCoreApplication::processEvents();
		QCOMPARE(results.count(), 2);
		const auto terminal = results.last().at(0).toMap();
		QCOMPARE(terminal.value("requestId"), accepted.value("requestId"));
		QCOMPARE(terminal.value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(terminal.value("reason").toString(), QStringLiteral("provider-disconnected"));
		QCOMPARE(completions, creation ? 1 : 0);
		QVERIFY(NativeDesktopFacadeTestHook::pendingIsEmpty(*this->desktops));
		QVERIFY(waitUntil([this] { return this->desktops->accessorLoaded(); }, 1500));
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(results.count(), 2); // Recovery never resurrects/replays the request.
	}
	void blockedCommandReportsTimeoutAndNeverReplays_data() {
		QTest::addColumn<QString>("operation");
		for (const auto* operation: {"switch", "create", "move"})
			QTest::newRow(operation) << QString::fromLatin1(operation);
	}
	void slowCommandRemainsAccepted_data() {
		QTest::addColumn<QString>("operation");
		QTest::addColumn<bool>("succeed");
		for (const auto* operation: {"switch", "create", "move"}) {
			QTest::newRow(qPrintable(QString::fromLatin1(operation) + "-success"))
			    << QString::fromLatin1(operation) << true;
			QTest::newRow(qPrintable(QString::fromLatin1(operation) + "-refused"))
			    << QString::fromLatin1(operation) << false;
		}
	}
	void slowCommandRemainsAccepted() {
		QFETCH(QString, operation);
		QFETCH(bool, succeed);
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		const auto hold = [blocked] { (*blocked)(); };
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		QVERIFY(waitUntil(
		    [this, window] { return this->desktops->windowDesktopIndex(window) == qsizetype(0); },
		    500
		));
		QCOMPARE(this->desktops->windowDesktopIndex(window), qsizetype(0));
		QCoreApplication::processEvents();
		QSignalSpy membership(this->desktops, &VirtualDesktops::windowDesktopsChanged);
		QSignalSpy moveCompleted(this->desktops, &VirtualDesktops::moveWindowCompleted);
		this->old.manager.publishSwitch = this->old.manager.publishCreation = true;
		if (operation == "switch") {
			this->old.manager.beforeSwitch = hold;
			this->old.manager.switchResult = succeed ? S_OK : E_ACCESSDENIED;
		}
		if (operation == "create") {
			this->old.manager.beforeCreate = hold;
			if (!succeed) this->old.manager.created = nullptr;
		}
		if (operation == "move") {
			this->old.manager.beforeMove = [this, hold, succeed] {
				hold();
				if (succeed) this->old.publicApi.desktopId = this->old.two.id;
			};
			this->old.manager.moveResult = succeed ? S_OK : E_ACCESSDENIED;
		}
		struct Completion {
			int calls = 0;
			bool ok = false;
		};
		auto completion = std::make_shared<Completion>();
		QElapsedTimer elapsed;
		elapsed.start();
		if (operation == "switch") QVERIFY(this->desktops->switchTo(1));
		if (operation == "move") QVERIFY(this->desktops->moveWindow(window, 1));
		if (operation == "create")
			this->desktops->ensureCount(3, [completion](bool ok) {
				++completion->calls;
				completion->ok = ok;
			});
		QVERIFY(elapsed.elapsed() < 250);
		const auto accepted = this->desktops->lastCommand();
		QCOMPARE(
		    accepted.value("outcome").toString(),
		    operation == "move" ? QStringLiteral("unknown") : QStringLiteral("accepted")
		);
		QVERIFY(nativeDesktopCommandPending());
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		QCOMPARE(completion->calls, 0);
		QCOMPARE(moveCompleted.size(), 0);
		if (operation == "move") QVERIFY(nativeDesktopPendingMoveSequence() != 0);
		blocked->release();
		QVERIFY(waitUntil([] { return !nativeDesktopCommandPending(); }, 1000));
		QVERIFY(!nativeDesktopCommandPending());
		QCOMPARE(this->acquisitions, 1);
		if (operation == "move") {
			QVERIFY(waitUntil([&moveCompleted] { return moveCompleted.size() == 1; }, 1000));
			QCOMPARE(moveCompleted.size(), 1);
			QVERIFY(moveCompleted.first().at(0).toULongLong() != 0);
			QCOMPARE(
			    moveCompleted.first().at(1).value<quintptr>(),
			    reinterpret_cast<quintptr>(window)
			);
			QCOMPARE(moveCompleted.first().at(2).toBool(), succeed);
			QCOMPARE(
			    moveCompleted.first().at(3).toString(),
			    succeed ? QString() : QStringLiteral("permission-denied")
			);
			QVERIFY(waitUntil(
			    [this, window, succeed] {
				    return this->desktops->windowDesktopIndex(window)
				        == (succeed ? qsizetype(1) : qsizetype(0));
			    },
			    1000
			));
			QCOMPARE(
			    this->desktops->windowDesktopIndex(window),
			    succeed ? qsizetype(1) : qsizetype(0)
			);
			QCOMPARE(this->desktops->lastCommand(), accepted); // No promised move observation.
			QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
			QVERIFY(!nativeDesktopRequestPending());
			QCoreApplication::processEvents();
			if (!succeed) QCOMPARE(membership.count(), 0); // No unchanged signal that reissues a panel move.
		} else {
			QVERIFY(waitUntil(
			    [this, succeed] {
				    return this->desktops->lastCommand().value("outcome").toString()
				        == (succeed ? QStringLiteral("observed") : QStringLiteral("timed-out"));
			    },
			    2500
			));
			QCOMPARE(
			    this->desktops->lastCommand().value("outcome").toString(),
			    succeed ? QStringLiteral("observed") : QStringLiteral("timed-out")
			);
			QCOMPARE(this->desktops->lastCommand().value("requestId"), accepted.value("requestId"));
			if (!succeed)
				QCOMPARE(
				    this->desktops->lastCommand().value("reason").toString(),
				    QStringLiteral("observation-timeout")
				);
		}
		if (operation == "create") {
			QCOMPARE(completion->calls, 1);
			QCOMPARE(completion->ok, succeed);
			QCOMPARE(this->old.manager.creates, 1);
		}
		if (operation == "switch") QCOMPARE(this->old.manager.switches, 1);
		if (operation == "move") QCOMPARE(this->old.manager.moves, 1);
	}
	void busyCommandsDoNotSupersedeAnAcceptedSwitch() {
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
		this->old.manager.publishSwitch = true;
		QVERIFY(this->desktops->switchTo(1));
		const auto accepted = this->desktops->lastCommand();
		QSignalSpy results(this->desktops, &VirtualDesktops::commandResult);
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!this->desktops->switchTo(0));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("provider-busy")
		);
		bool creationRefused = false;
		this->desktops->ensureCount(3, [&](bool ok) { creationRefused = !ok; });
		QVERIFY(creationRefused);
		QVERIFY(!this->desktops->moveWindow(reinterpret_cast<HWND>(ULONG_PTR(0xb2)), 1));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("provider-busy")
		);
		QVERIFY(elapsed.elapsed() < 50);
		QVERIFY(nativeDesktopCommandPending());
		blocked->release();
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));
		QCOMPARE(
		    this->desktops->lastCommand().value("outcome").toString(),
		    QStringLiteral("observed")
		);
		QCOMPARE(this->desktops->lastCommand().value("requestId"), accepted.value("requestId"));
		QCOMPARE(this->old.manager.switches, 1);
		QCOMPARE(this->old.manager.creates, 0);
		QCOMPARE(this->old.manager.moves, 0);
		QCOMPARE(results.count(), 4); // Three busy refusals and the original switch observation.
	}
	void aCommandCanWaitBehindOneSlowRefresh() {
		auto first = std::make_shared<std::atomic<bool>>(true);
		this->old.manager.beforeEnumeration = [first] {
			if (first->exchange(false)) std::this_thread::sleep_for(std::chrono::milliseconds(150));
		};
		this->old.manager.publishSwitch = true;
		requestNativeDesktopRefresh();
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(this->desktops->switchTo(1));
		QVERIFY(elapsed.elapsed() < 250);
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("accepted"));
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));
		QCOMPARE(
		    this->desktops->lastCommand().value("outcome").toString(),
		    QStringLiteral("observed")
		);
		QCOMPARE(this->old.manager.switches, 1);
		QCOMPARE(this->acquisitions, 1);
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
	}
	void creationObservationBudgetStartsAfterTheInFlightCommand() {
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeCreate = [blocked] { (*blocked)(); };
		this->old.manager.publishCreation = true;
		auto completed = std::make_shared<int>(0);
		this->desktops->ensureCount(3, [completed](bool ok) { *completed = ok ? 1 : -1; });
		// Longer than the creation observation budget (200 ms in this fixture,
		// 1 s in production), shorter than the stall verdict.
		QTest::qWait(450);
		QCOMPARE(*completed, 0);
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("accepted"));
		QVERIFY(nativeDesktopCommandPending());
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		blocked->release();
		QVERIFY(waitUntil([completed] { return *completed == 1; }, 1000));
		QCOMPARE(*completed, 1);
		QCOMPARE(this->old.manager.creates, 1);
		QCOMPARE(this->acquisitions, 1);
	}
	void onlyTheStuckBoundRetiresAnInFlightCommand() {
		setNativeDesktopStuckBoundForTest(200);
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
		QVERIFY(this->desktops->switchTo(1));
		const auto accepted = this->desktops->lastCommand();
		QCOMPARE(accepted.value("outcome").toString(), QStringLiteral("accepted"));
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		QVERIFY(waitUntil([this] { return !this->desktops->accessorLoaded(); }, 300));
		QVERIFY(!this->desktops->accessorLoaded());
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("provider-timeout")
		);
		QCOMPARE(this->desktops->lastCommand().value("requestId"), accepted.value("requestId"));
		QVERIFY(waitUntil([this] { return this->desktops->accessorLoaded(); }, 1500));
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(this->acquisitions, 2);
		QCOMPARE(this->fresh.manager.switches, 0);
		blocked->release();
		QVERIFY(waitUntil([] { return nativeDesktopWorkerCountForTest() == 1; }, 500));
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
		QCOMPARE(this->fresh.manager.switches, 0);
	}
	void observationWaitsForASlowPostCommandSnapshot() {
		this->old.manager.publishSwitch = true;
		this->old.manager.beforeSwitch = [] {
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
		};
		this->old.manager.beforeEnumeration = [] {
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
		};
		QVERIFY(this->desktops->switchTo(1));
		// The command is done, but its confirming snapshot is still in flight.
		QTest::qWait(550);
		QVERIFY(!nativeDesktopCommandPending());
		QVERIFY(nativeDesktopRequestPending());
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("accepted"));
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));
		QCOMPARE(
		    this->desktops->lastCommand().value("outcome").toString(),
		    QStringLiteral("observed")
		);
		QCOMPARE(this->acquisitions, 1);
		QCOMPARE(this->old.manager.switches, 1);
	}
	void blockedCommandReportsTimeoutAndNeverReplays() {
		QFETCH(QString, operation);
		setNativeDesktopStuckBoundForTest(50);
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		const auto block = [blocked] { (*blocked)(); };
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		if (operation == "switch") this->old.manager.beforeSwitch = block;
		if (operation == "create") this->old.manager.beforeCreate = block;
		if (operation == "move") this->old.manager.beforeMove = block;
		QSignalSpy results(this->desktops, &VirtualDesktops::commandResult);
		int completions = 0;
		QElapsedTimer elapsed;
		elapsed.start();
		if (operation == "switch") QVERIFY(!this->desktops->switchTo(1));
		if (operation == "move") QVERIFY(!this->desktops->moveWindow(window, 1));
		if (operation == "create")
			this->desktops->ensureCount(3, [&](bool ok) {
				QVERIFY(!ok);
				++completions;
			});
		QVERIFY(elapsed.elapsed() < 250);
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("provider-timeout")
		);
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(completions, operation == "create" ? 1 : 0);
		QCOMPARE(this->desktops->count(), qsizetype(2));
		QVERIFY(NativeDesktopFacadeTestHook::pendingIsEmpty(*this->desktops));
		QVERIFY(waitUntil([this] { return this->desktops->accessorLoaded(); }, 1500));
		QVERIFY(this->desktops->accessorLoaded());
		QCOMPARE(results.count(), 1);
		QCOMPARE(this->fresh.manager.switches, 0);
		QCOMPARE(this->fresh.manager.creates, 0);
		QCOMPARE(this->fresh.manager.moves, 0);
		blocked->release();
		QVERIFY(waitUntil([] { return nativeDesktopWorkerCountForTest() == 1; }, 500));
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
		QCOMPARE(results.count(), 1);
	}

	void lateUnmovableMoveDoesNotReusePriorPermissionError() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb3));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.moveResult = E_ACCESSDENIED;
		QVERIFY(!this->desktops->moveWindow(window, 1));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("permission-denied")
		);

		QSignalSpy moveCompleted(this->desktops, &VirtualDesktops::moveWindowCompleted);
		this->old.manager.moveResult = S_OK;
		this->old.manager.canMove = false;
		{
			auto blocked = std::make_shared<BlockingCall>();
			Unblock unblock {blocked};
			this->old.manager.beforeCanMove = [blocked] { (*blocked)(); };
			QVERIFY(this->desktops->moveWindow(window, 1));
			QVERIFY(nativeDesktopCommandPending());
			const auto sequence = nativeDesktopPendingMoveSequence();
			QVERIFY(sequence != 0);
			blocked->release();
			QVERIFY(waitUntil([&moveCompleted] { return moveCompleted.size() == 1; }, 1000));
			QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
			QCOMPARE(moveCompleted.size(), 1);
			QCOMPARE(moveCompleted.first().at(0).toULongLong(), sequence);
			QCOMPARE(
			    moveCompleted.first().at(1).value<quintptr>(),
			    reinterpret_cast<quintptr>(window)
			);
			QCOMPARE(moveCompleted.first().at(2).toBool(), false);
			QCOMPARE(moveCompleted.first().at(3).toString(), QStringLiteral("command-rejected"));
		}

		// A later provider disconnect before deferred delivery must not reclassify
		// an already-completed unmovable rejection as provider-disconnected.
		moveCompleted.clear();
		this->old.manager.canMove = false;
		{
			auto blocked = std::make_shared<BlockingCall>();
			Unblock unblock {blocked};
			this->old.manager.beforeCanMove = [blocked] { (*blocked)(); };
			QVERIFY(this->desktops->moveWindow(window, 1));
			const auto seqA = nativeDesktopPendingMoveSequence();
			QVERIFY(seqA != 0);
			blocked->release();
			QElapsedTimer waitWorker;
			waitWorker.start();
			while (!nativeDesktopPendingReadyForTest() && waitWorker.elapsed() < 1000) QThread::msleep(5);
			QVERIFY(nativeDesktopPendingReadyForTest());
			// Second move collects A synchronously (queuing A's notification) and then
			// disconnects the provider before Qt delivers A's queued callback.
			this->old.manager.canMove = true;
			this->old.manager.beforeCanMove = {};
			this->old.manager.moveResult = RPC_E_DISCONNECTED;
			QVERIFY(!this->desktops->moveWindow(window, 1));
			QCOMPARE(
			    this->desktops->lastCommand().value("reason").toString(),
			    QStringLiteral("provider-disconnected")
			);
			QVERIFY(waitUntil([&moveCompleted] { return moveCompleted.size() == 1; }, 1000));
			QCOMPARE(moveCompleted.size(), 1);
			QCOMPARE(moveCompleted.first().at(0).toULongLong(), seqA);
			QCOMPARE(moveCompleted.first().at(2).toBool(), false);
			QCOMPARE(moveCompleted.first().at(3).toString(), QStringLiteral("command-rejected"));
		}

		this->old.manager.canMove = true;
		this->old.manager.beforeCanMove = {};
	}

	void invalidationAfterCompletedMoveReportsActualOutcome() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb4));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		QSignalSpy moveCompleted(this->desktops, &VirtualDesktops::moveWindowCompleted);
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.moveResult = S_OK;
		this->old.manager.beforeMove = [blocked] { (*blocked)(); };
		QVERIFY(this->desktops->moveWindow(window, 1));
		const auto seq = nativeDesktopPendingMoveSequence();
		QVERIFY(seq != 0);
		blocked->release();
		QElapsedTimer waitWorker;
		waitWorker.start();
		while (!nativeDesktopPendingReadyForTest() && waitWorker.elapsed() < 1000) QThread::msleep(5);
		QVERIFY(nativeDesktopPendingReadyForTest());

		invalidateNativeDesktopAccessor(RPC_E_DISCONNECTED);
		QVERIFY(waitUntil([&moveCompleted] { return moveCompleted.size() == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(moveCompleted.size(), 1);
		QCOMPARE(moveCompleted.first().at(0).toULongLong(), seq);
		QCOMPARE(moveCompleted.first().at(2).toBool(), true);
		QCOMPARE(moveCompleted.first().at(3).toString(), QString());
		this->old.manager.beforeMove = {};
	}

	void reentrantMoveDuringCommandResultReportsEachSequenceSeparately() {
		const auto windowA = reinterpret_cast<HWND>(ULONG_PTR(0xb5));
		const auto windowB = reinterpret_cast<HWND>(ULONG_PTR(0xb6));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.moveResult = S_OK;
		this->old.manager.beforeMove = {};

		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		quint64 sequenceB = 0;
		bool startedB = false;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [this, windowB, blocked, &sequenceB, &startedB](const QVariantMap&) {
			    if (startedB) return;
			    startedB = true;
			    this->old.manager.beforeMove = [blocked] { (*blocked)(); };
			    QVERIFY(this->desktops->moveWindow(windowB, 1, &sequenceB));
		    }
		);

		quint64 sequenceA = 999;
		QVERIFY(this->desktops->moveWindow(windowA, 1, &sequenceA));
		QObject::disconnect(conn);
		QVERIFY(startedB);
		QCOMPARE(sequenceA, quint64(0));
		QVERIFY(sequenceB != 0);
		QCOMPARE(nativeDesktopPendingMoveSequence(windowB), sequenceB);
		QCOMPARE(nativeDesktopPendingMoveSequence(windowA), quint64(0));

		blocked->release();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeMove = {};
	}

	void moveAndCreationReturnOwnFailureReasonDespiteReentrantCommand() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb7));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.moveResult = E_ACCESSDENIED;
		this->old.manager.beforeMove = {};

		bool switched = false;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [this, &switched](const QVariantMap& result) {
			    if (switched || result.value("operation").toString() == QStringLiteral("switch")) return;
			    switched = true;
			    QVERIFY(this->desktops->switchTo(0));
		    }
		);

		quint64 pendingSeq = 999;
		QString moveReason;
		QVERIFY(!this->desktops->moveWindow(window, 1, &pendingSeq, &moveReason));
		QVERIFY(switched);
		QCOMPARE(pendingSeq, quint64(0));
		QCOMPARE(moveReason, QStringLiteral("permission-denied"));
		QCOMPARE(this->desktops->lastCommand().value("operation").toString(), QStringLiteral("switch"));

		switched = false;
		bool creationCalled = false;
		bool creationOk = true;
		QString creationReason;
		this->desktops->ensureCountWithReason(
		    99,
		    [&](bool ok, const QString& reason) {
			    creationCalled = true;
			    creationOk = ok;
			    creationReason = reason;
		    }
		);
		QObject::disconnect(conn);
		QVERIFY(switched);
		QVERIFY(creationCalled);
		QVERIFY(!creationOk);
		QCOMPARE(creationReason, QStringLiteral("workspace-limit"));
		QCOMPARE(this->desktops->lastCommand().value("operation").toString(), QStringLiteral("switch"));
	}

	void pendingMoveCallbackRegistersBeforeCommandResultNestedLoop() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb8));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.moveResult = E_ACCESSDENIED;
		this->old.manager.beforeMove = [blocked] { (*blocked)(); };

		quint64 registeredSeq = 0;
		bool sawRegisteredAtCompletion = false;
		bool sawCompletionInsideCommandResult = false;
		auto completionConn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::moveWindowCompleted,
		    this->desktops,
		    [&](quint64 sequence, quintptr, bool succeeded, const QString& reason) {
			    if (sequence == registeredSeq && registeredSeq != 0) {
				    sawRegisteredAtCompletion = true;
				    QVERIFY(!succeeded);
				    QCOMPARE(reason, QStringLiteral("permission-denied"));
			    }
		    }
		);
		auto resultConn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [&, blocked](const QVariantMap& result) {
			    if (result.value("operation").toString() != QStringLiteral("move")) return;
			    QVERIFY(registeredSeq != 0);
			    blocked->release();
			    sawCompletionInsideCommandResult = waitUntil(
			        [&] { return sawRegisteredAtCompletion; }, 1000
			    );
		    }
		);

		quint64 pendingSeq = 0;
		QVERIFY(this->desktops->moveWindow(
		    window,
		    1,
		    &pendingSeq,
		    nullptr,
		    [&registeredSeq](quint64 seq) { registeredSeq = seq; }
		));
		QObject::disconnect(resultConn);
		QObject::disconnect(completionConn);
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeMove = {};

		QVERIFY(pendingSeq != 0);
		QCOMPARE(registeredSeq, pendingSeq);
		QVERIFY(sawCompletionInsideCommandResult);
		QVERIFY(sawRegisteredAtCompletion);
	}

	void switchToRevalidatesAfterSupersededPublicationBeforeNativeSwitch() {
		{
			auto blocked = std::make_shared<BlockingCall>();
			Unblock unblock {blocked};
			this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
			QVERIFY(this->desktops->switchTo(1));
			blocked->release();
			QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		}

		int nativeSwitches = 0;
		this->old.manager.beforeSwitch = [&nativeSwitches] { ++nativeSwitches; };
		bool stillValid = true;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [&stillValid](const QVariantMap& result) {
			    if (result.value("outcome").toString() == QStringLiteral("superseded")) {
				    stillValid = false;
			    }
		    }
		);

		QVERIFY(!this->desktops->switchTo(1, [&stillValid] { return stillValid; }));
		QObject::disconnect(conn);
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeSwitch = {};
		QVERIFY(!stillValid);
		QCOMPARE(nativeSwitches, 0);
	}

	void reentrantSwitchDuringSupersededDoesNotOverrideNewerSwitch() {
		this->old.manager.publishSwitch = false;
		{
			auto blocked = std::make_shared<BlockingCall>();
			Unblock unblock {blocked};
			this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
			QVERIFY(this->desktops->switchTo(1));
			blocked->release();
			QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		}

		int nativeSwitches = 0;
		this->old.manager.beforeSwitch = [&nativeSwitches] { ++nativeSwitches; };
		bool innerRan = false;
		QVariantMap innerAccepted;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [this, &innerRan, &innerAccepted](const QVariantMap& result) {
			    if (!innerRan && result.value("outcome").toString() == QStringLiteral("superseded")) {
				    innerRan = true;
				    QVERIFY(this->desktops->switchTo(1));
				    innerAccepted = this->desktops->lastCommand();
			    }
		    }
		);

		QVERIFY(!this->desktops->switchTo(1));
		QObject::disconnect(conn);
		QVERIFY(innerRan);
		QCOMPARE(innerAccepted.value("outcome").toString(), QStringLiteral("accepted"));
		QCOMPARE(this->desktops->lastCommand(), innerAccepted);

		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("timed-out");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeSwitch = {};
		QCOMPARE(nativeSwitches, 1);
		QCOMPARE(this->desktops->lastCommand().value("requestId"), innerAccepted.value("requestId"));
	}

	void moveWindowShouldProceedAbortsWithoutNativeMoveOrRefusal() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb9));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		QSignalSpy results(this->desktops, &VirtualDesktops::commandResult);
		quint64 pendingSeq = 99;
		QString failureReason = QStringLiteral("stale");

		QVERIFY(!this->desktops->moveWindow(
		    window, 1, &pendingSeq, &failureReason, {}, [] { return false; }
		));
		QCOMPARE(pendingSeq, quint64(0));
		QCOMPARE(failureReason, QString());
		QCOMPARE(this->old.manager.moves, 0);
		QCOMPARE(results.size(), 0);
	}

	void switchToOnObservedRunsOnlyAfterObservationAndNotOnTimeout() {
		this->old.manager.publishSwitch = true;
		{
			auto blocked = std::make_shared<BlockingCall>();
			Unblock unblock {blocked};
			this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
			int observedCalls = 0;
			qsizetype observedIndex = -1;
			QVERIFY(this->desktops->switchTo(
			    1,
			    {},
			    [&observedCalls, &observedIndex](qsizetype idx) {
				    ++observedCalls;
				    observedIndex = idx;
			    }
			));
			// Still pending: onObserved must not run before observation.
			QCOMPARE(observedCalls, 0);
			blocked->release();
			QVERIFY(waitUntil([&observedCalls] { return observedCalls == 1; }, 1000));
			QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
			QCOMPARE(observedCalls, 1);
			QCOMPARE(observedIndex, qsizetype(1));
		}

		// A timed-out switch must never invoke onObserved.
		this->old.manager.publishSwitch = false;
		this->old.manager.beforeSwitch = {};
		int timedOutObserved = 0;
		QVERIFY(this->desktops->switchTo(0, {}, [&timedOutObserved](qsizetype) { ++timedOutObserved; }));
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("timed-out");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(timedOutObserved, 0);
	}

	void switchToExpectedIntentAbortsWhenNewerWorkspaceIntentSupersedes() {
		this->old.manager.publishSwitch = true;
		this->old.manager.beforeSwitch = {};
		const auto olderIntent = this->desktops->beginWorkspaceIntent();
		QVERIFY(this->desktops->switchTo(1));
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));

		int nativeSwitches = 0;
		int observedCalls = 0;
		this->old.manager.beforeSwitch = [&nativeSwitches] { ++nativeSwitches; };
		QVERIFY(!this->desktops->switchTo(
		    0, {}, [&observedCalls](qsizetype) { ++observedCalls; }, olderIntent
		));
		this->old.manager.beforeSwitch = {};
		QCOMPARE(nativeSwitches, 0);
		QCOMPARE(observedCalls, 0);
	}

	void reentrantRefusedSwitchDuringAcceptedStillRefreshesAndObserves() {
		this->old.manager.publishSwitch = true;
		this->old.manager.beforeSwitch = {};
		bool refusedInner = false;
		QVariantMap outerAccepted;
		int observedCalls = 0;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [this, &refusedInner, &outerAccepted](const QVariantMap& result) {
			    if (!refusedInner && result.value("outcome").toString() == QStringLiteral("accepted")) {
				    refusedInner = true;
				    outerAccepted = result;
				    QVERIFY(!this->desktops->switchTo(-1));
			    }
		    }
		);

		QVERIFY(this->desktops->switchTo(1, {}, [&observedCalls](qsizetype) { ++observedCalls; }));
		QObject::disconnect(conn);
		QVERIFY(refusedInner);
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));
		QCOMPARE(this->desktops->lastCommand().value("requestId"), outerAccepted.value("requestId"));
		QCOMPARE(observedCalls, 1);
	}

	void moveWindowReportsResolvedDestinationGuid() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xba));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.moveResult = S_OK;
		this->old.manager.beforeMove = {};
		const auto expectedGuid = this->desktops->desktops().at(1).id;
		GUID targetGuid {};

		QVERIFY(this->desktops->moveWindow(window, 1, nullptr, nullptr, {}, {}, &targetGuid));
		QVERIFY(IsEqualGUID(targetGuid, expectedGuid) != FALSE);
	}

	void switchToTargetGuidIgnoresOutOfSyncNativeDesktopOrder() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->count(), qsizetype(3));
		QVERIFY(IsEqualGUID(this->desktops->desktops().at(1).id, this->old.two.id) != FALSE);

		// Reorder the provider and refresh the native cache without letting the
		// facade process the snapshot yet: facade has [A, B, C], native cache has [A, C, B].
		this->old.manager.array.desktops = {&this->old.one, &this->old.three, &this->old.two};
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		QVERIFY(IsEqualGUID(this->desktops->desktops().at(1).id, this->old.two.id) != FALSE);
		QVERIFY(IsEqualGUID(this->accessor.getDesktopIdByNumber(1), this->old.three.id) != FALSE);

		this->old.manager.publishSwitch = true;
		int observedCalls = 0;
		qsizetype observedIndex = -1;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls, &observedIndex](qsizetype idx) {
			    ++observedCalls;
			    observedIndex = idx;
		    },
		    std::nullopt,
		    this->old.two.id
		));
		// Must switch to B (this->old.two), never C (this->old.three).
		QVERIFY(waitUntil([&observedCalls] { return observedCalls == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.current, &this->old.two);
		QCOMPARE(observedCalls, 1);
		QCOMPARE(observedIndex, qsizetype(2));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(2));
		QVERIFY(
		    IsEqualGUID(this->desktops->desktops().at(observedIndex).id, this->old.two.id) != FALSE
		);
	}

	void moveWindowUsesTargetGuidWhenNativeCacheReordered() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xbb));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.moveResult = S_OK;
		this->old.manager.beforeMove = {};
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QVERIFY(IsEqualGUID(this->desktops->desktops().at(1).id, this->old.two.id) != FALSE);

		// Reorder the provider and refresh native cache without updating the facade:
		// facade has [A, B, C], native cache has [A, C, B].
		this->old.manager.array.desktops = {&this->old.one, &this->old.three, &this->old.two};
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		QVERIFY(IsEqualGUID(this->accessor.getDesktopIdByNumber(1), this->old.three.id) != FALSE);

		GUID targetGuid {};
		QVERIFY(this->desktops->moveWindow(
		    window, 1, nullptr, nullptr, {}, {}, &targetGuid, this->old.two.id
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.lastMovedDesktop, &this->old.two);
		QVERIFY(IsEqualGUID(targetGuid, this->old.two.id) != FALSE);
	}

	void switchToDoesNotSkipSwitchWhenNativeCacheDisagreesWithStaleFacade() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.two;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));

		// Move native current to C without refreshing the facade: facade still says B, native cache says C.
		this->old.manager.current = &this->old.three;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
		QCOMPARE(this->accessor.getCurrentDesktopNumber(), 2);

		this->old.manager.publishSwitch = true;
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
		int observedCalls = 0;
		qsizetype observedIndex = -1;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls, &observedIndex](qsizetype idx) {
			    ++observedCalls;
			    observedIndex = idx;
		    },
		    std::nullopt,
		    this->old.two.id
		));
		// Must not treat stale facade state as alreadyCurrent or run onObserved before observation.
		QCOMPARE(observedCalls, 0);
		blocked->release();
		QVERIFY(waitUntil([&observedCalls] { return observedCalls == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeSwitch = {};
		QCOMPARE(this->old.manager.switches, 1);
		QCOMPARE(this->old.manager.current, &this->old.two);
		QCOMPARE(observedIndex, qsizetype(1));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
	}

	void pendingSwitchDoesNotObservePreCommandNativeSnapshot() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.three;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(2));

		// Populate native cache with B without updating facade (facade says C, native cache says B),
		// then move provider back to C and fail the pending switch to B.
		this->old.manager.current = &this->old.two;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(2));
		QCOMPARE(this->accessor.getCurrentDesktopNumber(), 1);

		this->old.manager.current = &this->old.three;
		this->old.manager.publishSwitch = false;
		this->old.manager.switchResult = E_ACCESSDENIED;
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
		int observedCalls = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    std::nullopt,
		    this->old.two.id
		));
		// Synchronous refresh during switchTo copies stale pre-command B snapshot;
		// observation must wait for a post-command refresh and never fire on the refused switch.
		QCOMPARE(observedCalls, 0);
		blocked->release();
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("timed-out");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeSwitch = {};
		this->old.manager.switchResult = S_OK;
		QCOMPARE(observedCalls, 0);
	}

	void reentrantMoveDuringAcceptedDoesNotObservePreCommandSwitchSnapshot() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xbc));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.three;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));

		// Populate native cache with B without refreshing facade (facade says C, native cache says B),
		// then move provider to C with a failing synchronous switch so Accepted runs a reentrant move.
		this->old.manager.current = &this->old.two;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->old.manager.current = &this->old.three;
		this->old.manager.publishSwitch = false;
		this->old.manager.switchResult = S_OK;
		this->old.manager.beforeSwitch = {};
		this->old.manager.moveResult = S_OK;
		this->old.manager.beforeMove = {};

		bool movedInsideAccepted = false;
		int observedCalls = 0;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [this, window, &movedInsideAccepted](const QVariantMap& result) {
			    if (!movedInsideAccepted && result.value("operation").toString() == QStringLiteral("switch")
			        && result.value("outcome").toString() == QStringLiteral("accepted"))
			    {
				    movedInsideAccepted = true;
				    QVERIFY(this->desktops->moveWindow(window, 1));
			    }
		    }
		);

		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    std::nullopt,
		    this->old.two.id
		));
		QObject::disconnect(conn);
		QVERIFY(movedInsideAccepted);
		// Reentrant move advanced the native command sequence without reading desktops,
		// so observation must still wait for a real desktop probe and time out on C.
		QCOMPARE(observedCalls, 0);
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("timed-out");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(observedCalls, 0);
	}

	void busySwitchAdvancesWorkspaceIntentWithoutCancellingPendingMove() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xbd));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.moveResult = S_OK;
		this->old.manager.beforeMove = [blocked] { (*blocked)(); };

		const auto moveIntent = this->desktops->beginWorkspaceIntent();
		quint64 pendingSeq = 0;
		QVERIFY(this->desktops->moveWindow(window, 1, &pendingSeq));
		QVERIFY(pendingSeq != 0);
		QVERIFY(nativeDesktopCommandPending());

		QVERIFY(!this->desktops->switchTo(1));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("provider-busy")
		);
		QVERIFY(nativeDesktopCommandPending());
		QVERIFY(this->desktops->workspaceIntentOrder() != moveIntent);

		blocked->release();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeMove = {};
		QCOMPARE(this->old.manager.moves, 1);
	}

	void switchBackBeforePostSwitchReadStillIssuesNativeSwitch() {
		this->old.manager.publishSwitch = true;
		this->old.manager.beforeSwitch = {};
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		auto blockedRead = std::make_shared<BlockingCall>();
		Unblock unblockRead {blockedRead};
		this->old.manager.beforeEnumeration = [blockedRead] { (*blockedRead)(); };

		QVERIFY(this->desktops->switchTo(1));
		// Deterministically wait for the first switch command to finish while its
		// post-switch desktop read is held so both caches still report desktop 0.
		QVERIFY(waitUntil([] { return !nativeDesktopCommandPending(); }, 1000));
		QCOMPARE(this->old.manager.current, &this->old.two);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		int observedBack = 0;
		QVERIFY(this->desktops->switchTo(
		    0,
		    {},
		    [&observedBack](qsizetype) { ++observedBack; },
		    std::nullopt,
		    this->old.one.id
		));
		// Must not take the alreadyCurrent shortcut while the first switch's read is pending.
		QCOMPARE(observedBack, 0);

		blockedRead->release();
		QVERIFY(waitUntil([&observedBack] { return observedBack == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeEnumeration = {};
		QCOMPARE(this->old.manager.switches, 2);
		QCOMPARE(this->old.manager.current, &this->old.one);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));
	}

	void switchBackAfterStalePostSwitchReadStillIssuesNativeSwitch() {
		// Windows accepts A -> B, but the first post-command desktop read still reports A.
		this->old.manager.publishSwitch = false;
		this->old.manager.beforeSwitch = {};
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		QVERIFY(this->desktops->switchTo(1));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		// Simulate the earlier switch landing on B right before a request to return to A.
		this->old.manager.current = &this->old.two;
		this->old.manager.publishSwitch = true;

		int observedBack = 0;
		QVERIFY(this->desktops->switchTo(
		    0,
		    {},
		    [&observedBack](qsizetype) { ++observedBack; },
		    std::nullopt,
		    this->old.one.id
		));
		QVERIFY(waitUntil([&observedBack] { return observedBack == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.switches, 2);
		QCOMPARE(this->old.manager.current, &this->old.one);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));
	}

	void providerDisconnectDuringObservedSkipsFollowCallback() {
		this->old.manager.publishSwitch = true;
		this->old.manager.beforeSwitch = {};
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		bool invalidated = false;
		int observedCalls = 0;
		auto conn = QObject::connect(
		    this->desktops,
		    &VirtualDesktops::commandResult,
		    this->desktops,
		    [&invalidated](const QVariantMap& result) {
			    if (!invalidated && result.value("operation").toString() == QStringLiteral("switch")
			        && result.value("outcome").toString() == QStringLiteral("observed"))
			    {
				    invalidated = true;
				    invalidateNativeDesktopAccessor();
			    }
		    }
		);

		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    std::nullopt,
		    this->old.two.id
		));
		QVERIFY(waitUntil([&invalidated] { return invalidated; }, 1000));
		QObject::disconnect(conn);
		QCOMPARE(observedCalls, 0);
	}
};
QTEST_GUILESS_MAIN(Erm625NativeDesktopFacadeTest)
#include "erm625_native_desktop_facade_test.moc"
