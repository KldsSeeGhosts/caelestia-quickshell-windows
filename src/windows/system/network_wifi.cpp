#include "network_wifi.hpp"
#include <vector>

#include <qcoreapplication.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qrunnable.h>
#include <qset.h>
#include <qstringbuilder.h>
#include <qstringview.h>
#include <qthreadpool.h>
#include <shellapi.h>

#include "../../core/backgroundpool.hpp"
#include "../../core/logcat.hpp"
#include "../startup.hpp"
#include "../util.hpp"
#include "network.hpp"
#include "network_wifi_route.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logWifi, "quickshell.windows.network.wifi", QtWarningMsg);

QString macToString(const DOT11_MAC_ADDRESS& mac) {
	return QStringLiteral("%1:%2:%3:%4:%5:%6")
	    .arg(static_cast<uint>(mac[0]), 2, 16, QChar('0'))
	    .arg(static_cast<uint>(mac[1]), 2, 16, QChar('0'))
	    .arg(static_cast<uint>(mac[2]), 2, 16, QChar('0'))
	    .arg(static_cast<uint>(mac[3]), 2, 16, QChar('0'))
	    .arg(static_cast<uint>(mac[4]), 2, 16, QChar('0'))
	    .arg(static_cast<uint>(mac[5]), 2, 16, QChar('0'))
	    .toUpper();
}

} // namespace

NetworkWifiBackend::NetworkWifiBackend(Network* owner): mOwner(owner) {}

NetworkWifiBackend::~NetworkWifiBackend() { this->closeHandle(); }

void NetworkWifiBackend::closeHandle() {
	this->cancelConnection();
	if (this->mHandle != nullptr) {
		WlanCloseHandle(this->mHandle, nullptr);
		this->mHandle = nullptr;
	}
	this->mHasAdapter = false;
}

void NetworkWifiBackend::start() {
	startup::afterFirstFrame(this, [this]() { this->open(); });
}

void NetworkWifiBackend::open() {
	auto guard = QPointer<NetworkWifiBackend>(this);
	auto* task = QRunnable::create([guard]() {
		HANDLE handle = nullptr;
		DWORD negotiatedVersion = 0;
		auto result = WlanOpenHandle(2, nullptr, &negotiatedVersion, &handle);

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, result, handle]() {
			    if (guard == nullptr) {
				    if (handle != nullptr) WlanCloseHandle(handle, nullptr);
				    return;
			    }
			    guard->finishOpen(result, handle);
		    },
		    Qt::QueuedConnection
		);
	});

	BackgroundThreadPool::instance()->start(task);
}

void NetworkWifiBackend::finishOpen(DWORD openResult, HANDLE handle) {
	if (openResult != ERROR_SUCCESS || handle == nullptr) {
		if (handle != nullptr) WlanCloseHandle(handle, nullptr);
		auto code = openResult != ERROR_SUCCESS ? openResult : ERROR_INVALID_HANDLE;
		qCWarning(logWifi) << "WlanOpenHandle failed:" << code;
		this->publishInitialized(
		    QStringLiteral("Windows WLAN could not be opened (error %1).").arg(code)
		);
		return;
	}

	this->mHandle = handle;

	// A failed enumeration is a backend error, not an absent adapter. Only an
	// empty interface list means the machine has no WLAN interface.
	DWORD enumError = ERROR_SUCCESS;
	if (!this->selectInterface(enumError)) {
		qCWarning(logWifi) << "WlanEnumInterfaces failed:" << enumError;
		this->publishInitialized(
		    QStringLiteral("Windows could not enumerate WLAN interfaces (error %1).").arg(enumError)
		);
		return;
	}

	// Notifications carry every interface's state; the callback filters by GUID.
	// Subscribe even without an interface now, so a later arrival is observed.
	// Without the subscription this backend cannot stay truthful, so a refusal
	// is published as an error instead of a ready state.
	auto subscribeResult = WlanRegisterNotification(
	    this->mHandle,
	    WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM,
	    TRUE,
	    &NetworkWifiBackend::notificationCallback,
	    this,
	    nullptr,
	    nullptr
	);
	if (subscribeResult != ERROR_SUCCESS) {
		qCWarning(logWifi) << "WlanRegisterNotification failed:" << subscribeResult;
		this->mOwner->backendSetWifiAdapterPresent(this->mHasAdapter);
		this->publishInitialized(
		    QStringLiteral("Windows refused WLAN notifications (error %1).").arg(subscribeResult)
		);
		return;
	}

	if (!this->mHasAdapter) {
		this->publishInitialized(QString());
		return;
	}

	this->mOwner->backendSetWifiAdapterPresent(true);
	this->refreshRadioState();
	this->refreshCurrentConnection();
	this->refreshAvailableNetworks();
	this->publishInitialized(QString());
}

