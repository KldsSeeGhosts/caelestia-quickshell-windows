#include "app_actions.hpp"

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qfuturewatcher.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmutex.h>
#include <qobject.h>
#include <qpromise.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qstring.h>
#include <qstringlist.h>
#include <quuid.h>
#include <qvariant.h>

#include <qt_windows.h>

#include <appmodel.h>
#include <objbase.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winioctl.h>
#include <wrl/client.h>

#include "../../core/desktopentry.hpp"
#include "../../core/logcat.hpp"
#include "../desktopentry_backend.hpp"
#include "../process_launch.hpp"
#include "../redact.hpp"
#include "../util.hpp"
#include "app_exec_alias.hpp"
#include "appx_manifest.hpp"
#include "shell_action_request.hpp"

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

bool isUncFilesystemPath(const QString& path) {
	if (path.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive)
	    || path.startsWith(QStringLiteral("\\\\.\\UNC\\"), Qt::CaseInsensitive))
	{
		return true;
	}
	if (!path.startsWith(QStringLiteral("\\\\"))) return false;
	return !path.startsWith(QStringLiteral("\\\\?\\"))
	    && !path.startsWith(QStringLiteral("\\\\.\\"));
}

std::optional<QChar> driveLetterOfFilesystemPath(const QString& path) {
	qsizetype offset = 0;
	if (path.startsWith(QStringLiteral("\\\\?\\"))) offset = 4;
	if (path.size() < offset + 3) return std::nullopt;

	const auto letter = path[offset];
	if (!letter.isLetter() || path[offset + 1] != u':' || path[offset + 2] != u'\\') {
		return std::nullopt;
	}
	return letter;
}

bool malformedFilesystemPath(const QString& path) {
	for (const auto c: path) {
		if (c.unicode() < 0x20 || c.unicode() == 0x7f) return true;
	}
	return false;
}

std::optional<QString> validFilesystemPath(const QString& path) {
	if (malformedFilesystemPath(path)) return std::nullopt;
	const auto native = QDir::toNativeSeparators(path);
	if (!driveLetterOfFilesystemPath(native).has_value() && !isUncFilesystemPath(native)) {
		return std::nullopt;
	}
	return native;
}

#ifndef SYMLINK_FLAG_RELATIVE
constexpr ULONG SYMLINK_FLAG_RELATIVE = 0x00000001UL;
#endif
#ifndef IO_REPARSE_TAG_WOF
constexpr DWORD IO_REPARSE_TAG_WOF = 0x80000017UL;
#endif

struct ReparsePointInfo {
	DWORD tag = 0;
	QString substituteTarget;
	bool relative = false;
};

#ifdef QS_TEST
using PropertiesInvoker = std::function<DWORD(const QString&, HWND, DWORD)>;
using DriveTypeInvoker = std::function<UINT(const QString&)>;
using FileAttributesInvoker = std::function<DWORD(const QString&)>;
using KnownFolderPathInvoker = std::function<HRESULT(const GUID&, DWORD, QString*)>;
using ReparsePointInvoker = std::function<bool(const QString&, DWORD*, QString*, bool*)>;
PropertiesInvoker propertiesInvokerForTest;
DriveTypeInvoker driveTypeInvokerForTest;
FileAttributesInvoker fileAttributesInvokerForTest;
KnownFolderPathInvoker knownFolderPathInvokerForTest;
ReparsePointInvoker reparsePointInvokerForTest;
std::optional<QStringList> shortcutRootsForTest;
#endif

UINT driveTypeForRoot(const QString& root) {
#ifdef QS_TEST
	if (driveTypeInvokerForTest) return driveTypeInvokerForTest(root);
#endif
	const auto wide = root.toStdWString();
	return GetDriveTypeW(wide.c_str());
}

bool isLocalDriveType(UINT driveType) {
	switch (driveType) {
	case DRIVE_FIXED:
	case DRIVE_REMOVABLE:
	case DRIVE_CDROM:
	case DRIVE_RAMDISK: return true;
	default: return false;
	}
}

