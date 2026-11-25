#include "app_actions.hpp"

#include <optional>
#include <vector>

#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmutex.h>
#include <qobject.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtenvironmentvariables.h>
#include <quuid.h>
#include <qvariant.h>
#include <qxmlstream.h>

#include <qt_windows.h>

#include <appmodel.h>
#include <objbase.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "../../core/desktopentry.hpp"
#include "../../core/logcat.hpp"
#include "../desktopentry_backend.hpp"
#include "../process_launch.hpp"
#include "../redact.hpp"
#include "../util.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logAppActions, "quickshell.windows.appactions", QtWarningMsg);

const auto INSTALLED_APPS_URI = QStringLiteral("ms-settings:appsfeatures");
const auto APP_SETTINGS_URI = QStringLiteral("ms-settings:appsfeatures-app?");
constexpr qsizetype PUBLISHER_ID_LENGTH = 13;

QString takeComString(PWSTR raw) {
	if (raw == nullptr) return QString();
	auto str = QString::fromWCharArray(raw);
	CoTaskMemFree(raw);
	return str;
}

const QString& windowsDirectory() {
	static const auto dir = []() {
		wchar_t buffer[MAX_PATH] = {};
		auto length = GetSystemWindowsDirectoryW(buffer, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return QString();
		return QString::fromWCharArray(buffer, static_cast<qsizetype>(length));
	}();

	return dir;
}

bool isUnderWindows(const QString& path) {
	const auto& dir = windowsDirectory();
	if (dir.isEmpty() || path.size() <= dir.size()) return false;
	return path.startsWith(dir, Qt::CaseInsensitive) && path[dir.size()] == u'\\';
}

bool isFilesystemPath(const QString& path) {
	if (path.size() < 3) return false;
	if (path[0].isLetter() && path[1] == u':' && path[2] == u'\\') return true;
	return path.startsWith(QStringLiteral("\\\\")) && path[2] != u'?' && path[2] != u'.';
}

bool canElevate(const QString& path) {
	static const auto suffixes = QStringList {"exe", "bat", "cmd", "com", "msc"};
	return !path.isEmpty() && suffixes.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
}

QString packageFamilyOf(const QString& token) {
	auto bang = token.indexOf(u'!');
	if (bang <= 0) return QString();

	auto family = token.left(bang);
	auto underscore = family.lastIndexOf(u'_');
	if (underscore <= 0) return QString();

	auto publisher = QStringView(family).sliced(underscore + 1);
	if (publisher.size() != PUBLISHER_ID_LENGTH) return QString();

	for (auto c: publisher) {
		if (c.unicode() > 0x7f || !c.isLetterOrNumber()) return QString();
	}

	return family;
}

struct AppxManifestInfo {
	bool allowElevation = false;
	QString executionAlias;
};

AppxManifestInfo readAppxManifest(const QString& packagePath) {
	auto info = AppxManifestInfo();
	QFile file(QDir(packagePath).filePath(QStringLiteral("AppxManifest.xml")));
	if (!file.open(QFile::ReadOnly | QFile::Text)) return info;

	auto xml = QXmlStreamReader(&file);
	while (!xml.atEnd()) {
		xml.readNext();
		if (!xml.isStartElement()) continue;
		const auto name = xml.name();
		if (name == u"Capability") {
			const auto capability = xml.attributes().value(QStringLiteral("Name"));
			if (capability.compare(QStringLiteral("allowElevation"), Qt::CaseInsensitive) == 0) {
				info.allowElevation = true;
			}
		} else if (name == u"ExecutionAlias" && info.executionAlias.isEmpty()) {
			const auto alias = xml.attributes().value(QStringLiteral("Alias")).toString();
			if (!alias.isEmpty() && !alias.contains(u'/') && !alias.contains(u'\\')) {
				info.executionAlias = alias;
			}
		}
	}

	return info;
}

struct PackageInstallInfo {
	QString fullName;
	QString path;
	bool allowElevation = false;
	QString executionAlias;
};

std::optional<PackageInstallInfo> packageInstall(const QString& family) {
	auto wfamily = family.toStdWString();
	UINT32 count = 0;
	UINT32 length = 0;

	auto rc = GetPackagesByPackageFamily(wfamily.c_str(), &count, nullptr, &length, nullptr);
	if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0) return std::nullopt;

	auto names = std::vector<PWSTR>(count);
	auto buffer = std::vector<wchar_t>(length);
	rc = GetPackagesByPackageFamily(wfamily.c_str(), &count, names.data(), &length, buffer.data());
	if (rc != ERROR_SUCCESS || count == 0) return std::nullopt;

	UINT32 pathLength = 0;
	if (GetPackagePathByFullName(names.front(), &pathLength, nullptr) != ERROR_INSUFFICIENT_BUFFER) {
		return std::nullopt;
	}

	auto path = std::vector<wchar_t>(pathLength);
	if (GetPackagePathByFullName(names.front(), &pathLength, path.data()) != ERROR_SUCCESS) {
		return std::nullopt;
	}

	auto install = PackageInstallInfo {
	    .fullName = QString::fromWCharArray(names.front()),
	    .path = QDir::toNativeSeparators(QString::fromWCharArray(path.data())),
	};

	const auto manifest = readAppxManifest(install.path);
	install.allowElevation = manifest.allowElevation;
	if (!manifest.executionAlias.isEmpty()) {
		const auto localAppData = qEnvironmentVariable("LOCALAPPDATA");
		if (!localAppData.isEmpty()) {
			install.executionAlias = QDir::toNativeSeparators(
			    localAppData + QStringLiteral("/Microsoft/WindowsApps/") + manifest.executionAlias
			);
		}
	}

	return install;
}

