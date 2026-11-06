#include "util.hpp"

#include <atomic>
#include <cmath>

#include <qbytearray.h>
#include <qguiapplication.h>
#include <qpointer.h>
#include <qpoint.h>
#include <qrect.h>
#include <qscreen.h>
#include <qscreen_platform.h>
#include <qthreadpool.h>
#include <qtypes.h>
#include <qwindow.h>

#include <dwmapi.h>
#include <objbase.h>

#include "activation_policy.hpp"
#include "focus_monitor.hpp"

namespace qs::windows {

HMONITOR monitorForScreen(QScreen* screen) {
	if (screen == nullptr) return nullptr;
	auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
	return native == nullptr ? nullptr : native->handle();
}

QScreen* screenForMonitor(HMONITOR monitor) {
	if (monitor == nullptr) return nullptr;

	for (auto* screen: QGuiApplication::screens()) {
		if (monitorForScreen(screen) == monitor) return screen;
	}

	return nullptr;
}

namespace {
FocusMonitor gFocusedMonitor; // NOLINT
}

QScreen* focusedScreen() { return gFocusedMonitor.screen(); }

bool focusActiveWindow(QScreen* screen) { return gFocusedMonitor.activeWindow(screen); }

bool focusCursorMonitor(QScreen* screen, bool focusFollowsMouse) {
	return gFocusedMonitor.cursorEntered(screen, focusFollowsMouse);
}

bool focusClickedMonitor(QScreen* screen) { return gFocusedMonitor.clicked(screen); }

bool focusFollowsMouseEnabled() {
	// The shell applies windows.focusFollowsMouse to this Windows X-Mouse
	// value, so the live setting is what actually moves focus on hover.
	BOOL tracking = FALSE;
	return SystemParametersInfoW(SPI_GETACTIVEWINDOWTRACKING, 0, &tracking, 0) != 0
	    && tracking != FALSE;
}

MonitorRects monitorRects(HMONITOR monitor) {
	MonitorRects rects;
	if (monitor == nullptr) return rects;

	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info)) return rects;

	rects.monitor = toQRect(info.rcMonitor);
	rects.work = toQRect(info.rcWork);
	rects.valid = true;
	return rects;
}

QPoint ScreenMapper::toLogical(const QPoint& physical) const {
	auto delta = physical - this->physicalOrigin;
	return this->logicalOrigin
	     + QPoint(
	           static_cast<int>(std::lround(delta.x() / this->dpr)),
	           static_cast<int>(std::lround(delta.y() / this->dpr))
	       );
}

QRect ScreenMapper::toLogical(const QRect& physical) const {
	auto topLeft = this->toLogical(physical.topLeft());
	auto bottomRight = this->toLogical(physical.topLeft() + QPoint(physical.width(), physical.height()));
	return QRect(topLeft, QSize(bottomRight.x() - topLeft.x(), bottomRight.y() - topLeft.y()));
}

QPoint ScreenMapper::toPhysical(const QPoint& logical) const {
	auto delta = logical - this->logicalOrigin;
	return this->physicalOrigin
	     + QPoint(
	           static_cast<int>(std::lround(delta.x() * this->dpr)),
	           static_cast<int>(std::lround(delta.y() * this->dpr))
	       );
}

QRect ScreenMapper::toPhysical(const QRect& logical) const {
	auto topLeft = this->toPhysical(logical.topLeft());
	auto bottomRight = this->toPhysical(logical.topLeft() + QPoint(logical.width(), logical.height()));
	return QRect(topLeft, QSize(bottomRight.x() - topLeft.x(), bottomRight.y() - topLeft.y()));
}

qint32 ScreenMapper::toPhysical(qint32 logicalLength) const {
	return static_cast<qint32>(std::lround(logicalLength * this->dpr));
}

HWND hwndOf(const QWindow* window) {
	// The offscreen QPA has no native window, but QPlatformWindow::winId() can
	// still be a bookkeeping value that is not an HWND. Treat it as windowless
	// so the native panel/desktop paths stay off and smoke gates do not drive
	// Win32 with a pseudo handle.
	if (QGuiApplication::platformName() == "offscreen") return nullptr;
	if (window == nullptr || window->handle() == nullptr) return nullptr;
	return reinterpret_cast<HWND>(window->winId()); // NOLINT(performance-no-int-to-ptr)
}