DWORD fileAttributes(const QString& path) {
#ifdef QS_TEST
	if (fileAttributesInvokerForTest) return fileAttributesInvokerForTest(path);
#endif
	const auto wide = path.toStdWString();
	return GetFileAttributesW(wide.c_str());
}

std::optional<ReparsePointInfo> readReparsePoint(const QString& path) {
#ifdef QS_TEST
	if (reparsePointInvokerForTest) {
		auto info = ReparsePointInfo();
		if (!reparsePointInvokerForTest(path, &info.tag, &info.substituteTarget, &info.relative)) {
			return std::nullopt;
		}
		return info;
	}
#endif
	const auto wide = path.toStdWString();
	auto* handle = CreateFileW(
	    wide.c_str(),
	    0,
	    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    nullptr,
	    OPEN_EXISTING,
	    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	    nullptr
	);
	if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

	auto buffer = QByteArray(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, '\0');
	DWORD returned = 0;
	const auto ok = DeviceIoControl(
	    handle,
	    FSCTL_GET_REPARSE_POINT,
	    nullptr,
	    0,
	    buffer.data(),
	    static_cast<DWORD>(buffer.size()),
	    &returned,
	    nullptr
	);
	CloseHandle(handle);
	if (!ok || returned < 8) return std::nullopt;

	DWORD tag = 0;
	USHORT dataLength = 0;
	std::memcpy(&tag, buffer.constData(), sizeof(tag));
	std::memcpy(&dataLength, buffer.constData() + 4, sizeof(dataLength));
	const auto totalLength = DWORD(dataLength) + 8;
	if (totalLength > returned) return std::nullopt;

	auto info = ReparsePointInfo();
	info.tag = tag;
	if (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT) {
		const auto headerSize = DWORD(tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16);
		if (totalLength < headerSize) return std::nullopt;

		USHORT subOffset = 0;
		USHORT subLength = 0;
		std::memcpy(&subOffset, buffer.constData() + 8, sizeof(subOffset));
		std::memcpy(&subLength, buffer.constData() + 10, sizeof(subLength));
		if ((subOffset % 2) != 0 || (subLength % 2) != 0) return std::nullopt;

		const auto start = headerSize + DWORD(subOffset);
		if (start > totalLength || DWORD(subLength) > totalLength - start) return std::nullopt;

		if (tag == IO_REPARSE_TAG_SYMLINK) {
			ULONG flags = 0;
			std::memcpy(&flags, buffer.constData() + 16, sizeof(flags));
			info.relative = (flags & SYMLINK_FLAG_RELATIVE) != 0;
		}
		info.substituteTarget = QString::fromUtf16(
		    reinterpret_cast<const char16_t*>(buffer.constData() + start),
		    static_cast<qsizetype>(subLength / 2)
		);
	}
	return info;
}

QString normalizeReparseTarget(const QString& parentDir, const ReparsePointInfo& info) {
	auto target = info.substituteTarget;
	if (target.isEmpty()) return {};

	if (info.relative) {
		const auto combined = parentDir + QStringLiteral("\\") + target;
		return QDir::toNativeSeparators(QDir::cleanPath(combined));
	}

	if (target.startsWith(QStringLiteral("\\??\\UNC\\"), Qt::CaseInsensitive)) {
		return QStringLiteral("\\\\?\\UNC\\") + target.mid(8);
	}
	if (target.startsWith(QStringLiteral("\\??\\"))) {
		target = target.mid(4);
	}
	return QDir::toNativeSeparators(target);
}

