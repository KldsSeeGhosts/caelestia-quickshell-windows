#include <array>
#include <cstring>
#include <memory>
#include <optional>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qcoreevent.h>
#include <qdir.h>
#include <qfile.h>
#include <qhash.h>
#include <qimage.h>
#include <qsemaphore.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qthreadpool.h>

#include "../system/clipboard.hpp"

using namespace qs::windows::sys;

namespace qs::windows::sys {

class ClipboardTestAccess {
public:
	static std::unique_ptr<Clipboard> create(
	    std::shared_ptr<ClipboardReadProvider> provider,
	    const QString& directory,
	    QThreadPool& pool
	) {
		return std::unique_ptr<Clipboard>(new Clipboard(std::move(provider), directory, &pool));
	}
	static void update(Clipboard& clipboard) { clipboard.onClipboardUpdate(); }
	static bool capturing(const Clipboard& clipboard) { return clipboard.capturing; }
	static bool captureAgain(const Clipboard& clipboard) { return clipboard.captureAgain; }
	static const std::vector<ClipboardEntry>& entries(const Clipboard& clipboard) {
		return clipboard.storage;
	}
	static void suppress(Clipboard& clipboard, quint32 sequence, qint64 id = -1) {
		clipboard.suppressedSequence = sequence;
		clipboard.suppressNextCaptureFor = id;
	}
	static void ownWrite(Clipboard& clipboard, quint32 sequence) {
		clipboard.writtenSequence = sequence;
	}
};

} // namespace qs::windows::sys

namespace {

constexpr quint32 EXCLUDE = 0xc001;
constexpr quint32 IGNORE_FORMAT = 0xc002;
constexpr quint32 HISTORY = 0xc003;
constexpr int BARRIER_MS = 5000;

QByteArray textBytes(const QString& text) {
	return QByteArray(reinterpret_cast<const char*>(text.utf16()), (text.size() + 1) * 2);
}

QByteArray dibBytes() {
	BITMAPINFOHEADER header {};
	header.biSize = sizeof(header);
	header.biWidth = 48;
	header.biHeight = 32;
	header.biPlanes = 1;
	header.biBitCount = 32;
	header.biCompression = BI_RGB;
	QByteArray result(sizeof(header) + 48 * 32 * 4, '\0');
	std::memcpy(result.data(), &header, sizeof(header));
	for (qsizetype pixel = sizeof(header); pixel < result.size(); pixel += 4) {
		result[pixel] = 0x22;
		result[pixel + 1] = 0x44;
		result[pixel + 2] = 0x66;
	}
	return result;
}

struct Snapshot {
	quint32 sequence = 100;
	QHash<quint32, std::optional<QByteArray>> formats;
};

Snapshot content(int kind, quint32 sequence = 100) {
	Snapshot snapshot;
	snapshot.sequence = sequence;
	if (kind == 2) snapshot.formats.insert(CF_DIB, dibBytes());
	else snapshot.formats.insert(
	    CF_UNICODETEXT,
	    textBytes(kind == 1 ? QString(32769, QChar(u'x')) : QStringLiteral("generated fixture"))
	);
	return snapshot;
}

// This is a clipboard snapshot boundary, not a replacement implementation of
// the policy or lifecycle. Every decision, conversion, spill and completion
// below runs through clipboard.cpp. Bounds are tiny synthetic inputs, ten
// opens at most, a five-second semaphore timeout and one Jobs=1 test pool.
class FakeClipboard final: public ClipboardReadProvider {
public:
	enum class Hold { None, Close, Retry };
	Snapshot current = content(0);
	std::optional<Snapshot> replaceOnOpen;
	std::array<quint32, 3> registrations {EXCLUDE, IGNORE_FORMAT, HISTORY};
	int failedOpens = 0;
	int opens = 0;
	int closes = 0;
	int pauses = 0;
	int outsideChecks = 0;
	int contentReads = 0;
	int markerReads = 0;
	bool opened = false;
	bool barrierTimedOut = false;
	Hold hold = Hold::None;
	QSemaphore reached;
	QSemaphore resume;

