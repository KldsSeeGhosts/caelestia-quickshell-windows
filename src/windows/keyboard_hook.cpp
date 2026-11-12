#include "keyboard_hook.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <windows.h>

#include "input_mask.hpp"
#include "util.hpp"

namespace qs::windows::hotkeys {

namespace {

Q_LOGGING_CATEGORY(logKeyboardHook, "quickshell.windows.hotkeys", QtInfoMsg);

std::atomic<std::shared_ptr<const HookSnapshot>> gSnapshot; // NOLINT
std::atomic<HWND> gTarget = nullptr;                         // NOLINT
std::atomic<UINT> gMessageBase = 0;                          // NOLINT
std::atomic<DWORD> gThreadId = 0;                            // NOLINT
std::atomic<bool> gStopRequested = false;                    // NOLINT
std::atomic<bool> gFailed = false;                           // NOLINT
std::atomic<bool> gForegroundBlocked = false; // NOLINT
std::atomic<bool> gSuppressLoneSuperTap = false; // NOLINT
std::atomic<bool (*)()> gEscapeFilter = nullptr; // NOLINT

std::thread* gThread = nullptr; // NOLINT

struct HeldKey {
	uint32_t id = 0;
	uint32_t serial = 0;
	DWORD lastTime = 0;
	bool active = false;
};

std::array<HeldKey, 256> gHeld {}; // NOLINT
bool gMaskHeld[256] = {};          // NOLINT
uint8_t gTapCandidate = 0;         // NOLINT
DWORD gTapStart = 0;               // NOLINT
uint8_t gHoldActive = 0;           // NOLINT
// A Super chord was swallowed since the Windows key went down. If an earlier
// native chord exposed Win-down, keep the legacy masked-release fallback.
bool gSuperChordConsumed = false; // NOLINT
bool gEscapeClaimed = false; // NOLINT
TaskbarShortcutFilter gTaskbarFilter; // NOLINT
UINT_PTR gWinLeaseTimer = 0;          // NOLINT
// Suppressed Win downs do not update GetAsyncKeyState. Keep the two sides
// separately, and publish only the physical hold for the mouse hook.
struct WinKey {
	bool down = false;
	bool forwarded = false;
	// The hook swallowed this down, so its repeat and release belong to the
	// hook as well. A down that passed through must pass its release too.
	bool consumed = false;
	DWORD lastTime = 0;
};
std::array<WinKey, 2> gWin {}; // NOLINT
std::atomic<bool> gWinDown = false; // NOLINT
bool gReplayed[256] = {}; // NOLINT

// Set only while a caller waits for physical keys to come up. Posting every
// transition lets that caller see the release that matters and cancel on new
// input without a second hook or a polling loop.
std::atomic<HWND> gInputObserver = nullptr; // NOLINT
std::atomic<UINT> gInputObserverMessage = 0; // NOLINT
std::atomic<UINT> gInputObserverResetMessage = 0; // NOLINT
// Physical state as the hook sees it, including keys it swallows before
// Windows updates GetAsyncKeyState (the Win key and a hook-backed chord key).
// `gPhysicalSwallowed` marks a down the hook consumed, so reconciliation must
// leave it to the hook's own release instead of trusting the system state.
std::atomic<bool> gPhysicalDown[256] {}; // NOLINT
bool gPhysicalSwallowed[256] = {};       // NOLINT

// The hook thread's queue is the serialization point for physical state: a
// snapshot and its observer subscription are both taken there, so a release
// cannot slip between them.
constexpr UINT WM_QS_ARM_OBSERVER = WM_APP + 0x40;
constexpr DWORD HOOK_START_TIMEOUT_MS = 5000;

struct InputObserverRequest {
	HWND target = nullptr;
	UINT keyMessage = 0;
	UINT resetMessage = 0;
	HANDLE done = nullptr;
	std::vector<uint8_t> keys;
};

// Signalled once the hook is installed (or failed), and once the thread has
// stopped touching physical state.
std::atomic<HANDLE> gReadyEvent = nullptr; // NOLINT
std::atomic<HANDLE> gExitEvent = nullptr;  // NOLINT

// Shared ownership: the hotkey manager's routing and any feature that needs
// the hook while it runs (Super drags need Esc) both keep it installed.
HookOwnership gOwnership; // NOLINT

// Set when routing stops, applied on the hook thread before the next key. It
// drops bind-owned keys so their releases pass through and a restarted map
// cannot match a key that is already held.
std::atomic<bool> gRoutingStopped = false; // NOLINT

// What the handlers match when no routing is configured. Physical and
// modifier state is still tracked through it; nothing is posted or swallowed.
const HookSnapshot gNoRouting; // NOLINT

constexpr DWORD STALE_REPEAT_MS = 1500;
// A hook ahead of us can eat a release, or Win+L can change input desktops.
// Do not retain local holds forever in that case. Win repeats refresh this
// lease; a motionless hold longer than this is deliberately cancelled.
constexpr DWORD WIN_LEASE_MS = 30000;

void updateWinLeaseTimer() {
	if (gWin[0].down || gWin[1].down) {
		if (gWinLeaseTimer == 0) gWinLeaseTimer = SetTimer(nullptr, 0, 1000, nullptr);
	} else if (gWinLeaseTimer != 0) {
		KillTimer(nullptr, gWinLeaseTimer);
		gWinLeaseTimer = 0;
	}
}

bool isDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

uint8_t heldModifiers() {
	uint8_t mods = 0;
	if (isDown(VK_CONTROL)) mods |= ModCtrl;
	if (isDown(VK_MENU)) mods |= ModAlt;
	if (isDown(VK_SHIFT)) mods |= ModShift;
	if (gWinDown.load() || isDown(VK_LWIN) || isDown(VK_RWIN)) mods |= ModSuper;
	return mods;
}

int otherSide(DWORD vk) {
	switch (vk) {
	case VK_LWIN: return VK_RWIN;
	case VK_RWIN: return VK_LWIN;
	case VK_LCONTROL: return VK_RCONTROL;
	case VK_RCONTROL: return VK_LCONTROL;
	case VK_LMENU: return VK_RMENU;
	case VK_RMENU: return VK_LMENU;
	case VK_LSHIFT: return VK_RSHIFT;
	case VK_RSHIFT: return VK_LSHIFT;
	default: return 0;
	}
}

bool isExtendedKey(uint8_t vk) {
	switch (vk) {
	case VK_INSERT:
	case VK_DELETE:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_LEFT:
	case VK_RIGHT:
	case VK_UP:
	case VK_DOWN:
	case VK_LWIN:
	case VK_RWIN:
	case VK_APPS:
	case VK_RCONTROL:
	case VK_RMENU:
	case VK_DIVIDE:
	case VK_SNAPSHOT: return true;
	default: return vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2;
	}
}

INPUT keyInput(uint8_t vk, bool up, WORD scan = 0, bool extended = false) {
	INPUT input {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk;
	input.ki.wScan = scan;
	input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
	input.ki.dwExtraInfo = KeyboardHook::INJECTION_MARKER;
	return input;
}

INPUT chordInput(uint8_t vk, bool up) {
	auto scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
	return keyInput(vk, up, scan, isExtendedKey(vk));
}

INPUT replayInput(const KBDLLHOOKSTRUCT* info) {
	return keyInput(
	    static_cast<uint8_t>(info->vkCode),
	    (info->flags & LLKHF_UP) != 0,
	    static_cast<WORD>(info->scanCode),
	    (info->flags & LLKHF_EXTENDED) != 0
	);
}

void injectMask() {
	std::array<INPUT, 2> inputs {
	    keyInput(KeyboardHook::MASK_KEY, false),
	    keyInput(KeyboardHook::MASK_KEY, true),
	};
	SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

bool injectMaskedRelease(const KBDLLHOOKSTRUCT* original) {
	std::array<INPUT, 3> inputs {
	    keyInput(KeyboardHook::MASK_KEY, false),
	    keyInput(KeyboardHook::MASK_KEY, true),
	    keyInput(
	        static_cast<uint8_t>(original->vkCode),
	        true,
	        static_cast<WORD>(original->scanCode),
	        (original->flags & LLKHF_EXTENDED) != 0
	    ),
	};
	// A swallowed release whose replay did not make it in would leave the
	// modifier logically down, so the caller has to know.
	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT))
	    == inputs.size();
}

void post(HookEvent event, uint32_t id, uint32_t serial) {
	auto* target = gTarget.load();
	if (target == nullptr) return;
	PostMessageW(target, gMessageBase.load() + event, id, static_cast<LPARAM>(serial));
}

void postAll(
    const HookSnapshot& snapshot,
    HookTrigger::Kind kind,
    uint8_t lone,
    HookEvent event
) {
	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind == kind && trigger.combo.lone == lone) post(event, trigger.id, snapshot.serial);
	}
}

