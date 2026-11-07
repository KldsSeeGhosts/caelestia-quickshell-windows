#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <qthreadpool.h>

#include "../../core/backgroundpool.hpp"
#include "../../core/logcat.hpp"
#include "../startup.hpp"
#include "../system/network_wifi.hpp"
#include "network_wifi_native_fixture.hpp"

namespace wifi_fixture {
State state;
void reset() { state = {}; }
void notify(
    qs::windows::sys::NetworkWifiBackend* backend,
    const QByteArray& bytes,
    const QString& profile,
    DWORD code,
    GUID guid,
    DWORD reason
) {
	WLAN_CONNECTION_NOTIFICATION_DATA connection {};
	connection.dot11Ssid.uSSIDLength = static_cast<ULONG>(bytes.size());
	std::memcpy(
	    connection.dot11Ssid.ucSSID,
	    bytes.constData(),
	    std::min<qsizetype>(32, bytes.size())
	);
	const auto wide = profile.toStdWString();
	std::copy_n(
	    wide.data(),
	    std::min<size_t>(wide.size(), WLAN_MAX_NAME_LENGTH - 1),
	    connection.strProfileName
	);
	connection.wlanReasonCode = reason;
	WLAN_NOTIFICATION_DATA data {};
	data.NotificationSource = WLAN_NOTIFICATION_SOURCE_ACM;
	data.NotificationCode = code;
	data.InterfaceGuid = guid;
	data.dwDataSize = sizeof(connection);
	data.pData = &connection;
	backend->notifyForTesting(&data);
}
} // namespace wifi_fixture

QThreadPool* BackgroundThreadPool::instance() { qFatal("Unexpected WLAN worker in fixture"); }
void qs::log::initLogCategoryLevel(const char*, QtMsgType) {}
void qs::windows::startup::afterFirstFrame(QObject*, std::function<void()>) {
	qFatal("Unexpected observation startup in fixture");
}

