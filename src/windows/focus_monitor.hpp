#pragma once

#include <qobject.h>
#include <qpointer.h>
#include <qscreen.h>

namespace qs::windows {

// The focused monitor with Hyprland's latest-event-wins rules. A new
// foreground window always moves it; a cursor crossing moves it only while
// focus-follows-mouse is enabled; a click always moves it. One instance is
// the source the tiler, panels and Hyprland compatibility events read.
class FocusMonitor {
public:
	[[nodiscard]] QScreen* screen() const { return this->mScreen.data(); }

	[[nodiscard]] bool activeWindow(QScreen* screen) { return this->set(screen); }

	[[nodiscard]] bool cursorEntered(QScreen* screen, bool focusFollowsMouse) {
		return focusFollowsMouse && this->set(screen);
	}

	[[nodiscard]] bool clicked(QScreen* screen) { return this->set(screen); }

private:
	bool set(QScreen* screen) {
		if (screen == nullptr || screen == this->mScreen) return false;
		this->mScreen = screen;
		return true;
	}

	QPointer<QScreen> mScreen;
};

// Follow only real active-window changes. A duplicate foreground event or
// returning from a shell panel to the retained application must not override
// a later cursor crossing or click on another monitor.
template <typename Tracker, typename Follow>
void followActiveWindow(Tracker* tracker, QObject* context, Follow follow) {
	QObject::connect(tracker, &Tracker::activeWindowChanged, context, [tracker, follow]() {
		if (auto* window = tracker->activeWindow()) follow(window);
	});
}

} // namespace qs::windows
