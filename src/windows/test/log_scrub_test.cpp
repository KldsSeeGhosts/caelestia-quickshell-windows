#include <string>

#include <qbytearray.h>
#include <qdir.h>
#include <qstring.h>
#include <qtenvironmentvariables.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qurl.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "logscrub.hpp"

class LogScrubTest: public QObject {
	Q_OBJECT

	QTemporaryDir parent;
	QString profile;
	QString relocatedTemp;

private slots:
	// The scrubber reads its roots once, on first use. Pin a profile whose folder
	// name differs from the account name before that happens. The folder really
	// exists so that Windows can give it an 8.3 alias.
	void initTestCase() {
		QVERIFY(this->parent.isValid());
		this->profile = this->parent.filePath(QStringLiteral("Case Sample"));
		QVERIFY(QDir().mkpath(this->profile));

		auto native = QDir::toNativeSeparators(this->profile);
		qputenv("USERPROFILE", native.toLocal8Bit());
		qputenv("LOCALAPPDATA", (native + QStringLiteral("\\AppData\\Local")).toLocal8Bit());
		qputenv("USERNAME", QByteArray("ci-account"));

		// A configured root may end in a separator. It must still match child paths.
		this->relocatedTemp = this->parent.filePath(QStringLiteral("Relocated Temp"));
		QVERIFY(QDir().mkpath(this->relocatedTemp));
		auto tempWithSeparator = QDir::toNativeSeparators(this->relocatedTemp) + QStringLiteral("\\");
		qputenv("TEMP", tempWithSeparator.toLocal8Bit());
		qputenv("TMP", tempWithSeparator.toLocal8Bit());
	}

	void rootWithTrailingSeparatorIsReplaced() {
		auto native = QDir::toNativeSeparators(this->relocatedTemp);
		QCOMPARE(qs::log::scrubLogText(native + QStringLiteral("\\crash.dmp")), QStringLiteral("%TEMP%\\crash.dmp"));
		QCOMPARE(
		    qs::log::scrubLogText(this->relocatedTemp + QStringLiteral("/crash.dmp")),
		    QStringLiteral("%TEMP%/crash.dmp")
		);
		QCOMPARE(qs::log::scrubLogText(native), QStringLiteral("%TEMP%"));
	}

	void percentEncodedFileUrlIsReplaced() {
		auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(this->profile, "/:"));
		QVERIFY(encoded.contains(QStringLiteral("%20")));

		auto scrubbed = qs::log::scrubLogText(QStringLiteral("file:///") + encoded + QStringLiteral("/note.txt"));
		QCOMPARE(scrubbed, QStringLiteral("file:///%USERPROFILE%/note.txt"));

		scrubbed = qs::log::scrubLogText(QStringLiteral("file:///") + encoded.toLower() + QStringLiteral("/note.txt"));
		QCOMPARE(scrubbed, QStringLiteral("file:///%USERPROFILE%/note.txt"));

		auto native = QString::fromLatin1(QUrl::toPercentEncoding(QDir::toNativeSeparators(this->profile)));
		scrubbed = qs::log::scrubLogText(QStringLiteral("open ") + native + QStringLiteral("%5Cnote.txt"));
		QVERIFY(!scrubbed.contains(QStringLiteral("Case"), Qt::CaseInsensitive));
		QVERIFY(scrubbed.contains(QStringLiteral("%USERPROFILE%")));
	}

	void doubledBackslashesAreReplaced() {
		auto native = QDir::toNativeSeparators(this->profile);
		auto doubled = QString(native).replace(QLatin1Char('\\'), QStringLiteral("\\\\"));

		auto scrubbed = qs::log::scrubLogText(doubled + QStringLiteral("\\\\Documents\\\\a.txt"));
		QCOMPARE(scrubbed, QStringLiteral("%USERPROFILE%\\\\Documents\\\\a.txt"));

		auto appData = QString(native + QStringLiteral("\\AppData\\Local")).replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
		scrubbed = qs::log::scrubLogText(appData + QStringLiteral("\\\\x"));
		QCOMPARE(scrubbed, QStringLiteral("%LOCALAPPDATA%\\\\x"));
	}

	void profileFolderNameIsRedactedWhenItDiffersFromTheAccount() {
		auto scrubbed = qs::log::scrubLogText(QStringLiteral("folder Case Sample was opened"));
		QCOMPARE(scrubbed, QStringLiteral("folder <user> was opened"));

		scrubbed = qs::log::scrubLogText(QStringLiteral("account ci-account was opened"));
		QCOMPARE(scrubbed, QStringLiteral("account <user> was opened"));

		auto text = QStringLiteral("Case Samples");
		QCOMPARE(qs::log::scrubLogText(text), text);
	}

