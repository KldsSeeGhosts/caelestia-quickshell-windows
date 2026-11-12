#include "clipboard.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include <qbytearray.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qimage.h>
#include <qimagewriter.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include <qpointer.h>
#include <qrunnable.h>
#include <qscopeguard.h>
#include <qstandardpaths.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qthreadpool.h>
#include <qtimer.h>

#include "../../core/backgroundpool.hpp"
#include "../../core/instanceinfo.hpp"
#include "../../core/logcat.hpp"
#include "../redact.hpp"
#include "../services/message_window.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logClipboard, "quickshell.windows.clipboard", QtWarningMsg);

constexpr qsizetype MAX_ENTRIES = 200;
constexpr qsizetype PREVIEW_CHARS = 500;
constexpr qsizetype SPILL_CHARS = 32 * 1024;
constexpr int PNG_QUALITY = 85;
constexpr int READ_ATTEMPTS = 10;
constexpr int WRITE_ATTEMPTS = 5;
constexpr DWORD OPEN_RETRY_MS = 10;

QString cacheRoot() {
	return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
	     + QStringLiteral("/quickshell/clipboard");
}

bool processAlive(DWORD pid) {
	auto* process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (process == nullptr) return false;
	auto alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	CloseHandle(process);
	return alive;
}

void pruneStaleCaches() {
	auto root = QDir(cacheRoot());
	if (!root.exists()) return;

	for (const auto& entry: root.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
		auto ok = false;
		auto pid = entry.fileName().section('-', 0, 0).toULong(&ok);
		if (entry.isDir() && ok && processAlive(static_cast<DWORD>(pid))) continue;

		if (entry.isDir()) QDir(entry.absoluteFilePath()).removeRecursively();
		else QFile::remove(entry.absoluteFilePath());
	}
}

QString previewOf(const QString& text) {
	auto cut = text.size() > PREVIEW_CHARS;
	auto preview = cut ? text.first(PREVIEW_CHARS) : text;
	if (cut && preview.back().isHighSurrogate()) preview.chop(1);

	preview.replace(QStringLiteral("\r\n"), QStringLiteral(" ⏎ "));
	preview.replace(QLatin1Char('\n'), QStringLiteral(" ⏎ "));
	preview = preview.trimmed();

	if (cut) preview.append(QStringLiteral("…"));
	return preview;
}

QThreadPool* capturePool() {
	static auto* pool = []() {
		auto* pool = new QThreadPool(); // NOLINT
		pool->setMaxThreadCount(1);
		return pool;
	}();

	return pool;
}

void* globalFromImage(const QImage& imageIn) {
	auto image = imageIn.convertToFormat(QImage::Format_RGB32);
	if (image.isNull()) return nullptr;

	BITMAPINFOHEADER bih {};
	bih.biSize = sizeof(BITMAPINFOHEADER);
	bih.biWidth = image.width();
	bih.biHeight = image.height();
	bih.biPlanes = 1;
	bih.biBitCount = 32;
	bih.biCompression = BI_RGB;

	auto pixelBytes = static_cast<SIZE_T>(image.width()) * image.height() * 4;
	auto* mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
	if (mem == nullptr) return nullptr;

	auto* dst = static_cast<unsigned char*>(GlobalLock(mem));
	if (dst == nullptr) {
		GlobalFree(mem);
		return nullptr;
	}

	memcpy(dst, &bih, sizeof(bih)); // NOLINT
	for (int y = 0; y < image.height(); y++) {
		memcpy( // NOLINT
		    dst + sizeof(bih) + static_cast<SIZE_T>(y) * image.width() * 4,
		    image.constScanLine(image.height() - 1 - y),
		    static_cast<SIZE_T>(image.width()) * 4
		);
	}
	GlobalUnlock(mem);
	return mem;
}

void* globalFromText(const QString& text) {
	auto bytes = (text.size() + 1) * sizeof(wchar_t);
	auto* mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (mem == nullptr) return nullptr;

	auto* dst = GlobalLock(mem);
	if (dst == nullptr) {
		GlobalFree(mem);
		return nullptr;
	}

	memcpy(dst, text.utf16(), bytes); // NOLINT
	GlobalUnlock(mem);
	return mem;
}

