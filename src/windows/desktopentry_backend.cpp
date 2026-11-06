#include "desktopentry_backend.hpp"

#include <utility>

#include <qdebug.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qloggingcategory.h>
#include <qmutex.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qthreadpool.h>

#include <qt_windows.h>

#include <objbase.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "../core/desktopentry.hpp"
#include "known_folders.hpp"
#include "redact.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logAppsFolder, "quickshell.windows.appsfolder", QtWarningMsg);

struct ComApartment {
	explicit ComApartment(DWORD coinit): hr(CoInitializeEx(nullptr, coinit)) {}

	ComApartment(const ComApartment&) = delete;
	ComApartment(ComApartment&&) = delete;
	ComApartment& operator=(const ComApartment&) = delete;
	ComApartment& operator=(ComApartment&&) = delete;

	~ComApartment() {
		if (SUCCEEDED(this->hr)) CoUninitialize();
	}

	HRESULT hr;
};

QString takeComString(PWSTR raw) {
	if (raw == nullptr) return QString();
	auto str = QString::fromWCharArray(raw);
	CoTaskMemFree(raw);
	return str;
}

const auto APPS_FOLDER_PREFIX = QStringLiteral("shell:AppsFolder\\");

QString normalizedExe(const QString& path) { return QDir::toNativeSeparators(path).toLower(); }

QString exeNameOf(const QString& path) {
	if (!path.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) return QString();
	return QFileInfo(QDir::fromNativeSeparators(path)).completeBaseName().toLower();
}

QString linkTarget(const ComPtr<IShellItem>& item) {
	ComPtr<IShellItem2> item2;
	if (FAILED(item.As(&item2))) return QString();

	PWSTR raw = nullptr;
	if (FAILED(item2->GetString(PKEY_Link_TargetParsingPath, &raw))) return QString();
	return takeComString(raw);
}

} // namespace

QMutex WindowsDesktopEntryBackend::sRegistryMutex;
QHash<QString, QString> WindowsDesktopEntryBackend::sRegistry;    // NOLINT
QHash<QString, QString> WindowsDesktopEntryBackend::sTargets;     // NOLINT
QHash<QString, QString> WindowsDesktopEntryBackend::sLinkTargets; // NOLINT
QHash<QString, QString> WindowsDesktopEntryBackend::sExeNames;    // NOLINT
QMutex WindowsDesktopEntryBackend::sWindowExeMutex;
QHash<QString, QString> WindowsDesktopEntryBackend::sWindowExes; // NOLINT

void WindowsDesktopEntryBackend::noteWindowExe(const QString& appId, const QString& exePath) {
	if (appId.isEmpty() || exePath.isEmpty()) return;
	QMutexLocker locker(&WindowsDesktopEntryBackend::sWindowExeMutex);
	WindowsDesktopEntryBackend::sWindowExes.insert(appId.toLower(), exePath);
}

QString WindowsDesktopEntryBackend::exeForAppId(const QString& appId) {
	if (appId.isEmpty()) return QString();
	QMutexLocker locker(&WindowsDesktopEntryBackend::sWindowExeMutex);
	return WindowsDesktopEntryBackend::sWindowExes.value(appId.toLower());
}

QString WindowsDesktopEntryBackend::aliasFor(const QString& name) {
	return WindowsDesktopEntryBackend::aliasForAppId(name);
}

QString WindowsDesktopEntryBackend::aliasForAppId(const QString& name) {
	static const QHash<QString, QString> shellAliases = {
	    {QStringLiteral("explorer"), QStringLiteral("microsoft.windows.explorer")},
	};

	auto key = name.toLower();
	// Executable AUMIDs such as "msedge.exe" are stored by their basename
	// without the extension: the desktop entry scan keys executables by stem
	// and the window tracker's fallback id also drops ".exe". Try the exact
	// id first, then that stem.
	auto stem = exeNameOf(key);

	// Read both window indexes before taking the registry lock; the two locks
	// are only ever taken in this order.
	auto exe = WindowsDesktopEntryBackend::exeForAppId(key);
	auto stemExe = stem.isEmpty() ? QString() : WindowsDesktopEntryBackend::exeForAppId(stem);

	QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);

	if (!exe.isEmpty()) {
		auto id = WindowsDesktopEntryBackend::sTargets.value(normalizedExe(exe));
		if (!id.isEmpty()) return id;
	}

	if (!stemExe.isEmpty()) {
		auto id = WindowsDesktopEntryBackend::sTargets.value(normalizedExe(stemExe));
		if (!id.isEmpty()) return id;
	}

	auto id = WindowsDesktopEntryBackend::sExeNames.value(key);
	if (!id.isEmpty()) return id;

	if (!stem.isEmpty()) {
		id = WindowsDesktopEntryBackend::sExeNames.value(stem);
		if (!id.isEmpty()) return id;
	}

	id = shellAliases.value(key);
	if (id.isEmpty() && !stem.isEmpty()) id = shellAliases.value(stem);
	return WindowsDesktopEntryBackend::sRegistry.contains(id) ? id : QString();
}

