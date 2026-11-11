#pragma once

#include <qt_windows.h>

namespace qs::windows {

// IsHungAppWindow alone does not notice a newly stalled message pump until
// Windows' hung-window threshold expires. Probe with a strict timeout as well.
// This is a preflight only: it cannot cancel a later SetParent/Qt native call.
inline bool windowResponding(HWND hwnd) {
	if (!hwnd || !IsWindow(hwnd) || IsHungAppWindow(hwnd)) return false;
	if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) return true;
	DWORD_PTR ignored = 0;
	return SendMessageTimeoutW(
	           hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 50, &ignored
	       ) != 0;
}

} // namespace qs::windows
