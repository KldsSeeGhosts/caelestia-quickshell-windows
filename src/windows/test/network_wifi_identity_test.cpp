#include <memory>

#include <qsignalspy.h>
#include <qtest.h>
#include <qxmlstream.h>

#include "../system/network.hpp"
#include "network_wifi_native_fixture.hpp"

using namespace qs::windows::sys;

namespace {
RawWifiNetwork row(const QByteArray& bytes = QByteArrayLiteral("fixture")) {
	RawWifiNetwork result;
	result.ssidBytes = bytes;
	result.ssid = QString::fromUtf8(bytes);
	result.interfaceGuid = wifi_fixture::adapter();
	result.strength = 70;
	return result;
}

void drain() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }

QString status(Network* network) { return network->lastWifiCommand().value("status").toString(); }
QString reason(Network* network) { return network->lastWifiCommand().value("reason").toString(); }

NetworkWifiNetwork* item(Network* network, int index = 0) {
	return qobject_cast<NetworkWifiNetwork*>(network->networks()->values().at(index));
}
} // namespace

class NetworkWifiIdentityTest: public QObject {
	Q_OBJECT;
private slots:
	void init() { wifi_fixture::reset(); }
	void cleanup() { QCOMPARE(wifi_fixture::state.scans, 0); }
	void rawProfile_data();
	void rawProfile();
	void ssidLength_data();
	void ssidLength();
	void security_data();
	void security();
	void savedProfiles_data() { security_data(); }
	void savedProfiles();
	void duplicateDisplayAndLegacyRefusal();
	void profileIdentityAndDedup();
	void profilelessTwin_data();
	void profilelessTwin();
	void staleSelectionAndAdapterReplacement();
	void pendingNotificationsAreIdentityBound();
	void queuedCompletionCannotFinishNewRequest();
	void cancellationAndRemoval();
	void refusedCommandsAreNotAccepted();
	void invalidSavedProfileCannotFallBackToDisplay();
	void observationIsNotCompletion();
	void missingCompletionReleasesRequestWithoutDisconnect();
	void propertyConsumerSeesStructuredIdentity();
	void unsupportedBssAndInvalidKey();
	void bssMetadataUsesRawIdentity();
	void reentrantCancellationDoesNotSubmit();
	void acceptedEventPrecedesReentrantCancellation();
	void adapterRemovalReentryCannotSubmit();
	void unsolicitedNotificationRefreshesWithoutResult();
};

void NetworkWifiIdentityTest::rawProfile_data() {
	QTest::addColumn<QByteArray>("bytes");
	QTest::newRow("ascii-xml-reserved") << QByteArrayLiteral("fixture<&\"'");
	QTest::newRow("utf8") << QByteArray::fromHex("666978747572652de29883");
	QTest::newRow("invalid-utf8") << QByteArray::fromHex("fffe8061");
	QTest::newRow("embedded-nul") << QByteArray::fromHex("6669780062696e");
	QTest::newRow("all-nul") << QByteArray(4, '\0');
	QTest::newRow("maximum") << QByteArray(32, '\xff');
}

void NetworkWifiIdentityTest::rawProfile() {
	QFETCH(QByteArray, bytes);
	wifi_fixture::state.rows = {row(bytes)};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QCOMPARE(network->networks()->values().size(), 1);
	auto* source = item(network.get());
	QCOMPARE(source->rawSsidHex(), QString::fromLatin1(bytes.toHex()));
	QVERIFY(source->newProfileSupported());
	QSignalSpy legacy(network.get(), &Network::wifiConnectResult);
	network->connectToNetworkById(source->selectionId(), {});
	QCOMPARE(wifi_fixture::state.profileWrites, 1);
	QCOMPARE(wifi_fixture::state.connects, 1);
	QVERIFY(!wifi_fixture::state.overwrite);
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
	QCOMPARE(legacy.count(), 0);

	QXmlStreamReader xml(wifi_fixture::state.xml);
	QByteArray recovered;
	int names = 0;
	while (!xml.atEnd()) {
		xml.readNext();
		if (!xml.isStartElement()) continue;
		if (xml.name() == QLatin1String("hex"))
			recovered = QByteArray::fromHex(xml.readElementText().toLatin1());
		if (xml.name() == QLatin1String("name")) ++names;
	}
	QVERIFY(!xml.hasError());
	QCOMPARE(recovered, bytes);
	QCOMPARE(names, 1); // Only profile name, never an SSID display-string name.
	wifi_fixture::notify(
	    network->wifiBackendForTesting(),
	    bytes,
	    wifi_fixture::state.connectedProfile,
	    wlan_notification_acm_connection_complete
	);
	drain();
	QCOMPARE(status(network.get()), QStringLiteral("observed"));
	QCOMPARE(legacy.count(), 1);
	QVERIFY(legacy.at(0).at(1).toBool());
}

