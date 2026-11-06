#include "foreground_observer.hpp"

#include <qstring.h>

#include "foreground_policy.hpp"
#include "util.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

Foreground::Foreground(QObject* parent): QObject(parent) {
	auto* tracker = WindowTracker::instance();

	QObject::connect(
	    tracker,
	    &WindowTracker::foregroundEvent,
	    this,
	    &Foreground::onForegroundChanged
	);
	QObject::connect(
	    tracker,
	    &WindowTracker::foregroundNameChanged,
	    this,
	    &Foreground::onForegroundNameChanged
	);
	QObject::connect(tracker, &WindowTracker::windowAdded, this, &Foreground::onWindowAdded);
	QObject::connect(tracker, &WindowTracker::windowRemoved, this, &Foreground::onWindowRemoved);

	// Seed from the tracker: it may already hold the last application while one
	// of the shell's panels owns the foreground.
	if (auto* window = tracker->activeWindow(); window != nullptr) {
		adopt(window->hwnd(), system_surface::observe(window->hwnd()));
	} else if (auto* hwnd = GetForegroundWindow(); hwnd != nullptr) {
		onForegroundChanged(hwnd, GetTickCount(), tracker->eventSequence());
	}
}

QString Foreground::address() const {
	if (this->mHwnd == nullptr) return {};
	return QString::number(reinterpret_cast<quintptr>(this->mHwnd), 16);
}

quint64 Foreground::sequence() const {
	return WindowTracker::instance()->eventSequence();
}

void Foreground::onForegroundChanged(HWND hwnd, quint32 time, quint64 sequence) {
	if (hwnd == nullptr) {
		this->clear();
	} else {
		auto identity = system_surface::observe(hwnd);

		switch (foregroundAction(identity, isOwnProcessWindow(hwnd))) {
		case ForegroundAction::Adopt: this->adopt(hwnd, identity); break;
		case ForegroundAction::Clear: this->clear(); break;
		case ForegroundAction::Keep:
			if (this->mHwnd == nullptr) {
				if (auto* window = WindowTracker::instance()->activeWindow(); window != nullptr) {
					this->adopt(window->hwnd(), system_surface::observe(window->hwnd()));
				}
			} else {
				// A shell panel or overlay owns the foreground. Keep the last
				// application only while it still is one; a minimized or closed
				// application must not stay on screen just because the launcher has
				// focus.
				auto previous = system_surface::observe(this->mHwnd);
				if (!isForegroundApplication(previous)) {
					this->clear();
				} else {
					this->refreshTitle(previous);
					this->refreshAppId(previous);
				}
			}
			break;
		}
	}

	// Emit for every raw event, not only a display change: returning to the
	// retained application is a foreground event the focus guard must see.
	emit this->foregroundChanged(
	    static_cast<qulonglong>(reinterpret_cast<quintptr>(hwnd)),
	    time,
	    sequence
	);
}

void Foreground::onForegroundNameChanged(HWND hwnd) {
	if (hwnd == nullptr || hwnd != this->mHwnd) return;

	auto identity = system_surface::observe(hwnd);
	this->refreshTitle(identity);
	this->refreshAppId(identity);
}

void Foreground::onWindowAdded(TrackedWindow* window) {
	if (window == nullptr || window->hwnd() != this->mHwnd) return;

	this->bindTracked(window);
	auto identity = system_surface::observe(window->hwnd());
	this->refreshTitle(identity);
	this->refreshAppId(identity);
}

void Foreground::onWindowRemoved(TrackedWindow* window) {
	if (window == nullptr || window != this->mTracked) return;

	this->bindTracked(nullptr);
	if (this->mHwnd == nullptr) return;

	auto identity = system_surface::observe(this->mHwnd);
	this->refreshTitle(identity);
	this->refreshAppId(identity);
}

void Foreground::adopt(HWND hwnd, const system_surface::Identity& identity) {
	if (hwnd == this->mHwnd) {
		this->refreshTitle(identity);
		this->refreshAppId(identity);
		return;
	}

	this->mHwnd = hwnd;
	this->bindTracked(WindowTracker::instance()->windowFor(hwnd));
	this->refreshTitle(identity);
	this->refreshAppId(identity);

	emit this->addressChanged();
}

void Foreground::clear() {
	if (this->mHwnd == nullptr) return;

	this->mHwnd = nullptr;
	this->bindTracked(nullptr);
	this->mTitle.clear();
	this->mAppId.clear();

	emit this->addressChanged();
	emit this->titleChanged();
	emit this->appIdChanged();
}

void Foreground::bindTracked(TrackedWindow* window) {
	QObject::disconnect(this->mTrackedTitleConnection);
	QObject::disconnect(this->mTrackedAppIdConnection);
	this->mTracked = window;

	if (window == nullptr) return;

	this->mTrackedTitleConnection =
	    QObject::connect(window, &TrackedWindow::titleChanged, this, [this, window]() {
		    if (window != this->mTracked) return;
		    this->refreshTitle(system_surface::observe(window->hwnd()));
	    });
	this->mTrackedAppIdConnection =
	    QObject::connect(window, &TrackedWindow::appIdChanged, this, [this, window]() {
		    if (window != this->mTracked) return;
		    this->refreshAppId(system_surface::observe(window->hwnd()));
	    });
}

void Foreground::refreshTitle(const system_surface::Identity& identity) {
	auto title = this->mTracked != nullptr ? this->mTracked->title() : identity.title;
	if (title == this->mTitle) return;

	this->mTitle = title;
	emit this->titleChanged();
}

void Foreground::refreshAppId(const system_surface::Identity& identity) {
	auto appId = this->mTracked != nullptr ? this->mTracked->appId()
	                                       : identity.processName.toLower();
	if (appId.endsWith(QStringLiteral(".exe"))) appId.chop(4);
	if (appId == this->mAppId) return;

	this->mAppId = appId;
	emit this->appIdChanged();
}

} // namespace qs::windows
