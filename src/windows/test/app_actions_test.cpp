#include <qcoreapplication.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../system/app_actions.hpp"

namespace qs::windows::sys::test {
void setParsingNameForId(const QString& id, const QString& token);
void clearParsingNames();
}

using qs::windows::sys::AppActions;

class AppActionsTest: public QObject {
	Q_OBJECT;

private slots:
	void cleanup() { qs::windows::sys::test::clearParsingNames(); }

	void unknownEntryRejectsProperties() {
		AppActions actions;
		QSignalSpy errors(&actions, &AppActions::lastErrorChanged);

		QVERIFY(!actions.openProperties(QStringLiteral("missing-fixture-entry")));
		QCOMPARE(
		    actions.lastError(),
		    QStringLiteral("The application entry is not available")
		);
		QCOMPARE(errors.size(), 1);
		QCOMPARE(
		    actions.info(QStringLiteral("missing-fixture-entry"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    false
		);
	}

	void win32EntryPublishesPropertiesCapability() {
		qs::windows::sys::test::setParsingNameForId(
		    QStringLiteral("fixture-win32"), QStringLiteral("C:\\Fixture\\Fixture App.exe")
		);

		AppActions actions;
		const auto info = actions.info(QStringLiteral("fixture-win32"));
		QCOMPARE(info.value(QStringLiteral("packaged")).toBool(), false);
		QCOMPARE(info.value(QStringLiteral("canOpenLocation")).toBool(), true);
		QCOMPARE(info.value(QStringLiteral("canOpenProperties")).toBool(), true);
	}

	void packagedEntryRejectsPropertiesWithoutShellUi() {
		qs::windows::sys::test::setParsingNameForId(
		    QStringLiteral("fixture-packaged"),
		    QStringLiteral("Fixture.App_123456789abcd!App")
		);

		AppActions actions;
		const auto info = actions.info(QStringLiteral("fixture-packaged"));
		QCOMPARE(info.value(QStringLiteral("packaged")).toBool(), true);
		QCOMPARE(info.value(QStringLiteral("canOpenProperties")).toBool(), false);

		QVERIFY(!actions.openProperties(QStringLiteral("fixture-packaged")));
		QCOMPARE(
		    actions.lastError(),
		    QStringLiteral("Properties is not supported for this application")
		);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	AppActionsTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "app_actions_test.moc"
