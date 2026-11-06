#include <qglobal.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "powerprofiles.hpp"

using namespace qs::service::upower;

namespace qs::log {
// Test stub for the logging-category hook referenced by powerprofiles.cpp. No
// logging runtime is linked into this fixture.
void initLogCategoryLevel(const char* name, QtMsgType defaultLevel) {
	Q_UNUSED(name);
	Q_UNUSED(defaultLevel);
}
} // namespace qs::log

namespace {
// Documented overlay GUID fixtures. Every powrprof call in this test targets a
// fake; no real power API, overlay read, overlay write or notification
// registration is performed.
const GUID kEfficiencyOverlay =
    {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
const GUID kPerformanceOverlay =
    {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
const GUID kBalancedOverlay = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
const GUID kUnknownOverlay = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};

struct FakePowerProfiles {
	GUID effectiveOverlay = kBalancedOverlay;
	DWORD getStatus = ERROR_SUCCESS;
	DWORD setStatus = ERROR_SUCCESS;
	bool applySetRequests = true;
	HRESULT registerStatus = 0; // S_OK

	int getCalls = 0;
	int setCalls = 0;
	GUID lastRequestedOverlay = {};
	EFFECTIVE_POWER_MODE_CALLBACK* registeredCallback = nullptr;
	void* registeredContext = nullptr;
};

FakePowerProfiles* fake = nullptr;

DWORD WINAPI fakeGetEffectiveOverlayScheme(GUID* overlayScheme) {
	++fake->getCalls;
	if (fake->getStatus != ERROR_SUCCESS) return fake->getStatus;
	*overlayScheme = fake->effectiveOverlay;
	return ERROR_SUCCESS;
}

DWORD WINAPI fakeSetActiveOverlayScheme(GUID overlayScheme) {
	++fake->setCalls;
	fake->lastRequestedOverlay = overlayScheme;
	if (fake->setStatus == ERROR_SUCCESS && fake->applySetRequests) {
		fake->effectiveOverlay = overlayScheme;
	}
	return fake->setStatus;
}

HRESULT WINAPI fakeRegisterNotifications(
    unsigned long /*version*/,
    EFFECTIVE_POWER_MODE_CALLBACK* callback,
    void* context,
    void** registrationHandle
) {
	if (fake->registerStatus != 0) return fake->registerStatus;
	fake->registeredCallback = *callback;
	fake->registeredContext = context;
	*registrationHandle = &fake->registeredCallback;
	return 0;
}

HRESULT WINAPI fakeUnregisterNotifications(void* /*registrationHandle*/) { return 0; }

PowerProfilesApi fakeApi() {
	PowerProfilesApi api;
	api.getEffectiveOverlayScheme = &fakeGetEffectiveOverlayScheme;
	api.setEffectiveOverlayScheme = &fakeSetActiveOverlayScheme;
	api.registerEffectivePowerModeNotifications = &fakeRegisterNotifications;
	api.unregisterEffectivePowerModeNotifications = &fakeUnregisterNotifications;
	return api;
}
} // namespace

class PowerProfilesTest: public QObject {
	Q_OBJECT;

private slots:
	void knownOverlayGuidsMapToProfiles() {
		FakePowerProfiles state;
		fake = &state;
		auto api = fakeApi();

		state.effectiveOverlay = kEfficiencyOverlay;
		PowerProfilesQml saver(nullptr, &api);
		QVERIFY(saver.bindableProfileKnown().value());
		QCOMPARE(saver.bindableProfile().value(), PowerProfile::PowerSaver);
		QVERIFY(!saver.bindableRawOverlayGuid().value().isEmpty());
		QVERIFY(saver.bindableLastError().value().isEmpty());
		QVERIFY(saver.bindableObserving().value());

		state.effectiveOverlay = kPerformanceOverlay;
		PowerProfilesQml performance(nullptr, &api);
		QVERIFY(performance.bindableProfileKnown().value());
		QCOMPARE(performance.bindableProfile().value(), PowerProfile::Performance);
		QCOMPARE(
		    performance.bindableRawOverlayGuid().value(),
		    QStringLiteral("{DED574B5-45A0-4F42-8737-46345C09C238}")
		);

		state.effectiveOverlay = kBalancedOverlay;
		PowerProfilesQml balanced(nullptr, &api);
		QVERIFY(balanced.bindableProfileKnown().value());
		QCOMPARE(balanced.bindableProfile().value(), PowerProfile::Balanced);
	}

	void unknownOverlayGuidIsNotABalancedClaim() {
		FakePowerProfiles state;
		state.effectiveOverlay = kUnknownOverlay;
		fake = &state;
		auto api = fakeApi();

		PowerProfilesQml profiles(nullptr, &api);
		QVERIFY(!profiles.bindableProfileKnown().value());
		// Documented compatibility behavior: the profile property still
		// surfaces unknown GUIDs as Balanced.
		QCOMPARE(profiles.bindableProfile().value(), PowerProfile::Balanced);
		QVERIFY(profiles.bindableRawOverlayGuid().value().contains(QStringLiteral("11111111")));
		QVERIFY(profiles.bindableLastError().value().isEmpty());
	}

