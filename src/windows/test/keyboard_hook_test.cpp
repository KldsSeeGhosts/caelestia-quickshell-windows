#include <algorithm>
#include <array>
#include <deque>
#include <vector>

#include <qcoreapplication.h>
#include <qtest.h>
#include <windows.h>

namespace {
UINT WINAPI fakeSendInput(UINT count, LPINPUT inputs, int size);
SHORT WINAPI fakeGetAsyncKeyState(int vk);
LRESULT WINAPI fakeCallNextHookEx(HHOOK hook, int code, WPARAM message, LPARAM data);
BOOL WINAPI fakePostMessageW(HWND target, UINT message, WPARAM id, LPARAM serial);
UINT_PTR WINAPI fakeSetTimer(HWND, UINT_PTR, UINT, TIMERPROC);
BOOL WINAPI fakeKillTimer(HWND, UINT_PTR);
} // namespace

// Exercise the actual callbacks and replay path, without installing a hook,
// posting to a real window, querying live keys or injecting desktop input.
#define SendInput fakeSendInput
#define GetAsyncKeyState fakeGetAsyncKeyState
#define CallNextHookEx fakeCallNextHookEx
#define PostMessageW fakePostMessageW
#define SetTimer fakeSetTimer
#define KillTimer fakeKillTimer
#include "../keyboard_hook.cpp"
#include "../input_mask.cpp"
#undef SendInput
#undef GetAsyncKeyState
#undef CallNextHookEx
#undef PostMessageW
#undef SetTimer
#undef KillTimer

namespace qs::windows {
void prioritizeInputThread(int) {}
HWND hwndOf(const QWindow*) { return nullptr; }
void setExStyleBits(HWND, LONG_PTR, bool) {}
bool SuperDragManager::onMouseHook(WPARAM, const MSLLHOOKSTRUCT*) { return false; }
void SuperDragManager::onHookStopped() {}
void DesktopDropManager::onMouseHook(WPARAM, const MSLLHOOKSTRUCT*) {}
} // namespace qs::windows

namespace {
using namespace qs::windows::hotkeys;

struct ObserverPost {
	UINT message = 0;
	WPARAM wParam = 0;
	LPARAM lParam = 0;
};

struct FakeDesktop {
	std::array<bool, 256> down {};
	std::deque<INPUT> injected;
	std::vector<KBDLLHOOKSTRUCT> delivered;
	std::vector<HookEvent> posted;
	std::vector<ObserverPost> hookPosts;
	std::vector<ObserverPost> observerPosts;
	bool discardMasks = false;
	bool filterBeforeUs = false;
	bool mutateMasks = false;
	bool captureWin = false;
	bool discardInjected = false;
	bool failInjection = false;
	UINT maxInjection = UINT_MAX;
	int injections = 0;
	int hotkeys = 0;
	int registeredVk = 0;
	bool winUsed = false;
	bool startOpened = false;
	bool inMouseHook = false;
	int maskChainCalls = 0;
	int otherChainCalls = 0;
	int timersArmed = 0;
	int timersKilled = 0;
};

FakeDesktop desktop;

UINT_PTR WINAPI fakeSetTimer(HWND, UINT_PTR, UINT interval, TIMERPROC) {
	desktop.timersArmed++;
	Q_ASSERT(interval == 1000);
	return 42;
}
BOOL WINAPI fakeKillTimer(HWND, UINT_PTR timer) {
	desktop.timersKilled++;
	Q_ASSERT(timer == 42);
	return TRUE;
}

UINT WINAPI fakeSendInput(UINT count, LPINPUT inputs, int size) {
	desktop.injections++;
	if (size != sizeof(INPUT) || desktop.failInjection) return 0;
	count = std::min(count, desktop.maxInjection);
	for (UINT i = 0; i < count; i++) desktop.injected.push_back(inputs[i]);
	return count;
}

SHORT WINAPI fakeGetAsyncKeyState(int vk) {
	if (vk == VK_CONTROL) return (desktop.down[VK_LCONTROL] || desktop.down[VK_RCONTROL]) ? SHORT(0x8000) : 0;
	if (vk == VK_SHIFT) return (desktop.down[VK_LSHIFT] || desktop.down[VK_RSHIFT]) ? SHORT(0x8000) : 0;
	return desktop.down.at(vk) ? SHORT(0x8000) : 0;
}

LRESULT WINAPI fakeCallNextHookEx(HHOOK, int, WPARAM, LPARAM data) {
	if (desktop.inMouseHook) return 0;

	auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
	if (info->vkCode == KeyboardHook::MASK_KEY) {
		desktop.maskChainCalls++;
		return desktop.discardMasks ? 1 : 0;
	}
	desktop.otherChainCalls++;
	return 0;
}

BOOL WINAPI fakePostMessageW(HWND, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message >= WM_APP && message < WM_APP + HookEventCount) {
		desktop.posted.push_back(static_cast<HookEvent>(message - WM_APP));
		desktop.hookPosts.push_back({.message = message, .wParam = wParam, .lParam = lParam});
	} else {
		desktop.observerPosts.push_back({.message = message, .wParam = wParam, .lParam = lParam});
	}
	return TRUE;
}

void dispatch(KBDLLHOOKSTRUCT info) {
	// A newer hook runs BEFORE ours, including for our SendInput events.
	// Returning zero without CallNextHookEx cannot bypass this filter.
	if (desktop.filterBeforeUs) {
		if (desktop.discardInjected && (info.flags & LLKHF_INJECTED)) return;
		if (desktop.captureWin && (info.vkCode == VK_LWIN || info.vkCode == VK_RWIN)) return;
		if ((info.flags & LLKHF_INJECTED) && info.vkCode == KeyboardHook::MASK_KEY) {
			desktop.maskChainCalls++;
			if (desktop.discardMasks) return;
			if (desktop.mutateMasks) info.flags |= LLKHF_UP;
		}
	}
	auto up = (info.flags & LLKHF_UP) != 0;
	auto message = up ? WM_KEYUP : WM_KEYDOWN;
	if (hookProc(HC_ACTION, message, reinterpret_cast<LPARAM>(&info)) != 0) return;

	// Model the system after the hook chain: only a delivered key can mask
	// the Win sequence. SendInput accepting an event is not proof of delivery.
	auto win = info.vkCode == VK_LWIN || info.vkCode == VK_RWIN;
	auto winDown = desktop.down[VK_LWIN] || desktop.down[VK_RWIN];
	if (info.vkCode == desktop.registeredVk && !up && winDown) {
		desktop.hotkeys++;
		return; // RegisterHotKey consumes this key, not an ordinary key mask.
	}
	if (win && !up && !winDown) desktop.winUsed = false;
	if (!win && !up && winDown) desktop.winUsed = true;
	if (win && up && winDown && !desktop.winUsed) desktop.startOpened = true;
	desktop.down.at(info.vkCode) = !up;
	desktop.delivered.push_back(info);
}

