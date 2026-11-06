#pragma once

#include <d3d11.h>
#include <dxgiformat.h>

#include <qsize.h>

class QRhi;
class QRhiTexture;
class QSGTexture;

namespace qs::windows::capture {

// Wraps one D3D11 texture for Qt Quick through the RHI import path. The window
// preview and the video wallpaper share this; QSGD3D11Texture::fromNative is
// not used because it assumes RGBA8.
class CaptureTexture {
public:
	CaptureTexture() = default;
	~CaptureTexture();
	CaptureTexture(const CaptureTexture&) = delete;
	CaptureTexture& operator=(const CaptureTexture&) = delete;

	bool create(QRhi* rhi, ID3D11Texture2D* texture, const QSize& size, DXGI_FORMAT format);
	void release();

	[[nodiscard]] QSGTexture* texture() const { return this->sgTexture; }
	[[nodiscard]] bool hasTexture() const { return this->sgTexture != nullptr; }
	[[nodiscard]] QRhiTexture* rhiTexture() const { return this->mRhiTexture; }
	[[nodiscard]] QSize size() const { return this->mSize; }
	[[nodiscard]] DXGI_FORMAT format() const { return this->mFormat; }

private:
	QRhiTexture* mRhiTexture = nullptr;
	QSGTexture* sgTexture = nullptr;
	QSize mSize;
	DXGI_FORMAT mFormat = DXGI_FORMAT_UNKNOWN;
};

} // namespace qs::windows::capture