QString knownFolderTarget(const QString& token) {
	if (!token.startsWith(u'{')) return QString();

	auto close = token.indexOf(u'}');
	if (close < 0 || close + 1 >= token.size() || token[close + 1] != u'\\') return QString();

	auto folderId = QUuid::fromString(QStringView(token).first(close + 1));
	if (folderId.isNull()) return QString();

	PWSTR raw = nullptr;
	auto hr = SHGetKnownFolderPath(static_cast<GUID>(folderId), KF_FLAG_DONT_VERIFY, nullptr, &raw);
	auto folder = takeComString(raw);
	if (FAILED(hr) || folder.isEmpty()) return QString();

	return folder + token.sliced(close + 1);
}

QString win32Target(const QString& id, const QString& token) {
	auto target = WindowsDesktopEntryBackend::linkTargetForId(id);
	if (target.isEmpty()) target = knownFolderTarget(token);
	if (target.isEmpty()) target = token;

	target = QDir::toNativeSeparators(target);
	return isFilesystemPath(target) ? target : QString();
}

struct LinkIdentity {
	QString aumid;
	QString target;
};

LinkIdentity readLink(const QString& path) {
	auto wpath = path.toStdWString();
	ComPtr<IShellItem2> item;
	if (FAILED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&item)))) return {};

	auto link = LinkIdentity();
	PWSTR raw = nullptr;
	if (SUCCEEDED(item->GetString(PKEY_AppUserModel_ID, &raw))) link.aumid = takeComString(raw);

	raw = nullptr;
	if (SUCCEEDED(item->GetString(PKEY_Link_TargetParsingPath, &raw))) {
		link.target = QDir::toNativeSeparators(takeComString(raw));
	}

	return link;
}

QStringList startMenuRoots() {
	auto roots = QStringList();

	for (const auto& folderId: {FOLDERID_StartMenu, FOLDERID_CommonStartMenu}) {
		PWSTR raw = nullptr;
		auto hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw);
		auto root = takeComString(raw);
		if (SUCCEEDED(hr) && !root.isEmpty()) roots.append(root);
	}

	return roots;
}

QString findShortcut(const AppActionInfo& app) {
	auto named = QStringList();
	auto others = QStringList();

	for (const auto& root: startMenuRoots()) {
		auto iter = QDirIterator(
		    root,
		    {QStringLiteral("*.lnk")},
		    QDir::Files | QDir::Hidden,
		    QDirIterator::Subdirectories
		);

		while (iter.hasNext()) {
			auto path = QDir::toNativeSeparators(iter.next());
			auto baseName = iter.fileInfo().completeBaseName();
			auto isNamed = baseName.compare(app.name, Qt::CaseInsensitive) == 0;
			(isNamed ? named : others).append(path);
		}
	}

	auto fallback = QString();

	for (const auto* list: {&named, &others}) {
		for (const auto& path: *list) {
			auto link = readLink(path);

			if (!link.aumid.isEmpty()) {
				if (link.aumid.compare(app.token, Qt::CaseInsensitive) == 0) return path;
				continue;
			}

			if (link.target.isEmpty() || link.target.compare(app.target, Qt::CaseInsensitive) != 0) {
				continue;
			}

			if (list == &named) return path;
			if (fallback.isEmpty()) fallback = path;
		}
	}

	return fallback;
}