bool hasTrigger(const HookSnapshot& snapshot, HookTrigger::Kind kind, uint8_t lone) {
	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind == kind && trigger.combo.lone == lone) return true;
	}
	return false;
}

void cancelWin(const HookSnapshot* snapshot) {
	for (size_t i = 0; i < gWin.size(); i++) {
		if (gWin[i].forwarded) {
			auto input = chordInput(i == 0 ? VK_LWIN : VK_RWIN, true);
			SendInput(1, &input, sizeof(INPUT));
		}
		gWin[i] = {};
	}
	gWinDown.store(false);
	updateWinLeaseTimer();
	if ((gHoldActive & ModSuper) != 0) {
		gHoldActive &= ~ModSuper;
		if (snapshot != nullptr) postAll(*snapshot, HookTrigger::Hold, ModSuper, HookReleased);
	}
	if (gTapCandidate == ModSuper) gTapCandidate = 0;
	gSuppressLoneSuperTap.store(false);
	gSuperChordConsumed = false;
}

void expireWin(DWORD now) {
	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	if (snapshot == nullptr) return;
	for (const auto& key: gWin) {
		if (key.down && now - key.lastTime >= WIN_LEASE_MS) {
			cancelWin(snapshot.get());
			break;
		}
	}
}

std::vector<uint8_t> physicalKeys() {
	auto keys = std::vector<uint8_t>();

	for (int vk = 0x08; vk <= 0xFE; ++vk) {
		// The generic modifier aliases duplicate a side-specific key the hook
		// reports separately. The mask key is injected by the hook itself.
		if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
		if (vk == KeyboardHook::MASK_KEY) continue;
		if (gPhysicalDown[vk].load(std::memory_order_acquire)) keys.push_back(static_cast<uint8_t>(vk));
	}

	return keys;
}

