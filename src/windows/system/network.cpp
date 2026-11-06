#include "network.hpp"

#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtimer.h>
#include <shellapi.h>

#include "network_connectivity.hpp"

namespace qs::windows::sys {

Network::Network(QObject* parent): Network(parent, true) {}

#ifdef QS_TEST
Network* Network::createForTesting() { return new Network(nullptr, false); }
#endif

Network::Network(QObject* parent, bool observe): QObject(parent) {
	this->bWifiStatus = QStringLiteral("disabled");

	this->mWifi = std::make_unique<NetworkWifiBackend>(this);
	this->mConnectivity = std::make_unique<NetworkConnectivityBackend>(this);

	this->mScanTimer.setInterval(8000);
	QObject::connect(&this->mScanTimer, &QTimer::timeout, this, &Network::scanWifiNetworks);

	// A fixture constructs the state without touching the live WLAN service or
	// the connectivity observer.
	if (!observe) return;

	this->mConnectivity->start();
	this->mWifi->start();
}

Network::~Network() = default;

void Network::setWifiRadioEnabled(bool enabled) { this->mWifi->setRadioEnabled(enabled); }

void Network::scanWifiNetworks() { this->mWifi->scan(); }

NetworkWifiNetwork* Network::findNetwork(const QString& ssid) const {
	NetworkWifiNetwork* match = nullptr;
	for (auto* net: this->mNetworks.valueList()) {
		if (net->bindableSsid().value() != ssid) continue;
		if (match != nullptr) return nullptr; // Ambiguous display label, fail closed.
		match = net;
	}
	return match;
}

NetworkWifiNetwork* Network::findNetworkById(const QString& selectionId) const {
	if (selectionId.isEmpty()) return nullptr;
	for (auto* net: this->mNetworks.valueList()) {
		if (net->selectionId() == selectionId) return net;
	}
	return nullptr;
}

void Network::connectToNetwork(const QString& ssid, const QString& password) {
	this->connectNetwork(this->findNetwork(ssid), ssid, password);
}

void Network::connectToNetworkById(const QString& selectionId, const QString& password) {
	this->connectNetwork(this->findNetworkById(selectionId), selectionId, password);
}

void Network::connectNetwork(
    NetworkWifiNetwork* network,
    const QString& selection,
    const QString& password
) {
	if (this->mPendingWifiRequest != 0) {
		this->publishWifiCommand({}, selection, QStringLiteral("denied"), QStringLiteral("busy"));
		return;
	}
	if (network == nullptr) {
		this->publishWifiCommand(
		    {},
		    selection,
		    QStringLiteral("denied"),
		    QStringLiteral("stale-or-ambiguous")
		);
		emit this->wifiConnectResult(selection, false, QStringLiteral("stale-or-ambiguous"));
		return;
	}
	this->mPendingWifiNetwork = network->network();
	this->mPendingWifiRequest = ++this->mNextWifiRequest;
	const auto requestId = this->mPendingWifiRequest;
	const auto target = this->mPendingWifiNetwork;
	this->mPendingWifiAccepted = false;
	this->bWifiConnectingSsid = target.ssid;
	if (this->mPendingWifiRequest != requestId) return;
	this->bWifiConnecting = true;
	if (this->mPendingWifiRequest != requestId) return;
	this->updateWifiStatus();

	if (this->mPendingWifiRequest == requestId)
		this->mWifi->connectToNetwork(target, password, requestId);
}

void Network::cancelWifiConnection() {
	if (this->mPendingWifiRequest == 0) return;
	const auto id = this->mPendingWifiRequest;
	const auto network = this->mPendingWifiNetwork;
	const auto accepted = this->mPendingWifiAccepted;
	this->mPendingWifiRequest = 0;
	this->mPendingWifiAccepted = false;
	// Stop attributing results to this request. WlanDisconnect targets the
	// interface's current connection, which may now be a different network.
	// Only the separate explicit Disconnect command may request that action.
	this->mWifi->cancelConnection();
	this->bWifiConnecting = false;
	if (this->mPendingWifiRequest == 0) this->bWifiConnectingSsid = QString();
	this->updateWifiStatus();
	this->publishWifiCommand(
	    accepted ? QString::number(id) : QString(),
	    wifi_identity::selectionId(network),
	    QStringLiteral("cancelled"),
	    QStringLiteral("completion-unobserved")
	);
	if (this->mPendingWifiRequest == 0)
		emit this->wifiConnectResult(network.ssid, false, QStringLiteral("cancelled"));
}

void Network::disconnectActive() {
	const auto generation = this->mNextWifiRequest;
	if (this->mPendingWifiRequest != 0) this->cancelWifiConnection();
	if (this->mNextWifiRequest == generation) this->mWifi->disconnectActive();
}

void Network::forgetNetwork(const QString& ssid) {
	auto* net = this->findNetwork(ssid);
	if (net != nullptr) this->forgetNetworkById(net->selectionId());
}

void Network::forgetNetworkById(const QString& selectionId) {
	auto* net = this->findNetworkById(selectionId);
	if (net == nullptr) return;
	const auto network = net->network();
	if (this->mPendingWifiRequest != 0
	    && wifi_identity::selectionId(this->mPendingWifiNetwork) == selectionId)
		this->cancelWifiConnection();
	this->mWifi->forgetNetwork(network);
}

void Network::openWifiSettings() {
	ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL);
}

