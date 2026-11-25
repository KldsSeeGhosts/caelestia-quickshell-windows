#include "window_tracker.hpp"
#include "hyprland/dispatch_refusals.hpp"

namespace qs::windows {

quint64 TrackedWindow::beginMoveRequest() {
	this->mLatestMoveSequence = this->tracker != nullptr ? this->tracker->nextActionSequence()
	                                                     : this->mLatestMoveSequence + 1;
	return this->mLatestMoveSequence;
}

void TrackedWindow::notifyActionErrorChanged() {
	if (this->mNotifiedActionError == this->mLastActionError) return;
	this->mNotifiedActionError = this->mLastActionError;
	emit this->lastActionErrorChanged();
}

void TrackedWindow::reportActionRefused(
    const QString& action,
    const QString& reason,
    bool elevated
) {
	const auto sequence = this->tracker != nullptr ? this->tracker->nextActionSequence()
	                                               : this->mLastActionErrorSequence + 1;
	this->reportActionRefusedWithSequence(sequence, action, reason, elevated);
}

void TrackedWindow::reportActionRefusedWithSequence(
    quint64 sequence,
    const QString& action,
    const QString& reason,
    bool elevated
) {
	if (sequence == 0 || this->mLastActionErrorSequence >= sequence) return;
	auto* tracker = this->tracker;
	if (tracker != nullptr && tracker->mLastActionErrorSequence >= sequence) return;

	const auto text = window_action_detail::errorText(action, reason);
	this->mLastActionErrorSequence = sequence;
	this->mLastActionError = text;

	QPointer<TrackedWindow> self(this);
	QPointer<WindowTracker> guardedTracker(tracker);
	if (tracker != nullptr) {
		tracker->mLastActionErrorSequence = sequence;
		tracker->mLastActionErrorWindow = this;
		tracker->mLastActionError = text;
	}

	const auto stillCurrent = [&] {
		return guardedTracker && tracker->mLastActionErrorSequence == sequence
		    && tracker->mLastActionError == text;
	};

	this->notifyActionErrorChanged();
	if (self && this->mLastActionErrorSequence == sequence && this->mLastActionError == text) {
		emit this->actionRefused(action, reason, elevated);
	}
	if (stillCurrent()) {
		tracker->notifyActionErrorChanged();
	}
	if (stillCurrent()) {
		emit tracker->windowActionRefused(action, reason, elevated);
	}
}

void TrackedWindow::clearActionError() {
	const auto sequence = this->tracker != nullptr ? this->tracker->nextActionSequence()
	                                               : this->mLastActionErrorSequence + 1;
	this->clearActionErrorWithSequence(sequence);
}

void TrackedWindow::clearActionErrorWithSequence(quint64 sequence) {
	if (sequence == 0 || this->mLastActionErrorSequence >= sequence) return;
	this->mLastActionErrorSequence = sequence;
	this->mLastActionError.clear();

	auto* tracker = this->tracker;
	QPointer<WindowTracker> guardedTracker(tracker);
	if (tracker != nullptr && !tracker->mLastActionError.isEmpty()
	    && tracker->mLastActionErrorWindow == this)
	{
		tracker->mLastActionError.clear();
		tracker->mLastActionErrorWindow = nullptr;
	}

	this->notifyActionErrorChanged();
	if (guardedTracker) {
		tracker->notifyActionErrorChanged();
	}
}

bool TrackedWindow::finishMoveRequest(
    bool accepted,
    quint64 pendingSequence,
    bool pendingRegistered,
    const QString& failureCode,
    quint64 moveSequence,
    const std::function<bool(const TrackedWindow*)>& ownerMatches,
    const std::function<bool(HWND)>& checkElevated
) {
	if (!accepted) {
		this->tracker->publishMoveFailure(
		    moveSequence, this, this->mHwnd, failureCode, ownerMatches, checkElevated
		);
		return false;
	}

	if (this->mLatestMoveSequence != moveSequence || this->mLastActionErrorSequence >= moveSequence) {
		return this->tracker->lastActionErrorSequence() != moveSequence
		    && this->mLatestMoveSequence == moveSequence;
	}

	if (pendingSequence != 0) {
		if (!pendingRegistered) {
			this->tracker->notePendingUserMove(pendingSequence, this, moveSequence);
		}
		return this->tracker->lastActionErrorSequence() != moveSequence;
	}

	if (this->tracker->windowFor(this->mHwnd) == this && ownerMatches && ownerMatches(this)) {
		this->clearActionErrorWithSequence(moveSequence);
	}
	return true;
}

void WindowTracker::notifyActionErrorChanged() {
	if (this->mNotifiedActionError == this->mLastActionError) return;
	this->mNotifiedActionError = this->mLastActionError;
	emit this->lastActionErrorChanged();
}

void WindowTracker::reportWindowActionRefusedWithSequence(
    quint64 sequence,
    const TrackedWindow* window,
    const QString& action,
    const QString& reason,
    bool elevated
) {
	if (sequence == 0 || this->mLastActionErrorSequence >= sequence) return;
	const auto text = window_action_detail::errorText(action, reason);
	this->mLastActionErrorSequence = sequence;
	this->mLastActionErrorWindow = window;
	this->mLastActionError = text;

	QPointer<WindowTracker> self(this);
	this->notifyActionErrorChanged();
	if (self && this->mLastActionErrorSequence == sequence && this->mLastActionError == text) {
		emit this->windowActionRefused(action, reason, elevated);
	}
}

void WindowTracker::reportActionRefusal(const QString& action, const QString& reason, bool elevated) {
	this->reportActionRefusalWithSequence(this->nextActionSequence(), action, reason, elevated);
}

void WindowTracker::reportActionRefusalWithSequence(
    quint64 sequence,
    const QString& action,
    const QString& reason,
    bool elevated
) {
	this->reportWindowActionRefusedWithSequence(sequence, nullptr, action, reason, elevated);
}

void WindowTracker::notePendingUserMove(
    quint64 sequence,
    TrackedWindow* window,
    quint64 moveSequence
) {
	if (sequence == 0 || window == nullptr || window->hwnd() == nullptr || moveSequence == 0) return;
	this->mPendingUserMoves.insert(
	    sequence,
	    PendingUserMove {
	        .window = window,
	        .hwnd = window->hwnd(),
	        .moveSequence = moveSequence,
	    }
	);
}

void WindowTracker::publishMoveFailure(
    quint64 moveSequence,
    TrackedWindow* window,
    HWND hwnd,
    const QString& code,
    const std::function<bool(const TrackedWindow*)>& ownerMatches,
    const std::function<bool(HWND)>& checkElevated
) {
	if (moveSequence == 0 || this->mLastActionErrorSequence >= moveSequence) return;
	if (window != nullptr
	    && (window->mLatestMoveSequence != moveSequence
	        || window->mLastActionErrorSequence >= moveSequence))
	{
		return;
	}

	const auto sameTracked = window != nullptr && this->byHwnd.value(hwnd) == window;
	if (sameTracked && ownerMatches && ownerMatches(window)) {
		const auto elevated = checkElevated ? checkElevated(hwnd) : false;
		QPointer<TrackedWindow> guardedWindow(window);
		QPointer<WindowTracker> guardedTracker(this);
		window->bElevated = elevated;
		if (!guardedTracker || guardedTracker->mLastActionErrorSequence >= moveSequence) return;
		if (!guardedWindow || guardedWindow->mLatestMoveSequence != moveSequence
		    || guardedWindow->mLastActionErrorSequence >= moveSequence)
		{
			return;
		}
		if (guardedTracker->byHwnd.value(hwnd) == guardedWindow && ownerMatches(guardedWindow)) {
			const auto reportElevated =
			    elevated && qs::hyprland::ipc::isWindowRejectionMoveCode(code);
			guardedWindow->reportActionRefusedWithSequence(
			    moveSequence,
			    QStringLiteral("move"),
			    qs::hyprland::ipc::moveRefusalReason(code, reportElevated),
			    reportElevated
			);
			return;
		}
		guardedTracker->reportActionRefusalWithSequence(
		    moveSequence,
		    QStringLiteral("move"),
		    qs::hyprland::ipc::moveRefusalReason(code, false),
		    false
		);
		return;
	}

	this->reportActionRefusalWithSequence(
	    moveSequence,
	    QStringLiteral("move"),
	    qs::hyprland::ipc::moveRefusalReason(code, false),
	    false
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
		if (sameTracked && window->mLatestMoveSequence == entry.moveSequence
		    && window->mLastActionErrorSequence < entry.moveSequence && ownerMatches
		    && ownerMatches(window))
		{
			window->clearActionErrorWithSequence(entry.moveSequence);
		}
		return;
	}

	this->publishMoveFailure(
	    entry.moveSequence, window, hwnd, code, ownerMatches, checkElevated
	);
}

} // namespace qs::windows