void NetworkWifiIdentityTest::ssidLength_data() {
	QTest::addColumn<int>("length");
	QTest::addColumn<bool>("valid");
	QTest::newRow("zero") << 0 << false;
	QTest::newRow("one") << 1 << true;
	QTest::newRow("thirty-two") << 32 << true;
	QTest::newRow("thirty-three") << 33 << false;
}

void NetworkWifiIdentityTest::ssidLength() {
	QFETCH(int, length);
	QFETCH(bool, valid);
	const auto target = row(QByteArray(length, 'x'));
	const auto profile = wifi_identity::newProfile(target, {});
	QCOMPARE(profile.reason.isEmpty(), valid);
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QCOMPARE(network->networks()->values().size(), valid ? 1 : 0);
	// A malformed native length is rejected, never clamped to another identity.
	DOT11_SSID native {};
	native.uSSIDLength = static_cast<ULONG>(length);
	QCOMPARE(wifi_identity::ssidBytes(native).size(), valid ? length : 0);
}

void NetworkWifiIdentityTest::security_data() {
	QTest::addColumn<int>("authentication");
	QTest::addColumn<int>("cipher");
	QTest::addColumn<bool>("enabled");
	QTest::addColumn<bool>("supported");
	QTest::newRow("open") << int(DOT11_AUTH_ALGO_80211_OPEN) << int(DOT11_CIPHER_ALGO_NONE) << false
	                      << true;
	QTest::newRow("wpa2-aes") << int(DOT11_AUTH_ALGO_RSNA_PSK) << int(DOT11_CIPHER_ALGO_CCMP) << true
	                          << true;
	QTest::newRow("wpa3-sae-aes") << int(DOT11_AUTH_ALGO_WPA3_SAE) << int(DOT11_CIPHER_ALGO_CCMP)
	                              << true << true;
	QTest::newRow("wep-shared") << int(DOT11_AUTH_ALGO_80211_SHARED_KEY) << int(DOT11_CIPHER_ALGO_WEP)
	                            << true << false;
	QTest::newRow("wep-open-auth") << int(DOT11_AUTH_ALGO_80211_OPEN) << int(DOT11_CIPHER_ALGO_WEP)
	                               << true << false;
	QTest::newRow("inconsistent-open-wep")
	    << int(DOT11_AUTH_ALGO_80211_OPEN) << int(DOT11_CIPHER_ALGO_WEP) << false << false;
	QTest::newRow("wpa-personal") << int(DOT11_AUTH_ALGO_WPA_PSK) << int(DOT11_CIPHER_ALGO_TKIP)
	                              << true << false;
	QTest::newRow("wpa-enterprise") << int(DOT11_AUTH_ALGO_WPA) << int(DOT11_CIPHER_ALGO_CCMP) << true
	                                << false;
	QTest::newRow("wpa2-enterprise")
	    << int(DOT11_AUTH_ALGO_RSNA) << int(DOT11_CIPHER_ALGO_CCMP) << true << false;
	QTest::newRow("wpa3-enterprise")
	    << int(DOT11_AUTH_ALGO_WPA3) << int(DOT11_CIPHER_ALGO_CCMP) << true << false;
	QTest::newRow("owe") << int(DOT11_AUTH_ALGO_OWE) << int(DOT11_CIPHER_ALGO_CCMP) << true << false;
	QTest::newRow("wpa2-tkip") << int(DOT11_AUTH_ALGO_RSNA_PSK) << int(DOT11_CIPHER_ALGO_TKIP) << true
	                           << false;
	QTest::newRow("wpa3-gcmp") << int(DOT11_AUTH_ALGO_WPA3_SAE) << int(DOT11_CIPHER_ALGO_GCMP) << true
	                           << false;
	QTest::newRow("unknown") << 999 << 999 << true << false;
}

