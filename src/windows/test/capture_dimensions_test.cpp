// No device, WGC session, desktop, or captured image. The real one-shot guard
// consumes injected original source descriptions and generated padded rows.
#include <limits>

#include <qbytearray.h>
#include <qcolor.h>
#include <qtest.h>

#include "capture_dimensions.hpp"

using namespace qs::windows::capture;
using winrt::Windows::Graphics::SizeInt32;

namespace {

D3D11_TEXTURE2D_DESC surface(UINT width, UINT height) {
	D3D11_TEXTURE2D_DESC desc {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = SDR_FRAME_FORMAT.dxgi;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	return desc;
}

struct FakeReadback {
	D3D11_TEXTURE2D_DESC actual = surface(4, 3);
	D3D11_TEXTURE2D_DESC staging {};
	D3D11_BOX box {};
	D3D11_MAPPED_SUBRESOURCE mapped {};
	HRESULT copyError = S_OK;
	HRESULT mapError = S_OK;
	int descriptions = 0;
	int copies = 0;
	int maps = 0;
	int unmaps = 0;

	D3D11_TEXTURE2D_DESC description() {
		++this->descriptions;
		return this->actual;
	}
	void copy(const D3D11_TEXTURE2D_DESC& desc, const D3D11_BOX& region) {
		++this->copies;
		this->staging = desc;
		this->box = region;
		winrt::check_hresult(this->copyError);
	}
	D3D11_MAPPED_SUBRESOURCE map() {
		++this->maps;
		winrt::check_hresult(this->mapError);
		return this->mapped;
	}
	void unmap() { ++this->unmaps; }
};

} // namespace

class CaptureDimensionsTest: public QObject {
	Q_OBJECT;

private slots:
	void rejectsDimensionsBeforeCopy_data();
	void rejectsDimensionsBeforeCopy();
	void rejectsSurfaceBeforeCopy_data();
	void rejectsSurfaceBeforeCopy();
	void returnsDeclaredPixels_data();
	void returnsDeclaredPixels();
	void rejectsMappedData_data();
	void rejectsMappedData();
	void apiFailureDoesNotPublish_data();
	void apiFailureDoesNotPublish();
};

void CaptureDimensionsTest::rejectsDimensionsBeforeCopy_data() {
	QTest::addColumn<int>("poolWidth");
	QTest::addColumn<int>("poolHeight");
	QTest::addColumn<int>("width");
	QTest::addColumn<int>("height");
	QTest::addColumn<quint32>("sourceWidth");
	QTest::addColumn<quint32>("sourceHeight");
	QTest::addColumn<OneShotStatus>("expected");

	const auto resize = OneShotStatus::ResizeRequired;
	const auto invalid = OneShotStatus::InvalidSize;
	const auto overflow = OneShotStatus::SizeOverflow;
	const auto badSurface = OneShotStatus::InvalidSurface;
	QTest::newRow("grow-width") << 4 << 3 << 5 << 3 << 4u << 3u << resize;
	QTest::newRow("grow-height") << 4 << 3 << 4 << 4 << 4u << 3u << resize;
	QTest::newRow("shrink-width") << 4 << 3 << 3 << 3 << 4u << 3u << resize;
	QTest::newRow("shrink-height") << 4 << 3 << 4 << 2 << 4u << 3u << resize;
	QTest::newRow("rotate-with-old-surface") << 4 << 3 << 3 << 4 << 4u << 3u << resize;
	QTest::newRow("rotate-with-new-surface") << 4 << 3 << 3 << 4 << 3u << 4u << resize;
	QTest::newRow("stale-pool-matching-new-source") << 4 << 3 << 5 << 3 << 5u << 3u << resize;
	// Content agrees with the requested size, but the *actual* surface does not.
	QTest::newRow("actual-source-narrower") << 4 << 3 << 4 << 3 << 3u << 3u << resize;
	QTest::newRow("actual-source-shorter") << 4 << 3 << 4 << 3 << 4u << 2u << resize;
	QTest::newRow("actual-source-wider") << 4 << 3 << 4 << 3 << 5u << 3u << resize;
	QTest::newRow("actual-source-taller") << 4 << 3 << 4 << 3 << 4u << 4u << resize;
	QTest::newRow("actual-source-rotated") << 4 << 3 << 4 << 3 << 3u << 4u << resize;
	QTest::newRow("zero-content-width") << 4 << 3 << 0 << 3 << 4u << 3u << invalid;
	QTest::newRow("zero-content-height") << 4 << 3 << 4 << 0 << 4u << 3u << invalid;
	QTest::newRow("negative-content-width") << 4 << 3 << -1 << 3 << 4u << 3u << invalid;
	QTest::newRow("negative-content-height") << 4 << 3 << 4 << -1 << 4u << 3u << invalid;
	QTest::newRow("zero-pool") << 0 << 3 << 4 << 3 << 4u << 3u << invalid;
	QTest::newRow("negative-pool") << 4 << -1 << 4 << 3 << 4u << 3u << invalid;
	QTest::newRow("zero-source-width") << 4 << 3 << 4 << 3 << 0u << 3u << badSurface;
	QTest::newRow("zero-source-height") << 4 << 3 << 4 << 3 << 4u << 0u << badSurface;
	const auto maxInt = std::numeric_limits<int>::max();
	QTest::newRow("row-byte-overflow") << 4 << 3 << maxInt << 3 << 4u << 3u << overflow;
	QTest::newRow("image-byte-overflow")
	    << maxInt << maxInt << maxInt << maxInt << static_cast<quint32>(maxInt)
	    << static_cast<quint32>(maxInt) << overflow;
	QTest::newRow("d3d-dimension-limit") << 4 << 3 << 16385 << 3 << 16385u << 3u << invalid;
	QTest::newRow("unsigned-source-limit")
	    << 4 << 3 << 4 << 3 << std::numeric_limits<quint32>::max() << 3u << resize;
}

void CaptureDimensionsTest::rejectsDimensionsBeforeCopy() {
	QFETCH(int, poolWidth);
	QFETCH(int, poolHeight);
	QFETCH(int, width);
	QFETCH(int, height);
	QFETCH(quint32, sourceWidth);
	QFETCH(quint32, sourceHeight);
	QFETCH(OneShotStatus, expected);
	FakeReadback readback;
	readback.actual = surface(sourceWidth, sourceHeight);
	const auto result =
	    readOneShotFrame({poolWidth, poolHeight}, {width, height}, SDR_FRAME_FORMAT, readback);
	QCOMPARE(readback.copies, 0);
	QCOMPARE(readback.maps, 0);
	QCOMPARE(readback.unmaps, 0);
	QCOMPARE(result.status, expected);
	QVERIFY(result.image.isNull());
}

void CaptureDimensionsTest::rejectsSurfaceBeforeCopy_data() {
	QTest::addColumn<int>("kind");
	QTest::newRow("unexpected-hdr-format") << 0;
	QTest::newRow("wrong-sdr-channel-layout") << 1;
	QTest::newRow("multisampled") << 2;
	QTest::newRow("sample-quality") << 3;
	QTest::newRow("mip-chain") << 4;
	QTest::newRow("texture-array") << 5;
}

void CaptureDimensionsTest::rejectsSurfaceBeforeCopy() {
	QFETCH(int, kind);
	FakeReadback readback;
	switch (kind) {
	case 0: readback.actual.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
	case 1: readback.actual.Format = SDR_FALLBACK_FRAME_FORMAT.dxgi; break;
	case 2: readback.actual.SampleDesc.Count = 2; break;
	case 3: readback.actual.SampleDesc.Quality = 1; break;
	case 4: readback.actual.MipLevels = 2; break;
	case 5: readback.actual.ArraySize = 2; break;
	}
	const auto result = readOneShotFrame({4, 3}, {4, 3}, SDR_FRAME_FORMAT, readback);
	QCOMPARE(result.status, OneShotStatus::InvalidSurface);
	QVERIFY(result.image.isNull());
	QCOMPARE(readback.descriptions, 1);
	QCOMPARE(readback.copies, 0);
	QCOMPARE(readback.maps, 0);
}

void CaptureDimensionsTest::returnsDeclaredPixels_data() {
	QTest::addColumn<int>("width");
	QTest::addColumn<int>("height");
	QTest::addColumn<int>("padding");
	QTest::addColumn<bool>("fallback");
	QTest::newRow("landscape-padded-bgra") << 4 << 3 << 12 << false;
	QTest::newRow("landscape-tight-bgra") << 4 << 3 << 0 << false;
	QTest::newRow("portrait-new-request") << 3 << 4 << 4 << false;
	QTest::newRow("single-pixel") << 1 << 1 << 0 << false;
	QTest::newRow("padded-sdr-fallback") << 4 << 3 << 8 << true;
}

void CaptureDimensionsTest::returnsDeclaredPixels() {
	QFETCH(int, width);
	QFETCH(int, height);
	QFETCH(int, padding);
	QFETCH(bool, fallback);
	const auto& format = fallback ? SDR_FALLBACK_FRAME_FORMAT : SDR_FRAME_FORMAT;
	const auto pitch = width * 4 + padding;
	// Deliberately omit padding after the final row. Reading/copying RowPitch
	// bytes instead of pixel bytes would overrun this generated buffer.
	QByteArray bytes((height - 1) * pitch + width * 4, '\x5a');
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto* pixel = bytes.data() + y * pitch + x * 4;
			pixel[0] = static_cast<char>(fallback ? 20 + x : 80 + y);
			pixel[1] = 50;
			pixel[2] = static_cast<char>(fallback ? 80 + y : 20 + x);
			pixel[3] = '\xff';
		}
	}

	FakeReadback readback;
	readback.actual = surface(static_cast<UINT>(width), static_cast<UINT>(height));
	readback.actual.Format = format.dxgi;
	const auto original = readback.actual;
	readback.mapped = {bytes.data(), static_cast<UINT>(pitch), 0};
	const auto result = readOneShotFrame({width, height}, {width, height}, format, readback);
	QCOMPARE(result.status, OneShotStatus::Ready);
	QCOMPARE(result.image.size(), QSize(width, height));
	QCOMPARE(result.image.format(), format.image);
	QCOMPARE(readback.descriptions, 1);
	QCOMPARE(readback.copies, 1);
	QCOMPARE(readback.maps, 1);
	QCOMPARE(readback.unmaps, 1);
	QCOMPARE(readback.actual.Width, original.Width);
	QCOMPARE(readback.actual.Height, original.Height);
	QCOMPARE(readback.staging.Width, original.Width);
	QCOMPARE(readback.staging.Height, original.Height);
	QCOMPARE(readback.staging.Format, format.dxgi);
	QCOMPARE(readback.staging.Usage, D3D11_USAGE_STAGING);
	QCOMPARE(readback.staging.BindFlags, 0u);
	QCOMPARE(readback.staging.MiscFlags, 0u);
	QCOMPARE(readback.staging.CPUAccessFlags, static_cast<UINT>(D3D11_CPU_ACCESS_READ));
	QCOMPARE(readback.box.left, 0u);
	QCOMPARE(readback.box.top, 0u);
	QCOMPARE(readback.box.front, 0u);
	QCOMPARE(readback.box.right, original.Width);
	QCOMPARE(readback.box.bottom, original.Height);
	QCOMPARE(readback.box.back, 1u);
	for (int y = 0; y < height; ++y) {
		QCOMPARE(
		    QByteArray(reinterpret_cast<const char*>(result.image.constScanLine(y)), width * 4),
		    bytes.mid(y * pitch, width * 4)
		);
		for (int x = 0; x < width; ++x) {
			QCOMPARE(result.image.pixelColor(x, y), QColor(20 + x, 50, 80 + y));
		}
	}
}

