#pragma once

#include <qbytearray.h>
#include <qstring.h>
#include <qt_windows.h>
#include <wlanapi.h>

namespace qs::windows::sys {

struct RawWifiNetwork {
	// Display only. Never regenerate ssidBytes from this string.
	QString ssid;
	QByteArray ssidBytes;
	GUID interfaceGuid {};
	DOT11_AUTH_ALGORITHM authentication = DOT11_AUTH_ALGO_80211_OPEN;
	DOT11_CIPHER_ALGORITHM cipher = DOT11_CIPHER_ALGO_NONE;
	DOT11_BSS_TYPE bssType = dot11_BSS_type_infrastructure;
	bool securityEnabled = false;
	QString bssid;
	int strength = 0;
	int frequency = 0;
	bool active = false;
	QString security;
	bool hasProfile = false;
	QString profileName;
};

namespace wifi_identity {

inline bool validSsid(const QByteArray& bytes) { return !bytes.isEmpty() && bytes.size() <= 32; }

inline QByteArray ssidBytes(const DOT11_SSID& ssid) {
	if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > sizeof(ssid.ucSSID)) return {};
	return {reinterpret_cast<const char*>(ssid.ucSSID), static_cast<qsizetype>(ssid.uSSIDLength)};
}

inline QString selectionId(const RawWifiNetwork& network) {
	if (!validSsid(network.ssidBytes)) return {};
	// UTF-16 code units preserve even profile names that cannot round-trip UTF-8.
	const auto profile = network.hasProfile
	                       ? QByteArray(
	                             reinterpret_cast<const char*>(network.profileName.utf16()),
	                             network.profileName.size() * 2
	                         )
	                             .toHex()
	                       : QByteArray();
	return QStringLiteral("wlan:%1:%2:%3:%4:%5:%6:%7:%8")
	    .arg(
	        QString::fromLatin1(
	            QByteArray(reinterpret_cast<const char*>(&network.interfaceGuid), sizeof(GUID))
	                .toHex()
	        ),
	        QString::fromLatin1(network.ssidBytes.toHex())
	    )
	    .arg(static_cast<quint32>(network.authentication))
	    .arg(static_cast<quint32>(network.cipher))
	    .arg(network.securityEnabled ? 1 : 0)
	    .arg(static_cast<quint32>(network.bssType))
	    .arg(network.hasProfile ? 1 : 0)
	    .arg(QString::fromLatin1(profile));
}

inline bool supportsNewProfile(const RawWifiNetwork& network) {
	if (network.bssType != dot11_BSS_type_infrastructure) return false;
	if (!network.securityEnabled) {
		return network.authentication == DOT11_AUTH_ALGO_80211_OPEN
		    && network.cipher == DOT11_CIPHER_ALGO_NONE;
	}
	return network.cipher == DOT11_CIPHER_ALGO_CCMP
	    && (network.authentication == DOT11_AUTH_ALGO_RSNA_PSK
	        || network.authentication == DOT11_AUTH_ALGO_WPA3_SAE);
}

inline QString securityLabel(const RawWifiNetwork& network) {
	if (!network.securityEnabled && network.authentication == DOT11_AUTH_ALGO_80211_OPEN
	    && network.cipher == DOT11_CIPHER_ALGO_NONE)
		return {};
	switch (network.authentication) {
	case DOT11_AUTH_ALGO_80211_OPEN:
	case DOT11_AUTH_ALGO_80211_SHARED_KEY: return QStringLiteral("WEP");
	case DOT11_AUTH_ALGO_WPA:
	case DOT11_AUTH_ALGO_WPA_PSK: return QStringLiteral("WPA");
	case DOT11_AUTH_ALGO_RSNA: return QStringLiteral("WPA2-Enterprise");
	case DOT11_AUTH_ALGO_RSNA_PSK: return QStringLiteral("WPA2");
	case DOT11_AUTH_ALGO_WPA3: return QStringLiteral("WPA3-Enterprise");
	case DOT11_AUTH_ALGO_WPA3_SAE: return QStringLiteral("WPA3");
	case DOT11_AUTH_ALGO_OWE: return QStringLiteral("OWE");
	default: return QStringLiteral("Secured");
	}
}

inline QString xmlEscape(QString text) {
	text.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
	text.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
	text.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
	text.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
	text.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
	return text;
}

struct Profile {
	QString name;
	QString xml;
	QString reason;
};

inline Profile newProfile(const RawWifiNetwork& network, const QString& password) {
	if (!validSsid(network.ssidBytes)) return {{}, {}, QStringLiteral("invalid-ssid")};
	if (!supportsNewProfile(network)) return {{}, {}, QStringLiteral("unsupported-security")};

	const auto hex = QString::fromLatin1(network.ssidBytes.toHex()).toUpper();
	// Independent of the display label, XML-safe and below Windows' name limit.
	const auto name =
	    QStringLiteral("Caelestia-%1-%2").arg(hex).arg(static_cast<quint32>(network.authentication));
	const auto authentication = !network.securityEnabled ? QStringLiteral("open")
	                          : network.authentication == DOT11_AUTH_ALGO_WPA3_SAE
	                              ? QStringLiteral("WPA3SAE")
	                              : QStringLiteral("WPA2PSK");
	QString sharedKey;
	if (network.securityEnabled) {
		// This is only the existing personal-profile XML representation, not an
		// enrollment/credential prompt. The shell still hands unsaved secure
		// networks to Settings. Reject XML control characters and invalid keys.
		const auto ascii = password.toLatin1();
		if (password.size() < 8 || password.size() > 63 || QString::fromLatin1(ascii) != password)
			return {{}, {}, QStringLiteral("invalid-key")};
		for (const auto ch: ascii) {
			if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126)
				return {{}, {}, QStringLiteral("invalid-key")};
		}
		sharedKey = QStringLiteral(
		                "<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
		                "<keyMaterial>%1</keyMaterial></sharedKey>"
		)
		                .arg(xmlEscape(password));
	}
	return {
	    name,
	    QStringLiteral(
	        "<?xml version=\"1.0\"?>"
	        "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
	        "<name>%1</name><SSIDConfig><SSID><hex>%2</hex></SSID></SSIDConfig>"
	        "<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>"
	        "<MSM><security><authEncryption><authentication>%3</authentication>"
	        "<encryption>%4</encryption><useOneX>false</useOneX></authEncryption>%5"
	        "</security></MSM></WLANProfile>"
	    )
	        .arg(
	            name,
	            hex,
	            authentication,
	            network.securityEnabled ? QStringLiteral("AES") : QStringLiteral("none"),
	            sharedKey
	        ),
	    {}
	};
}

} // namespace wifi_identity
} // namespace qs::windows::sys