void NetworkWifiIdentityTest::security() {
	QFETCH(int, authentication);
	QFETCH(int, cipher);
	QFETCH(bool, enabled);
	QFETCH(bool, supported);
	auto target = row();
	target.authentication = static_cast<DOT11_AUTH_ALGORITHM>(authentication);
	target.cipher = static_cast<DOT11_CIPHER_ALGORITHM>(cipher);
	target.securityEnabled = enabled;
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	auto* source = item(network.get());
	QCOMPARE(source->authentication(), quint32(authentication));
	QCOMPARE(source->cipher(), quint32(cipher));
	network->connectToNetworkById(source->selectionId(), QStringLiteral("synthetic<&key"));
	QCOMPARE(wifi_fixture::state.profileWrites, supported ? 1 : 0);
	QCOMPARE(wifi_fixture::state.connects, supported ? 1 : 0);
	QCOMPARE(source->newProfileSupported(), supported);
	QCOMPARE(
	    status(network.get()),
	    supported ? QStringLiteral("accepted") : QStringLiteral("denied")
	);
	if (!supported) {
		QCOMPARE(reason(network.get()), QStringLiteral("unsupported-security"));
		QVERIFY(network->lastWifiCommand().value("requestId").toString().isEmpty());
	} else if (enabled) {
		QVERIFY(
		    wifi_fixture::state.xml.contains(
		        authentication == DOT11_AUTH_ALGO_WPA3_SAE ? "<authentication>WPA3SAE</authentication>"
		                                                   : "<authentication>WPA2PSK</authentication>"
		    )
		);
		QVERIFY(wifi_fixture::state.xml.contains("<encryption>AES</encryption>"));
		QVERIFY(wifi_fixture::state.xml.contains("synthetic&lt;&amp;key"));
	}
}

void NetworkWifiIdentityTest::savedProfiles() {
	QFETCH(int, authentication);
	QFETCH(int, cipher);
	QFETCH(bool, enabled);
	auto target = row(QByteArray::fromHex("66697800ff"));
	target.authentication = static_cast<DOT11_AUTH_ALGORITHM>(authentication);
	target.cipher = static_cast<DOT11_CIPHER_ALGORITHM>(cipher);
	target.securityEnabled = enabled;
	target.hasProfile = true;
	target.profileName = QString::fromUtf8("Synthetic profile <&> \xe2\x98\x83 exact ");
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	network->connectToNetworkById(item(network.get())->selectionId(), {});
	QCOMPARE(wifi_fixture::state.profileWrites, 0);
	QCOMPARE(wifi_fixture::state.connects, 1);
	QCOMPARE(wifi_fixture::state.connectedProfile, target.profileName);
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
	network->cancelWifiConnection();
	network->forgetNetworkById(wifi_identity::selectionId(target));
	QCOMPARE(wifi_fixture::state.deletedProfile, target.profileName);
}

