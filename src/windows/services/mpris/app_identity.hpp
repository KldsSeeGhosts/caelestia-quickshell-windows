#pragma once

#include <qstring.h>

namespace qs::windows::services::mpris {

/// User-visible identity of a media app, resolved from its AppUserModelID.
struct AppIdentity {
	/// Friendly application name, for example a package display name.
	QString name;
	/// Desktop entry id that the shell's existing icon lookup resolves, or the
	/// raw id when no better metadata exists.
	QString desktopEntry;
};

/// A desktop entry the shell already resolved for the same app id.
///
/// A runtime AppUserModelID need not be the AppsFolder token, for example an
/// explicitly set id. The desktop entry index bridges those ids through the
/// window's executable; supplying the matching entry lets the lookup stay
/// independent of that index.
struct AppIdentityHint {
	QString desktopEntry;
	QString parsingName;
	QString executablePath;
};

/// Resolve an AppUserModelID to an app name and desktop entry id.
///
/// Packaged apps are read through Windows.ApplicationModel.AppInfo. Desktop
/// apps use their AppsFolder shell item, then the executable's version
/// resources. The raw id is the last resort. Call this off the UI thread.
[[nodiscard]] AppIdentity resolveAppIdentity(const QString& aumid, const AppIdentityHint& hint = {});

} // namespace qs::windows::services::mpris
