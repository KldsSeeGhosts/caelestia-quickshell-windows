#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <windows.h>

namespace qs::windows::hotkeys {

enum Modifier : uint8_t {
	ModCtrl = 1 << 0,
	ModAlt = 1 << 1,
	ModShift = 1 << 2,
	ModSuper = 1 << 3,
};

struct KeyCombo {
	uint8_t mods = 0;
	uint8_t vk = 0;
	uint8_t lone = 0;

	[[nodiscard]] bool operator==(const KeyCombo& other) const = default;
};

struct HookTrigger {
	enum Kind : uint8_t {
		Combo,
		Tap,
		Hold,
	};

	Kind kind = Combo;
	KeyCombo combo;
	uint32_t id = 0;
};

struct HookSnapshot {
	std::vector<HookTrigger> triggers;
	// Super chords delivered by RegisterHotKey. The hook does not own them; it
	// only taps the masking key while the Windows key is down so Windows does
	// not open the Start menu when the key is released after the chord.
	std::vector<KeyCombo> masks;
	uint32_t serial = 0;
	uint32_t tapTimeoutMs = 1000;
};

/// What one trigger becomes once RegisterHotKey has been offered its chord.
struct TriggerMechanism {
	bool hotkey = false;
	bool hook = false;
	bool mask = false;
};

/// Win+<Key> chords the desktop supervisor's keyboard filter consumes while
/// `windows.hideTaskbar` is on (TaskbarShortcutPolicy.Matches in
/// DesktopTakeover.cs). Keep this in step with that policy.
///
/// The supervisor's hook sits behind the shell's in the chain. A hook-backed
/// chord is therefore handled here first, but a RegisterHotKey chord is
/// forwarded and the supervisor swallows it before Windows can deliver
/// WM_HOTKEY.
[[nodiscard]] constexpr bool supervisorOwnsChord(uint8_t mods, uint8_t vk) {
	if ((mods & ModSuper) == 0) return false;

	const auto ctrl = (mods & ModCtrl) != 0;
	const auto alt = (mods & ModAlt) != 0;
	const auto shift = (mods & ModShift) != 0;

	switch (vk) {
	case 'T': return !ctrl && !alt;
	case 'B': return !shift && !ctrl && !alt;
	case VK_RETURN: return alt && !shift && !ctrl;
	default: break;
	}

	if (vk < '0' || vk > '9') return false;
	if (alt) return !shift && !ctrl;
	return !(shift && ctrl);
}

// Cached once, shared by routing and the hook for the process lifetime.
[[nodiscard]] inline bool taskbarShortcutFilterEnabled() {
	static const bool enabled = [] {
		wchar_t value[2] {};
		return GetEnvironmentVariableW(L"QS_WINDOWS_TASKBAR_SHORTCUT_FILTER", value, 2) == 1
		    && value[0] == L'1';
	}();
	return enabled;
}

struct TaskbarShortcutDecision {
	bool swallow = false;
	bool maskedRelease = false;
};

// Port of DesktopTakeover.cs's TaskbarShortcutFilter. No platform calls:
// the caller executes a masked release and swallows it only on full success.
class TaskbarShortcutFilter {
public:
	TaskbarShortcutDecision handle(
	    int vk,
	    bool up,
	    bool ownInjection,
	    uint8_t mods,
	    bool otherWinDown,
	    bool bindOwnsChord = false
	) {
		if (vk < 0 || vk > 255) return {};
		auto win = vk == VK_LWIN || vk == VK_RWIN;
		auto side = vk == VK_LWIN ? 0 : 1;
		if (ownInjection) {
			if (up && win) this->winDown[side] = false;
			return {};
		}
		if (up) {
			if (win) {
				this->winDown[side] = false;
				if (!this->maskPending || otherWinDown) return {};
				this->maskPending = false;
				return {.maskedRelease = true};
			}
			auto swallowed = this->down[vk];
			this->down[vk] = false;
			return {.swallow = swallowed};
		}
		if (win) {
			if (this->winDown[side]) return {};
			if (this->maskPending && !otherWinDown) this->maskPending = false;
			this->winDown[side] = true;
			return {};
		}
		if (this->down[vk]) return {.swallow = true};
		if (bindOwnsChord) return {};
		if (!supervisorOwnsChord(mods, static_cast<uint8_t>(vk))) return {};
		this->down[vk] = true;
		this->maskPending = true;
		return {.swallow = true};
	}

