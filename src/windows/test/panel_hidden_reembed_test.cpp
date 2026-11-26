// #214 P2: WinPanelWindow keeps its QQuickWindow and HWND when the QML window
// is hidden, because deleteOnInvisible is false. A later host replacement
// re-embeds that still-hidden window. Re-embedding must not reveal it.
//
// This fixture instantiates the production WinPanelWindow, so it needs the
// Windows QPA and a real HWND. desktop_zorder_test cannot catch this path: its
// raw-HWND callback never creates a panel.

#include <atomic>
#include <memory>

#include <qelapsedtimer.h>
#include <qguiapplication.h>
#include <qscreen.h>
#include <qsignalspy.h>
#include <qtest.h>
#include <qthread.h>
#include <qt_windows.h>

#include "desktop_host.hpp"
#include "erm625_desktop_fakes.hpp"
#include "native_desktop_accessor.hpp"
#include "panel_window.hpp"
#include "virtual_desktops.hpp"

namespace qs::windows {
struct NativeDesktopFacadeTestHook {
	static void selectNative(VirtualDesktops& desktops) {
		desktops.nativeAccessor = true;
		desktops.testAvailability = DesktopAvailability::Disconnected;
	}
	static void observeSnapshots(VirtualDesktops& desktops) {
		desktops.bindNativeDesktopQueries();
	}
};
} // namespace qs::windows

namespace {

HWND g_fakePinnedHwnd = nullptr;
bool g_allowUnpin = true;
int g_pinCalls = 0;
int g_unpinCalls = 0;

int fakeIsPinnedWindow(HWND hwnd) { return hwnd != nullptr && hwnd == g_fakePinnedHwnd ? 1 : 0; }
int fakePinWindow(HWND hwnd) {
	++g_pinCalls;
	g_fakePinnedHwnd = hwnd;
	return 0;
}
int fakeUnpinWindow(HWND hwnd) {
	++g_unpinCalls;
	if (!g_allowUnpin) return -1;
	if (g_fakePinnedHwnd == hwnd) g_fakePinnedHwnd = nullptr;
	return 0;
}

constexpr const wchar_t* HOST_CLASS = L"CaelestiaPanelHiddenReembedTestHost";

LRESULT CALLBACK hostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

void registerHostClass() {
	WNDCLASSW cls {};
	cls.lpfnWndProc = &hostProc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = HOST_CLASS;
	RegisterClassW(&cls);
}

HWND createHost() {
	return CreateWindowExW(
	    0,
	    HOST_CLASS,
	    L"",
	    WS_POPUP | WS_VISIBLE,
	    -10000,
	    -10000,
	    200,
	    100,
	    nullptr,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
}

} // namespace

class TestPanelHiddenReembed: public QObject {
	Q_OBJECT

private slots:
	void initTestCase() {
		if (QGuiApplication::platformName() != QStringLiteral("windows")) {
			QSKIP("The panel HWND regression needs the Windows QPA.");
		}

		registerHostClass();
	}

	void init() {
		if (!needsHosts()) return;

		m_hostA = createHost();
		m_hostB = createHost();
		QVERIFY(m_hostA != nullptr && m_hostB != nullptr);

		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(true, true);
		host->adoptHostForTesting(m_hostA);
	}

	void cleanup() {
		if (!needsHosts()) return;

		auto* host = qs::windows::DesktopHost::instance();
		host->setDiscoveredHostForTesting(nullptr);
		host->setEnabled(false);
		QCoreApplication::processEvents();

		if (m_hostB != nullptr) DestroyWindow(m_hostB);
		if (m_hostA != nullptr) DestroyWindow(m_hostA);
		m_hostA = nullptr;
		m_hostB = nullptr;
	}

	void hiddenPanelStaysHiddenAcrossHostReplacement() {
		auto* primary = QGuiApplication::primaryScreen();
		QVERIFY(primary != nullptr);
		QuickshellScreenInfo screenInfo(nullptr, primary);
		qs::windows::WinPanelWindow panel;
		panel.setLayer(qs::windows::PanelLayer::Background);
		panel.setExclusionMode(ExclusionMode::Ignore);
		panel.setScreen(&screenInfo);
		panel.createWindow();

		auto* window = panel.backingWindow();
		QVERIFY(window != nullptr);
		auto* hwnd = reinterpret_cast<HWND>(window->winId());
		QVERIFY(hwnd != nullptr && IsWindow(hwnd));
		QVERIFY(SetWindowPos(
		    hwnd,
		    nullptr,
		    -10000,
		    -10000,
		    200,
		    100,
		    SWP_NOACTIVATE | SWP_NOZORDER
		));

		panel.setVisible(true);
		panel.setVisibleDirect(true);
		QTRY_VERIFY(GetParent(hwnd) == m_hostA);
		QVERIFY(IsWindowVisible(hwnd));

		// A Background/Bottom panel that was shown and then hidden keeps its
		// QQuickWindow and HWND (deleteOnInvisible is false).
		panel.setVisible(false);
		panel.setVisibleDirect(false);
		QTRY_VERIFY(!IsWindowVisible(hwnd));
		QVERIFY(IsWindow(hwnd));
		QVERIFY(GetParent(hwnd) == m_hostA);

		// A host replacement (the handback edge or toggling desktop embedding)
		// re-runs discovery while the old host stays valid. The hidden panel
		// re-embeds into the new host and must stay hidden.
		const auto presentsBeforeMove = panel.presentResetsForTesting();
		auto* host = qs::windows::DesktopHost::instance();
		host->setDiscoveredHostForTesting(m_hostB);
		host->refreshForTesting();

		QTRY_VERIFY(GetParent(hwnd) == m_hostB);
		QVERIFY(IsWindow(hwnd));
		QVERIFY(!window->isVisible());
		QVERIFY(!IsWindowVisible(hwnd));
		// The hidden panel was re-embedded without being shown to force a
		// frame.
		QCOMPARE(panel.presentResetsForTesting(), presentsBeforeMove);

		// Destroy the backing window before cleanup destroys the hosts, so the
		// native child is not torn down externally.
		panel.deleteWindow();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}

