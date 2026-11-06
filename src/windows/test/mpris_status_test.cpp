#include <qsignalspy.h>
#include <qtest.h>

#include "backend_status.hpp"

using qs::windows::services::mpris::MprisBackendStatus;

class MprisStatusTest: public QObject {
	Q_OBJECT;

private slots:
	void uninitializedIsNotAnEmptySuccessfulScan() {
		MprisBackendStatus status;
		QVERIFY(!status.ready());
		QVERIFY(status.lastError().isEmpty());
		status.setResult({});
		QVERIFY(status.ready());
		QVERIFY(status.lastError().isEmpty());
	}

	void denialRemainsUnavailableUntilSuccessfulScan() {
		MprisBackendStatus status;
		QSignalSpy changed(&status, &MprisBackendStatus::changed);
		const auto denied = QStringLiteral("GSMTC RequestAsync failed (HRESULT 0x80070005)");
		status.setResult(denied);
		QVERIFY(!status.ready());
		QCOMPARE(status.lastError(), denied);
		QCOMPARE(changed.count(), 1);
		status.setResult(denied);
		QCOMPARE(changed.count(), 1);
		status.setResult({});
		QVERIFY(status.ready());
		QVERIFY(status.lastError().isEmpty());
		QCOMPARE(changed.count(), 2);
	}

	void failedRefreshInvalidatesReadinessAndRecovers() {
		MprisBackendStatus status;
		status.setResult({});
		const auto failed = QStringLiteral("GSMTC GetSessions failed (HRESULT 0x80004005)");
		status.setResult(failed);
		QVERIFY(!status.ready());
		QCOMPARE(status.lastError(), failed);
		status.setResult({});
		QVERIFY(status.ready());
		QVERIFY(status.lastError().isEmpty());
	}

	void observersAlwaysSeeConsistentStatus() {
		MprisBackendStatus status;
		int notifications = 0;
		QObject::connect(&status, &MprisBackendStatus::changed, &status, [&] {
			++notifications;
			QCOMPARE(status.ready(), status.lastError().isEmpty());
		});
		status.setResult({});
		status.setResult(QStringLiteral("Fixture failure"));
		status.setResult({});
		QCOMPARE(notifications, 3);
	}
};

QTEST_GUILESS_MAIN(MprisStatusTest)

#include "mpris_status_test.moc"
