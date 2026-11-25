#include "window_tracker.hpp"
#include "hyprland/dispatch_refusals.hpp"

namespace qs::windows {

void TrackedWindow::reportActionRefused(
    const QString& action,
    const QString& reason,
    bool elevated
) {
	++this->mActionErrorGeneration;
	auto text = window_action_detail::errorText(action, reason);
	if (this->mLastActionError != text) {
		this->mLastActionError = text;
		emit this->lastActionErrorChanged();
	}
	emit this->actionRefused(action, reason, elevated);
	this->tracker->reportWindowActionRefused(this, action, reason, elevated);
}

void TrackedWindow::clearActionError() {
	++this->mActionErrorGeneration;
	if (!this->mLastActionError.isEmpty()) {
		this->mLastActionError.clear();
		emit this->lastActionErrorChanged();
	}
	this->tracker->clearWindowActionError(this);
}

void WindowTracker::reportWindowActionRefused(
    const TrackedWindow* window,
    const QString& action,
    const QString& reason,
    bool elevated
) {
	++this->mActionErrorGeneration;
	auto text = window_action_detail::errorText(action, reason);
	this->mLastActionErrorWindow = window;
	if (this->mLastActionError != text) {
		this->mLastActionError = text;
		emit this->lastActionErrorChanged();
	}
	emit this->windowActionRefused(action, reason, elevated);
}

void WindowTracker::reportActionRefusal(const QString& action, const QString& reason, bool elevated) {
	this->reportWindowActionRefused(nullptr, action, reason, elevated);
}

void WindowTracker::clearWindowActionError(const TrackedWindow* window) {
	if (this->mLastActionError.isEmpty() || this->mLastActionErrorWindow != window) return;
	this->mLastActionError.clear();
	this->mLastActionErrorWindow = nullptr;
	emit this->lastActionErrorChanged();
}

void WindowTracker::notePendingUserMove(quint64 sequence, TrackedWindow* window) {
	if (sequence == 0 || window == nullptr || window->hwnd() == nullptr) return;
	++window->mActionErrorGeneration;
	this->mPendingUserMoves.insert(
	    sequence,
	    PendingUserMove {
	        .window = window,
	        .hwnd = window->hwnd(),
	        .windowErrorGeneration = window->mActionErrorGeneration,
	        .trackerErrorGeneration = this->mActionErrorGeneration,
	    }
	);
}

void WindowTracker::completePendingUserMove(
    quint64 sequence,
    quintptr rawHwnd,
    bool succeeded,
    const QString& code,
    const std::function<bool(const TrackedWindow*)>& ownerMatches,
    const std::function<bool(HWND)>& checkElevated
) {
	const auto hwnd = reinterpret_cast<HWND>(rawHwnd);
	const auto pending = this->mPendingUserMoves.constFind(sequence);
	if (sequence == 0 || pending == this->mPendingUserMoves.cend() || pending->hwnd != hwnd) {
		return;
	}
	const auto entry = pending.value();
	this->mPendingUserMoves.erase(pending);

	auto* window = entry.window.data();
	const auto sameTracked = window != nullptr && this->byHwnd.value(hwnd) == window;
	if (succeeded) {
		if (sameTracked && window->mActionErrorGeneration == entry.windowErrorGeneration
		    && ownerMatches && ownerMatches(window))
		{
			window->clearActionError();
		}
		return;
	}

	if (this->mActionErrorGeneration != entry.trackerErrorGeneration
	    || (sameTracked && window->mActionErrorGeneration != entry.windowErrorGeneration))
	{
		return;
	}

	if (sameTracked && ownerMatches && ownerMatches(window)) {
		const auto elevated = checkElevated ? checkElevated(hwnd) : false;
		window->bElevated = elevated;
		const auto reportElevated = elevated && qs::hyprland::ipc::isWindowRejectionMoveCode(code);
		window->reportActionRefused(
		    QStringLiteral("move"),
		    qs::hyprland::ipc::moveRefusalReason(code, reportElevated),
		    reportElevated
		);
		return;
	}

	qs::hyprland::ipc::reportMoveRefusal(this, code, false);
}

} // namespace qs::windows