struct ShortcutCache {
	QMutex mutex;
	QHash<QString, QString> paths;
};

ShortcutCache& shortcutCache() {
	static auto* cache = new ShortcutCache(); // NOLINT
	return *cache;
}

void clearShortcutCache() {
	auto& cache = shortcutCache();
	QMutexLocker locker(&cache.mutex);
	cache.paths.clear();
}

QString startMenuShortcut(const AppActionInfo& app) {
	auto& cache = shortcutCache();
	auto cached = QString();
	auto known = false;

	{
		QMutexLocker locker(&cache.mutex);
		auto it = cache.paths.constFind(app.token);
		known = it != cache.paths.constEnd();
		if (known) cached = *it;
	}

	if (known && (cached.isEmpty() || QFileInfo::exists(cached))) return cached;

	auto path = findShortcut(app);

	QMutexLocker locker(&cache.mutex);
	cache.paths.insert(app.token, path);
	return path;
}

DWORD runElevated(const QString& id, const AppActionInfo& app, HWND owner) {
	auto error = static_cast<DWORD>(ERROR_FILE_NOT_FOUND);

	if (app.packaged) {
		const auto shellTarget = QStringLiteral("shell:AppsFolder\\") + app.token;
		auto result = process::shellExecuteTarget(
		    shellTarget,
		    QString(),
		    QString(),
		    QStringLiteral("runas"),
		    owner,
		    true,
		    false
		);
		error = result.error;

		if (error != ERROR_SUCCESS && error != ERROR_CANCELLED && !app.executionAlias.isEmpty()) {
			qCDebug(logAppActions) << "Elevating the packaged app"
			                       << qs::windows::logId(id)
			                       << "through AppsFolder failed:" << error;
			result = process::shellExecuteTarget(
			    app.executionAlias,
			    QString(),
			    QString(),
			    QStringLiteral("runas"),
			    owner,
			    false,
			    false
			);
			error = result.error;
		}
	} else {
		auto shortcut = startMenuShortcut(app);

		if (!shortcut.isEmpty()) {
			error = process::shellExecuteTarget(
			            shortcut,
			            QString(),
			            QString(),
			            QStringLiteral("runas"),
			            owner,
			            true,
			            false
			        )
			        .error;
			if (error != ERROR_SUCCESS && error != ERROR_CANCELLED) {
				qCDebug(logAppActions) << "Elevating the shortcut of"
				                       << qs::windows::logId(id) << "failed:" << error;
			}
		}

		if (error != ERROR_SUCCESS && error != ERROR_CANCELLED) {
			auto directory = QDir::toNativeSeparators(QFileInfo(app.target).absolutePath());
			error = process::shellExecuteTarget(
			            app.target,
			            QString(),
			            directory,
			            QStringLiteral("runas"),
			            owner,
			            true,
			            false
			        )
			        .error;
		}
	}

	if (error == ERROR_CANCELLED) {
		qCDebug(logAppActions) << "Elevation of" << qs::windows::logId(id) << "was cancelled";
	} else if (error != ERROR_SUCCESS) {
		qCWarning(logAppActions) << "Could not run" << qs::windows::logId(id)
		                         << "as administrator:" << error;
	}

	return error;
}

QString windowsErrorString(DWORD error) {
	if (error == ERROR_SUCCESS) return QString();

	wchar_t buffer[1024] = {};
	const auto length = FormatMessageW(
	    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	    nullptr,
	    error,
	    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	    buffer,
	    static_cast<DWORD>(std::size(buffer)),
	    nullptr
	);
	if (length == 0) return QStringLiteral("Windows error %1").arg(error);
	return QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed();
}

AppActions::ActionResult actionResultForError(DWORD error) {
	if (error == ERROR_SUCCESS) return AppActions::ActionResult::Started;
	if (error == ERROR_CANCELLED) return AppActions::ActionResult::Cancelled;
	return AppActions::ActionResult::Refused;
}

QString actionStatus(AppActions::ActionResult result) {
	switch (result) {
	case AppActions::ActionResult::Started: return QStringLiteral("started");
	case AppActions::ActionResult::Cancelled: return QStringLiteral("cancelled");
	case AppActions::ActionResult::Refused: return QStringLiteral("refused");
	default: return QStringLiteral("notSupported");
	}
}