class NativeClipboardWriteProvider final: public ClipboardWriteProvider {
public:
	void* imageData(const QImage& image) override { return globalFromImage(image); }
	void* textData(const QString& text) override { return globalFromText(text); }
	void freeData(void* data) override { GlobalFree(data); }
	bool open() override {
		return OpenClipboard(qs::windows::services::ServiceMessageWindow::instance()->hwnd()) != FALSE;
	}
	bool empty() override { return EmptyClipboard() != FALSE; }
	bool set(quint32 format, void* data) override {
		return SetClipboardData(format, data) != nullptr;
	}
	void close() override { CloseClipboard(); }
	quint32 sequence() override { return GetClipboardSequenceNumber(); }
};

class NativeClipboardListenerProvider final: public ClipboardListenerProvider {
public:
	bool start(QObject* context, std::function<void()> update) override {
		auto* window = qs::windows::services::ServiceMessageWindow::instance();
		if (this->users == 0 && !AddClipboardFormatListener(window->hwnd())) {
			qCWarning(logClipboard) << "AddClipboardFormatListener failed:" << GetLastError();
			return false;
		}
		++this->users;
		window->addHandler(WM_CLIPBOARDUPDATE, context, [update = std::move(update)](WPARAM, LPARAM) {
			update();
		});
		return true;
	}
	void stop() override {
		if (--this->users == 0) {
			auto* window = qs::windows::services::ServiceMessageWindow::instance();
			RemoveClipboardFormatListener(window->hwnd());
		}
	}

private:
	int users = 0;
};

std::shared_ptr<ClipboardListenerProvider> nativeListener() {
	static auto provider = std::make_shared<NativeClipboardListenerProvider>();
	return provider;
}

QString readTextFile(const QString& path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	return QString::fromUtf8(file.readAll());
}

void removeEntryFiles(const ClipboardEntry& entry) {
	if (entry.isImage && !entry.imagePath.isEmpty()) QFile::remove(entry.imagePath);
	if (!entry.textPath.isEmpty()) QFile::remove(entry.textPath);
}

bool sameText(const ClipboardEntry& a, const ClipboardEntry& b) {
	if (a.isImage || b.isImage) return false;
	if (a.textLength != b.textLength || a.textHash != b.textHash) return false;
	if (a.textPath.isEmpty() && b.textPath.isEmpty()) return a.text == b.text;
	return true;
}

class NativeClipboardReadProvider final: public ClipboardReadProvider {
public:
	std::array<quint32, 3> privacyFormats() override {
		static const std::array<quint32, 3> formats {
		    RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"),
		    RegisterClipboardFormatW(L"Clipboard Viewer Ignore"),
		    RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"),
		};
		return formats;
	}

	bool open() override { return OpenClipboard(nullptr) != FALSE; }
	void close() override { CloseClipboard(); }
	void retryPause() override { Sleep(OPEN_RETRY_MS); }
	quint32 sequence() override { return GetClipboardSequenceNumber(); }
	bool available(quint32 format) override { return IsClipboardFormatAvailable(format) != FALSE; }

	std::optional<QByteArray> data(quint32 format) override {
		auto* handle = GetClipboardData(format);
		if (handle == nullptr) return std::nullopt;
		auto size = GlobalSize(handle);
		if (size == 0 || size > static_cast<SIZE_T>(std::numeric_limits<qsizetype>::max())) {
			return std::nullopt;
		}
		auto* locked = GlobalLock(handle);
		if (locked == nullptr) return std::nullopt;
		const auto unlock = qScopeGuard([handle]() { GlobalUnlock(handle); });
		return QByteArray(static_cast<const char*>(locked), static_cast<qsizetype>(size));
	}
};

QString textFromBytes(const QByteArray& bytes) {
	auto capacity = bytes.size() / static_cast<qsizetype>(sizeof(char16_t));
	if (capacity == 0) return {};
	auto text = QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData()), capacity);
	auto terminator = text.indexOf(QChar(u'\0'));
	if (terminator >= 0) text.truncate(terminator);
	return text;
}

