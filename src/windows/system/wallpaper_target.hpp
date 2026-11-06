#pragma once

#include <qcryptographichash.h>
#include <qstringlist.h>

#include "../screen_identity.hpp"

namespace qs::windows::sys::wallpaper_target {

inline QString normalizedPath(QString path) { return path.replace(u'\\', u'/').toCaseFolded(); }

// Only the current registry snapshot belongs here. Labels, GDI sources and
// session-only identities cannot route IDesktopWallpaper operations.
inline QString devicePath(const QList<DisplayIdentity>& live, const QString& screenName) {
	if (screenName.isEmpty()) return {};
	QString target;
	auto matches = 0;
	for (const auto& identity: live) {
		if (identity.uniqueName.compare(screenName, Qt::CaseInsensitive) != 0) continue;
		++matches;
		target = identity.devicePath;
	}
	if (matches != 1 || target.isEmpty()) return {};

	// A mirrored/ambiguous target must not silently route to another screen.
	auto normalized = normalizedPath(target);
	auto targets = 0;
	for (const auto& identity: live) {
		if (normalizedPath(identity.devicePath) == normalized) ++targets;
	}
	return targets == 1 ? target : QString();
}

inline QString targetId(const QString& devicePath) {
	if (devicePath.isEmpty()) return {};
	return QString::fromLatin1(
	    QCryptographicHash::hash(normalizedPath(devicePath).toUtf8(), QCryptographicHash::Sha256)
	        .toHex()
	);
}

// COM may spell the interface path differently from DisplayConfig. Match only
// this target, but pass the exact COM-returned string back to IDesktopWallpaper.
inline QString monitorPath(const QStringList& paths, const QString& target) {
	if (target.isEmpty()) return {};
	auto normalized = normalizedPath(target);
	QString matched;
	auto matches = 0;
	for (const auto& path: paths) {
		if (normalizedPath(path) != normalized) continue;
		matched = path;
		++matches;
	}
	return matches == 1 ? matched : QString();
}

} // namespace qs::windows::sys::wallpaper_target