// The hook starts after a registered chord's keys may already be down, so seed
// its physical view from Windows once. A later rearm reconciles instead.
void seedPhysicalState() {
	for (int vk = 0x08; vk <= 0xFE; ++vk) {
		if (vk == KeyboardHook::MASK_KEY) continue;
		gPhysicalDown[vk].store((GetAsyncKeyState(vk) & 0x8000) != 0, std::memory_order_release);
		gPhysicalSwallowed[vk] = false;
	}
}

// A release can be lost to another hook or an input-desktop change. A key the
// hook did not swallow is still visible to GetAsyncKeyState, so clear only
// those; a swallowed key has no system state and keeps the hook's own hold.
void reconcilePhysicalState() {
	for (int vk = 0x08; vk <= 0xFE; ++vk) {
		if (vk == KeyboardHook::MASK_KEY) continue;
		if (!gPhysicalDown[vk].load(std::memory_order_acquire)) continue;
		if (gPhysicalSwallowed[vk]) continue;
		if ((GetAsyncKeyState(vk) & 0x8000) != 0) continue;
		gPhysicalDown[vk].store(false, std::memory_order_release);
	}
}

// Lock screen, UAC and switch user move the input to another desktop and take
// every hold with them without a release. Drop all local state, releasing any
// injected key still logically down, then let the observer cancel its wait.
void resetPhysicalState() {
	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	// Routing may already have been removed by unregisterAll. Physical input
	// ownership still has to be closed, including forwarded Win-downs.
	cancelWin(snapshot.get());

	gWin = {};
	gWinDown.store(false);
	gHeld.fill({});
	for (auto& masked: gMaskHeld) masked = false;
	gTapCandidate = 0;
	gHoldActive = 0;
	gSuppressLoneSuperTap.store(false);
	gSuperChordConsumed = false;
	gEscapeClaimed = false;
	gTaskbarFilter.reset();

	for (size_t vk = 0; vk < std::size(gReplayed); vk++) {
		if (!gReplayed[vk]) continue;
		auto input = chordInput(static_cast<uint8_t>(vk), true);
		SendInput(1, &input, sizeof(INPUT));
		gReplayed[vk] = false;
	}

	for (auto& down: gPhysicalDown) down.store(false, std::memory_order_release);
	for (auto& swallowed: gPhysicalSwallowed) swallowed = false;
}

// Runs on the hook thread, where a key transition cannot interleave between
// the snapshot and the subscription.
void armObserver(InputObserverRequest& request) {
	reconcilePhysicalState();
	request.keys = physicalKeys();
	gInputObserverMessage.store(request.keyMessage, std::memory_order_relaxed);
	gInputObserverResetMessage.store(request.resetMessage, std::memory_order_relaxed);
	gInputObserver.store(request.target, std::memory_order_release);
	if (request.done != nullptr) SetEvent(request.done);
}

void CALLBACK inputDesktopChanged(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND /*hwnd*/,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (event != EVENT_SYSTEM_DESKTOPSWITCH) return;

	resetPhysicalState();
	if (auto snapshot = gSnapshot.load(std::memory_order_acquire)) {
		post(HookReset, 0, snapshot->serial);
	}

	if (auto* observer = gInputObserver.load(std::memory_order_acquire); observer != nullptr) {
		PostMessageW(
		    observer,
		    gInputObserverResetMessage.load(std::memory_order_relaxed),
		    0,
		    0
		);
	}
}

// Replay the deferred downs AND this event in one batch. Passing the physical
// key after SendInput would let it overtake the queued Win-down.
LRESULT forwardWin(const KBDLLHOOKSTRUCT* info, bool mask) {
	std::array<INPUT, 5> inputs {};
	UINT count = 0;
	for (size_t i = 0; i < gWin.size(); i++) {
		// Only a down this hook swallowed is deferred and still needs replaying.
		// A down that reached Windows directly must not be sent twice.
		if (gWin[i].down && gWin[i].consumed && !gWin[i].forwarded) {
			inputs[count++] = chordInput(i == 0 ? VK_LWIN : VK_RWIN, false);
		}
	}
	if (count == 0) return 0;
	inputs[count] = replayInput(info);
	UINT total = count + 1;
	if (mask) {
		inputs[total++] = keyInput(KeyboardHook::MASK_KEY, false);
		inputs[total++] = keyInput(KeyboardHook::MASK_KEY, true);
	}
	auto sent = SendInput(total, inputs.data(), sizeof(INPUT));
	// Even a partial insertion may have sent a modifier down. Its release
	// must be replayed (or passed if that fails); cancellation releases it too.
	for (UINT i = 0; i < sent && i < count; i++) {
		gWin[inputs[i].ki.wVk == VK_LWIN ? 0 : 1].forwarded = true;
	}
	// The key itself may have been inserted even if a trailing mask was not.
	if (sent <= count) return 0;
	gReplayed[info->vkCode] = true;
	return 1;
}

