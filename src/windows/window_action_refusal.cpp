#include "window_tracker.hpp"
#include "hyprland/dispatch_refusals.hpp"

namespace qs::windows {

quint64 TrackedWindow::beginMoveRequest() {
	this->mActionOrder->latestMoveSequence = this->tracker != nullptr
	    ? this->tracker->nextActionSequence()
	    : std::max(this->mActionOrder->latestMoveSequence, this->mActionOrder->lastActionErrorSequence)
	          + 1;
	return this->mActionOrder->latestMoveSequence;
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
	const auto sequence = this->tracker != nullptr
	    ? this->tracker->nextActionSequence()
	    : std::max(this->mActionOrder->latestMoveSequence, this->mActionOrder->lastActionErrorSequence)
	          + 1;
	this->reportActionRefusedWithSequence(sequence, action, reason, elevated);
}

void TrackedWindow::reportActionRefusedWithSequence(
    quint64 sequence,
    const QString& action,
    const QString& reason,
    bool elevated
) {
	if (sequence == 0 || this->mActionOrder->lastActionErrorSequence >= sequence
	    || this->mActionOrder->latestMoveSequence > sequence)
	{
		return;
	}
	auto* tracker = this->tracker;
	if (tracker != nullptr && tracker->mLastActionErrorSequence >= sequence) return;

	const auto text = window_action_detail::errorText(action, reason);
	this->mActionOrder->lastActionErrorSequence = sequence;
	this->mLastActionError = text;

	QPointer<TrackedWindow> self(this);
	QPointer<WindowTracker> guardedTracker(tracker);
	const auto actionOrder = this->mActionOrder;

	this->notifyActionErrorChanged();
	if (self && actionOrder->lastActionErrorSequence == sequence
	    && actionOrder->latestMoveSequence <= sequence && this->mLastActionError == text)
	{
		emit this->actionRefused(action, reason, elevated);
	}
	if (guardedTracker && guardedTracker->mLastActionErrorSequence < sequence
	    && actionOrder->lastActionErrorSequence == sequence
	    && actionOrder->latestMoveSequence <= sequence)
	{
		guardedTracker->reportWindowActionRefusedWithSequence(
		    sequence, self.data(), action, reason, elevated, actionOrder
		);
	}
}

void TrackedWindow::clearActionError() {
	const auto sequence = this->tracker != nullptr
	    ? this->tracker->nextActionSequence()
	    : std::max(this->mActionOrder->latestMoveSequence, this->mActionOrder->lastActionErrorSequence)
	          + 1;
	this->clearActionErrorWithSequence(sequence);
}

void TrackedWindow::clearActionErrorWithSequence(quint64 sequence) {
	if (sequence == 0 || this->mActionOrder->lastActionErrorSequence >= sequence) return;
	this->mActionOrder->lastActionErrorSequence = sequence;
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
    std::optional<bool> pendingOutcome,
    const QString& failureCode,
    quint64 moveSequence,
    const std::function<bool(const TrackedWindow*)>& ownerMatches,
    const std::function<bool(HWND)>& checkElevated,
    std::function<void(quint64)> onConfirmedSuccess
) {
	const auto actionOrder = this->mActionOrder;
	if (!accepted) {
		this->tracker->publishMoveFailure(
		    moveSequence, this, actionOrder, this->mHwnd, failureCode, ownerMatches, checkElevated
		);
		return false;
	}

	if (pendingOutcome.has_value() && !*pendingOutcome) {
		return false;
	}

	if (actionOrder->latestMoveSequence != moveSequence) {
		return false;
	}

	const auto hwnd = this->mHwnd;
	if (pendingSequence != 0) {
		if (!pendingRegistered && actionOrder->lastActionErrorSequence < moveSequence) {
			this->tracker->notePendingUserMove(
			    pendingSequence, this, moveSequence, nullptr, std::move(onConfirmedSuccess)
			);
		}
		return actionOrder->latestMoveSequence == moveSequence
		    && this->tracker->windowFor(hwnd) == this && ownerMatches && ownerMatches(this);
	}

	QPointer<TrackedWindow> self(this);
	QPointer<WindowTracker> guardedTracker(this->tracker);
	if (this->tracker->windowFor(hwnd) == this && ownerMatches && ownerMatches(this)) {
		if (actionOrder->lastActionErrorSequence < moveSequence) {
			this->clearActionErrorWithSequence(moveSequence);
		}
		const auto confirmed = self && guardedTracker
		    && actionOrder->latestMoveSequence == moveSequence
		    && guardedTracker->windowFor(hwnd) == self && ownerMatches(self);
		if (confirmed && onConfirmedSuccess) onConfirmedSuccess(moveSequence);
		return confirmed;
	}
	return false;
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
    bool elevated,
    const std::shared_ptr<WindowActionOrderState>& actionOrder
) {
	if (sequence == 0 || this->mLastActionErrorSequence >= sequence) return;
	if (actionOrder != nullptr
	    && (actionOrder->latestMoveSequence > sequence
	        || actionOrder->lastActionErrorSequence > sequence))
	{
		return;
	}
	const auto text = window_action_detail::errorText(action, reason);
	this->mLastActionErrorSequence = sequence;
	this->mLastActionErrorWindow = window;
	this->mLastActionError = text;

	QPointer<WindowTracker> self(this);
	this->notifyActionErrorChanged();
	if (self && this->mLastActionErrorSequence == sequence && this->mLastActionError == text
	    && (actionOrder == nullptr
	        || (actionOrder->latestMoveSequence <= sequence
	            && actionOrder->lastActionErrorSequence <= sequence)))
	{
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
    bool elevated,
    const std::shared_ptr<WindowActionOrderState>& actionOrder
) {
	this->reportWindowActionRefusedWithSequence(
	    sequence, nullptr, action, reason, elevated, actionOrder
	);
}

void WindowTracker::notePendingUserMove(
    quint64 sequence,
    TrackedWindow* window,
    quint64 moveSequence,
    std::shared_ptr<std::optional<bool>> completedOutcome,
    std::function<void(quint64)> onSucceeded
) {
	if (sequence == 0 || window == nullptr || window->hwnd() == nullptr || moveSequence == 0) return;
	this->mPendingUserMoves.insert(
	    sequence,
	    PendingUserMove {
	        .window = window,
	        .actionOrder = window->actionOrderState(),
	        .hwnd = window->hwnd(),
	        .moveSequence = moveSequence,
	        .completedOutcome = std::move(completedOutcome),
	        .onSucceeded = std::move(onSucceeded),
	    }
	);
}

void WindowTracker::publishMoveFailure(
    quint64 moveSequence,
    TrackedWindow* window,
    const std::shared_ptr<WindowActionOrderState>& actionOrder,
    HWND hwnd,
    const QString& code,
    const std::function<bool(const TrackedWindow*)>& ownerMatches,
    const std::function<bool(HWND)>& checkElevated
) {
	auto orderSuperseded = [&] {
		return actionOrder != nullptr
		    && (actionOrder->latestMoveSequence != moveSequence
		        || actionOrder->lastActionErrorSequence >= moveSequence);
	};
	if (moveSequence == 0 || this->mLastActionErrorSequence >= moveSequence || orderSuperseded()) {
		return;
	}

	const auto sameTracked = window != nullptr && this->byHwnd.value(hwnd) == window;
	if (sameTracked && ownerMatches && ownerMatches(window)) {
		const auto elevated = checkElevated ? checkElevated(hwnd) : false;
		QPointer<TrackedWindow> guardedWindow(window);
		QPointer<WindowTracker> guardedTracker(this);
		window->bElevated = elevated;
		if (!guardedTracker || guardedTracker->mLastActionErrorSequence >= moveSequence
		    || orderSuperseded())
		{
			return;
		}
		if (guardedWindow && guardedTracker->byHwnd.value(hwnd) == guardedWindow
		    && ownerMatches(guardedWindow))
		{
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
		    false,
		    actionOrder
		);
		return;
	}

	this->reportActionRefusalWithSequence(
	    moveSequence,
	    QStringLiteral("move"),
	    qs::hyprland::ipc::moveRefusalReason(code, false),
	    false,
	    actionOrder
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
	if (entry.completedOutcome) *entry.completedOutcome = succeeded;

	auto* window = entry.window.data();
	const auto actionOrder = entry.actionOrder;
	const auto sameTracked = window != nullptr && this->byHwnd.value(hwnd) == window;
	if (succeeded) {
		if (sameTracked && actionOrder != nullptr
		    && actionOrder->latestMoveSequence == entry.moveSequence && ownerMatches
		    && ownerMatches(window))
		{
			QPointer<TrackedWindow> guardedWindow(window);
			QPointer<WindowTracker> guardedTracker(this);
			if (actionOrder->lastActionErrorSequence < entry.moveSequence) {
				window->clearActionErrorWithSequence(entry.moveSequence);
			}
			if (entry.onSucceeded && guardedWindow && guardedTracker
			    && guardedTracker->byHwnd.value(hwnd) == guardedWindow
			    && actionOrder->latestMoveSequence == entry.moveSequence
			    && ownerMatches(guardedWindow))
			{
				entry.onSucceeded(entry.moveSequence);
			}
		}
		return;
	}

	this->publishMoveFailure(
	    entry.moveSequence, window, actionOrder, hwnd, code, ownerMatches, checkElevated
	);
}

} // namespace qs::windows