QVariantMap actionResultMap(AppActions::ActionResult result, DWORD error, const QString& message) {
	return {
	    {QStringLiteral("status"), actionStatus(result)},
	    {QStringLiteral("code"), static_cast<int>(result)},
	    {QStringLiteral("error"), message.isEmpty() ? windowsErrorString(error) : message},
	};
}

HWND hwndForObject(QObject* object) {
	if (object == nullptr) return nullptr;

	auto* window = qobject_cast<QWindow*>(object);
	if (window == nullptr) {
		if (auto* item = qobject_cast<QQuickItem*>(object)) window = item->window();
	}

	return hwndOf(window);
}

bool selectInExplorer(const QString& id, const QString& path) {
	auto wpath = path.toStdWString();
	PIDLIST_ABSOLUTE pidl = nullptr;

	auto hr = SHParseDisplayName(wpath.c_str(), nullptr, &pidl, 0, nullptr);
	if (FAILED(hr)) {
		qCDebug(logAppActions) << "SHParseDisplayName failed for"
		                       << qs::windows::logId(id) << ":" << Qt::hex << hr;
		return false;
	}

	AllowSetForegroundWindow(ASFW_ANY);
	hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
	ILFree(pidl);

	if (FAILED(hr)) {
		qCDebug(logAppActions) << "SHOpenFolderAndSelectItems failed for"
		                       << qs::windows::logId(id) << ":" << Qt::hex << hr;
		return false;
	}

	return true;
}

void openLocation(const QString& id, const AppActionInfo& app) {
	auto shortcut = startMenuShortcut(app);
	if (!shortcut.isEmpty() && selectInExplorer(id, shortcut)) return;
	if (selectInExplorer(id, app.target)) return;

	qCWarning(logAppActions) << "Could not open the file location of"
	                         << qs::windows::logId(id);
}

void openSettings(const QString& id, const QString& uri) {
	runLauncher([id, uri]() {
		const auto result = process::shellExecuteTarget(uri);
		if (!result.started()) {
			qCWarning(logAppActions) << "Could not open Settings for"
			                         << qs::windows::logId(id) << ":" << result.error;
		}
	});
}

} // namespace

AppActions::AppActions(QObject* parent): QObject(parent) {
	QObject::connect(
	    DesktopEntryManager::instance(),
	    &DesktopEntryManager::applicationsChanged,
	    this,
	    [this]() {
		    this->mCache.clear();
		    clearShortcutCache();
	    }
	);
}

AppActionInfo AppActions::details(const QString& entryId) {
	auto id = entryId.toLower();
	auto token = WindowsDesktopEntryBackend::parsingNameForId(id);
	if (token.isEmpty()) return AppActionInfo();

	auto cached = this->mCache.constFind(id);
	if (cached != this->mCache.constEnd() && cached->token == token) return *cached;

	auto app = AppActionInfo();
	app.token = token;
	app.packageFamilyName = packageFamilyOf(token);
	app.packaged = !app.packageFamilyName.isEmpty();

	if (app.packaged) {
		const auto install = packageInstall(app.packageFamilyName);
		if (install.has_value()) {
			app.canUninstall = !isUnderWindows(install->path);
			app.canRunAsAdmin = install->allowElevation;
			app.executionAlias = install->executionAlias;
		} else {
			// Preserve the old failure behavior: metadata lookup failure did not
			// prove that the package is a system package.
			app.canUninstall = true;
		}
	} else {
		app.target = win32Target(id, token);

		auto* entry = DesktopEntryManager::instance()->byId(id);
		if (entry != nullptr) app.name = entry->bindableName().value();

		app.canRunAsAdmin = canElevate(app.target);
		app.canOpenLocation = !app.target.isEmpty();
		app.canUninstall = !app.target.isEmpty() && !isUnderWindows(app.target);
	}

	this->mCache.insert(id, app);
	return app;
}

QVariantMap AppActions::info(const QString& entryId) {
	auto app = this->details(entryId);

	return {
	    {QStringLiteral("packaged"), app.packaged},
	    {QStringLiteral("canRunAsAdmin"), app.canRunAsAdmin},
	    {QStringLiteral("canOpenLocation"), app.canOpenLocation},
	    {QStringLiteral("canUninstall"), app.canUninstall},
	    {QStringLiteral("packageFamilyName"), app.packageFamilyName},
	};
}

void AppActions::setLastError(const QString& error) {
	if (this->mLastError == error) return;
	this->mLastError = error;
	emit this->lastErrorChanged();
}