QString WindowsDesktopEntryBackend::parsingNameForId(const QString& id) {
	QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);
	return WindowsDesktopEntryBackend::sRegistry.value(id);
}

QString WindowsDesktopEntryBackend::idForParsingName(const QString& token) {
	QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);
	const auto& registry = WindowsDesktopEntryBackend::sRegistry;

	for (auto it = registry.constBegin(); it != registry.constEnd(); ++it) {
		if (it.value().compare(token, Qt::CaseInsensitive) == 0) return it.key();
	}

	return QString();
}

QString WindowsDesktopEntryBackend::linkTargetForId(const QString& id) {
	QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);
	return WindowsDesktopEntryBackend::sLinkTargets.value(id);
}

namespace {

// Adds key -> id unless the key already names a different app; a key that
// names more than one app is dropped and stays dropped.
void insertUnambiguous(
    QHash<QString, QString>& map,
    QSet<QString>& ambiguous,
    const QString& key,
    const QString& id
) {
	if (key.isEmpty() || ambiguous.contains(key)) return;
	auto existing = map.constFind(key);
	if (existing == map.cend()) {
		map.insert(key, id);
	} else if (existing.value() != id) {
		map.remove(key);
		ambiguous.insert(key);
	}
}

} // namespace

// Pure alias index: the COM scan only collects candidates. An alias is kept
// only while it names exactly one app. Two shortcuts can target the same
// executable with different arguments (browser profiles, Electron hosts), and
// unrelated apps share basenames (Squirrel's Update.exe, launcher.exe); first
// writer wins would resolve, and launch, whichever registered first. A dropped
// alias leaves an exact id or AUMID match, and the icon provider still uses the
// window's own executable.
DesktopEntryAliasMaps
WindowsDesktopEntryBackend::buildAliasMaps(const QList<DesktopEntryAliasCandidate>& entries) {
	DesktopEntryAliasMaps maps;
	QSet<QString> ambiguousTargets;
	QSet<QString> ambiguousExeNames;

	for (const auto& entry: entries) {
		if (!exeNameOf(entry.target).isEmpty()) {
			insertUnambiguous(maps.targets, ambiguousTargets, normalizedExe(entry.target), entry.id);
		}

		for (const auto& path: {entry.target, entry.token}) {
			insertUnambiguous(maps.exeNames, ambiguousExeNames, exeNameOf(path), entry.id);
		}
	}

	return maps;
}

void WindowsDesktopEntryBackend::install() {
	static WindowsDesktopEntryBackend backend; // NOLINT
	DesktopEntryManager::installBackend(&backend);
}

QStringList WindowsDesktopEntryBackend::watchPaths() {
	auto paths = QStringList();

	static const KNOWNFOLDERID* const START_MENU_PROGRAMS[] = {
	    &FOLDERID_CommonPrograms,
	    &FOLDERID_Programs,
	};

	for (const auto* id: START_MENU_PROGRAMS) {
		auto path = knownFolderPath(*id);
		if (!path.isEmpty()) paths.append(path);
	}

	return paths;
}

