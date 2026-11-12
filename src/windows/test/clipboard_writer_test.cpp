#include <array>
#include <memory>

#include <qcoreapplication.h>
#include <qcoreevent.h>
#include <qdir.h>
#include <qfile.h>
#include <qsemaphore.h>
#include <qsignalspy.h>
#include <qt_windows.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qthread.h>
#include <qthreadpool.h>

#include "../../core/instanceinfo.hpp"
#include "../system/clipboard.hpp"
#include "clipboard_test_providers.hpp"

using namespace qs::windows::sys;

namespace qs::windows::sys {

class ClipboardTestAccess {
public:
	static std::unique_ptr<Clipboard> history(
	    std::shared_ptr<ClipboardReadProvider> reader,
	    std::shared_ptr<ClipboardListenerProvider> listener,
	    std::shared_ptr<ClipboardWriteProvider> writer,
	    const QString& directory,
	    QThreadPool* pool
	) {
		return std::unique_ptr<Clipboard>(
		    new Clipboard(std::move(reader), std::move(listener), std::move(writer), directory, pool)
		);
	}
	static std::unique_ptr<ClipboardWriter>
	writer(std::shared_ptr<ClipboardWriteProvider> writer, QThreadPool* pool) {
		return std::unique_ptr<ClipboardWriter>(new ClipboardWriter(std::move(writer), pool));
	}
	static bool dormant(const Clipboard& clipboard) {
		return clipboard.mCacheDir.isEmpty() && !clipboard.captureGeneration
		    && !clipboard.listenerContext && clipboard.storage.empty();
	}
	static void update(Clipboard& clipboard) { clipboard.onClipboardUpdate(); }
	static bool capturing(const Clipboard& clipboard) { return clipboard.capturing; }
	static size_t entries(const Clipboard& clipboard) { return clipboard.storage.size(); }
	static quint32 writtenSequence(const Clipboard& clipboard) { return clipboard.writtenSequence; }
};

} // namespace qs::windows::sys

namespace {

constexpr int WAIT_MS = 5000;

class FakeReader final: public ClipboardReadProvider {
public:
	int reads = 0;
	int sequences = 0;
	quint32 currentSequence = 100;
	bool hold = false;
	bool timedOut = false;
	QSemaphore reached;
	QSemaphore resume;

	std::array<quint32, 3> privacyFormats() override { return {0xc001, 0xc002, 0xc003}; }
	bool open() override {
		++this->reads;
		return true;
	}
	void close() override {
		if (!this->hold) return;
		this->hold = false;
		this->reached.release();
		if (!this->resume.tryAcquire(1, WAIT_MS)) this->timedOut = true;
	}
	void retryPause() override {}
	quint32 sequence() override {
		++this->sequences;
		return this->currentSequence;
	}
	bool available(quint32 format) override { return format == CF_UNICODETEXT; }
	std::optional<QByteArray> data(quint32) override {
		auto text = QString(32769, QChar(u'x'));
		return QByteArray(reinterpret_cast<const char*>(text.utf16()), (text.size() + 1) * 2);
	}
};

QString fixtureTemplate() {
	auto root = qEnvironmentVariable("QS_CLIPBOARD_FIXTURE_ROOT");
	if (!QDir::isAbsolutePath(root) || !QDir(root).exists()) {
		qFatal("Set QS_CLIPBOARD_FIXTURE_ROOT to an existing disposable absolute directory");
	}
	return root + QStringLiteral("/writer-XXXXXX");
}

struct Fixture {
	QTemporaryDir directory {fixtureTemplate()};
	QThreadPool pool;
	std::shared_ptr<FakeReader> reader = std::make_shared<FakeReader>();
	std::shared_ptr<FakeClipboardListener> listener = std::make_shared<FakeClipboardListener>();
	std::shared_ptr<FakeClipboardWrite> provider = std::make_shared<FakeClipboardWrite>();
	std::unique_ptr<Clipboard> clipboard;
	std::unique_ptr<ClipboardWriter> writer;

	Fixture() {
		if (!this->directory.isValid()) qFatal("Cannot create disposable fixture directory");
		this->pool.setMaxThreadCount(1);
		this->clipboard = ClipboardTestAccess::history(
		    this->reader,
		    this->listener,
		    this->provider,
		    this->cache(),
		    &this->pool
		);
		this->writer = ClipboardTestAccess::writer(this->provider, &this->pool);
	}
	~Fixture() {
		this->reader->resume.release();
		this->pool.waitForDone(WAIT_MS);
		this->writer.reset();
		this->clipboard.reset();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	}
	QString cache() const { return this->directory.path() + QStringLiteral("/cache"); }
	QString path(const QString& name) const { return this->directory.path() + "/" + name; }
	bool settle() {
		if (!this->pool.waitForDone(WAIT_MS)) return false;
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		return !ClipboardTestAccess::capturing(*this->clipboard);
	}
};

} // namespace

