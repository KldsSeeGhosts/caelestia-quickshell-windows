#pragma once

#include <functional>
#include <vector>

#include <qlist.h>
#include <qstring.h>
#include <qt_windows.h>

namespace qs::windows::sys {

// One TSF input profile as EnumProfiles reports it. Keyboard layouts carry an
// HKL; text services (IMEs) are identified by clsid, langid and guidProfile and
// may have no HKL and no substitute at all.
struct InputProfile {
	DWORD type = 0; // TF_PROFILETYPE_*
	LANGID langid = 0;
	GUID clsid {};
	GUID guidProfile {};
	GUID catid {};
	HKL hkl = nullptr;
	HKL hklSubstitute = nullptr;
	DWORD flags = 0; // TF_IPP_FLAG_*
	QString description;
};

// One selectable input entry. `id` is the TSF identity for a text service
// ("{clsid}:langid:{guidProfile}") and the HKL for a keyboard layout, so
// several IMEs on one language, or IMEs that share a substitute, stay
// distinct. `hkl` is the handle a WM_INPUTLANGCHANGEREQUEST can carry for
// this entry, or null when none is installed.
struct InputEntry {
	QString id;
	LANGID langid = 0;
	HKL hkl = nullptr;
	QString description;
};

// Builds the popout's entries from the enabled TSF keyboard profiles, in the
// order of `installed` (GetKeyboardLayoutList, the user's order). Text
// services without an HKL are kept and switch through their substitute, or
// else through the installed HKL of their language. Installed HKLs that no
// profile covers are appended, so a TSF failure still lists every layout.
QList<InputEntry> buildInputEntries(const QList<InputProfile>& profiles, const std::vector<HKL>& installed);

// One "Keyboard Layouts" registry entry. `layoutId` is the Layout Id value
// ("0002"); `klid` is the subkey name ("00010409").
struct LayoutRegistryEntry {
	QString klid;
	QString layoutId;
};

// Derives the KLID from an HKL without activating it, as WinForms does. A
// device word of 0xFxxx is the registry Layout Id, matched across every layout
// key regardless of language; a zero or language-equal device word means the
// KLID is the language id; any other device word is the KLID. An undocumented
// compatibility heuristic, used only when TSF has no description.
QString klidFromHkl(HKL hkl, const QList<LayoutRegistryEntry>& entries);

// The entry's TSF description first, then the read-only registry fallback.
// The reader is injected so the name path can be tested without the registry.
QString resolveLayoutName(
    const InputEntry& entry,
    const QList<LayoutRegistryEntry>& entries,
    const std::function<QString(const QString&)>& readRegistryName
);

// Reads "Layout Display Name" through SHLoadIndirectString and falls back to
// "Layout Text". Uses KEY_READ only.
QString layoutNameFromRegistry(const QString& klid);

// All read-only. COM failures return empty lists so the caller keeps working
// with locale names.
QList<InputProfile> queryInputProfiles();
QList<LayoutRegistryEntry> queryLayoutRegistryEntries();

} // namespace qs::windows::sys
