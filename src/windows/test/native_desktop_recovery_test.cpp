#include <atomic>
#include <chrono>
#include <thread>

#include <qtest.h>

#include "desktop_recovery.hpp"
#include "erm625_desktop_fakes.hpp"
#include "native_desktop_accessor.hpp"
#include "native_desktop_interfaces.hpp"

using namespace qs::windows;
using namespace qs::windows::native_desktop;
using namespace erm625;
namespace qs::windows {
DWORD testBuild = 26100;
DWORD windowsBuild() { return testBuild; }
} // namespace qs::windows

class NativeDesktopRecoveryTest: public QObject {
	Q_OBJECT;
private slots:
	void cleanup() {
		setNativeDesktopFactoryForTest({});
		testBuild = 26100;
	}
	void unknownBuildNeverAcquires() {
		int calls = 0;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager**, ViewCollection**, IVirtualDesktopManager**) {
			    ++calls;
			    return E_FAIL;
		    }
		);
		testBuild = 99999;
		VirtualDesktops::Accessor accessor;
		QObject context;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::UnsupportedBuild);
		QVERIFY(!retryNativeDesktopAccessor());
		QCOMPARE(calls, 0);
	}
	void acquisitionDenied() {
		int calls = 0;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager**, ViewCollection**, IVirtualDesktopManager**) {
			    ++calls;
			    return E_ACCESSDENIED;
		    }
		);
		VirtualDesktops::Accessor accessor;
		QObject context;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::PermissionDenied);
		QCOMPARE(nativeDesktopLastError(), E_ACCESSDENIED);
		initializeNativeDesktopQueries(&context, [] {});
		QCOMPARE(calls, 1); // Attaching the GUI observer must not bypass backoff.
	}
	void unexpectedHresultIsNotStructuredException() {
		setNativeDesktopFactoryForTest(
		    [](DesktopManager**, ViewCollection**, IVirtualDesktopManager**) { return E_UNEXPECTED; }
		);
		VirtualDesktops::Accessor accessor;
		QObject context;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::MissingProvider);
		QCOMPARE(nativeDesktopLastError(), E_UNEXPECTED);
	}
	void structuredExceptionInvalidates_data() {
		QTest::addColumn<QString>("operation");
		for (const auto* operation:
		     {"enumerate",
		      "desktop-id",
		      "switch",
		      "create",
		      "window-id",
		      "on-current",
		      "broken-release",
		      "current",
		      "get-view",
		      "can-move",
		      "move",
		      "local-array-release",
		      "local-current-release",
		      "local-create-release",
		      "local-view-release",
		      "snapshot-release",
		      "local-failed-output-release",
		      "local-pending-list-release"})
		{
			QTest::newRow(operation) << QString::fromLatin1(operation);
		}
	}
	void structuredExceptionInvalidates() {
		QFETCH(QString, operation);
		Fixture old, fresh;
		FakeDesktop active(1);
		fresh.one.id.Data1 = 101;
		int acquisitions = 0, recovered = 0;
		bool online = false;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    if (++acquisitions == 1) return old.acquire(m, v, p);
			    if (!online) return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
			    return fresh.acquire(m, v, p);
		    }
		);
		struct Reset {
			Fixture& old;
			Fixture& fresh;
			FakeDesktop& active;
			~Reset() {
				old.clearFaults();
				fresh.clearFaults();
				active.faultOnRelease = false;
				setNativeDesktopFactoryForTest({});
			}
		} reset {old, fresh, active};
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; }));
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0x625));
		setNativeDesktopWindowValidatorForTest([window](HWND candidate) {
			return candidate == window;
		});
		if (operation == "enumerate") old.manager.faultOnEnumeration = true;
		if (operation == "desktop-id") old.one.faultOnId = true;
		if (operation == "switch" || operation == "broken-release") old.manager.faultOnSwitch = true;
		if (operation == "broken-release") old.manager.faultOnRelease = true;
		if (operation == "create") old.manager.faultOnCreate = true;
		if (operation == "window-id" || operation == "on-current") old.publicApi.faultOnRead = true;
		if (operation == "current") old.manager.faultOnCurrent = true;
		if (operation == "get-view") old.views.faultOnGet = true;
		if (operation == "can-move") old.manager.faultOnCanMove = true;
		if (operation == "move") old.manager.faultOnMove = true;
		if (operation == "local-array-release") old.manager.array.faultOnRelease = true;
		if (operation == "local-current-release") {
			old.manager.current = &active;
			active.faultOnRelease = true;
		}
		if (operation == "local-create-release") old.three.faultOnRelease = true;
		if (operation == "local-view-release") old.views.view.faultOnRelease = true;
		if (operation == "snapshot-release") old.one.faultOnRelease = true;
		if (operation == "local-failed-output-release") {
			old.one.faultOnId = old.one.faultOnRelease = true;
		}
		if (operation == "local-pending-list-release") {
			old.two.faultOnId = old.one.faultOnRelease = true;
		}

		int result = 0;
		if (operation == "get-view" || operation == "can-move" || operation == "move"
		    || operation == "local-view-release")
			result = accessor.moveWindowToDesktopNumber(window, 1);
		else if (operation == "create" || operation == "local-create-release")
			result = accessor.createDesktop();
		else if (operation == "window-id" || operation == "on-current") {
			// Reads are cached. Register a window and explicitly refresh the
			// worker before asserting the fault result.
			accessor.getWindowDesktopNumber(window);
			result = refreshNativeDesktopForTest();
		} else if (operation == "switch" || operation == "broken-release")
			result = accessor.goToDesktopNumber(1);
		else result = refreshNativeDesktopForTest();
		old.clearFaults();
		active.faultOnRelease = false;
		QCOMPARE(result, -1);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		QCOMPARE(nativeDesktopLastError(), E_UNEXPECTED);
		QCOMPARE(
		    desktopAvailabilityReason(nativeDesktopAvailability()),
		    QStringLiteral("provider-disconnected")
		);
		QCOMPARE(old.manager.refs, 1UL);
		QCOMPARE(old.views.refs, 1UL);
		QCOMPARE(old.publicApi.refs, 1UL);
		QCOMPARE(old.manager.array.refs, 1UL);
		QCOMPARE(old.one.refs, 1UL);
		QCOMPARE(old.two.refs, 1UL);
		QCOMPARE(old.three.refs, 1UL); // Partial CreateDesktop output was also released.
		QCOMPARE(active.refs, 1UL);
		QCOMPARE(old.views.view.refs, 1UL);
		if (operation == "get-view") {
			QCOMPARE(old.views.view.releases, 0);
			QCOMPARE(old.manager.canMoves, 0);
			QCOMPARE(old.manager.moves, 0);
		}
		if (operation == "can-move") QCOMPARE(old.manager.moves, 0);
		const auto enumerations = old.manager.enumerations;
		const auto switches = old.manager.switches;
		QVERIFY(!retryNativeDesktopAccessor());
		QCOMPARE(accessor.goToDesktopNumber(1), -1);
		QCOMPARE(accessor.getDesktopCount(), -1);
		QCOMPARE(old.manager.enumerations, enumerations);
		QCOMPARE(old.manager.switches, switches);
		// Commands fail while unavailable; reads retain their last snapshot.
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), old.one.id));
		QCOMPARE(accessor.getCurrentDesktopNumber(), 0);
		QCOMPARE(acquisitions, 1); // Dispatch cannot bypass the active backoff.
		online = true;
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 1500);
		QCOMPARE(acquisitions, 2);
		QVERIFY(retryNativeDesktopAccessor());
		QCOMPARE(nativeDesktopLastError(), S_OK);
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), fresh.one.id));
		QCOMPARE(accessor.goToDesktopNumber(1), 0);
		QCOMPARE(fresh.manager.switches, 1);
		QCOMPARE(old.manager.switches, switches);
		setNativeDesktopFactoryForTest({});
		QCOMPARE(fresh.manager.refs, 1UL);
		QCOMPARE(fresh.one.refs, 1UL);
	}
	void failedViewAcquisitionDoesNotTransferOwnership_data() {
		QTest::addColumn<qint32>("failure");
		for (const auto failure: {E_FAIL, E_UNEXPECTED, E_ACCESSDENIED, RPC_E_DISCONNECTED}) {
			QTest::newRow(qPrintable(QString::number(failure, 16))) << qint32(failure);
		}
	}
	void failedViewAcquisitionDoesNotTransferOwnership() {
		QFETCH(qint32, failure);
		Fixture fixture;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return fixture.acquire(m, v, p);
		    }
		);
		struct Reset {
			Fixture& fixture;
			~Reset() {
				fixture.clearFaults();
				setNativeDesktopFactoryForTest({});
			}
		} reset {fixture};
		VirtualDesktops::Accessor accessor;
		QObject context;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [] {}));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		fixture.views.result = failure;
		QCOMPARE(accessor.moveWindowToDesktopNumber(reinterpret_cast<HWND>(ULONG_PTR(0x625)), 1), -1);
		QCOMPARE(fixture.views.view.refs, 1UL);
		QCOMPARE(fixture.views.view.releases, 0);
		QCOMPARE(fixture.manager.canMoves, 0);
		QCOMPARE(nativeDesktopLastError(), HRESULT(failure));
		QCOMPARE(
		    nativeDesktopAvailability(),
		    failure == RPC_E_DISCONNECTED ? DesktopAvailability::Disconnected
		                                  : DesktopAvailability::Available
		);
	}
	void acquisitionLocalReleaseFaultRecovers() {
		Fixture old, fresh;
		old.manager.array.faultOnRelease = true;
		int acquisitions = 0, recovered = 0;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return (++acquisitions == 1 ? old : fresh).acquire(m, v, p);
		    }
		);
		struct Reset {
			Fixture& old;
			Fixture& fresh;
			~Reset() {
				old.clearFaults();
				fresh.clearFaults();
				setNativeDesktopFactoryForTest({});
			}
		} reset {old, fresh};
		QObject context;
		VirtualDesktops::Accessor accessor;
		const auto loaded = loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; });
		old.clearFaults();
		QVERIFY(!loaded);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		QCOMPARE(nativeDesktopLastError(), E_UNEXPECTED);
		QCOMPARE(old.manager.array.refs, 1UL);
		QCOMPARE(old.one.refs, 1UL);
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 1500);
		QCOMPARE(acquisitions, 2);
		QVERIFY(retryNativeDesktopAccessor());
	}
	void structuredExceptionDuringAcquisitionRecovers() {
		Fixture partial, fresh;
		int acquisitions = 0, recovered = 0;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    ++acquisitions;
			    if (acquisitions <= 2) {
				    partial.acquire(m, v, p);
				    fault();
			    }
			    return fresh.acquire(m, v, p);
		    }
		);
		struct Reset {
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; }));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		QCOMPARE(nativeDesktopLastError(), E_UNEXPECTED);
		QCOMPARE(partial.manager.refs, 1UL);
		QCOMPARE(partial.views.refs, 1UL);
		QCOMPARE(partial.publicApi.refs, 1UL);
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 2000);
		QCOMPARE(acquisitions, 3);
		QCOMPARE(partial.manager.refs, 1UL);
		QCOMPARE(partial.views.refs, 1UL);
		QCOMPARE(partial.publicApi.refs, 1UL);
		QVERIFY(retryNativeDesktopAccessor());
	}
	void emptySnapshotIsHealthy() {
		FakeManager manager;
		FakeViews views;
		FakePublic publicApi;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    manager.AddRef();
			    views.AddRef();
			    publicApi.AddRef();
			    *m = &manager;
			    *v = &views;
			    *p = &publicApi;
			    return S_OK;
		    }
		);
		struct Reset {
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		VirtualDesktops::Accessor accessor;
		QObject context;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [] {}));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(accessor.getDesktopCount(), 0);
		QCOMPARE(accessor.getCurrentDesktopNumber(), -1);
	}
	void disconnect_data() {
		QTest::addColumn<qint32>("failure");
		QTest::addColumn<int>("availability");
		QTest::newRow("RPC_E_DISCONNECTED")
		    << qint32(RPC_E_DISCONNECTED) << int(DesktopAvailability::Disconnected);
		QTest::newRow("RPC_S_SERVER_UNAVAILABLE")
		    << qint32(HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
		    << int(DesktopAvailability::Disconnected);
		QTest::newRow("CO_E_OBJNOTCONNECTED")
		    << qint32(CO_E_OBJNOTCONNECTED) << int(DesktopAvailability::Disconnected);
		QTest::newRow("E_ACCESSDENIED")
		    << qint32(E_ACCESSDENIED) << int(DesktopAvailability::PermissionDenied);
		QTest::newRow("E_FAIL") << qint32(E_FAIL) << int(DesktopAvailability::MissingProvider);
	}
	void disconnect() {
		QFETCH(qint32, failure);
		QFETCH(int, availability);
		FakeDesktop one(1), two(2), three(3);
		FakeManager manager;
		FakeViews views;
		FakePublic publicApi;
		manager.array.desktops = {&one, &two};
		manager.current = &one;
		int acquisitions = 0;
		bool online = true;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    ++acquisitions;
			    if (!online) return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
			    manager.AddRef();
			    views.AddRef();
			    publicApi.AddRef();
			    *m = &manager;
			    *v = &views;
			    *p = &publicApi;
			    return S_OK;
		    }
		);
		// Reset even if an assertion below exits early; the factory captures locals.
		struct Reset {
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		QObject context;
		int recovered = 0;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; }));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		const auto before = manager.enumerations;
		QCOMPARE(accessor.getDesktopCount(), 2);
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), one.id));
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(1), two.id));
		QCOMPARE(accessor.getCurrentDesktopNumber(), 0);
		QCOMPARE(manager.enumerations, before); // Cached reads never enumerate.
		manager.result = failure;
		online = false;
		QCOMPARE(refreshNativeDesktopForTest(), -1);
		QCOMPARE(nativeDesktopAvailability(), static_cast<DesktopAvailability>(availability));
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), one.id));
		QCOMPARE(accessor.getCurrentDesktopNumber(), 0);
		QCOMPARE(manager.refs, 1UL);
		QCOMPARE(views.refs, 1UL);
		QCOMPARE(publicApi.refs, failure == E_ACCESSDENIED || failure == E_FAIL ? 2UL : 1UL);
		QCOMPARE(one.refs, 1UL);
		QCOMPARE(two.refs, 1UL);
		QCOMPARE(accessor.goToDesktopNumber(1), -1);
		QTRY_VERIFY_WITH_TIMEOUT(acquisitions >= 2, 1500); // First retry while Explorer is absent.
		manager.result = S_OK;
		online = true;
		manager.array.desktops = {&three, &two, &one};
		manager.current = &two;
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 2000);
		QVERIFY(retryNativeDesktopAccessor());
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(accessor.getDesktopCount(), 3);
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), three.id));
		QCOMPARE(accessor.getCurrentDesktopNumber(), 1);
		QCOMPARE(accessor.goToDesktopNumber(2), 0);
		QCOMPARE(manager.switches, 1);
		invalidateNativeDesktopAccessor(); // TaskbarCreated takes the same path.
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), three.id));
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 2, 1500);
	}
	void blockedCallsAreBounded_data() {
		QTest::addColumn<QString>("operation");
		for (const auto* operation:
		     {"activation",
		      "enumeration",
		      "switch",
		      "create",
		      "move",
		      "window-query",
		      "own-window-move",
		      "release"})
			QTest::newRow(operation) << QString::fromLatin1(operation);
	}
	void slowRefreshKeepsTheApartmentAndAppliesItsSnapshot() {
		auto fake = std::make_shared<Fixture>();
		auto acquisitions = std::make_shared<std::atomic<int>>(0);
		setNativeDesktopFactoryForTest(
		    [fake, acquisitions](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    ++*acquisitions;
			    return fake->acquire(m, v, p);
		    }
		);
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [] {}));
		fake->manager.beforeEnumeration = [weak = std::weak_ptr<Fixture>(fake)] {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			if (auto fake = weak.lock()) fake->manager.current = &fake->two;
		};
		requestNativeDesktopRefresh();
		QTest::qWait(150);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(accessor.getCurrentDesktopNumber(), 0);
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		QTRY_COMPARE_WITH_TIMEOUT(accessor.getCurrentDesktopNumber(), 1, 1000);
		QCOMPARE(acquisitions->load(), 1);
		setNativeDesktopFactoryForTest({});
	}
	void slowAcquisitionDoesNotStartRecoveryOrRunTwice() {
		auto fake = std::make_shared<Fixture>();
		auto acquisitions = std::make_shared<std::atomic<int>>(0);
		setNativeDesktopFactoryForTest(
		    [fake, acquisitions](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    ++*acquisitions;
			    std::this_thread::sleep_for(std::chrono::milliseconds(300));
			    return fake->acquire(m, v, p);
		    }
		);
		QObject context;
		auto recovered = std::make_shared<int>(0);
		VirtualDesktops::Accessor accessor;
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [recovered] { ++*recovered; }));
		QVERIFY(elapsed.elapsed() < 250);
		initializeNativeDesktopQueries(&context, [] {});
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
		QTRY_COMPARE_WITH_TIMEOUT(*recovered, 1, 1000);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(accessor.getDesktopCount(), 2);
		QCOMPARE(acquisitions->load(), 1);
	}
	void slowPublicMoveIsAcceptedAndAppliesItsLateResult() {
		auto fake = std::make_shared<Fixture>();
		setNativeDesktopFactoryForTest(
		    [fake](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return fake->acquire(m, v, p);
		    }
		);
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [] {}));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		QTRY_VERIFY_WITH_TIMEOUT(IsEqualGUID(nativeWindowDesktopId(window), fake->one.id), 500);
		fake->publicApi.beforeMove = [] {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		};
		QElapsedTimer elapsed;
		elapsed.start();
		const auto moved = moveNativeDesktopOwnWindow(window, fake->two.id);
		QVERIFY(moved.has_value() && *moved);
		QVERIFY(elapsed.elapsed() < 250);
		QVERIFY(nativeDesktopCommandPending());
		const auto busy = moveNativeDesktopOwnWindow(window, fake->one.id);
		QVERIFY(busy.has_value() && !*busy);
		QCOMPARE(nativeDesktopLastError(), HRESULT_FROM_WIN32(ERROR_BUSY));
		QTRY_VERIFY_WITH_TIMEOUT(IsEqualGUID(nativeWindowDesktopId(window), fake->two.id), 1000);
		QVERIFY(!nativeDesktopCommandPending());
		QCOMPARE(nativeDesktopWorkerCountForTest(), 1);
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
	}
	void blockedCallsAreBounded() {
		QFETCH(QString, operation);
		auto old = std::make_shared<Fixture>();
		auto fresh = std::make_shared<Fixture>();
		fresh->one.id.Data1 = 101;
		auto blocked = std::make_shared<BlockingCall>();
		std::atomic<int> acquisitions = 0;
		std::atomic<DWORD> apartmentThread = 0;
		setNativeDesktopFactoryForTest([&, old, fresh, blocked](
		                                   DesktopManager** m,
		                                   ViewCollection** v,
		                                   IVirtualDesktopManager** p
		                               ) {
			apartmentThread = GetCurrentThreadId();
			const auto first = ++acquisitions == 1;
			if (first && operation == "activation") (*blocked)();
			return (first ? old : fresh)->acquire(m, v, p);
		});
		setNativeDesktopStuckBoundForTest(50); // A genuine stall, with a fast test-only verdict.
		struct Reset {
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		Unblock unblock {blocked};
		QObject context;
		VirtualDesktops::Accessor accessor;
		QElapsedTimer elapsed;
		elapsed.start();
		const auto loaded = loadNativeDesktopAccessor(accessor, &context, [] {});
		QVERIFY2(
		    elapsed.elapsed() < 250,
		    "Activation exceeded the 100 ms wait plus scheduling allowance"
		);
		if (operation != "activation") QVERIFY(loaded);
		else QVERIFY(!loaded);
		QVERIFY(apartmentThread.load() != GetCurrentThreadId());
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		if (loaded) setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		const auto block = [blocked] { (*blocked)(); };
		if (operation == "enumeration") old->manager.beforeEnumeration = block;
		if (operation == "switch") old->manager.beforeSwitch = block;
		if (operation == "create") old->manager.beforeCreate = block;
		if (operation == "move") old->manager.beforeMove = block;
		if (operation == "window-query") old->publicApi.beforeQuery = block;
		if (operation == "own-window-move") old->publicApi.beforeMove = block;
		if (operation == "release") old->one.beforeRelease = block;
		elapsed.restart();
		if (operation == "enumeration" || operation == "release") {
			requestNativeDesktopRefresh();
			QVERIFY(elapsed.elapsed() < 50);
			QTRY_COMPARE_WITH_TIMEOUT(
			    nativeDesktopAvailability(),
			    DesktopAvailability::Disconnected,
			    250
			);
		} else if (operation == "window-query") {
			QVERIFY(IsEqualGUID(nativeWindowDesktopId(window), GUID {}));
			QVERIFY(elapsed.elapsed() < 50);
			QTRY_COMPARE_WITH_TIMEOUT(
			    nativeDesktopAvailability(),
			    DesktopAvailability::Disconnected,
			    250
			);
		} else if (operation == "switch") QCOMPARE(accessor.goToDesktopNumber(1), -1);
		else if (operation == "create") QCOMPARE(accessor.createDesktop(), -1);
		else if (operation == "move") QCOMPARE(accessor.moveWindowToDesktopNumber(window, 1), -1);
		else if (operation == "own-window-move") {
			const auto result = moveNativeDesktopOwnWindow(window, old->two.id);
			QVERIFY(result.has_value() && !*result);
		}
		QVERIFY(elapsed.elapsed() < 250);
		QCOMPARE(WaitForSingleObject(blocked->entered, 0), DWORD(WAIT_OBJECT_0));
		QCOMPARE(nativeDesktopLastError(), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		// GUI snapshots and invalidation never wait for the stuck COM owner.
		elapsed.restart();
		for (int i = 0; i < 100; ++i) {
			accessor.getDesktopCount();
			accessor.getCurrentDesktopNumber();
			accessor.getDesktopIdByNumber(0);
			(void) nativeWindowDesktopId(window);
			(void) nativeWindowOnCurrentDesktop(window);
		}
		QVERIFY(elapsed.elapsed() < 50);
		// The replacement succeeds while the previous apartment stays blocked.
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopAvailability(), DesktopAvailability::Available, 1500);
		QCOMPARE(acquisitions.load(), 2);
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), fresh->one.id));
		blocked->release();
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopWorkerCountForTest(), 1, 500);
		QCoreApplication::processEvents();
		// A late acquisition/refresh/command must not overwrite this generation.
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), fresh->one.id));
		QCOMPARE(fresh->manager.switches, 0);
		QCOMPARE(fresh->manager.creates, 0);
	}
	void shutdownDoesNotWaitForRelease() {
		auto old = std::make_shared<Fixture>();
		auto blocked = std::make_shared<BlockingCall>();
		setNativeDesktopFactoryForTest(
		    [old](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return old->acquire(m, v, p);
		    }
		);
		Unblock unblock {blocked};
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [] {}));
		old->manager.beforeRelease = [blocked] { (*blocked)(); };
		QElapsedTimer elapsed;
		elapsed.start();
		shutdownNativeDesktopAccessor();
		QVERIFY(elapsed.elapsed() < 50);
		QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(blocked->entered, 0), DWORD(WAIT_OBJECT_0), 200);
		blocked->release();
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopWorkerCountForTest(), 0, 500);
	}
	void retiredApartmentsHaveAGlobalCap() {
		auto blocked = std::make_shared<BlockingCall>();
		std::atomic<int> attempts = 0;
		setNativeDesktopFactoryForTest(
		    [blocked, &attempts](DesktopManager**, ViewCollection**, IVirtualDesktopManager**) {
			    ++attempts;
			    (*blocked)();
			    return E_FAIL;
		    }
		);
		setNativeDesktopStuckBoundForTest(50);
		Unblock unblock {blocked};
		QObject context;
		VirtualDesktops::Accessor accessor;
		for (int i = 0; i < 6; ++i) {
			QElapsedTimer elapsed;
			elapsed.start();
			QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
			QVERIFY(elapsed.elapsed() < 250);
			QVERIFY(nativeDesktopWorkerCountForTest() <= 4);
		}
		QCOMPARE(attempts.load(), 4);
		QCOMPARE(nativeDesktopLastError(), HRESULT_FROM_WIN32(ERROR_BUSY));
		blocked->release();
		shutdownNativeDesktopAccessor();
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopWorkerCountForTest(), 0, 1000);
	}
	void queuedCommandExpiresWithoutDispatch() {
		auto old = std::make_shared<Fixture>();
		auto blocked = std::make_shared<BlockingCall>();
		setNativeDesktopFactoryForTest(
		    [old](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return old->acquire(m, v, p);
		    }
		);
		Unblock unblock {blocked};
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [] {}));
		setNativeDesktopStuckBoundForTest(50);
		old->manager.beforeEnumeration = [blocked] { (*blocked)(); };
		requestNativeDesktopRefresh();
		QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(blocked->entered, 0), DWORD(WAIT_OBJECT_0), 100);
		QElapsedTimer elapsed;
		elapsed.start();
		QCOMPARE(accessor.goToDesktopNumber(1), -1);
		QVERIFY(elapsed.elapsed() < 250);
		blocked->release();
		shutdownNativeDesktopAccessor();
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopWorkerCountForTest(), 0, 500);
		QCOMPARE(old->manager.switches, 0);
	}
	void publicQueriesRemainAvailableOnUnknownBuild() {
		auto fake = std::make_shared<Fixture>();
		setNativeDesktopFactoryForTest(
		    [fake](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return fake->acquire(m, v, p);
		    }
		);
		testBuild = 99999;
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
		initializeNativeDesktopQueries(&context, [] {});
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		QVERIFY(IsEqualGUID(nativeWindowDesktopId(window), GUID {}));
		QTRY_VERIFY_WITH_TIMEOUT(IsEqualGUID(nativeWindowDesktopId(window), fake->one.id), 250);
		QCOMPARE(nativeWindowOnCurrentDesktop(window), 1);
		QCOMPARE(fake->manager.enumerations, 0);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::UnsupportedBuild);
		QVERIFY(!accessor.goToDesktopNumber);
	}
	void privateRefusalDoesNotDisablePublicWindowQueries() {
		auto fake = std::make_shared<Fixture>();
		fake->manager.result = E_NOINTERFACE;
		setNativeDesktopFactoryForTest(
		    [fake](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return fake->acquire(m, v, p);
		    }
		);
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		QVERIFY(IsEqualGUID(nativeWindowDesktopId(window), GUID {}));
		QTRY_VERIFY_WITH_TIMEOUT(IsEqualGUID(nativeWindowDesktopId(window), fake->one.id), 200);
		QCOMPARE(nativeWindowOnCurrentDesktop(window), 1);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::MissingProvider);
		const auto moved = moveNativeDesktopOwnWindow(window, fake->two.id);
		QVERIFY(moved.has_value() && *moved);
		fake->manager.result = S_OK;
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopAvailability(), DesktopAvailability::Available, 1500);
	}
	void boundedBackoffAndDispatchRetry() {
		qint64 age = 0;
		int attempts = 0, recovered = 0;
		bool online = false;
		DesktopRecovery recovery(
		    [&] {
			    ++attempts;
			    return online;
		    },
		    [&] { ++recovered; },
		    [&] { return age; }
		);
		recovery.disconnected();
		QCOMPARE(recovery.pendingDelay(), 250);
		for (int expected: {500, 1000, 2000, 4000, 4000}) {
			recovery.attempt();
			QCOMPARE(recovery.pendingDelay(), expected);
		}
		age = 29900;
		recovery.attempt();
		QCOMPARE(recovery.pendingDelay(), 100);
		age = 30000;
		QTest::qWait(150); // Timer expires without scheduling outside the budget.
		QCOMPARE(recovery.pendingDelay(), -1);
		const auto exhausted = attempts;
		QTest::qWait(100);
		QCOMPARE(attempts, exhausted);
		online = true;
		age = 0;
		recovery.retryOnDispatch();
		QCOMPARE(attempts, exhausted + 1);
		QCOMPARE(recovered, 1);
		QVERIFY(!recovery.active());
		QCOMPARE(recovery.pendingDelay(), -1);
	}
	void lossAfterExpiredWindowSchedulesAgain() {
		qint64 age = 0;
		DesktopRecovery recovery([] { return false; }, [] {}, [&] { return age; });
		recovery.disconnected();
		age = 30000;
		QTest::qWait(300);
		QCOMPARE(recovery.pendingDelay(), -1);
		age = 0;
		recovery.disconnected();
		QCOMPARE(recovery.pendingDelay(), 250);
	}
	void lossDuringActiveWindowKeepsBackoffAndDeadline() {
		qint64 age = 0;
		std::function<void()> reportLoss;
		DesktopRecovery recovery(
		    [&] {
			    reportLoss();
			    return false;
		    },
		    [] {},
		    [&] { return age; }
		);
		reportLoss = [&] { recovery.disconnected(); };
		recovery.disconnected();
		recovery.attempt();
		QCOMPARE(recovery.pendingDelay(), 500);
		recovery.disconnected();
		QCOMPARE(recovery.pendingDelay(), 500);
		age = 29900;
		recovery.attempt();
		QCOMPARE(recovery.pendingDelay(), 100);
		age = 30000;
		recovery.attempt();
		QCOMPARE(recovery.pendingDelay(), -1);
		age = 0;
		recovery.disconnected();
		QCOMPARE(recovery.pendingDelay(), 250);
	}
	void refusedThenGrantedAcquisitionRecovers() {
		FakeDesktop one(1);
		FakeManager manager;
		FakeViews views;
		FakePublic publicApi;
		manager.array.desktops = {&one};
		manager.current = &one;
		int acquisitions = 0;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    if (++acquisitions <= 2) return E_ACCESSDENIED;
			    manager.AddRef();
			    views.AddRef();
			    publicApi.AddRef();
			    *m = &manager;
			    *v = &views;
			    *p = &publicApi;
			    return S_OK;
		    }
		);
		struct Reset {
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		QObject context;
		int recovered = 0;
		VirtualDesktops::Accessor accessor;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; }));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::PermissionDenied);
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 3000);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
		QCOMPARE(accessor.getDesktopCount(), 1);
	}
	void explorerReturnAfterExpiredWindowRecovers() {
		FakeDesktop one(1);
		FakeManager manager;
		FakeViews views;
		FakePublic publicApi;
		manager.array.desktops = {&one};
		manager.current = &one;
		int acquisitions = 0;
		bool online = false;
		setNativeDesktopFactoryForTest(
		    [&](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    ++acquisitions;
			    if (!online) return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
			    manager.AddRef();
			    views.AddRef();
			    publicApi.AddRef();
			    *m = &manager;
			    *v = &views;
			    *p = &publicApi;
			    return S_OK;
		    }
		);
		struct Reset {
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		QObject context;
		int recovered = 0;
		VirtualDesktops::Accessor accessor;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; }));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		qint64 age = 0;
		setNativeDesktopRecoveryAgeForTest([&] { return age; });
		// The real timer still delivers the retry. Advancing only its elapsed
		// clock to the exact deadline must prevent another retry being scheduled.
		age = 30000;
		QTRY_VERIFY_WITH_TIMEOUT(acquisitions >= 2, 1000);
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), -1);
		const auto exhausted = acquisitions;
		QTest::qWait(500);
		QCOMPARE(acquisitions, exhausted);
		online = true;
		age = 0; // A new recovery window starts when Explorer returns.
		invalidateNativeDesktopAccessor();
		QCOMPARE(nativeDesktopRecoveryPendingDelayForTest(), 250);
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 2000);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
	}
};
QTEST_GUILESS_MAIN(NativeDesktopRecoveryTest)
#include "native_desktop_recovery_test.moc"