class TestClipboardWriter: public QObject {
	Q_OBJECT;

private slots:
	void defaultConstruction();
	void historyLifecycle();
	void disabledCapture_data();
	void disabledCapture();
	void shellFilenameGuard();
	void listenerFailure();
	void outcomes_data();
	void outcomes();
	void overlappingOperations();
	void retrySuccess();
	void destructionDuringRetry();
	void destructionDuringDecode();
	void legacyWrites();
	void legacyCopyFailureDoesNotSuppress();
};

void TestClipboardWriter::defaultConstruction() {
	// Production constructors must not enter the fatal native-window/prune stubs.
	ClipboardWriter productionWriter;
	Clipboard productionHistory;
	QVERIFY(!productionHistory.historyEnabled());
	QVERIFY(ClipboardTestAccess::dormant(productionHistory));
	productionHistory.wipe();
	ClipboardTestAccess::update(productionHistory);
	QVERIFY(ClipboardTestAccess::dormant(productionHistory));

	Fixture f;
	QVERIFY(!f.clipboard->historyEnabled());
	ClipboardTestAccess::update(*f.clipboard);
	f.clipboard->wipe();
	QVERIFY(f.settle());
	QCOMPARE(f.listener->starts, 0);
	QCOMPARE(f.listener->stops, 0);
	QCOMPARE(f.reader->reads, 0);
	QCOMPARE(f.reader->sequences, 0);
	QCOMPARE(f.provider->opens, 0);
	QVERIFY(!QFile::exists(f.cache()));
}

void TestClipboardWriter::historyLifecycle() {
	Fixture f;
	QSignalSpy changes(f.clipboard.get(), &Clipboard::historyEnabledChanged);
	f.clipboard->setHistoryEnabled(true);
	f.clipboard->setHistoryEnabled(true);
	QCOMPARE(f.listener->starts, 1);
	QVERIFY(f.clipboard->historyEnabled());
	f.listener->notify();
	QVERIFY(f.settle());
	QCOMPARE(ClipboardTestAccess::entries(*f.clipboard), size_t(1));
	QVERIFY(QFile::exists(f.cache() + "/1.txt"));

	f.clipboard->setHistoryEnabled(false);
	f.clipboard->setHistoryEnabled(false);
	QCOMPARE(f.listener->stops, 1);
	QVERIFY(f.listener->context.isNull());
	QCOMPARE(ClipboardTestAccess::entries(*f.clipboard), size_t(0));
	QVERIFY(f.clipboard->bindableEntries().value().empty());
	QVERIFY(!QFile::exists(f.cache()));
	auto reads = f.reader->reads;
	auto sequences = f.reader->sequences;
	f.listener->notify();
	ClipboardTestAccess::update(*f.clipboard); // Previously queued native update.
	QVERIFY(f.settle());
	QCOMPARE(f.reader->reads, reads);
	QCOMPARE(f.reader->sequences, sequences);

	f.clipboard->setHistoryEnabled(true);
	QCOMPARE(f.listener->starts, 2);
	f.reader->currentSequence = 101;
	f.listener->notify();
	QVERIFY(f.settle());
	QCOMPARE(ClipboardTestAccess::entries(*f.clipboard), size_t(1));
	QCOMPARE(changes.size(), 3);
	f.clipboard.reset();
	QCOMPARE(f.listener->stops, 2);
	QVERIFY(!QFile::exists(f.cache()));
}

void TestClipboardWriter::disabledCapture_data() {
	QTest::addColumn<bool>("queued");
	QTest::newRow("worker-in-flight") << false;
	QTest::newRow("spill-result-queued") << true;
}

void TestClipboardWriter::disabledCapture() {
	QFETCH(bool, queued);
	Fixture f;
	f.clipboard->setHistoryEnabled(true);
	f.reader->hold = !queued;
	f.listener->notify();
	if (queued) {
		QVERIFY(f.pool.waitForDone(WAIT_MS));
		QVERIFY(QFile::exists(f.cache() + "/1.txt"));
	} else {
		QVERIFY(f.reader->reached.tryAcquire(1, WAIT_MS));
	}
	f.clipboard->setHistoryEnabled(false);
	f.reader->resume.release();
	QVERIFY(f.settle());
	QVERIFY(!f.reader->timedOut);
	QCOMPARE(ClipboardTestAccess::entries(*f.clipboard), size_t(0));
	QVERIFY(!QFile::exists(f.cache()));
	f.clipboard->setHistoryEnabled(true);
	f.reader->currentSequence = 101;
	f.listener->notify();
	QVERIFY(f.settle());
	QCOMPARE(ClipboardTestAccess::entries(*f.clipboard), size_t(1));
}