extern "C" {
DWORD WINAPI WlanOpenHandle(DWORD, PVOID, PDWORD, PHANDLE) {
	qFatal("Unexpected live-style open in fixture");
}
DWORD WINAPI WlanCloseHandle(HANDLE, PVOID) { return ERROR_SUCCESS; }
VOID WINAPI WlanFreeMemory(PVOID memory) { std::free(memory); }
DWORD WINAPI WlanEnumInterfaces(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST* output) {
	auto* list =
	    static_cast<PWLAN_INTERFACE_INFO_LIST>(std::calloc(1, sizeof(WLAN_INTERFACE_INFO_LIST)));
	list->dwNumberOfItems = 1;
	list->InterfaceInfo[0].InterfaceGuid = wifi_fixture::state.selected;
	list->InterfaceInfo[0].isState = wlan_interface_state_disconnected;
	*output = list;
	return ERROR_SUCCESS;
}
DWORD WINAPI
WlanRegisterNotification(HANDLE, DWORD, BOOL, WLAN_NOTIFICATION_CALLBACK, PVOID, PVOID, PDWORD) {
	return ERROR_SUCCESS;
}
DWORD WINAPI WlanQueryInterface(
    HANDLE,
    const GUID*,
    WLAN_INTF_OPCODE,
    PVOID,
    PDWORD,
    PVOID* output,
    PWLAN_OPCODE_VALUE_TYPE
) {
	*output = nullptr;
	return ERROR_NOT_FOUND;
}
DWORD WINAPI WlanSetInterface(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, PVOID, PVOID) {
	qFatal("Unexpected radio mutation in fixture");
}
DWORD WINAPI WlanScan(HANDLE, const GUID*, PDOT11_SSID, PWLAN_RAW_DATA, PVOID) {
	++wifi_fixture::state.scans;
	qFatal("Unexpected scan in fixture");
}
DWORD WINAPI WlanGetAvailableNetworkList(
    HANDLE,
    const GUID*,
    DWORD,
    PVOID,
    PWLAN_AVAILABLE_NETWORK_LIST* output
) {
	const auto& rows = wifi_fixture::state.rows;
	const auto size =
	    sizeof(WLAN_AVAILABLE_NETWORK_LIST) + rows.size() * sizeof(WLAN_AVAILABLE_NETWORK);
	auto* list = static_cast<PWLAN_AVAILABLE_NETWORK_LIST>(std::calloc(1, size));
	list->dwNumberOfItems = static_cast<DWORD>(rows.size());
	for (size_t i = 0; i < rows.size(); ++i) {
		const auto& row = rows[i];
		auto& native = list->Network[i];
		native.dot11Ssid.uSSIDLength = static_cast<ULONG>(row.ssidBytes.size());
		std::memcpy(
		    native.dot11Ssid.ucSSID,
		    row.ssidBytes.constData(),
		    std::min<qsizetype>(32, row.ssidBytes.size())
		);
		native.dot11DefaultAuthAlgorithm = row.authentication;
		native.dot11DefaultCipherAlgorithm = row.cipher;
		native.dot11BssType = row.bssType;
		native.bSecurityEnabled = row.securityEnabled;
		native.wlanSignalQuality = row.strength;
		if (row.active) native.dwFlags |= WLAN_AVAILABLE_NETWORK_CONNECTED;
		if (row.hasProfile) native.dwFlags |= WLAN_AVAILABLE_NETWORK_HAS_PROFILE;
		const auto profile = row.profileName.toStdWString();
		std::copy_n(
		    profile.data(),
		    std::min<size_t>(profile.size(), WLAN_MAX_NAME_LENGTH - 1),
		    native.strProfileName
		);
	}
	*output = list;
	return ERROR_SUCCESS;
}
DWORD WINAPI WlanGetNetworkBssList(
    HANDLE,
    const GUID*,
    PDOT11_SSID,
    DOT11_BSS_TYPE,
    BOOL,
    PVOID,
    PWLAN_BSS_LIST* output
) {
	const auto& rows = wifi_fixture::state.bssRows;
	const auto size = sizeof(WLAN_BSS_LIST) + rows.size() * sizeof(WLAN_BSS_ENTRY);
	auto* list = static_cast<PWLAN_BSS_LIST>(std::calloc(1, size));
	list->dwNumberOfItems = static_cast<DWORD>(rows.size());
	for (size_t i = 0; i < rows.size(); ++i) {
		auto& entry = list->wlanBssEntries[i];
		const auto& row = rows[i];
		entry.dot11Ssid.uSSIDLength = static_cast<ULONG>(row.ssidBytes.size());
		std::memcpy(
		    entry.dot11Ssid.ucSSID,
		    row.ssidBytes.constData(),
		    std::min<qsizetype>(32, row.ssidBytes.size())
		);
		entry.lRssi = row.strength;
		entry.ulChCenterFrequency = static_cast<ULONG>(row.frequency * 1000);
		const auto parts = row.bssid.split(':');
		for (qsizetype part = 0; part < std::min<qsizetype>(6, parts.size()); ++part)
			entry.dot11Bssid[part] = static_cast<UCHAR>(parts[part].toUInt(nullptr, 16));
	}
	*output = list;
	return ERROR_SUCCESS;
}
DWORD WINAPI WlanSetProfile(
    HANDLE,
    const GUID*,
    DWORD,
    LPCWSTR xml,
    LPCWSTR,
    BOOL overwrite,
    PVOID,
    PDWORD reason
) {
	auto& state = wifi_fixture::state;
	++state.profileWrites;
	state.xml = QString::fromWCharArray(xml);
	state.overwrite = overwrite != FALSE;
	*reason = 0;
	return state.profileResult;
}
DWORD WINAPI
WlanConnect(HANDLE, const GUID* guid, const PWLAN_CONNECTION_PARAMETERS params, PVOID) {
	auto& state = wifi_fixture::state;
	++state.connects;
	state.connectedProfile = QString::fromWCharArray(params->strProfile);
	state.connectedAdapter = *guid;
	return state.connectResult;
}
DWORD WINAPI WlanDisconnect(HANDLE, const GUID*, PVOID) {
	++wifi_fixture::state.disconnects;
	return ERROR_SUCCESS;
}
DWORD WINAPI WlanDeleteProfile(HANDLE, const GUID*, LPCWSTR name, PVOID) {
	wifi_fixture::state.deletedProfile = QString::fromWCharArray(name);
	return ERROR_SUCCESS;
}
}