void NetworkWifiBackend::publishInitialized(const QString& error) {
	this->mOwner->backendSetWifiBackendError(error);
	this->mOwner->backendSetWifiInitialized(true);
}

bool NetworkWifiBackend::selectInterface(DWORD& error, const GUID* exclude) {
	PWLAN_INTERFACE_INFO_LIST ifaceList = nullptr;
	auto result = WlanEnumInterfaces(this->mHandle, nullptr, &ifaceList);
	if (result != ERROR_SUCCESS) {
		error = result;
		return false;
	}

	std::vector<wifi_route::InterfaceCandidate> candidates;
	if (ifaceList != nullptr) {
		candidates.reserve(ifaceList->dwNumberOfItems);
		for (DWORD i = 0; i < ifaceList->dwNumberOfItems; i++) {
			const auto& info = ifaceList->InterfaceInfo[i]; // NOLINT
			if (exclude != nullptr && IsEqualGUID(info.InterfaceGuid, *exclude)) continue;
			candidates.push_back({info.InterfaceGuid, info.isState});
		}
		WlanFreeMemory(ifaceList);
	}

	auto selection = wifi_route::nextSelection(
	    this->mInterfaceGuid,
	    this->mHasAdapter,
	    candidates.data(),
	    candidates.size()
	);
	this->mHasAdapter = selection.present;
	this->mInterfaceGuid = selection.guid;
	return true;
}

void NetworkWifiBackend::adoptSelectedInterface(const GUID* exclude) {
	DWORD error = ERROR_SUCCESS;
	if (!this->selectInterface(error, exclude)) {
		qCWarning(logWifi) << "WlanEnumInterfaces failed while re-selecting:" << error;
		this->clearSelectedInterface();
		return;
	}

	if (!this->mHasAdapter) {
		this->clearSelectedInterface();
		return;
	}

	this->mOwner->backendSetWifiAdapterPresent(true);
	this->refreshRadioState();
	this->refreshCurrentConnection();
	this->refreshAvailableNetworks();
}

void NetworkWifiBackend::clearSelectedInterface() {
	// The selected interface is gone or no longer enumerable, so a connection
	// pending on it can never complete. End it as a failure before clearing the
	// rest of the state; otherwise the shell stays busy and rejects new requests.
	this->cancelConnection();
	this->mHasAdapter = false;
	this->mInterfaceGuid = {};
	this->mOwner->backendEndPendingConnection();
	this->mScanRequested = false;
	this->mOwner->backendSetWifiScanning(false);
	this->mOwner->backendSetWifiRadioOn(false);
	this->mOwner->backendSetWifiAdapterPresent(false);
	this->mOwner->backendSetCurrentConnection({}, false, false);
	this->mOwner->backendUpdateAvailableNetworks({});
}

bool NetworkWifiBackend::ownsInterface(const GUID& guid) const {
	return this->mHasAdapter && wifi_route::matchesSelected(this->mInterfaceGuid, guid);
}

void NetworkWifiBackend::refreshRadioState() {
	if (!this->mHasAdapter) return;

	DWORD size = 0;
	PVOID data = nullptr;
	auto result = WlanQueryInterface(
	    this->mHandle,
	    &this->mInterfaceGuid,
	    wlan_intf_opcode_radio_state,
	    nullptr,
	    &size,
	    &data,
	    nullptr
	);
	if (result != ERROR_SUCCESS || data == nullptr) {
		qCDebug(logWifi) << "WlanQueryInterface(radio_state) failed:" << result;
		return;
	}

	auto* state = static_cast<PWLAN_RADIO_STATE>(data);
	auto on = false;
	for (DWORD i = 0; i < state->dwNumberOfPhys; i++) {
		const auto& phy = state->PhyRadioState[i]; // NOLINT
		if (phy.dot11SoftwareRadioState == dot11_radio_state_on
		    && phy.dot11HardwareRadioState == dot11_radio_state_on)
		{
			on = true;
			break;
		}
	}
	WlanFreeMemory(data);

	this->mOwner->backendSetWifiRadioOn(on);
}