std::optional<QString> resolveLocalFilesystemPath(const QString& path, int depth) {
	constexpr int MAX_REPARSE_DEPTH = 8;
	if (depth > MAX_REPARSE_DEPTH) return std::nullopt;

	const auto valid = validFilesystemPath(path);
	if (!valid.has_value()) return std::nullopt;
	if (isUncFilesystemPath(*valid)) return std::nullopt;

	const auto letter = driveLetterOfFilesystemPath(*valid);
	if (!letter.has_value()) return std::nullopt;
	const auto driveRoot = QString(*letter) + QStringLiteral(":\\");
	if (!isLocalDriveType(driveTypeForRoot(driveRoot))) return std::nullopt;

	const auto rootLength = valid->startsWith(QStringLiteral("\\\\?\\")) ? qsizetype(7) : qsizetype(3);
	auto current = valid->left(rootLength);
	const auto remainder = valid->mid(rootLength);
	const auto segments = remainder.split(u'\\', Qt::SkipEmptyParts);

	if (segments.isEmpty()) {
		return fileAttributes(current) != INVALID_FILE_ATTRIBUTES ? std::optional<QString>(current)
		                                                          : std::nullopt;
	}

	for (const auto& segment: segments) {
		if (segment == QStringLiteral(".")) continue;
		if (segment == QStringLiteral("..")) return std::nullopt;

		const auto parentDir = current;
		current = current.endsWith(u'\\') ? (current + segment) : (current + QStringLiteral("\\") + segment);

		const auto attrs = fileAttributes(current);
		if (attrs == INVALID_FILE_ATTRIBUTES) return std::nullopt;
		if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) continue;

		const auto reparse = readReparsePoint(current);
		if (!reparse.has_value()) return std::nullopt;

		if (reparse->tag == IO_REPARSE_TAG_SYMLINK || reparse->tag == IO_REPARSE_TAG_MOUNT_POINT) {
			if (!reparse->relative
			    && reparse->substituteTarget.startsWith(QStringLiteral("\\??\\Volume{"), Qt::CaseInsensitive))
			{
				const auto closeBrace = reparse->substituteTarget.indexOf(u'}');
				if (closeBrace < 0) return std::nullopt;
				const auto volumeEnd = closeBrace + 1;
				const auto trailing = reparse->substituteTarget.mid(volumeEnd);
				if (!trailing.isEmpty() && trailing != QStringLiteral("\\")) return std::nullopt;
				const auto volumeRoot = QStringLiteral("\\\\?\\")
				    + reparse->substituteTarget.mid(4, volumeEnd - 4) + QStringLiteral("\\");
				if (!isLocalDriveType(driveTypeForRoot(volumeRoot))) return std::nullopt;
				continue;
			}

			const auto target = normalizeReparseTarget(parentDir, *reparse);
			const auto resolved = resolveLocalFilesystemPath(target, depth + 1);
			if (!resolved.has_value()) return std::nullopt;
			current = *resolved;
			continue;
		}

		const auto localFileTag = (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0
		    && (reparse->tag == IO_REPARSE_TAG_WOF || reparse->tag == IO_REPARSE_TAG_APPEXECLINK
		        || reparse->tag == IO_REPARSE_TAG_DEDUP);
		if (!localFileTag) return std::nullopt;
	}

	return current;
}

std::optional<QString> validLocalFilesystemPath(const QString& path) {
	const auto valid = validFilesystemPath(path);
	if (!valid.has_value()) return std::nullopt;
	return resolveLocalFilesystemPath(*valid, 0).has_value() ? valid : std::nullopt;
}

bool existingFilesystemPath(const QString& path) {
	return fileAttributes(path) != INVALID_FILE_ATTRIBUTES;
}

QString existingLocalFilesystemPath(const QString& path) {
	return validLocalFilesystemPath(path).value_or(QString());
}

enum class ShortcutLookupMode : quint8 { Unfiltered, LocalOnly };

QString existingCandidatePath(const QString& path, ShortcutLookupMode mode) {
	if (mode == ShortcutLookupMode::LocalOnly) return existingLocalFilesystemPath(path);
	const auto valid = validFilesystemPath(path);
	return valid.has_value() && existingFilesystemPath(*valid) ? *valid : QString();
}

