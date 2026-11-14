#include <qcoreapplication.h>
#include <qfileinfo.h>
#include <qtest.h>

#include "../process_image_name.hpp"

using qs::windows::appIdFromImageName;
using qs::windows::processImageNameFromSnapshot;

class ProcessImageNameTest: public QObject {
	Q_OBJECT;

private slots:
	void appIdIsLowerCaseWithoutExeSuffix() {
		QCOMPARE(appIdFromImageName(QStringLiteral("Consent.EXE")), QStringLiteral("consent"));
		QCOMPARE(appIdFromImageName(QStringLiteral("consent.exe")), QStringLiteral("consent"));
		QCOMPARE(appIdFromImageName(QStringLiteral("tool.com")), QStringLiteral("tool.com"));
		QCOMPARE(appIdFromImageName(QString()), QString());
	}

	// The snapshot needs no process handle, which is why it still names a
	// process that OpenProcess refuses.
	void snapshotNamesAProcessWithoutOpeningIt() {
		auto expected = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
		auto actual = processImageNameFromSnapshot(GetCurrentProcessId());
		QCOMPARE(actual.toLower(), expected.toLower());
	}

	void snapshotReturnsNothingForAnAbsentProcess() {
		QCOMPARE(processImageNameFromSnapshot(0), QString());
		// Process ids are multiples of four, so this one never exists.
		QCOMPARE(processImageNameFromSnapshot(0xFFFFFFFE), QString());
	}
};

QTEST_GUILESS_MAIN(ProcessImageNameTest)
#include "process_image_name_test.moc"
