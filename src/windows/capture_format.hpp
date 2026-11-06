#pragma once

#include <memory>

#include <d3d11.h>
#include <dxgiformat.h>

#include <qimage.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>

namespace qs::windows::capture {

struct SharedFrame;

// One 8-bit SDR pixel layout, named in every vocabulary the capture path uses:
// the WGC frame pool, the shared texture, the Qt scene-graph texture and QImage.
struct FrameFormat {
	DXGI_FORMAT dxgi;
	winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixel;
	QImage::Format image;
	const char* name;
};

// The capture color policy is 8-bit SDR everywhere; see docs/CAPTURE-COLOR.md.
// B8G8R8A8 is the format Microsoft documents for the WGC frame pool.
inline constexpr FrameFormat SDR_FRAME_FORMAT {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
    QImage::Format_RGB32,
    "B8G8R8A8_UNORM",
};

// Before the policy was explicit this backend asked WGC for R8G8B8A8. WGC
// accepts that too, so it is the fallback when a system rejects the documented
// format. It is still 8-bit SDR: it never preserves HDR, and nothing here asks
// for an FP16 pool.
inline constexpr FrameFormat SDR_FALLBACK_FRAME_FORMAT {
    DXGI_FORMAT_R8G8B8A8_UNORM,
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R8G8B8A8UIntNormalized,
    QImage::Format_RGBX8888,
    "R8G8B8A8_UNORM",
};

struct FrameFormatChoice {
	const FrameFormat* format = nullptr;
	bool fallback = false;

	[[nodiscard]] bool valid() const { return this->format != nullptr; }
};

// Tries the policy format, then the SDR fallback. tryFormat returns true when
// the system accepted that format. There is no HDR branch on purpose.
template <typename TryFormat>
FrameFormatChoice chooseFrameFormat(TryFormat&& tryFormat) {
	if (tryFormat(SDR_FRAME_FORMAT)) return {&SDR_FRAME_FORMAT, false};
	if (tryFormat(SDR_FALLBACK_FRAME_FORMAT)) return {&SDR_FALLBACK_FRAME_FORMAT, true};
	return {};
}

// Creates a frame pool for one SDR format. Returns null and fills error when the
// system rejects the format or the pool cannot be created.
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool tryCreateFramePool(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice& device,
    const FrameFormat& format,
    int buffers,
    winrt::Windows::Graphics::SizeInt32 size,
    winrt::hresult* error = nullptr
);

// Creates the producer-side shared texture in the chosen SDR format. The
// consumer opens it and matches frame->format, so the format cannot drift
// between the frame pool and the Qt texture.
std::shared_ptr<SharedFrame>
createSharedFrame(ID3D11Device* device, const winrt::Windows::Graphics::SizeInt32& size, const FrameFormat& format);

} // namespace qs::windows::capture