	std::array<quint32, 3> privacyFormats() override { return registrations; }
	bool open() override {
		++this->opens;
		if (this->opens <= this->failedOpens) return false;
		if (this->replaceOnOpen) {
			this->current = std::move(*this->replaceOnOpen);
			this->replaceOnOpen.reset();
		}
		this->snapshot = this->current;
		this->opened = true;
		return true;
	}
	void close() override {
		this->opened = false;
		++this->closes;
		this->barrier(Hold::Close);
	}
	void retryPause() override {
		++this->pauses;
		this->barrier(Hold::Retry);
	}
	quint32 sequence() override {
		return this->opened ? this->snapshot.sequence : this->current.sequence;
	}
	bool available(quint32 format) override {
		if (!this->opened) ++this->outsideChecks;
		return (this->opened ? this->snapshot : this->current).formats.contains(format);
	}
	std::optional<QByteArray> data(quint32 format) override {
		if (!this->opened) ++this->outsideChecks;
		if (format == CF_UNICODETEXT || format == CF_DIB) ++this->contentReads;
		else ++this->markerReads;
		return (this->opened ? this->snapshot : this->current).formats.value(format);
	}

private:
	void barrier(Hold phase) {
		if (this->hold != phase) return;
		this->hold = Hold::None;
		this->reached.release();
		if (!this->resume.tryAcquire(1, BARRIER_MS)) this->barrierTimedOut = true;
	}
	Snapshot snapshot;
};

QString fixtureTemplate() {
	auto root = qEnvironmentVariable("QS_CLIPBOARD_FIXTURE_ROOT");
	// Never silently fall back to a personal temp/cache/profile directory.
	if (!QDir::isAbsolutePath(root) || !QDir(root).exists()) {
		qFatal("Set QS_CLIPBOARD_FIXTURE_ROOT to an existing disposable absolute directory");
	}
	return root + QStringLiteral("/clipboard-XXXXXX");
}

QStringList files(const QString& path) {
	return QDir(path).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

bool settle(Clipboard& clipboard, QThreadPool& pool) {
	for (int pass = 0; pass < 4; ++pass) {
		if (!pool.waitForDone(BARRIER_MS)) return false;
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		if (!ClipboardTestAccess::capturing(clipboard)) return true;
	}
	return false;
}

struct Fixture {
	QTemporaryDir directory {fixtureTemplate()};
	QThreadPool pool;
	std::shared_ptr<FakeClipboard> provider = std::make_shared<FakeClipboard>();
	std::unique_ptr<Clipboard> clipboard;

	explicit Fixture(int kind = 0) {
		if (!this->directory.isValid()) qFatal("Cannot create disposable fixture directory");
		this->pool.setMaxThreadCount(1);
		this->provider->current = content(kind);
		this->clipboard = ClipboardTestAccess::create(this->provider, this->directory.path(), this->pool);
	}
	~Fixture() {
		// Assertion failures must not leave a held worker behind.
		this->provider->resume.release();
		this->pool.waitForDone(BARRIER_MS);
		this->clipboard.reset();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	}
	void update() { ClipboardTestAccess::update(*this->clipboard); }
	const std::vector<ClipboardEntry>& entries() const {
		return ClipboardTestAccess::entries(*this->clipboard);
	}
};

void lateData() {
	QTest::addColumn<int>("kind");
	QTest::addColumn<bool>("queued");
	for (int kind: {1, 2}) {
		for (bool queued: {false, true}) {
			QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(kind).arg(queued)))
			    << kind << queued;
		}
	}
}

} // namespace

