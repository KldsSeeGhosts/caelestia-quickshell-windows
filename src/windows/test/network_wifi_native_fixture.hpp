#pragma once

#include <vector>

#include <qstring.h>

#include "../system/network_wifi_identity.hpp"

namespace qs::windows::sys {
class NetworkWifiBackend;
}

// All observations are generated. These entry points are linked into the
// component executable instead of wlanapi.lib, so no real WLAN call can occur.
namespace wifi_fixture {
inline GUID adapter(quint32 id = 1) {
	GUID guid {};
	guid.Data1 = id;
	return guid;
}
struct State {
	std::vector<qs::windows::sys::RawWifiNetwork> rows;
	std::vector<qs::windows::sys::RawWifiNetwork> bssRows;
	GUID selected = adapter();
	QString xml;
	QString connectedProfile;
	QString deletedProfile;
	GUID connectedAdapter {};
	int profileWrites = 0;
	int connects = 0;
	int disconnects = 0;
	int scans = 0;
	bool overwrite = true;
	DWORD profileResult = ERROR_SUCCESS;
	DWORD connectResult = ERROR_SUCCESS;
};
extern State state;
void reset();
void notify(
    qs::windows::sys::NetworkWifiBackend* backend,
    const QByteArray& bytes,
    const QString& profile,
    DWORD code,
    GUID guid = adapter(),
    DWORD reason = 0
);
} // namespace wifi_fixture
