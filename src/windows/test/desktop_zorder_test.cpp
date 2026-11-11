// The desktop handback from #220 and the visibility rule from #214, against
// temporary windows instead of the live Progman/WorkerW. While a foreign
// wallpaper window is visible inside the host the shell Background belongs
// below it. Hiding that window without destroying it must release the
// Background to the top of the host, and showing it again must pin the
// Background below it once more. Destruction hands the desktop back the same
// way.
//
// Three processes, like the real desktop: this test process is the shell, a
// copy of the executable owns the host window (Explorer's WorkerW), and a
// second copy owns the wallpaper window and restacks it from its own thread
// on request. The production hooks are therefore scoped to the host's process
// and thread, which cannot see what the wallpaper process does to its own
// window. Nothing here touches Explorer's desktop.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <qbytearray.h>
#include <qelapsedtimer.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qprocess.h>
#include <qsignalspy.h>
#include <qstring.h>
#include <qtest.h>
#include <qt_windows.h>

#include "appbar.hpp"
#include "desktop_host.hpp"

namespace qs::windows {

// The desktop host only needs the AppBar broadcast id. Linking the real
// appbar would bring in the panel stack for no benefit.
UINT WinAppBar::taskbarCreatedMessage() {
	static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
	return message;
}

} // namespace qs::windows

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace {

constexpr const wchar_t* HOST_CLASS = L"CaelestiaDesktopZorderTestHost";
constexpr const wchar_t* CHILD_CLASS = L"CaelestiaDesktopZorderTestChild";
constexpr const wchar_t* HOLDER_CLASS = L"CaelestiaDesktopZorderTestHolder";
constexpr const wchar_t* FOREIGN_CLASS = L"CaelestiaDesktopZorderTestForeign";

constexpr UINT WM_WALLPAPER_COMMAND = WM_APP + 1;
constexpr WPARAM CMD_BOTTOM = 1;
constexpr WPARAM CMD_TOP = 2;
constexpr WPARAM CMD_REPLACE = 3;
constexpr WPARAM CMD_STOP = 4;
constexpr WPARAM CMD_PLAY = 5;

// Only the long-lived owner windows end their process when destroyed; a
// wallpaper child that is replaced must not.
LRESULT CALLBACK ownerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

LRESULT CALLBACK plainProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

void registerTestClass(const wchar_t* name, WNDPROC proc) {
	WNDCLASSW cls {};
	cls.lpfnWndProc = proc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = name;
	RegisterClassW(&cls);
}

HWND createWindow(const wchar_t* cls, HWND parent, DWORD style) {
	return CreateWindowExW(
	    0,
	    cls,
	    L"",
	    style,
	    -10000,
	    -10000,
	    100,
	    100,
	    parent,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
}

// True when `window` is below `other` in the parent's child z-order.
bool isBelow(HWND parent, HWND window, HWND other) {
	for (auto* child = GetWindow(parent, GW_CHILD); child != nullptr;
	     child = GetWindow(child, GW_HWNDNEXT))
	{
		if (child == other) return true;
		if (child == window) return false;
	}

	return false;
}

void printHandle(const char* prefix, HWND hwnd) {
	std::printf("%s%lld\n", prefix, static_cast<long long>(reinterpret_cast<qintptr>(hwnd)));
	std::fflush(stdout);
}

void pumpMessages() {
	MSG message {};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

// Host mode: own the visible window the wallpaper and the shell are parented
// into, and print its handle. It only keeps its message queue alive.
int runHostWindow() {
	registerTestClass(HOST_CLASS, &ownerProc);

	auto* host = createWindow(HOST_CLASS, nullptr, WS_POPUP | WS_VISIBLE);
	if (host == nullptr) return 2;

	printHandle("", host);
	pumpMessages();
	return 0;
}

struct Wallpaper {
	HWND host = nullptr;
	HWND holder = nullptr;
	HWND child = nullptr;
} gWallpaper; // NOLINT

HWND createWallpaperChild() {
	auto* child = createWindow(FOREIGN_CLASS, gWallpaper.holder, WS_CHILD | WS_VISIBLE);
	if (child != nullptr) SetParent(child, gWallpaper.host);
	return child;
}

LRESULT CALLBACK wallpaperProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	if (message == WM_WALLPAPER_COMMAND) {
		// Everything here runs on this process' own thread, so the z-order
		// events come from the wallpaper process, not from Explorer's thread.
		auto flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

		switch (wparam) {
		case CMD_BOTTOM: SetWindowPos(gWallpaper.child, HWND_BOTTOM, 0, 0, 0, 0, flags); break;
		case CMD_TOP: SetWindowPos(gWallpaper.child, HWND_TOP, 0, 0, 0, 0, flags); break;
		case CMD_REPLACE: {
			// A new child inserted beneath everything, then the old one dies.
			auto* next = createWallpaperChild();
			SetWindowPos(next, HWND_BOTTOM, 0, 0, 0, 0, flags);
			DestroyWindow(gWallpaper.child);
			gWallpaper.child = next;
			break;
		}
		case CMD_STOP:
			// Wallpaper Engine's "-control stop": the desktop windows are
			// destroyed while the process keeps running.
			DestroyWindow(gWallpaper.child);
			gWallpaper.child = nullptr;
			break;
		case CMD_PLAY:
			// Its "-control play": a new child is created hidden directly inside
			// the host, then shown. No parent change and no host reorder.
			gWallpaper.child = createWindow(FOREIGN_CLASS, gWallpaper.host, WS_CHILD);
			ShowWindow(gWallpaper.child, SW_SHOWNA);
			break;
		default: break;
		}

		printHandle("ok ", gWallpaper.child);
		return 0;
	}

	return ownerProc(hwnd, message, wparam, lparam);
}

// Wallpaper mode: parent a visible child into the host from this process, then
// restack or replace it on command, like a live wallpaper app does.
int runWallpaperWindow(HWND host) {
	registerTestClass(HOLDER_CLASS, &wallpaperProc);
	registerTestClass(FOREIGN_CLASS, &plainProc);

	gWallpaper.host = host;
	gWallpaper.holder = createWindow(HOLDER_CLASS, nullptr, WS_POPUP);
	if (gWallpaper.holder == nullptr) return 2;

	gWallpaper.child = createWallpaperChild();
	if (gWallpaper.child == nullptr) return 3;

	printHandle("", gWallpaper.child);

	std::thread([] {
		std::string line;
		while (std::getline(std::cin, line)) {
			WPARAM command = 0;
			if (line == "bottom") command = CMD_BOTTOM;
			else if (line == "top") command = CMD_TOP;
			else if (line == "replace") command = CMD_REPLACE;
			else if (line == "stop") command = CMD_STOP;
			else if (line == "play") command = CMD_PLAY;
			PostMessageW(gWallpaper.holder, WM_WALLPAPER_COMMAND, command, 0);
		}
	}).detach();

	pumpMessages();
	return 0;
}

LRESULT CALLBACK ownerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

class TestDesktopZorder: public QObject {
	Q_OBJECT

private slots:
	void initTestCase() {
		registerTestClass(CHILD_CLASS, &plainProc);
		registerTestClass(HOLDER_CLASS, &plainProc);
	}

	void init() {
		auto* host = qs::windows::DesktopHost::instance();
		host->setExternalWallpaperEnabled(true);
		host->setReconcileSourcesForTesting(true, true);

		// The host window lives in its own process, like Explorer's WorkerW.
		QVERIFY(startHost(m_hostProcess, m_host));

		// The shell Backgrounds belong to this process and are reparented
		// into the foreign host, as the production panels are.
		m_localParent = createWindow(HOLDER_CLASS, nullptr, WS_POPUP);
		m_shell = createWindow(CHILD_CLASS, m_localParent, WS_CHILD | WS_VISIBLE);
		m_other = createWindow(CHILD_CLASS, m_localParent, WS_CHILD | WS_VISIBLE);
		QVERIFY(m_shell != nullptr && m_other != nullptr);
		SetParent(m_shell, m_host);
		SetParent(m_other, m_host);
		QVERIFY(GetParent(m_shell) == m_host && GetParent(m_other) == m_host);

		// The production panel marks itself as a Background right before the
		// group placement; mirror that here.
		qs::windows::markDesktopBackground(m_shell, true);
		qs::windows::markDesktopBackground(m_other, true);

		// Also installs the production hooks, scoped to the host's process and
		// thread.
		host->adoptHostForTesting(m_host);
		// No external owner yet: this is the shell's own desktop, so it is
		// not an underlay even though the host has no insert-after.
		QVERIFY(host->active());
		QVERIFY(!host->underlayActive());
		// SetParent puts a window on top; make the shell the top of the group so
		// there is a relative order (shell above other) to preserve.
		SetWindowPos(m_shell, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		qs::windows::placeDesktopBackgrounds(m_host);
		QVERIFY(GetWindow(m_host, GW_CHILD) == m_shell);
		QVERIFY(isBelow(m_host, m_other, m_shell));

		// The production panel re-places on this signal. The test connection
		// mirrors it so the placement path is exercised end to end.
		m_orderConnection = QObject::connect(
		    host,
		    &qs::windows::DesktopHost::backgroundOrderChanged,
		    this,
		    [this] {
			    if (auto* current = qs::windows::DesktopHost::instance()->parentWindow()) {
				    qs::windows::placeDesktopBackgrounds(
				        current,
				        qs::windows::DesktopHost::instance()->externalWallpaperEnabled()
				    );
			    }
		    },
		    Qt::QueuedConnection
		);

		// The production panel re-embeds into the host discovery reports on
		// parentChanged. Mirror that so a replaced host is exercised through
		// the same signal, not by the test moving the windows by hand.
		m_parentConnection = QObject::connect(
		    host,
		    &qs::windows::DesktopHost::parentChanged,
		    this,
		    [this] {
			    this->moveBackgrounds(qs::windows::DesktopHost::instance()->parentWindow());
		    },
		    Qt::QueuedConnection
		);
	}

	void cleanup() {
		QObject::disconnect(m_orderConnection);
		QObject::disconnect(m_parentConnection);
		m_orderConnection = {};
		m_parentConnection = {};

		auto* host = qs::windows::DesktopHost::instance();
		host->setDiscoveredHostForTesting(nullptr);
		host->setReconcileSourcesForTesting(true, true);
		host->setEnabled(false);
		host->setExternalWallpaperEnabled(true);

		for (auto* process: {&m_foreignProcess, &m_replacementProcess, &m_hostProcess}) {
			if (process->state() != QProcess::NotRunning) {
				process->kill();
				process->waitForFinished(3000);
			}
		}

		if (m_shell != nullptr) DestroyWindow(m_shell);
		if (m_other != nullptr) DestroyWindow(m_other);
		if (m_localParent != nullptr) DestroyWindow(m_localParent);
		m_foreign = nullptr;
		m_host = nullptr;
		m_replacementHost = nullptr;
		m_shell = nullptr;
		m_other = nullptr;
		m_localParent = nullptr;
	}

	void underlayStaysBelowForeignOwner() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();

		// The external window is topmost and both Backgrounds are below it,
		// in their pre-existing relative order.
		QVERIFY(GetWindow(m_host, GW_CHILD) == m_foreign);
		QVERIFY(isBelow(m_host, m_shell, m_foreign));
		QVERIFY(isBelow(m_host, m_other, m_shell));
		QVERIFY(host->underlayActive());
	}

	void disabledIntegrationCoversRunningForeignOwner() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		qs::windows::DesktopLayer layer;
		QSignalSpy changed(&layer, &qs::windows::DesktopLayer::externalWallpaperEnabledChanged);
		QVERIFY(layer.externalWallpaperEnabled());

		layer.setExternalWallpaperEnabled(false);
		QCOMPARE(changed.count(), 1);
		QVERIFY(!host->underlayActive());
		QVERIFY(host->active());
		QVERIFY(GetParent(m_shell) == m_host);
		QVERIFY(GetWindow(m_host, GW_CHILD) == m_shell);
		QVERIFY(isBelow(m_host, m_other, m_shell));
		QVERIFY(isBelow(m_host, m_foreign, m_other));
		QVERIFY(!qs::windows::desktopBackgroundsMisordered(m_host, false));

		// The engine can restack or replace its window without restarting.
		QVERIFY(command("top"));
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_shell, 2500);
		QVERIFY(command("replace"));
		QVERIFY(command("top"));
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_shell, 2500);
		QVERIFY(isBelow(m_host, m_foreign, m_other));
		QVERIFY(!host->underlayActive());

		auto moves = qs::windows::desktopBackgroundMovesForTesting();
		layer.setExternalWallpaperEnabled(false);
		qs::windows::placeDesktopBackgrounds(m_host, false);
		QCOMPARE(changed.count(), 1);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), moves);

		// Re-enabling restores exactly the existing underlay policy.
		layer.setExternalWallpaperEnabled(true);
		QCOMPARE(changed.count(), 2);
		QVERIFY(host->underlayActive());
		QVERIFY(orderIsCorrect());
	}

	void disabledIntegrationBeforeOwnerArrives() {
		auto* host = qs::windows::DesktopHost::instance();
		host->setExternalWallpaperEnabled(false);
		QVERIFY(!host->underlayActive());
		QVERIFY(GetWindow(m_host, GW_CHILD) == m_shell);
		QVERIFY(startWallpaper());
		QVERIFY(command("top"));
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_shell, 2500);
		QVERIFY(isBelow(m_host, m_foreign, m_other));
		QVERIFY(!host->underlayActive());
	}

	void correctOrderIsNeverRestacked() {
		QVERIFY(attachWallpaperBelowShell());
		// A correct order must cost no SetWindowPos, however many reconcile
		// events pass: the reconciliation only asks, it does not re-place.
		auto moves = qs::windows::desktopBackgroundMovesForTesting();
		QTest::qWait(2600);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), moves);
		QVERIFY(orderIsCorrect());
		QVERIFY(!qs::windows::desktopBackgroundsMisordered(m_host));

		// The same holds when the wallpaper restacks itself to where it
		// already is.
		QVERIFY(command("top"));
		QTest::qWait(1300);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), moves);
		QVERIFY(orderIsCorrect());
	}

	void firstForeignOwnerIsDiscoveredWithoutRefresh() {
		// The shell starts with an empty wallpaper host; the first wallpaper
		// owner attaches later, from its own process, and puts itself beneath
		// the shell. No hook can be scoped to that process yet, so only the
		// desktop-wide event route can notice. Nothing here re-adopts or
		// refreshes the host.
		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(true, false);
		QVERIFY(host->active());
		QVERIFY(!host->underlayActive());
		QVERIFY(GetWindow(m_host, GW_CHILD) == m_shell);

		QVERIFY(startWallpaper());
		QVERIFY(command("bottom"));
		// The desktop-wide event route can already have repaired the order
		// before this line, because the command pumps this thread's messages
		// while it waits. Only the repaired state is asserted.
		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 2500);
		QTRY_VERIFY_WITH_TIMEOUT(host->underlayActive(), 2500);

		// And it keeps working afterwards: the same owner restacks again and
		// the event route repairs it.
		QVERIFY(command("bottom"));
		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 2500);
	}

	void hostScopedHookAloneMissesForeignRestack() {
		QVERIFY(attachWallpaperBelowShell());
		// Negative control for the whole fix: with both the desktop-wide event
		// route and the foreign process hooks off, only the production hooks
		// on the host's process and thread remain. They do not see the
		// wallpaper process restack its own window, so the shell stays above
		// the running wallpaper.
		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(false, false);

		QVERIFY(command("bottom"));
		QTest::qWait(1800);

		QVERIFY(qs::windows::desktopBackgroundsMisordered(m_host));
		QVERIFY(GetWindow(m_host, GW_CHILD) != m_foreign);
		QVERIFY(isBelow(m_host, m_foreign, m_shell));
	}

	void eventRouteRepairsForeignRestackBeneathShell() {
		QVERIFY(attachWallpaperBelowShell());
		// The desktop-wide event route on its own, with every per-process hook
		// off: the wallpaper process restacks its child, which the host scoped
		// hooks cannot see.
		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(true, false);

		auto moves = qs::windows::desktopBackgroundMovesForTesting();
		QVERIFY(command("bottom"));
		// Presence and acknowledgement never change.
		QVERIFY(host->underlayActive());

		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 2500);
		QVERIFY(host->underlayActive());

		// One repair, then nothing more.
		auto repaired = qs::windows::desktopBackgroundMovesForTesting();
		QVERIFY(repaired > moves);
		QTest::qWait(2200);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), repaired);
		QVERIFY(orderIsCorrect());
	}

	void eventRouteRepairsForeignChildReplacedBeneathShell() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(true, false);

		auto* old = m_foreign;
		auto moves = qs::windows::desktopBackgroundMovesForTesting();

		// The wallpaper swaps its child for a new one inserted beneath the
		// shell. Presence stays true throughout.
		QVERIFY(command("replace"));
		QVERIFY(m_foreign != old);
		QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(old), 3000);
		QVERIFY(host->underlayActive());

		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 2500);
		QVERIFY(host->underlayActive());

		auto repaired = qs::windows::desktopBackgroundMovesForTesting();
		QVERIFY(repaired > moves);
		QTest::qWait(2200);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), repaired);
		QVERIFY(orderIsCorrect());
	}

	void foreignHookRepairsRestackWithoutTheEventRoute() {
		QVERIFY(attachWallpaperBelowShell());
		// Immediate repair from the hook scoped to the wallpaper process, with
		// the desktop-wide route off: well inside what one reconcile event
		// could explain only if it missed it.
		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(false, true);

		QVERIFY(command("bottom"));
		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 2000);

		auto* old = m_foreign;
		QVERIFY(command("replace"));
		QVERIFY(m_foreign != old);
		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 2000);
		QVERIFY(host->underlayActive());
	}

	void stopPlayRecreatedInsideHostIsReclaimed() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		QSignalSpy ackSpy(host, &qs::windows::DesktopHost::underlayActiveChanged);

		// Stop: the wallpaper windows die while the process lives. With no
		// foreign child left, the per-process hooks are removed.
		QVERIFY(command("stop"));
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_shell, 3000);
		QVERIFY(!qs::windows::desktopHostHasForeignChild(m_host, false));
		QTRY_VERIFY_WITH_TIMEOUT(ackSpy.count() >= 1, 3000);
		// Let the handback refresh settle before playing again.
		QTest::qWait(800);
		ackSpy.clear();

		// Play: a new hidden child created directly in the host, then shown.
		// Only the desktop-wide show route can see it; before it existed the
		// shell stayed on top of the returning wallpaper.
		QVERIFY(command("play"));
		QTRY_VERIFY_WITH_TIMEOUT(orderIsCorrect(), 3000);
		QTRY_VERIFY_WITH_TIMEOUT(ackSpy.count() >= 1, 3000);
		QVERIFY(host->underlayActive());
	}

	void hiddenForeignReleasesBackgroundToTop() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		QSignalSpy orderSpy(host, &qs::windows::DesktopHost::backgroundOrderChanged);
		QSignalSpy ackSpy(host, &qs::windows::DesktopHost::underlayActiveChanged);
		QSignalSpy movedSpy(host, &qs::windows::DesktopHost::parentMoved);
		QVERIFY(host->underlayActive());

		// Wallpaper Engine hides its desktop window while staying alive. The
		// child is still in the host and still a foreign window; it is just
		// not visible any more, so it covers nothing.
		ShowWindow(m_foreign, SW_HIDE);
		QVERIFY(!IsWindowVisible(m_foreign));
		QVERIFY(IsWindow(m_foreign));
		QVERIFY(qs::windows::desktopHostHasForeignChild(m_host, false));
		QVERIFY(!qs::windows::desktopHostHasForeignChild(m_host, true));

		// The host sees the hide, drops the acknowledgement and raises the
		// marked Backgrounds to the top of the host. A hidden foreign child
		// must not keep the group buried below it: that is the black desktop
		// from #214.
		QTRY_VERIFY_WITH_TIMEOUT(!host->underlayActive(), 3000);
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_shell, 3000);
		QVERIFY(isBelow(m_host, m_other, m_shell));
		QVERIFY(isBelow(m_host, m_foreign, m_other));
		QVERIFY(orderSpy.count() >= 1);
		QVERIFY(ackSpy.count() >= 1);
		// The handback re-runs discovery even when the host did not change.
		// The resulting parentMoved is what makes the panel re-place and
		// present its surface again.
		QTRY_VERIFY_WITH_TIMEOUT(movedSpy.count() >= 1, 3000);

		// An already correct top placement costs no SetWindowPos, so the
		// visibility edge cannot start a restack loop.
		auto moves = qs::windows::desktopBackgroundMovesForTesting();
		qs::windows::placeDesktopBackgrounds(m_host);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), moves);
		QVERIFY(GetWindow(m_host, GW_CHILD) == m_shell);

		// Showing it again acknowledges the underlay and puts the Backgrounds
		// back below the bottom-most visible foreign window.
		ShowWindow(m_foreign, SW_SHOW);
		QVERIFY(IsWindowVisible(m_foreign));
		QTRY_VERIFY_WITH_TIMEOUT(host->underlayActive(), 3000);
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_foreign, 3000);
		QVERIFY(isBelow(m_host, m_shell, m_foreign));
		QVERIFY(isBelow(m_host, m_other, m_shell));
		QVERIFY(orderSpy.count() >= 2);
		QVERIFY(ackSpy.count() >= 2);
	}

	void handbackDropsAckAndReturnsToVisibleTop() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		QSignalSpy ackSpy(host, &qs::windows::DesktopHost::underlayActiveChanged);
		QSignalSpy orderSpy(host, &qs::windows::DesktopHost::backgroundOrderChanged);

		// Wallpaper Engine exits: its window dies in its own process.
		m_foreignProcess.kill();
		QVERIFY(m_foreignProcess.waitForFinished(5000));
		QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(m_foreign), 3000);
		m_foreign = nullptr;

		// The host notices, drops the acknowledgement, and the group placement
		// puts the Background back on top of the host.
		QTRY_VERIFY_WITH_TIMEOUT(!host->underlayActive(), 3000);
		QTRY_VERIFY_WITH_TIMEOUT(ackSpy.count() >= 1, 3000);
		QTRY_VERIFY_WITH_TIMEOUT(orderSpy.count() >= 1, 3000);
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_host, GW_CHILD) == m_shell, 3000);
		QVERIFY(isBelow(m_host, m_other, m_shell));
		QVERIFY(host->active());
		QVERIFY(!host->underlayActive());
	}

	void handbackReembedsIntoReplacementHost() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		QSignalSpy parentSpy(host, &qs::windows::DesktopHost::parentChanged);
		QVERIFY(host->underlayActive());

		// Explorer shows a new WorkerW on the handback. The old window stays
		// valid, so no destroy/hide of the current parent and no WorkerW show
		// event can repair this on its own: discovery has to run on the
		// handback edge itself.
		QVERIFY(startHost(m_replacementProcess, m_replacementHost));
		host->setDiscoveredHostForTesting(m_replacementHost);

		// Only the foreign wallpaper child leaves. Its process hides it; the
		// shell's hooks on the old host never see that.
		ShowWindow(m_foreign, SW_HIDE);
		QVERIFY(!IsWindowVisible(m_foreign));

		QTRY_VERIFY_WITH_TIMEOUT(!host->underlayActive(), 3000);
		QTRY_VERIFY_WITH_TIMEOUT(parentSpy.count() >= 1, 3000);
		QCOMPARE(host->parentWindow(), m_replacementHost);

		// The Backgrounds follow the production parentChanged path: a visible
		// child of the host discovery now reports, not a leftover sibling in
		// the old host.
		QTRY_VERIFY_WITH_TIMEOUT(GetParent(m_shell) == m_replacementHost, 3000);
		QTRY_VERIFY_WITH_TIMEOUT(GetParent(m_other) == m_replacementHost, 3000);
		QVERIFY(IsWindowVisible(m_shell));
		QVERIFY(IsWindowVisible(m_other));
		QVERIFY(IsWindowVisible(m_replacementHost));
		QTRY_VERIFY_WITH_TIMEOUT(GetWindow(m_replacementHost, GW_CHILD) == m_shell, 3000);
		QVERIFY(isBelow(m_replacementHost, m_other, m_shell));
		QVERIFY(host->active());
	}

	void embedRefusalWithdrawsTheUnderlay() {
		QVERIFY(attachWallpaperBelowShell());
		auto* host = qs::windows::DesktopHost::instance();
		QSignalSpy refusedSpy(host, &qs::windows::DesktopHost::embedRefusedChanged);
		QVERIFY(host->underlayActive());
		QVERIFY(!host->embedRefused());

		// A panel that could not be reparented stays top level. The
		// acknowledgement must drop, but the host is still active: the
		// external wallpaper window did not leave.
		host->noteEmbedRefused(m_host);
		QVERIFY(host->embedRefused());
		QVERIFY(!host->underlayActive());
		QVERIFY(host->active());
		QCOMPARE(refusedSpy.count(), 1);

		// Disabling the host forgets the refusal so the next lookup can retry.
		host->setEnabled(false);
		QVERIFY(!host->embedRefused());
		QCOMPARE(refusedSpy.count(), 2);
	}

	void destroyedHostRediscoversReplacement() {
		auto* host = qs::windows::DesktopHost::instance();
		QVERIFY(startHost(m_replacementProcess, m_replacementHost));
		host->setDiscoveredHostForTesting(m_replacementHost);
		QSignalSpy parentSpy(host, &qs::windows::DesktopHost::parentChanged);

		m_hostProcess.kill();
		QVERIFY(m_hostProcess.waitForFinished(5000));
		QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(m_host), 3000);

		QTRY_VERIFY_WITH_TIMEOUT(parentSpy.count() >= 1, 3000);
		QCOMPARE(host->parentWindow(), m_replacementHost);
		QVERIFY(host->active());
		QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(m_shell), 2000);
		QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(m_other), 2000);
		m_shell = nullptr;
		m_other = nullptr;
	}

	void reconcileReadCost() {
		QVERIFY(attachWallpaperBelowShell());
		constexpr int iterations = 20000;
		QElapsedTimer timer;
		timer.start();
		auto misordered = 0;
		for (int i = 0; i < iterations; ++i) {
			misordered += qs::windows::desktopBackgroundsMisordered(m_host) ? 1 : 0;
			misordered += qs::windows::desktopHostHasForeignChild(m_host, true) ? 0 : 1;
			misordered += qs::windows::desktopHostHasForeignChild(m_host, false) ? 0 : 1;
		}
		const auto ns = timer.nsecsElapsed();
		QCOMPARE(misordered, 0);
		qInfo("reconcile read cost: %.2f us per pass (%d passes, 4 host children)",
		      double(ns) / iterations / 1000.0, iterations);
	}

	void taskbarCreatedRediscoversHost() {
		auto* host = qs::windows::DesktopHost::instance();
		host->refreshForTesting();
		auto* listener = ownListener();
		QVERIFY(listener != nullptr);
		QVERIFY(startHost(m_replacementProcess, m_replacementHost));
		host->setDiscoveredHostForTesting(m_replacementHost);
		QSignalSpy parentSpy(host, &qs::windows::DesktopHost::parentChanged);

		SendMessageW(listener, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);

		QTRY_VERIFY_WITH_TIMEOUT(parentSpy.count() >= 1, 2000);
		QCOMPARE(host->parentWindow(), m_replacementHost);
		QTRY_VERIFY_WITH_TIMEOUT(GetParent(m_shell) == m_replacementHost, 2000);
	}

	void disableBeforeRediscoveryKeepsHostReleased() {
		auto* host = qs::windows::DesktopHost::instance();
		QVERIFY(startHost(m_replacementProcess, m_replacementHost));
		host->setDiscoveredHostForTesting(m_replacementHost);

		ShowWindow(m_host, SW_HIDE);
		host->setEnabled(false);
		QTest::qWait(1200);

		QVERIFY(!host->active());
		QVERIFY(host->parentWindow() == nullptr);
		QVERIFY(GetParent(m_shell) != m_replacementHost);
	}

