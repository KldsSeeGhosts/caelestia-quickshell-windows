#include "network_wifi_item.hpp"

namespace qs::windows::sys {

void NetworkWifiNetwork::setNetwork(const RawWifiNetwork& network) {
	const auto previous = this->selectionId();
	this->mNetwork = network;
	this->setSsid(network.ssid);
	this->setBssid(network.bssid);
	this->setStrength(network.strength);
	this->setFrequency(network.frequency);
	this->setActive(network.active);
	this->setSecurity(wifi_identity::securityLabel(network));
	this->setHasProfile(network.hasProfile);
	this->setProfileName(network.profileName);
	if (previous != this->selectionId()) emit this->identityChanged();
}

} // namespace qs::windows::sys
