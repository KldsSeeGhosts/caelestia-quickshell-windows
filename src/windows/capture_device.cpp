#include "capture_device.hpp"

#include <utility>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <qlogging.h>
#include <qloggingcategory.h>

#include <winrt/base.h>

using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace qs::windows::capture {

namespace {
Q_LOGGING_CATEGORY(logCaptureDevice, "quickshell.windows.capture", QtWarningMsg);
}

HRESULT createCaptureDevice(CaptureDeviceParts* parts) {
	CaptureDeviceParts created;

	auto hr = D3D11CreateDevice(
	    nullptr,
	    D3D_DRIVER_TYPE_HARDWARE,
	    nullptr,
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	    nullptr,
	    0,
	    D3D11_SDK_VERSION,
	    created.d3d.put(),
	    nullptr,
	    created.context.put()
	);
	if (FAILED(hr)) return hr;

	try {
		winrt::com_ptr<::IInspectable> inspectable;
		winrt::check_hresult(
		    CreateDirect3D11DeviceFromDXGIDevice(created.d3d.as<IDXGIDevice>().get(), inspectable.put())
		);
		created.winrt = inspectable.as<IDirect3DDevice>();
	} catch (const winrt::hresult_error& e) {
		return e.code();
	}

	*parts = std::move(created);
	return S_OK;
}

HRESULT captureDeviceRemovedReason(ID3D11Device* device) {
	if (device == nullptr) return S_OK;
	return device->GetDeviceRemovedReason();
}

bool deviceRemovalResult(HRESULT result) {
	return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
}

bool CaptureDevice::ensure() {
	if (this->parts.d3d != nullptr) return true;

	CaptureDeviceParts created;
	this->lastError = this->source.create(&created);
	if (FAILED(this->lastError)) return false;

	this->parts = std::move(created);
	this->mGeneration++;
	return true;
}

HRESULT CaptureDevice::removedReason() const {
	if (this->parts.d3d == nullptr) return S_OK;
	return this->source.removedReason(this->parts.d3d.get());
}

void CaptureDevice::recover(HRESULT reason) {
	if (this->parts.d3d == nullptr) return;

	qCWarning(logCaptureDevice) << "Capture device lost:" << Qt::hex << static_cast<quint32>(reason)
	                            << "- releasing it; the next capture request creates a new device";
	this->release();
}

void CaptureDevice::release() {
	this->parts.winrt = nullptr;
	this->parts.context = nullptr;
	this->parts.d3d = nullptr;
}

HRESULT captureRemovalReason(const CaptureDevice& device, HRESULT result) {
	auto reason = device.removedReason();
	if (reason != S_OK) return reason;
	return deviceRemovalResult(result) ? result : S_OK;
}

bool recoveryStillApplies(const CaptureDevice& device, quint64 failedGeneration) {
	return device.present() && device.generation() == failedGeneration;
}

} // namespace qs::windows::capture