void drainInput() {
	while (!desktop.injected.empty()) {
		auto input = desktop.injected.front().ki;
		desktop.injected.pop_front();
		dispatch(
		    KBDLLHOOKSTRUCT {
		        .vkCode = input.wVk,
		        .scanCode = input.wScan,
		        .flags = static_cast<DWORD>(
		            LLKHF_INJECTED | ((input.dwFlags & KEYEVENTF_KEYUP) ? LLKHF_UP : 0)
		            | ((input.dwFlags & KEYEVENTF_EXTENDEDKEY) ? LLKHF_EXTENDED : 0)
		        ),
		        .time = 200,
		        .dwExtraInfo = input.dwExtraInfo,
		    }
		);
	}
}

void physical(DWORD vk, bool up, DWORD time) {
	dispatch(KBDLLHOOKSTRUCT {.vkCode = vk, .flags = up ? DWORD(LLKHF_UP) : 0, .time = time});
	drainInput();
}

void mouse(WPARAM message, DWORD time) {
	MSLLHOOKSTRUCT info {};
	info.pt = POINT {.x = 640, .y = 360};
	info.time = time;
	desktop.inMouseHook = true;
	qs::windows::InputMaskTracker::dispatchMouseHookForTesting(
	    HC_ACTION,
	    message,
	    reinterpret_cast<LPARAM>(&info) // NOLINT(performance-no-int-to-ptr)
	);
	desktop.inMouseHook = false;
}

int taps() {
	return static_cast<int>(std::count(desktop.posted.begin(), desktop.posted.end(), HookTapped));
}
} // namespace

class KeyboardHookTest: public QObject {
	Q_OBJECT;

private slots:
	void taskbarSwitchRoutesThroughTheExistingHook_data() {
		QTest::addColumn<bool>("deferred");
		QTest::addColumn<bool>("fail");
		QTest::newRow("forwarded") << false << false;
		QTest::newRow("deferred") << true << false;
		QTest::newRow("failed-mask-release") << false << true;
	}

	void taskbarSwitchRoutesThroughTheExistingHook() {
		QFETCH(bool, deferred);
		QFETCH(bool, fail);
		if (!deferred) KeyboardHook::setSnapshot(nullptr);
		physical(VK_LWIN, false, 100);
		physical('B', false, 110);
		physical('B', false, 120);
		physical('B', true, 130);
		desktop.failInjection = fail;
		physical(VK_LWIN, true, 140);
		QVERIFY(!desktop.down[VK_LWIN]);
		auto deliveredB =
		    std::count_if(desktop.delivered.begin(), desktop.delivered.end(), [](const auto& info) {
			    return info.vkCode == 'B';
		    });
		QCOMPARE(deliveredB, taskbarShortcutFilterEnabled() ? 0 : 3);
		QVERIFY(!desktop.startOpened || fail);
		if (taskbarShortcutFilterEnabled() && !deferred) {
			QCOMPARE(desktop.injections, 1);
		}
	}

