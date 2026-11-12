#include <atomic>
#include <functional>

#include <qelapsedtimer.h>
#include <qqmlcomponent.h>
#include <qqmlcontext.h>
#include <qqmlengine.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "erm625_desktop_fakes.hpp"
#include "hyprland/pin_support.hpp"
#include "native_desktop_accessor.hpp"
#include "virtual_desktops.hpp"

using namespace qs::windows;

// The test builds virtual_desktops.cpp without the rest of the backend. These
// are the only two production dependencies of the translation unit.
namespace qs::windows::startup {
void afterFirstFrame(QObject*, std::function<void()>) {}
} // namespace qs::windows::startup

namespace qs::windows {
DWORD windowsBuild() { return 22631; }
struct NativeDesktopFacadeTestHook {
	static void observe(VirtualDesktops& desktops) {
		initializeNativeDesktopQueries(&desktops, [&desktops] { desktops.applyNativeSnapshot(); });
	}
};
} // namespace qs::windows

namespace {

constexpr GUID REAL_DESKTOP_ONE = {
    0x11111111,
    0x2222,
    0x3333,
    {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}
};

constexpr GUID REAL_DESKTOP_TWO = {
    0xaaaaaaaa,
    0xbbbb,
    0xcccc,
    {0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33, 0x44}
};

struct FakeState {
	int count = 2;
	int current = 0;
	int switchCalls = 0;
	int lastSwitch = -1;
	int createCalls = 0;
	int currentCalls = 0;
	bool publishCreation = true;
	bool publishSwitch = true;
	bool rejectSwitch = false;
	bool rejectCreation = false;
};

FakeState fake;

void resetFake() { fake = FakeState(); }

int fakeDesktopCount() { return fake.count; }
int fakeCurrentDesktop() {
	fake.currentCalls++;
	return fake.current;
}
int fakeGoToDesktop(int index) {
	fake.switchCalls++;
	fake.lastSwitch = index;
	if (fake.publishSwitch) fake.current = index;
	return fake.rejectSwitch ? -1 : 0;
}
int fakeCreateDesktop() {
	fake.createCalls++;
	if (fake.rejectCreation) return -1;
	if (fake.publishCreation) fake.count++;
	return fake.count;
}
int fakeIsPinned(HWND) { return 1; }
int fakePin(HWND) { return 0; }

VirtualDesktops::Accessor fakeAccessor(bool withCreate = true) {
	VirtualDesktops::Accessor accessor;
	accessor.getCurrentDesktopNumber = &fakeCurrentDesktop;
	accessor.getDesktopCount = &fakeDesktopCount;
	accessor.goToDesktopNumber = &fakeGoToDesktop;
	accessor.createDesktop = withCreate ? &fakeCreateDesktop : nullptr;
	return accessor;
}

VirtualDesktops::TestHooks hooks(VirtualDesktops::ChordSender chordSender = {}) {
	VirtualDesktops::TestHooks hooks;
	hooks.chordSender = std::move(chordSender);
	hooks.desktops = {
	    {.id = {}, .name = QStringLiteral("one")},
	    {.id = {}, .name = QStringLiteral("two")},
	};
	hooks.current = 0;
	return hooks;
}

} // namespace

class VirtualDesktopsTest: public QObject {
	Q_OBJECT;

private:
	VirtualDesktops* desktops = nullptr;
	int chords = 0;

	VirtualDesktops* make(VirtualDesktops::TestHooks hooks) {
		this->desktops = VirtualDesktops::createForTest(std::move(hooks));
		return this->desktops;
	}

	VirtualDesktops::ChordSender chordRecorder() {
		return [this](WORD, int) { this->chords++; };
	}

private slots:
	void init() {
		resetFake();
		this->chords = 0;
		this->desktops = nullptr;
	}

	void cleanup() {
		setNativeDesktopFactoryForTest({});
		if (this->desktops != nullptr) {
			this->desktops->destroyForTest();
			this->desktops = nullptr;
		}
	}