QImage imageFromDib(const QByteArray& dib) {
	if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER))) return {};

	const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.constData()); // NOLINT
	auto width = header->biWidth;
	auto height = std::abs(header->biHeight);

	if (header->biSize < sizeof(BITMAPINFOHEADER) || width <= 0 || height <= 0 || width > 16384
	    || height > 16384)
	{
		return {};
	}

	auto colors = static_cast<size_t>(header->biClrUsed);
	if (colors == 0 && header->biBitCount > 0 && header->biBitCount <= 8) {
		colors = static_cast<size_t>(1) << header->biBitCount;
	}

	if (header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == BI_BITFIELDS) {
		colors += 3;
	}

	auto offset = header->biSize + colors * sizeof(RGBQUAD);
	auto stride = (static_cast<size_t>(width) * header->biBitCount + 31) / 32 * 4;
	auto uncompressed = header->biCompression == BI_RGB || header->biCompression == BI_BITFIELDS;
	auto bitsSize = uncompressed ? stride * height : static_cast<size_t>(header->biSizeImage);

	if (bitsSize == 0 || offset + bitsSize > static_cast<size_t>(dib.size())) return {};

	auto* screenDc = GetDC(nullptr);

	BITMAPINFO outInfo {};
	outInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	outInfo.bmiHeader.biWidth = width;
	outInfo.bmiHeader.biHeight = -height;
	outInfo.bmiHeader.biPlanes = 1;
	outInfo.bmiHeader.biBitCount = 32;
	outInfo.bmiHeader.biCompression = BI_RGB;

	void* outBits = nullptr;
	auto* outDib = CreateDIBSection(screenDc, &outInfo, DIB_RGB_COLORS, &outBits, nullptr, 0);
	auto* memDc = CreateCompatibleDC(screenDc);
	auto* oldBmp = SelectObject(memDc, outDib);

	auto blitted = false;
	if (outDib != nullptr && outBits != nullptr) {
		blitted = StretchDIBits(
		              memDc,
		              0,
		              0,
		              width,
		              height,
		              0,
		              0,
		              width,
		              height,
		              dib.constData() + offset,
		              reinterpret_cast<const BITMAPINFO*>(header), // NOLINT
		              DIB_RGB_COLORS,
		              SRCCOPY
		          )
		       != static_cast<int>(GDI_ERROR);
	}

	QImage image;
	if (blitted) {
		QImage view(static_cast<uchar*>(outBits), width, height, width * 4, QImage::Format_RGB32);
		image = view.copy();
	}

	SelectObject(memDc, oldBmp);
	DeleteDC(memDc);
	if (outDib != nullptr) DeleteObject(outDib);
	ReleaseDC(nullptr, screenDc);

	return image;
}

