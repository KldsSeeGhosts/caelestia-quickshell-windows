#pragma once

#include <qobject.h>
#include <qscreen.h>
#include <qset.h>

namespace qs::windows {

// Which screens currently have a fullscreen occluder: the AppBar panel stack's
// verdict (system_surface::isFullscreenOccluder), published once so that every
// consumer shares it. Hyprland toplevels keep tool and no-activate windows out
// of the task list, so QML cannot derive fullscreen from them alone; a
// fullscreen player hidden from the taskbar still covers the screen.
//
// GUI thread only.
class FullscreenOccluders: public QObject {
	Q_OBJECT

public:
	static FullscreenOccluders* instance();

	[[nodiscard]] bool active(QScreen* screen) const;
	void set(QScreen* screen, bool active);

signals:
	void changed(QScreen* screen);

private:
	QSet<QScreen*> mActive;
	QSet<QScreen*> mWatched;
};

} // namespace qs::windows