void NetworkWifiBackend::setRadioEnabled(bool enabled) {
	if (!this->mHasAdapter) {
		runLauncher([] {
			ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL);
		});
		return;
	}

	WLAN_PHY_RADIO_STATE state {};
	state.dwPhyIndex = 0;
	state.dot11SoftwareRadioState = enabled ? dot11_radio_state_on : dot11_radio_state_off;

	auto result = WlanSetInterface(
	    this->mHandle,
	    &this->mInterfaceGuid,
	    wlan_intf_opcode_radio_state,
	    sizeof(state),
	    &state,
	    nullptr
	);
	if (result != ERROR_SUCCESS) {
		qCWarning(logWifi) << "WlanSetInterface(radio_state) failed (needs admin?):" << result
		                   << "-- opening Wi-Fi settings instead";
		runLauncher([] {
			ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL);
		});
		return;
	}

	this->refreshRadioState();
}

void NetworkWifiBackend::scan() {
	if (!this->mHasAdapter) return;

	this->mOwner->backendSetWifiScanning(true);
	this->mScanRequested = true;
	auto result = WlanScan(this->mHandle, &this->mInterfaceGuid, nullptr, nullptr, nullptr);
	if (result != ERROR_SUCCESS) {
		qCDebug(logWifi) << "WlanScan failed:" << result;
		this->mScanRequested = false;
		this->mOwner->backendSetWifiScanning(false);
	}
}

void NetworkWifiBackend::refreshAvailableNetworks() {
	if (!this->mHasAdapter) return;

	PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
	auto result =
	    WlanGetAvailableNetworkList(this->mHandle, &this->mInterfaceGuid, 0, nullptr, &list);
	if (result == ERROR_ACCESS_DENIED) {
		this->mOwner->backendSetNeedsLocationPermission(true);
		return;
	}
	if (result != ERROR_SUCCESS || list == nullptr) {
		qCDebug(logWifi) << "WlanGetAvailableNetworkList failed:" << result;
		return;
	}
	this->mOwner->backendSetNeedsLocationPermission(false);

	QHash<QString, RawWifiNetwork> byIdentity;
	for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
		const auto& net = list->Network[i]; // NOLINT
		RawWifiNetwork raw;
		raw.ssidBytes = wifi_identity::ssidBytes(net.dot11Ssid);
		if (!wifi_identity::validSsid(raw.ssidBytes)) continue;
		raw.ssid = QString::fromUtf8(raw.ssidBytes);
		raw.interfaceGuid = this->mInterfaceGuid;
		raw.authentication = net.dot11DefaultAuthAlgorithm;
		raw.cipher = net.dot11DefaultCipherAlgorithm;
		raw.bssType = net.dot11BssType;
		raw.securityEnabled = net.bSecurityEnabled != FALSE;
		raw.strength = static_cast<int>(net.wlanSignalQuality);
		raw.active = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
		raw.hasProfile = (net.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
		raw.security = wifi_identity::securityLabel(raw);
		if (raw.hasProfile) raw.profileName = QString::fromWCharArray(net.strProfileName);

		const auto key = wifi_identity::selectionId(raw);
		auto existing = byIdentity.constFind(key);
		if (existing == byIdentity.constEnd()) {
			byIdentity.insert(key, raw);
		} else if (raw.active && !existing->active) {
			byIdentity.insert(key, raw);
		} else if (!raw.active && !existing->active && raw.strength > existing->strength) {
			byIdentity.insert(key, raw);
		}
	}
	WlanFreeMemory(list);

	// Windows can enumerate a saved profile and a profileless representation
	// of the same available network. Keep distinct saved profiles and security
	// variants, but do not offer the redundant representation as a new network.
	QSet<QString> savedNetworks;
	for (const auto& entry: byIdentity) {
		if (!entry.hasProfile) continue;
		auto network = entry;
		network.hasProfile = false;
		network.profileName.clear();
		savedNetworks.insert(wifi_identity::selectionId(network));
	}
	for (auto entry = byIdentity.begin(); entry != byIdentity.end();) {
		if (!entry->hasProfile && savedNetworks.contains(entry.key())) entry = byIdentity.erase(entry);
		else ++entry;
	}

	PWLAN_BSS_LIST bssList = nullptr;
	result = WlanGetNetworkBssList(
	    this->mHandle,
	    &this->mInterfaceGuid,
	    nullptr,
	    dot11_BSS_type_any,
	    FALSE,
	    nullptr,
	    &bssList
	);
	if (result == ERROR_ACCESS_DENIED) {
		this->mOwner->backendSetNeedsLocationPermission(true);
	} else if (result == ERROR_SUCCESS && bssList != nullptr) {
		QHash<QByteArray, LONG> bestRssi;
		for (DWORD i = 0; i < bssList->dwNumberOfItems; i++) {
			const auto& bss = bssList->wlanBssEntries[i]; // NOLINT
			const auto bytes = wifi_identity::ssidBytes(bss.dot11Ssid);
			if (!wifi_identity::validSsid(bytes)) continue;
			auto best = bestRssi.constFind(bytes);
			if (best != bestRssi.constEnd() && *best >= bss.lRssi) continue;
			bestRssi[bytes] = bss.lRssi;
			for (auto& raw: byIdentity) {
				if (raw.ssidBytes != bytes) continue;
				raw.bssid = macToString(bss.dot11Bssid);
				raw.frequency = static_cast<int>(bss.ulChCenterFrequency / 1000);
			}
		}
		WlanFreeMemory(bssList);
	}

	std::vector<RawWifiNetwork> raw;
	raw.reserve(static_cast<size_t>(byIdentity.size()));
	for (const auto& network: byIdentity) raw.push_back(network);

	// One completed query, one snapshot: the model holds these rows before the
	// generation advances, so a consumer may read it from the change handler.
	this->mOwner->backendPublishAvailableNetworks(raw);
}

