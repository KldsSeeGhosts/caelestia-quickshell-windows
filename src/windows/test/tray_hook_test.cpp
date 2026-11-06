#include <atomic>
#include <cstring>
#include <vector>

#include <qtest.h>
#include <qt_windows.h>

#include "../activation_policy.hpp"
#include "../services/systray/hook.hpp"
#include "../services/systray/tray_reorder.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace qs::windows {
std::optional<ActivationSource> currentActivationSource() { return std::nullopt; }
} // namespace qs::windows

using namespace qs::windows::services::systray;

namespace {

constexpr const wchar_t* TASKBAR_CLASS = L"CaelestiaTrayReorderTestTaskbar";
constexpr const wchar_t* OTHER_CLASS = L"CaelestiaTrayReorderTestWindow";
constexpr const wchar_t* RESTART_PROP = L"CaelestiaTrayRestartProbe";
constexpr ULONGLONG LONG_FRONT_MS = 8000;

LRESULT CALLBACK plainProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void registerTestClass(const wchar_t* name) {
	WNDCLASSW cls {};
	cls.lpfnWndProc = plainProc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = name;
	RegisterClassW(&cls);
}

HWND createTestWindow(const wchar_t* name, DWORD exStyle) {
	return CreateWindowExW(
	    exStyle,
	    name,
	    L"",
	    WS_POPUP,
	    -20000,
	    -20000,
	    100,
	    100,
	    nullptr,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
}

// A stand-in for Explorer's taskbar: same fixture class as the mirror, so the
// production first-tray-window rule sees both, but no hook property, so it is
// treated as Explorer's window.
HWND createFixtureTaskbar() { return createTestWindow(TrayHook::testTrayClass(), WS_EX_TOPMOST); }

void startFixtureHook(bool brief) {
	TrayHook::setTestElevated(false);
	TrayHook::start([](TrayIconMessage) {}, []() {}, brief);
}

bool rectsEqual(HWND a, HWND b) {
	RECT ra {};
	RECT rb {};
	return GetWindowRect(a, &ra) && GetWindowRect(b, &rb) && EqualRect(&ra, &rb);
}

// True when `upper` is above `lower` in the top-level z-order.
bool aboveInZOrder(HWND upper, HWND lower) {
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (hwnd == upper) return true;
		if (hwnd == lower) return false;
	}

	return false;
}

} // namespace

class TrayHookTest: public QObject {
	Q_OBJECT

private slots:
	void initTestCase() {
		// The hook thread runs per-monitor aware; match it so GetWindowRect and
		// SetWindowPos use the same physical pixels on both threads.
		SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	}

	void cleanup() {
		TrayHook::stop();
		TrayHook::setTestTaskbar(nullptr);
		TrayHook::setTestBriefFrontMs(LONG_FRONT_MS);
	}

	void disabled() {
		QCOMPARE(TrayHook::capability().value("availability").toString(), "disabled");
		QCOMPARE(TrayHook::capability().value("reason").toString(), "hook-disabled");
		QVERIFY(!TrayHook::running());
	}

	void elevatedStartIsUnavailable() {
		TrayHook::setTestElevated(true);
		TrayHookState published = TrayHookState::Disabled;
		TrayHook::start(
		    [](TrayIconMessage) {},
		    []() {},
		    false,
		    [&published](TrayHookState state) { published = state; }
		);
		QCOMPARE(TrayHook::capability().value("availability").toString(), "unavailable");
		QCOMPARE(TrayHook::capability().value("reason").toString(), "elevated");
		QCOMPARE(trayHookCapability(published), TrayHook::capability());
		QVERIFY(!TrayHook::running());
	}

	void failedWindowIsUnavailable() {
		TrayHook::setTestElevated(false);
		TrayHook::setTestTaskbar(nullptr);
		std::atomic<TrayHookState> published = TrayHookState::Disabled;
		TrayHook::start(
		    [](TrayIconMessage) {},
		    []() {},
		    false,
		    [&published](TrayHookState state) { published.store(state); }
		);
		QTRY_COMPARE(published.load(), TrayHookState::WindowCreationFailed);
		QTRY_COMPARE(TrayHook::capability().value("reason").toString(), "window-creation-failed");
		QCOMPARE(TrayHook::capability().value("availability").toString(), "unavailable");
		QVERIFY(!TrayHook::running());
	}