ClipboardCapture readClipboard(
    ClipboardReadProvider& provider,
    const std::shared_ptr<ClipboardCaptureGeneration>& generation,
    qint64 id,
    const QString& dir
) {
	ClipboardCapture capture;
	capture.generation = generation;
	capture.entry.id = id;

	auto formats = provider.privacyFormats();
	// A missing privacy format registration must not silently disable a check.
	if (std::ranges::find(formats, 0) != formats.end()) return capture;

	bool opened = false;
	for (int attempt = 1; attempt <= READ_ATTEMPTS; ++attempt) {
		if (generation->cancelled) return capture;
		if (provider.open()) {
			opened = true;
			break;
		}
		if (attempt < READ_ATTEMPTS) provider.retryPause();
	}
	if (!opened) return capture;

	QByteArray bytes;
	bool isText = false;
	{
		const auto close = qScopeGuard([&provider]() { provider.close(); });
		capture.sequence = provider.sequence();
		if (generation->cancelled) return capture;

		// These decisions and content reads belong to this same open snapshot.
		if (provider.available(formats[0]) || provider.available(formats[1])) return capture;
		if (provider.available(formats[2])) {
			auto marker = provider.data(formats[2]);
			// Preserve the supported first-byte semantics, but fail closed when
			// an advertised marker cannot be read, locked or has no bytes.
			if (!marker || marker->isEmpty() || marker->front() == 0) return capture;
		}

		isText = provider.available(CF_UNICODETEXT);
		if (!isText && !provider.available(CF_DIB)) return capture;
		auto content = provider.data(isText ? CF_UNICODETEXT : CF_DIB);
		if (!content) return capture;
		bytes = std::move(*content);
	}

	if (generation->cancelled) return capture;

	if (isText) {
		capture.entry.text = textFromBytes(bytes);
		capture.captured = !capture.entry.text.isEmpty();
		capture.entry.textLength = capture.entry.text.size();
		capture.entry.textHash = qHash(capture.entry.text);

		if (capture.entry.text.size() > SPILL_CHARS) {
			auto path = QStringLiteral("%1/%2.txt").arg(dir).arg(id);
			QFile file(path);
			auto utf8 = capture.entry.text.toUtf8();
			if (file.open(QIODevice::WriteOnly) && file.write(utf8) == utf8.size() && file.flush()) {
				file.close();
				capture.entry.preview = previewOf(capture.entry.text);
				capture.entry.textPath = path;
				capture.entry.text.clear();
			} else {
				file.close();
				QFile::remove(path);
			}
		}
		if (generation->cancelled) {
			removeEntryFiles(capture.entry);
			capture.captured = false;
		}
		return capture;
	}

	if (bytes.isEmpty()) return capture;

	auto image = imageFromDib(bytes);
	if (image.isNull()) {
		qCWarning(logClipboard) << "Failed to convert clipboard DIB to an image";
		return capture;
	}

	auto path = dir + QStringLiteral("/%1.png").arg(id);
	QImageWriter writer(path, "png");
	writer.setQuality(PNG_QUALITY);

	if (!writer.write(image)) {
		qCWarning(logClipboard) << "Failed to save clipboard image to" << qs::windows::logPath(path)
		                        << writer.errorString();
		QFile::remove(path);
		return capture;
	}

	capture.entry.isImage = true;
	capture.entry.imagePath = path;
	capture.entry.width = image.width();
	capture.entry.height = image.height();
	capture.captured = true;
	if (generation->cancelled) {
		removeEntryFiles(capture.entry);
		capture.captured = false;
	}
	return capture;
}

} // namespace

// Every queued retry owns its data through RAII, including when QObject
// destruction removes a pending timer or a queued image completion.
class ClipboardWriteController: public QObject {
public:
	using Done = std::function<void(bool, const QString&)>;

	ClipboardWriteController(std::shared_ptr<ClipboardWriteProvider> provider, QThreadPool* pool)
	    : provider(std::move(provider))
	    , pool(pool) {}

	bool copyImageFile(const QString& path, Done done) {
		QFile file(path);
		auto readable = file.open(QIODevice::ReadOnly);
		auto bytes = readable ? file.readAll() : QByteArray();
		readable = readable && file.error() == QFileDevice::NoError;
		if (!readable) {
			QTimer::singleShot(0, this, [done = std::move(done)]() {
				done(false, QStringLiteral("read-failed"));
			});
			return false;
		}

		this->pool->start(QRunnable::create(
		    [self = QPointer(this), bytes = std::move(bytes), done = std::move(done)]() {
			    auto image = QImage::fromData(bytes);
			    QMetaObject::invokeMethod(
			        QCoreApplication::instance(),
			        [self, image = std::move(image), done]() {
				        if (!self) return;
				        if (image.isNull()) {
					        done(false, QStringLiteral("decode-failed"));
					        return;
				        }
				        self->writeData(CF_DIB, self->provider->imageData(image), done);
			        },
			        Qt::QueuedConnection
			    );
		    }
		));
		return true;
	}

	void copyText(const QString& text, Done done) {
		QTimer::singleShot(0, this, [this, text, done = std::move(done)]() {
			this->writeData(CF_UNICODETEXT, this->provider->textData(text), done);
		});
	}

	quint32 sequence() const { return this->provider->sequence(); }

private:
	struct Data {
		std::shared_ptr<ClipboardWriteProvider> provider;
		void* handle;
		~Data() {
			if (this->handle) this->provider->freeData(this->handle);
		}
	};

