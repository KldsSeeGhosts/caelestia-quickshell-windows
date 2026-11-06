#include <qsignalspy.h>
#include <qtest.h>

#include "device.hpp"

using namespace qs::service::upower;

namespace {
// Documented SYSTEM_POWER_STATUS fixture values. No Windows call or hardware
// is used; the tests exercise the pure interpreter and device projection.
constexpr quint8 kNoBatteryFlag = 128;
constexpr quint8 kUnknownBatteryFlag = 255;
constexpr quint8 kChargingFlag = 8;
constexpr quint8 kUnknownPercentage = 255;
constexpr quint8 kAcOnline = 1;
constexpr quint8 kAcOffline = 0;
constexpr quint32 kUnknownLifeTime = 0xFFFFFFFF;
} // namespace

class UPowerStatusTest: public QObject {
	Q_OBJECT;

private slots:
	void successfulReadOfAbsentBattery() {
		auto reading = interpretSystemPowerStatus(
		    true,
		    kNoBatteryFlag,
		    kUnknownPercentage,
		    kAcOnline,
		    kUnknownLifeTime
		);
		QVERIFY(reading.available);
		QVERIFY(reading.presenceKnown);
		QVERIFY(!reading.batteryPresent);
		QVERIFY(reading.onAc);
		QVERIFY(!reading.percentageKnown);
		QCOMPARE(reading.percentage, 0.0);

		UPowerDevice device;
		device.applyReading(reading);
		QVERIFY(device.bindableIsAvailable().value());
		QVERIFY(device.bindableIsPresenceKnown().value());
		QVERIFY(!device.bindableIsPresent().value());
		QVERIFY(!device.bindableIsLaptopBattery().value());
		QVERIFY(!device.bindableIsPercentageKnown().value());
		QCOMPARE(device.bindablePercentage().value(), 0.0);
		QVERIFY(device.bindableNativePath().value().isEmpty());
	}

	void unknownBatteryFlagIsNotProvenAbsence() {
		auto reading = interpretSystemPowerStatus(
		    true,
		    kUnknownBatteryFlag,
		    kUnknownPercentage,
		    kAcOnline,
		    kUnknownLifeTime
		);
		QVERIFY(reading.available);
		QVERIFY(!reading.presenceKnown);
		QVERIFY(!reading.batteryPresent);

		UPowerDevice device;
		device.applyReading(reading);
		QVERIFY(device.bindableIsAvailable().value());
		QVERIFY(!device.bindableIsPresenceKnown().value());
		QVERIFY(!device.bindableIsPresent().value());
		QVERIFY(device.bindableNativePath().value().isEmpty());
	}