void NetworkWifiBackend::refreshCurrentConnection() {
	if (!this->mHasAdapter) {
		this->mOwner->backendSetCurrentConnection({}, false, false);
		return;
	}

	DWORD size = 0;
	PVOID data = nullptr;
	auto result = WlanQueryInterface(
	    this->mHandle,
	    &this->mInterfaceGuid,
	    wlan_intf_opcode_current_connection,
	    nullptr,
	    &size,
	    &data,
	    nullptr
	);
	if (result == ERROR_ACCESS_DENIED) {
		this->mOwner->backendSetNeedsLocationPermission(true);
		return;
	}
	if (result != ERROR_SUCCESS || data == nullptr) {
		this->mOwner->backendSetCurrentConnection({}, false, false);
		return;
	}

	auto* conn = static_cast<PWLAN_CONNECTION_ATTRIBUTES>(data);
	auto connected = conn->isState == wlan_interface_state_connected;
	auto connecting = conn->isState == wlan_interface_state_associating
	               || conn->isState == wlan_interface_state_discovering
	               || conn->isState == wlan_interface_state_authenticating;
	RawWifiNetwork current;
	current.ssidBytes = wifi_identity::ssidBytes(conn->wlanAssociationAttributes.dot11Ssid);
	current.ssid = QString::fromUtf8(current.ssidBytes);
	current.interfaceGuid = this->mInterfaceGuid;
	current.bssid = macToString(conn->wlanAssociationAttributes.dot11Bssid);
	current.strength = static_cast<int>(conn->wlanAssociationAttributes.wlanSignalQuality);
	current.authentication = conn->wlanSecurityAttributes.dot11AuthAlgorithm;
	current.cipher = conn->wlanSecurityAttributes.dot11CipherAlgorithm;
	current.securityEnabled = conn->wlanSecurityAttributes.bSecurityEnabled != FALSE;
	current.security = wifi_identity::securityLabel(current);
	current.bssType = conn->wlanAssociationAttributes.dot11BssType;
	current.profileName = QString::fromWCharArray(conn->strProfileName);
	current.hasProfile = !current.profileName.isEmpty();
	WlanFreeMemory(data);

	this->mOwner->backendSetCurrentConnection(current, connected, connecting);
}

