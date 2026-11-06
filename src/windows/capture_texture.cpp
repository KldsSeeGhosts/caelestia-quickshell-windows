#include "capture_texture.hpp"

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qsgtexture.h>
#include <rhi/qrhi.h>
#include <private/qsgplaintexture_p.h>

namespace qs::windows::capture {

namespace {
Q_LOGGING_CATEGORY(logCaptureTexture, "quickshell.windows.capture", QtWarningMsg);

// The scene-graph texture has to carry the producer's format: wrapping a BGRA8
// texture as RGBA8 makes D3D11 reject the SRV with E_INVALIDARG.
QRhiTexture::Format rhiFormatFor(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM: return QRhiTexture::BGRA8;
	case DXGI_FORMAT_R8G8B8A8_UNORM: return QRhiTexture::RGBA8;
	default: return QRhiTexture::UnknownFormat;
	}
}
} // namespace

CaptureTexture::~CaptureTexture() { this->release(); }

void CaptureTexture::release() {
	delete this->sgTexture;
	this->sgTexture = nullptr;
	if (this->mRhiTexture != nullptr) this->mRhiTexture->deleteLater();
	this->mRhiTexture = nullptr;
	this->mSize = QSize();
	this->mFormat = DXGI_FORMAT_UNKNOWN;
}

bool CaptureTexture::create(QRhi* rhi, ID3D11Texture2D* texture, const QSize& size, DXGI_FORMAT format) {
	auto rhiFormat = rhiFormatFor(format);
	if (rhi == nullptr || texture == nullptr || rhiFormat == QRhiTexture::UnknownFormat) return false;

	auto* rhiTexture = rhi->newTexture(rhiFormat, size);
	if (rhiTexture == nullptr) return false;

	if (!rhiTexture->createFrom({reinterpret_cast<quint64>(texture), 0})) {
		qCWarning(logCaptureTexture) << "Importing the capture texture as" << rhiFormat << "failed";
		delete rhiTexture;
		return false;
	}

	auto* sgTexture = new QSGPlainTexture();
	sgTexture->setOwnsTexture(false);
	sgTexture->setTexture(rhiTexture);
	sgTexture->setTextureSize(size);
	sgTexture->setHasAlphaChannel(false);

	this->release();
	this->mRhiTexture = rhiTexture;
	this->sgTexture = sgTexture;
	this->mSize = size;
	this->mFormat = format;
	return true;
}

} // namespace qs::windows::capture



