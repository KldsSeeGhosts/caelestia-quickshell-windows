#pragma once

#include <qt_windows.h>

#include "virtual_desktops.hpp"

namespace qs::windows {

// Per-panel state machine for pinning a top-level shell panel to every virtual
// desktop. Ties every retry and in-flight async pin to a specific HWND and
// generation so overlapping signals, HWND recreation, hide/embed transitions,
// accessor recovery and Explorer restarts cannot spend another window's retry
// budget or apply stale completions.
struct PanelPinState {
	// Exceeds the native worker's 5 s stall bound (30 * 200 ms = 6 s) so a
	// second panel waiting behind one slow pin never gives up prematurely.
	static constexpr int MAX_BUSY_RETRIES = 30;
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
	int busyRetries = 0;
	bool pinned = false;

	[[nodiscard]] bool pinnedHere(HWND hwnd) const {
		return this->pinned && hwnd != nullptr && this->pinnedHwnd == hwnd;
	}

	[[nodiscard]] bool inFlightFor(HWND hwnd) const {
		return hwnd != nullptr && this->inFlightHwnd == hwnd
		    && this->inFlightGeneration == this->generation;
	}

	void reset(bool clearPinned) {
		++this->generation;
		this->inFlightHwnd = nullptr;
		this->inFlightGeneration = 0;
		this->busyRetries = 0;
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

	[[nodiscard]] SubmitAction
	onSubmitResult(HWND hwnd, quint64 expectedGeneration, OwnWindowPin result) {
		if (hwnd == nullptr || expectedGeneration != this->generation || this->attemptHwnd != hwnd) {
			return SubmitAction::None;
		}
		switch (result) {
		case OwnWindowPin::Pinned:
			this->applyResult(hwnd, expectedGeneration, true);
			return SubmitAction::None;
		case OwnWindowPin::Pending:
			this->inFlightHwnd = hwnd;
			this->inFlightGeneration = expectedGeneration;
			return SubmitAction::None;
		case OwnWindowPin::Busy:
			if (this->busyRetries < MAX_BUSY_RETRIES) {
				++this->busyRetries;
				return SubmitAction::RetryLater;
			}
			this->applyResult(hwnd, expectedGeneration, false);
			return SubmitAction::TriggerFollow;
		case OwnWindowPin::Refused:
			this->applyResult(hwnd, expectedGeneration, false);
			return SubmitAction::TriggerFollow;
		case OwnWindowPin::Unavailable:
			return SubmitAction::FallbackLegacy;
		}
		return SubmitAction::None;
	}

	[[nodiscard]] CompletionAction onAsyncCompletion(
	    HWND completedHwnd,
	    OwnWindowPin result,
	    HWND currentHwnd,
	    bool visible,
	    bool embedded,
	    bool accessorLoaded,
	    bool retryScheduled
	) {
		if (completedHwnd == nullptr) return CompletionAction::None;

		// Another panel's pin just freed the worker slot: kick our busy retry
		// immediately instead of waiting for the remaining 200 ms interval.
		if (completedHwnd != this->inFlightHwnd || this->inFlightGeneration != this->generation) {
			if (retryScheduled && visible && !embedded && currentHwnd != nullptr
			    && !this->pinnedHere(currentHwnd) && !this->inFlightFor(currentHwnd))
			{
				return CompletionAction::RetryImmediate;
			}
			return CompletionAction::None;
		}

		const auto gen = this->generation;
		this->inFlightHwnd = nullptr;
		this->inFlightGeneration = 0;

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
