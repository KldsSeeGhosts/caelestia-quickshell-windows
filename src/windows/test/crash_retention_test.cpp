#include <qfile.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qt_windows.h>

#include "../crash/retention.hpp"

namespace {
bool writeFile(const QString& path, qint64 time) {
	QFile file(path);
	if (!file.open(QFile::WriteOnly)) return false;
	if (file.write("fixture") != 7) return false;
	return file.setFileTime(QDateTime::fromMSecsSinceEpoch(time), QFileDevice::FileModificationTime);
}

bool makeReport(const QDir& root, const QString& id, qint64 time, bool support = true) {
	if (!root.mkpath(id)) return false;
	QDir dir(root.filePath(id));
	if (!writeFile(dir.filePath(id + "-123.dmp"), time)) return false;
	return !support
	    || (writeFile(dir.filePath("report.txt"), time)
	        && writeFile(dir.filePath("log.qslog.log"), time));
}
} // namespace

class TestCrashRetention: public QObject {
	Q_OBJECT;

private slots:
	void newestThreeAndTheirSupportSurvive() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QDir root(temp.path());
		for (int i = 1; i <= 6; ++i) {
			QVERIFY(makeReport(root, QString::number(i), 100000 + i * 10000));
		}
		qs::crash::pruneCrashReports(root);
		for (int i = 1; i <= 6; ++i) {
			const auto id = QString::number(i);
			QCOMPARE(root.exists(id), i > 3);
			if (i > 3) {
				QVERIFY(QFile::exists(root.filePath(id + "/report.txt")));
				QVERIFY(QFile::exists(root.filePath(id + "/log.qslog.log")));
			}
		}
		qs::crash::pruneCrashReports(root);
		QCOMPARE(root.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 3);
	}

	void partialCrashesCountButUnknownContentsArePreserved() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QDir root(temp.path());
		for (int i = 1; i <= 4; ++i) {
			QVERIFY(makeReport(root, QString::number(i), 100000 + i * 10000, false));
		}
		QVERIFY(makeReport(root, "foreign", 1));
		QVERIFY(writeFile(root.filePath("foreign/user.txt"), 1));
		QVERIFY(root.mkpath("empty"));
		QVERIFY(root.mkpath("nested/subdir"));
		qs::crash::pruneCrashReports(root);
		QVERIFY(!root.exists("1"));
		QVERIFY(root.exists("2"));
		QVERIFY(root.exists("foreign/foreign-123.dmp"));
		QVERIFY(root.exists("foreign/user.txt"));
		QVERIFY(root.exists("empty"));
		QVERIFY(root.exists("nested/subdir"));
	}

	void missingRootIsNotCreated() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QDir root(QDir(temp.path()).filePath("missing"));
		qs::crash::pruneCrashReports(root);
		QVERIFY(!root.exists());
	}

	void reportWithoutDumpIsAlsoPruned() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QDir root(temp.path());
		QVERIFY(root.mkpath("old"));
		QVERIFY(writeFile(root.filePath("old/report.txt"), 1));
		for (int i = 1; i <= 3; ++i) {
			QVERIFY(makeReport(root, QString::number(i), 100000 + i * 10000));
		}
		qs::crash::pruneCrashReports(root);
		QVERIFY(!root.exists("old"));
	}

	void lockedDumpKeepsItsSupportingFiles() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QDir root(temp.path());
		for (int i = 1; i <= 4; ++i) {
			QVERIFY(makeReport(root, QString::number(i), 100000 + i * 10000));
		}
		const auto path = root.filePath("1/1-123.dmp");
		HANDLE handle = CreateFileW(
		    reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, 0, nullptr,
		    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
		);
		QVERIFY(handle != INVALID_HANDLE_VALUE);
		qs::crash::pruneCrashReports(root);
		const bool kept = root.exists("1/1-123.dmp") && root.exists("1/report.txt")
		    && root.exists("1/log.qslog.log");
		CloseHandle(handle);
		QVERIFY(kept);
		qs::crash::pruneCrashReports(root);
		QVERIFY(!root.exists("1"));
	}

	void symbolicDirectoriesAreNotTraversed() {
		QTemporaryDir temp;
		QTemporaryDir outside;
		QVERIFY(temp.isValid());
		QVERIFY(outside.isValid());
		QDir root(temp.path());
		QVERIFY(makeReport(QDir(outside.path()), "old", 1));
		const auto link = root.filePath("old");
		const auto target = QDir(outside.path()).filePath("old");
		if (!CreateSymbolicLinkW(
		        reinterpret_cast<LPCWSTR>(link.utf16()),
		        reinterpret_cast<LPCWSTR>(target.utf16()),
		        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
		    ))
		{
			QSKIP("Creating directory symlinks requires developer mode or an existing privilege.");
		}
		for (int i = 1; i <= 3; ++i) {
			QVERIFY(makeReport(root, QString::number(i), 100000 + i * 10000));
		}
		qs::crash::pruneCrashReports(root);
		const bool kept = QFile::exists(QDir(target).filePath("old-123.dmp"));
		// Remove only the link. Never traverse the target during fixture cleanup.
		QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16())));
		QVERIFY(kept);
	}
};

QTEST_GUILESS_MAIN(TestCrashRetention)

#include "crash_retention_test.moc"