void Network::openLocationSettings() {
	ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-location", nullptr, nullptr, SW_SHOWNORMAL);
}

void Network::setWifiListVisible(bool visible) {
	if (this->mWifiListVisible == visible) return;
	this->mWifiListVisible = visible;

	if (visible) {
		this->mWifi->refreshAvailableNetworks();
		this->mWifi->scan();
		this->mScanTimer.start();
	} else {
		this->mScanTimer.stop();
	}
}

void Network::updateWifiStatus() {
	QString status;
	if (!this->bWifiRadioOn.value()) {
		status = QStringLiteral("disabled");
	} else if (this->bWifiConnecting.value()) {
		status = QStringLiteral("connecting");
	} else if (!this->bActiveSsid.value().isEmpty()) {
		status = this->bHasInternet.value() ? QStringLiteral("connected") : QStringLiteral("limited");
	} else {
		status = QStringLiteral("disconnected");
	}

	if (status != this->bWifiStatus.value()) this->bWifiStatus = status;
}

void Network::backendSetHasInternet(bool hasInternet) {
	this->bHasInternet = hasInternet;
	this->updateWifiStatus();
}

void Network::backendSetEthernetConnected(bool connected) { this->bEthernetConnected = connected; }

void Network::backendSetWifiAdapterPresent(bool present) { this->bWifiAdapterPresent = present; }

void Network::backendSetWifiInitialized(bool initialized) { this->bWifiInitialized = initialized; }

void Network::backendSetWifiBackendError(const QString& error) { this->bWifiBackendError = error; }

void Network::backendSetWifiRadioOn(bool on) {
	this->bWifiRadioOn = on;
	this->updateWifiStatus();
}

void Network::backendSetWifiScanning(bool scanning) { this->bWifiScanning = scanning; }

void Network::backendSetNeedsLocationPermission(bool needs) {
	this->bNeedsLocationPermission = needs;
}

void Network::backendSetCurrentConnection(
    const RawWifiNetwork& network,
    bool connected,
    bool connecting
) {
	this->bActiveSsid = connected ? network.ssid : QString();
	this->bActiveBssid = connected ? network.bssid : QString();
	this->bActiveSignalQuality = connected ? network.strength : 0;
	this->bActiveSecurity = connected ? wifi_identity::securityLabel(network) : QString();

	if (connected && !this->mWifiListVisible) {
		auto* net = this->findNetworkById(wifi_identity::selectionId(network));
		if (net != nullptr && net->bindableActive().value()) net->setStrength(network.strength);
	}

	// A display-string observation is not command completion. Only the WLAN
	// request's matching GUID/raw SSID/profile notification ends our request.
	if (this->mPendingWifiRequest == 0) {
		this->bWifiConnecting = connecting;
		this->bWifiConnectingSsid = connecting ? network.ssid : QString();
	}

	this->updateWifiStatus();
}

