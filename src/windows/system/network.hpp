#pragma once

#include <memory>
#include <vector>

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../core/doc.hpp"
#include "../../core/model.hpp"
#include "network_wifi.hpp"
#include "network_wifi_item.hpp"

namespace qs::windows::sys {

class NetworkConnectivityBackend;

class Network: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(bool hasInternet READ default NOTIFY hasInternetChanged BINDABLE bindableHasInternet);
	Q_PROPERTY(bool ethernetConnected READ default NOTIFY ethernetConnectedChanged BINDABLE bindableEthernetConnected);
	Q_PROPERTY(bool wifiAdapterPresent READ default NOTIFY wifiAdapterPresentChanged BINDABLE bindableWifiAdapterPresent);
	Q_PROPERTY(bool wifiInitialized READ default NOTIFY wifiInitializedChanged BINDABLE bindableWifiInitialized);
	Q_PROPERTY(QString wifiBackendError READ default NOTIFY wifiBackendErrorChanged BINDABLE bindableWifiBackendError);
	Q_PROPERTY(bool wifiRadioOn READ default NOTIFY wifiRadioOnChanged BINDABLE bindableWifiRadioOn);
	Q_PROPERTY(bool wifiScanning READ default NOTIFY wifiScanningChanged BINDABLE bindableWifiScanning);
	Q_PROPERTY(bool wifiConnecting READ default NOTIFY wifiConnectingChanged BINDABLE bindableWifiConnecting);
	Q_PROPERTY(QString wifiConnectingSsid READ default NOTIFY wifiConnectingSsidChanged BINDABLE bindableWifiConnectingSsid);
	Q_PROPERTY(QString activeSsid READ default NOTIFY activeSsidChanged BINDABLE bindableActiveSsid);
	Q_PROPERTY(QString activeBssid READ default NOTIFY activeBssidChanged BINDABLE bindableActiveBssid);
	Q_PROPERTY(int activeSignalQuality READ default NOTIFY activeSignalQualityChanged BINDABLE bindableActiveSignalQuality);
	Q_PROPERTY(QString activeSecurity READ default NOTIFY activeSecurityChanged BINDABLE bindableActiveSecurity);
	Q_PROPERTY(QString wifiStatus READ default NOTIFY wifiStatusChanged BINDABLE bindableWifiStatus);
	// Advanced once per completed available-network refresh, after the networks
	// model holds the new rows. A consumer that reads the model from this
	// property's change handler therefore sees the rows that refresh published.
	Q_PROPERTY(int wifiSnapshot READ default NOTIFY wifiSnapshotChanged BINDABLE bindableWifiSnapshot);
	Q_PROPERTY(bool needsLocationPermission READ default NOTIFY needsLocationPermissionChanged BINDABLE bindableNeedsLocationPermission);
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::windows::sys::NetworkWifiNetwork>*);
	Q_PROPERTY(UntypedObjectModel* networks READ networks CONSTANT);
	// clang-format on
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Network(QObject* parent = nullptr);
	~Network() override;
	Q_DISABLE_COPY_MOVE(Network);

#ifdef QS_TEST
	// State only: no WLAN or connectivity observation starts, so a fixture can
	// exercise the publish path without touching the live service.
	static Network* createForTesting();