void NetworkWifiIdentityTest::duplicateDisplayAndLegacyRefusal() {
	const auto first = row(QByteArray::fromHex("ff"));
	const auto second = row(QByteArray::fromHex("fe"));
	QCOMPARE(first.ssid, second.ssid);
	wifi_fixture::state.rows = {first, second};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QCOMPARE(network->networks()->values().size(), 2);
	QVERIFY(item(network.get(), 0)->selectionId() != item(network.get(), 1)->selectionId());
	network->connectToNetwork(first.ssid, {});
	QCOMPARE(status(network.get()), QStringLiteral("denied"));
	QCOMPARE(wifi_fixture::state.connects, 0);
	QCOMPARE(wifi_fixture::state.profileWrites, 0);
	network->forgetNetwork(first.ssid);
	QVERIFY(wifi_fixture::state.deletedProfile.isEmpty());
	network->connectToNetworkById(wifi_identity::selectionId(second), {});
	QCOMPARE(wifi_fixture::state.connects, 1);
	QVERIFY(wifi_fixture::state.xml.contains("<hex>FE</hex>"));
}

void NetworkWifiIdentityTest::profileIdentityAndDedup() {
	auto first = row();
	first.hasProfile = true;
	first.profileName = QStringLiteral("Synthetic profile A");
	auto duplicate = first;
	duplicate.strength = 90;
	auto second = first;
	second.profileName = QStringLiteral("Synthetic profile B");
	auto securityVariant = row();
	securityVariant.securityEnabled = true;
	securityVariant.authentication = DOT11_AUTH_ALGO_RSNA;
	securityVariant.cipher = DOT11_CIPHER_ALGO_CCMP;
	wifi_fixture::state.rows = {first, duplicate, second, securityVariant};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QCOMPARE(network->networks()->values().size(), 3);
	NetworkWifiNetwork* stable = nullptr;
	for (auto* object: network->networks()->values()) {
		auto* source = qobject_cast<NetworkWifiNetwork*>(object);
		if (source->selectionId() == wifi_identity::selectionId(first)) stable = source;
	}
	QVERIFY(stable);
	QCOMPARE(stable->bindableStrength().value(), 90);
	network->wifiBackendForTesting()->refreshAvailableNetworks();
	QVERIFY(network->networks()->values().contains(stable));
	network->connectToNetworkById(wifi_identity::selectionId(second), {});
	QCOMPARE(wifi_fixture::state.connectedProfile, second.profileName);
}

void NetworkWifiIdentityTest::profilelessTwin_data() {
	QTest::addColumn<int>("difference");
	QTest::newRow("same-network") << 0;
	QTest::newRow("different-raw-ssid") << 1;
	QTest::newRow("different-authentication") << 2;
	QTest::newRow("different-cipher") << 3;
	QTest::newRow("different-bss") << 4;
	QTest::newRow("different-security-flag") << 5;
}

void NetworkWifiIdentityTest::profilelessTwin() {
	QFETCH(int, difference);
	auto saved = row();
	saved.hasProfile = true;
	saved.profileName = QStringLiteral("Exact saved profile");
	auto unsaved = row();
	switch (difference) {
	case 1: unsaved.ssidBytes = QByteArrayLiteral("other"); break;
	case 2: unsaved.authentication = DOT11_AUTH_ALGO_RSNA_PSK; break;
	case 3: unsaved.cipher = DOT11_CIPHER_ALGO_CCMP; break;
	case 4: unsaved.bssType = dot11_BSS_type_independent; break;
	case 5: unsaved.securityEnabled = true; break;
	}
	// Reverse enumeration must not affect whether the redundant twin survives.
	for (const auto savedFirst: {false, true}) {
		wifi_fixture::state.rows =
		    savedFirst ? std::vector {saved, unsaved} : std::vector {unsaved, saved};
		std::unique_ptr<Network> network(Network::createForTesting());
		network->wifiBackendForTesting()->finishOpenForTesting(
		    ERROR_SUCCESS,
		    reinterpret_cast<HANDLE>(1)
		);
		QCOMPARE(network->networks()->values().size(), difference == 0 ? 1 : 2);
		if (difference == 0) {
			QCOMPARE(item(network.get())->selectionId(), wifi_identity::selectionId(saved));
			network->connectToNetwork(saved.ssid, {});
			QCOMPARE(status(network.get()), QStringLiteral("accepted"));
			QCOMPARE(wifi_fixture::state.connectedProfile, saved.profileName);
			QCOMPARE(wifi_fixture::state.profileWrites, 0);
			network->cancelWifiConnection();
		}
	}
}