	void windowReadsStayCachedWhilePublicProviderBlocksAndRecovers() {
		auto old = std::make_shared<erm625::Fixture>();
		auto fresh = std::make_shared<erm625::Fixture>();
		old->publicApi.desktopId = REAL_DESKTOP_ONE;
		old->publicApi.onCurrent = FALSE;
		fresh->publicApi.desktopId = REAL_DESKTOP_TWO;
		auto acquisitions = std::make_shared<std::atomic<int>>(0);
		setNativeDesktopFactoryForTest([old, fresh, acquisitions](
		                                   native_desktop::DesktopManager** m,
		                                   native_desktop::ViewCollection** v,
		                                   IVirtualDesktopManager** p
		                               ) {
			return (++*acquisitions == 1 ? old : fresh)->acquire(m, v, p);
		});
		auto seed = hooks();
		seed.desktops[0].id = REAL_DESKTOP_ONE;
		seed.desktops[1].id = REAL_DESKTOP_TWO;
		auto* desktops = this->make(seed);
		NativeDesktopFacadeTestHook::observe(*desktops);
		setNativeDesktopWindowValidatorForTest([](HWND) { return true; });
		QSignalSpy membership(desktops, &VirtualDesktops::windowDesktopsChanged);
		const auto window = reinterpret_cast<HWND>(ULONG_PTR(0xb2));
		QCOMPARE(desktops->windowDesktopIndex(window), qsizetype(-1));
		QVERIFY(desktops->isWindowOnCurrent(window)); // Existing unknown-window fallback.
		QTRY_COMPARE_WITH_TIMEOUT(desktops->windowDesktopIndex(window), qsizetype(0), 250);
		QVERIFY(!desktops->isWindowOnCurrent(window));
		auto blocked = std::make_shared<erm625::BlockingCall>();
		erm625::Unblock unblock {blocked};
		old->publicApi.beforeQuery = [blocked] { (*blocked)(); };
		requestNativeDesktopRefresh();
		QElapsedTimer elapsed;
		elapsed.start();
		for (int i = 0; i < 100; ++i) {
			QVERIFY(IsEqualGUID(desktops->windowDesktopId(window), REAL_DESKTOP_ONE));
			QCOMPARE(desktops->windowDesktopIndex(window), qsizetype(0));
			QVERIFY(!desktops->isWindowOnCurrent(window));
		}
		QVERIFY(elapsed.elapsed() < 50);
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopAvailability(), DesktopAvailability::Disconnected, 250);
		QVERIFY(IsEqualGUID(desktops->windowDesktopId(window), REAL_DESKTOP_ONE));
		QTRY_COMPARE_WITH_TIMEOUT(desktops->windowDesktopIndex(window), qsizetype(1), 1500);
		QVERIFY(desktops->isWindowOnCurrent(window));
		QVERIFY(membership.count() > 0);
		QCOMPARE(acquisitions->load(), 2);
		blocked->release();
		QTRY_COMPARE_WITH_TIMEOUT(nativeDesktopWorkerCountForTest(), 1, 500);
		QVERIFY(IsEqualGUID(desktops->windowDesktopId(window), REAL_DESKTOP_TWO));
	}

	void placeholderOnlyDesktopIsAddressable() {
		VirtualDesktops::TestHooks seed;
		seed.desktops = {{.id = {}, .name = QStringLiteral("Desktop 1")}};
		auto* desktops = this->make(seed);

		QCOMPARE(desktops->count(), qsizetype(1));
		// The single-desktop fallback has no GUID; its placeholder is desktop 0.
		QCOMPARE(desktops->indexOf(GUID {}), qsizetype(0));
		QCOMPARE(desktops->indexOf(REAL_DESKTOP_ONE), qsizetype(-1));
	}

	void nullGuidWithRealDesktopsIsNotFound() {
		VirtualDesktops::TestHooks seed;
		seed.desktops = {
		    {.id = REAL_DESKTOP_ONE, .name = QStringLiteral("one")},
		    {.id = REAL_DESKTOP_TWO, .name = QStringLiteral("two")},
		};
		auto* desktops = this->make(seed);

		QCOMPARE(desktops->indexOf(REAL_DESKTOP_ONE), qsizetype(0));
		QCOMPARE(desktops->indexOf(REAL_DESKTOP_TWO), qsizetype(1));
		// A failed lookup stays unaddressable when real desktop ids exist.
		QCOMPARE(desktops->indexOf(GUID {}), qsizetype(-1));
	}

	void nullGuidWithMultiplePlaceholdersIsNotFound() {
		auto* desktops = this->make(hooks());

		QCOMPARE(desktops->count(), qsizetype(2));
		QCOMPARE(desktops->indexOf(GUID {}), qsizetype(-1));
	}

	void capabilityStartsUnavailable() {
		auto* desktops = this->make(hooks());
		QVERIFY(!desktops->desktopControlSupported());
	}

	void availabilityStates_data() {
		QTest::addColumn<int>("state");
		QTest::addColumn<QString>("availability");
		QTest::addColumn<QString>("reason");
		QTest::newRow("unknown-build") << int(DesktopAvailability::UnsupportedBuild) << QStringLiteral("unavailable") << QStringLiteral("unsupported-build");
		QTest::newRow("disconnected") << int(DesktopAvailability::Disconnected) << QStringLiteral("unavailable") << QStringLiteral("provider-disconnected");
		QTest::newRow("disabled") << int(DesktopAvailability::Disabled) << QStringLiteral("disabled") << QStringLiteral("dispatch-disabled");
		QTest::newRow("denied") << int(DesktopAvailability::PermissionDenied) << QStringLiteral("denied") << QStringLiteral("permission-denied");
		QTest::newRow("missing") << int(DesktopAvailability::MissingProvider) << QStringLiteral("unavailable") << QStringLiteral("provider-unavailable");
	}

	void availabilityStates() {
		QFETCH(int, state);
		QFETCH(QString, availability);
		QFETCH(QString, reason);
		auto seed = hooks();
		seed.availability = static_cast<DesktopAvailability>(state);
		auto* desktops = this->make(seed);
		const auto capability = desktops->capability();
		QCOMPARE(capability.value("availability").toString(), availability);
		QCOMPARE(capability.value("reason").toString(), reason);
		QCOMPARE(capability.value("provider").toString(), QStringLiteral("build-gated-private-com"));
		QVERIFY(!capability.value("operations").toMap().value("switch").toBool());
		QVERIFY(!desktops->switchTo(1));
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(desktops->lastCommand().value("reason").toString(), reason);
		QVERIFY(!desktops->lastCommand().contains("requestId"));
	}

	void healthyEmptyAndPerOperationSupport() {
		auto seed = hooks();
		seed.desktops.clear();
		auto* desktops = this->make(seed);
		desktops->installAccessorForTest(fakeAccessor(false));
		fake.count = 0;
		desktops->refresh();
		QCOMPARE(desktops->count(), qsizetype(0));
		QCOMPARE(desktops->currentIndex(), qsizetype(-1));
		QCOMPARE(desktops->capability().value("availability").toString(), QStringLiteral("available"));
		const auto operations = desktops->capability().value("operations").toMap();
		QVERIFY(operations.value("switch").toBool());
		QVERIFY(!operations.value("create").toBool());
		QVERIFY(!operations.value("remove").toBool());
	}

	void capabilityAndCommandAreVisibleToQml() {
		auto* desktops = this->make(hooks());
		QQmlEngine engine;
		engine.rootContext()->setContextProperty(QStringLiteral("nativeDesktop"), desktops);
		QQmlComponent component(&engine);
		component.setData(R"(
import QtQml
QtObject {
    property var capability: nativeDesktop.capability
    property var result: nativeDesktop.lastCommand
}
)", QUrl());
		QScopedPointer<QObject> qml(component.create());
		QVERIFY2(qml, qPrintable(component.errorString()));
		QCOMPARE(qml->property("capability").toMap().value("availability").toString(), QStringLiteral("unavailable"));
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(qml->property("capability").toMap().value("availability").toString(), QStringLiteral("available"));
		QVERIFY(desktops->switchTo(1));
		QCOMPARE(qml->property("result").toMap().value("outcome").toString(), QStringLiteral("observed"));
		desktops->loseAccessorForTest();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(qml->property("capability").toMap().value("reason").toString(), QStringLiteral("provider-disconnected"));
	}

	void commandAcceptedThenObserved() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishSwitch = false;
		QSignalSpy results(desktops, &VirtualDesktops::commandResult);
		QVERIFY(desktops->switchTo(1));
		const auto accepted = desktops->lastCommand();
		QCOMPARE(accepted.value("outcome").toString(), QStringLiteral("accepted"));
		QVERIFY(accepted.contains("requestId"));
		fake.current = 1;
		desktops->refresh();
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("observed"));
		QCOMPARE(desktops->lastCommand().value("requestId"), accepted.value("requestId"));
		QCOMPARE(results.count(), 2);
	}

	void rejectedCommandIsNotAccepted() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishSwitch = false;
		fake.rejectSwitch = true;
		QVERIFY(!desktops->switchTo(1));
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(desktops->lastCommand().value("reason").toString(), QStringLiteral("command-rejected"));
		QVERIFY(!desktops->lastCommand().contains("requestId"));
		QCOMPARE(desktops->currentIndex(), qsizetype(0));
	}

	void acceptedCommandTimesOutWithoutObservation() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishSwitch = false;
		QVERIFY(desktops->switchTo(1));
		QTRY_COMPARE_WITH_TIMEOUT(desktops->lastCommand().value("outcome").toString(), QStringLiteral("timed-out"), 2500);
		QCOMPARE(desktops->currentIndex(), qsizetype(0));
	}

	void newerRequestSupersedesPendingSwitch() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishSwitch = false;
		QVERIFY(desktops->switchTo(1));
		const auto firstId = desktops->lastCommand().value("requestId");
		QSignalSpy results(desktops, &VirtualDesktops::commandResult);
		QVERIFY(desktops->switchTo(0));
		QCOMPARE(results.at(0).at(0).toMap().value("outcome").toString(), QStringLiteral("superseded"));
		QCOMPARE(results.at(0).at(0).toMap().value("requestId"), firstId);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("observed"));
		QVERIFY(desktops->lastCommand().value("requestId") != firstId);
	}

	void lossDeniesPendingSwitch() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		fake.publishSwitch = false;
		QVERIFY(desktops->switchTo(1));
		const auto id = desktops->lastCommand().value("requestId");
		desktops->loseAccessorForTest();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(desktops->lastCommand().value("reason").toString(), QStringLiteral("provider-disconnected"));
		QCOMPARE(desktops->lastCommand().value("requestId"), id);
	}

	void reentrantObserverKeepsRequestIdentity() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishSwitch = false;
		QSignalSpy results(desktops, &VirtualDesktops::commandResult);
		QObject::connect(desktops, &VirtualDesktops::lastCommandChanged, desktops, [desktops] {
			if (desktops->lastCommand().value("outcome").toString() == QStringLiteral("accepted"))
				desktops->switchTo(0);
		});
		QVERIFY(desktops->switchTo(1));
		QCOMPARE(results.count(), 3);
		QCOMPARE(results.at(0).at(0).toMap().value("outcome").toString(), QStringLiteral("accepted"));
		QCOMPARE(results.at(1).at(0).toMap().value("outcome").toString(), QStringLiteral("superseded"));
		QCOMPARE(results.at(2).at(0).toMap().value("outcome").toString(), QStringLiteral("observed"));
		QCOMPARE(results.at(0).at(0).toMap().value("requestId"),
		         results.at(1).at(0).toMap().value("requestId"));
		QVERIFY(results.at(0).at(0).toMap().value("requestId")
		        != results.at(2).at(0).toMap().value("requestId"));
	}

	void creationRejectionNeverClaimsAcceptance() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.rejectCreation = true;
		bool called = false;
		QSignalSpy results(desktops, &VirtualDesktops::commandResult);
		desktops->ensureCount(3, [&](bool ok) { called = true; QVERIFY(!ok); });
		QVERIFY(called);
		QCOMPARE(results.count(), 1);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
	}

	void creationCompletesFromModelEvent() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishCreation = false;
		bool called = false;
		desktops->ensureCount(3, [&](bool ok) { called = true; QVERIFY(ok); });
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("accepted"));
		const auto id = desktops->lastCommand().value("requestId");
		fake.count = 3;
		desktops->refresh();
		QTRY_VERIFY(called);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("observed"));
		QCOMPARE(desktops->lastCommand().value("requestId"), id);
		QCOMPARE(fake.createCalls, 1);
	}

	void creationTimesOutWithoutObservation() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishCreation = false;
		bool called = false;
		desktops->ensureCount(3, [&](bool ok) { called = true; QVERIFY(!ok); });
		QTRY_VERIFY_WITH_TIMEOUT(called, 2000);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("timed-out"));
		QCOMPARE(fake.createCalls, 1);
	}

	void switchWithoutAccessorRefusesWithoutChord() {
		auto* desktops = this->make(hooks(this->chordRecorder()));
		QVERIFY(!desktops->desktopControlSupported());

		QVERIFY(!desktops->switchTo(1));
		// Even a no-op request is a refused operation while control is absent.
		QVERIFY(!desktops->switchTo(0));
		QCOMPARE(this->chords, 0);
		QCOMPARE(fake.switchCalls, 0);
	}

	void ensureWithoutAccessorCompletesFalseWithoutChord() {
		auto* desktops = this->make(hooks(this->chordRecorder()));

		auto called = false;
		auto value = true;
		desktops->ensureCount(3, [&](bool ok) {
			called = true;
			value = ok;
		});

		QVERIFY(called);
		QVERIFY(!value);
		QCOMPARE(desktops->count(), qsizetype(2));
		QCOMPARE(this->chords, 0);
		QCOMPARE(fake.createCalls, 0);
	}

	void ensureWithoutAccessorRefusesEvenWhenCountIsSufficient() {
		auto* desktops = this->make(hooks(this->chordRecorder()));

		auto called = false;
		auto value = true;
		desktops->ensureCount(1, [&](bool ok) {
			called = true;
			value = ok;
		});

		QVERIFY(called);
		QVERIFY(!value);
		QCOMPARE(this->chords, 0);
	}

	void creationWithoutCreateExportCompletesFalseWithoutChord() {
		auto* desktops = this->make(hooks(this->chordRecorder()));
		desktops->installAccessorForTest(fakeAccessor(false));

		auto called = false;
		auto value = true;
		desktops->ensureCount(3, [&](bool ok) {
			called = true;
			value = ok;
		});

		QVERIFY(called);
		QVERIFY(!value);
		QCOMPARE(this->chords, 0);
		QCOMPARE(fake.createCalls, 0);
	}

	void capabilityNotifiesOnLoadAndLoss() {
		auto* desktops = this->make(hooks());
		qs::hyprland::ipc::PinSupport observer(desktops);
		QSignalSpy changed(desktops, &VirtualDesktops::accessorStateChanged);
		QSignalSpy controlChanged(&observer, &qs::hyprland::ipc::PinSupport::desktopControlSupportedChanged);
		QSignalSpy pinChanged(&observer, &qs::hyprland::ipc::PinSupport::pinSupportedChanged);
		QVERIFY(!desktops->desktopControlSupported());

		desktops->installAccessorForTest(fakeAccessor());
		QVERIFY(desktops->desktopControlSupported());
		QVERIFY(observer.desktopControlSupported());
		QCOMPARE(changed.count(), 0); // Load notifications must be deferred.
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(changed.count(), 1);
		QCOMPARE(controlChanged.count(), 1);
		QCOMPARE(pinChanged.count(), 1);

		// Installing the same state again is not a capability transition.
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(changed.count(), 1);

		// Mirrors the crash fallback that drops the accessor.
		desktops->loseAccessorForTest();
		QVERIFY(!desktops->desktopControlSupported());
		QCOMPARE(changed.count(), 1); // Loss can be discovered inside a read.
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(changed.count(), 2);
		QCOMPARE(controlChanged.count(), 2);
		QCOMPARE(pinChanged.count(), 2);
		QVERIFY(!observer.desktopControlSupported());

		desktops->loseAccessorForTest();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(changed.count(), 2);
	}

	void optionalExports_data() {
		QTest::addColumn<int>("exports");
		for (int mask = 0; mask < 32; mask++) {
			QTest::newRow(qPrintable(QString::number(mask))) << mask;
		}
	}

	void optionalExports() {
		QFETCH(int, exports);
		auto* desktops = this->make(hooks(this->chordRecorder()));
		auto accessor = fakeAccessor();
		accessor.goToDesktopNumber = exports & 1 ? &fakeGoToDesktop : nullptr;
		accessor.createDesktop = exports & 2 ? &fakeCreateDesktop : nullptr;
		accessor.isPinnedWindow = exports & 4 ? &fakeIsPinned : nullptr;
		accessor.pinWindow = exports & 8 ? &fakePin : nullptr;
		accessor.unPinWindow = exports & 16 ? &fakePin : nullptr;
		desktops->installAccessorForTest(accessor);
		qs::hyprland::ipc::PinSupport observer(desktops);
		QCOMPARE(desktops->desktopControlSupported(), (exports & 3) == 3);
		QCOMPARE(observer.desktopControlSupported(), (exports & 3) == 3);
		auto pinSupported = (exports & 28) == 28;
		QCOMPARE(desktops->pinSupported(), pinSupported);
		QCOMPARE(observer.pinSupported(), pinSupported);
		auto window = reinterpret_cast<HWND>(quintptr(1));
		QCOMPARE(desktops->isWindowPinned(window), pinSupported);
		QCOMPARE(
		    qs::hyprland::ipc::dispatchPin(*desktops, window),
		    pinSupported ? qs::hyprland::ipc::PinDispatchResult::Succeeded
		                 : qs::hyprland::ipc::PinDispatchResult::Unsupported
		);
		QCOMPARE(desktops->switchTo(1), bool(exports & 1));
		QCOMPARE(fake.switchCalls, exports & 1 ? 1 : 0);
		QCOMPARE(this->chords, 0);
		desktops->loseAccessorForTest();
		QVERIFY(!desktops->pinSupported());
		QVERIFY(!desktops->desktopControlSupported());
		QVERIFY(!desktops->isWindowPinned(window));
	}

	void exportChangesNotifyBothCapabilitiesThroughSharedState() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		qs::hyprland::ipc::PinSupport observer(desktops);
		QSignalSpy controlChanged(&observer, &qs::hyprland::ipc::PinSupport::desktopControlSupportedChanged);
		QSignalSpy pinChanged(&observer, &qs::hyprland::ipc::PinSupport::pinSupportedChanged);
		desktops->installAccessorForTest(fakeAccessor(false));
		QVERIFY(!observer.desktopControlSupported());
		QCOMPARE(controlChanged.count(), 0);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(controlChanged.count(), 1);
		QCOMPARE(pinChanged.count(), 1);
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QVERIFY(observer.desktopControlSupported());
		QCOMPARE(controlChanged.count(), 2);
		QCOMPARE(pinChanged.count(), 2);
	}

	void lossDuringCreationCompletesAllWaitersFalse() {
		auto* desktops = this->make(hooks(this->chordRecorder()));
		desktops->installAccessorForTest(fakeAccessor());
		fake.publishCreation = false;
		int callbacks = 0;
		auto done = [&](bool ok) {
			QVERIFY(!ok);
			callbacks++;
		};
		desktops->ensureCount(3, done);
		desktops->ensureCount(4, done);
		QCOMPARE(callbacks, 0);
		desktops->loseAccessorForTest();
		QTRY_COMPARE_WITH_TIMEOUT(callbacks, 2, 2000);
		QCOMPARE(fake.createCalls, 1);
		QCOMPARE(this->chords, 0);
	}

	void recoveryNeverReplaysDeniedSwitch() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		fake.publishSwitch = false;
		QVERIFY(desktops->switchTo(1));
		const auto id = desktops->lastCommand().value("requestId");
		desktops->loseAccessorForTest();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
		QSignalSpy results(desktops, &VirtualDesktops::commandResult);
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		desktops->refresh();
		fake.current = 1;
		desktops->refresh();
		QTest::qWait(1700);
		QCOMPARE(fake.switchCalls, 1);
		QCOMPARE(results.count(), 0);
		QCOMPARE(desktops->lastCommand().value("requestId"), id);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
	}

	void recoveryNeverReplaysDeniedCreation() {
		auto* desktops = this->make(hooks());
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		fake.publishCreation = false;
		int callbacks = 0;
		desktops->ensureCount(3, [&](bool ok) {
			QVERIFY(!ok);
			callbacks++;
		});
		desktops->loseAccessorForTest();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(callbacks, 1);
		desktops->installAccessorForTest(fakeAccessor());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		fake.count = 3;
		desktops->refresh();
		QTest::qWait(1200);
		QCOMPARE(fake.createCalls, 1);
		QCOMPARE(callbacks, 1);
		QCOMPARE(desktops->lastCommand().value("outcome").toString(), QStringLiteral("denied"));
	}

	void switchWithAccessorCallsAccessorAndNoChord() {
		auto* desktops = this->make(hooks(this->chordRecorder()));
		desktops->installAccessorForTest(fakeAccessor());

		QVERIFY(desktops->desktopControlSupported());
		QVERIFY(desktops->switchTo(1));
		QCOMPARE(fake.switchCalls, 1);
		QCOMPARE(fake.lastSwitch, 1);
		QCOMPARE(desktops->currentIndex(), qsizetype(1));
		QCOMPARE(this->chords, 0);
	}

	void createWithAccessorCallsAccessorAndNoChord() {
		auto* desktops = this->make(hooks(this->chordRecorder()));
		desktops->installAccessorForTest(fakeAccessor());

		auto called = false;
		auto value = false;
		desktops->ensureCount(3, [&](bool ok) {
			called = true;
			value = ok;
		});

		QTRY_VERIFY_WITH_TIMEOUT(called, 2000);
		QVERIFY(value);
		QCOMPARE(fake.createCalls, 1);
		QCOMPARE(desktops->count(), qsizetype(3));
		QCOMPARE(this->chords, 0);
	}

	void losingAccessorRefusesSubsequentControlWithoutChord() {
		auto* desktops = this->make(hooks(this->chordRecorder()));
		desktops->installAccessorForTest(fakeAccessor());
		desktops->loseAccessorForTest();

		QVERIFY(!desktops->desktopControlSupported());
		QVERIFY(!desktops->switchTo(1));

		auto called = false;
		auto value = true;
		desktops->ensureCount(3, [&](bool ok) {
			called = true;
			value = ok;
		});

		QVERIFY(called);
		QVERIFY(!value);
		QCOMPARE(this->chords, 0);
	}
};

QTEST_GUILESS_MAIN(VirtualDesktopsTest)

#include "virtual_desktops_test.moc"