class TestClipboardCapture: public QObject {
	Q_OBJECT;

private slots:
	void privacySnapshot_data();
	void privacySnapshot();
	void errorPaths_data();
	void errorPaths();
	void boundedRetryAndNoReuse();
	void clearDuringRetry();
	void clearLateCompletion_data() { lateData(); }
	void clearLateCompletion();
	void queuedResultFromEarlierSnapshot_data();
	void queuedResultFromEarlierSnapshot();
	void clearDropsQueuedAgain();
	void newChangeAfterClear();
	void unknownAndRevertedSequence();
	void clearWithUnknownIdentity();
	void repeatedClear();
	void destructionAndReload_data() { lateData(); }
	void destructionAndReload();
	void staleRemovesOnlyOwnFiles_data();
	void staleRemovesOnlyOwnFiles();
	void duplicateAndSuppression();
	void imageBehavior();
	void writeFailure_data();
	void writeFailure();
};

void TestClipboardCapture::privacySnapshot_data() {
	QTest::addColumn<int>("kind");
	QTest::addColumn<int>("marker");
	QTest::addColumn<bool>("accepted");
	for (int kind: {0, 1, 2}) {
		for (int marker = 0; marker < 8; ++marker) {
			QTest::newRow(qPrintable(QStringLiteral("kind-%1-marker-%2").arg(kind).arg(marker)))
			    << kind << marker << (marker == 3 || marker == 4 || marker == 7);
		}
	}
}

void TestClipboardCapture::privacySnapshot() {
	QFETCH(int, kind);
	QFETCH(int, marker);
	QFETCH(bool, accepted);
	Fixture f(kind);
	auto replacement = content(kind, 101);
	switch (marker) {
	case 0: replacement.formats.insert(EXCLUDE, QByteArray()); break;
	case 1: replacement.formats.insert(IGNORE_FORMAT, QByteArray()); break;
	case 2: replacement.formats.insert(HISTORY, QByteArray(4, '\0')); break;
	case 3: replacement.formats.insert(HISTORY, QByteArray(1, '\1')); break;
	case 4: break; // No marker is permitted.
	case 5: replacement.formats.insert(HISTORY, std::nullopt); break;
	case 6: replacement.formats.insert(HISTORY, QByteArray()); break;
	case 7: replacement.formats.insert(HISTORY, QByteArray(1, '\xff')); break;
	}
	// The producer replaces an ordinary value at the actual open. A policy
	// precheck sees only permitted content and therefore cannot protect this.
	f.provider->replaceOnOpen = replacement;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1);
	QCOMPARE(f.provider->closes, 1);
	QVERIFY(!f.provider->opened);
	QCOMPARE(f.entries().size(), accepted ? size_t(1) : size_t(0));
	QCOMPARE(files(f.directory.path()).size(), accepted && kind != 0 ? 1 : 0);
	QCOMPARE(f.provider->contentReads, accepted ? 1 : 0);
	QCOMPARE(f.provider->outsideChecks, 0);
	if (accepted && kind == 1) {
		QFile file(f.entries().front().textPath);
		QVERIFY(file.open(QIODevice::ReadOnly));
		QCOMPARE(file.readAll(), QByteArray(32769, 'x'));
	}
	if (accepted && kind == 2) {
		QImage image(f.entries().front().imagePath);
		QCOMPARE(image.size(), QSize(48, 32));
		QCOMPARE(image.pixelColor(0, 0), QColor(0x66, 0x44, 0x22));
	}
	if (!accepted) {
		f.provider->current = content(kind, 102);
		f.update();
		QVERIFY(settle(*f.clipboard, f.pool));
		QCOMPARE(f.entries().size(), size_t(1));
		QCOMPARE(f.provider->closes, 2);
	}
}

void TestClipboardCapture::errorPaths_data() {
	QTest::addColumn<int>("fault");
	for (int fault = 0; fault < 8; ++fault) {
		QTest::newRow(qPrintable(QString::number(fault))) << fault;
	}
}