	void observersSeeConsistentSnapshots() {
		FakePowerProfiles state;
		state.effectiveOverlay = kPerformanceOverlay;
		fake = &state;
		auto api = fakeApi();
		PowerProfilesQml profiles(nullptr, &api);
		QVERIFY(profiles.bindableProfileKnown().value());

		// Known Performance -> unknown GUID: when profile changes to Balanced,
		// validity and GUID must already describe the unknown overlay.
		bool sawProfileChange = false;
		bool knownAtChange = true;
		QString guidAtChange;
		auto connection = QObject::connect(&profiles, &PowerProfilesQml::profileChanged, [&] {
			sawProfileChange = true;
			knownAtChange = profiles.bindableProfileKnown().value();
			guidAtChange = profiles.bindableRawOverlayGuid().value();
		});
		state.effectiveOverlay = kUnknownOverlay;
		profiles.refresh();
		QVERIFY(sawProfileChange);
		QVERIFY(!knownAtChange);
		QVERIFY(guidAtChange.contains(QStringLiteral("11111111")));
		QObject::disconnect(connection);

		// Failed read -> successful PowerSaver read: when validity returns,
		// the profile and cleared error are already published.
		state.getStatus = ERROR_ACCESS_DENIED;
		profiles.refresh();
		QVERIFY(!profiles.bindableProfileKnown().value());
		state.getStatus = ERROR_SUCCESS;
		state.effectiveOverlay = kEfficiencyOverlay;
		bool sawKnownChange = false;
		PowerProfile::Enum profileAtKnown = PowerProfile::Balanced;
		QString errorAtKnown = QStringLiteral("unset");
		connection = QObject::connect(&profiles, &PowerProfilesQml::profileKnownChanged, [&] {
			sawKnownChange = true;
			profileAtKnown = profiles.bindableProfile().value();
			errorAtKnown = profiles.bindableLastError().value();
		});
		profiles.refresh();
		QVERIFY(sawKnownChange);
		QCOMPARE(profileAtKnown, PowerProfile::PowerSaver);
		QVERIFY(errorAtKnown.isEmpty());
		QObject::disconnect(connection);
	}

	void readFailureIsDistinguishableFromPowerSaver() {
		FakePowerProfiles ok;
		ok.effectiveOverlay = kEfficiencyOverlay;
		fake = &ok;
		auto api = fakeApi();
		PowerProfilesQml saver(nullptr, &api);
		QCOMPARE(saver.bindableProfile().value(), PowerProfile::PowerSaver);
		QVERIFY(saver.bindableProfileKnown().value());

		FakePowerProfiles failing;
		failing.getStatus = ERROR_ACCESS_DENIED;
		fake = &failing;
		PowerProfilesQml broken(nullptr, &api);
		QVERIFY(!broken.bindableProfileKnown().value());
		QVERIFY(broken.bindableLastError().value().contains(QStringLiteral("PowerGetEffectiveOverlayScheme")));
		QVERIFY(broken.bindableRawOverlayGuid().value().isEmpty());
		// The zero-initialized profile value alone still reads PowerSaver;
		// profileKnown is what separates a real read from this legacy state.
		QCOMPARE(broken.bindableProfile().value(), PowerProfile::PowerSaver);
		QVERIFY(saver.bindableProfileKnown().value());

		// A later successful read restores knowledge and clears the error.
		failing.getStatus = ERROR_SUCCESS;
		failing.effectiveOverlay = kPerformanceOverlay;
		broken.refresh();
		QVERIFY(broken.bindableProfileKnown().value());
		QCOMPARE(broken.bindableProfile().value(), PowerProfile::Performance);
		QVERIFY(broken.bindableLastError().value().isEmpty());
	}

	void missingGetExportSetsError() {
		FakePowerProfiles state;
		fake = &state;
		auto api = fakeApi();
		api.getEffectiveOverlayScheme = nullptr;

		PowerProfilesQml profiles(nullptr, &api);
		QVERIFY(!profiles.bindableProfileKnown().value());
		QVERIFY(profiles.bindableLastError().value().contains(QStringLiteral("not available")));
		QVERIFY(profiles.bindableRawOverlayGuid().value().isEmpty());
	}

	void writeSuccessRequiresMatchingReread() {
		FakePowerProfiles state;
		fake = &state;
		auto api = fakeApi();
		PowerProfilesQml profiles(nullptr, &api);
		QVERIFY(profiles.bindableWriteSupported().value());
		QVERIFY(profiles.bindableHasPerformanceProfile().value());

		QSignalSpy results(&profiles, &PowerProfilesQml::profileWriteResult);
		profiles.setProfile(PowerProfile::Performance);
		QCOMPARE(results.count(), 1);
		auto args = results.takeFirst();
		QCOMPARE(args.at(0).toInt(), static_cast<int>(PowerProfile::Performance));
		QCOMPARE(args.at(1).toBool(), true);
		QCOMPARE(args.at(2).toString(), QString());
		QCOMPARE(state.setCalls, 1);
		QVERIFY(IsEqualGUID(state.lastRequestedOverlay, kPerformanceOverlay));
		QCOMPARE(profiles.bindableProfile().value(), PowerProfile::Performance);
		QVERIFY(profiles.bindableProfileKnown().value());
	}