void TestClipboardWriter::shellFilenameGuard() {
	auto previous = InstanceInfo::CURRENT.configPath;
	InstanceInfo::CURRENT.configPath = QStringLiteral("fixture/not-shell.qml");
	Clipboard clipboard;
	clipboard.setHistoryEnabled(true);
	QVERIFY(!clipboard.historyEnabled());
	QVERIFY(ClipboardTestAccess::dormant(clipboard));
	InstanceInfo::CURRENT.configPath = previous;
}

void TestClipboardWriter::listenerFailure() {
	Fixture f;
	f.listener->allow = false;
	f.clipboard->setHistoryEnabled(true);
	QVERIFY(!f.clipboard->historyEnabled());
	f.clipboard->setHistoryEnabled(false);
	QCOMPARE(f.listener->stops, 0);
	QCOMPARE(f.reader->reads, 0);
	QVERIFY(!QFile::exists(f.cache()));
}

void TestClipboardWriter::outcomes_data() {
	QTest::addColumn<QString>("fault");
	for (const auto* fault:
	     {"success",
	      "read-failed",
	      "decode-failed",
	      "allocation-failed",
	      "clipboard-busy",
	      "set-failed",
	      "empty-failed"})
	{
		QTest::newRow(fault) << QString::fromLatin1(fault);
	}
}

void TestClipboardWriter::outcomes() {
	QFETCH(QString, fault);
	Fixture f;
	auto path = f.path(QStringLiteral("image.png"));
	if (fault == "decode-failed") {
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		QCOMPARE(file.write("not an image"), 12);
	} else if (fault != "read-failed") {
		QImage image(2, 3, QImage::Format_RGB32);
		image.fill(Qt::red);
		QVERIFY(image.save(path));
	}
	f.provider->allocate = fault != "allocation-failed";
	f.provider->allowOpen = fault != "clipboard-busy";
	f.provider->allowSet = fault != "set-failed";
	f.provider->allowEmpty = fault != "empty-failed";
	QSignalSpy finished(f.writer.get(), &ClipboardWriter::writeFinished);
	bool guiThread = false;
	connect(f.writer.get(), &ClipboardWriter::writeFinished, this, [&guiThread]() {
		guiThread = QThread::currentThread() == QCoreApplication::instance()->thread();
	});
	auto id = f.writer->copyImageFile(path);
	QVERIFY(id > 0);
	QCOMPARE(finished.size(), 0);
	// The caller can remove its hand-over file as soon as the call returns.
	QFile::remove(path);
	QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, WAIT_MS);
	QVERIFY(guiThread);
	auto result = finished.front();
	QCOMPARE(result[0].toInt(), id);
	QCOMPARE(result[1].toBool(), fault == "success");
	QCOMPARE(
	    result[2].toString(),
	    fault == "success"        ? QString()
	    : fault == "empty-failed" ? QStringLiteral("set-failed")
	                              : fault
	);
	QVERIFY(f.pool.waitForDone(WAIT_MS));
	QCoreApplication::processEvents();
	QCOMPARE(finished.size(), 1);
	QVERIFY(f.provider->owned.isEmpty());
	QCOMPARE(f.provider->transfers, fault == "success" ? 1 : 0);
	QCOMPARE(
	    f.provider->opens,
	    fault == "clipboard-busy"                                                ? 5
	    : fault == "success" || fault == "set-failed" || fault == "empty-failed" ? 1
	                                                                             : 0
	);
	if (fault == "success") {
		QCOMPARE(f.provider->image.size(), QSize(2, 3));
		QCOMPARE(f.provider->image.pixelColor(0, 0), QColor(Qt::red));
	}
	QCOMPARE(f.listener->starts, 0);
	QCOMPARE(f.reader->reads, 0);
	QVERIFY(!QFile::exists(f.cache()));
}