void TestClipboardCapture::errorPaths() {
	QFETCH(int, fault);
	Fixture f;
	if (fault < 3) f.provider->registrations[static_cast<size_t>(fault)] = 0;
	if (fault == 3) f.provider->current.formats[CF_UNICODETEXT] = std::nullopt;
	if (fault == 4) f.provider->current.formats[CF_UNICODETEXT] = QByteArray();
	if (fault == 5) f.provider->current.formats.clear();
	if (fault == 6 || fault == 7) {
		f.provider->current.formats.clear();
		f.provider->current.formats[CF_DIB] =
		    fault == 6 ? std::optional<QByteArray>() : QByteArray("bad DIB");
	}
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
	QCOMPARE(f.provider->opens, fault < 3 ? 0 : 1);
	QCOMPARE(f.provider->closes, fault < 3 ? 0 : 1);
	QCOMPARE(f.provider->outsideChecks, 0);
}

void TestClipboardCapture::boundedRetryAndNoReuse() {
	Fixture f(1);
	f.provider->failedOpens = 3;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 4);
	QCOMPARE(f.provider->pauses, 3);
	QCOMPARE(f.provider->closes, 1);
	QCOMPARE(f.entries().size(), size_t(1));
	f.clipboard->deleteEntry(f.entries().front().id);
	QVERIFY(files(f.directory.path()).empty());

	f.provider->current = content(2, 101);
	f.provider->failedOpens = 100;
	f.provider->opens = 0;
	f.provider->pauses = 0;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 10);
	QCOMPARE(f.provider->pauses, 9);
	QCOMPARE(f.provider->closes, 1);
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());

	f.provider->failedOpens = 0;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(1));
	QVERIFY(f.entries().front().isImage);
}

void TestClipboardCapture::clearDuringRetry() {
	Fixture f(1);
	f.provider->failedOpens = 10;
	f.provider->hold = FakeClipboard::Hold::Retry;
	f.update();
	QVERIFY(f.provider->reached.tryAcquire(1, BARRIER_MS));
	f.clipboard->wipe();
	f.provider->resume.release();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1);
	QCOMPARE(f.provider->closes, 0);
	QVERIFY(!f.provider->barrierTimedOut);
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
}

void TestClipboardCapture::clearLateCompletion() {
	QFETCH(int, kind);
	QFETCH(bool, queued);
	Fixture f(kind);
	if (!queued) f.provider->hold = FakeClipboard::Hold::Close;
	f.update();
	if (queued) {
		QVERIFY(f.pool.waitForDone(BARRIER_MS));
		QCOMPARE(files(f.directory.path()).size(), 1);
	} else {
		QVERIFY(f.provider->reached.tryAcquire(1, BARRIER_MS));
	}
	f.clipboard->wipe();
	QVERIFY(f.entries().empty());
	f.provider->resume.release();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(!f.provider->barrierTimedOut);
	QVERIFY(f.entries().empty());
	QVERIFY(f.clipboard->bindableEntries().value().empty());
	QVERIFY(files(f.directory.path()).empty());
	f.update(); // Notification already queued before clear.
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1);
}

void TestClipboardCapture::clearDropsQueuedAgain() {
	Fixture f(1);
	f.provider->hold = FakeClipboard::Hold::Close;
	f.update();
	QVERIFY(f.provider->reached.tryAcquire(1, BARRIER_MS));
	f.update();
	QVERIFY(ClipboardTestAccess::captureAgain(*f.clipboard));
	f.clipboard->wipe();
	QVERIFY(!ClipboardTestAccess::captureAgain(*f.clipboard));
	f.provider->resume.release();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1);
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
}

void TestClipboardCapture::queuedResultFromEarlierSnapshot_data() {
	QTest::addColumn<int>("kind");
	QTest::newRow("spilled-text") << 1;
	QTest::newRow("image") << 2;
}

void TestClipboardCapture::queuedResultFromEarlierSnapshot() {
	QFETCH(int, kind);
	Fixture f(kind);
	f.update();
	QVERIFY(f.pool.waitForDone(BARRIER_MS));
	QCOMPARE(files(f.directory.path()).size(), 1);
	// Clear observes a *different* sequence than the result already queued.
	// A sequence-equality fence alone cannot invalidate this old generation.
	f.provider->current = content(kind, 101);
	f.clipboard->wipe();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1);
	f.provider->current = content(kind, 102);
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(1));
}