HRESULT knownFolderPath(const KNOWNFOLDERID& folderId, DWORD flags, QString* path) {
#ifdef QS_TEST
	if (knownFolderPathInvokerForTest) return knownFolderPathInvokerForTest(folderId, flags, path);
#endif
	PWSTR raw = nullptr;
	const auto hr = SHGetKnownFolderPath(folderId, flags, nullptr, &raw);
	*path = takeComString(raw);
	return hr;
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

struct PackageInstallInfo {
	QString path;
	bool allowElevation = false;
	QString executionAlias;
	QString elevationTarget;
};

std::optional<AppExecAliasInfo> verifiedPackageAlias(
    const QString& family,
    const QStringList& aliases,
    const QString& aumid,
    const QString& installRoot
) {
	if (aliases.isEmpty()) return std::nullopt;

	PWSTR raw = nullptr;
	const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
	const auto localAppData = takeComString(raw);
	if (FAILED(hr) || localAppData.isEmpty()) return std::nullopt;

	// The unscoped alias can belong to a different package. Prefer the
	// package-scoped alias, then verify package identity and install target
	// inside the reparse payload before accepting either location.
	const auto root = QDir(localAppData + QStringLiteral("/Microsoft/WindowsApps"));
	const auto scoped = QDir(root.filePath(family));
	for (const auto& alias: aliases) {
		// Prefer the package-scoped alias. Also accept the legacy unscoped alias
		// only after its reparse payload proves the same family and AUMID.
		for (const auto& path:
		     {QDir::toNativeSeparators(scoped.filePath(alias)),
		      QDir::toNativeSeparators(root.filePath(alias))})
		{
			if (auto info = verifiedAppExecAlias(path, family, aumid, installRoot)) return info;
		}
	}
	return std::nullopt;
}

std::optional<PackageInstallInfo> packageInstall(const QString& family, QStringView applicationId) {
	auto wfamily = family.toStdWString();
	UINT32 count = 0;
	UINT32 length = 0;

	auto rc = GetPackagesByPackageFamily(wfamily.c_str(), &count, nullptr, &length, nullptr);
	if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0) return std::nullopt;

	auto names = std::vector<PWSTR>(count);
	auto buffer = std::vector<wchar_t>(length);
	rc = GetPackagesByPackageFamily(wfamily.c_str(), &count, names.data(), &length, buffer.data());
	if (rc != ERROR_SUCCESS || count == 0) return std::nullopt;

	auto firstInstall = std::optional<PackageInstallInfo>();
	for (UINT32 i = 0; i < count; ++i) {
		UINT32 pathLength = 0;
		if (GetPackagePathByFullName(names[i], &pathLength, nullptr) != ERROR_INSUFFICIENT_BUFFER) {
			continue;
		}

		auto path = std::vector<wchar_t>(pathLength);
		if (GetPackagePathByFullName(names[i], &pathLength, path.data()) != ERROR_SUCCESS) continue;

		auto install = PackageInstallInfo();
		install.path = QDir::toNativeSeparators(QString::fromWCharArray(path.data()));
		if (!firstInstall) firstInstall = install;
		QFile file(QDir(install.path).filePath(QStringLiteral("AppxManifest.xml")));
		if (!file.open(QFile::ReadOnly)) continue;

		const auto manifest = readAppxManifest(&file, applicationId);
		if (!manifest.applicationFound) continue;
		install.allowElevation = manifest.allowElevation;
		const auto aumid = family + u'!' + applicationId.toString();
		if (const auto alias = verifiedPackageAlias(family, manifest.desktopExecutionAliases, aumid, install.path)) {
			install.executionAlias = alias->aliasName;
			install.elevationTarget = alias->targetPath;
		}
		return install;
	}
	return firstInstall;
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

QString win32Target(const QString& id, const QString& token, QString* registeredTarget = nullptr) {
	auto target = WindowsDesktopEntryBackend::linkTargetForId(id);
	if (registeredTarget != nullptr) *registeredTarget = QDir::toNativeSeparators(target);
	if (target.isEmpty()) target = knownFolderTarget(token);
	if (target.isEmpty()) target = token;

	target = QDir::toNativeSeparators(target);
	return validFilesystemPath(target).value_or(QString());
}

struct LinkIdentity {
	QString aumid;
	QString target;
};

QString normalizedNamespaceTarget(const QString& target) {
	auto normalized = target;
	if (normalized.startsWith(QStringLiteral("shell:::"), Qt::CaseInsensitive)) {
		normalized.remove(0, QStringLiteral("shell:").size());
	}
	return normalized.startsWith(QStringLiteral("::{")) ? normalized.toLower() : QString();
}

bool sameLinkTarget(const QString& left, const QString& right) {
	if (left.compare(right, Qt::CaseInsensitive) == 0) return true;
	const auto leftNamespace = normalizedNamespaceTarget(left);
	return !leftNamespace.isEmpty() && leftNamespace == normalizedNamespaceTarget(right);
}

LinkIdentity readLink(const QString& path) {
	const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(com) && com != RPC_E_CHANGED_MODE) return {};

	auto wpath = path.toStdWString();
	auto link = LinkIdentity();
	ComPtr<IShellItem2> item;
	if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
		PWSTR raw = nullptr;
		if (SUCCEEDED(item->GetString(PKEY_AppUserModel_ID, &raw))) link.aumid = takeComString(raw);

		raw = nullptr;
		if (SUCCEEDED(item->GetString(PKEY_Link_TargetParsingPath, &raw))) {
			link.target = QDir::toNativeSeparators(takeComString(raw));
		}
	}

	ComPtr<IShellLinkW> shellLink;
	if (SUCCEEDED(CoCreateInstance(
	        CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink)
	    )))
	{
		ComPtr<IPersistFile> persist;
		if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&persist)))
		    && SUCCEEDED(persist->Load(wpath.c_str(), STGM_READ)))
		{
			PIDLIST_ABSOLUTE pidl = nullptr;
			if (SUCCEEDED(shellLink->GetIDList(&pidl)) && pidl != nullptr) {
				PWSTR parsingName = nullptr;
				if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &parsingName))) {
					link.target = takeComString(parsingName);
				}
				CoTaskMemFree(pidl);
			}

			wchar_t target[MAX_PATH] = {};
			if (link.target.isEmpty()
			    && SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)))
			{
				link.target = QDir::toNativeSeparators(QString::fromWCharArray(target));
			}
		}
	}

	item.Reset();
	shellLink.Reset();
	if (SUCCEEDED(com)) CoUninitialize();
	return link;
}

