#pragma once

#include <qt_windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

namespace qs::windows {

// A restore or placement the drag worker is waiting for.
//
// Completion is decided only by the state reported by the target or by the
// answer to the placement. The deadline exists so a target that never answers
// (a hung window) can be reported as a failure; reaching the deadline never
// counts as completion.
enum class DragWaitKind : uint8_t {
	None,
	Restore,
	Placement,
};

enum class DragWaitResult : uint8_t {
	Waiting,
	Done,
	Failed,
};

struct DragWait {
	DragWaitKind kind = DragWaitKind::None;
	DWORD started = 0;
	DWORD timeoutMs = 0;

	void begin(DragWaitKind kind, DWORD now, DWORD timeoutMs) {
		this->kind = kind;
		this->started = now;
		this->timeoutMs = timeoutMs;
	}

	void clear() { *this = DragWait(); }

	[[nodiscard]] bool active() const { return this->kind != DragWaitKind::None; }
};

inline bool dragWaitExpired(const DragWait& wait, DWORD now) {
	return now - wait.started >= wait.timeoutMs;
}

// The one placement in flight. Placement is synchronous on its own thread, so
// the answer is the rect the window has once SetWindowPos returned: the
// requested geometry, a constrained one (a minimum size, a DPI snap) or the
// unchanged geometry of a refusal. Any of them answers the request. Targets
// that arrive while a request is outstanding coalesce into `dirty`; only the
// latest cursor position is placed once the answer arrives.
struct DragPlacement {
	uint32_t seq = 0;
	bool pending = false;
	bool dirty = false;

	// Whether request `seq` may be sent now. Otherwise it is coalesced.
	bool submit(uint32_t seq) {
		if (this->pending) {
			this->dirty = true;
			return false;
		}

		this->seq = seq;
		this->pending = true;
		this->dirty = false;
		return true;
	}