void Network::backendUpdateAvailableNetworks(const std::vector<RawWifiNetwork>& networks) {
	QSet<QString> seen;
	for (const auto& r: networks) {
		const auto id = wifi_identity::selectionId(r);
		if (!id.isEmpty()) seen.insert(id);
	}

	auto& list = this->mNetworks.valueList();
	for (auto i = list.size() - 1; i >= 0; i--) {
		if (!seen.contains(list.at(i)->selectionId())) {
			auto* obj = list.at(i);
			this->mNetworks.removeAt(i);
			obj->deleteLater();
		}
	}

	for (const auto& r: networks) {
		const auto id = wifi_identity::selectionId(r);
		if (id.isEmpty()) continue;
		auto* item = this->findNetworkById(id);
		if (item == nullptr) {
			item = new NetworkWifiNetwork(&this->mNetworks);
			this->mNetworks.insertObject(item);
		}

		item->setNetwork(r);
	}
}

void Network::backendPublishAvailableNetworks(const std::vector<RawWifiNetwork>& networks) {
	this->backendUpdateAvailableNetworks(networks);
	this->bWifiSnapshot = this->bWifiSnapshot.value() + 1;
}

void Network::backendEndPendingConnection() {
	this->mWifi->cancelConnection();
	if (this->mPendingWifiRequest != 0)
		this->backendWifiConnectResult(
		    this->mPendingWifiRequest,
		    false,
		    QStringLiteral("adapter-removed")
		);
}

void Network::publishWifiCommand(
    const QString& id,
    const QString& selectionId,
    const QString& status,
    const QString& reason
) {
	this->mLastWifiCommand = {
	    {QStringLiteral("requestId"), id},
	    {QStringLiteral("selectionId"), selectionId},
	    {QStringLiteral("status"), status},
	    {QStringLiteral("reason"), reason},
	};
	// Preserve this event even if a signal handler starts another command.
	const auto result = this->mLastWifiCommand;
	emit this->wifiCommandResult(result);
	emit this->wifiCommandChanged();
}

void Network::backendWifiConnectAccepted(quint64 requestId) {
	if (requestId == 0 || requestId != this->mPendingWifiRequest) return;
	this->mPendingWifiAccepted = true;
	this->publishWifiCommand(
	    QString::number(requestId),
	    wifi_identity::selectionId(this->mPendingWifiNetwork),
	    QStringLiteral("accepted"),
	    {}
	);
}

void Network::backendWifiConnectResult(quint64 requestId, bool success, const QString& reason) {
	if (requestId == 0 || requestId != this->mPendingWifiRequest) return;
	const auto network = this->mPendingWifiNetwork;
	const auto accepted = this->mPendingWifiAccepted;
	this->mPendingWifiRequest = 0;
	this->mPendingWifiAccepted = false;
	this->bWifiConnecting = false;
	if (this->mPendingWifiRequest == 0) this->bWifiConnectingSsid = QString();
	this->updateWifiStatus();
	this->publishWifiCommand(
	    accepted ? QString::number(requestId) : QString(),
	    wifi_identity::selectionId(network),
	    success && accepted ? QStringLiteral("observed")
	    : accepted          ? QStringLiteral("failed")
	                        : QStringLiteral("denied"),
	    success && accepted ? QString() : reason
	);
	if (this->mPendingWifiRequest == 0)
		emit this->wifiConnectResult(network.ssid, success && accepted, reason);
}

} // namespace qs::windows::sys
