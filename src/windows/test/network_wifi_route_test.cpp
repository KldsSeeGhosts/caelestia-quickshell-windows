#include <qsignalspy.h>
#include <qtest.h>

#include "../system/network.hpp"
#include "../system/network_wifi_route.hpp"

using namespace qs::windows::sys::wifi_route;

namespace {
GUID guidFor(quint32 id) {
	GUID guid {};
	guid.Data1 = id;
	guid.Data2 = 0x243;
	return guid;
}

constexpr auto IDLE_A = 0x1000;
constexpr auto IDLE_B = 0x2000;
constexpr auto CONNECTED = 0x3000;
} // namespace

// Pure routing fixtures. WlanEnumInterfaces, WlanRegisterNotification and the
// live WLAN service are not called; only the selection and notification rules
// the backend applies to their results are exercised.
class NetworkWifiRouteTest: public QObject {
	Q_OBJECT;

private slots:
	void selectsConnectedInterface();
	void prefersConnectingOverIdle();
	void keepsEnumerationOrderOnTie();
	void keepsSelectedWhileStillEnumerated();
	void foreignArrivalDoesNotMoveSelection();
	void removingAnotherAdapterDoesNotClearSelection();
	void adoptsReplacementWhenSelectedIsRemoved();
	void removingSelectedWhileConnectingEndsPendingConnection();
	void clearsSelectionWhenNoneRemain();
	void notificationForAnotherInterfaceIsIgnored();
	void notificationWithoutSelectionIsIgnored();
};

void NetworkWifiRouteTest::selectsConnectedInterface() {
	const InterfaceCandidate items[] = {
	    {guidFor(IDLE_A), wlan_interface_state_disconnected},
	    {guidFor(CONNECTED), wlan_interface_state_connected},
	    {guidFor(IDLE_B), wlan_interface_state_not_ready},
	};

	auto selection = nextSelection(GUID {}, false, items, 3);
	QVERIFY(selection.present);
	QVERIFY(selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, guidFor(CONNECTED)));
	QVERIFY(IsEqualGUID(primary(items, 3)->guid, guidFor(CONNECTED)));
}

void NetworkWifiRouteTest::prefersConnectingOverIdle() {
	const InterfaceCandidate items[] = {
	    {guidFor(IDLE_A), wlan_interface_state_not_ready},
	    {guidFor(IDLE_B), wlan_interface_state_authenticating},
	};

	auto selection = nextSelection(GUID {}, false, items, 2);
	QVERIFY(selection.present);
	QVERIFY(IsEqualGUID(selection.guid, guidFor(IDLE_B)));
}

void NetworkWifiRouteTest::keepsEnumerationOrderOnTie() {
	const InterfaceCandidate items[] = {
	    {guidFor(IDLE_A), wlan_interface_state_disconnected},
	    {guidFor(IDLE_B), wlan_interface_state_disconnected},
	};

	auto selection = nextSelection(GUID {}, false, items, 2);
	QVERIFY(selection.present);
	QVERIFY(IsEqualGUID(selection.guid, guidFor(IDLE_A)));
}

void NetworkWifiRouteTest::keepsSelectedWhileStillEnumerated() {
	const auto selected = guidFor(IDLE_A);
	const InterfaceCandidate items[] = {
	    {guidFor(CONNECTED), wlan_interface_state_connected},
	    {selected, wlan_interface_state_disconnected},
	};

	auto selection = nextSelection(selected, true, items, 2);
	QVERIFY(selection.present);
	QVERIFY(!selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, selected));
}

void NetworkWifiRouteTest::foreignArrivalDoesNotMoveSelection() {
	const auto selected = guidFor(IDLE_A);
	const InterfaceCandidate items[] = {
	    {selected, wlan_interface_state_disconnected},
	    {guidFor(CONNECTED), wlan_interface_state_connected},
	};

	auto selection = nextSelection(selected, true, items, 2);
	QVERIFY(selection.present);
	QVERIFY(!selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, selected));
}

void NetworkWifiRouteTest::removingAnotherAdapterDoesNotClearSelection() {
	// The regression from #243: a second adapter leaving still leaves the
	// selected interface enumerated, so its state must survive.
	const auto selected = guidFor(IDLE_A);
	const InterfaceCandidate items[] = {{selected, wlan_interface_state_connected}};

	auto selection = nextSelection(selected, true, items, 1);
	QVERIFY(selection.present);
	QVERIFY(!selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, selected));
}

void NetworkWifiRouteTest::adoptsReplacementWhenSelectedIsRemoved() {
	const auto removed = guidFor(IDLE_A);
	const InterfaceCandidate items[] = {
	    {guidFor(IDLE_B), wlan_interface_state_disconnected},
	};

	auto selection = nextSelection(removed, true, items, 1);
	QVERIFY(selection.present);
	QVERIFY(selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, guidFor(IDLE_B)));
}