	void unknownPercentageIsNotZeroPercent() {
		auto unknown =
		    interpretSystemPowerStatus(true, kChargingFlag, kUnknownPercentage, kAcOffline, 5400);
		QVERIFY(unknown.presenceKnown);
		QVERIFY(unknown.batteryPresent);
		QVERIFY(unknown.charging);
		QVERIFY(!unknown.percentageKnown);
		QCOMPARE(unknown.percentage, 0.0);

		UPowerDevice device;
		auto known = interpretSystemPowerStatus(true, 0, 91, kAcOffline, 5400);
		device.applyReading(known);
		QVERIFY(device.bindableIsPercentageKnown().value());
		QCOMPARE(device.bindablePercentage().value(), 0.91);

		device.applyReading(unknown);
		QVERIFY(device.bindableIsPresent().value());
		QVERIFY(!device.bindableIsPercentageKnown().value());
		QCOMPARE(device.bindablePercentage().value(), 0.0);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Charging);
		QCOMPARE(device.bindableTimeToEmpty().value(), 5400.0);
	}

	void failedReadClearsStaleReadings() {
		UPowerDevice device;
		auto known = interpretSystemPowerStatus(true, 0, 91, kAcOffline, 7200);
		device.applyReading(known);
		QVERIFY(device.bindableIsAvailable().value());
		QVERIFY(device.bindableIsPresent().value());
		QVERIFY(device.bindableIsLaptopBattery().value());
		QCOMPARE(device.bindablePercentage().value(), 0.91);

		QSignalSpy availableSpy(&device, &UPowerDevice::isAvailableChanged);
		auto failed = interpretSystemPowerStatus(
		    false,
		    kUnknownBatteryFlag,
		    kUnknownPercentage,
		    0,
		    kUnknownLifeTime
		);
		QVERIFY(!failed.available);
		QVERIFY(!failed.presenceKnown);

		device.applyReading(failed);
		QVERIFY(!device.bindableIsAvailable().value());
		QVERIFY(!device.bindableIsPresenceKnown().value());
		QVERIFY(!device.bindableIsPresent().value());
		QVERIFY(!device.bindableIsPercentageKnown().value());
		QVERIFY(!device.bindableIsLaptopBattery().value());
		QCOMPARE(device.bindablePercentage().value(), 0.0);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Unknown);
		QCOMPARE(device.bindableTimeToEmpty().value(), 0.0);
		QVERIFY(device.bindableIconName().value().isEmpty());
		QVERIFY(device.bindableNativePath().value().isEmpty());
		QVERIFY(device.bindableModel().value().isEmpty());
		QCOMPARE(availableSpy.count(), 1);
		QVERIFY(!isOnBattery(failed));
	}

	void outOfRangePercentageIsUnknown() {
		for (quint8 percentage: {quint8(101), quint8(254), quint8(255)}) {
			auto reading = interpretSystemPowerStatus(true, 0, percentage, kAcOnline, 3600);
			QVERIFY(reading.batteryPresent);
			QVERIFY(!reading.percentageKnown);
			UPowerDevice device;
			device.applyReading(reading);
			QVERIFY(!device.bindableIsPercentageKnown().value());
			QCOMPARE(device.bindablePercentage().value(), 0.0);
			QVERIFY(device.bindableState().value() != UPowerDeviceState::FullyCharged);
		}
	}

	void stateAndTimeFollowFlags() {
		UPowerDevice device;
		auto charging = interpretSystemPowerStatus(true, kChargingFlag, 63, kAcOffline, 5400);
		device.applyReading(charging);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Charging);
		QCOMPARE(device.bindableTimeToEmpty().value(), 5400.0);
		QCOMPARE(device.bindablePercentage().value(), 0.63);
		QCOMPARE(device.bindableIconName().value(), QStringLiteral("battery-good-charging-symbolic"));
		QCOMPARE(device.bindableType().value(), UPowerDeviceType::Battery);

		auto full = interpretSystemPowerStatus(true, 0, 100, kAcOnline, kUnknownLifeTime);
		device.applyReading(full);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::FullyCharged);
		QCOMPARE(device.bindableTimeToEmpty().value(), 0.0);
		QVERIFY(isOnBattery(full) == false);

		auto partial = interpretSystemPowerStatus(true, 0, 50, kAcOnline, kUnknownLifeTime);
		device.applyReading(partial);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Charging);

		auto unknownOnAc =
		    interpretSystemPowerStatus(true, 0, kUnknownPercentage, kAcOnline, kUnknownLifeTime);
		device.applyReading(unknownOnAc);
		QVERIFY(device.bindableIsPresent().value());
		QVERIFY(!device.bindableIsPercentageKnown().value());
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Charging);

		auto discharging = interpretSystemPowerStatus(true, 0, 50, kAcOffline, 3600);
		device.applyReading(discharging);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Discharging);
		QCOMPARE(device.bindableTimeToEmpty().value(), 3600.0);
		QVERIFY(isOnBattery(discharging));

		auto unknownLife = interpretSystemPowerStatus(true, 0, 50, kAcOffline, kUnknownLifeTime);
		device.applyReading(unknownLife);
		QCOMPARE(device.bindableTimeToEmpty().value(), 0.0);
	}

	void onBatteryRequiresSuccessfulRead() {
		QVERIFY(!isOnBattery(SystemPowerStatusReading {}));

		auto discharging = interpretSystemPowerStatus(true, 0, 63, kAcOffline, 5400);
		QVERIFY(discharging.available);
		QVERIFY(isOnBattery(discharging));

		auto online = interpretSystemPowerStatus(true, 0, 63, kAcOnline, 5400);
		QVERIFY(online.onAcKnown);
		QVERIFY(!isOnBattery(online));

		// ACLineStatus 255 is unknown, not offline.
		auto unknownAc = interpretSystemPowerStatus(true, 0, 63, 255, 5400);
		QVERIFY(unknownAc.available);
		QVERIFY(!unknownAc.onAcKnown);
		QVERIFY(!isOnBattery(unknownAc));
		QVERIFY(!unknownAc.timeToEmptyKnown);
		UPowerDevice device;
		device.applyReading(unknownAc);
		QCOMPARE(device.bindableState().value(), UPowerDeviceState::Unknown);
		QCOMPARE(device.bindableTimeToEmpty().value(), 0.0);

		// A failed read cannot keep the previous "on battery" claim alive.
		auto failed = interpretSystemPowerStatus(false, 0, 63, kAcOffline, 5400);
		QVERIFY(!failed.available);
		QVERIFY(!isOnBattery(failed));
	}
};

QTEST_GUILESS_MAIN(UPowerStatusTest)

#include "upower_status_test.moc"
