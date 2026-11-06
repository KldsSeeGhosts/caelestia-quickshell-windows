#include "device.hpp"

#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtypes.h>

namespace qs::service::upower {

QString UPowerDeviceState::toString(UPowerDeviceState::Enum status) {
	switch (status) {
	case UPowerDeviceState::Unknown: return "Unknown";
	case UPowerDeviceState::Charging: return "Charging";
	case UPowerDeviceState::Discharging: return "Discharging";
	case UPowerDeviceState::Empty: return "Empty";
	case UPowerDeviceState::FullyCharged: return "Fully Charged";
	case UPowerDeviceState::PendingCharge: return "Pending Charge";
	case UPowerDeviceState::PendingDischarge: return "Pending Discharge";
	default: return "Invalid Status";
	}
}

QString UPowerDeviceType::toString(UPowerDeviceType::Enum type) {
	switch (type) {
	case UPowerDeviceType::Unknown: return "Unknown";
	case UPowerDeviceType::LinePower: return "Line Power";
	case UPowerDeviceType::Battery: return "Battery";
	case UPowerDeviceType::Ups: return "Ups";
	case UPowerDeviceType::Monitor: return "Monitor";
	case UPowerDeviceType::Mouse: return "Mouse";
	case UPowerDeviceType::Keyboard: return "Keyboard";
	case UPowerDeviceType::Pda: return "Pda";
	case UPowerDeviceType::Phone: return "Phone";
	case UPowerDeviceType::MediaPlayer: return "Media Player";
	case UPowerDeviceType::Tablet: return "Tablet";
	case UPowerDeviceType::Computer: return "Computer";
	case UPowerDeviceType::GamingInput: return "Gaming Input";
	case UPowerDeviceType::Pen: return "Pen";
	case UPowerDeviceType::Touchpad: return "Touchpad";
	case UPowerDeviceType::Modem: return "Modem";
	case UPowerDeviceType::Network: return "Network";
	case UPowerDeviceType::Headset: return "Headset";
	case UPowerDeviceType::Speakers: return "Speakers";
	case UPowerDeviceType::Headphones: return "Headphones";
	case UPowerDeviceType::Video: return "Video";
	case UPowerDeviceType::OtherAudio: return "Other Audio";
	case UPowerDeviceType::RemoteControl: return "Remote Control";
	case UPowerDeviceType::Printer: return "Printer";
	case UPowerDeviceType::Scanner: return "Scanner";
	case UPowerDeviceType::Camera: return "Camera";
	case UPowerDeviceType::Wearable: return "Wearable";
	case UPowerDeviceType::Toy: return "Toy";
	case UPowerDeviceType::BluetoothGeneric: return "Bluetooth Generic";
	default: return "Invalid Type";
	}
}

namespace {
// SYSTEM_POWER_STATUS values documented by Win32. 255 means "unknown" for
// both the flag byte and the percentage, and must never be shown as 0%.
constexpr quint8 kBatteryFlagNoBattery = 128;
constexpr quint8 kBatteryFlagUnknown = 255;
constexpr quint8 kBatteryFlagCharging = 8;
constexpr quint8 kBatteryPercentageUnknown = 255;
constexpr quint32 kBatteryLifeTimeUnknown = 0xFFFFFFFF;
} // namespace

SystemPowerStatusReading interpretSystemPowerStatus(
    bool readSucceeded,
    quint8 batteryFlag,
    quint8 batteryLifePercent,
    quint8 acLineStatus,
    quint32 batteryLifeSeconds
) {
	SystemPowerStatusReading reading;
	if (!readSucceeded) return reading;

	reading.available = true;
	reading.onAc = acLineStatus == 1;
	// ACLineStatus 255 is unknown, not offline; it cannot prove battery power.
	reading.onAcKnown = acLineStatus == 0 || acLineStatus == 1;
	// 255 is "battery flag unknown": a successful read that still cannot prove
	// battery presence or absence. Only 128 proves a no-battery system.
	if (batteryFlag == kBatteryFlagUnknown) return reading;

	reading.presenceKnown = true;
	reading.batteryPresent = batteryFlag != kBatteryFlagNoBattery;
	if (!reading.batteryPresent) return reading;

	reading.charging = (batteryFlag & kBatteryFlagCharging) != 0;
	reading.percentageKnown = batteryLifePercent <= 100;
	if (reading.percentageKnown) reading.percentage = qreal(batteryLifePercent) / 100.0;

	reading.timeToEmptyKnown =
	    reading.onAcKnown && !reading.onAc && batteryLifeSeconds != kBatteryLifeTimeUnknown;
	if (reading.timeToEmptyKnown) reading.timeToEmpty = qreal(batteryLifeSeconds);

	return reading;
}

bool isOnBattery(const SystemPowerStatusReading& reading) {
	return reading.available && reading.onAcKnown && !reading.onAc;
}

UPowerDevice::UPowerDevice(QObject* parent): QObject(parent) {
	this->bIsLaptopBattery.setBinding([this]() {
		return this->bType == UPowerDeviceType::Battery && this->bPowerSupply.value();
	});

	this->bIsAvailable = false;
	this->bIsPresenceKnown = false;
	this->bIsPercentageKnown = false;
	this->bHealthSupported = false;
	this->bHealthPercentage = 100.0;
	this->bReady = true;
}

void UPowerDevice::applyReading(const SystemPowerStatusReading& reading) {
	this->bIsAvailable = reading.available;
	this->bIsPresenceKnown = reading.presenceKnown;

	if (!reading.available) {
		// A failed read proves neither presence nor absence. Clear every
		// projection so a previous reading cannot be presented as current.
		this->bIsPresent = false;
		this->bPowerSupply = false;
		this->bType = UPowerDeviceType::Unknown;
		this->bPercentage = 0.0;
		this->bIsPercentageKnown = false;
		this->bState = UPowerDeviceState::Unknown;
		this->bTimeToEmpty = 0.0;
		this->bTimeToFull = 0.0;
		this->bIconName = QString();
		this->bNativePath = QString();
		this->bModel = QString();
		return;
	}

	auto present = reading.presenceKnown && reading.batteryPresent;

	this->bIsPresent = present;
	this->bPowerSupply = present;
	this->bType = present ? UPowerDeviceType::Battery : UPowerDeviceType::Unknown;
	this->bNativePath = present ? QStringLiteral("WindowsBattery") : QString();
	this->bModel = present ? QStringLiteral("System Battery") : QString();

	this->bIsPercentageKnown = present && reading.percentageKnown;
	if (this->bIsPercentageKnown.value()) {
		this->bPercentage = reading.percentage;
	} else {
		// 255 is unknown, not an empty battery. Never substitute a level.
		this->bPercentage = 0.0;
	}

	UPowerDeviceState::Enum state = UPowerDeviceState::Unknown;
	if (present) {
		if (reading.charging) {
			state = UPowerDeviceState::Charging;
		} else if (reading.onAc) {
			state = reading.percentageKnown && reading.percentage >= 0.99
			          ? UPowerDeviceState::FullyCharged
			          : UPowerDeviceState::Charging;
		} else if (reading.onAcKnown) {
			state = UPowerDeviceState::Discharging;
		}
	}
	this->bState = state;

	this->bTimeToEmpty = present && reading.timeToEmptyKnown ? reading.timeToEmpty : 0.0;
	this->bTimeToFull = 0.0;

	this->bIconName = present ? (reading.charging ? QStringLiteral("battery-good-charging-symbolic")
	                                              : QStringLiteral("battery-good-symbolic"))
	                          : QString();
}

SystemPowerStatusReading UPowerDevice::updateFromSystemPowerStatus() {
	SYSTEM_POWER_STATUS status {};
	auto readSucceeded = GetSystemPowerStatus(&status) != 0;
	auto reading = interpretSystemPowerStatus(
	    readSucceeded,
	    readSucceeded ? status.BatteryFlag : 0,
	    readSucceeded ? status.BatteryLifePercent : kBatteryPercentageUnknown,
	    readSucceeded ? status.ACLineStatus : 0,
	    readSucceeded ? static_cast<quint32>(status.BatteryLifeTime) : kBatteryLifeTimeUnknown
	);
	this->applyReading(reading);
	return reading;
}

} // namespace qs::service::upower
