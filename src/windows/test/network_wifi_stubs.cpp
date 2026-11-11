// Link stubs for the network snapshot fixture. The fixture drives the real
// Network publish path and its networks model. Backend objects are constructed
// but observation never starts. The native identity fixture supplies the real
// WLAN backend with fake Windows entry points and uses only the connectivity
// stubs below. Neither variant uses the live WLAN service or WinRT.
#include <qstring.h>
#include <functional>

#include "../system/network.hpp"
#include "../system/network_connectivity.hpp"
#include "../system/network_wifi.hpp"

namespace qs::windows {
void runLauncher(std::function<void()>) { qFatal("Unexpected settings launch in WLAN fixture"); }
}

namespace qs::windows::sys {

#ifndef QS_WIFI_NATIVE_FIXTURE
NetworkWifiBackend::NetworkWifiBackend(Network* owner): mOwner(owner) {}
NetworkWifiBackend::~NetworkWifiBackend() = default;

void NetworkWifiBackend::start() {}
void NetworkWifiBackend::refreshRadioState() {}
void NetworkWifiBackend::setRadioEnabled(bool) {}
void NetworkWifiBackend::scan() {}
void NetworkWifiBackend::refreshAvailableNetworks() {}
void NetworkWifiBackend::refreshCurrentConnection() {}
void NetworkWifiBackend::connectToNetwork(const RawWifiNetwork&, const QString&, quint64) {}
void NetworkWifiBackend::cancelConnection() {}
void NetworkWifiBackend::disconnectActive() {}
void NetworkWifiBackend::forgetNetwork(const RawWifiNetwork&) {}
#endif

NetworkConnectivityBackend::NetworkConnectivityBackend(Network* owner): mOwner(owner) {}
NetworkConnectivityBackend::~NetworkConnectivityBackend() = default;

void NetworkConnectivityBackend::start() {}
void NetworkConnectivityBackend::refreshConnectivity() {}
void NetworkConnectivityBackend::refreshEthernet() {}

} // namespace qs::windows::sys
