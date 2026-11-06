#pragma once

#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qt_windows.h>

#include "system_surface.hpp"

namespace qs::windows {

class TrackedWindow;

// The shell's single view of the foreground application, fed by WindowTracker's
// existing EVENT_SYSTEM_FOREGROUND and EVENT_OBJECT_NAMECHANGE hook. It keeps
// the last application through the shell's own panels and the Windows shell
// overlays, and adopts a titled application that the taskbar/tiling rules do not
// track, so the bar can still name it. It never polls and never creates a
// synthetic toplevel.
class Foreground: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(Foreground);
	QML_SINGLETON;
	// Lowercase hexadecimal HWND without a 0x prefix, matching a Hyprland
	// toplevel address. Empty when no application is focused.
	Q_PROPERTY(QString address READ address NOTIFY addressChanged);
	Q_PROPERTY(QString title READ title NOTIFY titleChanged);
	Q_PROPERTY(QString appId READ appId NOTIFY appIdChanged);
	// The tracker's latest queued event sequence, including events the main
	// thread has not delivered yet. A consumer records it to reject events that
	// were queued before a boundary, even when they share a tick count.
	Q_PROPERTY(qulonglong sequence READ sequence);

public:
	explicit Foreground(QObject* parent = nullptr);

	[[nodiscard]] QString address() const;
	[[nodiscard]] QString title() const { return this->mTitle; }
	[[nodiscard]] QString appId() const { return this->mAppId; }
	[[nodiscard]] quint64 sequence() const;

signals:
	// Every raw foreground event, including one for the window that is already
	// displayed (for example, the user returning to the retained application
	// after a panel). `hwnd` is zero when none is focused, `time` is the event's
	// tick count and `sequence` orders it against `sequence`. The address,
	// title and app-id properties stay deduplicated and use their own signals.
	void foregroundChanged(qulonglong hwnd, uint time, qulonglong sequence);
	void addressChanged();
	void titleChanged();
	void appIdChanged();

private:
	void onForegroundChanged(HWND hwnd, quint32 time, quint64 sequence);
	void onForegroundNameChanged(HWND hwnd);
	void onWindowAdded(TrackedWindow* window);
	void onWindowRemoved(TrackedWindow* window);
	void adopt(HWND hwnd, const system_surface::Identity& identity);
	void clear();
	void bindTracked(TrackedWindow* window);
	void refreshTitle(const system_surface::Identity& identity);
	void refreshAppId(const system_surface::Identity& identity);

	HWND mHwnd = nullptr;
	QString mTitle;
	QString mAppId;
	QPointer<TrackedWindow> mTracked;
	QMetaObject::Connection mTrackedTitleConnection;
	QMetaObject::Connection mTrackedAppIdConnection;
};

} // namespace qs::windows