void TestClipboardWriter::overlappingOperations() {
	Fixture f;
	QSignalSpy finished(f.writer.get(), &ClipboardWriter::writeFinished);
	auto bad = f.writer->copyImageFile(f.path(QStringLiteral("missing.png")));
	auto good = f.writer->copyText(QStringLiteral("fixture text"));
	QVERIFY(bad > 0 && good > 0 && bad != good);
	QCOMPARE(finished.size(), 0);
	QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, WAIT_MS);
	QSet<int> seen;
	for (const auto& result: finished) {
		auto id = result[0].toInt();
		QVERIFY(!seen.contains(id));
		seen.insert(id);
		QCOMPARE(result[1].toBool(), id == good);
		QCOMPARE(result[2].toString(), id == good ? QString() : QStringLiteral("read-failed"));
	}
	QVERIFY(seen.contains(bad) && seen.contains(good));
	QCOMPARE(f.provider->text, QStringLiteral("fixture text"));
	QCoreApplication::processEvents();
	QCOMPARE(finished.size(), 2);
}

void TestClipboardWriter::retrySuccess() {
	Fixture f;
	f.provider->failedOpens = 4;
	QSignalSpy finished(f.writer.get(), &ClipboardWriter::writeFinished);
	auto id = f.writer->copyText(QStringLiteral("retry fixture"));
	QCOMPARE(finished.size(), 0);
	QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, WAIT_MS);
	QCOMPARE(finished.front()[0].toInt(), id);
	QVERIFY(finished.front()[1].toBool());
	QCOMPARE(f.provider->opens, 5);
	QCOMPARE(f.provider->transfers, 1);
	QCOMPARE(f.provider->frees, 0);
}

void TestClipboardWriter::destructionDuringRetry() {
	Fixture f;
	f.provider->allowOpen = false;
	f.writer->copyText(QStringLiteral("pending retry"));
	QCoreApplication::processEvents();
	QCOMPARE(f.provider->opens, 1);
	QCOMPARE(f.provider->owned.size(), 1);
	f.writer.reset();
	QCOMPARE(f.provider->frees, 1);
	QVERIFY(f.provider->owned.isEmpty());
	QCoreApplication::processEvents();
	QCOMPARE(f.provider->opens, 1);
}

void TestClipboardWriter::destructionDuringDecode() {
	Fixture f;
	auto path = f.path(QStringLiteral("image.png"));
	QImage image(2, 2, QImage::Format_RGB32);
	image.fill(Qt::blue);
	QVERIFY(image.save(path));
	f.writer->copyImageFile(path);
	QVERIFY(f.pool.waitForDone(WAIT_MS)); // Decoded result is queued, not delivered.
	f.writer.reset();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	QCOMPARE(f.provider->opens, 0);
	QVERIFY(f.provider->owned.isEmpty());
}

void TestClipboardWriter::legacyWrites() {
	Fixture f;
	QVERIFY(!f.clipboard->copyImageFile(f.path(QStringLiteral("missing.png"))));
	f.clipboard->copyText(QStringLiteral("legacy text"));
	QTRY_COMPARE_WITH_TIMEOUT(f.provider->transfers, 1, WAIT_MS);
	auto path = f.path(QStringLiteral("image.png"));
	QImage image(2, 2, QImage::Format_RGB32);
	image.fill(Qt::green);
	QVERIFY(image.save(path));
	QVERIFY(f.clipboard->copyImageFile(path));
	QTRY_COMPARE_WITH_TIMEOUT(f.provider->transfers, 2, WAIT_MS);
	QCOMPARE(f.listener->starts, 0);
	QCOMPARE(f.reader->reads, 0);
	QCOMPARE(f.provider->sequences, 0);
	QVERIFY(!QFile::exists(f.cache()));
}

void TestClipboardWriter::legacyCopyFailureDoesNotSuppress() {
	Fixture f;
	f.clipboard->setHistoryEnabled(true);
	f.listener->notify();
	QVERIFY(f.settle());
	f.provider->allowSet = false;
	f.clipboard->copy(1);
	QTRY_COMPARE_WITH_TIMEOUT(f.provider->sets, 1, WAIT_MS);
	QCOMPARE(ClipboardTestAccess::writtenSequence(*f.clipboard), quint32(0));
	QCOMPARE(f.provider->sequences, 0);
	QCOMPARE(f.provider->frees, 1);
	f.provider->allowSet = true;
	f.clipboard->copy(1);
	QTRY_COMPARE_WITH_TIMEOUT(f.provider->transfers, 1, WAIT_MS);
	QCOMPARE(ClipboardTestAccess::writtenSequence(*f.clipboard), quint32(500));
}

QTEST_GUILESS_MAIN(TestClipboardWriter);
#include "clipboard_writer_test.moc"
