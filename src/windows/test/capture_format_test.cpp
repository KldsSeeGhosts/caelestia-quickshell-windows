// Capture color policy fixtures. No capture session and no desktop is observed:
// the frame pool is created and closed without an item, which is enough to test
// the format the system accepts, and a shared frame is imported through the
// same RHI path the Qt consumer uses.

#include <memory>

#include <d3d11.h>
#include <inspectable.h>
#include <windows.h>

#include <QtTest>
#include <rhi/qrhi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "capture.hpp"
#include "capture_format.hpp"
#include "capture_texture.hpp"

using namespace qs::windows::capture;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

struct TestDevice {
	winrt::com_ptr<ID3D11Device> d3d;
	IDirect3DDevice winrt {nullptr};

	bool create() {
		auto hr = D3D11CreateDevice(
		    nullptr,
		    D3D_DRIVER_TYPE_HARDWARE,
		    nullptr,
		    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		    nullptr,
		    0,
		    D3D11_SDK_VERSION,
		    this->d3d.put(),
		    nullptr,
		    nullptr
		);
		if (FAILED(hr)) return false;

		winrt::com_ptr<::IInspectable> inspectable;
		hr = CreateDirect3D11DeviceFromDXGIDevice(
		    this->d3d.as<IDXGIDevice>().get(),
		    inspectable.put()
		);
		if (FAILED(hr)) return false;

		this->winrt = inspectable.as<IDirect3DDevice>();
		return true;
	}
};

} // namespace

class CaptureFormatTest: public QObject {
	Q_OBJECT;

private slots:
	void policyFormatIsTheDocumentedSdrFormat();
	void fallbackFormatIsStillSdr();
	void preferredFormatWins();
	void fallbackFormatIsUsedWhenThePolicyFormatIsRejected();
	void noFormatIsReportedWhenBothAreRejected();

	void framePoolAcceptsThePolicyFormat();

	void importedTexturesUseTheFrameFormat();
};

void CaptureFormatTest::policyFormatIsTheDocumentedSdrFormat() {
	QCOMPARE(SDR_FRAME_FORMAT.dxgi, DXGI_FORMAT_B8G8R8A8_UNORM);
	QCOMPARE(
	    static_cast<int>(SDR_FRAME_FORMAT.pixel),
	    static_cast<int>(DirectXPixelFormat::B8G8R8A8UIntNormalized)
	);
	QCOMPARE(SDR_FRAME_FORMAT.image, QImage::Format_RGB32);
	QCOMPARE(QString::fromLatin1(SDR_FRAME_FORMAT.name), QStringLiteral("B8G8R8A8_UNORM"));
}

void CaptureFormatTest::fallbackFormatIsStillSdr() {
	QCOMPARE(SDR_FALLBACK_FRAME_FORMAT.dxgi, DXGI_FORMAT_R8G8B8A8_UNORM);
	QCOMPARE(
	    static_cast<int>(SDR_FALLBACK_FRAME_FORMAT.pixel),
	    static_cast<int>(DirectXPixelFormat::R8G8B8A8UIntNormalized)
	);
	QCOMPARE(SDR_FALLBACK_FRAME_FORMAT.image, QImage::Format_RGBX8888);
}

void CaptureFormatTest::preferredFormatWins() {
	QStringList tried;
	auto choice = chooseFrameFormat([&](const FrameFormat& format) {
		tried.append(QString::fromLatin1(format.name));
		return true;
	});

	QVERIFY(choice.valid());
	QCOMPARE(choice.format, &SDR_FRAME_FORMAT);
	QVERIFY(!choice.fallback);
	QCOMPARE(tried, QStringList {"B8G8R8A8_UNORM"});
}

