#include "keyboard_layout_name.hpp"
#include <algorithm>
#include <iterator>

#include <msctf.h>
#include <oleauto.h>
#include <qglobal.h>
#include <qlogging.h>
#include <qset.h>
#include <qstringlist.h>
#include <quuid.h>
#include <shlwapi.h>

#include "registry_util.hpp"

namespace qs::windows::sys {

namespace {
QString hex8(WORD value) {
	return QStringLiteral("%1").arg(static_cast<uint>(value), 8, 16, QLatin1Char('0')).toUpper();
}

// Every successful CoInitializeEx, including S_FALSE for an already
// initialized apartment, must be balanced. RPC_E_CHANGED_MODE is a failure
// that leaves the existing apartment alone and is not balanced.
class ComApartment {
public:
	ComApartment(): result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
	~ComApartment() {
		if (SUCCEEDED(this->result)) CoUninitialize();
	}
	Q_DISABLE_COPY_MOVE(ComApartment);

	// COM can be used on this thread, in this or the existing apartment.
	[[nodiscard]] bool usable() const {
		return SUCCEEDED(this->result) || this->result == RPC_E_CHANGED_MODE;
	}

private:
	HRESULT result;
};

} // namespace

// HKL decoding is an undocumented compatibility heuristic. The value is
// opaque to applications, so this is used only as a fallback when TSF gives no
// description for the layout. It follows the resolution in WinForms'
// InputLanguage (dotnet/winforms, InputLanguage.cs):
//  - a device word of 0xFxxx carries the registry "Layout Id" in its low 12
//    bits; the matching key is found among all layout keys, whatever its
//    language suffix, because a keyboard can be attached to another language;
//  - a zero or equal device word means the KLID is the language id;
//  - otherwise the KLID is the device word.
QString klidFromHkl(HKL hkl, const QList<LayoutRegistryEntry>& entries) {
	auto value = static_cast<DWORD>(reinterpret_cast<quintptr>(hkl)); // NOLINT
	WORD languageId = LOWORD(value);
	WORD deviceWord = HIWORD(value);

	if ((deviceWord & 0xF000) == 0xF000) {
		auto wanted = static_cast<uint>(deviceWord & 0x0FFF);
		for (const auto& entry: entries) {
			bool ok = false;
			if (entry.layoutId.toUInt(&ok, 16) != wanted || !ok) continue;
			return entry.klid;
		}

		return QString();
	}

	if (deviceWord == 0 || deviceWord == languageId) return hex8(languageId);
	return hex8(deviceWord);
}

namespace {
bool isInstalled(const std::vector<HKL>& installed, HKL hkl) {
	return hkl != nullptr && std::find(installed.begin(), installed.end(), hkl) != installed.end();
}

// The handle a cross-thread switch can carry for a text service. A TIP has no
// HKL of its own; its substitute is preferred, then the installed layout of
// its language, which makes Windows select that language's active profile.
HKL switchHandleFor(const InputProfile& profile, const std::vector<HKL>& installed) {
	if (profile.hkl != nullptr) return profile.hkl;
	if (isInstalled(installed, profile.hklSubstitute)) return profile.hklSubstitute;
	for (auto* hkl: installed) {
		if (LOWORD(reinterpret_cast<quintptr>(hkl)) == profile.langid) return hkl; // NOLINT
	}
	return nullptr;
}

QString hklId(HKL hkl) {
	return QString::number(reinterpret_cast<quintptr>(hkl), 16); // NOLINT
}

QString guidId(const GUID& guid) { return QUuid(guid).toString(QUuid::WithBraces).toUpper(); }
} // namespace

QList<InputEntry> buildInputEntries(const QList<InputProfile>& profiles, const std::vector<HKL>& installed) {
	QList<InputEntry> entries;
	QSet<QString> seen;
	QSet<HKL> replaced;

	for (const auto& profile: profiles) {
		if ((profile.flags & TF_IPP_FLAG_ENABLED) == 0) continue;

		InputEntry entry;
		entry.langid = profile.langid;
		entry.description = profile.description;

		if (profile.type == TF_PROFILETYPE_KEYBOARDLAYOUT) {
			// A layout an IME replaces is shown through that IME instead, as
			// Windows' own switcher does. Track it even when the IME's switch
			// handle is a different same-language HKL, so fallback cannot
			// resurrect the replaced layout as a bare entry.
			if ((profile.flags & TF_IPP_FLAG_SUBSTITUTEDBYINPUTPROCESSOR) != 0) {
				if (profile.hkl != nullptr) replaced.insert(profile.hkl);
				continue;
			}
			if (!isInstalled(installed, profile.hkl)) continue;
			entry.id = hklId(profile.hkl);
			entry.hkl = profile.hkl;
		} else if (profile.type == TF_PROFILETYPE_INPUTPROCESSOR) {
			if (!IsEqualGUID(profile.catid, GUID_TFCAT_TIP_KEYBOARD)) continue;
			entry.id = QStringLiteral("%1:%2:%3")
			               .arg(guidId(profile.clsid))
			               .arg(static_cast<uint>(profile.langid), 4, 16, QLatin1Char('0'))
			               .arg(guidId(profile.guidProfile));
			entry.hkl = switchHandleFor(profile, installed);
		} else {
			continue;
		}

		if (seen.contains(entry.id)) continue;
		seen.insert(entry.id);
		entries.append(entry);
	}

	for (auto* hkl: installed) {
		if (replaced.contains(hkl)) continue;
		bool covered = std::any_of(entries.begin(), entries.end(), [hkl](const InputEntry& entry) {
			return entry.hkl == hkl;
		});
		if (covered) continue;
		entries.append({hklId(hkl), LOWORD(reinterpret_cast<quintptr>(hkl)), hkl, QString()}); // NOLINT
	}

	// The user's order, by the position of each entry's handle; entries without
	// one go last. The stable sort keeps TSF's order within a language.
	auto rank = [&installed](const InputEntry& entry) {
		auto it = std::find(installed.begin(), installed.end(), entry.hkl);
		return entry.hkl != nullptr ? std::distance(installed.begin(), it)
		                            : static_cast<std::ptrdiff_t>(installed.size());
	};
	std::stable_sort(entries.begin(), entries.end(), [&rank](const InputEntry& a, const InputEntry& b) {
		return rank(a) < rank(b);
	});

	return entries;
}

QString resolveLayoutName(
    const InputEntry& entry,
    const QList<LayoutRegistryEntry>& entries,
    const std::function<QString(const QString&)>& readRegistryName
) {
	if (!entry.description.isEmpty()) return entry.description;
	if (entry.hkl == nullptr) return QString();

	auto klid = klidFromHkl(entry.hkl, entries);
	if (klid.isEmpty()) return QString();
	return readRegistryName(klid);
}

QString layoutNameFromRegistry(const QString& klid) {
	if (klid.isEmpty()) return QString();

	auto subkey = QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\") + klid;
	auto subkeyName = subkey.toStdWString();
	auto indirect =
	    readRegistryString(HKEY_LOCAL_MACHINE, subkeyName.c_str(), L"Layout Display Name");
	if (!indirect.isEmpty()) {
		wchar_t buffer[512] {};
		auto source = indirect.toStdWString();
		if (SHLoadIndirectString(source.c_str(), buffer, 512, nullptr) == S_OK) {
			auto text = QString::fromWCharArray(buffer).trimmed();
			if (!text.isEmpty()) return text;
		}
		if (!indirect.startsWith(QChar(u'@'))) return indirect.trimmed();
	}

	return readRegistryString(HKEY_LOCAL_MACHINE, subkeyName.c_str(), L"Layout Text").trimmed();
}

QList<InputProfile> queryInputProfiles() {
	QList<InputProfile> result;

	// The shell thread may already be in an STA or MTA; the guard balances
	// every successful initialization.
	ComApartment apartment;
	if (!apartment.usable()) return result;

	ITfInputProcessorProfiles* profiles = nullptr;
	ITfInputProcessorProfileMgr* manager = nullptr;
	IEnumTfInputProcessorProfiles* enumerator = nullptr;

	do {
		if (FAILED(CoCreateInstance(
		        CLSID_TF_InputProcessorProfiles,
		        nullptr,
		        CLSCTX_INPROC_SERVER,
		        IID_ITfInputProcessorProfiles,
		        reinterpret_cast<void**>(&profiles) // NOLINT
		    )))
		{
			break;
		}

		if (FAILED(profiles->QueryInterface(
		        IID_ITfInputProcessorProfileMgr,
		        reinterpret_cast<void**>(&manager) // NOLINT
		    )))
		{
			break;
		}

		if (FAILED(manager->EnumProfiles(0, &enumerator))) break;

		TF_INPUTPROCESSORPROFILE profile {};
		ULONG fetched = 0;
		while (enumerator->Next(1, &profile, &fetched) == S_OK && fetched == 1) {
			// Every profile is kept with its full identity; buildInputEntries
			// decides what is selectable, so it can be tested without TSF.
			InputProfile entry;
			entry.type = profile.dwProfileType;
			entry.langid = profile.langid;
			entry.clsid = profile.clsid;
			entry.guidProfile = profile.guidProfile;
			entry.catid = profile.catid;
			entry.hkl = profile.hkl;
			entry.hklSubstitute = profile.hklSubstitute;
			entry.flags = profile.dwFlags;

			BSTR description = nullptr;
			auto hr = profiles->GetLanguageProfileDescription(
			    profile.clsid,
			    profile.langid,
			    profile.guidProfile,
			    &description
			);
			if (SUCCEEDED(hr) && description != nullptr) {
				entry.description = QString::fromWCharArray(description, SysStringLen(description));
			}
			result.append(entry);
			if (description != nullptr) SysFreeString(description);
		}
	} while (false);

	if (enumerator != nullptr) enumerator->Release();
	if (manager != nullptr) manager->Release();
	if (profiles != nullptr) profiles->Release();

	return result;
}

QList<LayoutRegistryEntry> queryLayoutRegistryEntries() {
	QList<LayoutRegistryEntry> result;

	HKEY key = nullptr;
	if (RegOpenKeyExW(
	        HKEY_LOCAL_MACHINE,
	        L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts",
	        0,
	        KEY_READ,
	        &key
	    )
	    != ERROR_SUCCESS)
	{
		return result;
	}

	wchar_t name[256] {};
	DWORD index = 0;
	while (true) {
		DWORD length = 256;
		auto status = RegEnumKeyExW(key, index++, name, &length, nullptr, nullptr, nullptr, nullptr);
		if (status == ERROR_NO_MORE_ITEMS) break;
		if (status != ERROR_SUCCESS) {
			qWarning() << "Could not enumerate keyboard layout registry entries:" << status;
			break;
		}

		auto subkey = QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\")
		            + QString::fromWCharArray(name, length);
		auto subkeyName = subkey.toStdWString();
		auto layoutId = readRegistryString(HKEY_LOCAL_MACHINE, subkeyName.c_str(), L"Layout Id");
		if (!layoutId.isEmpty()) {
			result.append({QString::fromWCharArray(name, length), layoutId});
		}
	}

	RegCloseKey(key);
	return result;
}

} // namespace qs::windows::sys