void NetworkWifiIdentityTest::missingCompletionReleasesRequestWithoutDisconnect() {
	const auto target = row();
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	network->connectToNetworkById(item(network.get())->selectionId(), {});
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
	const auto firstId = network->lastWifiCommand().value("requestId");
	auto* timer = network->wifiCompletionTimerForTesting();
	QVERIFY(timer->isActive());
	QCOMPARE(timer->interval(), 60000);
	QVERIFY(timer->isSingleShot());
	// Exercise the real timer connection without a minute-long wall-clock wait.
	QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
	QVERIFY(!timer->isActive());
	QVERIFY(!network->bindableWifiConnecting().value());
	QCOMPARE(status(network.get()), QStringLiteral("failed"));
	QCOMPARE(reason(network.get()), QStringLiteral("completion-unobserved"));
	QCOMPARE(network->lastWifiCommand().value("requestId"), firstId);
	network->connectToNetworkById(item(network.get())->selectionId(), {});
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
	QVERIFY(network->lastWifiCommand().value("requestId") != firstId);
	QVERIFY(timer->isActive());
	network->cancelWifiConnection();
	QVERIFY(!timer->isActive());
	// The fake aborts on an unexpected disconnect, including timeout/cancel.
}

void NetworkWifiIdentityTest::staleSelectionAndAdapterReplacement() {
	const auto old = row();
	wifi_fixture::state.rows = {old};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	wifi_fixture::state.rows = {row(QByteArrayLiteral("replacement"))};
	backend->refreshAvailableNetworks();
	network->connectToNetworkById(wifi_identity::selectionId(old), {});
	QCOMPARE(wifi_fixture::state.connects, 0);
	network->connectToNetwork(QStringLiteral("unknown"), {});
	QCOMPARE(wifi_fixture::state.profileWrites, 0);
	wifi_fixture::state.selected = wifi_fixture::adapter(2);
	wifi_fixture::notify(backend, {}, {}, wlan_notification_acm_interface_removal);
	drain();
	network->connectToNetworkById(wifi_identity::selectionId(old), {});
	QCOMPARE(wifi_fixture::state.connects, 0);
	auto replacement = row(QByteArrayLiteral("replacement"));
	replacement.interfaceGuid = wifi_fixture::adapter(2);
	network->connectToNetworkById(wifi_identity::selectionId(replacement), {});
	QCOMPARE(wifi_fixture::state.connects, 1);
	QVERIFY(IsEqualGUID(wifi_fixture::state.connectedAdapter, replacement.interfaceGuid));
}

void NetworkWifiIdentityTest::pendingNotificationsAreIdentityBound() {
	const auto target = row(QByteArray::fromHex("ff"));
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	QSignalSpy terminal(network.get(), &Network::wifiConnectResult);
	network->connectToNetworkById(wifi_identity::selectionId(target), {});
	const auto profile = wifi_fixture::state.connectedProfile;
	wifi_fixture::notify(
	    backend,
	    QByteArray::fromHex("fe"),
	    profile,
	    wlan_notification_acm_connection_complete
	);
	wifi_fixture::notify(
	    backend,
	    target.ssidBytes,
	    QStringLiteral("wrong profile"),
	    wlan_notification_acm_connection_complete
	);
	wifi_fixture::notify(
	    backend,
	    target.ssidBytes,
	    profile,
	    wlan_notification_acm_connection_complete,
	    wifi_fixture::adapter(2)
	);
	drain();
	QCOMPARE(terminal.count(), 0);
	QVERIFY(network->bindableWifiConnecting().value());
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
	wifi_fixture::notify(
	    backend,
	    target.ssidBytes,
	    profile,
	    wlan_notification_acm_connection_attempt_fail,
	    wifi_fixture::adapter(),
	    1
	);
	drain();
	QCOMPARE(terminal.count(), 1);
	QCOMPARE(status(network.get()), QStringLiteral("failed"));
	QVERIFY(!terminal.at(0).at(1).toBool());
	wifi_fixture::notify(
	    backend,
	    target.ssidBytes,
	    profile,
	    wlan_notification_acm_connection_complete
	);
	drain();
	QCOMPARE(terminal.count(), 1);
}