	// Explorer re-raising its taskbar is a top-level z-order change announced
	// as a reorder on the desktop. The repair must put the mirror back first
	// every time; extra or coalesced deliveries are fine, but the outcome is
	// not. A fixed repair count flaked when Windows coalesced events.
	void taskbarRestackRepairs() {
		TrayHook::registerTestTrayClass();
		auto* taskbar = createFixtureTaskbar();
		QVERIFY(taskbar != nullptr);

		TrayHook::setTestTaskbar(taskbar);
		TrayHook::setTestBriefFrontMs(LONG_FRONT_MS);
		startFixtureHook(false);

		QTRY_VERIFY(TrayHook::testMirror() != nullptr);
		auto* mirror = TrayHook::testMirror();
		QTRY_VERIFY(aboveInZOrder(mirror, taskbar));

		for (auto round = 0; round < 2; round++) {
			auto repairs = TrayHook::testRepairs();
			SetWindowPos(taskbar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			QTRY_VERIFY(TrayHook::testRepairs() > repairs);
			QTRY_VERIFY(aboveInZOrder(mirror, taskbar));
		}

		TrayHook::stop();
		DestroyWindow(taskbar);
	}

	// A move or resize never changes the z-order, so the taskbar following the
	// work area (auto-hide, DPI, display change) reaches the repair through its
	// location change instead. Another window's move must not.
	void taskbarMoveRepairs() {
		registerTestClass(TASKBAR_CLASS);
		registerTestClass(OTHER_CLASS);

		auto* taskbar = createTestWindow(TASKBAR_CLASS, 0);
		auto* other = createTestWindow(OTHER_CLASS, 0);
		QVERIFY(taskbar != nullptr);
		QVERIFY(other != nullptr);

		std::atomic<int> repairs = 0;
		TrayReorderWatch watch;
		QVERIFY(watch.install(taskbar, [&repairs] { repairs.fetch_add(1); }));

		SetWindowPos(other, nullptr, -20100, -20000, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE);
		SetWindowPos(taskbar, nullptr, -20200, -20000, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE);
		QTRY_COMPARE(repairs.load(), 1);

		// A second round catches a location change that slipped through the
		// wrong-window filter: the count would be one ahead.
		SetWindowPos(other, nullptr, -20300, -20000, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE);
		SetWindowPos(taskbar, nullptr, -20400, -20000, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE);
		QTRY_COMPARE(repairs.load(), 2);

		DestroyWindow(taskbar);
		DestroyWindow(other);
	}

	// The replacement taskbar can already be settled before the new hooks
	// exist. Binding it must run the same repair an event would: no later event
	// is delivered here, so the mirror has to move and copy properties from the
	// restart itself.
	void explorerRestartSyncsOnce() {
		TrayHook::registerTestTrayClass();
		auto* first = createFixtureTaskbar();
		auto* replacement = createFixtureTaskbar();
		QVERIFY(first != nullptr);
		QVERIFY(replacement != nullptr);

		SetWindowPos(replacement, nullptr, -20400, -20000, 160, 80, SWP_NOZORDER | SWP_NOACTIVATE);
		SetPropW(replacement, RESTART_PROP, reinterpret_cast<HANDLE>(42)); // NOLINT

		TrayHook::setTestTaskbar(first);
		TrayHook::setTestBriefFrontMs(LONG_FRONT_MS);
		startFixtureHook(false);

		QTRY_VERIFY(TrayHook::testMirror() != nullptr);
		auto* mirror = TrayHook::testMirror();
		QTRY_VERIFY(rectsEqual(mirror, first));

		TrayHook::resetTestRepairs();
		TrayHook::setTestTaskbar(replacement);
		TrayHook::testTaskbarCreated();

		QTRY_VERIFY(GetPropW(mirror, RESTART_PROP) != nullptr);
		QTRY_VERIFY(rectsEqual(mirror, replacement));
		QCOMPARE(GetPropW(mirror, RESTART_PROP), reinterpret_cast<HANDLE>(42)); // NOLINT
		QCOMPARE(TrayHook::testRepairs(), 1);

		TrayHook::stop();
		DestroyWindow(first);
		DestroyWindow(replacement);
	}

	// Icon traffic can reach the mirror after Explorer is replaced but before
	// TaskbarCreated, and that makes the replacement the cached taskbar. The
	// restart must still rebind and repair from what the hooks actually watch.
	void restartAfterEarlyIconTrafficRebinds() {
		TrayHook::registerTestTrayClass();
		auto* first = createFixtureTaskbar();
		auto* replacement = createFixtureTaskbar();
		QVERIFY(first != nullptr);
		QVERIFY(replacement != nullptr);

		TrayHook::setTestTaskbar(first);
		TrayHook::setTestBriefFrontMs(LONG_FRONT_MS);
		startFixtureHook(false);

		QTRY_VERIFY(TrayHook::testMirror() != nullptr);
		auto* mirror = TrayHook::testMirror();
		QTRY_VERIFY(rectsEqual(mirror, first));

		DestroyWindow(first);
		TrayHook::setTestTaskbar(replacement);

		// Shell_NotifyIcon's own path: a WM_COPYDATA notify-icon message to the
		// mirror, which looks up Explorer and so caches the replacement.
		std::vector<char> wire(sizeof(DWORD) * 2 + 956);
		const DWORD signature = 0x34753423;
		std::memcpy(wire.data(), &signature, sizeof(signature));
		COPYDATASTRUCT data {};
		data.dwData = 1;
		data.cbData = static_cast<DWORD>(wire.size());
		data.lpData = wire.data();
		SendMessageW(mirror, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));

		// Settle the replacement after the traffic: nothing watches it yet, so
		// only the restart can carry this to the mirror.
		SetWindowPos(replacement, nullptr, -20400, -20000, 160, 80, SWP_NOZORDER | SWP_NOACTIVATE);
		SetPropW(replacement, RESTART_PROP, reinterpret_cast<HANDLE>(42)); // NOLINT
		QVERIFY(!rectsEqual(mirror, replacement));

		TrayHook::testTaskbarCreated();

		QTRY_VERIFY(rectsEqual(mirror, replacement));
		QTRY_COMPARE(GetPropW(mirror, RESTART_PROP), reinterpret_cast<HANDLE>(42)); // NOLINT

		// The hooks now watch the replacement: its move and restack reach the
		// repair as events, with no fixture call.
		SetWindowPos(replacement, nullptr, -20600, -20000, 120, 90, SWP_NOZORDER | SWP_NOACTIVATE);
		QTRY_VERIFY(rectsEqual(mirror, replacement));

		SetWindowPos(replacement, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		QTRY_VERIFY(aboveInZOrder(mirror, replacement));

		TrayHook::stop();
		DestroyWindow(replacement);
	}

	// Brief mode hands the top spot back when the deadline passes. Nothing else
	// moves the mirror then, so the single-shot deadline timer must run the
	// normal repair without any repair call from the fixture.
	void briefModeHandsBackAtDeadline() {
		TrayHook::registerTestTrayClass();
		auto* taskbar = createFixtureTaskbar();
		QVERIFY(taskbar != nullptr);

		TrayHook::setTestTaskbar(taskbar);
		TrayHook::setTestBriefFrontMs(600);
		startFixtureHook(true);

		QTRY_VERIFY(TrayHook::testMirror() != nullptr);
		auto* mirror = TrayHook::testMirror();
		QVERIFY(aboveInZOrder(mirror, taskbar));

		QTRY_VERIFY(TrayHook::testRepairs() >= 1);
		QTRY_VERIFY(aboveInZOrder(taskbar, mirror));

		TrayHook::stop();
		DestroyWindow(taskbar);
	}
};

QTEST_GUILESS_MAIN(TrayHookTest)
#include "tray_hook_test.moc"
