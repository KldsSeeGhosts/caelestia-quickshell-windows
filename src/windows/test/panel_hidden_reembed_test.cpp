// #214 P2: WinPanelWindow keeps its QQuickWindow and HWND when the QML window
// is hidden, because deleteOnInvisible is false. A later host replacement
// re-embeds that still-hidden window. Re-embedding must not reveal it.
//
// This fixture instantiates the production WinPanelWindow, so it needs the
// Windows QPA and a real HWND. desktop_zorder_test cannot catch this path: its
// raw-HWND callback never creates a panel.

#include <qguiapplication.h>
#include <qscreen.h>
#include <qsignalspy.h>
#include <qtest.h>
#include <qt_windows.h>

#include "desktop_host.hpp"
#include "panel_window.hpp"

namespace {

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