void NetworkWifiIdentityTest::queuedCompletionCannotFinishNewRequest() {
	const auto target = row();
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	network->connectToNetworkById(wifi_identity::selectionId(target), {});
	wifi_fixture::notify(
	    backend,
	    target.ssidBytes,
	    wifi_fixture::state.connectedProfile,
	    wlan_notification_acm_connection_complete
	);
	network->cancelWifiConnection();
	network->connectToNetworkById(wifi_identity::selectionId(target), {});
	const auto secondId = network->lastWifiCommand().value("requestId").toString();
	drain();
	QVERIFY(network->bindableWifiConnecting().value());
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
	QCOMPARE(network->lastWifiCommand().value("requestId").toString(), secondId);
	network->backendWifiConnectResult(secondId.toULongLong() - 1, true, {});
	QVERIFY(network->bindableWifiConnecting().value());
}

void NetworkWifiIdentityTest::cancellationAndRemoval() {
	const auto target = row();
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	network->connectToNetworkById(wifi_identity::selectionId(target), {});
	network->connectToNetworkById(wifi_identity::selectionId(target), {});
	QCOMPARE(reason(network.get()), QStringLiteral("busy"));
	QCOMPARE(wifi_fixture::state.connects, 1);
	network->cancelWifiConnection();
	QCOMPARE(status(network.get()), QStringLiteral("cancelled"));
	QCOMPARE(wifi_fixture::state.disconnects, 0);
	QVERIFY(!network->bindableWifiConnecting().value());
	wifi_fixture::notify(
	    backend,
	    target.ssidBytes,
	    wifi_fixture::state.connectedProfile,
	    wlan_notification_acm_connection_complete
	);
	drain();
	QCOMPARE(status(network.get()), QStringLiteral("cancelled"));
	network->disconnectActive();
	QCOMPARE(wifi_fixture::state.disconnects, 1);
	network->connectToNetworkById(wifi_identity::selectionId(target), {});
	wifi_fixture::state.selected = wifi_fixture::adapter(2);
	wifi_fixture::notify(backend, {}, {}, wlan_notification_acm_interface_removal);
	drain();
	QCOMPARE(status(network.get()), QStringLiteral("failed"));
	QCOMPARE(reason(network.get()), QStringLiteral("adapter-removed"));
	QVERIFY(!network->bindableWifiConnecting().value());
}

void NetworkWifiIdentityTest::refusedCommandsAreNotAccepted() {
	wifi_fixture::state.rows = {row()};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	wifi_fixture::state.profileResult = ERROR_ACCESS_DENIED;
	network->connectToNetwork(QStringLiteral("fixture"), {});
	QCOMPARE(status(network.get()), QStringLiteral("denied"));
	QCOMPARE(reason(network.get()), QStringLiteral("profile-rejected"));
	QCOMPARE(wifi_fixture::state.connects, 0);
	wifi_fixture::state.profileResult = ERROR_SUCCESS;
	wifi_fixture::state.connectResult = ERROR_ACCESS_DENIED;
	network->connectToNetwork(QStringLiteral("fixture"), {});
	QCOMPARE(status(network.get()), QStringLiteral("denied"));
	QCOMPARE(reason(network.get()), QStringLiteral("command-rejected"));
	QVERIFY(network->lastWifiCommand().value("requestId").toString().isEmpty());
}

void NetworkWifiIdentityTest::invalidSavedProfileCannotFallBackToDisplay() {
	auto target = row();
	target.hasProfile = true;
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	network->connectToNetwork(QStringLiteral("fixture"), {});
	QCOMPARE(reason(network.get()), QStringLiteral("invalid-profile"));
	QCOMPARE(wifi_fixture::state.connects, 0);
	QCOMPARE(wifi_fixture::state.profileWrites, 0);
	network->forgetNetwork(QStringLiteral("fixture"));
	QVERIFY(wifi_fixture::state.deletedProfile.isEmpty());
}

