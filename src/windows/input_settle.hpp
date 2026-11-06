#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qs::windows::hotkeys {

// One request to run an action after the keys that triggered it are released.
//
// The state is fed by the existing keyboard and mouse hooks: physical key
// releases are matched against the set that was down when the request armed,
// while any other key or mouse button press cancels it. Key repeat is not new
// input. This class has no Win32 dependency so its transitions can be tested
// without a desktop.
class InputSettle {
public:
	enum class Result : uint8_t {
		Empty,     // Nothing was held when armed; the caller runs at once.
		Waiting,   // Still waiting for the held keys to come up.
		Settled,   // Every held key came up; run the action now.
		Cancelled, // Another key or mouse button was pressed first.
	};

	Result arm(const std::vector<uint8_t>& keys);
	Result release(uint8_t vk);
	Result press(uint8_t vk);
	Result cancel();

	[[nodiscard]] bool pending() const { return this->mPending; }
	[[nodiscard]] std::size_t held() const { return this->mCount; }

private:
	bool mPending = false;
	bool mHeld[256] = {};
	std::size_t mCount = 0;
};

// What a new request should do. Only a keybind dispatch may wait for its
// triggering chord; a direct IPC call never does, and it supersedes a pending
// keybind wait instead of being rejected by it.
enum class ChordRequest : uint8_t {
	Wait,           // This keybind dispatch arms a new wait.
	AlreadyWaiting, // The same keybind asked again while its wait is armed.
	SendNow,        // No chord, or a direct request while a wait is pending.
};

inline ChordRequest classifyChordRequest(bool dispatching, bool gateActive) {
	if (gateActive) return dispatching ? ChordRequest::AlreadyWaiting : ChordRequest::SendNow;
	return dispatching ? ChordRequest::Wait : ChordRequest::SendNow;
}

} // namespace qs::windows::hotkeys