#endif

	[[nodiscard]] UntypedObjectModel* networks() { return &this->mNetworks; }

	[[nodiscard]] QBindable<bool> bindableHasInternet() const { return &this->bHasInternet; }
	[[nodiscard]] QBindable<bool> bindableEthernetConnected() const {
		return &this->bEthernetConnected;
	}
	[[nodiscard]] QBindable<bool> bindableWifiAdapterPresent() const {
		return &this->bWifiAdapterPresent;
	}
	[[nodiscard]] QBindable<bool> bindableWifiInitialized() const { return &this->bWifiInitialized; }
	[[nodiscard]] QBindable<QString> bindableWifiBackendError() const {
		return &this->bWifiBackendError;
	}
	[[nodiscard]] QBindable<bool> bindableWifiRadioOn() const { return &this->bWifiRadioOn; }
	[[nodiscard]] QBindable<bool> bindableWifiScanning() const { return &this->bWifiScanning; }
	[[nodiscard]] QBindable<bool> bindableWifiConnecting() const { return &this->bWifiConnecting; }
	[[nodiscard]] QBindable<QString> bindableWifiConnectingSsid() const {
		return &this->bWifiConnectingSsid;
	}
	[[nodiscard]] QBindable<QString> bindableActiveSsid() const { return &this->bActiveSsid; }
	[[nodiscard]] QBindable<QString> bindableActiveBssid() const { return &this->bActiveBssid; }
	[[nodiscard]] QBindable<int> bindableActiveSignalQuality() const {
		return &this->bActiveSignalQuality;
	}
	[[nodiscard]] QBindable<QString> bindableActiveSecurity() const {
		return &this->bActiveSecurity;
	}
	[[nodiscard]] QBindable<QString> bindableWifiStatus() const { return &this->bWifiStatus; }
	[[nodiscard]] QBindable<int> bindableWifiSnapshot() const { return &this->bWifiSnapshot; }
	[[nodiscard]] QBindable<bool> bindableNeedsLocationPermission() const {
		return &this->bNeedsLocationPermission;
	}

	Q_INVOKABLE void setWifiRadioEnabled(bool enabled);
	Q_INVOKABLE void scanWifiNetworks();
	Q_INVOKABLE void connectToNetwork(const QString& ssid, const QString& password);
	Q_INVOKABLE void disconnectActive();
	Q_INVOKABLE void forgetNetwork(const QString& ssid);
	Q_INVOKABLE static void openWifiSettings();
	Q_INVOKABLE static void openLocationSettings();
	Q_INVOKABLE void setWifiListVisible(bool visible);

	[[nodiscard]] bool wifiListVisible() const { return this->mWifiListVisible; }

	void backendSetHasInternet(bool hasInternet);
	void backendSetEthernetConnected(bool connected);
	void backendSetWifiAdapterPresent(bool present);
	void backendSetWifiInitialized(bool initialized);
	void backendSetWifiBackendError(const QString& error);
	void backendSetWifiRadioOn(bool on);
	void backendSetWifiScanning(bool scanning);
	void backendSetNeedsLocationPermission(bool needs);
	void backendSetCurrentConnection(
	    const QString& ssid,
	    const QString& bssid,
	    int signalQuality,
	    const QString& security,
	    bool connected,
	    bool connecting
	);
	void backendUpdateAvailableNetworks(const std::vector<RawWifiNetwork>& networks);
	// One completed available-network refresh. The networks model is reconciled
	// with these rows first, then wifiSnapshot advances once, even when the rows
	// are unchanged: a refresh that found the same access points is still a new
	// snapshot a consumer may use to confirm an absence.
	void backendPublishAvailableNetworks(const std::vector<RawWifiNetwork>& networks);
	void backendEndPendingConnection();
	void backendWifiConnectResult(const QString& ssid, bool success, const QString& reason);

signals:
	void hasInternetChanged();
	void ethernetConnectedChanged();
	void wifiAdapterPresentChanged();
	void wifiInitializedChanged();
	void wifiBackendErrorChanged();
	void wifiRadioOnChanged();
	void wifiScanningChanged();
	void wifiConnectingChanged();
	void wifiConnectingSsidChanged();
	void activeSsidChanged();
	void activeBssidChanged();
	void activeSignalQualityChanged();
	void activeSecurityChanged();
	void wifiStatusChanged();
	void wifiSnapshotChanged();
	void needsLocationPermissionChanged();
	void wifiConnectResult(const QString& ssid, bool success, const QString& reason);

private:
	explicit Network(QObject* parent, bool observe);
	void updateWifiStatus();
	[[nodiscard]] NetworkWifiNetwork* findNetwork(const QString& ssid) const;

	std::unique_ptr<NetworkWifiBackend> mWifi;
	std::unique_ptr<NetworkConnectivityBackend> mConnectivity;
	ObjectModel<NetworkWifiNetwork> mNetworks {this};

	bool mWifiListVisible = false;
	QTimer mScanTimer;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bHasInternet, &Network::hasInternetChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bEthernetConnected, &Network::ethernetConnectedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bWifiAdapterPresent, &Network::wifiAdapterPresentChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bWifiInitialized, &Network::wifiInitializedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, QString, bWifiBackendError, &Network::wifiBackendErrorChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bWifiRadioOn, &Network::wifiRadioOnChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bWifiScanning, &Network::wifiScanningChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bWifiConnecting, &Network::wifiConnectingChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, QString, bWifiConnectingSsid, &Network::wifiConnectingSsidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, QString, bActiveSsid, &Network::activeSsidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, QString, bActiveBssid, &Network::activeBssidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, int, bActiveSignalQuality, &Network::activeSignalQualityChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, QString, bActiveSecurity, &Network::activeSecurityChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, QString, bWifiStatus, &Network::wifiStatusChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, int, bWifiSnapshot, &Network::wifiSnapshotChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Network, bool, bNeedsLocationPermission, &Network::needsLocationPermissionChanged);
	// clang-format on
};

} // namespace qs::windows::sys
