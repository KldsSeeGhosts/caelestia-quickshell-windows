#pragma once

#include <qdir.h>
#include <qstring.h>
#include <qt_windows.h>
#include <shlobj.h>

namespace qs::windows {

// Resolve a user folder through the shell instead of assuming it lives under
// the profile. OneDrive folder backup and Folder Properties both redirect these
// folders, so appending a name to USERPROFILE misses the real location.
// KF_FLAG_DONT_VERIFY keeps an offline redirected folder resolvable.
inline QString knownFolderPath(REFKNOWNFOLDERID id) {
	// The caller must free the shell buffer, including when the call fails.
	struct CoTaskMemBuffer {
		PWSTR value = nullptr;
		~CoTaskMemBuffer() { CoTaskMemFree(value); }
	} buffer;

	if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &buffer.value))
	    || buffer.value == nullptr)
	{
		return {};
	}

	return QDir::fromNativeSeparators(QString::fromWCharArray(buffer.value));
}

} // namespace qs::windows
