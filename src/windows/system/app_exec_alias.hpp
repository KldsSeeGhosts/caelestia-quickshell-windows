#pragma once

#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include <qbytearray.h>
#include <qdir.h>
#include <qendian.h>
#include <qfileinfo.h>
#include <qstring.h>

#include <qt_windows.h>

#include <winioctl.h>

namespace qs::windows::sys {

#ifndef IO_REPARSE_TAG_APPEXECLINK
constexpr DWORD IO_REPARSE_TAG_APPEXECLINK = 0x8000001BL;
#endif

struct AppExecAliasInfo {
	QString aliasName;
	QString packageFamilyName;
	QString aumid;
	QString targetPath;
	QString applicationType;
};

namespace detail {

inline std::optional<QString> readUtf16String(const QByteArray& data, qsizetype& offset) {
	auto utf16 = QByteArray();
	while (offset + 1 < data.size()) {
		const auto unit = qFromLittleEndian<quint16>(
		    reinterpret_cast<const uchar*>(data.constData() + offset)
		);
		offset += 2;
		if (unit == 0) {
			return utf16.isEmpty() ? QString()
			                       : QString::fromUtf16(
			                             reinterpret_cast<const char16_t*>(utf16.constData()),
			                             utf16.size() / 2
			                         );
		}
		utf16.append(static_cast<char>(unit & 0xff));
		utf16.append(static_cast<char>(unit >> 8));
	}
	return std::nullopt;
}

} // namespace detail

// Parses the payload after the REPARSE_DATA_BUFFER header. Version 3 stores a
// little-endian version followed by package family, AUMID, executable and type
// as NUL-terminated UTF-16 strings.
inline std::optional<AppExecAliasInfo> parseAppExecAliasPayload(const QByteArray& payload) {
	if (payload.size() < 4) return std::nullopt;
	const auto version = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(payload.constData()));
	if (version != 3) return std::nullopt;

	auto offset = qsizetype(4);
	auto info = AppExecAliasInfo();
	const auto family = detail::readUtf16String(payload, offset);
	const auto aumid = detail::readUtf16String(payload, offset);
	const auto target = detail::readUtf16String(payload, offset);
	const auto type = detail::readUtf16String(payload, offset);
	if (!family || !aumid || !target || !type || family->isEmpty() || aumid->isEmpty()
	    || target->isEmpty())
	{
		return std::nullopt;
	}

	info.packageFamilyName = *family;
	info.aumid = *aumid;
	info.targetPath = QDir::toNativeSeparators(*target);
	info.applicationType = *type;
	return info;
}

inline bool appExecAliasMatches(
    const AppExecAliasInfo& info,
    const QString& packageFamilyName,
    const QString& aumid
) {
	return info.packageFamilyName.compare(packageFamilyName, Qt::CaseInsensitive) == 0
	    && info.aumid.compare(aumid, Qt::CaseInsensitive) == 0;
}

inline std::optional<AppExecAliasInfo> readAppExecAlias(const QString& path) {
	const auto wide = QDir::toNativeSeparators(path).toStdWString();
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
	USHORT length = 0;
	std::memcpy(&tag, buffer.constData(), sizeof(tag));
	std::memcpy(&length, buffer.constData() + 4, sizeof(length));
	if (tag != IO_REPARSE_TAG_APPEXECLINK || static_cast<DWORD>(length) + 8 > returned) {
		return std::nullopt;
	}
	return parseAppExecAliasPayload(buffer.mid(8, length));
}

using CanonicalPathProvider = std::function<QString(const QString&)>;

struct FinalPathInfo {
	QString path;
	DWORD attributes = 0;
};

inline QString stripExtendedPathPrefix(QString path) {
	const auto uncPrefix = QStringLiteral("\\\\?\\UNC\\");
	const auto extendedPrefix = QStringLiteral("\\\\?\\");
	if (path.startsWith(uncPrefix, Qt::CaseInsensitive)) {
		return QStringLiteral("\\\\") + path.mid(uncPrefix.size());
	}
	if (path.startsWith(extendedPrefix, Qt::CaseInsensitive)) {
		return path.mid(extendedPrefix.size());
	}
	return path;
}

// Resolves junctions, symlinks and volume mount points through the object
// manager. FILE_FLAG_OPEN_REPARSE_POINT is deliberately absent.
inline std::optional<FinalPathInfo> finalPathByHandle(const QString& path) {
	const auto wide = QDir::toNativeSeparators(path).toStdWString();
	auto* handle = CreateFileW(
	    wide.c_str(),
	    0,
	    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    nullptr,
	    OPEN_EXISTING,
	    FILE_FLAG_BACKUP_SEMANTICS,
	    nullptr
	);
	if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

	auto size = DWORD(1024);
	auto buffer = std::vector<wchar_t>(size);
	auto length = DWORD(0);
	for (;;) {
		length = GetFinalPathNameByHandleW(
		    handle, buffer.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS
		);
		if (length == 0 || length < size) break;
		size = length + 1;
		buffer.assign(size, L'\0');
	}

	BY_HANDLE_FILE_INFORMATION fileInformation {};
	const auto hasInformation = GetFileInformationByHandle(handle, &fileInformation);
	CloseHandle(handle);
	if (length == 0 || !hasInformation) return std::nullopt;

	auto finalPath = QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length));
	finalPath = QDir::toNativeSeparators(stripExtendedPathPrefix(finalPath));
	return FinalPathInfo {.path = finalPath, .attributes = fileInformation.dwFileAttributes};
}

inline QString canonicalFilesystemPath(const QString& path) {
	const auto finalPath = finalPathByHandle(path);
	return finalPath ? finalPath->path : QString();
}

// Verifies that targetPath is an existing regular file strictly inside the
// registered package install directory. The resolver is injectable so tests
// can model canonicalization failures without machine-wide packages.
inline std::optional<QString> canonicalPackageTargetInsideRoot(
    const QString& targetPath,
    const QString& installRoot,
    const CanonicalPathProvider& canonicalize = {}
) {
	QString root;
	QString target;
	auto targetIsFile = false;
	if (canonicalize) {
		root = canonicalize(installRoot);
		target = canonicalize(targetPath);
		targetIsFile = QFileInfo(target).isFile();
	} else {
		const auto rootInfo = finalPathByHandle(installRoot);
		const auto targetInfo = finalPathByHandle(targetPath);
		if (rootInfo) root = rootInfo->path;
		if (targetInfo) {
			target = targetInfo->path;
			targetIsFile = (targetInfo->attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}
	}

	if (root.isEmpty() || target.isEmpty() || target == root
	    || !target.startsWith(root + u'\\', Qt::CaseInsensitive) || !targetIsFile)
	{
		return std::nullopt;
	}
	return target;
}

inline std::optional<AppExecAliasInfo> verifiedAppExecAlias(
    const QString& aliasPath,
    const QString& packageFamilyName,
    const QString& aumid,
    const QString& installRoot,
    const CanonicalPathProvider& canonicalize = {}
) {
	auto info = readAppExecAlias(aliasPath);
	if (info) info->aliasName = QFileInfo(aliasPath).fileName();
	if (!info || !appExecAliasMatches(*info, packageFamilyName, aumid)) return std::nullopt;

	const auto target = canonicalPackageTargetInsideRoot(info->targetPath, installRoot, canonicalize);
	if (!target) return std::nullopt;
	info->targetPath = *target;
	return info;
}

} // namespace qs::windows::sys
