#include "input_settle.hpp"

namespace qs::windows::hotkeys {

InputSettle::Result InputSettle::arm(const std::vector<uint8_t>& keys) {
	this->mPending = false;
	for (auto& held: this->mHeld) held = false;
	this->mCount = 0;

	for (auto vk: keys) {
		if (this->mHeld[vk]) continue;
		this->mHeld[vk] = true;
		++this->mCount;
	}

	// A tap or an external request has nothing left to release.
	if (this->mCount == 0) return Result::Empty;

	this->mPending = true;
	return Result::Waiting;
}

InputSettle::Result InputSettle::release(uint8_t vk) {
	if (!this->mPending) return Result::Cancelled;
	if (!this->mHeld[vk]) return Result::Waiting;

	this->mHeld[vk] = false;
	--this->mCount;

	if (this->mCount != 0) return Result::Waiting;

	this->mPending = false;
	return Result::Settled;
}

InputSettle::Result InputSettle::press(uint8_t vk) {
	if (!this->mPending) return Result::Cancelled;

	// Auto-repeat of a key that was already held is not new input.
	if (this->mHeld[vk]) return Result::Waiting;

	this->mPending = false;
	for (auto& held: this->mHeld) held = false;
	this->mCount = 0;
	return Result::Cancelled;
}

InputSettle::Result InputSettle::cancel() {
	this->mPending = false;
	for (auto& held: this->mHeld) held = false;
	this->mCount = 0;
	return Result::Cancelled;
}

} // namespace qs::windows::hotkeys
