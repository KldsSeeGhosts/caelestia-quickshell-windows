#include "device.hpp"

#include <atomic>

#include <qdebug.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qproperty.h>
#include <qstring.h>
#include <qtypes.h>

#include "adapter.hpp"
#include "backend_types.hpp"
#include "bluetooth.hpp"
#include "bt_backend.hpp"
#include "pairing_policy.hpp"
#include "../redact.hpp"

namespace qs::bluetooth {

namespace {
Q_LOGGING_CATEGORY(logDevice, "quickshell.windows.bluetooth.device", QtWarningMsg);

constexpr int STATE_TIMEOUT_MS = 20'000;
} // namespace

QString BluetoothDeviceState::toString(BluetoothDeviceState::Enum state) {
	switch (state) {
	case BluetoothDeviceState::Disconnected: return QStringLiteral("Disconnected");
	case BluetoothDeviceState::Connected: return QStringLiteral("Connected");
	case BluetoothDeviceState::Disconnecting: return QStringLiteral("Disconnecting");
	case BluetoothDeviceState::Connecting: return QStringLiteral("Connecting");
	default: return QStringLiteral("Unknown");
	}
}

BluetoothDevice::BluetoothDevice(const DeviceSnapshot& snapshot, QObject* parent)
    : QObject(parent)
    , mKey(snapshot.key)
    , mPath(snapshot.path) {
	this->mStateTimeout.setSingleShot(true);
	this->mStateTimeout.setInterval(STATE_TIMEOUT_MS);
	QObject::connect(&this->mStateTimeout, &QTimer::timeout, this, &BluetoothDevice::settleState);

	this->applySnapshot(snapshot);
}

BluetoothAdapter* BluetoothDevice::adapter() const { return WinBluetooth::instance()->adapter(); }

void BluetoothDevice::applySnapshot(const DeviceSnapshot& snapshot) {
	Qt::beginPropertyUpdateGroup();
	this->bAddress = snapshot.address;
	this->bName = snapshot.name;
	this->bDeviceName = snapshot.deviceName;
	this->bIcon = snapshot.icon;
	this->bPaired = snapshot.paired;
	this->bBonded = snapshot.paired;
	this->bTrusted = snapshot.paired;
	this->bConnected = snapshot.connected;
	this->bBattery = snapshot.batteryAvailable ? snapshot.battery : 0;
	Qt::endPropertyUpdateGroup();

	if (snapshot.batteryAvailable != this->mBatteryAvailable) {
		this->mBatteryAvailable = snapshot.batteryAvailable;
		emit this->batteryAvailableChanged();
	}
}

void BluetoothDevice::onConnectedChanged() {
	this->mStateTimeout.stop();
	this->bState =
	    this->bConnected ? BluetoothDeviceState::Connected : BluetoothDeviceState::Disconnected;
	emit this->connectedChanged();
}

void BluetoothDevice::settleState() {
	this->mStateTimeout.stop();
	this->bState =
	    this->bConnected ? BluetoothDeviceState::Connected : BluetoothDeviceState::Disconnected;
}

void BluetoothDevice::setConnected(bool connected) {
	if (connected == this->bConnected) return;

	if (connected) {
		this->connect();
	} else {
		this->disconnect();
	}
}

void BluetoothDevice::connect() {
	if (this->bConnected) {
		qCCritical(logDevice) << "Device" << this << "is already connected";
		return;
	}

	if (this->bState == BluetoothDeviceState::Connecting) {
		qCCritical(logDevice) << "Device" << this << "is already connecting";
		return;
	}

	if (!this->bPaired) {
		qCDebug(logDevice) << "Device" << this << "isn't paired, pairing instead of connecting";
		if (!this->bPairing) this->pair();
		return;
	}

	qCDebug(logDevice) << "Connecting to device" << this;
	this->bState = BluetoothDeviceState::Connecting;
	this->mStateTimeout.start();
	WinBluetooth::instance()->backend()->connectDevice(this->mKey, true);
}

void BluetoothDevice::disconnect() {
	if (!this->bConnected) {
		qCCritical(logDevice) << "Device" << this << "is already disconnected";
		return;
	}

	if (this->bState == BluetoothDeviceState::Disconnecting) {
		qCCritical(logDevice) << "Device" << this << "is already disconnecting";
		return;
	}

	qCDebug(logDevice) << "Disconnecting from device" << this;
	this->bState = BluetoothDeviceState::Disconnecting;
	this->mStateTimeout.start();
	WinBluetooth::instance()->backend()->connectDevice(this->mKey, false);
}

void BluetoothDevice::applyConnectFinished(bool /*connect*/, ConnectResult result) {
	if (result != ConnectResult::Sent) this->settleState();
}

void BluetoothDevice::pair() {
	if (this->bPaired) {
		qCCritical(logDevice) << "Device" << this << "is already paired";
		return;
	}

	if (this->bPairing) {
		qCCritical(logDevice) << "Device" << this << "is already pairing";
		return;
	}

	qCDebug(logDevice) << "Pairing with device" << this;
	auto attempt = this->beginPairing();
	WinBluetooth::instance()->backend()->pair(this->mKey, attempt);
}

quint64 BluetoothDevice::beginPairing() {
	// Process-wide IDs also distinguish a removed device from its replacement.
	static std::atomic<quint64> nextAttempt {0};
	this->mPairingAttempt = ++nextAttempt;
	this->bLastPairingKind = QString();
	this->bPairing = true;
	return this->mPairingAttempt;
}

void BluetoothDevice::applyPairingKind(quint64 attempt, const QString& kind) {
	if (!attempt || attempt != this->mPairingAttempt) return;
	if (kind.isEmpty() || kind == this->bLastPairingKind) return;
	this->bLastPairingKind = kind;
}

void BluetoothDevice::applyPairFinished(
    quint64 attempt,
    bool paired,
    qint32 status,
    const QString& pairingKind
) {
	if (!attempt || attempt != this->mPairingAttempt) return;
	this->mPairingAttempt = 0;
	qCDebug(logDevice) << "Pairing with device" << this << (paired ? "succeeded" : "failed")
	                   << pairingResultStatusName(status);
	if (!pairingKind.isEmpty()) this->bLastPairingKind = pairingKind;
	this->bPairing = false;
	emit this->pairingFinished(paired, pairingResultStatusName(status), pairingKind);
}

void BluetoothDevice::cancelPair() {
	if (!this->bPairing) {
		qCCritical(logDevice) << "Device" << this << "is not currently pairing";
		return;
	}

	qCDebug(logDevice) << "Cancelling pairing with device" << this;
	WinBluetooth::instance()->backend()->cancelPair(this->mKey, this->mPairingAttempt);
}

void BluetoothDevice::forget() {
	qCDebug(logDevice) << "Forgetting device" << this;
	WinBluetooth::instance()->backend()->forget(this->mKey);
}

void BluetoothDevice::warnUnsupported(const char* what) {
	if (this->mUnsupportedWarned) return;
	this->mUnsupportedWarned = true;
	qCWarning(logDevice) << "Setting" << what << "on" << this << "isn't supported on Windows";
}

void BluetoothDevice::setTrusted(bool trusted) {
	if (trusted != this->bTrusted) this->warnUnsupported("trusted");
}

void BluetoothDevice::setBlocked(bool blocked) {
	if (blocked != this->bBlocked) this->warnUnsupported("blocked");
}

void BluetoothDevice::setName(const QString& name) {
	if (name != this->bName) this->warnUnsupported("name");
}

void BluetoothDevice::setWakeAllowed(bool wakeAllowed) {
	if (wakeAllowed != this->bWakeAllowed) this->warnUnsupported("wakeAllowed");
}

} // namespace qs::bluetooth

QDebug operator<<(QDebug debug, const qs::bluetooth::BluetoothDevice* device) {
	auto saver = QDebugStateSaver(debug);

	if (device) {
		debug.nospace() << "BluetoothDevice(" << static_cast<const void*>(device)
		                << ", id=" << qs::windows::logId(device->key())
		                << ", connected=" << device->connected()
		                << ", pairing=" << device->pairing() << ")";
	} else {
		debug << "BluetoothDevice(nullptr)";
	}

	return debug;
}