QRect toQRect(const RECT& rect) {
	return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

RECT toRECT(const QRect& rect) {
	return RECT {
	    .left = rect.left(),
	    .top = rect.top(),
	    .right = rect.left() + rect.width(),
	    .bottom = rect.top() + rect.height(),
	};
}

DWORD windowsBuild() {
	static const DWORD build = []() -> DWORD {
		using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
		auto* ntdll = GetModuleHandleW(L"ntdll.dll");
		auto rtlGetVersion = ntdll == nullptr
		                       ? nullptr
		                       : reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
		if (rtlGetVersion == nullptr) return 0;

		OSVERSIONINFOW info {};
		info.dwOSVersionInfoSize = sizeof(info);
		return rtlGetVersion(&info) == 0 ? info.dwBuildNumber : 0;
	}();

	return build;
}

void runLauncher(std::function<void()> task) {
	static auto* pool = [] {
		auto* pool = new QThreadPool();
		pool->setMaxThreadCount(2);
		pool->setExpiryTimeout(30000);
		return pool;
	}();

	pool->start([task = std::move(task)]() {
		auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		task();
		if (SUCCEEDED(com)) CoUninitialize();
	});
}

namespace {

// Accepted Windows limitation, kept from the old forceForegroundWindow(): a
// key swallowed by the WH_KEYBOARD_LL hook was never delivered to this process,
// so Windows does not treat the shell as having received the last input event
// and SetForegroundWindow refuses. Lone-modifier taps can only use that hook
// path, so the default lone-Super launcher would otherwise open unfocused and
// type into the application behind it. The maintainer accepted this one
// escalation for hook-delivered activation only; hotkey, click and IPC
// activation never reach it. It runs once, with no retry, timer or synthetic
// input, and the accepted limitation is tracked in
// the shared architecture notes
bool forceForegroundFromHook(HWND hwnd) {
	auto* foreground = GetForegroundWindow();
	auto foregroundThread =
	    foreground == nullptr ? 0 : GetWindowThreadProcessId(foreground, nullptr);
	auto ownThread = GetCurrentThreadId();

	if (foregroundThread == 0 || foregroundThread == ownThread) return false;
	if (IsHungAppWindow(foreground)) return false;

	if (!AttachThreadInput(ownThread, foregroundThread, TRUE)) return false;
	auto ok = SetForegroundWindow(hwnd) != FALSE;
	AttachThreadInput(ownThread, foregroundThread, FALSE);

	if (ok) SetFocus(hwnd);
	return ok;
}

} // namespace

bool requestForegroundWindow(HWND hwnd, ActivationSource source) {
	if (hwnd == nullptr) return false;

	auto* foreground = GetForegroundWindow();
	if (foreground == hwnd) return true;

	DWORD targetPid = 0;
	GetWindowThreadProcessId(hwnd, &targetPid);

	// A handoff to another process is the one case where the shell must grant
	// the right before asking. When the shell does not own the foreground, its
	// right comes from an input event it received and no grant is valid.
	if (foregroundHandoff(isOwnProcessWindow(foreground), targetPid, GetCurrentProcessId())
	    == ForegroundHandoff::AllowTarget)
	{
		AllowSetForegroundWindow(targetPid);
	}

	// One request. A refusal means the foreground lock did not recognize an
	// input event for the shell; report it instead of forcing or retrying.
	if (SetForegroundWindow(hwnd)) return true;

	// Only a swallowed hook key has no documented permission. The escalation
	// is still a single call, never a retry loop.
	return mayForceForeground(source) && forceForegroundFromHook(hwnd);
}

ActivationSource activationSourceForCurrentState() {
	if (auto source = currentActivationSource()) return *source;
	return isOwnProcessWindow(GetForegroundWindow()) ? ActivationSource::Click
	                                                 : ActivationSource::Ipc;
}

bool isOwnProcessWindow(HWND hwnd) {
	if (hwnd == nullptr) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId();
}

namespace {

DWORD tokenIntegrity(HANDLE token) {
	DWORD size = 0;
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
	if (size == 0) return 0;

	auto buffer = QByteArray(static_cast<qsizetype>(size), '\0');
	auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
	if (!GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) return 0;

	auto* sid = label->Label.Sid;
	auto count = *GetSidSubAuthorityCount(sid);
	if (count == 0) return 0;
	return *GetSidSubAuthority(sid, count - 1);
}

DWORD ownIntegrity() {
	static const DWORD level = []() -> DWORD {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return SECURITY_MANDATORY_MEDIUM_RID;
		}
		auto level = tokenIntegrity(token);
		CloseHandle(token);
		return level == 0 ? SECURITY_MANDATORY_MEDIUM_RID : level;
	}();

	return level;
}

} // namespace

