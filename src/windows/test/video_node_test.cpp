// The real VideoNode from video_wallpaper.cpp, driven offscreen. No media is
// decoded and no desktop is captured: a QQuickRenderControl window that is
// never shown provides the Direct3D 11 scene-graph device, and synthetic shared
// frames stand in for the decoder output.

#include <memory>

#include <d3d11.h>
#include <windows.h>

#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickRenderControl>
#include <QtQuick/QQuickWindow>
#include <QtTest>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <winrt/base.h>

#include "capture.hpp"
#include "capture_format.hpp"
#include "capture_texture.hpp"

// The node lives in an unnamed namespace of the implementation file, so the
// test compiles the real one instead of a copy of its logic.
#include "video_wallpaper.cpp"

using namespace qs::windows::capture;

namespace {

// A shared frame whose keyed mutex is primed the way the producer leaves it.
std::shared_ptr<SharedFrame> readyFrame(ID3D11Device* device, int width, int height, const FrameFormat& format) {
	auto frame = createSharedFrame(device, {width, height}, format);
	if (!frame || frame->mutex->AcquireSync(0, 0) != S_OK) return nullptr;
	frame->mutex->ReleaseSync(1);
	return frame;
}

} // namespace

class VideoNodeTest: public QObject {
	Q_OBJECT;

private slots:
	void initTestCase() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11); }

	void frameReplacementKeepsTheNewImport();
	void recreatingAnImportChangesTheFormat();
};

void VideoNodeTest::frameReplacementKeepsTheNewImport() {
	QQuickRenderControl control;
	QQuickWindow window(&control); // never shown
	window.setGeometry(0, 0, 16, 16);
	if (!control.initialize() || control.rhi() == nullptr || control.rhi()->backend() != QRhi::D3D11) {
		QSKIP("No Direct3D 11 scene graph on this system.");
	}

	auto* handles = static_cast<const QRhiD3D11NativeHandles*>(control.rhi()->nativeHandles());
	auto* device = static_cast<ID3D11Device*>(handles->dev);

	qs::windows::video::VideoNode node(&window);

	// First frame: no earlier import exists.
	auto first = readyFrame(device, 16, 16, SDR_FRAME_FORMAT);
	QVERIFY(first != nullptr);
	QVERIFY(node.sync(first, 1));
	QVERIFY(node.hasTexture());
	QCOMPARE(node.frameSize(), QSize(16, 16));
	auto* firstTexture = node.QSGSimpleTextureNode::texture();
	QVERIFY(firstTexture != nullptr);

	// A second frame of the same size reuses the import.
	auto second = readyFrame(device, 16, 16, SDR_FRAME_FORMAT);
	QVERIFY(second != nullptr);
	QVERIFY(node.sync(second, 2));
	QVERIFY(node.hasTexture());
	QCOMPARE(node.QSGSimpleTextureNode::texture(), firstTexture);

	// A new size replaces the import. The replacement must survive and be the
	// texture the node hands to the scene graph.
	auto resized = readyFrame(device, 32, 24, SDR_FRAME_FORMAT);
	QVERIFY(resized != nullptr);
	QVERIFY(node.sync(resized, 3));
	QVERIFY(node.hasTexture());
	QCOMPARE(node.frameSize(), QSize(32, 24));
	QVERIFY(node.QSGSimpleTextureNode::texture() != nullptr);
}

void VideoNodeTest::recreatingAnImportChangesTheFormat() {
	QRhiD3D11InitParams params;
	std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &params));
	if (!rhi) QSKIP("No Direct3D 11 RHI on this system.");

	auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
	auto* device = static_cast<ID3D11Device*>(handles->dev);

	// The preview re-imports when the producer switches format. The second
	// create() must replace the first and leave a usable texture behind.
	auto bgra = createSharedFrame(device, {16, 16}, SDR_FRAME_FORMAT);
	auto rgba = createSharedFrame(device, {16, 16}, SDR_FALLBACK_FRAME_FORMAT);
	QVERIFY(bgra != nullptr);
	QVERIFY(rgba != nullptr);

	CaptureTexture imported;
	QVERIFY(imported.create(rhi.get(), bgra->texture, bgra->size, bgra->format));
	QVERIFY(imported.create(rhi.get(), rgba->texture, rgba->size, rgba->format));
	QVERIFY(imported.hasTexture());
	QCOMPARE(imported.format(), DXGI_FORMAT_R8G8B8A8_UNORM);
	QCOMPARE(static_cast<int>(imported.rhiTexture()->format()), static_cast<int>(QRhiTexture::RGBA8));
}

QTEST_MAIN(VideoNodeTest)
#include "video_node_test.moc"