void NetworkWifiRouteTest::removingSelectedWhileConnectingEndsPendingConnection() {
	// Unplugging the selected adapter during a connection attempt: its
	// completion can no longer arrive, so the request must end as failed while
	// the replacement is adopted instead of leaving the shell busy.
	const auto removed = guidFor(IDLE_A);
	const InterfaceCandidate remaining[] = {
	    {guidFor(CONNECTED), wlan_interface_state_connected},
	};

	auto selection = nextSelection(removed, true, remaining, 1);
	QVERIFY(selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, guidFor(CONNECTED)));

	const auto pending = endPendingOnRemoval(true, QStringLiteral("fixture-network"));
	QVERIFY(pending.failed);
	QCOMPARE(pending.ssid, QStringLiteral("fixture-network"));

	// With no request in flight, removal ends nothing.
	const auto idle = endPendingOnRemoval(false, QStringLiteral("fixture-network"));
	QVERIFY(!idle.failed);
	QVERIFY(idle.ssid.isEmpty());
}

void NetworkWifiRouteTest::clearsSelectionWhenNoneRemain() {
	auto selection = nextSelection(guidFor(IDLE_A), true, nullptr, 0);
	QVERIFY(!selection.present);
	QVERIFY(selection.changed);
	QVERIFY(IsEqualGUID(selection.guid, GUID {}));
}

void NetworkWifiRouteTest::notificationForAnotherInterfaceIsIgnored() {
	const auto selected = guidFor(IDLE_A);
	QVERIFY(matchesSelected(selected, selected));
	QVERIFY(!matchesSelected(selected, guidFor(IDLE_B)));
	// Notifications without an interface GUID never describe the selection.
	QVERIFY(!matchesSelected(selected, GUID {}));
	QVERIFY(!matchesSelected(GUID {}, guidFor(IDLE_A)));
}

void NetworkWifiRouteTest::notificationWithoutSelectionIsIgnored() {
	QVERIFY(!matchesSelected(GUID {}, GUID {}));
	QVERIFY(!matchesSelected(GUID {}, guidFor(IDLE_A)));
}

// The completed-refresh snapshot over the real Network state and its networks
// model. The WLAN and connectivity backends are link stubs and are never
// started, so no adapter, scan or notification is involved.
class NetworkSnapshotTest: public QObject {
	Q_OBJECT;

private slots:
	void completedRefreshPublishesOneSnapshot();
};

void NetworkSnapshotTest::completedRefreshPublishesOneSnapshot() {
	auto* network = qs::windows::sys::Network::createForTesting();
	QSignalSpy snapshots(network, &qs::windows::sys::Network::wifiSnapshotChanged);
	// What the model held when each generation reached a consumer.
	QList<int> rowsAtPublish;
	QObject::connect(network, &qs::windows::sys::Network::wifiSnapshotChanged, network, [&]() {
		rowsAtPublish.append(
		    static_cast<int>(network->networks()->rowCount(QModelIndex {}))
		);
	});

	QCOMPARE(network->bindableWifiSnapshot().value(), 0);

	std::vector<qs::windows::sys::RawWifiNetwork> rows(2);
	rows[0].ssid = QStringLiteral("First");
	rows[0].strength = 70;
	rows[1].ssid = QStringLiteral("Second");
	rows[1].strength = 40;

	network->backendPublishAvailableNetworks(rows);
	QCOMPARE(network->bindableWifiSnapshot().value(), 1);
	QCOMPARE(network->networks()->rowCount(QModelIndex {}), 2);

	// A refresh that found the same access points is still a completed snapshot:
	// it is what lets a consumer confirm that an absent row stays absent.
	network->backendPublishAvailableNetworks(rows);
	QCOMPARE(network->bindableWifiSnapshot().value(), 2);
	QCOMPARE(network->networks()->rowCount(QModelIndex {}), 2);

	// A row the refresh dropped is gone before the generation advances.
	rows.pop_back();
	network->backendPublishAvailableNetworks(rows);
	QCOMPARE(network->bindableWifiSnapshot().value(), 3);
	QCOMPARE(network->networks()->rowCount(QModelIndex {}), 1);

	QCOMPARE(snapshots.count(), 3);
	QCOMPARE(rowsAtPublish, (QList<int> {2, 2, 1}));
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	NetworkWifiRouteTest route;
	NetworkSnapshotTest snapshot;
	const auto first = QTest::qExec(&route, argc, argv);
	const auto second = QTest::qExec(&snapshot, argc, argv);
	return first | second;
}

#include "network_wifi_route_test.moc"