void NetworkWifiBackend::connectToNetwork(
    const RawWifiNetwork& network,
    const QString& password,
    quint64 requestId
) {
	if (!this->ownsInterface(network.interfaceGuid) || !wifi_identity::validSsid(network.ssidBytes)) {
		this->mOwner->backendWifiConnectResult(requestId, false, QStringLiteral("stale-selection"));
		return;
	}

	auto targetProfile = network.profileName;

	if (!network.hasProfile) {
		const auto profile = wifi_identity::newProfile(network, password);
		if (!profile.reason.isEmpty()) {
			this->mOwner->backendWifiConnectResult(requestId, false, profile.reason);
			return;
		}
		targetProfile = profile.name;
		auto wxml = profile.xml.toStdWString();
		DWORD reason = 0;
		auto setResult = WlanSetProfile(
		    this->mHandle,
		    &this->mInterfaceGuid,
		    0,
		    wxml.c_str(),
		    nullptr,
		    FALSE,
		    nullptr,
		    &reason
		);
		if (setResult != ERROR_SUCCESS) {
			qCWarning(logWifi) << "WlanSetProfile failed:" << setResult << "reason" << reason;
			this->mOwner->backendWifiConnectResult(requestId, false, QStringLiteral("profile-rejected"));
			return;
		}
	} else if (targetProfile.isEmpty() || targetProfile.contains(QChar(u'\0'))) {
		this->mOwner->backendWifiConnectResult(requestId, false, QStringLiteral("invalid-profile"));
		return;
	}

	this->mPendingConnection = PendingConnection {network, targetProfile, requestId};
	this->mCallbackRequestId.store(requestId);
	auto profile = targetProfile.toStdWString();
	WLAN_CONNECTION_PARAMETERS params {};
	params.wlanConnectionMode = wlan_connection_mode_profile;
	params.strProfile = profile.c_str();
	params.pDot11Ssid = nullptr;
	params.pDesiredBssidList = nullptr;
	params.dot11BssType = network.bssType;
	params.dwFlags = 0;

	auto result = WlanConnect(this->mHandle, &this->mInterfaceGuid, &params, nullptr);
	if (result != ERROR_SUCCESS) {
		this->cancelConnection();
		qCWarning(logWifi) << "WlanConnect failed:" << result;
		this->mOwner->backendWifiConnectResult(requestId, false, QStringLiteral("command-rejected"));
		return;
	}
	this->mOwner->backendWifiConnectAccepted(requestId);
}

void NetworkWifiBackend::cancelConnection() {
	this->mCallbackRequestId.store(0);
	this->mPendingConnection.reset();
}

void NetworkWifiBackend::disconnectActive() {
	this->cancelConnection();
	if (!this->mHasAdapter) return;
	WlanDisconnect(this->mHandle, &this->mInterfaceGuid, nullptr);
}

void NetworkWifiBackend::forgetNetwork(const RawWifiNetwork& network) {
	if (!this->ownsInterface(network.interfaceGuid) || !network.hasProfile
	    || network.profileName.isEmpty() || network.profileName.contains(QChar(u'\0')))
		return;
	auto name = network.profileName.toStdWString();
	WlanDeleteProfile(this->mHandle, &this->mInterfaceGuid, name.c_str(), nullptr);
}