	void visiblePanelPresentsAfterHostReplacement() {
		auto* primary = QGuiApplication::primaryScreen();
		QVERIFY(primary != nullptr);
		QuickshellScreenInfo screenInfo(nullptr, primary);
		qs::windows::WinPanelWindow panel;
		panel.setLayer(qs::windows::PanelLayer::Background);
		panel.setExclusionMode(ExclusionMode::Ignore);
		panel.setScreen(&screenInfo);
		panel.createWindow();

		auto* window = panel.backingWindow();
		QVERIFY(window != nullptr);
		auto* hwnd = reinterpret_cast<HWND>(window->winId());
		QVERIFY(hwnd != nullptr && IsWindow(hwnd));
		QVERIFY(SetWindowPos(
		    hwnd,
		    nullptr,
		    -10000,
		    -10000,
		    200,
		    100,
		    SWP_NOACTIVATE | SWP_NOZORDER
		));

		panel.setVisible(true);
		panel.setVisibleDirect(true);
		QTRY_VERIFY(GetParent(hwnd) == m_hostA);
		QVERIFY(IsWindowVisible(hwnd));
		QVERIFY(window->isVisible());
		const auto presentsBeforeMove = panel.presentResetsForTesting();

		// #214: Wallpaper Engine hands the desktop back while the same host is
		// reused, or Explorer replaces the host on that edge. A visible
		// embedded background must be told to present at the new parent, not
		// left compositing the surface it never painted while covered.
		auto* host = qs::windows::DesktopHost::instance();
		host->setDiscoveredHostForTesting(m_hostB);
		host->refreshForTesting();

		QTRY_VERIFY(GetParent(hwnd) == m_hostB);
		QTRY_VERIFY(panel.presentResetsForTesting() > presentsBeforeMove);
		QVERIFY(IsWindowVisible(hwnd));

		panel.deleteWindow();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}

	// excludeFromCapture is the QML-facing property on the production panel. The
	// window is created but never shown, so this touches no visible window and
	// needs no desktop host.
	void excludeFromCaptureFollowsTheProperty() {
		auto* primary = QGuiApplication::primaryScreen();
		QVERIFY(primary != nullptr);
		QuickshellScreenInfo screenInfo(nullptr, primary);
		qs::windows::WinPanelWindow panel;
		panel.setLayer(qs::windows::PanelLayer::Overlay);
		panel.setExclusionMode(ExclusionMode::Ignore);
		panel.setScreen(&screenInfo);

		// Default, and a value set before the window exists.
		QVERIFY(!panel.excludeFromCapture());
		QSignalSpy changed(&panel, &qs::windows::WinPanelWindow::excludeFromCaptureChanged);
		panel.setExcludeFromCapture(true);
		QVERIFY(panel.excludeFromCapture());
		QCOMPARE(changed.count(), 1);
		panel.setExcludeFromCapture(true);
		QCOMPARE(changed.count(), 1);
		QCOMPARE(panel.property("excludeFromCapture").toBool(), true);

		panel.createWindow();
		auto* window = panel.backingWindow();
		QVERIFY(window != nullptr);
		auto* hwnd = reinterpret_cast<HWND>(window->winId());
		QVERIFY(hwnd != nullptr && IsWindow(hwnd));
		QVERIFY(!IsWindowVisible(hwnd));

		// The creation applied the value that was set before it.
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));

