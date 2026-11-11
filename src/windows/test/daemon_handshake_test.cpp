#include <qcoreapplication.h>
#include <qtest.h>
#include <windows.h>

#include "../../launch/daemon.hpp"

class DaemonHandshakeTest: public QObject {
	Q_OBJECT;

private slots:
	void childAcknowledgesWithoutGui_data() {
		QTest::addColumn<QByteArray>("flag");
		QTest::newRow("short") << QByteArray("-d");
		QTest::newRow("long") << QByteArray("--daemonize");
	}

	void childAcknowledgesWithoutGui() {
		QFETCH(QByteArray, flag);
		QByteArray executable = QCoreApplication::applicationFilePath().toLocal8Bit();
		QByteArray marker = "--daemon-fixture-child";
		char* args[] = {executable.data(), flag.data(), marker.data()};
		int result = -1;
		QVERIFY(qs::launch::spawnDaemon(3, args, &result));
		QCOMPARE(result, 37);
		QCOMPARE(qs::launch::DAEMON_PIPE, nullptr);
		QVERIFY(!qEnvironmentVariableIsSet("__QUICKSHELL_DAEMON_PIPE"));
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	const auto args = app.arguments();
	if (args.contains("--daemon-fixture-child")) {
		// Model runCommand's flag-gated dispatch without starting a shell,
		// QGuiApplication, native providers or any desktop windows.
		if (!args.contains("-d") && !args.contains("--daemonize")) return 91;
		int result = -1;
		if (qs::launch::spawnDaemon(argc, argv, &result)) return 92;
		if (qs::launch::DAEMON_PIPE == nullptr) return 93;
		if (qEnvironmentVariableIsSet("__QUICKSHELL_DAEMON_PIPE")) return 94;
		DWORD flags = 0;
		if (!GetHandleInformation(qs::launch::DAEMON_PIPE, &flags)) return 95;
		if (flags & HANDLE_FLAG_INHERIT) return 96;
		qs::launch::exitDaemon(37);
		return qs::launch::DAEMON_PIPE == nullptr ? 0 : 97;
	}
	DaemonHandshakeTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "daemon_handshake_test.moc"