bool AppActions::runAsAdmin(const QString& entryId, QObject* parentWindow) {
	const auto result = this->runAsAdminDetailed(entryId, parentWindow);
	return result.value(QStringLiteral("status")).toString() == QStringLiteral("started");
}

QVariantMap AppActions::runAsAdminDetailed(const QString& entryId, QObject* parentWindow) {
	auto app = this->details(entryId);
	if (!app.canRunAsAdmin) {
		const auto message = app.token.isEmpty()
		    ? QStringLiteral("The application entry is not available")
		    : QStringLiteral("The application does not support elevation");
		this->setLastError(message);
		return actionResultMap(
		    ActionResult::NotSupported,
		    ERROR_NOT_SUPPORTED,
		    message
		);
	}

	// ShellExecuteExW stays on the caller thread up to the UAC handoff. The
	// caller can pass its panel window so consent.exe receives that foreground
	// ownership before the panel is hidden.
	const auto error = runElevated(entryId, app, hwndForObject(parentWindow));
	const auto result = actionResultForError(error);
	this->setLastError(windowsErrorString(error));
	return actionResultMap(result, error, QString());
}

QVariantMap AppActions::launchShellAction(const QVariantMap& spec, QObject* parentWindow) {
	const auto verb = spec.value(QStringLiteral("verb"), QStringLiteral("open")).toString();
	if (verb != QStringLiteral("open") && verb != QStringLiteral("runas")) {
		const auto message = QStringLiteral("Only the open and runas verbs are supported");
		this->setLastError(message);
		return actionResultMap(ActionResult::NotSupported, ERROR_NOT_SUPPORTED, message);
	}

	auto target = spec.value(QStringLiteral("target")).toString();
	auto invokeIdList = false;
	if (const auto aumid = spec.value(QStringLiteral("aumid")).toString(); !aumid.isEmpty()) {
		target = QStringLiteral("shell:AppsFolder\\") + aumid;
		invokeIdList = true;
	} else if (const auto uri = spec.value(QStringLiteral("uri")).toString(); !uri.isEmpty()) {
		target = uri;
	}

	if (target.isEmpty()) {
		const auto message = QStringLiteral("A shell action needs a target, aumid, or uri");
		this->setLastError(message);
		return actionResultMap(ActionResult::NotSupported, ERROR_FILE_NOT_FOUND, message);
	}

	auto arguments = QString();
	const auto argsValue = spec.value(QStringLiteral("args"));
	if (argsValue.metaType().id() == QMetaType::QString) {
		arguments = argsValue.toString();
	} else if (argsValue.canConvert<QStringList>()) {
		arguments = process::joinCommandArguments(argsValue.toStringList());
	}

	const auto result = process::shellExecuteTarget(
	    target,
	    arguments,
	    spec.value(QStringLiteral("workingDir")).toString(),
	    verb,
	    hwndForObject(parentWindow),
	    invokeIdList,
	    verb == QStringLiteral("open")
	);

	auto actionResult = ActionResult::Refused;
	if (result.status == process::LaunchStatus::Started) actionResult = ActionResult::Started;
	else if (result.status == process::LaunchStatus::Cancelled) actionResult = ActionResult::Cancelled;

	this->setLastError(windowsErrorString(result.error));
	return actionResultMap(actionResult, result.error, QString());
}

bool AppActions::isInlineSudoEnabled() const {
	DWORD enabled = 0;
	DWORD size = sizeof(enabled);
	const auto rc = RegGetValueW(
	    HKEY_LOCAL_MACHINE,
	    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Sudo",
	    L"Enabled",
	    RRF_RT_REG_DWORD,
	    nullptr,
	    &enabled,
	    &size
	);
	return rc == ERROR_SUCCESS && enabled == 3;
}

bool AppActions::openFileLocation(const QString& entryId) {
	auto app = this->details(entryId);
	if (!app.canOpenLocation) return false;

	runLauncher([id = entryId, app]() { openLocation(id, app); });
	return true;
}

bool AppActions::openAppSettings(const QString& entryId) {
	auto app = this->details(entryId);
	if (!app.packaged) return false;

	openSettings(entryId, APP_SETTINGS_URI + app.packageFamilyName);
	return true;
}

bool AppActions::uninstall(const QString& entryId) {
	auto app = this->details(entryId);
	if (!app.canUninstall) return false;

	openSettings(
	    entryId,
	    app.packaged ? APP_SETTINGS_URI + app.packageFamilyName : INSTALLED_APPS_URI
	);
	return true;
}

} // namespace qs::windows::sys
