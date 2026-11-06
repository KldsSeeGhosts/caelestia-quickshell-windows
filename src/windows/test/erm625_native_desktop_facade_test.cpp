// Preload dependencies before the narrow white-box access below. The existing
// public test constructor deliberately skips native selection; select that
// branch without extending the production facade or touching the live desktop.
#include <functional>
#include <optional>

#include <qlist.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "accessor_state.hpp"
#include "desktop_contract.hpp"
#define private public
#include "virtual_desktops.hpp"
#undef private

#include <qsignalspy.h>
#include <qtest.h>

#include "erm625_desktop_fakes.hpp"
#include "native_desktop_accessor.hpp"

using namespace qs::windows;
using namespace qs::windows::native_desktop;
using namespace erm625;

namespace qs::windows {
DWORD windowsBuild() { return 26100; }
} // namespace qs::windows
namespace qs::windows::startup {
void afterFirstFrame(QObject*, std::function<void()>) {}
} // namespace qs::windows::startup

class Erm625NativeDesktopFacadeTest: public QObject {
	Q_OBJECT;
	Fixture old, fresh;
	VirtualDesktops* desktops = nullptr;
	VirtualDesktops::Accessor accessor;
	int acquisitions = 0;

	void install() {
		this->desktops->installAccessorForTest(this->accessor);
		// Match the native provider's disconnected reason when unloaded. During
		// operations testMode keeps refresh on the injected model, not registry.
		this->desktops->testAvailability = DesktopAvailability::Disconnected;
	}

	void verifyUnavailable() {
		QVERIFY(!this->desktops->accessorLoaded());
		QCOMPARE(nativeDesktopAvailability(), DesktopAvailability::Disconnected);
		// Exercise the real native capability projection too, without calling
		// refresh (which would read the live registry outside testMode).
		this->desktops->testMode = false;
		const auto capability = this->desktops->capability();
		this->desktops->testMode = true;
		QCOMPARE(capability.value("availability").toString(), QStringLiteral("unavailable"));
		QCOMPARE(capability.value("reason").toString(), QStringLiteral("provider-disconnected"));
		QVERIFY(!capability.value("operations").toMap().value("switch").toBool());
	}

private slots:
	void init() {
		qputenv("CAELESTIA_WINDOWS_HYPR", "1");
		qputenv("CAELESTIA_WINDOWS_HYPR_DISPATCH", "1");
		this->acquisitions = 0;
		VirtualDesktops::TestHooks hooks;
		hooks.desktops = {{.id = this->old.one.id}, {.id = this->old.two.id}};
		this->desktops = VirtualDesktops::createForTest(std::move(hooks));
		this->desktops->nativeAccessor = true;
		setNativeDesktopFactoryForTest(
		    [this](DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
			    return (++this->acquisitions == 1 ? this->old : this->fresh).acquire(m, v, p);
		    }
		);
		QVERIFY(loadNativeDesktopAccessor(this->accessor, this->desktops, [this] { this->install(); }));
		this->install();
		QCoreApplication::processEvents();
	}
	void cleanup() {
		setNativeDesktopFactoryForTest({});
		this->desktops->destroyForTest();
		this->desktops = nullptr;
		QCOMPARE(this->old.manager.refs, 1UL);
		QCOMPARE(this->old.one.refs, 1UL);
		QCOMPARE(this->fresh.manager.refs, 1UL);
		QCOMPARE(this->fresh.one.refs, 1UL);
		this->old.manager.faultOnSwitch = false;
		this->old.manager.faultOnEnumeration = false;
		this->old.manager.switches = this->fresh.manager.switches = 0;
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
		QCOMPARE(this->desktops->lastCommand().value("outcome").toString(), QStringLiteral("observed"));
	}
	void pendingOperationsFailOnReadFault_data() {
		QTest::addColumn<bool>("creation");
		QTest::newRow("pending-switch") << false;
		QTest::newRow("pending-create") << true;
	}
	void pendingOperationsFailOnReadFault() {
		QFETCH(bool, creation);
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
		this->old.manager.faultOnEnumeration = true;
		this->desktops->refresh();
		this->verifyUnavailable();
		QCoreApplication::processEvents();
		QCOMPARE(results.count(), 2);
		const auto terminal = results.last().at(0).toMap();
		QCOMPARE(terminal.value("requestId"), accepted.value("requestId"));
		QCOMPARE(terminal.value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(terminal.value("reason").toString(), QStringLiteral("provider-disconnected"));
		QCOMPARE(completions, creation ? 1 : 0);
		QVERIFY(!this->desktops->pendingSwitch);
		QVERIFY(this->desktops->creationWaiters.isEmpty());
		QVERIFY(!this->desktops->switchDeadline.isActive());
		QVERIFY(!this->desktops->creationTimer.isActive());
		QTRY_VERIFY_WITH_TIMEOUT(this->desktops->accessorLoaded(), 1500);
		QCOMPARE(results.count(), 2); // Recovery never resurrects/replays the request.
	}
};
QTEST_GUILESS_MAIN(Erm625NativeDesktopFacadeTest)
#include "erm625_native_desktop_facade_test.moc"