	void writeData(quint32 format, void* handle, Done done) {
		if (!handle) {
			done(false, QStringLiteral("allocation-failed"));
			return;
		}
		auto data = std::make_shared<Data>(this->provider, handle);
		this->writeData(format, std::move(data), std::move(done), 1);
	}

	void writeData(quint32 format, std::shared_ptr<Data> data, Done done, int attempt) {
		if (!this->provider->open()) {
			if (attempt >= WRITE_ATTEMPTS) {
				done(false, QStringLiteral("clipboard-busy"));
				return;
			}
			QTimer::singleShot(OPEN_RETRY_MS, this, [this, format, data, done, attempt]() {
				this->writeData(format, data, done, attempt + 1);
			});
			return;
		}

		auto ok = this->provider->empty() && this->provider->set(format, data->handle);
		if (ok) data->handle = nullptr; // Windows now owns the HGLOBAL.
		this->provider->close();
		done(ok, ok ? QString() : QStringLiteral("set-failed"));
	}

	std::shared_ptr<ClipboardWriteProvider> provider;
	QThreadPool* pool;
};

ClipboardWriter::ClipboardWriter(QObject* parent)
    : QObject(parent)
    , writer(std::make_unique<ClipboardWriteController>(
          std::make_shared<NativeClipboardWriteProvider>(),
          capturePool()
      )) {}

ClipboardWriter::ClipboardWriter(
    std::shared_ptr<ClipboardWriteProvider> provider,
    QThreadPool* pool
)
    : writer(std::make_unique<ClipboardWriteController>(std::move(provider), pool)) {}

ClipboardWriter::~ClipboardWriter() = default;

int ClipboardWriter::nextOperationId() {
	// Never wrap and reuse an ID during an object's lifetime.
	if (this->lastId == std::numeric_limits<int>::max()) qFatal("Clipboard operation IDs exhausted");
	return ++this->lastId;
}

int ClipboardWriter::copyImageFile(const QString& path) {
	auto id = this->nextOperationId();
	this->writer->copyImageFile(path, [this, id](bool ok, const QString& error) {
		emit this->writeFinished(id, ok, error);
	});
	return id;
}

int ClipboardWriter::copyText(const QString& text) {
	auto id = this->nextOperationId();
	this->writer->copyText(text, [this, id](bool ok, const QString& error) {
		emit this->writeFinished(id, ok, error);
	});
	return id;
}

Clipboard::Clipboard(QObject* parent)
    : QObject(parent)
    , readProvider(std::make_shared<NativeClipboardReadProvider>())
    , readPool(capturePool())
    , listener(nativeListener())
    , writer(std::make_unique<ClipboardWriteController>(
          std::make_shared<NativeClipboardWriteProvider>(),
          capturePool()
      )) {}

void Clipboard::setHistoryEnabled(bool enabled) {
	if (this->mHistoryEnabled == enabled) return;
	if (!enabled) {
		this->mHistoryEnabled = false;
		this->listenerContext.reset();
		this->listener->stop();
		this->wipe();
		if (!this->mCacheDir.isEmpty()) QDir(this->mCacheDir).removeRecursively();
		emit this->historyEnabledChanged();
		return;
	}

	if (this->production) {
		auto entry = QFileInfo(InstanceInfo::CURRENT.configPath).fileName();
		if (!entry.isEmpty() && entry.compare("shell.qml", Qt::CaseInsensitive) != 0) {
			qCInfo(logClipboard) << "Not keeping clipboard history in" << qs::windows::logId(entry)
			                     << "(only a config's shell.qml does)";
			return;
		}

		static int instances = 0; // NOLINT
		auto isFirstInstance = instances == 0;
		if (this->mCacheDir.isEmpty()) {
			this->mCacheDir =
			    cacheRoot() + QStringLiteral("/%1-%2").arg(GetCurrentProcessId()).arg(instances++);
		}
		if (isFirstInstance) {
			BackgroundThreadPool::instance()->start(QRunnable::create(&pruneStaleCaches));
		}
	}

	auto context = std::make_unique<QObject>();
	if (!this->listener->start(context.get(), [self = QPointer(this)]() {
		    if (self) self->onClipboardUpdate();
	    }))
		return;
	this->listenerContext = std::move(context);
	this->captureGeneration = std::make_shared<ClipboardCaptureGeneration>();
	this->cleared = false;
	this->capturedSequence = 0;
	this->writtenSequence = 0;
	this->suppressedSequence = 0;
	this->mHistoryEnabled = true;
	emit this->historyEnabledChanged();
}