QList<ParsedDesktopEntryData> WindowsDesktopEntryBackend::scan() {
	auto results = QList<ParsedDesktopEntryData>();

	auto com = ComApartment(COINIT_MULTITHREADED);
	if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
		qCWarning(logAppsFolder) << "CoInitializeEx failed:" << Qt::hex << com.hr;
		return results;
	}

	ComPtr<IShellItem> appsFolder;
	auto hr = SHGetKnownFolderItem(
	    FOLDERID_AppsFolder,
	    KF_FLAG_DONT_VERIFY,
	    nullptr,
	    IID_PPV_ARGS(&appsFolder)
	);

	if (FAILED(hr)) {
		qCWarning(logAppsFolder) << "SHGetKnownFolderItem(FOLDERID_AppsFolder) failed:" << Qt::hex
		                         << hr;
		return results;
	}

	ComPtr<IEnumShellItems> enumItems;
	hr = appsFolder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumItems));

	if (FAILED(hr)) {
		qCWarning(logAppsFolder) << "BindToHandler(BHID_EnumItems) failed:" << Qt::hex << hr;
		return results;
	}

	auto registry = QHash<QString, QString>();
	auto linkTargets = QHash<QString, QString>();
	QList<DesktopEntryAliasCandidate> aliasCandidates;

	ComPtr<IShellItem> item;
	while (enumItems->Next(1, item.GetAddressOf(), nullptr) == S_OK) {
		auto current = item;
		item.Reset();

		PWSTR rawParsing = nullptr;
		if (FAILED(current->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &rawParsing))) continue;
		auto parsingName = takeComString(rawParsing);

		auto token = parsingName;
		if (token.startsWith(APPS_FOLDER_PREFIX, Qt::CaseInsensitive)) {
			token = token.sliced(APPS_FOLDER_PREFIX.length());
		}

		if (token.isEmpty()) continue;

		PWSTR rawDisplay = nullptr;
		auto displayHr = current->GetDisplayName(SIGDN_NORMALDISPLAY, &rawDisplay);
		auto displayName = SUCCEEDED(displayHr) ? takeComString(rawDisplay) : token;

		PWSTR rawFsPath = nullptr;
		auto fsHr = current->GetDisplayName(SIGDN_FILESYSPATH, &rawFsPath);
		auto fsPath = SUCCEEDED(fsHr) ? takeComString(rawFsPath) : QString();

		auto packaged = fsPath.isEmpty();

		QString id;
		QString workingDirectory;
		QVector<QString> keywords;

		if (packaged) {
			id = token.toLower();
			keywords = {token};
		} else {
			auto info = QFileInfo(fsPath);
			id = info.completeBaseName().toLower();
			workingDirectory = info.absolutePath();
			keywords = {info.completeBaseName(), info.fileName()};
		}

		if (id.isEmpty() || registry.contains(id)) continue;

		ParsedDesktopEntryData data;
		data.id = id;
		data.name = displayName;
		data.comment = packaged ? QString() : fsPath;
		data.icon = QStringLiteral("appicon:") + id;
		data.execString = token;
		data.command = {token};
		data.workingDirectory = workingDirectory;
		data.categories = {packaged ? QStringLiteral("PackagedApp") : QStringLiteral("Application")
		};
		data.keywords = keywords;

		registry.insert(id, token);
		results.append(std::move(data));

		auto target = linkTarget(current);
		if (!target.isEmpty()) linkTargets.insert(id, target);
		aliasCandidates.append({id, target, token});
	}

	auto aliases = WindowsDesktopEntryBackend::buildAliasMaps(aliasCandidates);

	{
		QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);
		WindowsDesktopEntryBackend::sRegistry = registry;
		WindowsDesktopEntryBackend::sTargets = aliases.targets;
		WindowsDesktopEntryBackend::sLinkTargets = linkTargets;
		WindowsDesktopEntryBackend::sExeNames = aliases.exeNames;
	}

	qCDebug(logAppsFolder) << "Scanned" << results.size() << "apps from the Windows Apps folder";
	return results;
}

namespace {

void launchToken(const QString& token, const QString& workingDirectory) {
	auto com = ComApartment(COINIT_APARTMENTTHREADED);

	ComPtr<IApplicationActivationManager> activationManager;
	auto hr = CoCreateInstance(
	    CLSID_ApplicationActivationManager,
	    nullptr,
	    CLSCTX_INPROC_SERVER,
	    IID_PPV_ARGS(&activationManager)
	);

	if (SUCCEEDED(hr)) {
		DWORD pid = 0;
		auto wtoken = token.toStdWString();
		hr = activationManager->ActivateApplication(wtoken.c_str(), nullptr, AO_NONE, &pid);
		if (SUCCEEDED(hr)) return;

		qCDebug(logAppsFolder) << "ActivateApplication failed for"
		                       << qs::windows::logId(token) << ":" << Qt::hex << hr
		                       << "- falling back to ShellExecuteExW";
	} else {
		qCDebug(logAppsFolder) << "CoCreateInstance(ApplicationActivationManager) failed:"
		                       << Qt::hex << hr;
	}

	auto shellPath = APPS_FOLDER_PREFIX + token;
	auto wpath = shellPath.toStdWString();
	auto wdir = workingDirectory.toStdWString();

	auto info = SHELLEXECUTEINFOW {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC;
	info.lpVerb = L"open";
	info.lpFile = wpath.c_str();
	info.lpDirectory = wdir.empty() ? nullptr : wdir.c_str();
	info.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&info)) {
		qCWarning(logAppsFolder) << "ShellExecuteExW failed for"
		                         << qs::windows::logPath(shellPath) << ":"
		                         << GetLastError();
	}
}

class LaunchTask: public QRunnable {
public:
	LaunchTask(QString token, QString workingDirectory)
	    : token(std::move(token))
	    , workingDirectory(std::move(workingDirectory)) {}

	void run() override { launchToken(this->token, this->workingDirectory); }

private:
	QString token;
	QString workingDirectory;
};

} // namespace

void WindowsDesktopEntryBackend::execute(
    const QVector<QString>& command,
    const QString& workingDirectory
) {
	if (command.isEmpty()) {
		qCWarning(logAppsFolder) << "Cannot launch: empty command";
		return;
	}

	WindowsDesktopEntryBackend::launch(command.first(), workingDirectory);
}

void WindowsDesktopEntryBackend::launch(const QString& token, const QString& workingDirectory) {
	if (token.isEmpty()) return;

	auto* task = new LaunchTask(token, workingDirectory);
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

} // namespace qs::windows
