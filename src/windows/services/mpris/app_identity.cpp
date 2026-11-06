#include "app_identity.hpp"

#include <qdir.h>
#include <qfileinfo.h>
#include <qstring.h>

#include <qt_windows.h>

#include <objbase.h>
#include <propkey.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/base.h>

using Microsoft::WRL::ComPtr;

namespace qs::windows::services::mpris {

namespace {

constexpr auto APPS_FOLDER_PREFIX = "shell:AppsFolder\\";

QString takeComString(PWSTR raw) {
	if (raw == nullptr) return {};
	auto value = QString::fromWCharArray(raw);
	CoTaskMemFree(raw);
	return value;
}

QString toQString(const winrt::hstring& value) {
	return QString::fromWCharArray(value.c_str(), static_cast<qsizetype>(value.size()));
}

ComPtr<IShellItem> shellItem(const QString& parsingName) {
	auto native = QDir::toNativeSeparators(parsingName).toStdWString();

	ComPtr<IShellItem> item;
	if (FAILED(SHCreateItemFromParsingName(native.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
		return {};
	}

	return item;
}

QString displayName(const ComPtr<IShellItem>& item) {
	PWSTR raw = nullptr;
	if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw))) return {};
	return takeComString(raw).trimmed();
}

QString fileDescription(const QString& path) {
	auto item = shellItem(path);
	if (!item) return {};

	ComPtr<IShellItem2> item2;
	if (FAILED(item.As(&item2))) return {};

	PWSTR raw = nullptr;
	if (FAILED(item2->GetString(PKEY_FileDescription, &raw))) return {};
	return takeComString(raw).trimmed();
}

bool isExecutable(const QString& value) {
	if (!value.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) return false;

	auto info = QFileInfo(QDir::fromNativeSeparators(value));
	return info.isAbsolute() && info.isFile();
}

AppIdentity resolve(const QString& aumid, const AppIdentityHint& hint) {
	// Packaged apps carry a localized display name in the package manifest.
	// The AppsFolder item for the same AUMID serves the package logo as its
	// shell icon, and the desktop entry scan keys that item by the lowercased
	// AUMID, so returning that id reuses the existing icon path.
	try {
		auto info = winrt::Windows::ApplicationModel::AppInfo::GetFromAppUserModelId(
		    winrt::hstring(aumid.toStdWString())
		);

		auto display = info.DisplayInfo();
		if (display) {
			auto name = toQString(display.DisplayName()).trimmed();
			if (!name.isEmpty()) return {.name = name, .desktopEntry = aumid.toLower()};
		}
	} catch (const winrt::hresult_error&) {
		// Not a packaged app id. Desktop apps reject this lookup.
	}

	// Desktop apps expose the same AppsFolder item the desktop entry scan
	// reads. Its token is the lowercased AUMID, the id that
	// DesktopEntries.heuristicLookup and the running-app pill already resolve
	// to a shell icon.
	if (auto item = shellItem(QString::fromLatin1(APPS_FOLDER_PREFIX) + aumid)) {
		auto name = displayName(item);
		if (!name.isEmpty()) return {.name = name, .desktopEntry = aumid.toLower()};
	}

	// A runtime AUMID need not be the AppsFolder token. The caller resolves the
	// same window app id through the desktop entry index, which is also what
	// the running-app pill uses, and passes the matching entry here.
	if (!hint.desktopEntry.isEmpty()) {
		if (auto item = hint.parsingName.isEmpty()
		            ? ComPtr<IShellItem> {}
		            : shellItem(QString::fromLatin1(APPS_FOLDER_PREFIX) + hint.parsingName))
		{
			auto name = displayName(item);
			if (!name.isEmpty()) return {.name = name, .desktopEntry = hint.desktopEntry};
		}

		auto name = fileDescription(hint.executablePath);
		if (name.isEmpty() && !hint.executablePath.isEmpty()) {
			name = QFileInfo(QDir::fromNativeSeparators(hint.executablePath)).completeBaseName();
		}
		if (!name.isEmpty()) return {.name = name, .desktopEntry = hint.desktopEntry};
	}

	// An executable outside the AppsFolder still carries a FileDescription in
	// its version resources. The icon provider also renders an executable path
	// directly, so no copy of the icon is needed here.
	if (isExecutable(aumid)) {
		auto name = fileDescription(aumid);
		if (name.isEmpty()) name = QFileInfo(QDir::fromNativeSeparators(aumid)).completeBaseName();
		return {.name = name, .desktopEntry = QDir::toNativeSeparators(aumid)};
	}

	// No dynamic metadata is available. Keep the raw id so the consumer can
	// show it instead of inventing a name.
	return {.name = aumid, .desktopEntry = aumid};
}

} // namespace

AppIdentity resolveAppIdentity(const QString& aumid, const AppIdentityHint& hint) {
	auto id = aumid.trimmed();
	if (id.isEmpty()) return {};
	return resolve(id, hint);
}

} // namespace qs::windows::services::mpris