Clipboard::Clipboard(
    std::shared_ptr<ClipboardReadProvider> provider,
    std::shared_ptr<ClipboardListenerProvider> listener,
    std::shared_ptr<ClipboardWriteProvider> writeProvider,
    const QString& directory,
    QThreadPool* pool
)
    : mCacheDir(directory)
    , readProvider(std::move(provider))
    , readPool(pool)
    , production(false)
    , listener(std::move(listener))
    , writer(std::make_unique<ClipboardWriteController>(std::move(writeProvider), pool)) {}

Clipboard::~Clipboard() {
	if (this->captureGeneration) this->captureGeneration->cancelled = true;
	if (this->mHistoryEnabled) {
		this->listenerContext.reset();
		this->listener->stop();
	}
	if (!this->mCacheDir.isEmpty()) QDir(this->mCacheDir).removeRecursively();
}

QString Clipboard::cacheDir() {
	QDir().mkpath(this->mCacheDir);
	return this->mCacheDir;
}

void Clipboard::onClipboardUpdate() {
	if (!this->mHistoryEnabled) return;
	if (this->capturing) {
		this->captureAgain = true;
		return;
	}

	this->startCapture();
}

void Clipboard::startCapture() {
	if (!this->mHistoryEnabled) return;
	this->captureAgain = false;

	auto sequence = this->readProvider->sequence();
	// An update already queued at clear is not a new copy. If Windows cannot
	// identify the current sequence, fail closed until an identifiable change.
	if (this->cleared && (sequence == 0 || sequence == this->clearSequence)) return;
	if (this->cleared && this->clearSequence == 0) {
		// Clear itself had no trustworthy identity. Establish a baseline first;
		// learning an identity is not evidence of a new post-clear copy.
		this->clearSequence = sequence;
		this->capturedSequence = sequence;
		return;
	}
	if (sequence != 0 && sequence == this->capturedSequence) return;
	if (sequence != 0 && sequence == this->suppressedSequence) {
		this->capturedSequence = sequence;
		return;
	}

	this->capturing = true;
	auto id = this->nextId++;

	auto* task = QRunnable::create([self = QPointer(this),
	                                provider = this->readProvider,
	                                generation = this->captureGeneration,
	                                id,
	                                dir = this->cacheDir()]() {
		auto capture = readClipboard(*provider, generation, id, dir);

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [self, capture = std::move(capture)]() {
			    if (self) self->finishCapture(capture);
			    else removeEntryFiles(capture.entry);
		    },
		    Qt::QueuedConnection
		);
	});

	this->readPool->start(task);
}

void Clipboard::finishCapture(const ClipboardCapture& capture) {
	this->capturing = false;

	auto stale = !this->mHistoryEnabled || capture.generation != this->captureGeneration
	          || capture.generation->cancelled
	          || (this->cleared && (capture.sequence == 0 || capture.sequence == this->clearSequence))
	          || capture.sequence < this->writtenSequence
	          || (capture.sequence != 0 && capture.sequence == this->suppressedSequence);
	// In particular, a pre-clear completion must not overwrite the clear
	// sequence fence and make an old queued notification eligible again.
	if (!stale && capture.sequence != 0) this->capturedSequence = capture.sequence;

	if (stale || !capture.captured) {
		removeEntryFiles(capture.entry);
	} else if (capture.entry.isImage) {
		this->captureImage(capture.entry);
	} else {
		this->captureText(capture.entry);
	}

	// A worker may have held a spill file open when disabling removed the
	// directory. Retry directory removal after that worker has closed it.
	if (!this->mHistoryEnabled && !this->mCacheDir.isEmpty()) {
		QDir(this->mCacheDir).removeRecursively();
	}
	if (this->captureAgain) this->startCapture();
}

