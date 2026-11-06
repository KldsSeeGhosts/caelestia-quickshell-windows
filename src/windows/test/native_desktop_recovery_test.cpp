#include <qtest.h>

#include "desktop_recovery.hpp"
#include "native_desktop_accessor.hpp"
#include "native_desktop_interfaces.hpp"

using namespace qs::windows;
using namespace qs::windows::native_desktop;
namespace qs::windows {
DWORD testBuild = 26100;
DWORD windowsBuild() { return testBuild; }
} // namespace qs::windows

namespace {
template <typename Interface>
struct FakeCom: Interface {
	ULONG refs = 1;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
	ULONG STDMETHODCALLTYPE Release() override { return --this->refs; }
};
struct FakeDesktop: FakeCom<Desktop> {
	GUID id {};
	explicit FakeDesktop(ULONG value) { this->id.Data1 = value; }
	HRESULT STDMETHODCALLTYPE IsViewVisible(ApplicationView*, BOOL*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetId(GUID* out) override {
		*out = this->id;
		return S_OK;
	}
};
struct FakeArray: FakeCom<IObjectArray> {
	QList<FakeDesktop*> desktops;
	HRESULT STDMETHODCALLTYPE GetCount(UINT* out) override {
		*out = static_cast<UINT>(this->desktops.size());
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetAt(UINT index, REFIID, void** out) override {
		if (index >= this->desktops.size()) return E_INVALIDARG;
		auto* desktop = this->desktops[index];
		desktop->AddRef();
		*out = desktop;
		return S_OK;
	}
};
struct FakeManager: FakeCom<DesktopManager> {
	FakeArray array;
	HRESULT result = S_OK;
	int enumerations = 0;
	int switches = 0;
	FakeDesktop* current = nullptr;
	HRESULT STDMETHODCALLTYPE GetCount(UINT* out) override { return this->array.GetCount(out); }
	HRESULT STDMETHODCALLTYPE MoveViewToDesktop(ApplicationView*, Desktop*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(ApplicationView*, BOOL*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetCurrentDesktop(Desktop** out) override {
		this->current->AddRef();
		*out = this->current;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray** out) override {
		++this->enumerations;
		if (FAILED(this->result) || this->result == RPC_S_SERVER_UNAVAILABLE) return this->result;
		this->array.AddRef();
		*out = &this->array;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(Desktop*, int, Desktop**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE SwitchDesktop(Desktop*) override {
		++this->switches;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(Desktop*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE CreateDesktop(Desktop**) override { return E_NOTIMPL; }
};
struct FakeViews: FakeCom<ViewCollection> {
	HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, ApplicationView**) override { return E_NOTIMPL; }
};
struct FakePublic: FakeCom<IVirtualDesktopManager> {
	HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND, BOOL*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND, GUID*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND, REFGUID) override { return E_NOTIMPL; }
};
} // namespace

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
		QTest::newRow("RPC_E_DISCONNECTED") << qint32(RPC_E_DISCONNECTED)
		    << int(DesktopAvailability::Disconnected);
		QTest::newRow("RPC_S_SERVER_UNAVAILABLE")
		    << qint32(HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
		    << int(DesktopAvailability::Disconnected);
		QTest::newRow("CO_E_OBJNOTCONNECTED") << qint32(CO_E_OBJNOTCONNECTED)
		    << int(DesktopAvailability::Disconnected);
		QTest::newRow("E_ACCESSDENIED") << qint32(E_ACCESSDENIED)
		    << int(DesktopAvailability::PermissionDenied);
		QTest::newRow("E_FAIL") << qint32(E_FAIL)
		    << int(DesktopAvailability::MissingProvider);
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
