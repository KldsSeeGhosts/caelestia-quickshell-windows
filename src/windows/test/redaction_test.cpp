#include <cstdio>

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qprocess.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtest.h>
#include <qtextstream.h>

#include "redact.hpp"

namespace {

QString debugText(const QString& value) {
	auto text = QString();
	auto debug = QDebug(&text);
	debug.noquote() << value;
	return text;
}

} // namespace

class RedactionTest: public QObject {
	Q_OBJECT

private slots:
	void idIsStableWithinTheProcess() {
		auto first = qs::windows::logId(QStringLiteral("aa:bb:cc:dd:ee:ff"));
		auto second = qs::windows::logId(QStringLiteral("aa:bb:cc:dd:ee:ff"));

		QCOMPARE(first, second);
		QVERIFY(first.startsWith(QStringLiteral("redacted:")));
		QVERIFY(QRegularExpression(QStringLiteral("^redacted:[0-9a-f]{12}$")).match(first).hasMatch());
	}

	void idChangesBetweenProcesses() {
		auto hashes = QStringList();
		for (int i = 0; i < 2; ++i) {
			auto child = QProcess();
			child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hash-id")});
			QVERIFY2(child.waitForFinished(10000), qPrintable(child.errorString()));
			QCOMPARE(child.exitStatus(), QProcess::NormalExit);
			QCOMPARE(child.exitCode(), 0);

			auto output = QString::fromUtf8(child.readAllStandardOutput()).simplified().split(' ');
			QCOMPARE(output.size(), 2);
			QVERIFY(QRegularExpression(QStringLiteral("^redacted:[0-9a-f]{12}$"))
			            .match(output.first())
			            .hasMatch());
			QCOMPARE(output.first(), output.last());
			hashes.append(output.first());
		}

		QVERIFY(hashes.first() != hashes.last());
	}

	void idHidesTheIdentifier() {
		auto address = debugText(qs::windows::logId(QStringLiteral("aa:bb:cc:dd:ee:ff")));
		QVERIFY(!address.contains(QStringLiteral("aa:bb")));
		QVERIFY(!address.contains(QStringLiteral("aa:bb:cc:dd:ee:ff")));

		auto name = debugText(qs::windows::logId(QStringLiteral("Avery's Headphones")));
		QVERIFY(!name.contains(QStringLiteral("Avery")));
		QVERIFY(!name.contains(QStringLiteral("Headphones")));
	}

	void differentIdentifiersStayDifferent() {
		auto first = qs::windows::logId(QStringLiteral("aa:bb:cc:dd:ee:ff"));
		auto second = qs::windows::logId(QStringLiteral("aa:bb:cc:dd:ee:fe"));

		QVERIFY(first != second);
	}

	void emptyValuesAreVisible() {
		QCOMPARE(qs::windows::logId(QString()), QStringLiteral("<empty>"));
		QCOMPARE(qs::windows::logPath(QString()), QStringLiteral("<empty>"));
	}

	void pathsKeepOnlyTheExtension() {
		auto path = qs::windows::logPath(QStringLiteral("C:/Users/Avery/Pictures/holiday.png"));

		QVERIFY(path.startsWith(QStringLiteral("redacted:")));
		QVERIFY(path.endsWith(QStringLiteral("(.png)")));
		QVERIFY(!path.contains(QStringLiteral("Avery")));
		QVERIFY(!path.contains(QStringLiteral("holiday")));
		QVERIFY(!path.contains(QStringLiteral("Users")));

		auto dir = qs::windows::logPath(QStringLiteral("C:/Users/Avery/Pictures"));
		QVERIFY(!dir.contains(QStringLiteral("Avery")));
		QVERIFY(!dir.contains(QStringLiteral("Pictures")));

		auto dottedDir = qs::windows::logPath(QStringLiteral("C:/Users/Avery/Archive.Private"));
		QVERIFY(!dottedDir.contains(QStringLiteral("Private")));
		QVERIFY(!dottedDir.endsWith(QStringLiteral("(.private)")));
	}

	void pathsAreStableWithinTheProcess() {
		auto first = qs::windows::logPath(QStringLiteral("C:/Users/Avery/file.mp4"));
		auto second = qs::windows::logPath(QStringLiteral("C:/Users/Avery/file.mp4"));

		QCOMPARE(first, second);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().contains(QStringLiteral("--hash-id"))) {
		const auto id = QStringLiteral("caelestia-redaction-test-identifier");
		auto output = QTextStream(stdout);
		output << qs::windows::logId(id) << '\n' << qs::windows::logId(id) << '\n';
		return 0;
	}

	RedactionTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "redaction_test.moc"