		panel.setExcludeFromCapture(false);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_NONE));
		QCOMPARE(changed.count(), 2);

		panel.setExcludeFromCapture(true);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
		QCOMPARE(changed.count(), 3);

		// Changing other panel state leaves the exclusion alone.
		panel.setFocusable(true);
		panel.setAboveWindows(false);
		panel.setAboveWindows(true);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
		QVERIFY(panel.excludeFromCapture());

		// A recreated native window starts without the affinity and gets it again.
		panel.deleteWindow();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QVERIFY(panel.excludeFromCapture());

		panel.createWindow();
		window = panel.backingWindow();
		QVERIFY(window != nullptr);
		hwnd = reinterpret_cast<HWND>(window->winId());
		QVERIFY(hwnd != nullptr && IsWindow(hwnd));
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));

		// An unrelated panel is unaffected.
		qs::windows::WinPanelWindow other;
		other.setLayer(qs::windows::PanelLayer::Overlay);
		other.setExclusionMode(ExclusionMode::Ignore);
		other.setScreen(&screenInfo);
		other.createWindow();
		auto* otherHwnd = reinterpret_cast<HWND>(other.backingWindow()->winId());
		QVERIFY(otherHwnd != nullptr && IsWindow(otherHwnd));
		QCOMPARE(affinityOf(otherHwnd), static_cast<DWORD>(WDA_NONE));

		other.deleteWindow();
		panel.deleteWindow();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}

	void pinnedPanelUnpinsOrRefusesBeforeEmbeddingEvenAcrossHideAndRecovery() {
		auto* primary = QGuiApplication::primaryScreen();
		QVERIFY(primary != nullptr);
		QuickshellScreenInfo screenInfo(nullptr, primary);

		auto* desktops = qs::windows::VirtualDesktops::instance();
		g_fakePinnedHwnd = nullptr;
		g_allowUnpin = false;
		g_pinCalls = 0;
		g_unpinCalls = 0;
		qs::windows::VirtualDesktops::Accessor fakeAccessor {};
		fakeAccessor.isPinnedWindow = &fakeIsPinnedWindow;
		fakeAccessor.pinWindow = &fakePinWindow;
		fakeAccessor.unPinWindow = &fakeUnpinWindow;
		desktops->installAccessorForTest(fakeAccessor);
		struct AccessorCleanup {
			qs::windows::VirtualDesktops* desktops;
			~AccessorCleanup() {
				g_fakePinnedHwnd = nullptr;
				g_allowUnpin = true;
				this->desktops->loseAccessorForTest();
			}
		} accessorCleanup {desktops};

		// Start with DesktopHost disabled so the Background panel stays top-level and pins.
		auto* host = qs::windows::DesktopHost::instance();
		host->setEnabled(false);

		qs::windows::WinPanelWindow panel;
		panel.setLayer(qs::windows::PanelLayer::Background);
		panel.setExclusionMode(ExclusionMode::Ignore);
		panel.setScreen(&screenInfo);
		panel.createWindow();

		auto* window = panel.backingWindow();
		QVERIFY(window != nullptr);
		auto* hwnd = reinterpret_cast<HWND>(window->winId());
		QVERIFY(hwnd != nullptr && IsWindow(hwnd));
		QVERIFY(SetWindowPos(
		    hwnd,
		    nullptr,
		    -10000,
		    -10000,
		    200,
		    100,
		    SWP_NOACTIVATE | SWP_NOZORDER
		));

		panel.setVisible(true);
		panel.setVisibleDirect(true);
		QTRY_COMPARE(g_fakePinnedHwnd, hwnd);
		QVERIFY(g_pinCalls >= 1);
		QVERIFY(GetParent(hwnd) == nullptr);

		// Hide the panel and simulate accessor-only recovery: the unresolved pin
		// must survive both hide and accessorReady so SetParent stays fenced while
		// unpinning is refused.
		panel.setVisible(false);
		panel.setVisibleDirect(false);
		QTRY_VERIFY(!IsWindowVisible(hwnd));
		emit desktops->accessorReady();
		QCoreApplication::processEvents();

		host->setDiscoveredHostForTesting(m_hostA);
		host->setEnabled(true);
		QCoreApplication::processEvents();
		QTRY_VERIFY(g_unpinCalls >= 1);
		QVERIFY(GetParent(hwnd) == nullptr);

		// Once unpinning succeeds (e.g. after provider recovery), the same HWND
		// transitions cleanly into desktop embedding.
		g_allowUnpin = true;
		emit desktops->accessorReady();
		QTRY_COMPARE(GetParent(hwnd), m_hostA);
		QCOMPARE(g_fakePinnedHwnd, nullptr);

		panel.deleteWindow();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}

	void reloadedPanelPreservesPinStateAndUnpinsBeforeEmbedding() {
		auto* primary = QGuiApplication::primaryScreen();
		QVERIFY(primary != nullptr);
		QuickshellScreenInfo screenInfo(nullptr, primary);

		auto* desktops = qs::windows::VirtualDesktops::instance();
		g_fakePinnedHwnd = nullptr;
		g_allowUnpin = false;
		g_pinCalls = 0;
		g_unpinCalls = 0;
		qs::windows::VirtualDesktops::Accessor fakeAccessor {};
		fakeAccessor.isPinnedWindow = &fakeIsPinnedWindow;
		fakeAccessor.pinWindow = &fakePinWindow;
		fakeAccessor.unPinWindow = &fakeUnpinWindow;
		desktops->installAccessorForTest(fakeAccessor);
		struct AccessorCleanup {
			qs::windows::VirtualDesktops* desktops;
			~AccessorCleanup() {
				g_fakePinnedHwnd = nullptr;
				g_allowUnpin = true;
				this->desktops->loseAccessorForTest();
			}
		} accessorCleanup {desktops};

		// Keep DesktopHost enabled (m_hostA from init()), and start with a Top-layer
		// panel that pins as a top-level window.
		qs::windows::WinPanelWindow oldPanel;
		oldPanel.setLayer(qs::windows::PanelLayer::Top);
		oldPanel.setExclusionMode(ExclusionMode::Ignore);
		oldPanel.setScreen(&screenInfo);
		oldPanel.createWindow();

		auto* window = oldPanel.backingWindow();
		QVERIFY(window != nullptr);
		auto* hwnd = reinterpret_cast<HWND>(window->winId());
		QVERIFY(hwnd != nullptr && IsWindow(hwnd));
		QVERIFY(SetWindowPos(
		    hwnd,
		    nullptr,
		    -10000,
		    -10000,
		    200,
		    100,
		    SWP_NOACTIVATE | SWP_NOZORDER
		));

		oldPanel.setVisible(true);
		oldPanel.setVisibleDirect(true);
		QTRY_COMPARE(g_fakePinnedHwnd, hwnd);
		QVERIFY(GetParent(hwnd) == nullptr);

		// Simulate a QML reload that reuses the same native HWND while switching
		// to a Background layer that wants desktop embedding. The reloaded panel
		// must inherit mayBePinnedHwnd and refuse SetParent until unpin succeeds.
		qs::windows::WinPanelWindow newPanel;
		newPanel.setLayer(qs::windows::PanelLayer::Background);
		newPanel.setExclusionMode(ExclusionMode::Ignore);
		newPanel.setScreen(&screenInfo);
		newPanel.setVisible(true);
		newPanel.reload(&oldPanel);

		QVERIFY(newPanel.backingWindow() != nullptr);
		QCOMPARE(reinterpret_cast<HWND>(newPanel.backingWindow()->winId()), hwnd);
		QTRY_VERIFY(g_unpinCalls >= 1);
		QVERIFY(GetParent(hwnd) == nullptr);

		g_allowUnpin = true;
		emit desktops->accessorReady();
		QTRY_COMPARE(GetParent(hwnd), m_hostA);
		QCOMPARE(g_fakePinnedHwnd, nullptr);

		newPanel.deleteWindow();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}

	void nativeAsyncPinAndDisconnectDrainRecoveryBeforeEmbedding() {
		auto* primary = QGuiApplication::primaryScreen();
		QVERIFY(primary != nullptr);
		QuickshellScreenInfo screenInfo(nullptr, primary);

		auto* desktops = qs::windows::VirtualDesktops::instance();
		auto* host = qs::windows::DesktopHost::instance();
		host->setEnabled(false);

		const auto prevHypr = qgetenv("CAELESTIA_WINDOWS_HYPR");
		const auto prevDispatch = qgetenv("CAELESTIA_WINDOWS_HYPR_DISPATCH");
		// Prevent StartupCoordinator's deferred VirtualDesktops::loadAccessor()
		// from calling loadNativeDesktopAccessor() a second time mid-test.
		qunsetenv("CAELESTIA_WINDOWS_HYPR");
		qunsetenv("CAELESTIA_WINDOWS_HYPR_DISPATCH");

		erm625::Fixture oldFixture;
		erm625::Fixture freshFixture;
		erm625::FakePinnedApps pinned;
		pinned.expectedView =
		    reinterpret_cast<qs::windows::native_desktop::ApplicationView*>(&oldFixture.views.view);
		std::shared_ptr<erm625::BlockingCall> activeBlock;
		bool useFreshFixture = false;
		int acquisitions = 0;
		qs::windows::VirtualDesktops::Accessor nativeAccessor {};

		struct NativeCleanup {
			qs::windows::VirtualDesktops* desktops;
			erm625::FakePinnedApps* pinned;
			std::shared_ptr<erm625::BlockingCall>* block;
			QByteArray prevHypr;
			QByteArray prevDispatch;
			~NativeCleanup() {
				if (*this->block) (*this->block)->release();
				qs::windows::setNativeDesktopFactoryForTest({});
				QElapsedTimer timer;
				timer.start();
				while (qs::windows::nativeDesktopWorkerCountForTest() > 0 && timer.elapsed() < 2000) {
					QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
					QThread::msleep(1);
				}
				this->pinned->beforeQuery = {};
				this->pinned->beforePin = {};
				this->pinned->beforeUnpin = {};
				this->desktops->loseAccessorForTest();
				if (this->prevHypr.isNull()) qunsetenv("CAELESTIA_WINDOWS_HYPR");
				else qputenv("CAELESTIA_WINDOWS_HYPR", this->prevHypr);
				if (this->prevDispatch.isNull()) qunsetenv("CAELESTIA_WINDOWS_HYPR_DISPATCH");
				else qputenv("CAELESTIA_WINDOWS_HYPR_DISPATCH", this->prevDispatch);
			}
		} nativeCleanup {desktops, &pinned, &activeBlock, prevHypr, prevDispatch};

		qs::windows::setNativeDesktopFactoryForTest(
		    [&](qs::windows::native_desktop::DesktopManager** m,
		        qs::windows::native_desktop::ViewCollection** v,
		        IVirtualDesktopManager** p) {
			    ++acquisitions;
			    return (useFreshFixture ? freshFixture : oldFixture).acquire(m, v, p);
		    }
		);
		qs::windows::setNativeDesktopRecoveryAgeForTest([] { return qint64(30000 - 40); });
		auto installNative = [&] {
			desktops->installAccessorForTest(nativeAccessor);
			qs::windows::NativeDesktopFacadeTestHook::selectNative(*desktops);
		};
		auto onNativeRecovered = [&] {
			installNative();
			emit desktops->accessorReady();
		};
		if (!qs::windows::loadNativeDesktopAccessor(nativeAccessor, desktops, onNativeRecovered)) {
			QSKIP("Native desktop accessor requires Windows 11 24H2+ build.");
		}
		qs::windows::NativeDesktopFacadeTestHook::observeSnapshots(*desktops);
		installNative();
		qs::windows::setNativeDesktopPinnedAppsForTest(&pinned);
		QCoreApplication::processEvents();

		// Part A: PinView returns RPC_E_SERVER_DIED (unknown mutation outcome).
		// Hide the panel so pinRetryTimer stops and pinState.pinned stays false;
		// mayBePinnedHwnd alone must still fence SetParent until explicit UnpinView.
		{
			pinned.pinned = FALSE;
			pinned.pinResult = RPC_E_SERVER_DIED;
			pinned.unpinResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);
			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(pinDone.size() >= 1);
			QCOMPARE(
			    pinDone.at(0).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Disconnected
			);
			QCOMPARE(pinDone.at(0).at(3).toBool(), true);
			panel.setVisible(false);
			panel.setVisibleDirect(false);
			QTRY_VERIFY(!IsWindowVisible(hwnd));
			QCOMPARE(pinned.pins, 1);

			// Simulate Explorer having applied the pin before the reply was lost.
			pinned.pinned = TRUE;
			pinned.pinResult = S_OK;
			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforeUnpin = [blocked = activeBlock] { (*blocked)(); };

			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);
			QVERIFY(GetParent(hwnd) == nullptr);

			activeBlock->release();
			QTRY_COMPARE(GetParent(hwnd), m_hostA);
			pinned.beforeUnpin = {};
			activeBlock.reset();
			QCOMPARE(pinned.pinned, FALSE);
			QVERIFY(pinned.unpins >= 1);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part B: Explicit UnpinView is blocked when desktop disconnects; desktop
		// recovers BEFORE the old UnpinView drains and returns RPC_E_DISCONNECTED.
		// The late disconnect drain must retry on the recovered worker instead of
		// permanently blocking embedding via embedUnpinFailedHwnd.
		{
			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);
			QSignalSpy pinDrained(desktops, &qs::windows::VirtualDesktops::ownWindowPinDrained);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(pinDone.size() >= 1);
			QCOMPARE(
			    pinDone.at(0).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QCOMPARE(pinned.pinned, TRUE);
			QVERIFY(GetParent(hwnd) == nullptr);

			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforeUnpin = [blocked = activeBlock] { (*blocked)(); };
			pinned.unpinResult = RPC_E_DISCONNECTED;

			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);
			QVERIFY(GetParent(hwnd) == nullptr);

			// Disconnect desktop while UnpinView is blocked, then let desktop recover
			// BEFORE releasing the blocked old UnpinView.
			const auto acquisitionsBeforeDisconnect = acquisitions;
			useFreshFixture = true;
			qs::windows::invalidateNativeDesktopAccessor(RPC_E_DISCONNECTED);
			QTRY_VERIFY(acquisitions > acquisitionsBeforeDisconnect);
			QTRY_VERIFY(desktops->accessorLoaded());
			QVERIFY(desktops->isOwnWindowPinActive(hwnd));
			QVERIFY(GetParent(hwnd) == nullptr);

			// Let the old UnpinView return RPC_E_DISCONNECTED (still pinned), wait
			// until the worker finishes draining before clearing beforeUnpin, then
			// switch FakePinnedApps to the recovered fixture's view and S_OK before
			// the bounded retryEmbedding() timer fires.
			activeBlock->release();
			QTRY_VERIFY(pinDrained.size() >= 1);
			pinned.beforeUnpin = {};
			activeBlock.reset();
			QCOMPARE(pinDrained.at(0).at(5).toBool(), true);
			QVERIFY(GetParent(hwnd) == nullptr);

			pinned.expectedView = reinterpret_cast<qs::windows::native_desktop::ApplicationView*>(
			    &freshFixture.views.view
			);
			pinned.unpinResult = S_OK;
			QTRY_COMPARE_WITH_TIMEOUT(GetParent(hwnd), m_hostA, 3000);
			QCOMPARE(pinned.pinned, FALSE);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part C: Canceling an in-flight unpin by switching layer to Top while
		// IsViewPinned is blocked must not latch embedUnpinFailedHwnd when the
		// worker skips UnpinView and reports Pinned; switching back to Background
		// must unpin and embed cleanly.
		{
			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(pinDone.size() >= 1);
			QCOMPARE(
			    pinDone.at(0).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QCOMPARE(pinned.pinned, TRUE);
			QVERIFY(GetParent(hwnd) == nullptr);

			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforeQuery = [blocked = activeBlock] { (*blocked)(); };

			const auto doneBeforeCancel = pinDone.size();
			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);
			QVERIFY(GetParent(hwnd) == nullptr);

			// Stop wanting embedding while IsViewPinned is still blocked.
			panel.setLayer(qs::windows::PanelLayer::Top);
			QCoreApplication::processEvents();

			activeBlock->release();
			QTRY_VERIFY(pinDone.size() >= doneBeforeCancel + 1);
			pinned.beforeQuery = {};
			activeBlock.reset();
			QCOMPARE(
			    pinDone.at(doneBeforeCancel).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QCOMPARE(pinned.unpins, 0);
			QVERIFY(GetParent(hwnd) == nullptr);

			// Switching back to Background must not be blocked by embedUnpinFailedHwnd.
			panel.setLayer(qs::windows::PanelLayer::Background);
			QTRY_COMPARE(GetParent(hwnd), m_hostA);
			QCOMPARE(pinned.pinned, FALSE);
			QVERIFY(pinned.unpins >= 1);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part D: A late stillPinned=true drain from before an Explorer process
		// restart keeps the HWND fenced until the old worker settles, but must not
		// restore pinned or mayBePinned state in the new Explorer session.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(1001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = E_ACCESSDENIED;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDrained(desktops, &qs::windows::VirtualDesktops::ownWindowPinDrained);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			// Seed initial Explorer identity, then block PinView in the old session.
			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 1001, 1001));
			QCoreApplication::processEvents();

			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforePin = [blocked = activeBlock] { (*blocked)(); };

			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);

			// Retire the old worker into drainingPins while blocked in PinView and
			// simulate an Explorer process restart before the old worker drains.
			const auto acquisitionsBeforeRestart = acquisitions;
			simulatedExplorerPid->store(2002);
			qs::windows::invalidateNativeDesktopAccessor(RPC_E_DISCONNECTED);
			QVERIFY(panel.recoverExplorerForTesting(m_hostB, 2002, 2002));
			QTRY_VERIFY(acquisitions > acquisitionsBeforeRestart);
			QTRY_VERIFY(desktops->accessorLoaded());
			QVERIFY(desktops->isOwnWindowPinActive(hwnd));

			// Hide so the recovered session does not immediately submit a fresh pin,
			// then let the pre-restart PinView finish and drain with stillPinned=true.
			panel.setVisible(false);
			panel.setVisibleDirect(false);
			QTRY_VERIFY(!IsWindowVisible(hwnd));

			activeBlock->release();
			QTRY_VERIFY(pinDrained.size() >= 1);
			pinned.beforePin = {};
			activeBlock.reset();
			QCOMPARE(pinDrained.at(0).at(2).toBool(), true);
			QCOMPARE(pinDrained.at(0).at(6).toUInt(), quint32(1001));
			QVERIFY(!desktops->isOwnWindowPinActive(hwnd));
			QVERIFY(!panel.isPinnedHereForTesting());
			QVERIFY(!panel.mayBePinnedHereForTesting());

			// Because the dead Explorer session's stillPinned result was rejected,
			// embedding into the new Explorer proceeds without calling UnpinView.
			const auto unpinsBeforeEmbed = pinned.unpins;
			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(GetParent(hwnd), m_hostA);
			QCOMPARE(pinned.unpins, unpinsBeforeEmbed);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part E: Reverse recovery order across an Explorer process restart.
		// VirtualDesktops recovers against the new Explorer PID (2002) and pins
		// (or has an in-flight pin for) the panel BEFORE delayed recoverExplorer(2002)
		// runs. Delayed restart handling must preserve the new Explorer's pin/fence
		// so embedding still calls UnpinView before SetParent.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(1001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			// E1: New Explorer (2002) completes PinView BEFORE delayed recoverExplorer(2002).
			{
				pinned.pinned = FALSE;
				pinned.pinResult = S_OK;
				pinned.unpinResult = S_OK;
				pinned.pins = 0;
				pinned.unpins = 0;

				QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

				qs::windows::WinPanelWindow panel;
				panel.setLayer(qs::windows::PanelLayer::Background);
				panel.setExclusionMode(ExclusionMode::Ignore);
				panel.setScreen(&screenInfo);
				panel.createWindow();

				auto* window = panel.backingWindow();
				QVERIFY(window != nullptr);
				auto* hwnd = reinterpret_cast<HWND>(window->winId());
				QVERIFY(hwnd != nullptr && IsWindow(hwnd));
				QVERIFY(SetWindowPos(
				    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
				));

				// Pin in old Explorer session (1001).
				QVERIFY(panel.recoverExplorerForTesting(m_hostA, 1001, 1001));
				QCoreApplication::processEvents();
				panel.setVisible(true);
				panel.setVisibleDirect(true);
				QTRY_VERIFY(pinDone.size() >= 1);
				QCOMPARE(pinDone.at(0).at(4).toUInt(), quint32(1001));
				QVERIFY(panel.isPinnedHereForTesting());

				// Explorer restarts to 2002; VirtualDesktops recovers and pins against
				// 2002 BEFORE recoverExplorer(2002) reaches the panel.
				pinned.pinned = FALSE;
				const auto doneBeforeRestart = pinDone.size();
				const auto acquisitionsBeforeRestart = acquisitions;
				simulatedExplorerPid->store(2002);
				qs::windows::invalidateNativeDesktopAccessor(RPC_E_DISCONNECTED);
				QTRY_VERIFY(acquisitions > acquisitionsBeforeRestart);
				QTRY_VERIFY(desktops->accessorLoaded());
				QTRY_VERIFY(pinDone.size() > doneBeforeRestart);
				QCOMPARE(
				    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
				    qs::windows::OwnWindowPin::Pinned
				);
				QCOMPARE(pinDone.last().at(4).toUInt(), quint32(2002));
				QVERIFY(panel.isPinnedHereForTesting());
				QVERIFY(panel.mayBePinnedHereForTesting());
				QCOMPARE(pinned.pinned, TRUE);

				// Now delayed recoverExplorer(2002) arrives: it must NOT discard the
				// valid 2002 pin or mayBePinned fence.
				QVERIFY(panel.recoverExplorerForTesting(m_hostB, 2002, 2002));
				QCoreApplication::processEvents();
				QVERIFY(panel.isPinnedHereForTesting());
				QVERIFY(panel.mayBePinnedHereForTesting());

				// Enabling desktop embedding must call UnpinView before SetParent.
				const auto unpinsBeforeEmbed = pinned.unpins;
				host->setDiscoveredHostForTesting(m_hostA);
				host->setEnabled(true);
				QTRY_COMPARE(GetParent(hwnd), m_hostA);
				QVERIFY(pinned.unpins > unpinsBeforeEmbed);
				QCOMPARE(pinned.pinned, FALSE);

				panel.deleteWindow();
				QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
				host->setEnabled(false);
				QCoreApplication::processEvents();
			}

			// E2: New Explorer (2002) has an in-flight PinView when delayed
			// recoverExplorer(2002) arrives; the 2002 pin must not be cancelled or
			// discarded, and embedding afterwards must unpin before SetParent.
			{
				simulatedExplorerPid->store(1001);
				qs::windows::setNativeDesktopExplorerPidForTest(
				    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
				);
				pinned.pinned = FALSE;
				pinned.pinResult = S_OK;
				pinned.unpinResult = S_OK;
				pinned.pins = 0;
				pinned.unpins = 0;

				QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

				qs::windows::WinPanelWindow panel;
				panel.setLayer(qs::windows::PanelLayer::Background);
				panel.setExclusionMode(ExclusionMode::Ignore);
				panel.setScreen(&screenInfo);
				panel.createWindow();

				auto* window = panel.backingWindow();
				QVERIFY(window != nullptr);
				auto* hwnd = reinterpret_cast<HWND>(window->winId());
				QVERIFY(hwnd != nullptr && IsWindow(hwnd));
				QVERIFY(SetWindowPos(
				    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
				));

				QVERIFY(panel.recoverExplorerForTesting(m_hostA, 1001, 1001));
				QCoreApplication::processEvents();
				panel.setVisible(true);
				panel.setVisibleDirect(true);
				QTRY_VERIFY(pinDone.size() >= 1);
				QCOMPARE(pinDone.at(0).at(4).toUInt(), quint32(1001));

				// Explorer restarts to 2002; VirtualDesktops recovers first and enters
				// PinView for 2002 while recoverExplorer(2002) is still delayed.
				pinned.pinned = FALSE;
				activeBlock = std::make_shared<erm625::BlockingCall>();
				pinned.beforePin = [blocked = activeBlock] { (*blocked)(); };
				const auto doneBeforeRestart = pinDone.size();
				const auto acquisitionsBeforeRestart = acquisitions;
				simulatedExplorerPid->store(2002);
				qs::windows::invalidateNativeDesktopAccessor(RPC_E_DISCONNECTED);
				QTRY_VERIFY(acquisitions > acquisitionsBeforeRestart);
				QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);

				// Delayed recoverExplorer(2002) arrives while the 2002 pin is in flight.
				QVERIFY(panel.recoverExplorerForTesting(m_hostB, 2002, 2002));
				QCoreApplication::processEvents();

				activeBlock->release();
				QTRY_VERIFY(pinDone.size() > doneBeforeRestart);
				pinned.beforePin = {};
				activeBlock.reset();
				QCOMPARE(
				    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
				    qs::windows::OwnWindowPin::Pinned
				);
				QCOMPARE(pinDone.last().at(4).toUInt(), quint32(2002));
				QCOMPARE(pinned.unpins, 0);
				QVERIFY(panel.isPinnedHereForTesting());
				QVERIFY(panel.mayBePinnedHereForTesting());

				const auto unpinsBeforeEmbed = pinned.unpins;
				host->setDiscoveredHostForTesting(m_hostA);
				host->setEnabled(true);
				QTRY_COMPARE(GetParent(hwnd), m_hostA);
				QVERIFY(pinned.unpins > unpinsBeforeEmbed);
				QCOMPARE(pinned.pinned, FALSE);

				panel.deleteWindow();
				QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
				host->setEnabled(false);
				QCoreApplication::processEvents();
			}
		}

		// Part F: Same-Explorer stale Unpinned completion or confirmedUnpinned drain
		// with an older requestId must not clear mayBePinned/pinned state established
		// by a newer PinView request.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(3003);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 3003, 3003));
			QCoreApplication::processEvents();
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(pinDone.size() >= 1);
			const auto newerReqId = pinDone.last().at(0).toULongLong();
			QVERIFY(newerReqId >= 2);
			QVERIFY(panel.isPinnedHereForTesting());
			QVERIFY(panel.mayBePinnedHereForTesting());

			// Deliver an older same-session Unpinned completion and confirmedUnpinned drain.
			panel.simulatePinCompletionForTesting(
			    newerReqId - 1,
			    qs::windows::OwnWindowPin::Unpinned,
			    false,
			    3003
			);
			panel.simulatePinDrainForTesting(
			    newerReqId - 1,
			    false,
			    false,
			    true,
			    false,
			    3003
			);
			QVERIFY(panel.isPinnedHereForTesting());
			QVERIFY(panel.mayBePinnedHereForTesting());

			// Embedding must still unpin before SetParent.
			const auto unpinsBeforeEmbed = pinned.unpins;
			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(GetParent(hwnd), m_hostA);
			QVERIFY(pinned.unpins > unpinsBeforeEmbed);
			QCOMPARE(pinned.pinned, FALSE);

			// Reverse sequence: after a newer confirmed Unpinned, an older Pinned
			// completion or stillPinned drain must not restore the pin fence.
			host->setEnabled(false);
			QTRY_VERIFY(GetParent(hwnd) == nullptr);
			panel.setVisible(false);
			panel.setVisibleDirect(false);
			QTRY_VERIFY(!IsWindowVisible(hwnd));
			const auto unpinWatermarkReqId = newerReqId + 10;
			panel.simulatePinCompletionForTesting(
			    unpinWatermarkReqId,
			    qs::windows::OwnWindowPin::Unpinned,
			    false,
			    3003
			);
			QVERIFY(!panel.isPinnedHereForTesting());
			QVERIFY(!panel.mayBePinnedHereForTesting());

			panel.simulatePinCompletionForTesting(
			    unpinWatermarkReqId - 1,
			    qs::windows::OwnWindowPin::Pinned,
			    true,
			    3003
			);
			panel.simulatePinDrainForTesting(
			    unpinWatermarkReqId - 1,
			    true,
			    true,
			    false,
			    false,
			    3003
			);
			QVERIFY(!panel.isPinnedHereForTesting());
			QVERIFY(!panel.mayBePinnedHereForTesting());

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part G: Intermediate Explorer session B that the panel never observed
		// via recoverExplorer (panel saw A, submitted pin to B, hid before C):
		// a late stillPinned drain from B must be rejected against live C so
		// reopening the panel still pins in C instead of skipping PinView.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(5001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = E_ACCESSDENIED;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);
			QSignalSpy pinDrained(desktops, &qs::windows::VirtualDesktops::ownWindowPinDrained);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			// Panel observes Explorer A (5001).
			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 5001, 5001));
			QCoreApplication::processEvents();

			// Explorer restarts to B (5002) without recoverExplorer(B) reaching panel;
			// panel shows and starts PinView against B, then hides (clearing
			// inFlightPinExplorerPid) before Explorer restarts again to C (5003).
			simulatedExplorerPid->store(5002);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforePin = [blocked = activeBlock] { (*blocked)(); };

			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);

			panel.setVisible(false);
			panel.setVisibleDirect(false);
			QTRY_VERIFY(!IsWindowVisible(hwnd));

			// Explorer restarts to C (5003); panel recovers straight from A -> C
			// without ever recording B in retiredExplorerPids.
			const auto acquisitionsBeforeC = acquisitions;
			simulatedExplorerPid->store(5003);
			qs::windows::invalidateNativeDesktopAccessor(RPC_E_DISCONNECTED);
			QVERIFY(panel.recoverExplorerForTesting(m_hostB, 5003, 5003));
			QTRY_VERIFY(acquisitions > acquisitionsBeforeC);
			QTRY_VERIFY(desktops->accessorLoaded());

			// Let B's PinView drain with stillPinned=true (unobserved session B).
			activeBlock->release();
			QTRY_VERIFY(pinDrained.size() >= 1);
			pinned.beforePin = {};
			activeBlock.reset();
			QCOMPARE(pinDrained.last().at(2).toBool(), true);
			QCOMPARE(pinDrained.last().at(6).toUInt(), quint32(5002));
			QVERIFY(!panel.isPinnedHereForTesting());
			QVERIFY(!panel.mayBePinnedHereForTesting());

			// Reopening the panel in C must submit a fresh PinView for C (5003).
			pinned.pinned = FALSE;
			const auto doneBeforeReopen = pinDone.size();
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(pinDone.size() > doneBeforeReopen);
			QCOMPARE(
			    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QCOMPARE(pinDone.last().at(4).toUInt(), quint32(5003));
			QVERIFY(panel.isPinnedHereForTesting());
			QVERIFY(panel.mayBePinnedHereForTesting());

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part H: If the accessor recovers in live Explorer B before the panel
		// observes the restart, B's confirmed Unpinned reply must clear the
		// obsolete pin fence from A so embedding proceeds without looping.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(6001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 6001, 6001));
			QCoreApplication::processEvents();
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(pinDone.size() >= 1);
			QCOMPARE(pinDone.last().at(4).toUInt(), quint32(6001));
			QVERIFY(panel.mayBePinnedHereForTesting());

			// Hide panel so accessor recovery in Explorer B (6002) does not
			// immediately repin, and do NOT call recoverExplorer(6002) yet.
			panel.setVisible(false);
			panel.setVisibleDirect(false);
			QTRY_VERIFY(!IsWindowVisible(hwnd));
			pinned.pinned = FALSE;
			simulatedExplorerPid->store(6002);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);

			// Enabling embedding submits unpin against live Explorer B (6002).
			// B's confirmed Unpinned must clear A's obsolete fence and embed in one pass.
			const auto doneBeforeEmbed = pinDone.size();
			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(GetParent(hwnd), m_hostA);
			QVERIFY(!panel.mayBePinnedHereForTesting());
			QCOMPARE(pinDone.size(), doneBeforeEmbed + 1);
			QCOMPARE(
			    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Unpinned
			);
			QCOMPARE(pinDone.last().at(4).toUInt(), quint32(6002));

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part I: If Explorer changes mid-COM-acquisition and the in-flight pin
		// returns Disconnected tagged with the old PID (before recoverExplorer),
		// the visible panel must retry with bounded backoff and pin in the live
		// Explorer session instead of stalling unpinned.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(7001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Top);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 7001, 7001));
			QCoreApplication::processEvents();

			// Force a mid-acquisition disconnect tagged with old PID 7001 while live
			// Explorer has already moved to 7002.
			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforeQuery = [blocked = activeBlock] { (*blocked)(); };
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);

			// Keep beforeQuery blocked only for the first request, then let the
			// retried request succeed in 7002.
			pinned.beforeQuery = {};
			pinned.queryResult = RPC_E_DISCONNECTED;
			simulatedExplorerPid->store(7002);
			activeBlock->release();
			QTRY_VERIFY(pinDone.size() >= 1);
			activeBlock.reset();
			QCOMPARE(
			    pinDone.at(0).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Disconnected
			);
			QCOMPARE(pinDone.at(0).at(4).toUInt(), quint32(7001));

			// Now allow the bounded retry to succeed against live Explorer 7002
			// without any separate recoverExplorer callback.
			pinned.queryResult = S_OK;
			QTRY_VERIFY_WITH_TIMEOUT(panel.isPinnedHereForTesting(), 3000);
			QCOMPARE(pinDone.last().at(4).toUInt(), quint32(7002));
			QCOMPARE(pinned.pinned, TRUE);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part J: If Explorer A returns a successful Pinned reply just as
		// live Explorer B takes over (without an RPC disconnect), the stale A
		// worker must retire on PID mismatch so the panel's retry spawns a fresh
		// worker for B and pins in B instead of exhausting retries against A.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(8001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.queryResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Top);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 8001, 8001));
			QCoreApplication::processEvents();

			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforePin = [blocked = activeBlock] { (*blocked)(); };
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);

			// Live Explorer moves to 8002 while A's PinView is completing with S_OK.
			pinned.beforePin = {};
			simulatedExplorerPid->store(8002);
			activeBlock->release();
			QTRY_VERIFY(pinDone.size() >= 1);
			activeBlock.reset();
			QCOMPARE(
			    pinDone.at(0).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QCOMPARE(pinDone.at(0).at(4).toUInt(), quint32(8001));

			// The stale 8001 reply is rejected and 8001's pin worker retires, so
			// the bounded retry spawns a fresh worker for 8002 and pins cleanly.
			pinned.pinned = FALSE;
			QTRY_VERIFY_WITH_TIMEOUT(panel.isPinnedHereForTesting(), 3000);
			QVERIFY(pinDone.size() >= 2);
			QCOMPARE(
			    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QCOMPARE(pinDone.last().at(4).toUInt(), quint32(8002));
			QCOMPARE(pinned.pinned, TRUE);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}

		// Part K: Hiding a panel while a fallback follow move is pending clears
		// its pendingFollowMove state so reopening on the same desktop retries
		// instead of being suppressed by a stale sequence whose failure arrived
		// while the panel was removed from WinPanelStack.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(9001);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			// Force pinning refused so the panel falls back to following current desktop,
			// and place the window on desktop two while current is desktop one.
			pinned.pinned = FALSE;
			pinned.pinResult = E_ACCESSDENIED;
			pinned.unpinResult = S_OK;
			pinned.queryResult = S_OK;
			freshFixture.manager.current = &freshFixture.one;
			freshFixture.publicApi.desktopId = freshFixture.two.id;
			freshFixture.publicApi.onCurrent = FALSE;
			auto moveBlock = std::make_shared<erm625::BlockingCall>();
			freshFixture.publicApi.beforeMove = [moveBlock] { (*moveBlock)(); };
			freshFixture.manager.moveResult = E_ACCESSDENIED;
			freshFixture.publicApi.moveResult = E_ACCESSDENIED;
			struct ResetMoveFixture {
				erm625::Fixture* fixture;
				std::shared_ptr<erm625::BlockingCall>* block;
				~ResetMoveFixture() {
					if (*this->block) (*this->block)->release();
					this->fixture->manager.beforeMove = {};
					this->fixture->publicApi.beforeMove = {};
					this->fixture->manager.moveResult = S_OK;
					this->fixture->publicApi.moveResult = S_OK;
					this->fixture->publicApi.desktopId = this->fixture->one.id;
					this->fixture->publicApi.onCurrent = TRUE;
				}
			} resetMove {&freshFixture, &moveBlock};

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Top);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 9001, 9001));
			QCoreApplication::processEvents();

			const auto movesBefore = freshFixture.publicApi.moves;
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_COMPARE(WaitForSingleObject(moveBlock->entered, 0), WAIT_OBJECT_0);
			QCOMPARE(freshFixture.publicApi.moves, movesBefore);

			// Hide panel while the move is pending, then let the move fail while hidden.
			panel.setVisible(false);
			panel.setVisibleDirect(false);
			QTRY_VERIFY(!IsWindowVisible(hwnd));

			QSignalSpy moveDone(desktops, &qs::windows::VirtualDesktops::moveWindowCompleted);
			freshFixture.publicApi.beforeMove = {};
			moveBlock->release();
			QTRY_VERIFY(moveDone.size() >= 1);
			moveBlock.reset();
			QCOMPARE(moveDone.last().at(2).toBool(), false);
			QVERIFY(freshFixture.publicApi.moves > movesBefore);

			// Reopen on the same desktop with move succeeding: must issue a new move
			// instead of being suppressed by the stale hidden sequence.
			const auto movesBeforeReopen = freshFixture.publicApi.moves;
			freshFixture.manager.moveResult = S_OK;
			freshFixture.publicApi.moveResult = S_OK;
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY_WITH_TIMEOUT(freshFixture.publicApi.moves > movesBeforeReopen, 3000);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
			QTRY_VERIFY(!desktops->isOwnWindowPinActive(hwnd));
		}

		// Part L: If the desktop host disappears while an unpin is pending (so
		// wantsEmbedding() stays true but no host parent remains), completing
		// the unpin must restore the panel's top-level pin instead of leaving
		// it unpinned without an embedding target.
		{
			auto simulatedExplorerPid = std::make_shared<std::atomic<DWORD>>(9101);
			qs::windows::setNativeDesktopExplorerPidForTest(
			    [simulatedExplorerPid] { return simulatedExplorerPid->load(); }
			);
			struct ResetPid {
				~ResetPid() { qs::windows::setNativeDesktopExplorerPidForTest({}); }
			} resetPid;

			pinned.pinned = FALSE;
			pinned.pinResult = S_OK;
			pinned.unpinResult = S_OK;
			pinned.queryResult = S_OK;
			pinned.pins = 0;
			pinned.unpins = 0;

			QSignalSpy pinDone(desktops, &qs::windows::VirtualDesktops::ownWindowPinCompleted);

			qs::windows::WinPanelWindow panel;
			panel.setLayer(qs::windows::PanelLayer::Background);
			panel.setExclusionMode(ExclusionMode::Ignore);
			panel.setScreen(&screenInfo);
			panel.createWindow();

			auto* window = panel.backingWindow();
			QVERIFY(window != nullptr);
			auto* hwnd = reinterpret_cast<HWND>(window->winId());
			QVERIFY(hwnd != nullptr && IsWindow(hwnd));
			QVERIFY(SetWindowPos(
			    hwnd, nullptr, -10000, -10000, 200, 100, SWP_NOACTIVATE | SWP_NOZORDER
			));

			QVERIFY(panel.recoverExplorerForTesting(m_hostA, 9101, 9101));
			QCoreApplication::processEvents();
			panel.setVisible(true);
			panel.setVisibleDirect(true);
			QTRY_VERIFY(panel.isPinnedHereForTesting());

			// Start embedding so UnpinView enters beforeUnpin, then drop the host
			// parent window while keeping DesktopHost enabled (wantsEmbedding()==true).
			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforeUnpin = [blocked = activeBlock] { (*blocked)(); };
			host->setDiscoveredHostForTesting(m_hostA);
			host->setEnabled(true);
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);
			QVERIFY(GetParent(hwnd) == nullptr);

			host->clearHostForTesting();
			QCoreApplication::processEvents();
			QVERIFY(host->enabled());
			QVERIFY(host->parentWindow() == nullptr);

			// Complete UnpinView: panel remains top-level and must re-pin.
			const auto doneBeforeUnpinFinish = pinDone.size();
			pinned.beforeUnpin = {};
			activeBlock->release();
			QTRY_VERIFY_WITH_TIMEOUT(
			    pinDone.size() >= doneBeforeUnpinFinish + 2 && panel.isPinnedHereForTesting(),
			    3000
			);
			activeBlock.reset();
			QCOMPARE(
			    pinDone.at(doneBeforeUnpinFinish).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Unpinned
			);
			QCOMPARE(
			    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QVERIFY(GetParent(hwnd) == nullptr);
			QCOMPARE(pinned.pinned, TRUE);

			// L2: Explorer applies UnpinView (pinned becomes FALSE) and loses the
			// reply (RPC_E_DISCONNECTED -> mayBePinned=true) while the desktop host
			// disappears. Clearing the stale confirmed pin lets stickToAllDesktops()
			// re-verify and re-pin the panel cleanly.
			activeBlock = std::make_shared<erm625::BlockingCall>();
			pinned.beforeUnpin = [blocked = activeBlock, &pinned] {
				(*blocked)();
				pinned.pinned = FALSE;
			};
			pinned.unpinResult = RPC_E_DISCONNECTED;
			host->setDiscoveredHostForTesting(m_hostA);
			host->refreshForTesting();
			QTRY_COMPARE(WaitForSingleObject(activeBlock->entered, 0), WAIT_OBJECT_0);
			QVERIFY(GetParent(hwnd) == nullptr);

			host->clearHostForTesting();
			QCoreApplication::processEvents();
			QVERIFY(host->enabled());
			QVERIFY(host->parentWindow() == nullptr);

			const auto doneBeforeLostUnpin = pinDone.size();
			pinned.beforeUnpin = {};
			activeBlock->release();
			QTRY_VERIFY_WITH_TIMEOUT(
			    pinDone.size() >= doneBeforeLostUnpin + 2 && panel.isPinnedHereForTesting(),
			    3000
			);
			activeBlock.reset();
			pinned.unpinResult = S_OK;
			QCOMPARE(
			    pinDone.at(doneBeforeLostUnpin).at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Disconnected
			);
			QCOMPARE(pinDone.at(doneBeforeLostUnpin).at(3).toBool(), true);
			QCOMPARE(
			    pinDone.last().at(2).value<qs::windows::OwnWindowPin>(),
			    qs::windows::OwnWindowPin::Pinned
			);
			QVERIFY(GetParent(hwnd) == nullptr);
			QCOMPARE(pinned.pinned, TRUE);

			panel.deleteWindow();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			host->setEnabled(false);
			QCoreApplication::processEvents();
		}
	}

private:
	static bool needsHosts() {
		return qstrcmp(QTest::currentTestFunction(), "excludeFromCaptureFollowsTheProperty") != 0;
	}

	static DWORD affinityOf(HWND hwnd) {
		DWORD affinity = 0xFFFFFFFF;
		GetWindowDisplayAffinity(hwnd, &affinity);
		return affinity;
	}

	HWND m_hostA = nullptr;
	HWND m_hostB = nullptr;
};

int main(int argc, char** argv) {
	QGuiApplication app(argc, argv);
	TestPanelHiddenReembed test;
	return QTest::qExec(&test, argc, argv);
}

#include "panel_hidden_reembed_test.moc"