QStringList startMenuRoots(ShortcutLookupMode mode) {
	auto candidates = QStringList();
	auto useTestRoots = false;
#ifdef QS_TEST
	useTestRoots = shortcutRootsForTest.has_value();
	if (useTestRoots) candidates = *shortcutRootsForTest;
#endif

	if (!useTestRoots) {
		const auto localOnly = mode == ShortcutLookupMode::LocalOnly;
		const auto flags = localOnly ? DWORD(KF_FLAG_DONT_VERIFY) : DWORD(KF_FLAG_DEFAULT);
		for (const auto& folderId: {FOLDERID_StartMenu, FOLDERID_CommonStartMenu}) {
			auto root = QString();
			const auto hr = knownFolderPath(folderId, flags, &root);
			if (SUCCEEDED(hr) && !root.isEmpty()) candidates.append(root);
		}
	}

	if (mode == ShortcutLookupMode::Unfiltered) return candidates;

	auto roots = QStringList();
	for (const auto& root: candidates) {
		if (const auto local = resolveLocalFilesystemPath(root, 0)) roots.append(*local);
	}
	return roots;
}

QString findShortcut(const AppActionInfo& app, ShortcutLookupMode mode) {
	auto named = QStringList();
	auto others = QStringList();

	for (const auto& root: startMenuRoots(mode)) {
		if (mode == ShortcutLookupMode::LocalOnly) {
			auto pending = QStringList {root};
			while (!pending.isEmpty()) {
				const auto dir = pending.takeLast();
				const auto prefix = dir.endsWith(u'\\') ? dir : (dir + QStringLiteral("\\"));
				const auto pattern = (prefix + QStringLiteral("*")).toStdWString();
				WIN32_FIND_DATAW entry {};
				auto* handle = FindFirstFileExW(
				    pattern.c_str(),
				    FindExInfoBasic,
				    &entry,
				    FindExSearchNameMatch,
				    nullptr,
				    FIND_FIRST_EX_LARGE_FETCH
				);
				if (handle == INVALID_HANDLE_VALUE) continue;

				do {
					const auto name = QString::fromWCharArray(entry.cFileName);
					if (name == QStringLiteral(".") || name == QStringLiteral("..")) continue;
					const auto entryPath = prefix + name;
					const auto attrs = entry.dwFileAttributes;
					if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
						if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
						pending.append(entryPath);
						continue;
					}
					if (!name.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive)) continue;
					if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0
					    && !validLocalFilesystemPath(entryPath).has_value())
					{
						continue;
					}
					const auto baseName = QFileInfo(name).completeBaseName();
					const auto isNamed = baseName.compare(app.name, Qt::CaseInsensitive) == 0;
					(isNamed ? named : others).append(entryPath);
				} while (FindNextFileW(handle, &entry));
				FindClose(handle);
			}
			continue;
		}

		auto iter = QDirIterator(
		    root,
		    {QStringLiteral("*.lnk")},
		    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoSymLinks,
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

			const auto matchesTarget = !link.target.isEmpty()
			    && (sameLinkTarget(link.target, app.target)
			        || (!app.linkTarget.isEmpty() && sameLinkTarget(link.target, app.linkTarget)));
			if (!matchesTarget) continue;

			if (list == &named) return path;
			if (fallback.isEmpty()) fallback = path;
		}
	}

	return fallback;
}

