// The relaunch switch of the crash handler. A supervised shell asks for the
// minidump and the desktop cleanup of a crash but not for a replacement shell,
// so QS_DISABLE_CRASH_RELAUNCH must disable exactly the relaunch, must not need
// a value, and must not be confused with QS_DISABLE_CRASH_HANDLER, which keeps
// turning the whole handler off. No crash is triggered here.

#include <qbytearray.h>
#include <qtest.h>
#include <qtenvironmentvariables.h>

#include "../crash/relaunch_policy.hpp"

class TestCrashRelaunchPolicy: public QObject {
	Q_OBJECT;

private slots:
	void init() {
		qunsetenv("QS_DISABLE_CRASH_RELAUNCH");
		qunsetenv("QS_DISABLE_CRASH_HANDLER");
	}

	void relaunchStaysEnabledWithoutTheVariable();
	void anySetValueDisablesTheRelaunch_data();
	void anySetValueDisablesTheRelaunch();
	void theWholeHandlerSwitchIsIndependent();
};

void TestCrashRelaunchPolicy::relaunchStaysEnabledWithoutTheVariable() {
	QVERIFY(!qs::crash::crashRelaunchDisabled());
}

void TestCrashRelaunchPolicy::anySetValueDisablesTheRelaunch_data() {
	QTest::addColumn<QByteArray>("value");
	// The supervisor sets "1". Presence is the contract, so a value such as "0"
	// must not read as "relaunch anyway".
	QTest::newRow("one") << QByteArray("1");
	QTest::newRow("zero") << QByteArray("0");
	QTest::newRow("true") << QByteArray("true");
}

void TestCrashRelaunchPolicy::anySetValueDisablesTheRelaunch() {
	QFETCH(QByteArray, value);
	QVERIFY(qputenv("QS_DISABLE_CRASH_RELAUNCH", value));
	QVERIFY(qs::crash::crashRelaunchDisabled());
}

void TestCrashRelaunchPolicy::theWholeHandlerSwitchIsIndependent() {
	QVERIFY(qputenv("QS_DISABLE_CRASH_HANDLER", QByteArray("1")));
	QVERIFY(!qs::crash::crashRelaunchDisabled());
}

QTEST_GUILESS_MAIN(TestCrashRelaunchPolicy)

#include "crash_relaunch_policy_test.moc"