void CaptureDimensionsTest::rejectsMappedData_data() {
	QTest::addColumn<bool>("nullData");
	QTest::addColumn<quint32>("pitch");
	QTest::newRow("null-data") << true << 16u;
	QTest::newRow("zero-stride") << false << 0u;
	QTest::newRow("short-stride") << false << 15u;
}

void CaptureDimensionsTest::rejectsMappedData() {
	QFETCH(bool, nullData);
	QFETCH(quint32, pitch);
	char canary = '\x5a';
	FakeReadback readback;
	readback.mapped = {nullData ? nullptr : &canary, pitch, 0};
	const auto result = readOneShotFrame({4, 3}, {4, 3}, SDR_FRAME_FORMAT, readback);
	QCOMPARE(result.status, OneShotStatus::InvalidReadback);
	QVERIFY(result.image.isNull());
	QCOMPARE(readback.copies, 1);
	QCOMPARE(readback.maps, 1);
	QCOMPARE(readback.unmaps, 1);
	QCOMPARE(canary, '\x5a');
}

void CaptureDimensionsTest::apiFailureDoesNotPublish_data() {
	QTest::addColumn<bool>("duringCopy");
	QTest::addColumn<qint32>("error");
	QTest::newRow("create-device-removed") << true << static_cast<qint32>(DXGI_ERROR_DEVICE_REMOVED);
	QTest::newRow("map-device-reset") << false << static_cast<qint32>(DXGI_ERROR_DEVICE_RESET);
	QTest::newRow("create-failure") << true << static_cast<qint32>(E_FAIL);
	QTest::newRow("map-failure") << false << static_cast<qint32>(E_FAIL);
}

void CaptureDimensionsTest::apiFailureDoesNotPublish() {
	QFETCH(bool, duringCopy);
	QFETCH(qint32, error);
	FakeReadback readback;
	if (duringCopy) readback.copyError = error;
	else readback.mapError = error;
	OneShotReadback result {{}, OneShotStatus::InvalidReadback};
	auto caught = false;
	try {
		result = readOneShotFrame({4, 3}, {4, 3}, SDR_FRAME_FORMAT, readback);
	} catch (const winrt::hresult_error& e) {
		caught = true;
		QCOMPARE(e.code().value, error);
	}
	QVERIFY(caught);
	QVERIFY(result.image.isNull());
	QCOMPARE(readback.copies, 1);
	QCOMPARE(readback.maps, duringCopy ? 0 : 1);
	// Map never succeeded, so no Unmap is owed.
	QCOMPARE(readback.unmaps, 0);
}

QTEST_GUILESS_MAIN(CaptureDimensionsTest)
#include "capture_dimensions_test.moc"
