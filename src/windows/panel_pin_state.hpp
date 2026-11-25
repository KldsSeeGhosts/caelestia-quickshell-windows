#pragma once

#include <optional>

#include <qt_windows.h>

#include "virtual_desktops.hpp"

namespace qs::windows {

// Per-panel state machine for pinning a top-level shell panel to every virtual
// desktop. Ties every retry and in-flight async pin to a specific HWND,
// generation, and unique request ID so overlapping signals, HWND recreation,
// hide/show races, embed transitions, accessor recovery, and Explorer restarts
// cannot consume another attempt's completion or cut short the wait budget.
struct PanelPinState {
	// Exceeds the pin worker's 5 s stall bound so a waiting panel never gives
	// up while a preceding slow pin is still within its stall window.
	static constexpr qint64 BUSY_WAIT_BUDGET_MS = 6000;
	// Hard ceiling across consecutive slot handoffs.
	static constexpr qint64 MAX_TOTAL_BUSY_WAIT_MS = 30000;
	static constexpr int BUSY_RETRY_MS = 200;

	enum class SubmitAction : quint8 {
		None,
		RetryLater,
		FallbackLegacy,
		TriggerFollow,
	};

	enum class CompletionAction : quint8 {
		None,
		FallbackLegacy,
		TriggerFollow,
		RetryImmediate,
	};

	quint64 generation = 0;
	HWND pinnedHwnd = nullptr;
	HWND failedHwnd = nullptr;
	HWND attemptHwnd = nullptr;
	HWND inFlightHwnd = nullptr;
	quint64 inFlightGeneration = 0;
	quint64 inFlightRequestId = 0;
	std::optional<qint64> busySinceMs;
	std::optional<qint64> firstBusyMs;
	bool pinned = false;

	[[nodiscard]] bool pinnedHere(HWND hwnd) const {
		return this->pinned && hwnd != nullptr && this->pinnedHwnd == hwnd;
	}

	[[nodiscard]] bool inFlightFor(HWND hwnd) const {
		return hwnd != nullptr && this->inFlightRequestId != 0 && this->inFlightHwnd == hwnd
		    && this->inFlightGeneration == this->generation;
	}

	void reset(bool clearPinned) {
		++this->generation;
		this->inFlightHwnd = nullptr;
		this->inFlightGeneration = 0;
		this->inFlightRequestId = 0;
		this->busySinceMs.reset();
		this->firstBusyMs.reset();
		if (clearPinned) {
			this->pinned = false;
			this->pinnedHwnd = nullptr;
			this->failedHwnd = nullptr;
			this->attemptHwnd = nullptr;
		}
	}

	// Prepares for a pin attempt on `hwnd`. Returns true if the caller should
	// ensure its single retry timer is armed (at 0 ms when not already active).
	[[nodiscard]] bool prepare(HWND hwnd) {
		if (hwnd == nullptr) {
			this->reset(true);
			return false;
		}
		if (this->pinnedHere(hwnd)) return false;
		if (this->attemptHwnd != hwnd) {
			this->reset(true);
			this->attemptHwnd = hwnd;
		}
		if (this->inFlightFor(hwnd) || this->failedHwnd == hwnd) return false;
		return true;
	}

	[[nodiscard]] bool canAttempt(HWND hwnd, quint64 expectedGeneration) {
		if (hwnd == nullptr || expectedGeneration != this->generation) return false;
		if (this->pinnedHere(hwnd)) return false;
		if (this->attemptHwnd != hwnd) {
			this->reset(true);
			this->attemptHwnd = hwnd;
			return false;
		}
		return !this->inFlightFor(hwnd) && this->failedHwnd != hwnd;
	}