bool isMoreElevated(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	auto own = ownIntegrity();

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return own < SECURITY_MANDATORY_HIGH_RID;

	HANDLE token = nullptr;
	auto opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
	CloseHandle(process);
	if (!opened) return own < SECURITY_MANDATORY_HIGH_RID;

	auto level = tokenIntegrity(token);
	CloseHandle(token);
	return level > own;
}

bool isTrayHookWindow(HWND hwnd) {
	return hwnd != nullptr && GetPropW(hwnd, TRAY_HOOK_PROP) != nullptr;
}

namespace {
std::atomic<int> gTrayHookYields = 0; // NOLINT
} // namespace

bool trayHookYielding() { return gTrayHookYields.load() > 0; }

UINT trayHookRepairMessage() {
	static const UINT message = RegisterWindowMessageW(L"QuickshellTrayHookRepair"); // NOLINT
	return message;
}

void requestTrayHookRepair() {
	// The ask comes after a yield, so our window can be below Explorer's: walk
	// the class instead of taking the first one.
	HWND hwnd = nullptr;
	while ((hwnd = FindWindowExW(nullptr, hwnd, L"Shell_TrayWnd", nullptr)) != nullptr) {
		if (!isTrayHookWindow(hwnd)) continue;
		PostMessageW(hwnd, trayHookRepairMessage(), 0, 0);
		return;
	}
}

TrayHookYield::TrayHookYield() {
	if (gTrayHookYields.fetch_add(1) != 0) return;

	auto* first = FindWindowW(L"Shell_TrayWnd", nullptr);
	if (!isTrayHookWindow(first)) return;

	auto* explorer = explorerTaskbarWindow();
	if (explorer == nullptr) return;

	SetWindowPos(first, explorer, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

TrayHookYield::~TrayHookYield() {
	if (gTrayHookYields.fetch_sub(1) != 1) return;

	requestTrayHookRepair();
}

HWND explorerTaskbarWindow() {
	HWND hwnd = nullptr;
	while ((hwnd = FindWindowExW(nullptr, hwnd, L"Shell_TrayWnd", nullptr)) != nullptr) {
		if (!isTrayHookWindow(hwnd)) return hwnd;
	}
	return nullptr;
}

void applyPanelDwmAttributes(HWND hwnd) {
	if (hwnd == nullptr) return;

	auto corner = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_DONOTROUND);
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

	COLORREF border = DWMWA_COLOR_NONE;
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));

	BOOL enabled = TRUE;
	DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enabled, sizeof(enabled));
	DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &enabled, sizeof(enabled));

	auto policy = static_cast<DWMNCRENDERINGPOLICY>(DWMNCRP_DISABLED);
	DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));

	markNonRude(hwnd);
}

void markNonRude(HWND hwnd) {
	SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE)); // NOLINT
}

void setExStyleBits(HWND hwnd, LONG_PTR bits, bool enabled) {
	if (hwnd == nullptr) return;
	auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	auto newStyle = enabled ? (style | bits) : (style & ~bits);
	if (newStyle != style) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newStyle);
}

void prioritizeInputThread(int priority) {
	SetThreadPriority(GetCurrentThread(), priority);

	THREAD_POWER_THROTTLING_STATE state {};
	state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask = 0;
	SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
}

void optOutOfProcessPowerThrottling() {
	PROCESS_POWER_THROTTLING_STATE state {};
	state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

} // namespace qs::windows