void NetworkWifiIdentityTest::observationIsNotCompletion() {
	const auto target = row();
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	network->connectToNetwork(QStringLiteral("fixture"), {});
	network->backendSetCurrentConnection(target, true, false);
	QVERIFY(network->bindableWifiConnecting().value());
	QCOMPARE(status(network.get()), QStringLiteral("accepted"));
}

void NetworkWifiIdentityTest::propertyConsumerSeesStructuredIdentity() {
	auto target = row(QByteArray::fromHex("6600ff"));
	target.authentication = DOT11_AUTH_ALGO_80211_OPEN;
	target.cipher = DOT11_CIPHER_ALGO_WEP;
	target.securityEnabled = true;
	std::unique_ptr<Network> network(Network::createForTesting());
	int observations = 0;
	QObject::connect(network.get(), &Network::wifiSnapshotChanged, network.get(), [&]() {
		auto* object = network->networks()->values().at(0);
		QCOMPARE(object->property("selectionId").toString(), wifi_identity::selectionId(target));
		QCOMPARE(object->property("rawSsidHex").toString(), QStringLiteral("6600ff"));
		QCOMPARE(object->property("authentication").toUInt(), quint32(target.authentication));
		QCOMPARE(object->property("cipher").toUInt(), quint32(target.cipher));
		QVERIFY(!object->property("newProfileSupported").toBool());
		QCOMPARE(
		    object->property("newProfileReason").toString(),
		    QStringLiteral("unsupported-security")
		);
		// Open authentication with WEP must still look secured to the existing
		// shell's Settings handoff. An empty label would bypass that policy.
		QCOMPARE(object->property("security").toString(), QStringLiteral("WEP"));
		++observations;
	});
	network->backendPublishAvailableNetworks({target});
	QCOMPARE(observations, 1);
}

void NetworkWifiIdentityTest::unsupportedBssAndInvalidKey() {
	auto target = row();
	target.bssType = dot11_BSS_type_independent;
	QCOMPARE(wifi_identity::newProfile(target, {}).reason, QStringLiteral("unsupported-security"));
	target.bssType = dot11_BSS_type_infrastructure;
	target.securityEnabled = true;
	target.authentication = DOT11_AUTH_ALGO_RSNA_PSK;
	target.cipher = DOT11_CIPHER_ALGO_CCMP;
	QCOMPARE(wifi_identity::newProfile(target, {}).reason, QStringLiteral("invalid-key"));
	QCOMPARE(
	    wifi_identity::newProfile(target, QStringLiteral("bad\1key!")).reason,
	    QStringLiteral("invalid-key")
	);
}

void NetworkWifiIdentityTest::bssMetadataUsesRawIdentity() {
	auto first = row(QByteArray::fromHex("ff"));
	auto second = row(QByteArray::fromHex("fe"));
	first.frequency = 2412;
	first.strength = -70;
	first.bssid = QStringLiteral("02:00:00:00:00:01");
	second.frequency = 5180;
	second.strength = -50;
	second.bssid = QStringLiteral("02:00:00:00:00:02");
	auto weaker = first;
	weaker.strength = -90;
	weaker.frequency = 2427;
	wifi_fixture::state.rows = {first, second};
	wifi_fixture::state.bssRows = {first, second, weaker};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QCOMPARE(network->networks()->values().size(), 2);
	for (auto* object: network->networks()->values()) {
		auto* source = qobject_cast<NetworkWifiNetwork*>(object);
		const auto& expected = source->network().ssidBytes == first.ssidBytes ? first : second;
		QCOMPARE(source->bindableFrequency().value(), expected.frequency);
		QCOMPARE(source->bindableBssid().value(), expected.bssid);
	}
}

