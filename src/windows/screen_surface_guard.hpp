#pragma once

#include <functional>
#include <utility>

#include <qguiapplication.h>
#include <qobject.h>
#include <qpointer.h>
#include <qscreen.h>
#include <qtimer.h>
#include <qwindow.h>

namespace qs::windows {

// A specifically targeted surface must not follow Qt's primary-screen
// fallback during removal. Untargeted surfaces retain their normal behavior.
class ScreenSurfaceGuard {
public:
	ScreenSurfaceGuard(QObject* context, std::function<void()> hide,
	                   std::function<bool(QScreen*)> canRestore, std::function<void()> restore)
	    : context(context), hide(std::move(hide)), canRestore(std::move(canRestore)),
	      restore(std::move(restore)) {
		QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, context, [this](QScreen* screen) {
			if (screen == this->target) this->invalidate();
		});
	}

	void setWindow(QWindow* window) {
		QObject::disconnect(this->screenConnection);
		this->window = window;
		if (window == nullptr) return;
		this->screenConnection = QObject::connect(window, &QWindow::screenChanged, this->context,
		    [this](QScreen* screen) {
			    if (this->target != nullptr && screen != this->target) this->invalidate();
			    else this->scheduleRestore();
		    });
	}

	bool setTarget(QScreen* screen) {
		if (this->target != nullptr && !QGuiApplication::screens().contains(this->target))
			this->invalidate();
		auto wasBlocked = this->blocked;
		this->target = screen;
		if (screen != nullptr && QGuiApplication::screens().contains(screen)) this->blocked = false;
		return wasBlocked;
	}

	[[nodiscard]] bool isBlocked() const { return this->blocked; }
	[[nodiscard]] QScreen* targetScreen() const { return this->target; }

	// Called before Windows moves a visible tool window to another monitor.
	void movedOffTarget() {
		if (this->target != nullptr) this->invalidate();
	}

	// WM_DISPLAYCHANGE reports the monitor detached while Qt still holds its
	// QScreen. Hide before the removal migrates the surface to another screen.
	void screenDetached() {
		if (this->target != nullptr) this->invalidate();
	}

	void scheduleRestore() {
		if (!this->blocked || this->restorePending) return;
		this->restorePending = true;
		// Qt's Windows backend can move a window before it emits screenRemoved.
		// Let that synchronous removal finish before considering a return.
		QTimer::singleShot(0, this->context, [this] {
			this->restorePending = false;
			auto* target = this->target.data();
			if (!this->blocked || target == nullptr || this->window == nullptr
			    || !QGuiApplication::screens().contains(target) || this->window->screen() != target
			    || !this->canRestore(target))
				return;
			this->blocked = false;
			this->restore();
		});
	}

private:
	void invalidate() {
		if (this->blocked) return;
		this->blocked = true;
		this->hide();
	}

	QObject* context;
	std::function<void()> hide;
	std::function<bool(QScreen*)> canRestore;
	std::function<void()> restore;
	QPointer<QWindow> window;
	QPointer<QScreen> target;
	QMetaObject::Connection screenConnection;
	bool blocked = false;
	bool restorePending = false;
};

} // namespace qs::windows