struct ShortcutCache {
	QMutex mutex;
	QHash<QString, QString> paths;
	QHash<QString, QString> localPaths;
};

ShortcutCache& shortcutCache() {
	static auto* cache = new ShortcutCache(); // NOLINT
	return *cache;
}

void clearShortcutCache() {
	auto& cache = shortcutCache();
	QMutexLocker locker(&cache.mutex);
	cache.paths.clear();
	cache.localPaths.clear();
}

QString startMenuShortcut(const AppActionInfo& app, ShortcutLookupMode mode) {
	const auto registered = existingCandidatePath(app.shortcutPath, mode);
	if (!registered.isEmpty()) return registered;

	auto& cache = shortcutCache();
	auto* paths = mode == ShortcutLookupMode::LocalOnly ? &cache.localPaths : &cache.paths;
	auto cached = QString();

	{
		QMutexLocker locker(&cache.mutex);
		cached = paths->value(app.token);
	}

	if (!cached.isEmpty()) {
		const auto existing = existingCandidatePath(cached, mode);
		if (!existing.isEmpty()) return existing;
	}

	auto path = findShortcut(app, mode);

	QMutexLocker locker(&cache.mutex);
	if (path.isEmpty()) paths->remove(app.token);
	else paths->insert(app.token, path);
	return path;
}

QString resolvePropertiesTarget(const AppActionInfo& app) {
	if (app.token.isEmpty() || app.packaged) return {};

	for (const auto& candidate:
	     {startMenuShortcut(app, ShortcutLookupMode::LocalOnly), app.target})
	{
		const auto existing = existingLocalFilesystemPath(candidate);
		if (!existing.isEmpty()) return existing;
	}
	return {};
}