#ifdef Q_OS_WIN
	void shortNameAliasIsReplaced() {
		auto native = QDir::toNativeSeparators(this->profile);
		auto wide = native.toStdWString();
		auto size = GetShortPathNameW(wide.c_str(), nullptr, 0);
		if (size == 0) QSKIP("The profile has no 8.3 alias");

		auto buffer = std::wstring(size, L'\0');
		auto written = GetShortPathNameW(wide.c_str(), buffer.data(), size);
		buffer.resize(written);
		auto alias = QString::fromStdWString(buffer);
		if (alias.compare(native, Qt::CaseInsensitive) == 0) QSKIP("8.3 names are disabled for this volume");

		auto scrubbed = qs::log::scrubLogText(alias + QStringLiteral("\\Documents\\a.txt"));
		QCOMPARE(scrubbed, QStringLiteral("%USERPROFILE%\\Documents\\a.txt"));

		auto doubled = QString(alias).replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
		scrubbed = qs::log::scrubLogText(doubled + QStringLiteral("\\\\x"));
		QCOMPARE(scrubbed, QStringLiteral("%USERPROFILE%\\\\x"));

		auto leaf = alias.mid(alias.lastIndexOf(QLatin1Char('\\')) + 1);
		scrubbed = qs::log::scrubLogText(QStringLiteral("folder ") + leaf + QStringLiteral(" was opened"));
		QCOMPARE(scrubbed, QStringLiteral("folder <user> was opened"));
	}
#endif
	void profilePathIsReplaced() {
		auto text = QDir::homePath() + QStringLiteral("/Documents/report.txt");
		auto scrubbed = qs::log::scrubLogText(text);

		QVERIFY(scrubbed.contains(QStringLiteral("%USERPROFILE%/Documents/report.txt")));
		QVERIFY(!scrubbed.contains(QDir::homePath()));
	}

	void backslashesAndCaseAreReplaced() {
		auto path = QDir::homePath();
		path.replace(QLatin1Char('/'), QLatin1Char('\\'));
		auto scrubbed = qs::log::scrubLogText(path.toUpper() + QStringLiteral("\\Photos\\a.png"));

		QVERIFY(scrubbed.contains(QStringLiteral("%USERPROFILE%\\Photos\\a.png")));
	}

	void longestPrefixWins() {
		auto localAppData = qEnvironmentVariable("LOCALAPPDATA");
		if (localAppData.isEmpty()) QSKIP("LOCALAPPDATA is not set");

		auto scrubbed = qs::log::scrubLogText(localAppData + QStringLiteral("\\Cache\\x.json"));
		QVERIFY(scrubbed.contains(QStringLiteral("%LOCALAPPDATA%\\Cache\\x.json")));
		QVERIFY(!scrubbed.contains(QStringLiteral("%USERPROFILE%")));
	}

	void tempWinsOverTheProfile() {
		// cleanPath drops the trailing separator initTestCase gives TEMP.
		auto temp = QDir::toNativeSeparators(QDir::cleanPath(qEnvironmentVariable("TEMP")));
		auto home = QDir::homePath();
		auto normalizedTemp = QString(temp).replace(QLatin1Char('\\'), QLatin1Char('/'));
		auto normalizedHome = QString(home).replace(QLatin1Char('\\'), QLatin1Char('/'));
		if (normalizedTemp.isEmpty() || !normalizedTemp.startsWith(normalizedHome, Qt::CaseInsensitive)) {
			QSKIP("TEMP is not under home");
		}

		auto scrubbed = qs::log::scrubLogText(temp + QStringLiteral("\\run.log"));
		QVERIFY(scrubbed.contains(QStringLiteral("%TEMP%\\run.log")));
	}

	void usernameIsReplaced() {
		auto username = qEnvironmentVariable("USERNAME");
		if (username.isEmpty()) username = qEnvironmentVariable("USER");
		if (username.isEmpty()) QSKIP("No account username in the environment");

		auto scrubbed = qs::log::scrubLogText(QStringLiteral("session for ") + username + QStringLiteral(" started"));
		QVERIFY(scrubbed.contains(QStringLiteral("session for <user> started")));
		QVERIFY(!scrubbed.contains(username));
	}

	void usernameInsideLongerWordIsKept() {
		auto username = qEnvironmentVariable("USERNAME");
		if (username.isEmpty()) username = qEnvironmentVariable("USER");
		if (username.isEmpty()) QSKIP("No account username in the environment");

		auto text = username + QStringLiteral("2");
		QCOMPARE(qs::log::scrubLogText(text), text);
	}

	void unrelatedPathsAreKept() {
		auto text = QStringLiteral("D:/Games/foo/bar.txt");
		QCOMPARE(qs::log::scrubLogText(text), text);
	}

	void messagesWithoutCandidatesAreUnchanged() {
		auto text = QStringLiteral("Bluetooth adapter enabled");
		QCOMPARE(qs::log::scrubLogText(text), text);
	}
};

QTEST_GUILESS_MAIN(LogScrubTest)
#include "log_scrub_test.moc"
