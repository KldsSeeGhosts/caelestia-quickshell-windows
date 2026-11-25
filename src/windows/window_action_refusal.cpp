#include "window_tracker.hpp"

namespace qs::windows {

void WindowTracker::reportWindowActionRefused(
    const TrackedWindow* window,
    const QString& action,
    const QString& reason,
    bool elevated
) {
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

} // namespace qs::windows