private:
	static HWND ownListener() {
		HWND window = nullptr;
		while ((window = FindWindowExW(nullptr, window, L"QuickshellDesktopListener", nullptr)) != nullptr) {
			DWORD pid = 0;
			GetWindowThreadProcessId(window, &pid);
			if (pid == GetCurrentProcessId()) return window;
		}
		return nullptr;
	}

	// Starts a process that owns one synthetic desktop host window, like the
	// Explorer process owns WorkerW.
	bool startHost(QProcess& process, HWND& host) {
		process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--host-window")});
		if (!process.waitForStarted(5000)) return false;

		host = handleFrom(process);
		return host != nullptr && IsWindow(host) && IsWindowVisible(host);
	}

	// The production panel re-embeds each Background into the host that
	// discovery reports. Keep the group ordering in the new host as well.
	void moveBackgrounds(HWND host) {
		if (host == nullptr || !IsWindow(host)) return;

		for (auto* background: {m_shell, m_other}) {
			if (background == nullptr || !IsWindow(background)) continue;
			SetParent(background, host);
			qs::windows::markDesktopBackground(background, true);
		}

		if (m_shell != nullptr && IsWindow(m_shell)) {
			SetWindowPos(m_shell, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}

		qs::windows::placeDesktopBackgrounds(host);
	}

	// Starts the wallpaper process in a third process; it parents its child
	// into the host from there. Nothing tells the DesktopHost about it.
	bool startWallpaper() {
		m_foreignProcess.start(
		    QCoreApplication::applicationFilePath(),
		    {QStringLiteral("--wallpaper-window"),
		     QString::number(static_cast<qlonglong>(reinterpret_cast<qintptr>(m_host)))}
		);
		if (!m_foreignProcess.waitForStarted(5000)) return false;

		m_foreign = handleFrom(m_foreignProcess);
		if (m_foreign == nullptr || !IsWindow(m_foreign)) return false;

		QElapsedTimer timer;
		timer.start();
		while (GetParent(m_foreign) != m_host && timer.elapsed() < 5000) QTest::qWait(10);
		return GetParent(m_foreign) == m_host && IsWindowVisible(m_foreign);
	}

	// The shared starting state of the earlier cases: a wallpaper below the
	// shell (the state from which a promotion would cover it if the group
	// placement got it wrong), put in order and at rest. This one does
	// refresh the host explicitly; the first-owner case deliberately does not.
	bool attachWallpaperBelowShell() {
		if (!startWallpaper() || !command("bottom")) return false;

		auto* host = qs::windows::DesktopHost::instance();
		host->adoptHostForTesting(m_host);
		if (!host->underlayActive()) return false;
		qs::windows::placeDesktopBackgrounds(m_host);

		QElapsedTimer timer;
		timer.start();
		while (!orderIsCorrect() && timer.elapsed() < 3000) QTest::qWait(10);

		// Let queued reorder feedback settle so tests start from rest.
		QTest::qWait(150);
		return orderIsCorrect();
	}

	// The wallpaper on top, the shell Backgrounds beneath it in their
	// original relative order.
	bool orderIsCorrect() const {
		return GetWindow(m_host, GW_CHILD) == m_foreign && isBelow(m_host, m_shell, m_foreign)
		    && isBelow(m_host, m_other, m_shell);
	}

	// Reads one handle line while still pumping this thread's messages: the
	// other processes reparent windows into trees this thread shares input
	// with, and must not wait on a blocked queue.
	static HWND handleFrom(QProcess& process) {
		QElapsedTimer timer;
		timer.start();
		while (!process.canReadLine() && timer.elapsed() < 5000) QTest::qWait(10);
		if (!process.canReadLine()) return nullptr;

		auto value = process.readLine().trimmed().toLongLong();
		return reinterpret_cast<HWND>(static_cast<qintptr>(value));
	}

	// Sends a command to the wallpaper process and takes its current child.
	bool command(const QByteArray& name) {
		m_foreignProcess.write(name + '\n');

		QElapsedTimer timer;
		timer.start();
		while (!m_foreignProcess.canReadLine() && timer.elapsed() < 5000) QTest::qWait(10);
		if (!m_foreignProcess.canReadLine()) return false;

		auto line = m_foreignProcess.readLine().trimmed();
		if (!line.startsWith("ok ")) return false;

		m_foreign = reinterpret_cast<HWND>(static_cast<qintptr>(line.mid(3).toLongLong()));
		if (name == "stop") return m_foreign == nullptr;
		return IsWindow(m_foreign);
	}

	HWND m_host = nullptr;
	HWND m_replacementHost = nullptr;
	HWND m_localParent = nullptr;
	HWND m_shell = nullptr;
	HWND m_other = nullptr;
	HWND m_foreign = nullptr;
	QProcess m_hostProcess;
	QProcess m_replacementProcess;
	QProcess m_foreignProcess;
	QMetaObject::Connection m_orderConnection;
	QMetaObject::Connection m_parentConnection;
};

int main(int argc, char** argv) {
	if (argc > 1 && qstrcmp(argv[1], "--host-window") == 0) return runHostWindow();
	if (argc > 2 && qstrcmp(argv[1], "--wallpaper-window") == 0) {
		return runWallpaperWindow(reinterpret_cast<HWND>(static_cast<qintptr>(std::atoll(argv[2]))));
	}

	QGuiApplication app(argc, argv);
	TestDesktopZorder test;
	return QTest::qExec(&test, argc, argv);
}

#include "desktop_zorder_test.moc"