	void reset() { *this = {}; }

private:
	std::array<bool, 256> down {};
	bool maskPending = false;
	bool winDown[2] {};
};

/// Whether a trigger is offered to RegisterHotKey before the hook.
///
/// Only a WM_HOTKEY delivery gives the process the foreground permission that
/// lets it raise and focus a window it opens; a private hook message does not.
/// Chords without the Windows key always try registration first. A chord with
/// the Windows key stays on the hook, exactly as it was, unless a bind asks
/// for registration (`"register": true`) and the supervisor does not consume
/// the chord. When the backend owns the taskbar filter, registration can win
/// over that filter. Lone-modifier taps and holds only exist on the hook.
[[nodiscard]] constexpr bool shouldRegisterHotkey(
    HookTrigger::Kind kind,
    uint8_t mods,
    uint8_t vk,
    bool preferRegister,
    bool taskbarFilter = false
) {
	if (kind != HookTrigger::Combo) return false;
	if ((mods & ModSuper) == 0) return true;
	return preferRegister && (taskbarFilter || !supervisorOwnsChord(mods, vk));
}

/// Decide how one trigger is delivered once RegisterHotKey has (or has not)
/// been tried. A chord Windows refuses falls back to the hook.
///
/// A registered Super chord still needs a masking key tap from the hook when
/// it is pressed, because the hook does not swallow the chord and Windows can
/// otherwise treat the Windows key as a lone tap on release.
[[nodiscard]] constexpr TriggerMechanism resolveTriggerMechanism(
    HookTrigger::Kind kind,
    uint8_t mods,
    bool registered
) {
	if (kind != HookTrigger::Combo || !registered) return {.hook = true};
	return {.hotkey = true, .mask = (mods & ModSuper) != 0};
}

/// Fallback for a modifier whose down reached Windows. Deferred Win sequences
/// are handled locally by onWin instead. `maskAndReplay` injects the
/// mask key and the replayed release and keeps the physical release from
/// reaching the system; `postTap` delivers a lone-modifier tap. Ctrl and Shift
/// taps post without a mask, so their physical release still passes through.
struct ModifierReleasePlan {
	bool maskAndReplay = false;
	bool postTap = false;
};

/// Decide the action for one modifier key-up.
///
/// A Super chord the hook swallowed has to end like a lone Super tap: swallow
/// the physical release and replace it with a mask tap and a replayed release.
/// Passing the physical Win-up lets Windows finish the key sequence itself, and
/// Explorer then still opens its own Win+V clipboard flyout next to the
/// shell's picker even though the V press never reached Explorer. The mask
/// tap keeps the Start menu closed; the injected release keeps the key from
/// being left down. A release after no swallowed chord passes through, as does
/// one while the other Win key is still held (that release ends the sequence)
/// or when UIPI hides the foreground, which would block the injection.
///
/// `superChord` suppresses only the Win tap it was set for: a Super chord such
/// as a drag owns that Windows key sequence. Ctrl, Shift and Alt taps do not
/// belong to it and must still fire.
[[nodiscard]] constexpr ModifierReleasePlan planModifierRelease(
    uint8_t bit,
    bool chordConsumed,
    bool otherSideDown,
    bool tapCandidate,
    bool blocked,
    bool superChord
) {
	if (bit != ModSuper && bit != ModAlt && bit != ModCtrl && bit != ModShift) return {};
	if (otherSideDown || blocked) return {};

	// An Alt chord's menu bar is already masked when the chord is swallowed;
	// only the Win key has a release path that still runs the shell's own
	// Win+<key> handling.
	if (bit == ModSuper && chordConsumed) return {.maskAndReplay = true};
	if (!tapCandidate) return {};
	if (bit == ModSuper && superChord) return {};

	// A Win or Alt tap replaces the physical release with a mask tap and a
	// replayed one. Ctrl and Shift have no Windows key sequence to finish, so
	// they post the tap and let the physical release through, as before.
	return {.maskAndReplay = bit == ModSuper || bit == ModAlt, .postTap = true};
}

/// Whether the physical release may be swallowed after the replay attempt.
/// A masked release is kept back only when the injection made it in; otherwise
/// the modifier can be left logically down. An unmasked tap always passes.
[[nodiscard]] constexpr bool swallowModifierRelease(
    const ModifierReleasePlan& plan,
    bool replayed
) {
	return plan.maskAndReplay && replayed;
}

enum HookEvent : UINT {
	HookPressed = 0,
	HookRepeated = 1,
	HookReleased = 2,
	HookTapped = 3,
	HookReset = 4,
	HookEventCount = 5,
};

/// Shared ownership of the one WH_KEYBOARD_LL hook.
///
/// The hook has to run for more than the configured keybinds. Esc cancels a
/// Super drag from the hook's key filter (super_drag.cpp), and a user with no
/// keybinds configured still expects that, so a feature can hold the hook
/// independently of the hotkey manager's routing. The hook runs while any
/// owner needs it; starting and stopping stay idempotent at the caller, so an
/// unpaired stop never takes the hook away from another owner.
///
/// `routing` is the hotkey manager's own need: it is set while hook-backed
/// binds (or a temporary wait such as display-off) are registered. `holders`
/// counts the other features. Neither starts a hook by itself.
class HookOwnership {
public:
	void hold() { this->mHolders++; }