void TestClipboardCapture::newChangeAfterClear() {
	Fixture f(2);
	f.provider->hold = FakeClipboard::Hold::Close;
	f.update();
	QVERIFY(f.provider->reached.tryAcquire(1, BARRIER_MS));
	f.update(); // Pre-clear queued work is discarded.
	f.clipboard->wipe();
	f.provider->current = content(1, 101);
	f.update(); // A genuinely new change is remembered even while old work runs.
	f.provider->resume.release();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(!f.provider->barrierTimedOut);
	QCOMPARE(f.provider->opens, 2);
	QCOMPARE(f.entries().size(), size_t(1));
	QVERIFY(!f.entries().front().isImage);
	QCOMPARE(f.entries().front().textLength, 32769);
	QCOMPARE(files(f.directory.path()).size(), 1);
	f.update();
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 2);
	QCOMPARE(f.entries().size(), size_t(1));
	f.clipboard->wipe();
	QVERIFY(files(f.directory.path()).empty());
}

void TestClipboardCapture::unknownAndRevertedSequence() {
	Fixture f(1);
	f.clipboard->wipe();
	f.provider->current.sequence = 0;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 0);
	f.provider->current.sequence = 101;
	f.provider->replaceOnOpen = content(1, 100); // Returns to the clear snapshot.
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
	f.provider->current.sequence = 101;
	f.provider->replaceOnOpen = content(1, 0); // Lost snapshot identity.
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
	f.provider->current.sequence = 102;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(1));
}

void TestClipboardCapture::repeatedClear() {
	Fixture f(1);
	f.update();
	QVERIFY(f.pool.waitForDone(BARRIER_MS));
	f.clipboard->wipe();
	f.clipboard->wipe();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
	f.provider->current = content(2, 101);
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(1));
	f.clipboard->wipe();
	f.clipboard->wipe();
	QVERIFY(f.entries().empty());
	QVERIFY(files(f.directory.path()).empty());
}

void TestClipboardCapture::clearWithUnknownIdentity() {
	Fixture f(1);
	f.provider->current.sequence = 0;
	f.clipboard->wipe();
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	f.provider->current.sequence = 100;
	f.update(); // First known identity is a baseline, not proof of a new copy.
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 0);
	QVERIFY(f.entries().empty());
	f.provider->current.sequence = 101;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1);
	QCOMPARE(f.entries().size(), size_t(1));
}

void TestClipboardCapture::destructionAndReload() {
	QFETCH(int, kind);
	QFETCH(bool, queued);
	Fixture old(kind);
	if (!queued) old.provider->hold = FakeClipboard::Hold::Close;
	old.update();
	if (queued) QVERIFY(old.pool.waitForDone(BARRIER_MS));
	else QVERIFY(old.provider->reached.tryAcquire(1, BARRIER_MS));
	old.clipboard.reset();

	Fixture replacement(kind);
	replacement.update();
	QVERIFY(settle(*replacement.clipboard, replacement.pool));
	const auto replacementFiles = files(replacement.directory.path());
	QCOMPARE(replacementFiles.size(), 1);
	old.provider->resume.release();
	QVERIFY(old.pool.waitForDone(BARRIER_MS));
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	QVERIFY(!old.provider->barrierTimedOut);
	QVERIFY(files(old.directory.path()).empty());
	QCOMPARE(files(replacement.directory.path()), replacementFiles);
	QCOMPARE(replacement.entries().size(), size_t(1));
}

void TestClipboardCapture::staleRemovesOnlyOwnFiles_data() {
	QTest::addColumn<int>("kind");
	QTest::addColumn<bool>("suppressed");
	for (int kind: {1, 2}) {
		for (bool suppressed: {false, true}) {
			QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(kind).arg(suppressed)))
			    << kind << suppressed;
		}
	}
}

