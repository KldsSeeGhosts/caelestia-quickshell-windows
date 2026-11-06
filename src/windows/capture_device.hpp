#pragma once

#include <d3d11.h>

#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <qtypes.h>

namespace qs::windows::capture {

// The Direct3D 11 device, its immediate context and the WinRT wrapper the WGC
// frame pools are created from. A GPU device removal (driver update, TDR,
// adapter reset) makes all three unusable for good: DXGI keeps returning the
// removal reason from every call on that device.
struct CaptureDeviceParts {
	winrt::com_ptr<ID3D11Device> d3d;
	winrt::com_ptr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt {nullptr};
};

// Fills parts with a new hardware device. Fixtures replace this to count
// creations or to make one fail; production uses the default.
using CaptureDeviceFactory = HRESULT (*)(CaptureDeviceParts* parts);

// Reports why a device stopped working: ID3D11Device::GetDeviceRemovedReason.
// Fixtures replace this to inject DXGI_ERROR_DEVICE_REMOVED without a real GPU
// reset.
using CaptureDeviceRemovalProbe = HRESULT (*)(ID3D11Device* device);

HRESULT createCaptureDevice(CaptureDeviceParts* parts);
HRESULT captureDeviceRemovedReason(ID3D11Device* device);

// True when result is DXGI's answer to a call on a device that is gone:
// DXGI_ERROR_DEVICE_REMOVED or DXGI_ERROR_DEVICE_RESET.
bool deviceRemovalResult(HRESULT result);

class CaptureDevice {
public:
	struct Source {
		CaptureDeviceFactory create = &createCaptureDevice;
		CaptureDeviceRemovalProbe removedReason = &captureDeviceRemovedReason;
	};

	CaptureDevice() = default;
	explicit CaptureDevice(Source source): source(source) {}

	// Creates a device when there is none. A failed creation leaves the HRESULT
	// in error() and is not retried here: the next capture request tries once.
	bool ensure();

	// S_OK while the device is usable; the DXGI removal reason once it is gone.
	[[nodiscard]] HRESULT removedReason() const;

	// Reports the removal at warning level and releases the device. The frame
	// pools and sessions built on it must already be closed. One report per
	// removal event: calling this again with no device present does nothing.
	void recover(HRESULT reason);

	void release();

	[[nodiscard]] bool present() const { return this->parts.d3d != nullptr; }
	// Bumped for every created device. Work queued for a device that failed
	// can tell it apart from a replacement created before that work ran.
	[[nodiscard]] quint64 generation() const { return this->mGeneration; }
	[[nodiscard]] HRESULT error() const { return this->lastError; }
	[[nodiscard]] const winrt::com_ptr<ID3D11Device>& d3d() const { return this->parts.d3d; }
	[[nodiscard]] const winrt::com_ptr<ID3D11DeviceContext>& context() const {
		return this->parts.context;
	}
	[[nodiscard]] const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice&
	winrtDevice() const {
		return this->parts.winrt;
	}

private:
	Source source {};
	CaptureDeviceParts parts;
	HRESULT lastError = S_OK;
	quint64 mGeneration = 0;
};

// Why a capture call failed. The device's own removal reason wins over the
// call's HRESULT, which DXGI reports as DXGI_ERROR_DEVICE_REMOVED or _RESET for
// the same event. S_OK means the failure was something else and the device
// stays.
HRESULT captureRemovalReason(const CaptureDevice& device, HRESULT result);

// A recovery queued from a failed device names the generation it failed on. It
// still applies only while that device is the current one: a capture request
// may have replaced it with a healthy device before the queued work ran.
[[nodiscard]] bool recoveryStillApplies(const CaptureDevice& device, quint64 failedGeneration);

} // namespace qs::windows::capture