	void writeApiFailureReportsFailure() {
		FakePowerProfiles state;
		state.setStatus = ERROR_ACCESS_DENIED;
		state.applySetRequests = false;
		fake = &state;
		auto api = fakeApi();
		PowerProfilesQml profiles(nullptr, &api);

		QSignalSpy results(&profiles, &PowerProfilesQml::profileWriteResult);
		profiles.setProfile(PowerProfile::PowerSaver);
		QCOMPARE(results.count(), 1);
		auto args = results.takeFirst();
		QCOMPARE(args.at(0).toInt(), static_cast<int>(PowerProfile::PowerSaver));
		QCOMPARE(args.at(1).toBool(), false);
		QVERIFY(args.at(2).toString().contains(QStringLiteral("PowerSetActiveOverlayScheme failed")));
		QCOMPARE(profiles.bindableProfile().value(), PowerProfile::Balanced);
		QVERIFY(profiles.bindableProfileKnown().value());

		// An invalid enum request reports failure without calling the API.
		auto invalid = static_cast<PowerProfile::Enum>(99);
		profiles.setProfile(invalid);
		QCOMPARE(results.count(), 1);
		QCOMPARE(results.takeFirst().at(1).toBool(), false);
		QCOMPARE(state.setCalls, 1);
	}

	void writeAcceptedButOverlayMismatchFails() {
		FakePowerProfiles state;
		state.applySetRequests = false;
		fake = &state;
		auto api = fakeApi();
		PowerProfilesQml profiles(nullptr, &api);

		QSignalSpy results(&profiles, &PowerProfilesQml::profileWriteResult);
		// The set call succeeds but the effective overlay stays Balanced.
		profiles.setProfile(PowerProfile::Performance);
		QCOMPARE(results.count(), 1);
		auto args = results.takeFirst();
		QCOMPARE(args.at(1).toBool(), false);
		QVERIFY(!args.at(2).toString().isEmpty());
		QCOMPARE(profiles.bindableProfile().value(), PowerProfile::Balanced);

		// A set followed by a failed verification read also reports failure
		// and surfaces the read error.
		state.getStatus = ERROR_ACCESS_DENIED;
		profiles.setProfile(PowerProfile::PowerSaver);
		QCOMPARE(results.count(), 1);
		args = results.takeFirst();
		QCOMPARE(args.at(1).toBool(), false);
		QVERIFY(args.at(2).toString().contains(QStringLiteral("PowerGetEffectiveOverlayScheme")));
	}

	void unresolvedSetApiDisablesWritesAndPerformanceClaim() {
		FakePowerProfiles state;
		fake = &state;
		auto api = fakeApi();
		api.setEffectiveOverlayScheme = nullptr;

		PowerProfilesQml profiles(nullptr, &api);
		QVERIFY(!profiles.bindableWriteSupported().value());
		QVERIFY(!profiles.bindableHasPerformanceProfile().value());

		QSignalSpy results(&profiles, &PowerProfilesQml::profileWriteResult);
		profiles.setProfile(PowerProfile::Performance);
		QCOMPARE(results.count(), 1);
		auto args = results.takeFirst();
		QCOMPARE(args.at(1).toBool(), false);
		QVERIFY(args.at(2).toString().contains(QStringLiteral("not available")));
		QCOMPARE(state.setCalls, 0);
	}

	void registrationFailureObservesFalseAndPolls() {
		FakePowerProfiles state;
		state.registerStatus = 0x80004005; // E_FAIL
		fake = &state;
		auto api = fakeApi();
		PowerProfilesQml profiles(nullptr, &api, 50);
		QVERIFY(!profiles.bindableObserving().value());
		QCOMPARE(profiles.bindableProfile().value(), PowerProfile::Balanced);

		// The bounded fallback refresh observes the overlay change without
		// any notification callback.
		state.effectiveOverlay = kPerformanceOverlay;
		QTRY_COMPARE_WITH_TIMEOUT(profiles.bindableProfile().value(), PowerProfile::Performance, 5000);
		QVERIFY(profiles.bindableProfileKnown().value());

		// An unresolved registration export behaves the same way.
		auto noRegister = fakeApi();
		noRegister.registerEffectivePowerModeNotifications = nullptr;
		state.effectiveOverlay = kBalancedOverlay;
		PowerProfilesQml unregistered(nullptr, &noRegister, 50);
		QVERIFY(!unregistered.bindableObserving().value());
		state.effectiveOverlay = kEfficiencyOverlay;
		QTRY_COMPARE_WITH_TIMEOUT(
		    unregistered.bindableProfile().value(),
		    PowerProfile::PowerSaver,
		    5000
		);
	}
};

QTEST_GUILESS_MAIN(PowerProfilesTest)

#include "power_profiles_test.moc"
