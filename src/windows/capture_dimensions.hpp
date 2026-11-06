#pragma once

#include <cstring>
#include <limits>
#include <utility>

#include <d3d11.h>
#include <qimage.h>
#include <qscopeguard.h>

#include "capture_format.hpp"

namespace qs::windows::capture {

enum class OneShotStatus {
	Ready,
	InvalidSize,
	SizeOverflow,
	ResizeRequired,
	InvalidSurface,
	InvalidReadback,
	AllocationFailed,
};

inline const char* oneShotStatusName(OneShotStatus status) {
	switch (status) {
	case OneShotStatus::Ready: return "ready";
	case OneShotStatus::InvalidSize: return "invalid-size";
	case OneShotStatus::SizeOverflow: return "size-overflow";
	case OneShotStatus::ResizeRequired: return "resized-retry-required";
	case OneShotStatus::InvalidSurface: return "invalid-source-surface";
	case OneShotStatus::InvalidReadback: return "invalid-readback-stride";
	case OneShotStatus::AllocationFailed: return "image-allocation-failed";
	}
	return "invalid-readback";
}

// Check both QImage's signed byte range and D3D's row/dimension limits before
// allocating anything. A UINT cast of a transient negative WGC size is unsafe.
inline OneShotStatus
oneShotLayout(const winrt::Windows::Graphics::SizeInt32& size, size_t& rowBytes) {
	if (size.Width <= 0 || size.Height <= 0) return OneShotStatus::InvalidSize;

	const auto width = static_cast<size_t>(size.Width);
	const auto height = static_cast<size_t>(size.Height);
	constexpr auto maxBytes = static_cast<size_t>(std::numeric_limits<qsizetype>::max());
	if (width > maxBytes / 4) return OneShotStatus::SizeOverflow;
	const auto row = width * 4;
	if (row > std::numeric_limits<UINT>::max() || height > maxBytes / row) {
		return OneShotStatus::SizeOverflow;
	}
	if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
	{
		return OneShotStatus::InvalidSize;
	}

	rowBytes = row;
	return OneShotStatus::Ready;
}

struct OneShotReadback {
	QImage image;
	OneShotStatus status;
};

// The production adapter obtains description() from the actual source texture,
// not ContentSize or the pool size. Fixtures inject that same description and
// observe copy/map/unmap without a device, frame pool, or captured content.
//
// Fail closed on *any* resize. A clipped old pool surface must not masquerade
// as a complete current monitor image. The caller completes the one-shot with
// failure and can start a fresh request; there is no unbounded recreate loop.
template <typename Readback>
OneShotReadback readOneShotFrame(
    const winrt::Windows::Graphics::SizeInt32& poolSize,
    const winrt::Windows::Graphics::SizeInt32& content,
    const FrameFormat& format,
    Readback& readback
) {
	size_t rowBytes = 0;
	auto status = oneShotLayout(poolSize, rowBytes);
	if (status != OneShotStatus::Ready) return {{}, status};
	status = oneShotLayout(content, rowBytes);
	if (status != OneShotStatus::Ready) return {{}, status};

	const auto source = readback.description();
	if (source.Width == 0 || source.Height == 0 || source.MipLevels != 1 || source.ArraySize != 1
	    || source.SampleDesc.Count != 1 || source.SampleDesc.Quality != 0
	    || source.Format != format.dxgi)
	{
		return {{}, OneShotStatus::InvalidSurface};
	}

	if (content.Width != poolSize.Width || content.Height != poolSize.Height
	    || static_cast<UINT>(content.Width) != source.Width
	    || static_cast<UINT>(content.Height) != source.Height)
	{
		return {{}, OneShotStatus::ResizeRequired};
	}

	auto image = QImage(content.Width, content.Height, format.image);
	if (image.isNull()) return {{}, OneShotStatus::AllocationFailed};
	if (image.bytesPerLine() < static_cast<qsizetype>(rowBytes)) {
		return {{}, OneShotStatus::InvalidReadback};
	}

	// Only now derive the staging descriptor and copy box. Keep the original
	// resource description intact until every dimension has been checked.
	auto staging = source;
	staging.Usage = D3D11_USAGE_STAGING;
	staging.BindFlags = 0;
	staging.MiscFlags = 0;
	staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	const D3D11_BOX box {0, 0, 0, source.Width, source.Height, 1};
	readback.copy(staging, box);

	const auto mapped = readback.map();
	const auto unmap = qScopeGuard([&]() { readback.unmap(); });
	constexpr auto maxBytes = static_cast<size_t>(std::numeric_limits<qsizetype>::max());
	const auto rowsBeforeLast = static_cast<size_t>(content.Height - 1);
	if (mapped.pData == nullptr || mapped.RowPitch < rowBytes
	    || rowsBeforeLast > (maxBytes - rowBytes) / mapped.RowPitch)
	{
		return {{}, OneShotStatus::InvalidReadback};
	}

	for (int y = 0; y < content.Height; ++y) {
		std::memcpy(
		    image.scanLine(y),
		    static_cast<const char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
		    rowBytes
		);
	}

	return {std::move(image), OneShotStatus::Ready};
}

} // namespace qs::windows::capture