void WINAPI NetworkWifiBackend::notificationCallback(PWLAN_NOTIFICATION_DATA data, PVOID context) {
	if (data == nullptr || context == nullptr) return;
	auto* self = static_cast<NetworkWifiBackend*>(context);
	auto guid = data->InterfaceGuid;

	if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM) {
		auto code = data->NotificationCode;

		if ((code == wlan_notification_acm_connection_complete
		     || code == wlan_notification_acm_connection_attempt_fail)
		    && data->pData != nullptr
		    && data->dwDataSize >= offsetof(WLAN_CONNECTION_NOTIFICATION_DATA, strProfileXml))
		{
			const auto* connData = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData);
			auto bytes = wifi_identity::ssidBytes(connData->dot11Ssid);
			auto profile = QString::fromWCharArray(
			    connData->strProfileName,
			    static_cast<qsizetype>(wcsnlen(connData->strProfileName, WLAN_MAX_NAME_LENGTH))
			);
			const auto requestId = self->mCallbackRequestId.load();
			auto success = code == wlan_notification_acm_connection_complete
			            && connData->wlanReasonCode == WLAN_REASON_CODE_SUCCESS;
			auto reasonCode = connData->wlanReasonCode;

			QMetaObject::invokeMethod(
			    self,
			    [self, guid, bytes, profile, requestId, success, reasonCode]() {
				    if (!self->ownsInterface(guid)) return;
				    const auto& pending = self->mPendingConnection;
				    if (requestId != 0 && pending && pending->requestId == requestId
				        && pending->network.ssidBytes == bytes && pending->profileName == profile)
				    {
					    self->cancelConnection();
					    self->mOwner->backendWifiConnectResult(
					        requestId,
					        success,
					        success                                  ? QString()
					        : reasonCode != WLAN_REASON_CODE_SUCCESS ? QStringLiteral("auth")
					                                                 : QStringLiteral("other")
					    );
				    }
				    // Windows UI and other clients also change the connection.
				    // Observe those events without claiming our command completed.
				    self->refreshCurrentConnection();
				    self->refreshAvailableNetworks();
			    },
			    Qt::QueuedConnection
			);
			return;
		}

		QMetaObject::invokeMethod(
		    self,
		    [self, code, guid]() { self->handleAcmNotification(code, guid); },
		    Qt::QueuedConnection
		);
	} else if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_MSM) {
		auto code = data->NotificationCode;
		QMetaObject::invokeMethod(
		    self,
		    [self, code, guid]() { self->handleMsmNotification(code, guid); },
		    Qt::QueuedConnection
		);
	}
}

void NetworkWifiBackend::handleAcmNotification(DWORD code, const GUID& guid) {
	// Arrival and removal change the interface set itself. They are handled
	// before the per-interface filter: only this backend's interface being
	// removed may clear its state, and an arrival is only adopted while no
	// interface is selected.
	switch (code) {
	case wlan_notification_acm_interface_removal:
		if (!this->ownsInterface(guid)) return;
		// Honor the removal immediately, then adopt a real replacement. The
		// removed GUID is excluded because an enumeration racing the
		// notification may still list it.
		this->clearSelectedInterface();
		this->adoptSelectedInterface(&guid);
		return;
	case wlan_notification_acm_interface_arrival:
		if (!this->mHasAdapter) this->adoptSelectedInterface();
		return;
	default: break;
	}

	if (!this->ownsInterface(guid)) return;

	switch (code) {
	case wlan_notification_acm_scan_complete:
		this->mOwner->backendSetWifiScanning(false);
		if (this->mScanRequested || this->mOwner->wifiListVisible()) {
			this->mScanRequested = false;
			this->refreshAvailableNetworks();
		}
		break;
	case wlan_notification_acm_scan_fail:
		this->mScanRequested = false;
		this->mOwner->backendSetWifiScanning(false);
		break;
	case wlan_notification_acm_disconnecting:
	case wlan_notification_acm_disconnected:
		this->refreshCurrentConnection();
		this->refreshAvailableNetworks();
		break;
	case wlan_notification_acm_profile_change:
	case wlan_notification_acm_profile_name_change: this->refreshAvailableNetworks(); break;
	default: break;
	}
}

void NetworkWifiBackend::handleMsmNotification(DWORD code, const GUID& guid) {
	if (!this->ownsInterface(guid)) return;

	switch (code) {
	case wlan_notification_msm_radio_state_change: this->refreshRadioState(); break;
	case wlan_notification_msm_connected:
	case wlan_notification_msm_disconnected:
		this->refreshCurrentConnection();
		this->refreshAvailableNetworks();
		break;
	case wlan_notification_msm_signal_quality_change: this->refreshCurrentConnection(); break;
	default: break;
	}
}

} // namespace qs::windows::sys