DWORD runElevated(const QString& id, const AppActionInfo& app, HWND owner) {
	const auto launch = [&](const QString& target, bool invokeIdList, const QString& directory = {}) {
		return process::shellExecuteTarget(
		    target, {}, directory, QStringLiteral("runas"), owner, invokeIdList
		);
	};
	auto result = process::detail::launchResultForError(ERROR_FILE_NOT_FOUND);

	if (app.packaged) {
		if (app.allowAppsFolderElevation) {
			result = launch(QStringLiteral("shell:AppsFolder\\") + app.token, true);
		}
		result = process::detail::fallbackOnFailure(result, [&] {
			if (app.executionAlias.isEmpty()) return result;
			// Resolve the registered package install directory and re-read the
			// mutable alias immediately before launch. The launch target is the
			// verified canonical regular file inside that registered directory.
			const auto applicationId = QStringView(app.token).sliced(app.packageFamilyName.size() + 1);
			const auto install = packageInstall(app.packageFamilyName, applicationId);
			if (!install || install->executionAlias != app.executionAlias
			    || install->elevationTarget.isEmpty())
			{
				return process::detail::launchResultForError(ERROR_ACCESS_DENIED);
			}
			return launch(install->elevationTarget, false);
		});
	} else {
		const auto shortcut = startMenuShortcut(app, ShortcutLookupMode::Unfiltered);
		if (!shortcut.isEmpty()) result = launch(shortcut, true);
		result = process::detail::fallbackOnFailure(result, [&] {
			const auto directory = QDir::toNativeSeparators(QFileInfo(app.target).absolutePath());
			return launch(app.target, true, directory);
		});
	}

	const auto error = result.error;
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
	case AppActions::ActionResult::NotSupported: return QStringLiteral("notSupported");
	case AppActions::ActionResult::Queued: return QStringLiteral("queued");
	}
	Q_UNREACHABLE_RETURN(QString());
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
		if (auto* item = qobject_cast<QQuickItem*>(object)) {
			window = item->window();
		} else if (auto* content = qobject_cast<QQuickItem*>(
		               object->property("contentItem").value<QObject*>()
		           )) {
			window = content->window();
		}
	}

	return hwndOf(window);
}

DWORD showProperties(const QString& path, HWND owner) {
	constexpr DWORD flags = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
#ifdef QS_TEST
	if (propertiesInvokerForTest) return propertiesInvokerForTest(path, owner, flags);
#endif

	const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(com)) return ERROR_INVALID_PARAMETER;

	auto wpath = path.toStdWString();
	auto info = SHELLEXECUTEINFOW {};
	info.cbSize = sizeof(info);
	info.fMask = flags;
	info.hwnd = owner;
	info.lpVerb = L"properties";
	info.lpFile = wpath.c_str();
	info.nShow = SW_SHOWNORMAL;

	auto error = DWORD(ERROR_SUCCESS);
	if (!ShellExecuteExW(&info)) {
		error = GetLastError();
		if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
	}
	CoUninitialize();
	return error;
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
	auto shortcut = startMenuShortcut(app, ShortcutLookupMode::Unfiltered);
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

