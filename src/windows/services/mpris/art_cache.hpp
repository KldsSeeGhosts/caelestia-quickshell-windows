#pragma once

#include <cstdint>

#include <qbytearray.h>
#include <qsize.h>
#include <qstring.h>

#include <winrt/Windows.Storage.Streams.h>

namespace qs::windows::services::mpris {

// Artwork is a thumbnail, not an arbitrary media payload. Bound encoded data
// to 16 MiB and decoded images to 4096 per axis and 16 million pixels
// (at most 64 MiB for a 32-bit image).
inline constexpr uint64_t MAX_ARTWORK_BYTES = 16 * 1024 * 1024;
inline constexpr int MAX_ARTWORK_DIMENSION = 4096;
inline constexpr uint64_t MAX_ARTWORK_PIXELS = 16 * 1024 * 1024;

constexpr bool artworkSizeAllowed(uint64_t size) {
	return size > 0 && size <= MAX_ARTWORK_BYTES;
}

inline bool artworkDimensionsAllowed(QSize size) {
	return size.width() > 0 && size.height() > 0
	    && size.width() <= MAX_ARTWORK_DIMENSION && size.height() <= MAX_ARTWORK_DIMENSION
	    && uint64_t(size.width()) * uint64_t(size.height()) <= MAX_ARTWORK_PIXELS;
}

struct CachedArt {
	QString url;
	QByteArray hash;
};

CachedArt cacheImageBytes(const QByteArray& bytes, const QString& cacheDirectory);

CachedArt
cacheThumbnail(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& thumbnail,
    const QString& cacheDirectory
);

} // namespace qs::windows::services::mpris
