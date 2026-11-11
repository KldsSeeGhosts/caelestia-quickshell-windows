#include <qdatastream.h>
#include <qlocalserver.h>
#include <qlocalsocket.h>
#include <qtest.h>
#include <quuid.h>

#include "../../ipc/limits.hpp"

using namespace qs::ipc;

class TestIpcLimits: public QObject {
	Q_OBJECT;

private slots:
	void admissionIsBounded() {
		IpcAdmission admission;
		for (int i = 0; i < IpcLimits::MaxConnections; ++i) QVERIFY(admission.acquire());
		QVERIFY(!admission.acquire());
		admission.release();
		QVERIFY(admission.acquire());
		QVERIFY(!admission.acquire());
	}

	void inputBounds_data() {
		QTest::addColumn<bool>("oversized");
		QTest::newRow("idle") << false;
		QTest::newRow("oversized") << true;
	}

	void inputBounds() {
		QFETCH(bool, oversized);
		QLocalServer server;
		QVERIFY(server.listen("qs-ipc-limits-" + QUuid::createUuid().toString(QUuid::Id128)));
		QLocalSocket client;
		client.connectToServer(server.fullServerName());
		QTRY_VERIFY(server.hasPendingConnections());
		auto* peer = server.nextPendingConnection();
		IpcInputGuard guard(peer, oversized ? 5000 : 50);
		if (oversized) {
			client.write(QByteArray(IpcLimits::MaxIncompleteBytes + 1, 'x'));
			client.flush();
		}
		QTRY_COMPARE_WITH_TIMEOUT(peer->state(), QLocalSocket::UnconnectedState, 2000);
	}

	void fragmentedInputAndCompletedListener() {
		QLocalServer server;
		QVERIFY(server.listen("qs-ipc-limits-" + QUuid::createUuid().toString(QUuid::Id128)));
		QLocalSocket client;
		client.connectToServer(server.fullServerName());
		QTRY_VERIFY(server.hasPendingConnections());
		auto* peer = server.nextPendingConnection();
		IpcInputGuard guard(peer, 200);
		QByteArray bytes;
		QDataStream output(&bytes, QIODevice::WriteOnly);
		output << QStringLiteral("normal request");
		QDataStream input(peer);
		client.write(bytes.first(3));
		client.flush();
		QTRY_COMPARE(peer->bytesAvailable(), 3);
		input.startTransaction();
		QString message;
		input >> message;
		QVERIFY(!input.commitTransaction());
		client.write(bytes.sliced(3));
		client.flush();
		QTRY_COMPARE(peer->bytesAvailable(), bytes.size());
		input.startTransaction();
		input >> message;
		QVERIFY(input.commitTransaction());
		QCOMPARE(message, QStringLiteral("normal request"));
		guard.complete();
		QTest::qWait(250);
		QCOMPARE(peer->state(), QLocalSocket::ConnectedState);
	}

	void trickleDoesNotExtendDeadline() {
		QLocalServer server;
		QVERIFY(server.listen("qs-ipc-limits-" + QUuid::createUuid().toString(QUuid::Id128)));
		QLocalSocket client;
		client.connectToServer(server.fullServerName());
		QTRY_VERIFY(server.hasPendingConnections());
		auto* peer = server.nextPendingConnection();
		IpcInputGuard guard(peer, 100);
		QTimer trickle;
		connect(&trickle, &QTimer::timeout, &client, [&] {
			client.write("x");
			client.flush();
		});
		trickle.start(10);
		QTRY_COMPARE_WITH_TIMEOUT(peer->state(), QLocalSocket::UnconnectedState, 500);
	}
};

QTEST_GUILESS_MAIN(TestIpcLimits)
#include "ipc_limits_test.moc"
