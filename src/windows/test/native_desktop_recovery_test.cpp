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
		setNativeDesktopFactoryForTest(
		    [](DesktopManager**, ViewCollection**, IVirtualDesktopManager**) { return E_ACCESSDENIED; }
		);
		VirtualDesktops::Accessor accessor;
		QObject context;
		QVERIFY(!loadNativeDesktopAccessor(accessor, &context, [] {}));
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::PermissionDenied);
		QCOMPARE(nativeDesktopLastError(), E_ACCESSDENIED);
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
		      "broken-release"})
		{
			QTest::newRow(operation) << QString::fromLatin1(operation);
		}
	}
	void structuredExceptionInvalidates() {
		QFETCH(QString, operation);
		Fixture old, fresh;
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
			~Reset() { setNativeDesktopFactoryForTest({}); }
		} reset;
		QObject context;
		VirtualDesktops::Accessor accessor;
		QVERIFY(loadNativeDesktopAccessor(accessor, &context, [&] { ++recovered; }));
		if (operation == "enumerate") old.manager.faultOnEnumeration = true;
		if (operation == "desktop-id") old.one.faultOnId = true;
		if (operation == "switch" || operation == "broken-release") old.manager.faultOnSwitch = true;
		if (operation == "broken-release") old.manager.faultOnRelease = true;
		if (operation == "create") old.manager.faultOnCreate = true;
		if (operation == "window-id" || operation == "on-current") old.publicApi.faultOnRead = true;

		int result = 0;
		if (operation == "enumerate" || operation == "desktop-id") result = accessor.getDesktopCount();
		else if (operation == "create") result = accessor.createDesktop();
		else if (operation == "window-id") result = accessor.getWindowDesktopNumber(nullptr);
		else if (operation == "on-current") result = accessor.isWindowOnCurrentVirtualDesktop(nullptr);
		else result = accessor.goToDesktopNumber(1);
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
		const auto enumerations = old.manager.enumerations;
		const auto switches = old.manager.switches;
		QVERIFY(!retryNativeDesktopAccessor());
		QCOMPARE(accessor.goToDesktopNumber(1), -1);
		QCOMPARE(accessor.getDesktopCount(), -1);
		QCOMPARE(old.manager.enumerations, enumerations);
		QCOMPARE(old.manager.switches, switches);
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), GUID {}));
		QCOMPARE(accessor.getCurrentDesktopNumber(), -1);
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
		QCOMPARE(manager.enumerations, before + 1); // One array per refresh.
		manager.result = failure;
		online = false;
		QCOMPARE(accessor.getDesktopCount(), -1);
		QCOMPARE(nativeDesktopAvailability(), static_cast<DesktopAvailability>(availability));
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), GUID {}));
		QCOMPARE(accessor.getCurrentDesktopNumber(), -1);
		QCOMPARE(manager.refs, 1UL);
		QCOMPARE(views.refs, 1UL);
		QCOMPARE(publicApi.refs, 1UL);
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
		QVERIFY(IsEqualGUID(accessor.getDesktopIdByNumber(0), GUID {}));
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 2, 1500);
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
		QTest::qWait(31000);
		const auto exhausted = acquisitions;
		QTest::qWait(500);
		QCOMPARE(acquisitions, exhausted);
		online = true;
		invalidateNativeDesktopAccessor();
		QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 2000);
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Available);
	}
};
QTEST_GUILESS_MAIN(NativeDesktopRecoveryTest)
#include "native_desktop_recovery_test.moc"