LRESULT onWin(
    const HookSnapshot& snapshot,
    const KBDLLHOOKSTRUCT* info,
    bool up,
    bool taskbarMaskedRelease = false
) {
	auto index = info->vkCode == VK_LWIN ? 0 : 1;
	auto& key = gWin[index];
	auto& other = gWin[1 - index];
	if (!up) {
		// As with ordinary held binds, a new press after a typematic-sized
		// gap recovers from a release hidden by another hook/input desktop.
		if (key.down && info->time - key.lastTime > STALE_REPEAT_MS) cancelWin(&snapshot);
		if (key.down) {
			key.lastTime = info->time;
			// A repeat of a down this hook owns stays owned; one Windows saw
			// passes through like the down did.
			return key.consumed ? 1 : 0;
		}
		// Preserve the existing UIPI boundary. A sequence whose down passed
		// through is never claimed retrospectively on its release.
		if (gForegroundBlocked.load() || isDown(static_cast<int>(info->vkCode))) return 0;
		auto ownsSuper = !snapshot.masks.empty();
		for (const auto& trigger: snapshot.triggers) {
			ownsSuper |= trigger.combo.lone == ModSuper || (trigger.combo.mods & ModSuper) != 0;
		}
		// Physical Super state is tracked even when routing owns no Super
		// chord: the mouse hook asks superDown() and a later routing start
		// must see the key that is really held. Only binding work is gated.
		if (ownsSuper) {
			if (!other.down) {
				gSuppressLoneSuperTap.store(false);
				gSuperChordConsumed = false;
			}
			if (gTapCandidate != 0) gTapCandidate = 0;
			else if (heldModifiers() == 0 && hasTrigger(snapshot, HookTrigger::Tap, ModSuper)) {
				gTapCandidate = ModSuper;
				gTapStart = info->time;
			}
		}
		key = {.down = true, .forwarded = false, .consumed = ownsSuper, .lastTime = info->time};
		gWinDown.store(true);
		if (ownsSuper) {
			if (!other.down) {
				gHoldActive |= ModSuper;
				postAll(snapshot, HookTrigger::Hold, ModSuper, HookPressed);
			}
			return 1;
		}
		return 0;
	}
	if (!key.down) return taskbarMaskedRelease && injectMaskedRelease(info) ? 1 : 0;
	auto forwarded = key.forwarded;
	auto consumed = key.consumed;
	auto chordConsumed = gSuperChordConsumed;
	key = {};
	gWinDown.store(other.down);
	if (!other.down) {
		if ((gHoldActive & ModSuper) != 0) {
			gHoldActive &= ~ModSuper;
			postAll(snapshot, HookTrigger::Hold, ModSuper, HookReleased);
		}
		if (gTapCandidate == ModSuper && !gSuppressLoneSuperTap.load() && !gForegroundBlocked.load()
		    && info->time - gTapStart <= snapshot.tapTimeoutMs)
		{
			postAll(snapshot, HookTrigger::Tap, ModSuper, HookTapped);
		}
		gTapCandidate = 0;
		gSuppressLoneSuperTap.store(false);
		gSuperChordConsumed = false;
	}
	// A local tap, hold or shell chord never exposed a Win-down to Explorer.
	// No mask or replayed release exists for another hook to corrupt.
	// The taskbar policy also masks a down that passed directly through,
	// including over an elevated foreground. On failure pass the physical up.
	if (taskbarMaskedRelease && (forwarded || !consumed)) {
		return injectMaskedRelease(info) ? 1 : 0;
	}
	if (forwarded && chordConsumed && !other.down && !gForegroundBlocked.load()) {
		return injectMaskedRelease(info) ? 1 : 0;
	}
	if (forwarded) {
		// Queue after any pending chord replay, even if a fast physical up
		// was already in the hook queue ahead of that replay.
		auto input = replayInput(info);
		return SendInput(1, &input, sizeof(INPUT)) == 1 ? 1 : 0;
	}
	// A down that passed through must let its release through too, or Windows
	// is left with a modifier it believes is held.
	return consumed ? 1 : 0;
}

DWORD integrityLevel(HANDLE token) {
	DWORD size = 0;
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
	if (size == 0) return 0;

	auto buffer = std::make_unique<uint8_t[]>(size);
	if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.get(), size, &size)) return 0;

	auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.get());
	auto count = *GetSidSubAuthorityCount(label->Label.Sid);
	return count == 0 ? 0 : *GetSidSubAuthority(label->Label.Sid, count - 1);
}

DWORD ownIntegrityLevel() {
	static const DWORD level = []() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return DWORD(0);
		auto level = integrityLevel(token);
		CloseHandle(token);
		return level;
	}();

	return level;
}

