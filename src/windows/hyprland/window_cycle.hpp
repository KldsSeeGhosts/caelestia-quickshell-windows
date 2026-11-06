#pragma once

#include <qlist.h>
#include <qpointer.h>

namespace qs::hyprland::ipc {

enum class WindowCycleResult { Accepted, Empty, Stale, Denied };

// Hyprland v0.54.0 getWindowCycle/circleNext:
// https://github.com/hyprwm/Hyprland/blob/0002f148c9a4fe421a9d33c0faa5528cdc411e62/src/Compositor.cpp
// Cycle the current Windows desktop (across monitors), including floating
// windows, with wraparound. Tracker adoption order supplies the initial order.
// Keep that snapshot while foreground is the last cycle target on this desktop.
// Unrelated foreground/desktop changes reset it; new windows join on reset.
// Weak references plus the caller's live identity check reject stale handles.
// Owned dialogs remain the responsibility of Window::activate(), not this walk.
template <typename Window>
class WindowCycle {
public:
	template <typename Eligible>
	WindowCycleResult cycle(
	    const QList<Window*>& windows,
	    Window* foreground,
	    qsizetype desktop,
	    bool backward,
	    Eligible eligible
	) {
		if (!this->lastActivated || foreground != this->lastActivated || desktop != this->desktop) {
			this->snapshot.clear();
			this->lastActivated.clear();
			this->desktop = desktop;
			for (auto* window: windows) {
				if (eligible(window)) this->snapshot.append(QPointer<Window>(window));
			}
			this->index = this->snapshot.indexOf(QPointer<Window>(foreground));
			// Hyprland starts at the first candidate when focus is clear.
			if (this->index < 0 && backward) this->index = 1;
			if (this->snapshot.isEmpty()) return WindowCycleResult::Empty;
		}
		const auto count = this->snapshot.size();
		for (qsizetype visited = 0; visited < count; ++visited) {
			this->index = (this->index + (backward ? -1 : 1) + count) % count;
			auto* window = this->snapshot[this->index].data();
			if (!window || !windows.contains(window) || !eligible(window)) continue;
			if (!window->activate()) {
				this->lastActivated.clear();
				return WindowCycleResult::Denied;
			}
			this->lastActivated = window;
			return WindowCycleResult::Accepted;
		}
		return WindowCycleResult::Stale;
	}

private:
	QList<QPointer<Window>> snapshot;
	QPointer<Window> lastActivated;
	qsizetype desktop = -1;
	qsizetype index = -1;
};

} // namespace qs::hyprland::ipc
