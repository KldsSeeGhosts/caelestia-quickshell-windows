#include "clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include <qt_windows.h>

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
		QImage view(
		    static_cast<uchar*>(outBits),
		    width,
		    height,
		    width * 4,
		    QImage::Format_RGB32
		);
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
		qCWarning(logClipboard) << "Failed to save clipboard image to"
	                        << qs::windows::logPath(path) << writer.errorString();
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

Clipboard::Clipboard(QObject* parent)
    : QObject(parent)
    , readProvider(std::make_shared<NativeClipboardReadProvider>())
    , readPool(capturePool()) {
	static int instances = 0; // NOLINT
	auto isFirstInstance = instances == 0;
	this->mCacheDir = cacheRoot() + QStringLiteral("/%1-%2").arg(GetCurrentProcessId()).arg(instances++);

	auto entry = QFileInfo(InstanceInfo::CURRENT.configPath).fileName();
	if (!entry.isEmpty() && entry.compare("shell.qml", Qt::CaseInsensitive) != 0) {
		qCInfo(logClipboard) << "Not keeping clipboard history in"
		                     << qs::windows::logId(entry)
		                     << "(only a config's shell.qml does)";
		return;
	}

	if (isFirstInstance) {
		BackgroundThreadPool::instance()->start(QRunnable::create(&pruneStaleCaches));
	}

	auto* window = qs::windows::services::ServiceMessageWindow::instance();

	static auto listening = false; // NOLINT
	if (!listening) {
		listening = AddClipboardFormatListener(window->hwnd()) != FALSE;
		if (!listening) qCWarning(logClipboard) << "AddClipboardFormatListener failed:" << GetLastError();
	}

	window->addHandler(WM_CLIPBOARDUPDATE, this, [self = QPointer(this)](WPARAM /*w*/, LPARAM /*l*/) {
		if (self) self->onClipboardUpdate();
	});
}

Clipboard::Clipboard(
    std::shared_ptr<ClipboardReadProvider> provider,
    const QString& directory,
    QThreadPool* pool
)
    : mCacheDir(directory)
    , readProvider(std::move(provider))
    , readPool(pool) {}

Clipboard::~Clipboard() {
	this->captureGeneration->cancelled = true;
	QDir(this->mCacheDir).removeRecursively();
}

QString Clipboard::cacheDir() {
	QDir().mkpath(this->mCacheDir);
	return this->mCacheDir;
}

void Clipboard::onClipboardUpdate() {
	if (this->capturing) {
		this->captureAgain = true;
		return;
	}

	this->startCapture();
}

void Clipboard::startCapture() {
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

	auto stale = capture.generation != this->captureGeneration
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

void Clipboard::noteOwnWrite() { this->writtenSequence = GetClipboardSequenceNumber(); }

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
			lines.append(
			    QStringLiteral("%1\t[[ binary data %2x%3 ]]")
			        .arg(entry.id)
			        .arg(entry.width)
			        .arg(entry.height)
			);
		} else {
			lines.append(QStringLiteral("%1\t%2").arg(entry.id).arg(entry.preview));
		}
	}

	this->bEntries = lines;
}

void Clipboard::writeData(
    quint32 format,
    void* data,
    std::function<void(Clipboard*)> done,
    int attempt
) {
	if (data == nullptr) return;

	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	if (!OpenClipboard(window->hwnd())) {
		if (attempt >= WRITE_ATTEMPTS) {
			qCWarning(logClipboard) << "Couldn't open the clipboard to write";
			GlobalFree(data);
			return;
		}

		QTimer::singleShot(OPEN_RETRY_MS, this, [this, format, data, done = std::move(done), attempt]() mutable {
			this->writeData(format, data, std::move(done), attempt + 1);
		});
		return;
	}

	EmptyClipboard();
	if (SetClipboardData(format, data) == nullptr) GlobalFree(data);
	CloseClipboard();
	this->noteOwnWrite();
	if (done) done(this);
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

	auto* task = QRunnable::create([self = QPointer(this), entry, id]() {
		void* data = nullptr;
		quint32 format = 0;

		if (entry.isImage) {
			QImage image(entry.imagePath);
			if (!image.isNull()) {
				data = globalFromImage(image);
				format = CF_DIB;
			}
		} else {
			auto text = entry.textPath.isEmpty() ? entry.text : readTextFile(entry.textPath);
			if (!text.isEmpty()) {
				data = globalFromText(text);
				format = CF_UNICODETEXT;
			}
		}

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [self, format, data, id]() {
			    if (!self) {
				    if (data != nullptr) GlobalFree(data);
				    return;
			    }

			    self->writeData(format, data, [id](Clipboard* clipboard) {
				    clipboard->suppressedSequence = clipboard->writtenSequence;
				    clipboard->suppressNextCaptureFor = id;
			    });
		    },
		    Qt::QueuedConnection
		);
	});

	capturePool()->start(task);
}

bool Clipboard::copyImageFile(const QString& path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		qCWarning(logClipboard) << "Cannot read image" << qs::windows::logPath(path);
		return false;
	}

	auto bytes = file.readAll();
	auto* task = QRunnable::create([self = QPointer(this), bytes = std::move(bytes), path]() {
		auto image = QImage::fromData(bytes);
		void* data = image.isNull() ? nullptr : globalFromImage(image);
		if (image.isNull()) qCWarning(logClipboard) << "Cannot load image"
	                                            << qs::windows::logPath(path);

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [self, data]() {
			    if (!self) {
				    if (data != nullptr) GlobalFree(data);
				    return;
			    }

			    self->writeData(CF_DIB, data, nullptr);
		    },
		    Qt::QueuedConnection
		);
	});

	capturePool()->start(task);
	return true;
}

void Clipboard::copyText(const QString& text) {
	this->writeData(CF_UNICODETEXT, globalFromText(text), nullptr);
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
	this->captureGeneration->cancelled = true;
	this->captureGeneration = std::make_shared<ClipboardCaptureGeneration>();
	this->captureAgain = false;
	this->cleared = true;
	this->clearSequence = this->readProvider->sequence();
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
