#include "capture_format.hpp"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qsize.h>

#include <winrt/base.h>

#include "capture.hpp"

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Graphics::SizeInt32;

namespace qs::windows::capture {

namespace {
Q_LOGGING_CATEGORY(logCaptureFormat, "quickshell.windows.capture", QtWarningMsg);
}

Direct3D11CaptureFramePool tryCreateFramePool(
    const IDirect3DDevice& device,
    const FrameFormat& format,
    int buffers,
    SizeInt32 size,
    winrt::hresult* error
) {
	try {
		return Direct3D11CaptureFramePool::CreateFreeThreaded(device, format.pixel, buffers, size);
	} catch (const winrt::hresult_error& e) {
		qCWarning(logCaptureFormat) << "Frame pool rejected" << format.name << Qt::hex << e.code().value;
		if (error != nullptr) *error = e.code();
		return nullptr;
	}
}

SharedFrame::~SharedFrame() {
	if (this->mutex != nullptr) this->mutex->Release();
	if (this->texture != nullptr) this->texture->Release();
	if (this->handle != nullptr) CloseHandle(this->handle);
}

std::shared_ptr<SharedFrame> createSharedFrame(
    ID3D11Device* device,
    const SizeInt32& size,
    const FrameFormat& format
) {
	auto frame = std::make_shared<SharedFrame>();
	frame->size = QSize(size.Width, size.Height);
	frame->format = format.dxgi;

	D3D11_TEXTURE2D_DESC desc {};
	desc.Width = static_cast<UINT>(size.Width);
	desc.Height = static_cast<UINT>(size.Height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format.dxgi;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	auto hr = device->CreateTexture2D(&desc, nullptr, &frame->texture);
	if (FAILED(hr)) {
		qCWarning(logCaptureFormat) << "CreateTexture2D (shared frame) failed:" << Qt::hex << hr;
		return nullptr;
	}

	IDXGIResource1* resource = nullptr;
	hr = frame->texture->QueryInterface(IID_PPV_ARGS(&resource));
	if (SUCCEEDED(hr)) {
		hr = resource->CreateSharedHandle(
		    nullptr,
		    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		    nullptr,
		    &frame->handle
		);
		resource->Release();
	}

	if (SUCCEEDED(hr)) hr = frame->texture->QueryInterface(IID_PPV_ARGS(&frame->mutex));

	if (FAILED(hr)) {
		qCWarning(logCaptureFormat) << "Exporting the shared frame failed:" << Qt::hex << hr;
		return nullptr;
	}

	return frame;
}

} // namespace qs::windows::capture
