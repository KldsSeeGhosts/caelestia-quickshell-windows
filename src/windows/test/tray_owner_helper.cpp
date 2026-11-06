// Owns a real top-level window for tray_host_test, prints its handle, then
// waits to be killed. It disappears without sending NIM_DELETE, like an app
// that crashes or is force-closed while its tray icon is still registered.

#include <cstdio>

#include <qt_windows.h>

namespace {

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_CLOSE) {
		DestroyWindow(hwnd);
		return 0;
	}

	return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int main() {
	auto* instance = GetModuleHandleW(nullptr);

	WNDCLASSW windowClass {};
	windowClass.lpfnWndProc = windowProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = L"QsTrayOwnerFixture";
	if (RegisterClassW(&windowClass) == 0) return 1;

	auto hwnd = CreateWindowExW(
	    0,
	    windowClass.lpszClassName,
	    L"tray owner fixture",
	    WS_OVERLAPPEDWINDOW,
	    0,
	    0,
	    64,
	    64,
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
	if (hwnd == nullptr) return 1;

	std::printf("%llu\n", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(hwnd)));
	std::fflush(stdout);

	// The parent kills us; EOF also ends the process.
	char byte = 0;
	(void) std::fread(&byte, 1, 1, stdin);
	return 0;
}