void CaptureFormatTest::fallbackFormatIsUsedWhenThePolicyFormatIsRejected() {
	QStringList tried;
	auto choice = chooseFrameFormat([&](const FrameFormat& format) {
		tried.append(QString::fromLatin1(format.name));
		return format.dxgi != SDR_FRAME_FORMAT.dxgi;
	});

	QVERIFY(choice.valid());
	QCOMPARE(choice.format, &SDR_FALLBACK_FRAME_FORMAT);
	QVERIFY(choice.fallback);
	QCOMPARE(tried, (QStringList {"B8G8R8A8_UNORM", "R8G8B8A8_UNORM"}));
}

void CaptureFormatTest::noFormatIsReportedWhenBothAreRejected() {
	auto choice = chooseFrameFormat([](const FrameFormat&) { return false; });
	QVERIFY(!choice.valid());
	QVERIFY(choice.format == nullptr);
}

void CaptureFormatTest::framePoolAcceptsThePolicyFormat() {
	TestDevice device;
	if (!device.create()) QSKIP("No Direct3D 11 hardware device on this system.");

	winrt::hresult policyError {};
	auto policyPool = tryCreateFramePool(device.winrt, SDR_FRAME_FORMAT, 2, {64, 64}, &policyError);
	auto policyMessage = QStringLiteral("WGC rejected %1: 0x%2")
	                         .arg(QString::fromLatin1(SDR_FRAME_FORMAT.name))
	                         .arg(static_cast<quint32>(policyError.value), 8, 16, QChar('0'));
	QVERIFY2(policyPool != nullptr, qPrintable(policyMessage));

	winrt::hresult fallbackError {};
	auto fallbackPool =
	    tryCreateFramePool(device.winrt, SDR_FALLBACK_FRAME_FORMAT, 2, {64, 64}, &fallbackError);
	auto fallbackMessage = QStringLiteral("WGC rejected the SDR fallback %1: 0x%2")
	                           .arg(QString::fromLatin1(SDR_FALLBACK_FRAME_FORMAT.name))
	                           .arg(static_cast<quint32>(fallbackError.value), 8, 16, QChar('0'));
	QVERIFY2(fallbackPool != nullptr, qPrintable(fallbackMessage));

	auto choice = chooseFrameFormat([&](const FrameFormat& format) {
		winrt::hresult error {};
		auto pool = tryCreateFramePool(device.winrt, format, 2, {64, 64}, &error);
		return pool != nullptr;
	});
	QVERIFY(choice.valid());
	QCOMPARE(choice.format->dxgi, DXGI_FORMAT_B8G8R8A8_UNORM);
	QVERIFY(!choice.fallback);

	policyPool.Close();
	fallbackPool.Close();
}

void CaptureFormatTest::importedTexturesUseTheFrameFormat() {
	QRhiD3D11InitParams params;
	std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &params));
	if (!rhi) QSKIP("No Direct3D 11 RHI on this system.");

	auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
	if (handles == nullptr || handles->dev == nullptr) QSKIP("The RHI has no Direct3D 11 device.");

	auto* device = static_cast<ID3D11Device*>(handles->dev);

	// The Qt consumer wraps a texture created on its own scene-graph device, in
	// the format the producer chose. Importing a BGRA8 texture as RGBA8 is what
	// broke the preview: D3D11 rejects the SRV and the view stays empty.
	for (const auto* format: {&SDR_FRAME_FORMAT, &SDR_FALLBACK_FRAME_FORMAT}) {
		auto frame = createSharedFrame(device, {64, 64}, *format);
		QVERIFY2(frame != nullptr, format->name);

		auto expected = format->dxgi == DXGI_FORMAT_B8G8R8A8_UNORM ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8;
		CaptureTexture imported;
		QVERIFY2(imported.create(rhi.get(), frame->texture, frame->size, frame->format), format->name);
		QVERIFY(imported.hasTexture());
		QCOMPARE(imported.format(), format->dxgi);
		QCOMPARE(static_cast<int>(imported.rhiTexture()->format()), static_cast<int>(expected));
	}
}

QTEST_MAIN(CaptureFormatTest)
#include "capture_format_test.moc"
