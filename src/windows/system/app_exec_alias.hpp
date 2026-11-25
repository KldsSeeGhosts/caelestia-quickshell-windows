#pragma once

#include <cstring>
#include <optional>

#include <qbytearray.h>
#include <qdir.h>
#include <qendian.h>
#include <qfileinfo.h>
#include <qstring.h>
#include <qtenvironmentvariables.h>

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

inline bool isWindowsAppsPackageTarget(const QString& targetPath, const QString& packageFamilyName) {
	const auto programFiles = QDir::toNativeSeparators(qEnvironmentVariable("ProgramFiles"));
	if (programFiles.isEmpty()) return false;

	const auto targetInfo = QFileInfo(targetPath);
	const auto target = QDir::toNativeSeparators(targetInfo.canonicalFilePath());
	if (target.isEmpty()) return false;
	const auto root = QDir::toNativeSeparators(programFiles + QStringLiteral("\\WindowsApps"));
	if (!target.startsWith(root + u'\\', Qt::CaseInsensitive)) return false;

	const auto underscore = packageFamilyName.lastIndexOf(u'_');
	if (underscore <= 0 || underscore + 1 >= packageFamilyName.size()) return false;
	const auto packageName = packageFamilyName.left(underscore);
	const auto publisher = packageFamilyName.mid(underscore + 1);
	const auto rest = QStringView(target).sliced(root.size() + 1);
	const auto folder = rest.first(rest.indexOf(u'\\') < 0 ? rest.size() : rest.indexOf(u'\\'));
	return folder.startsWith(packageName + u'_', Qt::CaseInsensitive)
	    && folder.endsWith(QStringLiteral("__") + publisher, Qt::CaseInsensitive)
	    && targetInfo.isFile();
}

inline std::optional<AppExecAliasInfo> verifiedAppExecAlias(
    const QString& aliasPath,
    const QString& packageFamilyName,
    const QString& aumid
) {
	const auto info = readAppExecAlias(aliasPath);
	if (!info || !appExecAliasMatches(*info, packageFamilyName, aumid)
	    || !isWindowsAppsPackageTarget(info->targetPath, packageFamilyName))
	{
		return std::nullopt;
	}
	return info;
}

} // namespace qs::windows::sys
