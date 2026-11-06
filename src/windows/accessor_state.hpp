#pragma once

#include <qobject.h>

namespace qs::windows {

// Shared by the native accessor and fake-desktop fixtures. Publication is
// deferred because accessor loss can be discovered inside a pinned-state read.
class AccessorState: public QObject {
	Q_OBJECT;

public:
	using QObject::QObject;
	[[nodiscard]] bool accessorLoaded() const { return this->loaded; }
	[[nodiscard]] bool pinSupported() const { return this->loaded && this->pinExports; }
	[[nodiscard]] bool desktopControlSupported() const {
		return this->loaded && this->desktopControlExports;
	}

signals:
	void accessorStateChanged();

protected:
	void setAccessorLoaded(bool loaded) const {
		if (this->loaded == loaded) return;
		this->loaded = loaded;
		this->notifyAccessorStateChanged();
	}

	void setPinExports(bool isPinnedWindow, bool pinWindow, bool unPinWindow) {
		auto available = isPinnedWindow && pinWindow && unPinWindow;
		if (this->pinExports == available) return;
		this->pinExports = available;
		if (this->loaded) this->notifyAccessorStateChanged();
	}

	void setDesktopControlExports(bool goToDesktopNumber, bool createDesktop) {
		auto available = goToDesktopNumber && createDesktop;
		if (this->desktopControlExports == available) return;
		this->desktopControlExports = available;
		if (this->loaded) this->notifyAccessorStateChanged();
	}

private:
	void notifyAccessorStateChanged() const {
		auto* self = const_cast<AccessorState*>(this);
		QMetaObject::invokeMethod(
		    self,
		    [self]() { emit self->accessorStateChanged(); },
		    Qt::QueuedConnection
		);
	}

	mutable bool loaded = false;
	bool pinExports = false;
	bool desktopControlExports = false;
};

} // namespace qs::windows