	// Whether an answer belongs to the outstanding request. Answers to an
	// earlier drag's request are ignored.
	bool answer(uint32_t seq) {
		if (!this->pending || seq != this->seq) return false;
		this->pending = false;
		return true;
	}
};

// The size range a resize may ask for. A target that clamps a request teaches
// its bound, so the next request already respects it.
constexpr LONG DRAG_CLAMP_SLACK = 32;

struct DragLimits {
	LONG minWidth = 1;
	LONG minHeight = 1;
	LONG maxWidth = LONG_MAX;
	LONG maxHeight = LONG_MAX;
};

inline void dragLearnLimits(DragLimits& limits, const RECT& requested, const RECT& actual) {
	auto learn = [](LONG wanted, LONG got, LONG& minimum, LONG& maximum) {
		if (got - wanted > DRAG_CLAMP_SLACK) minimum = std::max(minimum, got);
		else if (wanted - got > DRAG_CLAMP_SLACK) maximum = std::min(maximum, got);
	};

	learn(requested.right - requested.left, actual.right - actual.left, limits.minWidth, limits.maxWidth);
	learn(requested.bottom - requested.top, actual.bottom - actual.top, limits.minHeight, limits.maxHeight);
}

// The resize target for a cursor displacement. It is always computed from the
// geometry and cursor position the drag began with, so what the target
// accepted along the way only enters through the learned limits and a later
// reversal measures the same displacement it always did. `left`/`top` mean the
// grabbed edge is the left/top one, so the opposite edge stays where it began.
inline RECT dragResizedRect(
    const RECT& origin,
    bool left,
    bool top,
    LONG dx,
    LONG dy,
    const DragLimits& limits
) {
	auto rect = origin;

	auto width = std::clamp(
	    origin.right - origin.left + (left ? -dx : dx),
	    limits.minWidth,
	    std::max(limits.minWidth, limits.maxWidth)
	);
	auto height = std::clamp(
	    origin.bottom - origin.top + (top ? -dy : dy),
	    limits.minHeight,
	    std::max(limits.minHeight, limits.maxHeight)
	);

	if (left) rect.left = rect.right - width;
	else rect.right = rect.left + width;

	if (top) rect.top = rect.bottom - height;
	else rect.bottom = rect.top + height;

	return rect;
}

// A restore is complete only once the window itself reports that it is no
// longer maximized.
inline DragWaitResult dragRestoreWait(const DragWait& wait, bool zoomed, DWORD now) {
	if (!zoomed) return DragWaitResult::Done;
	return dragWaitExpired(wait, now) ? DragWaitResult::Failed : DragWaitResult::Waiting;
}

// The end of a drag is complete once its last placement has been answered.
inline DragWaitResult dragPlacementWait(const DragWait& wait, bool pending, DWORD now) {
	if (!pending) return DragWaitResult::Done;
	return dragWaitExpired(wait, now) ? DragWaitResult::Failed : DragWaitResult::Waiting;
}

// One step of the Esc restore of a maximized drag, in the order the worker
// runs it. The write is the last one: it waits until Windows has left the
// maximized state (a restore already in flight would otherwise arrive
// afterwards) and until any placement that was in flight has answered. The
// target's own state event carries that answer and wakes the worker, so the
// step is taken on the answer; the deadline only reports a hung target.
//
// Once the request is sent, completion is this restore's own answer. The saved
// placement may itself be maximized, so the step never looks at the maximized
// state again; the placement thread compares the answer instead.
enum class DragCancelStep : uint8_t {
	WaitForRestore,
	SendRestore,
	WaitForAnswer,
	Done,
	Failed,
};

inline DragCancelStep dragCancelStep(
    bool restoreSent,
    bool restoreAnswered,
    bool placementPending,
    const DragWait& wait,
    DWORD now
) {
	if (!restoreSent) {
		if (!restoreAnswered || placementPending) {
			return dragWaitExpired(wait, now) ? DragCancelStep::Failed
			                                 : DragCancelStep::WaitForRestore;
		}
		return DragCancelStep::SendRestore;
	}

	switch (dragPlacementWait(wait, placementPending, now)) {
	case DragWaitResult::Waiting: return DragCancelStep::WaitForAnswer;
	case DragWaitResult::Done: return DragCancelStep::Done;
	case DragWaitResult::Failed: return DragCancelStep::Failed;
	}

	return DragCancelStep::WaitForAnswer;
}

// Whether a SetWindowPlacement answer restored the placement captured at drag
// start. MAXIMIZED and a normal frame are both legitimate results, so only the
// reported show command and normal rectangle are compared, never the current
// maximized state.
inline bool dragPlacementRestored(
    const WINDOWPLACEMENT& requested,
    const WINDOWPLACEMENT& actual,
    bool callSucceeded
) {
	if (!callSucceeded || requested.length == 0 || actual.length == 0) return false;
	if (requested.showCmd != actual.showCmd) return false;
	return EqualRect(&requested.rcNormalPosition, &actual.rcNormalPosition) != FALSE;
}

// Where a restored window goes. The grabbed point must stay under the cursor,
// measured against the frame the user actually grabbed while the window was
// maximized, not the frame it has after the restore moved it.
inline RECT dragRestoredRect(const RECT& normal, const RECT& maximizedFrame, POINT grab) {
	auto width = normal.right - normal.left;
	auto height = normal.bottom - normal.top;
	auto frameWidth = std::max(maximizedFrame.right - maximizedFrame.left, LONG(1));
	auto frameHeight = std::max(maximizedFrame.bottom - maximizedFrame.top, LONG(1));

	auto ratio =
	    std::clamp(static_cast<double>(grab.x - maximizedFrame.left) / frameWidth, 0.0, 1.0);
	auto offsetY = std::clamp(grab.y - maximizedFrame.top, LONG(0), frameHeight);
	auto scaledY = std::lround(static_cast<double>(offsetY) / frameHeight * height);
	auto grabY = offsetY < height / 2 ? offsetY : static_cast<LONG>(scaledY);

	auto left = grab.x - static_cast<LONG>(std::lround(ratio * width));
	auto top = grab.y - grabY;
	return RECT {.left = left, .top = top, .right = left + width, .bottom = top + height};
}

} // namespace qs::windows