void NetworkWifiIdentityTest::reentrantCancellationDoesNotSubmit() {
	wifi_fixture::state.rows = {row()};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QObject::connect(network.get(), &Network::wifiConnectingSsidChanged, network.get(), [&]() {
		if (!network->bindableWifiConnectingSsid().value().isEmpty()) network->cancelWifiConnection();
	});
	network->connectToNetwork(QStringLiteral("fixture"), {});
	QVERIFY(!network->bindableWifiConnecting().value());
	QVERIFY(network->bindableWifiConnectingSsid().value().isEmpty());
	QCOMPARE(wifi_fixture::state.profileWrites, 0);
	QCOMPARE(wifi_fixture::state.connects, 0);
	QCOMPARE(wifi_fixture::state.disconnects, 0);
	QCOMPARE(status(network.get()), QStringLiteral("cancelled"));
}

void NetworkWifiIdentityTest::acceptedEventPrecedesReentrantCancellation() {
	wifi_fixture::state.rows = {row()};
	std::unique_ptr<Network> network(Network::createForTesting());
	network->wifiBackendForTesting()->finishOpenForTesting(
	    ERROR_SUCCESS,
	    reinterpret_cast<HANDLE>(1)
	);
	QSignalSpy commands(network.get(), &Network::wifiCommandResult);
	QObject::connect(network.get(), &Network::wifiCommandChanged, network.get(), [&]() {
		if (status(network.get()) == QStringLiteral("accepted")) network->cancelWifiConnection();
	});
	network->connectToNetwork(QStringLiteral("fixture"), {});
	QCOMPARE(commands.size(), 2);
	QCOMPARE(commands[0][0].toMap().value("status").toString(), QStringLiteral("accepted"));
	QCOMPARE(commands[1][0].toMap().value("status").toString(), QStringLiteral("cancelled"));
	QCOMPARE(status(network.get()), QStringLiteral("cancelled"));
	QVERIFY(!network->bindableWifiConnecting().value());
}

void NetworkWifiIdentityTest::adapterRemovalReentryCannotSubmit() {
	const auto target = row();
	wifi_fixture::state.rows = {target};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	network->connectToNetwork(QStringLiteral("fixture"), {});
	QObject::connect(
	    network.get(),
	    &Network::wifiCommandResult,
	    network.get(),
	    [&](const QVariantMap& result) {
		    if (result.value("reason").toString() == QStringLiteral("adapter-removed"))
			    network->connectToNetworkById(wifi_identity::selectionId(target), {});
	    }
	);
	wifi_fixture::state.selected = wifi_fixture::adapter(2);
	wifi_fixture::notify(backend, {}, {}, wlan_notification_acm_interface_removal);
	drain();
	QCOMPARE(wifi_fixture::state.profileWrites, 1);
	QCOMPARE(wifi_fixture::state.connects, 1);
	QVERIFY(!network->bindableWifiConnecting().value());
}

void NetworkWifiIdentityTest::unsolicitedNotificationRefreshesWithoutResult() {
	wifi_fixture::state.rows = {row()};
	std::unique_ptr<Network> network(Network::createForTesting());
	auto* backend = network->wifiBackendForTesting();
	backend->finishOpenForTesting(ERROR_SUCCESS, reinterpret_cast<HANDLE>(1));
	QSignalSpy snapshots(network.get(), &Network::wifiSnapshotChanged);
	QSignalSpy terminal(network.get(), &Network::wifiConnectResult);
	wifi_fixture::notify(
	    backend,
	    QByteArrayLiteral("external-fixture"),
	    QStringLiteral("external-profile"),
	    wlan_notification_acm_connection_complete
	);
	drain();
	QCOMPARE(snapshots.count(), 1);
	QCOMPARE(terminal.count(), 0);
	QVERIFY(network->lastWifiCommand().isEmpty());
	QVERIFY(!network->bindableWifiConnecting().value());
}

QTEST_GUILESS_MAIN(NetworkWifiIdentityTest)
#include "network_wifi_identity_test.moc"