void Clipboard::captureText(ClipboardEntry entry) {
	if (!this->storage.empty() && this->suppressNextCaptureFor == this->storage.front().id
	    && sameText(this->storage.front(), entry))
	{
		this->suppressNextCaptureFor = -1;
		removeEntryFiles(entry);
		return;
	}

	if (!this->storage.empty() && sameText(this->storage.front(), entry)) {
		removeEntryFiles(entry);
		return;
	}

	if (entry.preview.isEmpty()) entry.preview = previewOf(entry.text);

	this->storage.insert(this->storage.begin(), std::move(entry));
	this->trimHistory();
	this->rebuildEntriesProperty();
}

void Clipboard::captureImage(const ClipboardEntry& entry) {
	this->storage.insert(this->storage.begin(), entry);
	this->trimHistory();
	this->rebuildEntriesProperty();
}

void Clipboard::noteOwnWrite() {
	if (this->mHistoryEnabled) this->writtenSequence = this->writer->sequence();
}

void Clipboard::trimHistory() {
	while (static_cast<qsizetype>(this->storage.size()) > MAX_ENTRIES) {
		removeEntryFiles(this->storage.back());
		this->storage.pop_back();
	}
}

void Clipboard::rebuildEntriesProperty() {
	QStringList lines;
	lines.reserve(static_cast<qsizetype>(this->storage.size()));

	for (const auto& entry: this->storage) {
		if (entry.isImage) {
			lines.append(QStringLiteral("%1\t[[ binary data %2x%3 ]]")
			                 .arg(entry.id)
			                 .arg(entry.width)
			                 .arg(entry.height));
		} else {
			lines.append(QStringLiteral("%1\t%2").arg(entry.id).arg(entry.preview));
		}
	}

	this->bEntries = lines;
}

void Clipboard::copy(qint64 id) {
	auto iter = std::find_if(this->storage.begin(), this->storage.end(), [id](const auto& e) {
		return e.id == id;
	});
	if (iter == this->storage.end()) return;

	auto entry = *iter;
	this->storage.erase(iter);
	this->storage.insert(this->storage.begin(), entry);
	this->rebuildEntriesProperty();

	auto done = [this, id](bool ok, const QString&) {
		if (!ok) return;
		this->noteOwnWrite();
		this->suppressedSequence = this->writtenSequence;
		this->suppressNextCaptureFor = id;
	};
	if (entry.isImage) {
		this->writer->copyImageFile(entry.imagePath, done);
	} else {
		auto text = entry.textPath.isEmpty() ? entry.text : readTextFile(entry.textPath);
		if (!text.isEmpty()) this->writer->copyText(text, done);
	}
}

bool Clipboard::copyImageFile(const QString& path) {
	return this->writer->copyImageFile(path, [this](bool ok, const QString&) {
		if (ok) this->noteOwnWrite();
	});
}

void Clipboard::copyText(const QString& text) {
	this->writer->copyText(text, [this](bool ok, const QString&) {
		if (ok) this->noteOwnWrite();
	});
}

void Clipboard::deleteEntry(qint64 id) {
	auto iter = std::find_if(this->storage.begin(), this->storage.end(), [id](const auto& e) {
		return e.id == id;
	});
	if (iter == this->storage.end()) return;

	removeEntryFiles(*iter);
	this->storage.erase(iter);
	this->rebuildEntriesProperty();
}

void Clipboard::wipe() {
	if (this->captureGeneration) this->captureGeneration->cancelled = true;
	this->captureGeneration =
	    this->mHistoryEnabled ? std::make_shared<ClipboardCaptureGeneration>() : nullptr;
	this->captureAgain = false;
	this->cleared = true;
	this->clearSequence = this->mHistoryEnabled ? this->readProvider->sequence() : 0;
	this->capturedSequence = this->clearSequence;
	this->suppressNextCaptureFor = -1;
	for (const auto& entry: this->storage) removeEntryFiles(entry);
	this->storage.clear();
	this->rebuildEntriesProperty();
}

QString Clipboard::imagePath(qint64 id) const {
	auto iter = std::find_if(this->storage.begin(), this->storage.end(), [id](const auto& e) {
		return e.id == id;
	});
	if (iter == this->storage.end() || !iter->isImage) return QString();
	return iter->imagePath;
}

} // namespace qs::windows::sys
