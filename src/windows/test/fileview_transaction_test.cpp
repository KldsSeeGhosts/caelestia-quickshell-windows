#include <atomic>

#include <qfile.h>
#include <qsavefile.h>
#include <qsignalspy.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qthreadpool.h>

#include "../../io/fileview.hpp"

namespace {
std::atomic<bool> denyCommit = false;
std::atomic<int> commitAttempts = 0;

class Adapter: public qs::io::FileViewAdapter {
public:
	QByteArray bytes;
	void deserializeAdapter(const QByteArray&) override {}
	QByteArray serializeAdapter() override { return this->bytes; }
};
} // namespace

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace qs::io {
// Exercise QSaveFile's actual failed-commit path, without filesystem races or ACLs.
void prepareFileViewCommit(QSaveFile& file) {
	++commitAttempts;
	if (denyCommit) file.cancelWriting();
}
} // namespace qs::io

using namespace qs::io;

class FileViewTransactionTest: public QObject {
	Q_OBJECT;

private slots:
	void init() {
		denyCommit = false;
		commitAttempts = 0;
	}

	void cleanup() {
		QThreadPool::globalInstance()->waitForDone();
		QCoreApplication::sendPostedEvents();
	}

	void identicalRetry_data() {
		QTest::addColumn<int>("mode");
		QTest::addColumn<int>("setter");
		QTest::addColumn<bool>("empty");
		for (int mode = 0; mode < 3; ++mode) {
			for (int setter = 0; setter < 3; ++setter) {
				for (bool empty: {false, true}) {
					QTest::newRow(qPrintable(QString("%1-%2-%3").arg(mode).arg(setter).arg(empty)))
					    << mode << setter << empty;
				}
			}
		}
	}

	void identicalRetry() {
		QFETCH(int, mode);
		QFETCH(int, setter);
		QFETCH(bool, empty);
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const auto path = dir.filePath("data");
		QFile disk(path);
		QVERIFY(disk.open(QIODevice::WriteOnly));
		QCOMPARE(disk.write("committed"), 9);
		disk.close();

		Adapter adapter;
		FileView view;
		view.setPath(path);
		QVERIFY(view.waitForJob());
		view.bindableBlockWrites().setValue(mode == 2);
		view.setAdapter(&adapter);
		const auto bytes = empty ? QByteArray() : QByteArray("attempted");
		adapter.bytes = bytes;
		auto write = [&]() {
			if (setter == 0) view.setText(QString::fromUtf8(bytes));
			else if (setter == 1) view.setData(bytes);
			else view.writeAdapter();
		};
		QSignalSpy saved(&view, &FileView::saved);
		QSignalSpy failed(&view, &FileView::saveFailed);
		denyCommit = true;
		write();
		if (mode == 1) QVERIFY(view.waitForJob());
		QTRY_COMPARE(failed.count(), 1);
		QCOMPARE(saved.count(), 0);
		QCOMPARE(failed.at(0).at(0).value<FileViewError::Enum>(), FileViewError::Unknown);
		QCOMPARE(view.data(), QByteArray("committed"));
		QVERIFY(disk.open(QIODevice::ReadOnly));
		QCOMPARE(disk.readAll(), QByteArray("committed"));
		disk.close();

		denyCommit = false;
		write();
		if (mode == 1) QVERIFY(view.waitForJob());
		QTRY_COMPARE(saved.count(), 1);
		QCOMPARE(failed.count(), 1);
		QCOMPARE(commitAttempts.load(), 2);
		QCOMPARE(view.data(), bytes);
		QVERIFY(disk.open(QIODevice::ReadOnly));
		QCOMPARE(disk.readAll(), bytes);
	}

	void reentrantCompletion_data() {
		QTest::addColumn<int>("mode");
		QTest::addColumn<QString>("signal");
		QTest::addColumn<bool>("nestedWait");
		for (int mode = 0; mode < 3; ++mode) {
			for (const auto& signal: {"saved", "saveFailed", "loaded", "loadFailed"}) {
				for (bool nestedWait: {false, true}) {
					QTest::newRow(qPrintable(QString("%1-%2-%3").arg(mode).arg(signal).arg(nestedWait)))
					    << mode << QString(signal) << nestedWait;
				}
			}
		}
	}

	void reentrantCompletion() {
		QFETCH(int, mode);
		QFETCH(QString, signal);
		QFETCH(bool, nestedWait);
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const auto path = dir.filePath("data");
		if (signal != "loadFailed") {
			QFile disk(path);
			QVERIFY(disk.open(QIODevice::WriteOnly));
			QCOMPARE(disk.write("initial"), 7);
		}

		FileView view;
		QVERIFY(view.setProperty("__preload", false));
		view.bindableBlockWrites().setValue(mode == 2);
		QVERIFY(view.setProperty("__blockAllReads", mode == 2));
		view.setPath(path);
		QSignalSpy saved(&view, &FileView::saved);
		QSignalSpy failed(&view, &FileView::saveFailed);
		QSignalSpy loaded(&view, &FileView::loaded);
		QSignalSpy loadFailed(&view, &FileView::loadFailed);
		bool started = false;
		bool waited = false;
		auto next = [&]() {
			if (started) return;
			started = true;
			denyCommit = false;
			view.setData("second");
			if (nestedWait) waited = view.waitForJob();
		};
		if (signal == "saved") connect(&view, &FileView::saved, &view, next);
		else if (signal == "saveFailed") connect(&view, &FileView::saveFailed, &view, next);
		else if (signal == "loaded") connect(&view, &FileView::loaded, &view, next);
		else connect(&view, &FileView::loadFailed, &view, next);

		if (signal == "loaded" || signal == "loadFailed") {
			(void) view.data();
		} else {
			denyCommit = signal == "saveFailed";
			view.setText("first");
		}
		if (mode == 1) QVERIFY(view.waitForJob());
		const auto saves = signal == "saved" ? 2 : 1;
		QTRY_COMPARE(saved.count(), saves);
		QVERIFY(started);
		QCOMPARE(waited, nestedWait && mode != 2);
		QCOMPARE(failed.count(), signal == "saveFailed" ? 1 : 0);
		QCOMPARE(loaded.count(), signal == "loaded" ? 1 : 0);
		QCOMPARE(loadFailed.count(), signal == "loadFailed" ? 1 : 0);
		QCOMPARE(view.data(), QByteArray("second"));
		QVERIFY(!view.waitForJob());
		QFile disk(path);
		QVERIFY(disk.open(QIODevice::ReadOnly));
		QCOMPARE(disk.readAll(), QByteArray("second"));
	}
};

QTEST_GUILESS_MAIN(FileViewTransactionTest)
#include "fileview_transaction_test.moc"
