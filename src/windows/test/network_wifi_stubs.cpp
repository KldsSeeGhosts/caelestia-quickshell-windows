// Link stubs for the network snapshot fixture. The fixture drives the real
// Network publish path and its networks model; the WLAN and connectivity
// backends are not constructed (Network::createForTesting) and are never
// called, so the live service, WLAN notifications and WinRT are absent.
#include <qstring.h>

#include "../system/network.hpp"
#include "../system/network_connectivity.hpp"
#include "../system/network_wifi.hpp"

namespace qs::windows::sys {

NetworkWifiBackend::NetworkWifiBackend(Network* owner): mOwner(owner) {}
NetworkWifiBackend::~NetworkWifiBackend() = default;

void NetworkWifiBackend::start() {}
void NetworkWifiBackend::refreshRadioState() {}
void NetworkWifiBackend::setRadioEnabled(bool) {}
void NetworkWifiBackend::scan() {}
void NetworkWifiBackend::refreshAvailableNetworks() {}
void NetworkWifiBackend::refreshCurrentConnection() {}
void NetworkWifiBackend::connectToNetwork(
    const QString&,
    const QString&,
    const QString&,
    bool,
    bool,
    bool
) {}
void NetworkWifiBackend::disconnectActive() {}
void NetworkWifiBackend::forgetNetwork(const QString&, const QString&) {}

NetworkConnectivityBackend::NetworkConnectivityBackend(Network* owner): mOwner(owner) {}
NetworkConnectivityBackend::~NetworkConnectivityBackend() = default;

void NetworkConnectivityBackend::start() {}
void NetworkConnectivityBackend::refreshConnectivity() {}
void NetworkConnectivityBackend::refreshEthernet() {}

} // namespace qs::windows::sys
