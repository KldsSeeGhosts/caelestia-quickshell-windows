#include "window_tracker.hpp"
#include "hyprland/dispatch_refusals.hpp"

namespace qs::windows {

quint64 TrackedWindow::beginMoveRequest() {
	if (this->tracker != nullptr) ++this->tracker->mMoveRequestGeneration;
	return ++this->mMoveRequestToken;
}

void TrackedWindow::reportActionRefused(
    const QString& action,
    const QString& reason,
    bool elevated
) {
	const auto windowGeneration = ++this->mActionErrorGeneration;
	const auto text = window_action_detail::errorText(action, reason);
	const auto windowChanged = this->mLastActionError != text;
	if (windowChanged) this->mLastActionError = text;

	auto* tracker = this->tracker;
	QPointer<TrackedWindow> self(this);
	QPointer<WindowTracker> guardedTracker(tracker);
	const auto trackerGeneration = ++tracker->mActionErrorGeneration;
	const auto trackerChanged = tracker->mLastActionError != text;
	tracker->mLastActionErrorWindow = this;
	if (trackerChanged) tracker->mLastActionError = text;

	const auto stillCurrent = [&] {
		return guardedTracker && tracker->mActionErrorGeneration == trackerGeneration
		    && tracker->mLastActionError == text;
	};

	if (windowChanged) emit this->lastActionErrorChanged();
	if (self && this->mActionErrorGeneration == windowGeneration) {
		emit this->actionRefused(action, reason, elevated);
	}
	if (trackerChanged && stillCurrent()) {
		emit tracker->lastActionErrorChanged();
	}
	if (stillCurrent()) {
		emit tracker->windowActionRefused(action, reason, elevated);
	}
}

void TrackedWindow::clearActionError() {
	const auto windowChanged = !this->mLastActionError.isEmpty();
	++this->mActionErrorGeneration;
	if (windowChanged) this->mLastActionError.clear();

	auto* tracker = this->tracker;
	QPointer<WindowTracker> guardedTracker(tracker);
	const auto trackerGeneration = tracker->mActionErrorGeneration;
	const auto trackerChanged =
	    !tracker->mLastActionError.isEmpty() && tracker->mLastActionErrorWindow == this;
	if (trackerChanged) {
		tracker->mLastActionError.clear();
		tracker->mLastActionErrorWindow = nullptr;
	}

	if (windowChanged) emit this->lastActionErrorChanged();
	if (guardedTracker && trackerChanged && tracker->mActionErrorGeneration == trackerGeneration
	    && tracker->mLastActionError.isEmpty())
	{
		emit tracker->lastActionErrorChanged();
	}
}

bool TrackedWindow::finishMoveRequest(
    bool accepted,
    quint64 pendingSequence,
    const QString& failureCode,
    quint64 moveToken,
    quint64 windowErrorGenerationBefore,
    quint64 trackerErrorGenerationBefore,
    const std::function<bool(const TrackedWindow*)>& ownerMatches,
    const std::function<bool(HWND)>& checkElevated
) {
	if (!accepted) {
		if (this->mMoveRequestToken == moveToken
		    && this->mActionErrorGeneration == windowErrorGenerationBefore
		    && this->tracker->actionErrorGeneration() == trackerErrorGenerationBefore)
		{
			const auto sameTracked = this->tracker->windowFor(this->mHwnd) == this;
			if (sameTracked && ownerMatches && ownerMatches(this)) {
				const auto elevated = checkElevated ? checkElevated(this->mHwnd) : false;
				this->bElevated = elevated;
				const auto reportElevated =
				    elevated && qs::hyprland::ipc::isWindowRejectionMoveCode(failureCode);
				this->reportActionRefused(
				    QStringLiteral("move"),
				    qs::hyprland::ipc::moveRefusalReason(failureCode, reportElevated),
				    reportElevated
				);
			} else {
				qs::hyprland::ipc::reportMoveRefusal(this->tracker, failureCode, false);
			}
		}
		return false;
	}

	if (this->mMoveRequestToken == moveToken
	    && this->mActionErrorGeneration == windowErrorGenerationBefore)
	{
		if (pendingSequence != 0) {
			this->tracker->notePendingUserMove(
			    pendingSequence, this, moveToken, trackerErrorGenerationBefore
			);
		} else if (this->tracker->windowFor(this->mHwnd) == this && ownerMatches && ownerMatches(this)) {
			this->clearActionError();
		}
	}
	return true;
}

void WindowTracker::reportWindowActionRefused(
    const TrackedWindow* window,
    const QString& action,
    const QString& reason,
    bool elevated
) {
	const auto trackerGeneration = ++this->mActionErrorGeneration;
	const auto text = window_action_detail::errorText(action, reason);
	const auto changed = this->mLastActionError != text;
	this->mLastActionErrorWindow = window;
	if (changed) this->mLastActionError = text;

	QPointer<WindowTracker> self(this);
	if (changed) emit this->lastActionErrorChanged();
	if (self && this->mActionErrorGeneration == trackerGeneration
	    && this->mLastActionError == text)
	{
		emit this->windowActionRefused(action, reason, elevated);
	}
}

void WindowTracker::reportActionRefusal(const QString& action, const QString& reason, bool elevated) {
	this->reportWindowActionRefused(nullptr, action, reason, elevated);
}

void WindowTracker::notePendingUserMove(
    quint64 sequence,
    TrackedWindow* window,
    quint64 moveToken,
    quint64 trackerErrorGeneration
) {
	if (sequence == 0 || window == nullptr || window->hwnd() == nullptr) return;
	++window->mActionErrorGeneration;
	this->mPendingUserMoves.insert(
	    sequence,
	    PendingUserMove {
	        .window = window,
	        .hwnd = window->hwnd(),
	        .moveToken = moveToken,
	        .windowErrorGeneration = window->mActionErrorGeneration,
	        .trackerErrorGeneration = trackerErrorGeneration,
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
		if (sameTracked && window->mMoveRequestToken == entry.moveToken
		    && window->mActionErrorGeneration == entry.windowErrorGeneration
		    && ownerMatches && ownerMatches(window))
		{
			window->clearActionError();
		}
		return;
	}

	if (this->mActionErrorGeneration != entry.trackerErrorGeneration
	    || (window != nullptr
	        && (window->mMoveRequestToken != entry.moveToken
	            || window->mActionErrorGeneration != entry.windowErrorGeneration)))
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
