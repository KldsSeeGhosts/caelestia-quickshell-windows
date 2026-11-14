#pragma once

#include <qstring.h>

#include <windows.h>

#include <tlhelp32.h>

namespace qs::windows {

// The application id used when a window has no AppUserModelID: the image file
// name in lower case without its ".exe" suffix.
inline QString appIdFromImageName(const QString& imageName) {
	auto appId = imageName.toLower();
	if (appId.endsWith(QStringLiteral(".exe"))) appId.chop(4);
	return appId;
}

// The image file name of a process the caller cannot open. OpenProcess refuses
// PROCESS_QUERY_LIMITED_INFORMATION for a SYSTEM process such as consent.exe,
// whose window is the deferred User Account Control request. The process
// snapshot lists every process in the session without opening it, so it still
// names the image. It returns no path, only the file name.
// https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot
inline QString processImageNameFromSnapshot(DWORD pid) {
	if (pid == 0) return {};

	auto* snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return {};

	QString name;
	PROCESSENTRY32W entry {};
	entry.dwSize = sizeof(entry);

	for (auto more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
	{
		if (entry.th32ProcessID != pid) continue;
		name = QString::fromWCharArray(entry.szExeFile);
		break;
	}

	CloseHandle(snapshot);
	return name;
}

} // namespace qs::windows
