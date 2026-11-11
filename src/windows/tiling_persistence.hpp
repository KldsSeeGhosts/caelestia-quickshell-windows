#pragma once

#include <qjsonobject.h>
#include <qlist.h>
#include <qset.h>
#include <qstring.h>
#include <qtypes.h>

namespace qs::windows::tiling {

// A process creation FILETIME disambiguates PID reuse. Hints are SHA-256,
// not raw app ids or titles. Never use hints across process lifetimes.
struct WindowIdentity {
	quintptr hwnd = 0;
	quint32 pid = 0;
	quint64 started = 0;
	QString app;
	QString title;
};

struct SavedWindow {
	WindowIdentity identity;
	QString monitor;
	QString desktop;
	int override = 0;
	bool floating = false;
	bool autoFloat = false;
};

struct SavedLayout {
	QString monitor;
	QString desktop;
	int kind = 0;
	QJsonObject tree;
};

struct LayoutSnapshot {
	QList<SavedWindow> windows;
	QList<SavedLayout> layouts;
	QJsonObject options;
};

struct RestoreMatch {
	qsizetype saved = -1;
	qsizetype current = -1;
};

struct RestorePlan {
	// Snapshot order, before unmatched discovery order.
	QList<RestoreMatch> matched;
	QList<qsizetype> unmatched;
};

[[nodiscard]] QString persistenceHint(const QString& value);
[[nodiscard]] RestorePlan planRestore(
    const QList<SavedWindow>& saved,
    const QList<WindowIdentity>& current,
    const QSet<QString>& monitors,
    const QSet<QString>& desktops
);
[[nodiscard]] QJsonObject encodeSnapshot(const LayoutSnapshot& snapshot);
[[nodiscard]] LayoutSnapshot decodeSnapshot(const QJsonObject& object);
// Bounded read, strict version check, and atomic replacement. No path or
// snapshot contents are ever logged by these helpers.
[[nodiscard]] LayoutSnapshot readSnapshot(const QString& path);
[[nodiscard]] bool writeSnapshot(const QString& path, const LayoutSnapshot& snapshot);

} // namespace qs::windows::tiling