bool isHiddenFromUs(HWND hwnd) {
	if (hwnd == nullptr) return false;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0 || pid == GetCurrentProcessId()) return false;

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return true;

	auto hidden = true;
	HANDLE token = nullptr;
	if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
		auto level = integrityLevel(token);
		hidden = level == 0 || level > ownIntegrityLevel();
		CloseHandle(token);
	}

	CloseHandle(process);
	return hidden;
}

void CALLBACK foregroundChanged(
    HWINEVENTHOOK /*hook*/,
    DWORD /*event*/,
    HWND hwnd,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (idObject != OBJID_WINDOW) return;
	gForegroundBlocked.store(isHiddenFromUs(hwnd));
}

LRESULT onModifier(const HookSnapshot& snapshot, const KBDLLHOOKSTRUCT* info, uint8_t bit, bool up) {
	auto vk = static_cast<int>(info->vkCode);
	auto blocked = gForegroundBlocked.load();
	auto otherDown = isDown(otherSide(info->vkCode));

	if (!up) {
		if (isDown(vk)) return 0;

		if (gTapCandidate != 0) {
			gTapCandidate = 0;
		} else if (!blocked && heldModifiers() == 0 && hasTrigger(snapshot, HookTrigger::Tap, bit)) {
			gTapCandidate = bit;
			gTapStart = info->time;
		}

		if (!otherDown) {
			gHoldActive |= bit;
			postAll(snapshot, HookTrigger::Hold, bit, HookPressed);
		}

		return 0;
	}

	if ((gHoldActive & bit) != 0 && !otherDown) {
		gHoldActive &= ~bit;
		postAll(snapshot, HookTrigger::Hold, bit, HookReleased);
	}

	auto tap = gTapCandidate == bit;
	auto plan = planModifierRelease(bit, false, otherDown, tap, blocked, false);

	if (tap) gTapCandidate = 0;
	if (!plan.maskAndReplay && !plan.postTap) return 0;

	auto replayed = plan.maskAndReplay && injectMaskedRelease(info);

	if (plan.postTap && info->time - gTapStart <= snapshot.tapTimeoutMs) {
		postAll(snapshot, HookTrigger::Tap, bit, HookTapped);
	}

	// A masked release is swallowed only when its replay made it in, so the
	// modifier cannot be left logically down; a Ctrl or Shift tap has no mask
	// and keeps passing the physical release through.
	return swallowModifierRelease(plan, replayed) ? 1 : 0;
}

LRESULT onKey(
    const HookSnapshot& snapshot,
    const KBDLLHOOKSTRUCT* info,
    bool up,
    TaskbarShortcutDecision taskbar = {}
) {
	auto vk = static_cast<uint8_t>(info->vkCode);
	auto& held = gHeld.at(vk);

	// A fast repeat/up can already be waiting ahead of the injected down.
	// Queue the rest of this key sequence too, or an up could precede its down.
	if (gReplayed[vk]) {
		auto input = replayInput(info);
		if (up) gReplayed[vk] = false;
		return SendInput(1, &input, sizeof(INPUT)) == 1 ? 1 : 0;
	}

	if (up) {
		gMaskHeld[vk] = false;
		if (!held.active) return taskbar.swallow ? 1 : 0;
		held.active = false;
		post(HookReleased, held.id, held.serial);
		return 1;
	}

	gTapCandidate = 0;

	if (held.active) {
		if (info->time - held.lastTime <= STALE_REPEAT_MS) {
			held.lastTime = info->time;
			post(HookRepeated, held.id, held.serial);
			return 1;
		}

		held.active = false;
	}

	if (gForegroundBlocked.load()) return taskbar.swallow ? 1 : 0;

	auto mods = heldModifiers();

	if (vk == VK_DELETE && (mods & ModCtrl) != 0 && (mods & ModAlt) != 0) return 0;

	// A registered Super chord is consumed by RegisterHotKey, so the hook
	// cannot swallow it. Tap the masking key while the Windows key is still
	// down instead; Windows then treats the chord as used and does not open the
	// Start menu when the Windows key is released.
	bool registered = false;
	for (const auto& combo: snapshot.masks) {
		if (combo.vk != vk || combo.mods != mods) continue;
		registered = true;
		if (!gMaskHeld[vk] && (gWin[0].forwarded || gWin[1].forwarded)) {
			injectMask();
			gMaskHeld[vk] = true;
		}
		break;
	}

	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind != HookTrigger::Combo) continue;
		if (trigger.combo.vk != vk || trigger.combo.mods != mods) continue;

		held = HeldKey {.id = trigger.id, .serial = snapshot.serial, .lastTime = info->time, .active = true};
		post(HookPressed, trigger.id, snapshot.serial);

		if ((mods & ModAlt) != 0 || gWin[0].forwarded || gWin[1].forwarded) injectMask();
		if ((mods & ModSuper) != 0) gSuperChordConsumed = true;
		return 1;
	}

	// Both hook-backed and RegisterHotKey binds win over the taskbar policy.
	// Do not forward a deferred Win-down for a filtered chord.
	if (!registered && taskbar.swallow) {
		gSuperChordConsumed = true;
		return 1;
	}
	return forwardWin(info, registered);
}

LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);

	auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam); // NOLINT(performance-no-int-to-ptr)
	qs::windows::noteHookDelay(true, info->time);

	if ((info->flags & LLKHF_INJECTED) != 0 && info->dwExtraInfo == KeyboardHook::INJECTION_MARKER) {
		if (taskbarShortcutFilterEnabled()) {
			gTaskbarFilter
			    .handle(static_cast<int>(info->vkCode), (info->flags & LLKHF_UP) != 0, true, 0, false);
		}
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	auto up = (info->flags & LLKHF_UP) != 0;
	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	auto& routing = snapshot != nullptr ? *snapshot : gNoRouting;
	auto swallow = 0;
	TaskbarShortcutDecision taskbar;
	if (taskbarShortcutFilterEnabled()) {
		auto mods = heldModifiers();
		bool bound = false;
		if (!gForegroundBlocked.load()) {
			for (const auto& trigger: routing.triggers) {
				bound |= trigger.kind == HookTrigger::Combo && trigger.combo.vk == info->vkCode
				      && trigger.combo.mods == mods;
			}
			for (const auto& combo: routing.masks) {
				bound |= combo.vk == info->vkCode && combo.mods == mods;
			}
		}
		taskbar = gTaskbarFilter.handle(
		    static_cast<int>(info->vkCode),
		    up,
		    false,
		    mods,
		    gWin[info->vkCode == VK_LWIN ? 1 : 0].down
		        || isDown(info->vkCode == VK_LWIN ? VK_RWIN : VK_LWIN),
		    bound
		);
	}

	// Routing stopped. Drop the keys its binds owned: their releases pass
	// through instead of being swallowed for a map that no longer exists, and
	// a restarted map can never match a key that is already held.
	if (gRoutingStopped.exchange(false, std::memory_order_acq_rel)) {
		gHeld.fill({});
		for (auto& masked: gMaskHeld) masked = false;
		gTapCandidate = 0;
		gHoldActive = 0;
	}

	// Esc that cancels a Super drag is consumed with its release. The filter
	// claims it only while such a drag is active, so Esc passes otherwise.
	if (info->vkCode == VK_ESCAPE && !up && !gEscapeClaimed) {
		auto* filter = gEscapeFilter.load(std::memory_order_acquire);
		gEscapeClaimed = filter != nullptr && filter();
		if (gEscapeClaimed) swallow = 1;
	} else if (info->vkCode == VK_ESCAPE && gEscapeClaimed) {
		// The claimed press's repeats and release.
		if (up) gEscapeClaimed = false;
		swallow = 1;
	}

	if (swallow == 0 && info->vkCode <= 0xFF) {
		// Physical and modifier state is tracked whenever the hook is
		// installed, routing or not: Super down/up feeds superDown(), and the
		// releases of keys the hook swallowed must still be handled. Only bind
		// matching and swallowing use the routing snapshot.
		static uint32_t routingSerial = 0;
		if (snapshot == nullptr) {
			routingSerial = 0;
		} else if (routingSerial != snapshot->serial) {
			routingSerial = snapshot->serial;
			gTapCandidate = gHoldActive = 0;
		}
		auto bit = modifierBit(info->vkCode);

		expireWin(info->time);
		swallow = bit == ModSuper ? onWin(routing, info, up, taskbar.maskedRelease)
		        : bit != 0        ? onModifier(routing, info, bit, up)
		                          : onKey(routing, info, up, taskbar);
	}
	updateWinLeaseTimer();

	// Record the physical transition after the decision, so a swallowed down
	// is known to have no GetAsyncKeyState counterpart, then notify the
	// observer. Both happen on the hook thread, in event order.
	if (info->vkCode <= 0xFF) {
		auto vk = static_cast<uint8_t>(info->vkCode);
		gPhysicalDown[vk].store(!up, std::memory_order_release);
		gPhysicalSwallowed[vk] = !up && swallow != 0;

		if (auto* observer = gInputObserver.load(std::memory_order_acquire); observer != nullptr) {
			PostMessageW(
			    observer,
			    gInputObserverMessage.load(std::memory_order_relaxed),
			    info->vkCode,
			    up ? 1 : 0
			);
		}
	}

	if (swallow != 0) return 1;

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void signalReady() {
	if (auto ready = gReadyEvent.load(); ready != nullptr) SetEvent(ready);
}

void hookThreadMain() {
	MSG msg {};
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	gThreadId.store(GetCurrentThreadId());

	if (gStopRequested.load()) {
		gThreadId.store(0);
		signalReady();
		if (auto exited = gExitEvent.load(); exited != nullptr) SetEvent(exited);
		return;
	}

	prioritizeInputThread(THREAD_PRIORITY_TIME_CRITICAL);

	auto* hook = SetWindowsHookExW(WH_KEYBOARD_LL, &hookProc, GetModuleHandleW(nullptr), 0);

	if (hook == nullptr) {
		auto error = GetLastError();
		gFailed.store(true);
		qCWarning(logKeyboardHook) << "Failed to install the keyboard hook (error" << error
		                           << "), binds with the Windows key are unavailable.";
		gThreadId.store(0);
		signalReady();
		if (auto exited = gExitEvent.load(); exited != nullptr) SetEvent(exited);
		return;
	}

	auto* foregroundHook = SetWinEventHook(
	    EVENT_SYSTEM_FOREGROUND,
	    EVENT_SYSTEM_FOREGROUND,
	    nullptr,
	    &foregroundChanged,
	    0,
	    0,
	    WINEVENT_OUTOFCONTEXT
	);

	// A secure desktop (lock screen, UAC, switch user) takes every hold away
	// without a release. Reset on the switch instead of retaining stale keys.
	auto* desktopHook = SetWinEventHook(
	    EVENT_SYSTEM_DESKTOPSWITCH,
	    EVENT_SYSTEM_DESKTOPSWITCH,
	    nullptr,
	    &inputDesktopChanged,
	    0,
	    0,
	    WINEVENT_OUTOFCONTEXT
	);

	gForegroundBlocked.store(isHiddenFromUs(GetForegroundWindow()));
	seedPhysicalState();
	signalReady();

	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (msg.message == WM_QS_ARM_OBSERVER) {
			armObserver(*reinterpret_cast<InputObserverRequest*>(msg.wParam)); // NOLINT
			continue;
		}
		if (msg.message == WM_TIMER && gWinLeaseTimer != 0 && msg.wParam == gWinLeaseTimer) {
			expireWin(GetTickCount());
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (desktopHook != nullptr) UnhookWinEvent(desktopHook);
	if (foregroundHook != nullptr) UnhookWinEvent(foregroundHook);
	resetPhysicalState();
	UnhookWindowsHookEx(hook);

	gInputObserver.store(nullptr);

	// Last write to the thread id: a request that observes this event knows the
	// thread will not touch physical state or its request any more.
	gThreadId.store(0);
	if (auto exited = gExitEvent.load(); exited != nullptr) SetEvent(exited);
}

// Installs the thread unless it is already up. Routing target and message base
// stay whatever the last start()/retain() set, so a hook kept alive by another
// owner keeps answering the hotkey manager until it stops routing.
bool startHookThread() {
	if (gThread != nullptr && !gFailed.load()) {
		auto exited = gExitEvent.load();
		if (exited == nullptr || WaitForSingleObject(exited, 0) != WAIT_OBJECT_0) return true;
	}

	if (gThread != nullptr) {
		gThread->join();
		delete gThread;
		gThread = nullptr;
		if (auto exited = gExitEvent.exchange(nullptr); exited != nullptr) CloseHandle(exited);
	}

	auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	auto exited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ready == nullptr || exited == nullptr) {
		if (ready != nullptr) CloseHandle(ready);
		if (exited != nullptr) CloseHandle(exited);
		return false;
	}

	gReadyEvent.store(ready);
	gExitEvent.store(exited);
	gThreadId.store(0);
	gStopRequested.store(false);
	gFailed.store(false);
	gThread = new std::thread(&hookThreadMain);

	auto answered = WaitForSingleObject(ready, HOOK_START_TIMEOUT_MS) == WAIT_OBJECT_0;
	auto started = answered && !gFailed.load();
	gReadyEvent.store(nullptr);
	CloseHandle(ready);

	if (!started) {
		gStopRequested.store(true);
		auto threadId = gThreadId.load();
		if (threadId != 0) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
		gThread->join();
		delete gThread;
		gThread = nullptr;
		gExitEvent.store(nullptr);
		CloseHandle(exited);
	}

	return started;
}

void stopHookThread() {
	if (gThread == nullptr) return;

	gStopRequested.store(true);
	auto threadId = gThreadId.load();
	if (threadId != 0) PostThreadMessageW(threadId, WM_QUIT, 0, 0);

	gThread->join();
	delete gThread;
	gThread = nullptr;
	gTarget.store(nullptr);
	if (auto exited = gExitEvent.exchange(nullptr); exited != nullptr) CloseHandle(exited);
}

} // namespace

