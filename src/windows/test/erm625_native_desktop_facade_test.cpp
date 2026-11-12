#include <qelapsedtimer.h>
#include <qsignalspy.h>
#include <qtest.h>
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
		initializeNativeDesktopQueries(&desktops, [&desktops] { desktops.applyNativeSnapshot(); });
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
		this->old.clearFaults();
		this->fresh.clearFaults();
		setNativeDesktopFactoryForTest({});
		if (this->desktops) this->desktops->destroyForTest();
		this->desktops = nullptr;
		this->old.manager.switches = this->fresh.manager.switches = 0;
		this->old.manager.current = &this->old.one;
		this->fresh.manager.current = &this->fresh.one;
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
		QTRY_VERIFY_WITH_TIMEOUT(this->desktops->accessorLoaded(), 1500);
		QCOMPARE(this->acquisitions, 2);
		this->fresh.manager.current = &this->fresh.two;
		QVERIFY(this->desktops->switchTo(1));
		QCOMPARE(this->fresh.manager.switches, 1);
		QCOMPARE(this->old.manager.switches, 1);
		QTRY_COMPARE(
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
		QTRY_VERIFY_WITH_TIMEOUT(this->desktops->accessorLoaded(), 1500);
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
		QTRY_VERIFY_WITH_TIMEOUT(!this->desktops->accessorLoaded(), 200);
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
		QTRY_VERIFY_WITH_TIMEOUT(this->desktops->accessorLoaded(), 1500);
		QCOMPARE(results.count(), 2); // Recovery never resurrects/replays the request.
	}
	void blockedCommandReportsTimeoutAndNeverReplays_data() {
		QTest::addColumn<QString>("operation");
		for (const auto* operation: {"switch", "create", "move"})
			QTest::newRow(operation) << QString::fromLatin1(operation);
	}
	void blockedCommandReportsTimeoutAndNeverReplays() {
		QFETCH(QString, operation);
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
		QTRY_VERIFY_WITH_TIMEOUT(this->desktops->accessorLoaded(), 1500);
		QCOMPARE(results.count(), 1);
		QCOMPARE(this->fresh.manager.switches, 0);
		QCOMPARE(this->fresh.manager.creates, 0);
		QCOMPARE(this->fresh.manager.moves, 0);
		blocked->release();
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopWorkerCountForTest(), 1, 500);
		QCOMPARE(results.count(), 1);
	}
};
QTEST_GUILESS_MAIN(Erm625NativeDesktopFacadeTest)
#include "erm625_native_desktop_facade_test.moc"
