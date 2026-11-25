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
#include "panel_pin_state.hpp"

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
	// Outlives every worker: cleanup shuts the provider down before reset.
	FakePinnedApps pinnedApps;
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
		this->pinnedApps.expectedView = reinterpret_cast<ApplicationView*>(&this->old.views.view);
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
		// The shut-down provider released the pinned-apps fake it held.
		QCOMPARE(this->pinnedApps.refs, 1UL);
		this->pinnedApps.pinned = FALSE;
		this->pinnedApps.queries = this->pinnedApps.pins = 0;
		this->pinnedApps.queryResult = S_OK;
		this->pinnedApps.pinResult = S_OK;
		this->pinnedApps.faultOnQuery = this->pinnedApps.faultOnPin = false;
		this->pinnedApps.beforeQuery = this->pinnedApps.beforePin = {};
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
	void pinsAnOwnWindowOnceAndQueriesFirst() {
		const auto foreign = reinterpret_cast<HWND>(ULONG_PTR(0xc1));
		HWND own = CreateWindowExW(
		    0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr
		);
		QVERIFY(own != nullptr);
		struct WindowGuard {
			HWND hwnd;
			~WindowGuard() {
				if (this->hwnd) DestroyWindow(this->hwnd);
			}
		} ownGuard {own};
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		auto& pinned = this->pinnedApps;
		setNativeDesktopPinnedAppsForTest(&pinned);
		QSignalSpy pinDone(this->desktops, &VirtualDesktops::ownWindowPinCompleted);
		const auto readSeqBefore = nativeDesktopReadSequence();

		// Facade rejects foreign/synthetic handles before touching the provider.
		quint64 foreignReqId = 99;
		QCOMPARE(this->desktops->pinOwnWindow(foreign, &foreignReqId), OwnWindowPin::Unavailable);
		QCOMPARE(foreignReqId, quint64(0));

		quint64 req1 = 0;
		QCOMPARE(this->desktops->pinOwnWindow(own, &req1), OwnWindowPin::Pending);
		QVERIFY(req1 != 0);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 1; }, 1000));
		QCOMPARE(pinDone.at(0).at(0).toULongLong(), req1);
		QCOMPARE(pinDone.at(0).at(1).toULongLong(), quintptr(own));
		QCOMPARE(pinDone.at(0).at(2).value<OwnWindowPin>(), OwnWindowPin::Pinned);
		QCOMPARE(pinned.queries, 1);
		QCOMPARE(pinned.pins, 1);

		// Already pinned: the provider reads the state and changes nothing.
		quint64 req2 = 0;
		QCOMPARE(this->desktops->pinOwnWindow(own, &req2), OwnWindowPin::Pending);
		QVERIFY(req2 != 0);
		QVERIFY(req2 != req1);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 2; }, 1000));
		QCOMPARE(pinDone.at(1).at(0).toULongLong(), req2);
		QCOMPARE(pinDone.at(1).at(1).toULongLong(), quintptr(own));
		QCOMPARE(pinDone.at(1).at(2).value<OwnWindowPin>(), OwnWindowPin::Pinned);
		QCOMPARE(pinned.queries, 2);
		QCOMPARE(pinned.pins, 1);

		// Async pin completions never overwrite snapshots or advance read sequence.
		QCOMPARE(nativeDesktopReadSequence(), readSeqBefore);
		// Every view the provider fetched was released again.
		QCOMPARE(this->old.views.view.refs, 1UL);
		// Pinning own panels never advertises Hyprland pin support.
		QVERIFY(!this->desktops->pinSupported());
	}
	void pinWithoutTheServiceIsUnavailable() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xc2));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		const auto gets = this->old.views.gets;
		quint64 reqId = 77;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqId), OwnWindowPin::Unavailable);
		QCOMPARE(reqId, quint64(0));
		QCOMPARE(this->old.views.gets, gets);
		QCOMPARE(this->old.manager.moves, 0);
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
	}
	void refusedPinIsReportedAndKeepsTheProvider() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xc3));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		auto& pinned = this->pinnedApps;
		pinned.pinResult = E_ACCESSDENIED;
		setNativeDesktopPinnedAppsForTest(&pinned);
		QSignalSpy pinDone(this->desktops, &VirtualDesktops::ownWindowPinCompleted);

		quint64 reqId = 0;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqId), OwnWindowPin::Pending);
		QVERIFY(reqId != 0);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 1; }, 1000));
		QCOMPARE(pinDone.at(0).at(0).toULongLong(), reqId);
		QCOMPARE(pinDone.at(0).at(1).toULongLong(), quintptr(window));
		QCOMPARE(pinDone.at(0).at(2).value<OwnWindowPin>(), OwnWindowPin::Refused);
		QCOMPARE(pinned.pins, 1);
		QCOMPARE(this->old.views.view.refs, 1UL);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QVERIFY(this->desktops->switchTo(1));
		QCOMPARE(this->old.manager.switches, 1);
	}
	void slowPinNeverBlocksSwitchMoveOrCreateAndDeliversLateCompletion() {
		const auto panelA = reinterpret_cast<HWND>(ULONG_PTR(0xc4));
		const auto panelB = reinterpret_cast<HWND>(ULONG_PTR(0xc5));
		const auto appWindow = reinterpret_cast<HWND>(ULONG_PTR(0xc6));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.publishSwitch = true;
		this->old.manager.publishCreation = true;
		auto& pinned = this->pinnedApps;
		setNativeDesktopPinnedAppsForTest(&pinned);
		QSignalSpy pinDone(this->desktops, &VirtualDesktops::ownWindowPinCompleted);

		// 1. While PinView is actively blocked on the dedicated pin worker,
		//    switchTo, moveWindow, ensureCount, and a second switchTo must all
		//    execute to full completion without waiting for the pin or returning
		//    provider-busy.
		{
			auto blocked = std::make_shared<BlockingCall>();
			Unblock unblock {blocked};
			pinned.beforePin = [blocked] { (*blocked)(); };
			quint64 reqA = 0;
			QCOMPARE(pinNativeDesktopOwnWindow(panelA, &reqA), OwnWindowPin::Pending);
			QVERIFY(reqA != 0);
			QCOMPARE(WaitForSingleObject(blocked->entered, 1000), WAIT_OBJECT_0);
			QVERIFY(!nativeDesktopCommandPending());
			QVERIFY(!nativeDesktopRequestPending());
			// A second concurrent panel pin gets Busy while panelA owns the pin slot.
			quint64 reqBusy = 123;
			QCOMPARE(pinNativeDesktopOwnWindow(panelB, &reqBusy), OwnWindowPin::Busy);
			QCOMPARE(reqBusy, quint64(0));

			QElapsedTimer elapsed;
			elapsed.start();
			QVERIFY(this->desktops->switchTo(1));
			QVERIFY(elapsed.elapsed() < 90);
			QVERIFY(waitUntil(
			    [this] {
				    return this->desktops->lastCommand().value("outcome").toString()
				        == QStringLiteral("observed");
			    },
			    1000
			));
			QCOMPARE(this->old.manager.switches, 1);

			QVERIFY(this->desktops->moveWindow(appWindow, 0));
			QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
			QCOMPARE(this->old.manager.moves, 1);

			bool created = false;
			bool createDone = false;
			this->desktops->ensureCount(3, [&](bool ok) {
				created = ok;
				createDone = true;
			});
			QVERIFY(waitUntil([&createDone] { return createDone; }, 1000));
			QVERIFY(created);
			QCOMPARE(this->old.manager.creates, 1);

			QVERIFY(this->desktops->switchTo(0));
			QVERIFY(waitUntil(
			    [this] {
				    return this->desktops->lastCommand().value("outcome").toString()
				        == QStringLiteral("observed");
			    },
			    1000
			));
			QCOMPARE(this->old.manager.switches, 2);

			// PinView has remained blocked throughout all four desktop commands.
			QCOMPARE(pinDone.size(), 0);
			blocked->release();
			QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 1; }, 1000));
			pinned.beforePin = {};
			QCOMPARE(pinDone.at(0).at(0).toULongLong(), reqA);
			QCOMPARE(pinDone.at(0).at(1).toULongLong(), quintptr(panelA));
			QCOMPARE(pinDone.at(0).at(2).value<OwnWindowPin>(), OwnWindowPin::Pinned);
			QCOMPARE(pinned.pins, 1);
		}

		// 2. A stalled pin retires only the pin worker, never invokes PinView
		//    after retirement, and must not retire the desktop worker or drop
		//    an in-flight switch.
		{
			setNativeDesktopStuckBoundForTest(350);
			auto pinBlocked = std::make_shared<BlockingCall>();
			Unblock unblockPin {pinBlocked};
			auto switchBlocked = std::make_shared<BlockingCall>();
			Unblock unblockSwitch {switchBlocked};
			pinned.pinned = FALSE;
			pinned.beforeQuery = [pinBlocked] { (*pinBlocked)(); };
			this->old.manager.beforeSwitch = [switchBlocked] { (*switchBlocked)(); };

			quint64 reqStall = 0;
			QCOMPARE(pinNativeDesktopOwnWindow(panelB, &reqStall), OwnWindowPin::Pending);
			QCOMPARE(WaitForSingleObject(pinBlocked->entered, 1000), WAIT_OBJECT_0);
			// Give the pin a head start so it stalls while switchTo is still well
			// inside its own stall bound.
			QTest::qWait(160);

			QVERIFY(this->desktops->switchTo(1));
			QCOMPARE(WaitForSingleObject(switchBlocked->entered, 1000), WAIT_OBJECT_0);
			QVERIFY(nativeDesktopCommandPending());

			// Wait for the pin to stall and retire ONLY the pin worker while
			// switchTo is still blocked in flight on the desktop worker.
			QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 2; }, 1000));
			QCOMPARE(pinDone.at(1).at(0).toULongLong(), reqStall);
			QCOMPARE(pinDone.at(1).at(1).toULongLong(), quintptr(panelB));
			QCOMPARE(pinDone.at(1).at(2).value<OwnWindowPin>(), OwnWindowPin::Unavailable);
			QVERIFY(nativeDesktopCommandPending());
			QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);

			// Releasing the switch completes and observes on the original desktop worker.
			switchBlocked->release();
			QVERIFY(waitUntil(
			    [this] {
				    return this->desktops->lastCommand().value("outcome").toString()
				        == QStringLiteral("observed");
			    },
			    1000
			));

			QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
			QCOMPARE(this->acquisitions, 1);
			QCOMPARE(this->old.manager.switches, 3);
			QCOMPARE(pinNativeDesktopOwnWindow(panelA), OwnWindowPin::Unavailable);

			this->old.manager.beforeSwitch = {};
			pinBlocked->release();
			QVERIFY(waitUntil([] { return nativeDesktopWorkerCountForTest() == 1; }, 1000));
			pinned.beforeQuery = {};
			// Retired request saw cancellation after IsViewPinned and skipped PinView.
			QCOMPARE(pinned.pins, 1);
			QCOMPARE(this->old.views.view.refs, 1UL);
		}
	}
	void pinDisconnectAndFaultDeliverUnavailableNotRefused() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xc7));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.publishSwitch = true;
		auto& pinned = this->pinnedApps;
		QSignalSpy pinDone(this->desktops, &VirtualDesktops::ownWindowPinCompleted);

		// Pin-only RPC disconnect delivers Unavailable, retires only the pin worker,
		// and does not disconnect desktop control or drop an in-flight switch.
		auto switchBlocked = std::make_shared<BlockingCall>();
		Unblock unblockSwitch {switchBlocked};
		this->old.manager.beforeSwitch = [switchBlocked] { (*switchBlocked)(); };
		QVERIFY(this->desktops->switchTo(1));
		QCOMPARE(WaitForSingleObject(switchBlocked->entered, 1000), WAIT_OBJECT_0);
		QVERIFY(nativeDesktopCommandPending());

		pinned.pinResult = RPC_E_DISCONNECTED;
		setNativeDesktopPinnedAppsForTest(&pinned);
		quint64 reqDisconnect = 0;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqDisconnect), OwnWindowPin::Pending);
		QVERIFY(reqDisconnect != 0);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 1; }, 1000));
		QCOMPARE(pinDone.at(0).at(0).toULongLong(), reqDisconnect);
		QCOMPARE(pinDone.at(0).at(1).toULongLong(), quintptr(window));
		QCOMPARE(pinDone.at(0).at(2).value<OwnWindowPin>(), OwnWindowPin::Unavailable);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QVERIFY(nativeDesktopCommandPending());
		QVERIFY(waitUntil([] { return nativeDesktopWorkerCountForTest() == 1; }, 1000));
		QCOMPARE(this->acquisitions, 1);

		switchBlocked->release();
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));
		this->old.manager.beforeSwitch = {};
		QCOMPARE(this->old.manager.switches, 1);

		// SEH fault during PinView delivers Unavailable, disables optional pinning,
		// and keeps desktop control available on its separate worker.
		pinned.pinResult = S_OK;
		pinned.faultOnPin = true;
		setNativeDesktopPinnedAppsForTest(&pinned);
		quint64 reqFault = 0;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqFault), OwnWindowPin::Pending);
		QVERIFY(reqFault != 0);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 2; }, 1000));
		QCOMPARE(pinDone.at(1).at(0).toULongLong(), reqFault);
		QCOMPARE(pinDone.at(1).at(1).toULongLong(), quintptr(window));
		QCOMPARE(pinDone.at(1).at(2).value<OwnWindowPin>(), OwnWindowPin::Unavailable);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(pinNativeDesktopOwnWindow(window), OwnWindowPin::Unavailable);
		QVERIFY(this->desktops->switchTo(0));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.switches, 2);
		QCOMPARE(this->acquisitions, 1);
		QCOMPARE(this->old.views.view.refs, 1UL);
	}
	void pinnedAppsQueryServiceRefusalFaultAndStallKeepDesktopControl() {
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xc8));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		this->old.manager.publishSwitch = true;
		this->fresh.manager.publishSwitch = true;
		auto shell = std::make_shared<FakePinnedAppsShell>();
		shell->pinnedApps = &this->pinnedApps;
		std::shared_ptr<BlockingCall> activeBlock;
		// Unblock first and keep both the callback and fake alive until worker
		// exit is confirmed so early assertions can never race or use-after-free.
		struct ShellTeardownGuard {
			std::shared_ptr<FakePinnedAppsShell> shell;
			std::shared_ptr<BlockingCall>* block;
			~ShellTeardownGuard() {
				if (*this->block) (*this->block)->release();
				setNativeDesktopPinnedAppsShellForTest(nullptr);
				setNativeDesktopFactoryForTest({});
				const auto exited = waitUntil([] { return nativeDesktopWorkerCountForTest() == 0; }, 5000);
				if (exited) {
					this->shell->beforeQuery = {};
				} else {
					// Keep the fake alive if a worker failed to exit before test teardown.
					new std::shared_ptr<FakePinnedAppsShell>(this->shell);
					QTest::qFail("Pin worker did not exit before shell teardown", __FILE__, __LINE__);
				}
			}
		} shellGuard {shell, &activeBlock};

		QSignalSpy pinDone(this->desktops, &VirtualDesktops::ownWindowPinCompleted);

		// 1. Refusal with a bogus non-owning out-pointer keeps desktop control available,
		//    never Releases the bogus pointer, and caches unavailability on the pin worker.
		shell->result = E_NOINTERFACE;
		shell->writeBogusOnFail = true;
		setNativeDesktopPinnedAppsShellForTest(shell.get());
		QVERIFY(loadNativeDesktopAccessor(this->accessor, this->desktops, [this] { this->install(); }));
		this->install();
		QCOMPARE(shell->queries, 0);
		quint64 reqRefuse = 0;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqRefuse), OwnWindowPin::Pending);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 1; }, 1000));
		QCOMPARE(pinDone.at(0).at(0).toULongLong(), reqRefuse);
		QCOMPARE(pinDone.at(0).at(2).value<OwnWindowPin>(), OwnWindowPin::Unavailable);
		QCOMPARE(shell->queries, 1);
		QCOMPARE(this->pinnedApps.refs, 1UL);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(pinNativeDesktopOwnWindow(window), OwnWindowPin::Unavailable);
		QCOMPARE(shell->queries, 1);
		QVERIFY(this->desktops->switchTo(1));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));

		// 2. SEH fault with a bogus non-owning out-pointer does not Release the bogus
		//    pointer, retires only the pin worker, and keeps desktop control available.
		shell->queries = 0;
		shell->result = S_OK;
		shell->faultOnQuery = true;
		shell->writeBogusOnFail = true;
		QVERIFY(loadNativeDesktopAccessor(this->accessor, this->desktops, [this] { this->install(); }));
		this->install();
		quint64 reqFault = 0;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqFault), OwnWindowPin::Pending);
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 2; }, 1000));
		QCOMPARE(pinDone.at(1).at(0).toULongLong(), reqFault);
		QCOMPARE(pinDone.at(1).at(2).value<OwnWindowPin>(), OwnWindowPin::Unavailable);
		QCOMPARE(shell->queries, 1);
		QCOMPARE(this->pinnedApps.refs, 1UL);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(pinNativeDesktopOwnWindow(window), OwnWindowPin::Unavailable);
		QCOMPARE(shell->queries, 1);
		QVERIFY(this->desktops->switchTo(0));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));

		// 3. Stall past stallLimit inside QueryService never blocks switchTo, retires
		//    only the pin worker, and disables subsequent pinning attempts.
		shell->queries = 0;
		shell->result = S_OK;
		shell->faultOnQuery = false;
		shell->writeBogusOnFail = false;
		activeBlock = std::make_shared<BlockingCall>();
		shell->beforeQuery = [blocked = activeBlock] { (*blocked)(); };
		setNativeDesktopStuckBoundForTest(60);
		QVERIFY(loadNativeDesktopAccessor(this->accessor, this->desktops, [this] { this->install(); }));
		this->install();
		quint64 reqStall = 0;
		QCOMPARE(pinNativeDesktopOwnWindow(window, &reqStall), OwnWindowPin::Pending);
		QCOMPARE(WaitForSingleObject(activeBlock->entered, 1000), WAIT_OBJECT_0);
		// Desktop control works immediately while QueryService is blocked.
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QVERIFY(this->desktops->switchTo(1));
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("observed");
		    },
		    1000
		));
		// Pin worker retires on its own stall bound without disconnecting desktops.
		QVERIFY(waitUntil([&pinDone] { return pinDone.size() == 3; }, 1000));
		QCOMPARE(pinDone.at(2).at(0).toULongLong(), reqStall);
		QCOMPARE(pinDone.at(2).at(2).value<OwnWindowPin>(), OwnWindowPin::Unavailable);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(shell->queries, 0);
		QCOMPARE(pinNativeDesktopOwnWindow(window), OwnWindowPin::Unavailable);

		activeBlock->release();
		activeBlock.reset();
		setNativeDesktopPinnedAppsShellForTest(nullptr);
		setNativeDesktopFactoryForTest({});
		QVERIFY(waitUntil([] { return nativeDesktopWorkerCountForTest() == 0; }, 1000));
		shell->beforeQuery = {};
		QCOMPARE(shell->queries, 1);
		QCOMPARE(this->pinnedApps.refs, 1UL);
	}
	void panelPinStateLifecycleTransitions() {
		const auto hwndA = reinterpret_cast<HWND>(ULONG_PTR(0xd1));
		const auto hwndB = reinterpret_cast<HWND>(ULONG_PTR(0xd2));
		const auto hwndOther = reinterpret_cast<HWND>(ULONG_PTR(0xd3));
		PanelPinState state;
		qint64 now = 1000;

		// Prepare hwndA and submit Busy retries without resetting on overlapping prepare().
		QVERIFY(state.prepare(hwndA));
		const auto genA = state.generation;
		QVERIFY(state.canAttempt(hwndA, genA));
		QCOMPARE(
		    state.onSubmitResult(hwndA, genA, OwnWindowPin::Busy, now),
		    PanelPinState::SubmitAction::RetryLater
		);
		// Overlapping stickToAllDesktops() for same HWND preserves busy wait timer.
		QVERIFY(state.prepare(hwndA));
		QVERIFY(state.firstBusyMs.has_value());

		// Fast sibling churn within BUSY_WAIT_BUDGET_MS does not exhaust the budget,
		// and a sibling completion renews the per-handoff window beyond a 5 s stall.
		for (int i = 0; i < 40; ++i) {
			now += 100;
			QCOMPARE(
			    state.onSubmitResult(hwndA, genA, OwnWindowPin::Busy, now),
			    PanelPinState::SubmitAction::RetryLater
			);
		}
		QCOMPARE(
		    state.onAsyncCompletion(
		        10, hwndOther, OwnWindowPin::Pinned, hwndA, true, false, true, true
		    ),
		    PanelPinState::CompletionAction::RetryImmediate
		);
		now += 5500;
		QCOMPARE(
		    state.onSubmitResult(hwndA, genA, OwnWindowPin::Busy, now),
		    PanelPinState::SubmitAction::RetryLater
		);

		// Submit async Pending with requestId 101; duplicate prepare() is suppressed.
		QCOMPARE(
		    state.onSubmitResult(hwndA, genA, OwnWindowPin::Pending, now, 101),
		    PanelPinState::SubmitAction::None
		);
		QVERIFY(state.inFlightFor(hwndA));
		QVERIFY(!state.prepare(hwndA));

		// Hide and re-show same HWND while request 101 is still queued, then submit
		// request 102. Late completion for request 101 must NOT match request 102.
		state.reset(false);
		QVERIFY(state.prepare(hwndA));
		const auto genA2 = state.generation;
		QCOMPARE(
		    state.onSubmitResult(hwndA, genA2, OwnWindowPin::Pending, now, 102),
		    PanelPinState::SubmitAction::None
		);
		QCOMPARE(
		    state.onAsyncCompletion(101, hwndA, OwnWindowPin::Pinned, hwndA, true, false, true, false),
		    PanelPinState::CompletionAction::None
		);
		QVERIFY(!state.pinnedHere(hwndA));
		QVERIFY(state.inFlightFor(hwndA));
		QCOMPARE(
		    state.onAsyncCompletion(102, hwndA, OwnWindowPin::Pinned, hwndA, true, false, true, false),
		    PanelPinState::CompletionAction::None
		);
		QVERIFY(state.pinnedHere(hwndA));

		// Accessor-only recovery reset(true) clears pinnedHere so IsViewPinned re-verifies.
		state.reset(true);
		QVERIFY(!state.pinnedHere(hwndA));

		// Provider disconnect mid-pin delivers Unavailable with accessorLoaded=false ->
		// TriggerFollow WITHOUT permanently marking failedHwnd.
		QVERIFY(state.prepare(hwndA));
		const auto genA3 = state.generation;
		QCOMPARE(
		    state.onSubmitResult(hwndA, genA3, OwnWindowPin::Pending, now, 103),
		    PanelPinState::SubmitAction::None
		);
		QCOMPARE(
		    state.onAsyncCompletion(
		        103, hwndA, OwnWindowPin::Unavailable, hwndA, true, false, false, false
		    ),
		    PanelPinState::CompletionAction::TriggerFollow
		);
		QCOMPARE(state.failedHwnd, nullptr);

		// HWND recreation resets failure/retry state; exceeding BUSY_WAIT_BUDGET_MS
		// without a slot handoff falls back to TriggerFollow and records failedHwnd.
		QVERIFY(state.prepare(hwndB));
		const auto genB = state.generation;
		QCOMPARE(
		    state.onSubmitResult(hwndB, genB, OwnWindowPin::Busy, 20000),
		    PanelPinState::SubmitAction::RetryLater
		);
		QCOMPARE(
		    state.onSubmitResult(
		        hwndB, genB, OwnWindowPin::Busy, 20000 + PanelPinState::BUSY_WAIT_BUDGET_MS - 1
		    ),
		    PanelPinState::SubmitAction::RetryLater
		);
		QCOMPARE(
		    state.onSubmitResult(
		        hwndB, genB, OwnWindowPin::Busy, 20000 + PanelPinState::BUSY_WAIT_BUDGET_MS
		    ),
		    PanelPinState::SubmitAction::TriggerFollow
		);
		QCOMPARE(state.failedHwnd, hwndB);
		QVERIFY(!state.prepare(hwndB));

		// Even with continuous sibling handoffs, MAX_TOTAL_BUSY_WAIT_MS caps waiting.
		state.reset(true);
		QVERIFY(state.prepare(hwndB));
		const auto genB2 = state.generation;
		qint64 capNow = 50000;
		QCOMPARE(
		    state.onSubmitResult(hwndB, genB2, OwnWindowPin::Busy, capNow),
		    PanelPinState::SubmitAction::RetryLater
		);
		for (int handoff = 0; handoff < 5; ++handoff) {
			QCOMPARE(
			    state.onAsyncCompletion(
			        quint64(200 + handoff), hwndOther, OwnWindowPin::Pinned, hwndB, true, false, true, true
			    ),
			    PanelPinState::CompletionAction::RetryImmediate
			);
			capNow += PanelPinState::BUSY_WAIT_BUDGET_MS - 1;
			if (capNow - 50000 < PanelPinState::MAX_TOTAL_BUSY_WAIT_MS) {
				QCOMPARE(
				    state.onSubmitResult(hwndB, genB2, OwnWindowPin::Busy, capNow),
				    PanelPinState::SubmitAction::RetryLater
				);
			}
		}
		QCOMPARE(
		    state.onSubmitResult(
		        hwndB, genB2, OwnWindowPin::Busy, 50000 + PanelPinState::MAX_TOTAL_BUSY_WAIT_MS
		    ),
		    PanelPinState::SubmitAction::TriggerFollow
		);
		QCOMPARE(state.failedHwnd, hwndB);
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
			const auto expectedOutcome = succeed
			                               ? QStringLiteral("observed")
			                               : (operation == "switch" ? QStringLiteral("denied")
			                                                        : QStringLiteral("timed-out"));
			const auto expectedReason = operation == "switch" ? QStringLiteral("permission-denied")
			                                                  : QStringLiteral("observation-timeout");
			QVERIFY(waitUntil(
			    [this, expectedOutcome] {
				    return this->desktops->lastCommand().value("outcome").toString() == expectedOutcome;
			    },
			    2500
			));
			QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), expectedOutcome);
			QCOMPARE(this->desktops->lastCommand().value("requestId"), accepted.value("requestId"));
			if (!succeed)
				QCOMPARE(this->desktops->lastCommand().value("reason").toString(), expectedReason);
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
		NativeDesktopFacadeTestHook::setObservationBudget(*this->desktops, 150);
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
		// Guard remains active immediately after the failed replacement, then recovers via DeadlineGuard.
		QVERIFY(!this->desktops->isDesktopCurrentlyActive(0, this->old.one.id));
		QVERIFY(waitUntil(
		    [this] { return this->desktops->isDesktopCurrentlyActive(0, this->old.one.id); },
		    1000
		));
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
			        == QStringLiteral("denied");
		    },
		    1000
		));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("permission-denied")
		);
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

	void externalDesktopChangeAdvancesWorkspaceIntentAndBlocksStaleFollow() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		const auto moveIntent = this->desktops->beginWorkspaceIntent();
		// Simulate an external Windows Task View switch from A to C.
		this->old.manager.current = &this->old.three;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(2));
		QVERIFY(this->desktops->workspaceIntentOrder() != moveIntent);

		int observedCalls = 0;
		QVERIFY(!this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    moveIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QCOMPARE(this->old.manager.switches, 0);
		QCOMPARE(observedCalls, 0);
	}

	void conditionalFollowSwitchAbortsWhenTaskViewChangedDesktopBeforeCacheRefresh() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		this->old.manager.publishSwitch = true;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		const auto moveIntent = this->desktops->beginWorkspaceIntent();
		// User switches to C in Task View before any native cache refresh runs.
		this->old.manager.current = &this->old.three;

		int observedCalls = 0;
		QVERIFY(!this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    moveIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.switches, 0);
		QCOMPARE(this->old.manager.current, &this->old.three);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(2));
		QCOMPARE(observedCalls, 0);
	}

	void slowOriginCheckCancelledWhenTaskViewSwitchedToDestinationSkipsFollow() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		this->old.manager.publishSwitch = true;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeEnumeration = [blocked] { (*blocked)(); };

		const auto moveIntent = this->desktops->beginWorkspaceIntent();
		int observedCalls = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    moveIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QVERIFY(nativeDesktopCommandPending());
		QCOMPARE(WaitForSingleObject(blocked->entered, 1000), WAIT_OBJECT_0);

		// User switched directly to destination B in Task View while the origin check was slow.
		this->old.manager.current = &this->old.two;
		blocked->release();
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("superseded");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeEnumeration = {};
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("origin-changed")
		);
		QCOMPARE(this->old.manager.switches, 0);
		QCOMPARE(observedCalls, 0);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
		QVERIFY(this->desktops->isDesktopCurrentlyActive(1, this->old.two.id));
	}

	void originChangedOnThirdDesktopStillAllowsSubsequentConditionalFollow() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		// Keep publishSwitch false so SwitchDesktop(B) succeeds without publishing B,
		// allowing the post-command refresh to report C and hit the third-desktop branch.
		this->old.manager.publishSwitch = false;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));

		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };

		const auto firstIntent = this->desktops->beginWorkspaceIntent();
		int firstObserved = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&firstObserved](qsizetype) { ++firstObserved; },
		    firstIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QVERIFY(nativeDesktopCommandPending());
		QCOMPARE(WaitForSingleObject(blocked->entered, 1000), WAIT_OBJECT_0);

		// Origin check on A passed and SwitchDesktop(B) is in flight; Task View moves to C.
		this->old.manager.current = &this->old.three;
		blocked->release();
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("superseded");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeSwitch = {};
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("origin-changed")
		);
		QCOMPARE(firstObserved, 0);
		QCOMPARE(this->old.manager.switches, 1);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(2));
		QVERIFY(this->desktops->isDesktopCurrentlyActive(2, this->old.three.id));

		// A subsequent conditional follow from C to B must still succeed.
		this->old.manager.publishSwitch = true;
		const auto nextIntent = this->desktops->beginWorkspaceIntent();
		int nextObserved = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&nextObserved](qsizetype) { ++nextObserved; },
		    nextIntent,
		    this->old.two.id,
		    this->old.three.id
		));
		QVERIFY(waitUntil([&nextObserved] { return nextObserved == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.switches, 2);
		QCOMPARE(this->old.manager.current, &this->old.two);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
	}

	void moveAndFollowSucceedsAfterObservationTimeout() {
		NativeDesktopFacadeTestHook::setObservationBudget(*this->desktops, 300);
		this->old.manager.array.desktops = {&this->old.one, &this->old.two};
		this->old.manager.current = &this->old.one;
		this->old.manager.publishSwitch = false;
		QCOMPARE(refreshNativeDesktopForTest(), 2);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		// Explicit switch accepted synchronously, never observed -> times out.
		QVERIFY(this->desktops->switchTo(1));
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("accepted"));
		QVERIFY(!this->desktops->isDesktopCurrentlyActive(0, this->old.one.id));
		// Drain any initial observation refresh before hooking the post-timeout refresh.
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));

		// Block the post-timeout refresh to verify the stale-cache guard holds until it lands.
		auto refreshBlocked = std::make_shared<BlockingCall>();
		Unblock unblockRefresh {refreshBlocked};
		this->old.manager.beforeEnumeration = [refreshBlocked] { (*refreshBlocked)(); };
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("timed-out");
		    },
		    1500
		));
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("observation-timeout")
		);
		QCOMPARE(WaitForSingleObject(refreshBlocked->entered, 1000), WAIT_OBJECT_0);
		QVERIFY(!this->desktops->isDesktopCurrentlyActive(0, this->old.one.id));

		refreshBlocked->release();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeEnumeration = {};
		QVERIFY(this->desktops->isDesktopCurrentlyActive(0, this->old.one.id));

		// A subsequent conditional move-and-follow from A to B must succeed after timeout recovery.
		this->old.manager.publishSwitch = true;
		const auto followIntent = this->desktops->beginWorkspaceIntent();
		int observedCalls = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    followIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QVERIFY(waitUntil([&observedCalls] { return observedCalls == 1; }, 1000));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->old.manager.current, &this->old.two);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
	}

	void lateDeniedSwitchWhenTaskViewLandsOnDestinationSkipsFollow() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		this->old.manager.publishSwitch = true;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		this->old.manager.beforeSwitch = [blocked] { (*blocked)(); };
		this->old.manager.switchResult = E_ACCESSDENIED;

		const auto followIntent = this->desktops->beginWorkspaceIntent();
		int observedCalls = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    followIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QVERIFY(nativeDesktopCommandPending());
		QCOMPARE(WaitForSingleObject(blocked->entered, 1000), WAIT_OBJECT_0);

		// Origin check on A passed; SwitchDesktop(B) will return E_ACCESSDENIED after 100 ms,
		// while Task View independently switches directly to destination B.
		this->old.manager.current = &this->old.two;
		blocked->release();
		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("denied");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeSwitch = {};
		this->old.manager.switchResult = S_OK;
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("permission-denied")
		);
		QCOMPARE(this->old.manager.switches, 1);
		QCOMPARE(observedCalls, 0);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
		QVERIFY(this->desktops->isDesktopCurrentlyActive(1, this->old.two.id));
	}

	void lateCancelledSwitchHiddenByNewerRefreshStillSkipsFollow() {
		this->old.manager.array.desktops = {&this->old.one, &this->old.two, &this->old.three};
		this->old.manager.current = &this->old.one;
		this->old.manager.publishSwitch = true;
		QCOMPARE(refreshNativeDesktopForTest(), 3);
		this->desktops->refresh();
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		QCOMPARE(this->desktops->currentIndex(), qsizetype(0));

		// Create a message-only window so the worker can synchronously trigger
		// requestNativeDesktopRefresh() via QS_SENDMESSAGE during execute()'s 100 ms wait.
		static const wchar_t* className = L"QsErm625SentRefreshProbe";
		WNDCLASSW wc {};
		wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
			if (msg == WM_APP) {
				requestNativeDesktopRefresh();
				return 1;
			}
			return DefWindowProcW(hwnd, msg, wp, lp);
		};
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = className;
		RegisterClassW(&wc);
		HWND msgHwnd =
		    CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
		QVERIFY(msgHwnd != nullptr);
		struct WindowGuard {
			HWND hwnd;
			~WindowGuard() {
				if (this->hwnd) DestroyWindow(this->hwnd);
			}
		} windowGuard {msgHwnd};

		auto blocked = std::make_shared<BlockingCall>();
		Unblock unblock {blocked};
		HANDLE refreshStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		struct EventGuard {
			HANDLE handle;
			~EventGuard() {
				if (this->handle) CloseHandle(this->handle);
			}
		} eventGuard {refreshStarted};
		std::atomic<int> enumCalls {0};
		this->old.manager.beforeEnumeration = [blocked, msgHwnd, refreshStarted, &enumCalls] {
			const auto call = ++enumCalls;
			if (call == 1) {
				SendMessageW(msgHwnd, WM_APP, 0, 0);
				(*blocked)();
			} else if (call == 2) {
				SetEvent(refreshStarted);
			}
		};

		const auto followIntent = this->desktops->beginWorkspaceIntent();
		int observedCalls = 0;
		QVERIFY(this->desktops->switchTo(
		    1,
		    {},
		    [&observedCalls](qsizetype) { ++observedCalls; },
		    followIntent,
		    this->old.two.id,
		    this->old.one.id
		));
		QVERIFY(nativeDesktopCommandPending());
		QCOMPARE(WaitForSingleObject(blocked->entered, 1000), WAIT_OBJECT_0);

		// Task View switched to destination B while the switch's origin check was blocked.
		this->old.manager.current = &this->old.two;
		blocked->release();
		// Let both the cancelled switch (N) and the queued refresh (N+1) complete on the worker
		// before pumping GUI events so Channel::collect() sees refresh N+1 ready alongside switch N.
		QCOMPARE(WaitForSingleObject(refreshStarted, 1000), WAIT_OBJECT_0);
		QThread::msleep(20);

		QVERIFY(waitUntil(
		    [this] {
			    return this->desktops->lastCommand().value("outcome").toString()
			        == QStringLiteral("superseded");
		    },
		    1000
		));
		QVERIFY(waitUntil([] { return !nativeDesktopRequestPending(); }, 1000));
		this->old.manager.beforeEnumeration = {};
		QCOMPARE(
		    this->desktops->lastCommand().value("reason").toString(),
		    QStringLiteral("origin-changed")
		);
		QCOMPARE(this->old.manager.switches, 0);
		QCOMPARE(observedCalls, 0);
		QCOMPARE(this->desktops->currentIndex(), qsizetype(1));
		QVERIFY(this->desktops->isDesktopCurrentlyActive(1, this->old.two.id));
	}
};
QTEST_GUILESS_MAIN(Erm625NativeDesktopFacadeTest)
#include "erm625_native_desktop_facade_test.moc"