	void taskbarHookBindHasPriority() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'B'}, .id = 25}
		);
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		physical('B', false, 110);
		physical('B', true, 120);
		physical(VK_LWIN, true, 130);
		QCOMPARE(desktop.posted, (std::vector<HookEvent> {HookPressed, HookReleased}));
		QCOMPARE(desktop.hookPosts.front().wParam, WPARAM(25));
		QVERIFY(desktop.delivered.empty());
		QVERIFY(!desktop.startOpened);
	}

	void taskbarMaskedReleasePreservesMetadataAndFailureFallback_data() {
		QTest::addColumn<int>("limit");
		for (int limit = 0; limit <= 3; limit++) {
			QTest::newRow(qPrintable(QString::number(limit))) << limit;
		}
	}

	void taskbarMaskedReleasePreservesMetadataAndFailureFallback() {
		QFETCH(int, limit);
		TaskbarShortcutFilter filter;
		filter.handle(VK_RWIN, false, false, 0, false);
		QVERIFY(filter.handle('T', false, false, ModSuper, false).swallow);
		auto decision = filter.handle(VK_RWIN, true, false, 0, false);
		QVERIFY(decision.maskedRelease);
		KBDLLHOOKSTRUCT up {
		    .vkCode = VK_RWIN,
		    .scanCode = 0x5C,
		    .flags = LLKHF_UP | LLKHF_EXTENDED,
		    .time = 200,
		    .dwExtraInfo = 0
		};
		desktop.maxInjection = limit;
		// No local Win-down, as when an elevated foreground passed it through.
		QCOMPARE(onWin(gNoRouting, &up, true, decision.maskedRelease), LRESULT(limit == 3));
		QCOMPARE(desktop.injections, 1);
		QCOMPARE(desktop.injected.size(), size_t(limit));
		if (limit == 3) {
			auto replay = desktop.injected.back().ki;
			QCOMPARE(replay.wVk, WORD(VK_RWIN));
			QCOMPARE(replay.wScan, WORD(0x5C));
			QCOMPARE(replay.dwFlags, DWORD(KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY));
			QCOMPARE(replay.dwExtraInfo, KeyboardHook::INJECTION_MARKER);
		}
	}

	void taskbarPolicyExactDecisionTable() {
		// Every modifier combination, all ten digits, and unrelated Win
		// chords from the supervisor's policy contract.
		for (int mods = 0; mods < 16; mods++) {
			for (int vk = 0; vk < 256; vk++) {
				bool win = (mods & ModSuper) != 0;
				bool shift = (mods & ModShift) != 0;
				bool ctrl = (mods & ModCtrl) != 0;
				bool alt = (mods & ModAlt) != 0;
				bool expected = win
				             && ((vk == 'T' && !ctrl && !alt) || (vk == 'B' && !shift && !ctrl && !alt)
				                 || (vk == VK_RETURN && alt && !shift && !ctrl)
				                 || (vk >= '0' && vk <= '9' && (alt ? !shift && !ctrl : !(shift && ctrl))));
				TaskbarShortcutFilter filter;
				QCOMPARE(filter.handle(vk, false, false, mods, false).swallow, expected);
			}
		}
	}

	void taskbarRepeatsAndKeyUpRemainOwnedAfterModifiersChange() {
		TaskbarShortcutFilter filter;
		QVERIFY(filter.handle('T', false, false, ModSuper, false).swallow);
		QVERIFY(filter.handle('T', false, false, ModCtrl, false).swallow);
		QVERIFY(filter.handle('T', true, false, 0, false).swallow);
		QVERIFY(!filter.handle('T', true, false, 0, false).swallow);
		auto release = filter.handle(VK_LWIN, true, false, 0, false);
		QVERIFY(release.maskedRelease);
		QVERIFY(!filter.handle(VK_LWIN, true, false, 0, false).maskedRelease);
	}

	void taskbarWaitsForLastWinAndIgnoresOwnInjections() {
		TaskbarShortcutFilter filter;
		filter.handle(VK_LWIN, false, false, 0, false);
		filter.handle(VK_RWIN, false, false, ModSuper, true);
		QVERIFY(!filter.handle('T', false, true, ModSuper, true).swallow);
		QVERIFY(filter.handle('T', false, false, ModSuper, true).swallow);
		QVERIFY(!filter.handle(VK_LWIN, true, false, 0, true).maskedRelease);
		QVERIFY(filter.handle(VK_RWIN, true, false, 0, false).maskedRelease);
	}

	void taskbarNewSequenceAndResetClearPendingMask() {
		TaskbarShortcutFilter filter;
		filter.handle(VK_LWIN, false, false, 0, false);
		filter.handle('1', false, false, ModSuper, false);
		// Repeats preserve the pending mask.
		filter.handle(VK_LWIN, false, false, ModSuper, false);
		QVERIFY(filter.handle(VK_LWIN, true, false, 0, false).maskedRelease);
		filter.handle(VK_LWIN, false, false, 0, false);
		filter.handle('2', false, false, ModSuper, false);
		// Only our injected up was seen, then a new physical sequence.
		filter.handle(VK_LWIN, true, true, 0, false);
		filter.handle(VK_LWIN, false, false, 0, false);
		QVERIFY(!filter.handle(VK_LWIN, true, false, 0, false).maskedRelease);
		filter.reset();
		QVERIFY(!filter.handle('2', true, false, 0, false).swallow);
		QVERIFY(!filter.handle(-1, false, false, ModSuper, false).swallow);
		QVERIFY(!filter.handle(256, false, false, ModSuper, false).swallow);
	}

	void taskbarBindsWinWithoutClaimingTheirRelease() {
		TaskbarShortcutFilter filter;
		QVERIFY(!filter.handle('B', false, false, ModSuper, false, true).swallow);
		QVERIFY(!filter.handle(VK_LWIN, true, false, 0, false).maskedRelease);
	}

	void winLeaseTimerOnlyRunsDuringTrackedHolds() {
		physical('A', false, 100);
		QCOMPARE(desktop.timersArmed, 0);
		physical(VK_LWIN, false, 110);
		QCOMPARE(desktop.timersArmed, 1);
		physical(VK_RWIN, false, 120);
		physical(VK_LWIN, true, 130);
		QCOMPARE(desktop.timersArmed, 1);
		QCOMPARE(desktop.timersKilled, 0);
		physical(VK_RWIN, true, 140);
		QCOMPARE(desktop.timersKilled, 1);
		physical(VK_LWIN, false, 200);
		expireWin(200 + WIN_LEASE_MS);
		drainInput();
		QCOMPARE(desktop.timersArmed, 2);
		QCOMPARE(desktop.timersKilled, 2);
		QVERIFY(!gWinDown.load());
	}

	// Routing stopping must not strand physical state: the Super down the hook
	// swallowed still has to clear on its release, or ordinary clicks become
	// Super drags forever.
	void stoppingRoutingKeepsPhysicalSuperStateTracking() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'O'}, .id = 1}
		);
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		physical('O', false, 110);
		QVERIFY(KeyboardHook::superDown());

		// unregisterAll removes the snapshot, then stops the hook's routing.
		KeyboardHook::setSnapshot(nullptr);
		KeyboardHook::stop();

		physical('O', true, 120);
		physical(VK_LWIN, true, 130);
		QVERIFY(!KeyboardHook::superDown());
	}

	// A key the old binds held is dropped with the routing: its release passes
	// and a restarted map must not see it as the old bind's repeat.
	void restartedRoutingDoesNotMatchAKeyHeldBeforeTheStop() {
		auto first = std::make_shared<HookSnapshot>();
		first->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'O'}, .id = 1}
		);
		KeyboardHook::setSnapshot(first);
		physical(VK_LWIN, false, 100);
		physical('O', false, 110);
		KeyboardHook::setSnapshot(nullptr);
		KeyboardHook::stop();
		physical('O', true, 120);
		physical(VK_LWIN, true, 130);

		auto second = std::make_shared<HookSnapshot>();
		second->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'E'}, .id = 2}
		);
		KeyboardHook::setSnapshot(second);

		desktop.delivered.clear();
		physical('O', false, 200);

		auto delivered = std::ranges::any_of(desktop.delivered, [](const KBDLLHOOKSTRUCT& info) {
			return info.vkCode == 'O' && (info.flags & LLKHF_UP) == 0;
		});
		QVERIFY2(delivered, "an unbound O after restart was consumed as the old bind's repeat");

		physical('O', true, 210);
		KeyboardHook::setSnapshot(nullptr);
	}

	// Esc cancelling a Super drag must not depend on configured keybinds. The
	// hook is shared: the hotkey manager's routing and a feature's holder are
	// both needs, and the hook runs while either is set.
	void hookOwnershipKeepsTheHookForAHolder() {
		HookOwnership ownership;

		QVERIFY(!ownership.needed());
		ownership.hold();
		QVERIFY(ownership.needed());
		ownership.drop();
		QVERIFY(!ownership.needed());
	}

	void hookOwnershipIgnoresUnpairedDrops() {
		HookOwnership ownership;

		// A spurious release must not take the hook from another owner.
		ownership.drop();
		QCOMPARE(ownership.holders(), 0);
		QVERIFY(!ownership.needed());

		ownership.hold();
		ownership.hold();
		ownership.drop();
		QVERIFY(ownership.needed());
		ownership.drop();
		QVERIFY(!ownership.needed());
	}

	void hookOwnershipKeepsTheHookWhileRoutingEnds() {
		HookOwnership ownership;

		ownership.setRouting(true);
		ownership.hold();
		QVERIFY(ownership.needed());

		// No hook-backed bind is left, but the holder still needs the hook.
		ownership.setRouting(false);
		QVERIFY(ownership.needed());
		ownership.drop();
		QVERIFY(!ownership.needed());

		// Routing alone keeps it too, so a stop after an unpaired start is safe.
		ownership.setRouting(true);
		QVERIFY(ownership.needed());
		ownership.setRouting(false);
		QVERIFY(!ownership.needed());
	}

	void submapShutdownReleasesForwardedWinAfterSnapshotRemoval() {
		physical(VK_LWIN, false, 100);
		physical('E', false, 110); // Native chord forwards the deferred Win-down.
		QVERIFY(desktop.down[VK_LWIN]);
		KeyboardHook::setSnapshot(nullptr); // unregisterAll precedes stop/reload.
		resetPhysicalState();
		drainInput();
		QVERIFY(!desktop.down[VK_LWIN]);
		QVERIFY(!KeyboardHook::superDown());
	}

	void submapModifierTapsPostTheSelectedTrigger_data() {
		QTest::addColumn<int>("vk");
		QTest::addColumn<int>("modifier");
		QTest::newRow("ctrl") << int(VK_LCONTROL) << int(ModCtrl);
		QTest::newRow("shift") << int(VK_LSHIFT) << int(ModShift);
		QTest::newRow("alt") << int(VK_LMENU) << int(ModAlt);
		QTest::newRow("super") << int(VK_LWIN) << int(ModSuper);
	}

	void submapModifierTapsPostTheSelectedTrigger() {
		QFETCH(int, vk);
		QFETCH(int, modifier);
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->serial = 613;
		snapshot->triggers.push_back({
		    .kind = HookTrigger::Tap,
		    .combo = {.lone = static_cast<uint8_t>(modifier)},
		    .id = 27,
		});
		KeyboardHook::setSnapshot(snapshot);
		physical(vk, false, 100);
		physical(vk, true, 200);
		QCOMPARE(taps(), 1);
		QCOMPARE(desktop.hookPosts.size(), size_t(1));
		QCOMPARE(desktop.hookPosts[0].message, UINT(WM_APP + HookTapped));
		QCOMPARE(desktop.hookPosts[0].wParam, WPARAM(27));
		QCOMPARE(desktop.hookPosts[0].lParam, LPARAM(613));
		QVERIFY(!desktop.down.at(vk));
	}

	void submapAltTapAfterSuperDragPostsEvenWhenInjectionFails() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->serial = 614;
		snapshot->triggers = {
		    {.kind = HookTrigger::Tap, .combo = {.lone = ModSuper}, .id = 1},
		    {.kind = HookTrigger::Tap, .combo = {.lone = ModAlt}, .id = 2},
		};
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		KeyboardHook::noteSuperChord();
		physical(VK_LWIN, true, 200);
		desktop.failInjection = true;
		physical(VK_LMENU, false, 300);
		physical(VK_LMENU, true, 400);
		QCOMPARE(taps(), 1);
		QCOMPARE(desktop.hookPosts.back().wParam, WPARAM(2));
		QCOMPARE(desktop.hookPosts.back().lParam, LPARAM(614));
		QVERIFY(!desktop.down[VK_LWIN]);
		QVERIFY(!desktop.down[VK_LMENU]); // Failed injection passes physical Alt-up.
	}

	void submapSwitchKeepsConsumedKeyUpsAndNativeChordsIntact() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->serial = 615;
		snapshot->triggers.push_back({
		    .kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'R'}, .id = 7});
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		physical('R', false, 110);
		auto submap = std::make_shared<HookSnapshot>();
		submap->serial = 616;
		submap->triggers.push_back({
		    .kind = HookTrigger::Combo, .combo = {.vk = 'H'}, .id = 8});
		KeyboardHook::setSnapshot(submap);
		physical('R', true, 120);
		physical(VK_LWIN, true, 130);
		QCOMPARE(desktop.posted, (std::vector<HookEvent>{HookPressed, HookReleased}));
		QCOMPARE(desktop.hookPosts.back().wParam, WPARAM(7));
		QCOMPARE(desktop.hookPosts.back().lParam, LPARAM(615));
		QVERIFY(desktop.delivered.empty());
		QVERIFY(!desktop.down[VK_LWIN]);
		physical(VK_LWIN, false, 200);
		physical('E', false, 210);
		physical('E', true, 220);
		physical(VK_LWIN, true, 230);
		QCOMPARE(desktop.delivered.size(), size_t(4)); // Native Win+E passes.
		QCOMPARE(desktop.posted.size(), size_t(2));
		QVERIFY(!desktop.down[VK_LWIN]);
	}

	void submapSnapshotSwitchCancelsTheOldModifierTap() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->serial = 10;
		snapshot->triggers.push_back({.kind = HookTrigger::Tap, .combo = {.lone = ModCtrl}});
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LCONTROL, false, 100);
		auto next = std::make_shared<HookSnapshot>(*snapshot);
		next->serial = 11;
		KeyboardHook::setSnapshot(next);
		physical(VK_LCONTROL, true, 200);
		QCOMPARE(taps(), 0); // A press in the old map is not a tap in the new one.
		QVERIFY(!desktop.down[VK_LCONTROL]);
	}

	void init() {
		desktop = {};
		gTaskbarFilter.reset();
		gWinLeaseTimer = 0;
		gHeld.fill({});
		for (auto& masked: gMaskHeld) masked = false;
		for (auto& replayed: gReplayed) replayed = false;
		gTapCandidate = gHoldActive = 0;
		gSuperChordConsumed = false;
		gEscapeClaimed = false;
		gEscapeFilter.store(nullptr);
		gWin = {};
		gWinDown.store(false);
		gSuppressLoneSuperTap.store(false);
		gForegroundBlocked.store(false);
		gThread = nullptr;
		gFailed.store(false);
		for (auto& down: gPhysicalDown) down.store(false);
		for (auto& swallowed: gPhysicalSwallowed) swallowed = false;
		gInputObserver.store(nullptr);
		gInputObserverMessage.store(0);
		gInputObserverResetMessage.store(0);
		gTarget.store(reinterpret_cast<HWND>(1));
		gMessageBase.store(WM_APP);
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back({.kind = HookTrigger::Tap, .combo = {.lone = ModSuper}});
		KeyboardHook::setSnapshot(snapshot);
	}

	void claimedEscapeConsumesRepeatsAndRelease() {
		KeyboardHook::setEscapeFilter(+[] { return true; });
		physical(VK_ESCAPE, false, 100);
		KeyboardHook::setEscapeFilter(nullptr);
		physical(VK_ESCAPE, false, 110);
		physical(VK_ESCAPE, true, 120);
		QVERIFY(desktop.delivered.empty());
		QVERIFY(!gEscapeClaimed);
		physical(VK_ESCAPE, false, 200);
		physical(VK_ESCAPE, true, 210);
		QCOMPARE(desktop.delivered.size(), size_t(2));
		QCOMPARE(desktop.injections, 0);
	}

	void inputResetClearsClaimedEscape_data() {
		QTest::addColumn<bool>("desktopSwitch");
		QTest::newRow("hook-reset") << false;
		QTest::newRow("desktop-switch") << true;
	}

	void inputResetClearsClaimedEscape() {
		QFETCH(bool, desktopSwitch);
		KeyboardHook::setEscapeFilter(+[] { return true; });
		physical(VK_ESCAPE, false, 100);
		QVERIFY(gEscapeClaimed);
		QVERIFY(desktop.delivered.empty());

		// The release happens on another desktop or while the hook is stopped.
		if (desktopSwitch) {
			inputDesktopChanged(nullptr, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, 0, 0, 0, 0);
		} else {
			resetPhysicalState();
		}
		KeyboardHook::setEscapeFilter(nullptr);
		QVERIFY(!gEscapeClaimed);
		physical(VK_ESCAPE, false, 200);
		physical(VK_ESCAPE, true, 210);
		QCOMPARE(desktop.delivered.size(), size_t(2));
		QVERIFY(!desktop.down[VK_ESCAPE]);
		QCOMPARE(desktop.injections, 0);
	}

	void bareWinSurvivesAMaskDiscardingHook_data() {
		QTest::addColumn<int>("vk");
		QTest::newRow("left") << int(VK_LWIN);
		QTest::newRow("right") << int(VK_RWIN);
	}

	void bareWinSurvivesAMaskDiscardingHook() {
		QFETCH(int, vk);
		desktop.discardMasks = true;
		physical(vk, false, 100);
		physical(vk, true, 200);
		QCOMPARE(taps(), 1);
		QVERIFY(!desktop.startOpened);
		QVERIFY(!desktop.down.at(vk));
		QCOMPARE(desktop.maskChainCalls, 0);
		QCOMPARE(desktop.otherChainCalls, 0);
		QCOMPARE(desktop.injections, 0);
	}

	void unboundWinChordStillPassesThrough() {
		physical(VK_LWIN, false, 100);
		physical('E', false, 120);
		physical('E', true, 150);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
		QVERIFY(!desktop.startOpened);
		QCOMPARE(desktop.delivered.size(), size_t(4));
		QCOMPARE(desktop.otherChainCalls, 4);
	}

	void newerHookCannotBreakBareWin_data() {
		QTest::addColumn<int>("vk");
		QTest::addColumn<bool>("mutate");
		QTest::newRow("left-eats-mask") << int(VK_LWIN) << false;
		QTest::newRow("right-eats-mask") << int(VK_RWIN) << false;
		QTest::newRow("left-rewrites-mask-down-to-up") << int(VK_LWIN) << true;
		QTest::newRow("right-rewrites-mask-down-to-up") << int(VK_RWIN) << true;
	}

	void newerHookCannotBreakBareWin() {
		QFETCH(int, vk);
		QFETCH(bool, mutate);
		desktop.filterBeforeUs = true;
		desktop.discardMasks = !mutate;
		desktop.mutateMasks = mutate;
		physical(vk, false, 100);
		physical(vk, true, 200);
		QCOMPARE(taps(), 1);
		QVERIFY(!desktop.startOpened);
		QVERIFY(!desktop.down[VK_LWIN]);
		// A bare tap must not depend on ANY injected input reaching Windows.
		QVERIFY(desktop.delivered.empty());
		QCOMPARE(desktop.maskChainCalls, 0);
	}

	void swallowedWinChordNeverExposesWin() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'V'}}
		);
		KeyboardHook::setSnapshot(snapshot);
		desktop.discardMasks = true;
		physical(VK_LWIN, false, 100);
		physical('V', false, 120);
		physical('V', true, 150);
		physical(VK_LWIN, true, 200);
		QVERIFY(!desktop.startOpened);
		QVERIFY(!desktop.down[VK_LWIN]);
		QCOMPARE(taps(), 0);
		QCOMPARE(desktop.maskChainCalls, 0);
		QCOMPARE(desktop.injections, 0);
		QVERIFY(std::none_of(desktop.delivered.begin(), desktop.delivered.end(), [](const auto& info) {
			return info.vkCode == 'V';
		}));
	}

	void foreignMasksAndOrdinaryInjectionsStillChain() {
		desktop.discardMasks = true;
		dispatch(
		    KBDLLHOOKSTRUCT {
		        .vkCode = KeyboardHook::MASK_KEY,
		        .flags = LLKHF_INJECTED,
		        .dwExtraInfo = 123,
		    }
		);
		dispatch(
		    KBDLLHOOKSTRUCT {
		        .vkCode = 'A',
		        .flags = LLKHF_INJECTED,
		        .dwExtraInfo = KeyboardHook::INJECTION_MARKER,
		    }
		);
		QCOMPARE(desktop.maskChainCalls, 1);
		QCOMPARE(desktop.otherChainCalls, 1);
	}

	void blockedForegroundKeepsThePrivilegeBoundary() {
		gForegroundBlocked.store(true);
		physical(VK_LWIN, false, 100);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
		QVERIFY(desktop.startOpened);
		QCOMPARE(desktop.delivered.size(), size_t(2));
	}

	void failedInjectionNeverLeavesWinDown() {
		desktop.failInjection = true;
		physical(VK_LWIN, false, 100);
		physical(VK_LWIN, true, 200);
		QVERIFY(!desktop.down[VK_LWIN]);
		QVERIFY(!desktop.startOpened);
		QCOMPARE(taps(), 1);
		QCOMPARE(desktop.injections, 0);
	}

	void newerHookCannotBreakShellChord() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back({.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'V'}});
		KeyboardHook::setSnapshot(snapshot);
		desktop.filterBeforeUs = desktop.discardInjected = true;
		physical(VK_LWIN, false, 100);
		physical('V', false, 120);
		physical('V', false, 140);
		physical('V', true, 150);
		physical(VK_LWIN, true, 200);
		QVERIFY(!desktop.startOpened);
		QVERIFY(desktop.delivered.empty());
		QCOMPARE(desktop.injections, 0);
		QCOMPARE(desktop.posted, (std::vector<HookEvent>{HookPressed, HookRepeated, HookReleased}));
	}

	void repeatDoesNotRestartTapOrHold() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers = {
		    {.kind = HookTrigger::Tap, .combo = {.lone = ModSuper}},
		    {.kind = HookTrigger::Hold, .combo = {.lone = ModSuper}},
		};
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		QVERIFY(KeyboardHook::superDown());
		QVERIFY(!desktop.down[VK_LWIN]);
		physical(VK_LWIN, false, 900);
		physical(VK_LWIN, false, 1600);
		physical(VK_LWIN, true, 1700);
		QCOMPARE(taps(), 0); // Timeout is measured from the first down.
		QCOMPARE(desktop.posted, (std::vector<HookEvent>{HookPressed, HookReleased}));
		QVERIFY(!KeyboardHook::superDown());
		QVERIFY(!desktop.startOpened);
	}

	void twoWinKeysDoNotCreateATap() {
		physical(VK_LWIN, false, 100);
		physical(VK_RWIN, false, 120);
		physical(VK_LWIN, true, 150);
		QVERIFY(KeyboardHook::superDown());
		physical(VK_RWIN, true, 200);
		QVERIFY(!KeyboardHook::superDown());
		QCOMPARE(taps(), 0);
		QVERIFY(desktop.delivered.empty());
	}

	void shiftSuperChordUsesThePhysicalHold() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back({.kind = HookTrigger::Combo, .combo = {.mods = ModSuper | ModShift, .vk = 'S'}});
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		physical(VK_LSHIFT, false, 110);
		physical('S', false, 120);
		physical('S', true, 130);
		physical(VK_LSHIFT, true, 150);
		physical(VK_LWIN, true, 200);
		QCOMPARE(desktop.posted, (std::vector<HookEvent>{HookPressed, HookReleased}));
		QVERIFY(!desktop.startOpened);
		QCOMPARE(desktop.delivered.size(), size_t(2)); // Shift only.
	}

	void dragSuppressesTheTapWithoutExposingWin() {
		physical(VK_LWIN, false, 100);
		KeyboardHook::noteSuperChord();
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
		QVERIFY(desktop.delivered.empty());
	}

	void superTapWithoutMouseUseIsDelivered() {
		physical(VK_LWIN, false, 100);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 1);
	}

	void superDragConsumesThePendingTap() {
		physical(VK_LWIN, false, 100);
		mouse(WM_LBUTTONDOWN, 110);
		mouse(WM_MOUSEMOVE, 120);
		mouse(WM_MOUSEMOVE, 130);
		mouse(WM_LBUTTONUP, 140);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
	}

	void superClickConsumesThePendingTap() {
		physical(VK_LWIN, false, 100);
		mouse(WM_LBUTTONDOWN, 110);
		mouse(WM_LBUTTONUP, 120);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
	}

	void everyMouseButtonConsumesThePendingTap_data() {
		QTest::addColumn<int>("down");
		QTest::addColumn<int>("up");
		QTest::newRow("left") << int(WM_LBUTTONDOWN) << int(WM_LBUTTONUP);
		QTest::newRow("right") << int(WM_RBUTTONDOWN) << int(WM_RBUTTONUP);
		QTest::newRow("middle") << int(WM_MBUTTONDOWN) << int(WM_MBUTTONUP);
		QTest::newRow("x") << int(WM_XBUTTONDOWN) << int(WM_XBUTTONUP);
	}

	void everyMouseButtonConsumesThePendingTap() {
		QFETCH(int, down);
		QFETCH(int, up);
		physical(VK_LWIN, false, 100);
		mouse(static_cast<WPARAM>(down), 110);
		mouse(static_cast<WPARAM>(up), 120);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
	}

	void pointerMotionAloneKeepsThePendingTap() {
		physical(VK_LWIN, false, 100);
		mouse(WM_MOUSEMOVE, 110);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 1);
	}

	void pointerUseBeforeSuperDownKeepsThePendingTap() {
		mouse(WM_LBUTTONDOWN, 90);
		mouse(WM_LBUTTONUP, 95);
		physical(VK_LWIN, false, 100);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 1);
	}

	void registeredChordStillReachesWindows_data() {
		QTest::addColumn<int>("vk");
		QTest::newRow("ordinary") << int('I');
		QTest::newRow("taskbar-policy") << int('B');
	}

	void registeredChordStillReachesWindows() {
		QFETCH(int, vk);
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->masks.push_back({.mods = ModSuper, .vk = static_cast<uint8_t>(vk)});
		KeyboardHook::setSnapshot(snapshot);
		desktop.registeredVk = vk;
		physical(VK_LWIN, false, 100);
		physical(vk, false, 120);
		physical(vk, true, 150);
		physical(VK_LWIN, true, 200);
		QCOMPARE(desktop.hotkeys, 1);
		QVERIFY(!desktop.startOpened);
		QVERIFY(!desktop.down[VK_LWIN]);
	}

	void replayFailureDoesNotStrandTheModifier_data() {
		QTest::addColumn<int>("limit");
		QTest::newRow("nothing-inserted") << 0;
		QTest::newRow("only-win-inserted") << 1;
	}

	void replayFailureDoesNotStrandTheModifier() {
		QFETCH(int, limit);
		desktop.maxInjection = limit;
		physical(VK_LWIN, false, 100);
		physical('E', false, 120);
		physical('E', true, 150);
		physical(VK_LWIN, true, 200);
		QVERIFY(!desktop.down[VK_LWIN]);
		QVERIFY(!KeyboardHook::superDown());
		QCOMPARE(taps(), 0);
	}

	void queuedReleaseFollowsTheReplay() {
		physical(VK_LWIN, false, 100);
		dispatch({.vkCode = 'E', .time = 120});
		dispatch({.vkCode = 'E', .time = 125}); // Queued auto-repeat.
		dispatch({.vkCode = 'E', .flags = LLKHF_UP, .time = 128});
		dispatch({.vkCode = VK_LWIN, .flags = LLKHF_UP, .time = 130});
		drainInput();
		QCOMPARE(desktop.delivered.size(), size_t(5));
		QCOMPARE(desktop.delivered[0].vkCode, DWORD(VK_LWIN));
		QCOMPARE(desktop.delivered[1].vkCode, DWORD('E'));
		QCOMPARE(desktop.delivered[4].vkCode, DWORD(VK_LWIN));
		QVERIFY(!desktop.down[VK_LWIN]);
		QVERIFY(!desktop.down['E']);
		QVERIFY(!desktop.startOpened);
	}

	void remoteCaptureDoesNotBecomeALocalTap() {
		desktop.filterBeforeUs = desktop.captureWin = true;
		physical(VK_LWIN, false, 100);
		physical(VK_LWIN, true, 200);
		QVERIFY(!KeyboardHook::superDown());
		QCOMPARE(taps(), 0);
		QVERIFY(!desktop.startOpened);
		desktop.captureWin = false;
		physical(VK_LWIN, false, 300);
		physical(VK_LWIN, true, 400);
		QCOMPARE(taps(), 1);
	}

	void lostReleaseExpiresWithoutATap() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers = {
		    {.kind = HookTrigger::Tap, .combo = {.lone = ModSuper}},
		    {.kind = HookTrigger::Hold, .combo = {.lone = ModSuper}},
		};
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		desktop.filterBeforeUs = desktop.captureWin = true;
		physical(VK_LWIN, true, 200);
		expireWin(100 + WIN_LEASE_MS);
		drainInput();
		QVERIFY(!KeyboardHook::superDown());
		QCOMPARE(taps(), 0);
		QCOMPARE(desktop.posted, (std::vector<HookEvent>{HookPressed, HookReleased}));
		QVERIFY(!desktop.startOpened);
		desktop.captureWin = false;
		physical('A', false, 100 + WIN_LEASE_MS + 10);
		QVERIFY(!desktop.down[VK_LWIN]);
		QCOMPARE(desktop.delivered.back().vkCode, DWORD('A'));
	}

	void foregroundBecomingBlockedStillClosesTheOwnedSequence() {
		physical(VK_LWIN, false, 100);
		gForegroundBlocked.store(true);
		physical(VK_LWIN, true, 200);
		QCOMPARE(taps(), 0);
		QVERIFY(!desktop.startOpened);
		QVERIFY(!KeyboardHook::superDown());
	}

	void freshPressRecoversBeforeTheLeaseExpires() {
		physical(VK_LWIN, false, 100);
		desktop.filterBeforeUs = desktop.captureWin = true;
		physical(VK_LWIN, true, 200);
		desktop.captureWin = false;
		physical(VK_LWIN, false, 2000);
		physical(VK_LWIN, true, 2100);
		QCOMPARE(taps(), 1);
		QVERIFY(!KeyboardHook::superDown());
		QVERIFY(!desktop.startOpened);
	}

	void armingCapturesAndSubscribesInOneStep() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'O'}}
		);
		KeyboardHook::setSnapshot(snapshot);

		physical(VK_LWIN, false, 100);
		physical('O', false, 110);

		InputObserverRequest request {
		    .target = reinterpret_cast<HWND>(2),
		    .keyMessage = WM_APP + 50,
		    .resetMessage = WM_APP + 51,
		    .done = nullptr,
		    .keys = {},
		};
		armObserver(request);

		// The hook swallowed Win and O, so neither is in the fake system
		// state; the captured set still has both.
		QCOMPARE(request.keys.size(), size_t(2));
		QVERIFY(std::find(request.keys.begin(), request.keys.end(), VK_LWIN) != request.keys.end());
		QVERIFY(std::find(request.keys.begin(), request.keys.end(), 'O') != request.keys.end());
		QCOMPARE(gInputObserver.load(), reinterpret_cast<HWND>(2));

		// The subscription is already in place, so the next release reaches
		// the observer instead of being lost between snapshot and subscribe.
		physical('O', true, 120);
		QCOMPARE(desktop.observerPosts.size(), size_t(1));
		QCOMPARE(desktop.observerPosts[0].message, UINT(WM_APP + 50));
		QCOMPARE(desktop.observerPosts[0].wParam, WPARAM('O'));
		QCOMPARE(desktop.observerPosts[0].lParam, LPARAM(1));
	}

	void rearmDropsAReleaseTheHookMissed() {
		physical('A', false, 100);
		QVERIFY(gPhysicalDown['A'].load());

		// Windows saw the release; the hook did not. Rearming reconciles the
		// non-swallowed key against the system instead of keeping it down.
		desktop.down['A'] = false;

		InputObserverRequest request {
		    .target = reinterpret_cast<HWND>(2),
		    .keyMessage = WM_APP + 50,
		    .resetMessage = WM_APP + 51,
		    .done = nullptr,
		    .keys = {},
		};
		armObserver(request);
		QCOMPARE(request.keys.size(), size_t(0));
	}

	void rearmKeepsASwallowedHold() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'O'}}
		);
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		QVERIFY(!desktop.down[VK_LWIN]);

		InputObserverRequest request {
		    .target = reinterpret_cast<HWND>(2),
		    .keyMessage = WM_APP + 50,
		    .resetMessage = WM_APP + 51,
		    .done = nullptr,
		    .keys = {},
		};
		armObserver(request);

		// GetAsyncKeyState cannot see the swallowed down, so reconciliation
		// must keep the hook's own hold.
		QVERIFY(std::find(request.keys.begin(), request.keys.end(), VK_LWIN) != request.keys.end());
	}

	void startingSeedsKeysAlreadyDown() {
		desktop.down['A'] = true;
		seedPhysicalState();
		QVERIFY(gPhysicalDown['A'].load());
		QVERIFY(!gPhysicalSwallowed['A']);
	}

	void inputDesktopChangeDropsHoldsAndNotifies() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back(
		    {.kind = HookTrigger::Combo, .combo = {.mods = ModSuper, .vk = 'O'}}
		);
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		physical('O', false, 110);

		InputObserverRequest request {
		    .target = reinterpret_cast<HWND>(2),
		    .keyMessage = WM_APP + 50,
		    .resetMessage = WM_APP + 51,
		    .done = nullptr,
		    .keys = {},
		};
		armObserver(request);
		QCOMPARE(request.keys.size(), size_t(2));

		// Lock screen, UAC or switch user takes the keys without a release.
		inputDesktopChanged(nullptr, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, 0, 0, 0, 0);

		QVERIFY(!gPhysicalDown[VK_LWIN].load());
		QVERIFY(!gPhysicalDown['O'].load());
		QVERIFY(!gWin[0].down);
		QCOMPARE(desktop.observerPosts.size(), size_t(1));
		QCOMPARE(desktop.observerPosts[0].message, UINT(WM_APP + 51));
		QCOMPARE(gInputObserver.load(), reinterpret_cast<HWND>(2));
		QCOMPARE(desktop.hookPosts.back().message, UINT(WM_APP + HookReset));
		QCOMPARE(desktop.hookPosts.back().lParam, LPARAM(snapshot->serial));
	}

	void nonSuperSnapshotDoesNotClaimWin() {
		auto snapshot = std::make_shared<HookSnapshot>();
		snapshot->triggers.push_back({.kind = HookTrigger::Tap, .combo = {.lone = ModCtrl}});
		KeyboardHook::setSnapshot(snapshot);
		physical(VK_LWIN, false, 100);
		physical(VK_LWIN, true, 200);
		QVERIFY(desktop.startOpened);
		QCOMPARE(taps(), 0);
		QCOMPARE(desktop.delivered.size(), size_t(2));
	}

	// The late-input report is armed by late hook input, not by a timer: input
	// that arrives on time counts nothing and opens no window, one window posts
	// a single wakeup however many late events land in it, and the counts stay
	// for the report when the wakeup finds no tracker to arm.
	void lateInputReportOnlyRunsWhileSomethingIsLate() {
		// Settle any wakeup an earlier case posted.
		QCoreApplication::processEvents();
		const auto before = qs::windows::InputMaskTracker::lateReportStateForTesting();
		QVERIFY(!before.armed);

		// Hook input that arrives on time is neither counted nor armed.
		physical('A', false, GetTickCount());
		physical('A', true, GetTickCount());
		mouse(WM_MOUSEMOVE, GetTickCount());
		const auto onTime = qs::windows::InputMaskTracker::lateReportStateForTesting();
		QCOMPARE(onTime.keys, before.keys);
		QCOMPARE(onTime.mouse, before.mouse);
		QVERIFY(!onTime.armed);

		// The first late event opens the report window.
		physical('B', false, GetTickCount() - 500);
		const auto armed = qs::windows::InputMaskTracker::lateReportStateForTesting();
		QCOMPARE(armed.keys, before.keys + 1);
		QVERIFY(armed.armed);

		// A storm inside one window adds counts but no second wakeup.
		mouse(WM_LBUTTONDOWN, GetTickCount() - 500);
		mouse(WM_MOUSEMOVE, GetTickCount() - 500);
		const auto storm = qs::windows::InputMaskTracker::lateReportStateForTesting();
		QCOMPARE(storm.mouse, before.mouse + 2);
		QCOMPARE(storm.keys, before.keys + 1);
		QVERIFY(storm.armed);

		// This fixture never instantiates the tracker, so the wakeup has nothing
		// to arm: the mark clears and the counts wait for the next report.
		QCoreApplication::processEvents();
		const auto drained = qs::windows::InputMaskTracker::lateReportStateForTesting();
		QVERIFY(!drained.armed);
		QCOMPARE(drained.mouse, before.mouse + 2);
		QCOMPARE(drained.keys, before.keys + 1);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	KeyboardHookTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "keyboard_hook_test.moc"