uint8_t modifierBit(DWORD vk) {
	switch (vk) {
	case VK_LWIN:
	case VK_RWIN: return ModSuper;
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL: return ModCtrl;
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU: return ModAlt;
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT: return ModShift;
	default: return 0;
	}
}

bool KeyboardHook::start(HWND target, UINT messageBase) {
	gTarget.store(target);
	gMessageBase.store(messageBase);
	gOwnership.setRouting(true);

	auto started = startHookThread();
	if (!started) gOwnership.setRouting(false);
	return started;
}

void KeyboardHook::stop() {
	// Stop routing, but keep the hook while another owner still needs it. An
	// active Super drag holds it so Esc can cancel without configured binds.
	gOwnership.setRouting(false);

	// Routing is over. A holder may keep the hook installed for its own filter;
	// physical state tracking continues, but no stale bind may match a key.
	setSnapshot(nullptr);
	gRoutingStopped.store(true, std::memory_order_release);

	// Opted-in taskbar filtering owns the hook for the process lifetime.
	if (gOwnership.needed() || taskbarShortcutFilterEnabled()) return;

	stopHookThread();
}

bool KeyboardHook::retain() {
	gOwnership.hold();

	// A feature's reference is independent of the hotkey manager's routing, so
	// it is enough on its own for the hook to be installed.
	return startHookThread();
}