void TestClipboardCapture::staleRemovesOnlyOwnFiles() {
	QFETCH(int, kind);
	QFETCH(bool, suppressed);
	Fixture f(kind);
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	const auto acceptedFiles = files(f.directory.path());
	QCOMPARE(acceptedFiles.size(), 1);
	QFile sentinel(f.directory.path() + QStringLiteral("/not-a-result"));
	QVERIFY(sentinel.open(QIODevice::WriteOnly));
	QCOMPARE(sentinel.write("sentinel"), 8);
	sentinel.close();
	f.provider->current.sequence = 101;
	f.update();
	QVERIFY(f.pool.waitForDone(BARRIER_MS));
	QCOMPARE(files(f.directory.path()).size(), 3);
	if (suppressed) ClipboardTestAccess::suppress(*f.clipboard, 101);
	else ClipboardTestAccess::ownWrite(*f.clipboard, 102);
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(1));
	QVERIFY(QFile::exists(f.directory.path() + "/" + acceptedFiles.front()));
	QVERIFY(QFile::exists(sentinel.fileName()));
	QCOMPARE(files(f.directory.path()).size(), 2);
	f.clipboard->wipe();
	QCOMPARE(files(f.directory.path()), QStringList {"not-a-result"});
}

void TestClipboardCapture::duplicateAndSuppression() {
	Fixture f(1);
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	const auto firstId = f.entries().front().id;
	const auto firstFiles = files(f.directory.path());
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 1); // Same sequence.
	f.provider->current.sequence = 101;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(1)); // Same spilled text, new sequence.
	QCOMPARE(files(f.directory.path()), firstFiles);
	ClipboardTestAccess::suppress(*f.clipboard, 102, firstId);
	f.provider->current.sequence = 102;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.provider->opens, 2); // Own-write sequence is not read.
	f.provider->current.sequence = 103;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().front().id, firstId); // Copy suppression retains identity.
	QCOMPARE(files(f.directory.path()), firstFiles);
	f.provider->current = content(0, 104);
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(2)); // A distinct permitted value still enters.
}

void TestClipboardCapture::imageBehavior() {
	Fixture f(2);
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	f.provider->current.sequence = 101;
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QCOMPARE(f.entries().size(), size_t(2)); // Existing image behavior, no text dedup.
	QCOMPARE(files(f.directory.path()).size(), 2);
	f.clipboard->deleteEntry(f.entries().front().id);
	QCOMPARE(f.entries().size(), size_t(1));
	QCOMPARE(files(f.directory.path()).size(), 1);
	f.clipboard->wipe();
	QVERIFY(files(f.directory.path()).empty());
}

void TestClipboardCapture::writeFailure_data() {
	QTest::addColumn<int>("kind");
	QTest::newRow("text-memory-fallback") << 1;
	QTest::newRow("image-rejected") << 2;
}

void TestClipboardCapture::writeFailure() {
	QFETCH(int, kind);
	Fixture f(kind);
	const auto occupied = f.directory.path() + (kind == 1 ? "/1.txt" : "/1.png");
	QVERIFY(QDir().mkpath(occupied)); // Deterministic open failure, no ACL changes.
	QFile sentinel(occupied + "/sentinel");
	QVERIFY(sentinel.open(QIODevice::WriteOnly));
	sentinel.close();
	f.update();
	QVERIFY(settle(*f.clipboard, f.pool));
	QVERIFY(files(f.directory.path()).empty());
	QVERIFY(QFile::exists(sentinel.fileName()));
	QCOMPARE(f.entries().size(), kind == 1 ? size_t(1) : size_t(0));
	if (kind == 1) {
		QVERIFY(f.entries().front().textPath.isEmpty());
		QCOMPARE(f.entries().front().text.size(), 32769);
	}
}

QTEST_GUILESS_MAIN(TestClipboardCapture);
#include "clipboard_capture_test.moc"