	[[nodiscard]] SubmitAction onSubmitResult(
	    HWND hwnd,
	    quint64 expectedGeneration,
	    OwnWindowPin result,
	    qint64 nowMs,
	    quint64 requestId = 0
	) {
		if (hwnd == nullptr || expectedGeneration != this->generation || this->attemptHwnd != hwnd) {
			return SubmitAction::None;
		}
		switch (result) {
		case OwnWindowPin::Pinned:
			this->busySinceMs.reset();
			this->firstBusyMs.reset();
			this->applyResult(hwnd, expectedGeneration, true);
			return SubmitAction::None;
		case OwnWindowPin::Pending:
			this->busySinceMs.reset();
			this->firstBusyMs.reset();
			this->inFlightHwnd = hwnd;
			this->inFlightGeneration = expectedGeneration;
			this->inFlightRequestId = requestId;
			return SubmitAction::None;
		case OwnWindowPin::Busy: {
			if (!this->firstBusyMs) this->firstBusyMs = nowMs;
			if (!this->busySinceMs) this->busySinceMs = nowMs;
			const auto sinceHandoff = nowMs - *this->busySinceMs;
			const auto totalElapsed = nowMs - *this->firstBusyMs;
			if (sinceHandoff < BUSY_WAIT_BUDGET_MS && totalElapsed < MAX_TOTAL_BUSY_WAIT_MS) {
				return SubmitAction::RetryLater;
			}
			this->busySinceMs.reset();
			this->firstBusyMs.reset();
			this->applyResult(hwnd, expectedGeneration, false);
			return SubmitAction::TriggerFollow;
		}
		case OwnWindowPin::Refused:
			this->busySinceMs.reset();
			this->firstBusyMs.reset();
			this->applyResult(hwnd, expectedGeneration, false);
			return SubmitAction::TriggerFollow;
		case OwnWindowPin::Unavailable:
			this->busySinceMs.reset();
			this->firstBusyMs.reset();
			return SubmitAction::FallbackLegacy;
		}
		return SubmitAction::None;
	}

	[[nodiscard]] CompletionAction onAsyncCompletion(
	    quint64 completedRequestId,
	    HWND completedHwnd,
	    OwnWindowPin result,
	    HWND currentHwnd,
	    bool visible,
	    bool embedded,
	    bool accessorLoaded,
	    bool retryScheduled
	) {
		if (completedHwnd == nullptr || completedRequestId == 0) return CompletionAction::None;

		const auto matchesInFlight = this->inFlightRequestId != 0
		                          && completedRequestId == this->inFlightRequestId
		                          && completedHwnd == this->inFlightHwnd
		                          && this->inFlightGeneration == this->generation;

		if (!matchesInFlight) {
			// Another pin just freed the worker slot: renew the per-handoff
			// wait budget and kick our busy retry immediately if waiting.
			if (retryScheduled && visible && !embedded && currentHwnd != nullptr
			    && !this->pinnedHere(currentHwnd) && !this->inFlightFor(currentHwnd))
			{
				this->busySinceMs.reset();
				return CompletionAction::RetryImmediate;
			}
			return CompletionAction::None;
		}

		const auto gen = this->generation;
		this->inFlightHwnd = nullptr;
		this->inFlightGeneration = 0;
		this->inFlightRequestId = 0;

		// Revalidate HWND, visibility and embedding before applying the result.
		if (currentHwnd != completedHwnd || !visible || embedded) {
			return CompletionAction::None;
		}

		if (result == OwnWindowPin::Pinned) {
			this->applyResult(completedHwnd, gen, true);
			return CompletionAction::None;
		}
		if (result == OwnWindowPin::Unavailable) {
			if (accessorLoaded) return CompletionAction::FallbackLegacy;
			// Provider disconnected mid-pin: leave failedHwnd unset so
			// onDesktopAccessorReady() retries pinning after recovery.
			return CompletionAction::TriggerFollow;
		}
		this->applyResult(completedHwnd, gen, false);
		return CompletionAction::TriggerFollow;
	}

	bool applyResult(HWND hwnd, quint64 expectedGeneration, bool isPinned) {
		if (hwnd == nullptr || expectedGeneration != this->generation) return false;
		this->pinned = isPinned;
		this->pinnedHwnd = isPinned ? hwnd : nullptr;
		this->failedHwnd = isPinned ? nullptr : hwnd;
		return true;
	}
};

} // namespace qs::windows