	// Never underflows: a spurious release must not hide another owner's hold.
	void drop() {
		if (this->mHolders > 0) this->mHolders--;
	}

	void setRouting(bool routing) { this->mRouting = routing; }

	[[nodiscard]] bool needed() const { return this->mRouting || this->mHolders > 0; }

	[[nodiscard]] int holders() const { return this->mHolders; }

private:
	int mHolders = 0;
	bool mRouting = false;
};

class KeyboardHook {
public:
	static bool start(HWND target, UINT messageBase);
	static void stop();
	[[nodiscard]] static bool isRunning();
	[[nodiscard]] static bool foregroundBlocked();
	// Physical Win state also includes downs deferred by the keyboard hook.
	[[nodiscard]] static bool superDown();

	// Shared ownership independent of the hotkey manager's binds. `retain`
	// starts the hook when nothing else has it running, `release` stops it only
	// when no owner is left. Both are safe to call regardless of start/stop.
	static bool retain();
	static void release();

	// Atomically, on the hook thread, reconciles stale physical state, captures
	// the physical keys that are down (excluding mouse buttons and the generic
	// modifier aliases) and subscribes `target` to every later transition.
	// `keyMessage` carries wParam = virtual key and lParam = 1 for key-up;
	// `resetMessage` is posted when the input desktop changes and every hold is
	// gone. Returns false when the hook did not answer.
	static bool armInputObserver(
	    HWND target,
	    UINT keyMessage,
	    UINT resetMessage,
	    std::vector<uint8_t>& keys
	);

	// Stops posting to the observer installed by armInputObserver.
	static void clearInputObserver();

	static void setSnapshot(std::shared_ptr<const HookSnapshot> snapshot);

	static bool sendKey(uint8_t vk);
	static bool sendChord(uint8_t mods, uint8_t vk, int times = 1);
	static void noteSuperChord();
	// Called on the hook thread for an Esc press; returning true consumes the
	// press and its release. Super drags use it to cancel.
	static void setEscapeFilter(bool (*filter)());

	static constexpr ULONG_PTR INJECTION_MARKER = 0x5153484B;
	static constexpr uint8_t MASK_KEY = 0xE8;
};

[[nodiscard]] uint8_t modifierBit(DWORD vk);

} // namespace qs::windows::hotkeys
