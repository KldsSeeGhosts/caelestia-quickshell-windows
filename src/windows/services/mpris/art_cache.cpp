#include "art_cache.hpp"

#include <qbytearray.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qimage.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstring.h>
#include "../../redact.hpp"
#include <qurl.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Storage::Streams;

namespace qs::windows::services::mpris {

namespace {
Q_LOGGING_CATEGORY(logMprisArt, "quickshell.windows.mpris.art", QtWarningMsg);

constexpr qsizetype MAX_CACHED_FILES = 50;

void pruneCache(const QString& dir, const QString& retainedFileName) {
	QDir qdir(dir);
	auto entries = qdir.entryInfoList(QDir::Files, QDir::Time);
	qsizetype retained = 1;
	for (const auto& entry: entries) {
		if (entry.fileName() == retainedFileName) continue;
		if (retained < MAX_CACHED_FILES) ++retained;
		else QFile::remove(entry.absoluteFilePath());
	}
}
} // namespace

CachedArt cacheImageBytes(const QByteArray& bytes, const QString& cacheDirectory) {
	if (bytes.isEmpty() || cacheDirectory.isEmpty()) return {};
	QImage image;
	if (!image.loadFromData(bytes)) {
		qCWarning(logMprisArt) << "Could not decode a thumbnail of" << bytes.size() << "bytes";
		return {};
	}
	if (!QDir().mkpath(cacheDirectory)) {
		qCWarning(logMprisArt) << "Could not create artwork cache"
	                       << qs::windows::logPath(cacheDirectory);
		return {};
	}

	auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
	auto path = QDir(cacheDirectory).filePath(hash + ".png");
	if (QFileInfo::exists(path)) return {QUrl::fromLocalFile(path).toString(), hash};
	if (!image.save(path, "PNG")) {
		qCWarning(logMprisArt) << "Could not save thumbnail to" << qs::windows::logPath(path);
		return {};
	}
	pruneCache(cacheDirectory, QFileInfo(path).fileName());
	return {QUrl::fromLocalFile(path).toString(), hash};
}

CachedArt cacheThumbnail(
    const IRandomAccessStreamReference& thumbnail,
    const QString& cacheDirectory
) {
	if (thumbnail == nullptr) return {};

	try {
		auto stream = thumbnail.OpenReadAsync().get();
		auto size = static_cast<uint32_t>(stream.Size());
		if (size == 0) return {};

		DataReader reader(stream);
		reader.LoadAsync(size).get();

		QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
		reader.ReadBytes(winrt::array_view<uint8_t>(
		    reinterpret_cast<uint8_t*>(bytes.data()), // NOLINT
		    reinterpret_cast<uint8_t*>(bytes.data()) + bytes.size() // NOLINT
		));

		return cacheImageBytes(bytes, cacheDirectory);
	} catch (const winrt::hresult_error& e) {
		qCWarning(logMprisArt) << "Failed reading thumbnail:" << QString::fromWCharArray(e.message().c_str());
		return {};
	}
}

} // namespace qs::windows::services::mpris
