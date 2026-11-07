#pragma once

#include <atomic>
#include <optional>

#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <wlanapi.h>

#include "network_wifi_identity.hpp"

namespace qs::windows::sys {

class Network;

class NetworkWifiBackend: public QObject {
	Q_OBJECT;

public:
	explicit NetworkWifiBackend(Network* owner);
	~NetworkWifiBackend() override;
	Q_DISABLE_COPY_MOVE(NetworkWifiBackend);

	void start();

	void refreshRadioState();
	void setRadioEnabled(bool enabled);
	void scan();
	void refreshAvailableNetworks();
	void refreshCurrentConnection();
	void connectToNetwork(const RawWifiNetwork& network, const QString& password, quint64 requestId);
	void cancelConnection();
	void disconnectActive();
	void forgetNetwork(const RawWifiNetwork& network);

#ifdef QS_TEST
	void finishOpenForTesting(DWORD result, HANDLE handle) { this->finishOpen(result, handle); }
	void notifyForTesting(PWLAN_NOTIFICATION_DATA data) { notificationCallback(data, this); }
#endif

private:
	void open();
	void closeHandle();
	void finishOpen(DWORD openResult, HANDLE handle);
	void publishInitialized(const QString& error);
	bool selectInterface(DWORD& error, const GUID* exclude = nullptr);
	void adoptSelectedInterface(const GUID* exclude = nullptr);
	void clearSelectedInterface();
	[[nodiscard]] bool ownsInterface(const GUID& guid) const;
	static void WINAPI notificationCallback(PWLAN_NOTIFICATION_DATA data, PVOID context);
	void handleAcmNotification(DWORD code, const GUID& guid);
	void handleMsmNotification(DWORD code, const GUID& guid);

	Network* mOwner;
	HANDLE mHandle = nullptr;
	bool mHasAdapter = false;
	bool mScanRequested = false;
	GUID mInterfaceGuid {};
	struct PendingConnection {
		RawWifiNetwork network;
		QString profileName;
		quint64 requestId = 0;
	};
	std::optional<PendingConnection> mPendingConnection;
	// Read on the WLAN callback thread, checked again on the QObject thread.
	std::atomic<quint64> mCallbackRequestId {0};
};

} // namespace qs::windows::sys
