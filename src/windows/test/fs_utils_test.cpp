#include <qdir.h>
#include <qfile.h>
#include <qlist.h>
#include <qmap.h>
#include <qsignalspy.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qtestcase.h>
#include <qvariant.h>
#include <qvariantlist.h>
#include <qvariantmap.h>

#include <qt_windows.h>

#include "fs_utils.hpp"

using qs::windows::sys::FsUtils;

namespace {

bool writeFile(const QString& path) {
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
}

QStringList names(const QVariantList& entries) {
	auto result = QStringList();
	for (const auto& value: entries)
		result.append(value.toMap().value(QStringLiteral("name")).toString());
	return result;
}

QMap<QString, bool> onDiskByName(const QVariantList& entries) {
	auto result = QMap<QString, bool>();
	for (const auto& value: entries) {
		const auto map = value.toMap();
		result.insert(
		    map.value(QStringLiteral("name")).toString(),
		    map.value(QStringLiteral("onDisk")).toBool()
		);
	}
	return result;
}

} // namespace

class TestFsUtils: public QObject {
	Q_OBJECT;

private slots:
	void asyncListingDeliversFiles();
	void onDiskOnlyStillListsPlaceholders();
	void missingDirectoryStillDelivers();
};

void TestFsUtils::asyncListingDeliversFiles() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	QVERIFY(writeFile(dir.filePath(QStringLiteral("z.png"))));
	QVERIFY(writeFile(dir.filePath(QStringLiteral("a.jpg"))));
	QVERIFY(writeFile(dir.filePath(QStringLiteral("notes.txt"))));
	QVERIFY(writeFile(dir.filePath(QStringLiteral(".hidden.png"))));
	const auto hidden = dir.filePath(QStringLiteral(".hidden.png")).toStdWString();
	QVERIFY(SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN));

	FsUtils utils;
	QSignalSpy spy(&utils, &FsUtils::listDirFinished);
	const auto request = utils.listDirAsync(dir.path());

	// The answer is posted from the worker and needs the event loop, so the
	// call itself must not have produced it.
	QCOMPARE(spy.count(), 0);
	QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);

	const auto args = spy.takeFirst();
	QCOMPARE(args.at(0).toInt(), request);
	QCOMPARE(args.at(1).toString(), dir.path());
	const auto entries = args.at(2).toList();
	QCOMPARE(names(entries), (QStringList {"a.jpg", "notes.txt", "z.png"}));
	const auto onDisk = onDiskByName(entries);
	QVERIFY(onDisk.value(QStringLiteral("a.jpg")));
	QVERIFY(onDisk.value(QStringLiteral("notes.txt")));
	QVERIFY(onDisk.value(QStringLiteral("z.png")));
}

void TestFsUtils::onDiskOnlyStillListsPlaceholders() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const auto local = dir.filePath(QStringLiteral("local.png"));
	const auto placeholder = dir.filePath(QStringLiteral("placeholder.png"));
	QVERIFY(writeFile(local));
	QVERIFY(writeFile(placeholder));

	const auto widePlaceholder = placeholder.toStdWString();
	if (!SetFileAttributesW(widePlaceholder.c_str(), FILE_ATTRIBUTE_OFFLINE))
		QSKIP("This filesystem cannot mark a file offline");
	if ((GetFileAttributesW(widePlaceholder.c_str()) & FILE_ATTRIBUTE_OFFLINE) == 0)
		QSKIP("The offline attribute did not persist");

	FsUtils utils;
	QSignalSpy spy(&utils, &FsUtils::listDirFinished);
	utils.listDirAsync(dir.path());
	QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);

	const auto entries = spy.takeFirst().at(2).toList();
	const auto onDisk = onDiskByName(entries);
	// The placeholder is still listed, but is marked as not local so the
	// thumbnail path can leave it alone until the user opens it.
	QVERIFY(onDisk.contains(QStringLiteral("placeholder.png")));
	QVERIFY(!onDisk.value(QStringLiteral("placeholder.png")));
	QVERIFY(onDisk.value(QStringLiteral("local.png")));
}

void TestFsUtils::missingDirectoryStillDelivers() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	FsUtils utils;
	QSignalSpy spy(&utils, &FsUtils::listDirFinished);
	utils.listDirAsync(dir.filePath(QStringLiteral("missing")));
	QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);

	const auto entries = spy.takeFirst().at(2).toList();
	QVERIFY(entries.isEmpty());
}

QTEST_GUILESS_MAIN(TestFsUtils)

#include "fs_utils_test.moc"
