#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::sys {

class Wallpaper: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(bool lockScreenImageSupported READ lockScreenImageSupported NOTIFY
	               lockScreenImageSupportedChanged);
	Q_PROPERTY(bool systemDarkMode READ systemDarkMode NOTIFY systemDarkModeChanged);
	Q_PROPERTY(QString currentDesktop READ currentDesktop NOTIFY currentDesktopChanged);
	Q_PROPERTY(QVariantList desktops READ desktops NOTIFY desktopsChanged);

public:
	explicit Wallpaper(QObject* parent = nullptr);

	Q_INVOKABLE static QString currentWallpaper();

	Q_INVOKABLE static QString screenTargetId(const QString& screenName);

	Q_INVOKABLE static QString currentWallpaperForScreen(const QString& screenName);

	Q_INVOKABLE static bool setWallpaperForScreen(const QString& path, const QString& screenName);

	Q_INVOKABLE static bool setWallpaper(const QString& path, bool animate = true);

	Q_INVOKABLE static void setWallpaperInBackground(const QString& path);

	Q_INVOKABLE bool setDesktopWallpapers(const QVariantMap& paths, const QString& fallback);

	Q_INVOKABLE static bool isDarkMode();

	Q_INVOKABLE static void setDarkMode(bool dark);

	Q_INVOKABLE static bool setAccentColor(const QString& hex);

	Q_INVOKABLE static QString matugenPath();

	[[nodiscard]] bool lockScreenImageSupported() const { return this->mLockScreenImageSupported; }

	[[nodiscard]] bool systemDarkMode() const { return this->mSystemDarkMode; }

	[[nodiscard]] QString currentDesktop();
	[[nodiscard]] QVariantList desktops();

	Q_INVOKABLE void setLockScreenImage(const QString& path);

	Q_INVOKABLE static void openLockScreenPersonalizationSettings();

	Q_INVOKABLE static QString lockScreenMode();

signals:
	// The bool setters return true for success or live in-flight acceptance.
	// False means refused/completed failure and emits no result. Late outcomes
	// use this signal. For screen picks, target is screenTargetId(screenName);
	// for global/per-desktop batches it is empty. Path is native absolute form.
	void wallpaperOperationFinished(
	    const QString& operation,
	    const QString& path,
	    const QString& target,
	    bool ok,
	    const QString& reason
	);
	void lockScreenImageSupportedChanged();
	void lockScreenImageApplied(bool ok);
	void systemDarkModeChanged();
	void currentDesktopChanged();
	void desktopsChanged();

private:
	void applyLockScreenResult(bool ok, const QString& errorMessage);
	void refreshSystemDarkMode();
	void trackDesktops();

	bool mLockScreenImageSupported = true;
	bool mLockScreenAttemptRunning = false;
	bool mSystemDarkMode = false;
	bool mTrackingDesktops = false;
};

} // namespace qs::windows::sys