#ifdef QS_TEST
namespace test {

void setPropertiesInvokerForTest(const std::function<DWORD(const QString&, HWND, DWORD)>& invoker) {
	propertiesInvokerForTest = invoker;
}

void setShortcutRootsForTest(const QStringList& roots) {
	shortcutRootsForTest = roots;
	clearShortcutCache();
}

void setDriveTypeInvokerForTest(const DriveTypeInvoker& invoker) {
	driveTypeInvokerForTest = invoker;
}

void setFileAttributesInvokerForTest(const FileAttributesInvoker& invoker) {
	fileAttributesInvokerForTest = invoker;
}

void setKnownFolderPathInvokerForTest(const KnownFolderPathInvoker& invoker) {
	knownFolderPathInvokerForTest = invoker;
}

void setReparsePointInvokerForTest(const ReparsePointInvoker& invoker) {
	reparsePointInvokerForTest = invoker;
}

void resetPropertiesTestHooks() {
	propertiesInvokerForTest = {};
	driveTypeInvokerForTest = {};
	fileAttributesInvokerForTest = {};
	knownFolderPathInvokerForTest = {};
	reparsePointInvokerForTest = {};
	shortcutRootsForTest.reset();
	clearShortcutCache();
}

} // namespace test
#endif

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
		const auto install = packageInstall(
		    app.packageFamilyName, QStringView(token).sliced(app.packageFamilyName.size() + 1)
		);
		if (install.has_value()) {
			app.canUninstall = !isUnderWindows(install->path);
			app.allowAppsFolderElevation = install->allowElevation;
			app.executionAlias = install->executionAlias;
			app.elevationTarget = install->elevationTarget;
			app.canRunAsAdmin = app.allowAppsFolderElevation || !app.elevationTarget.isEmpty();
		} else {
			// Preserve the old failure behavior: metadata lookup failure did not
			// prove that the package is a system package.
			app.canUninstall = true;
		}
	} else {
		app.target = win32Target(id, token, &app.linkTarget);
		app.shortcutPath = QDir::toNativeSeparators(WindowsDesktopEntryBackend::shortcutPathForId(id));

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
	const auto canOpenProperties = !app.packaged && !resolvePropertiesTarget(app).isEmpty();

	return {
	    {QStringLiteral("packaged"), app.packaged},
	    {QStringLiteral("canRunAsAdmin"), app.canRunAsAdmin},
	    {QStringLiteral("canOpenLocation"), app.canOpenLocation},
	    {QStringLiteral("canOpenProperties"), canOpenProperties},
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
	auto aumid = spec.value(QStringLiteral("aumid")).toString();
	if (aumid.startsWith(QStringLiteral("shell:AppsFolder\\"), Qt::CaseInsensitive)) {
		aumid = aumid.sliced(17);
	}
	auto invokeIdList = false;
	if (!aumid.isEmpty()) {
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

	const auto directory = spec.value(QStringLiteral("workingDir")).toString();
	if (verb == QStringLiteral("open")) {
		const auto requestId = detail::nextShellActionRequestId();
		// Explorer COM and the fallback helper can both block. The watcher is
		// owned by AppActions, but the launcher task never captures AppActions.
		auto promise = std::make_shared<QPromise<DWORD>>();
		promise->start();
		auto* watcher = new QFutureWatcher<DWORD>(this);
		connect(watcher, &QFutureWatcher<DWORD>::finished, this, [this, watcher, requestId] {
			const auto error = watcher->result();
			watcher->deleteLater();
			const auto message = windowsErrorString(error);
			this->setLastError(message);
			emit this->shellActionFinished(
			    requestId, actionStatus(actionResultForError(error)), message
			);
		});
		watcher->setFuture(promise->future());
		runLauncher([target, aumid, arguments, directory, invokeIdList, promise] {
			const auto error = !aumid.isEmpty()
			    ? WindowsDesktopEntryBackend::launchSync(aumid, directory, arguments)
			    : process::shellExecuteTarget(
			          target, arguments, directory, QStringLiteral("open"), nullptr, invokeIdList
			      ).error;
			promise->addResult(error);
			promise->finish();
		});
		this->setLastError({});
		auto queued = actionResultMap(ActionResult::Queued, ERROR_SUCCESS, {});
		detail::addShellActionRequestId(queued, requestId);
		return queued;
	}

	const auto result = process::shellExecuteTarget(
	    target, arguments, directory, verb, hwndForObject(parentWindow), invokeIdList
	);
	const auto message = windowsErrorString(result.error);
	this->setLastError(message);
	return actionResultMap(actionResultForError(result.error), result.error, message);
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

bool AppActions::openProperties(const QString& entryId, QObject* parentWindow) {
	auto app = this->details(entryId);
	if (app.token.isEmpty()) {
		this->setLastError(QStringLiteral("The application entry is not available"));
		return false;
	}
	if (app.packaged) {
		this->setLastError(QStringLiteral("Properties is not supported for this application"));
		return false;
	}

	const auto target = resolvePropertiesTarget(app);
	if (target.isEmpty()) {
		this->setLastError(QStringLiteral("Properties is not supported for this application"));
		return false;
	}

	const auto error = showProperties(target, hwndForObject(parentWindow));
	this->setLastError(windowsErrorString(error));
	return error == ERROR_SUCCESS;
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