void KeyboardHook::release() {
	gOwnership.drop();
	if (gOwnership.needed() || taskbarShortcutFilterEnabled()) return;

	stopHookThread();
}

bool KeyboardHook::isRunning() { return gThread != nullptr && !gFailed.load(); }
bool KeyboardHook::foregroundBlocked() { return gForegroundBlocked.load(); }
bool KeyboardHook::superDown() {
	return gWinDown.load() || isDown(VK_LWIN) || isDown(VK_RWIN);
}

void KeyboardHook::setSnapshot(std::shared_ptr<const HookSnapshot> snapshot) {
	gSnapshot.store(std::move(snapshot), std::memory_order_release);
}

void KeyboardHook::noteSuperChord() { gSuppressLoneSuperTap.store(true); }
void KeyboardHook::setEscapeFilter(bool (*filter)()) { gEscapeFilter.store(filter, std::memory_order_release); }

bool KeyboardHook::armInputObserver(
    HWND target,
    UINT message,
    UINT resetMessage,
    std::vector<uint8_t>& keys
) {
	auto threadId = gThreadId.load();
	auto exited = gExitEvent.load();
	if (gThread == nullptr || threadId == 0 || exited == nullptr) return false;

	auto done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (done == nullptr) return false;

	InputObserverRequest request {
	    .target = target,
	    .keyMessage = message,
	    .resetMessage = resetMessage,
	    .done = done,
	    .keys = {},
	};

	if (!PostThreadMessageW(
	        threadId,
	        WM_QS_ARM_OBSERVER,
	        reinterpret_cast<WPARAM>(&request), // NOLINT(performance-no-int-to-ptr)
	        0
	    ))
	{
		CloseHandle(done);
		return false;
	}

	// The hook thread either answers or stops; it never leaves the request
	// queued while exiting, so the stack request stays valid until this returns.
	HANDLE handles[] = {done, exited};
	auto wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	CloseHandle(done);

	if (wait != WAIT_OBJECT_0) return false;

	keys = std::move(request.keys);
	return true;
}

void KeyboardHook::clearInputObserver() { gInputObserver.store(nullptr, std::memory_order_release); }

bool KeyboardHook::sendKey(uint8_t vk) {
	auto scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
	auto extended = isExtendedKey(vk);

	std::array<INPUT, 2> inputs {
	    keyInput(vk, false, scan, extended),
	    keyInput(vk, true, scan, extended),
	};

	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

bool KeyboardHook::sendChord(uint8_t mods, uint8_t vk, int times) {
	struct Group {
		uint8_t bit;
		uint8_t left;
		uint8_t right;
	};

	static constexpr std::array<Group, 4> groups {{
	    {.bit = ModCtrl, .left = VK_LCONTROL, .right = VK_RCONTROL},
	    {.bit = ModAlt, .left = VK_LMENU, .right = VK_RMENU},
	    {.bit = ModShift, .left = VK_LSHIFT, .right = VK_RSHIFT},
	    {.bit = ModSuper, .left = VK_LWIN, .right = VK_RWIN},
	}};

	std::vector<INPUT> lift;
	std::vector<INPUT> press;
	std::vector<INPUT> restore;
	auto masked = false;

	for (const auto& group: groups) {
		if ((mods & group.bit) != 0) {
			if (!isDown(group.left) && !isDown(group.right)) {
				press.push_back(chordInput(group.left, false));
			}

			continue;
		}

		for (auto key: {group.left, group.right}) {
			if (!isDown(key)) continue;
			lift.push_back(chordInput(key, true));
			restore.push_back(chordInput(key, false));
			masked = masked || group.bit == ModSuper || group.bit == ModAlt;
		}
	}

	std::vector<INPUT> inputs;
	auto mask = [&inputs]() {
		inputs.push_back(keyInput(KeyboardHook::MASK_KEY, false));
		inputs.push_back(keyInput(KeyboardHook::MASK_KEY, true));
	};

	if (masked) mask();
	inputs.insert(inputs.end(), lift.begin(), lift.end());
	inputs.insert(inputs.end(), press.begin(), press.end());

	for (auto i = 0; i < times; i++) {
		inputs.push_back(chordInput(vk, false));
		inputs.push_back(chordInput(vk, true));
	}

	for (auto input = press.rbegin(); input != press.rend(); ++input) {
		input->ki.dwFlags |= KEYEVENTF_KEYUP;
		inputs.push_back(*input);
	}

	inputs.insert(inputs.end(), restore.begin(), restore.end());
	if (masked) mask();

	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

} // namespace qs::windows::hotkeys
