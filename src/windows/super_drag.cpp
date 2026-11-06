#include "super_drag.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <thread>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpoint.h>
#include <qpointer.h>

#include "input_mask.hpp"
#include "keyboard_hook.hpp"
#include "super_drag_wait.hpp"
#include "tiling.hpp"
#include "util.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logSuperDrag, "quickshell.windows.superdrag", QtWarningMsg);

constexpr auto MESSAGE_WINDOW_CLASS = L"QuickshellSuperDrag";

constexpr UINT WM_SD_BEGIN_MOVE = WM_APP + 1;
constexpr UINT WM_SD_BEGIN_RESIZE = WM_APP + 2;
constexpr UINT WM_SD_MOVE = WM_APP + 3;
constexpr UINT WM_SD_END = WM_APP + 4;
constexpr UINT WM_SD_TARGET_EVENT = WM_APP + 5;
constexpr UINT WM_SD_PLACED = WM_APP + 6;
constexpr UINT WM_SD_CANCEL = WM_APP + 7;

constexpr UINT WM_SD_STARTED = WM_APP + 1;
constexpr UINT WM_SD_TILED_RESIZE = WM_APP + 2;
constexpr UINT WM_SD_ENDED = WM_APP + 3;
constexpr UINT WM_SD_CANCELLED = WM_APP + 4;

constexpr LPARAM FLAG_RESIZE = 1;
constexpr LPARAM FLAG_LEFT = 2;
constexpr LPARAM FLAG_TOP = 4;

constexpr LRESULT STARTED_TILED_RESIZE = 1;
constexpr LRESULT STARTED_WATCHED = 2;

constexpr UINT GUI_TIMEOUT_MS = 250;
// Only a bound on how long an unconfirmed restore or unanswered placement is
// waited for. It reports failure; the target's own state and the placement's
// answer decide completion.
constexpr DWORD SETTLE_TIMEOUT_MS = 3000;
constexpr DWORD ELEVATION_CACHE_MS = 2000;

constexpr UINT POS_FLAGS = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;

static_assert(sizeof(LPARAM) >= sizeof(uint64_t));

std::atomic<bool> gEnabled = false;      // NOLINT
std::atomic<DWORD> gWorkerId = 0;        // NOLINT
std::atomic<bool> gWorkerStop = false;   // NOLINT
std::atomic<HWND> gGuiWindow = nullptr;  // NOLINT
std::atomic<uint64_t> gCursor = 0;       // NOLINT
std::atomic<bool> gMovePending = false;  // NOLINT
std::atomic<uint64_t> gTiledDelta = 0;       // NOLINT
std::atomic<bool> gTiledPending = false;     // NOLINT
std::atomic<bool> gTargetEventPending = false; // NOLINT
// A drag's button is down (mouse hook thread), and Esc cancelled it
// (keyboard hook thread); the button's release then ends nothing.
std::atomic<bool> gDragLive = false;  // NOLINT
std::atomic<bool> gCancelled = false; // NOLINT

struct HookState {
	bool active = false;
	bool resize = false;
	DWORD elevationPid = 0;
	DWORD elevationTime = 0;
	bool elevated = false;
};

HookState gHook; // NOLINT

uint64_t pack(LONG x, LONG y) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
}

uint64_t pack(POINT point) { return pack(point.x, point.y); }

POINT unpack(uint64_t value) {
	return POINT {
	    .x = static_cast<LONG>(static_cast<int32_t>(static_cast<uint32_t>(value >> 32))),
	    .y = static_cast<LONG>(static_cast<int32_t>(static_cast<uint32_t>(value))),
	};
}

LONG widthOf(const RECT& rect) { return rect.right - rect.left; }
LONG heightOf(const RECT& rect) { return rect.bottom - rect.top; }

bool isDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool superAlone() {
	if (!hotkeys::KeyboardHook::superDown()) return false;
	return !isDown(VK_CONTROL) && !isDown(VK_MENU) && !isDown(VK_SHIFT);
}

bool isShellWindow(HWND hwnd) {
	static constexpr std::array<const wchar_t*, 13> CLASSES {
	    L"Progman",
	    L"WorkerW",
	    L"Shell_TrayWnd",
	    L"Shell_SecondaryTrayWnd",
	    L"NotifyIconOverflowWindow",
	    L"TopLevelWindowForOverflowXamlIsland",
	    L"Windows.UI.Core.CoreWindow",
	    L"XamlExplorerHostIslandWindow",
	    L"Windows.Internal.Shell.TabProxyWindow",
	    L"MultitaskingViewFrame",
	    L"TaskListThumbnailWnd",
	    L"Shell_InputSwitchTopLevelWindow",
	    L"#32768",
	};

	std::array<wchar_t, 128> name {};
	if (GetClassNameW(hwnd, name.data(), static_cast<int>(name.size())) == 0) return true;

	return std::ranges::any_of(CLASSES, [&name](const wchar_t* cls) {
		return std::wcscmp(name.data(), cls) == 0;
	});
}

bool coversMonitor(HWND hwnd) {
	RECT rect {};
	if (!GetWindowRect(hwnd, &rect)) return false;

	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return false;

	const auto& monitor = info.rcMonitor;
	return rect.left <= monitor.left && rect.top <= monitor.top && rect.right >= monitor.right
	    && rect.bottom >= monitor.bottom;
}

bool isElevatedCached(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	auto now = GetTickCount();

	if (pid != 0 && pid == gHook.elevationPid && now - gHook.elevationTime < ELEVATION_CACHE_MS) {
		return gHook.elevated;
	}

	gHook.elevated = isMoreElevated(hwnd);
	gHook.elevationPid = pid;
	gHook.elevationTime = now;
	return gHook.elevated;
}

HWND dragTarget(POINT point) {
	auto* hit = WindowFromPhysicalPoint(point);
	if (hit == nullptr) return nullptr;

	auto* hwnd = GetAncestor(hit, GA_ROOT);
	if (hwnd == nullptr || isOwnProcessWindow(hwnd)) return nullptr;
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return nullptr;

	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if ((style & WS_CHILD) != 0) return nullptr;

	auto caption = (style & WS_CAPTION) == WS_CAPTION;
	if (!caption && (style & WS_THICKFRAME) == 0) return nullptr;
	if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0) return nullptr;

	auto maximized = IsZoomed(hwnd) != FALSE;
	if (isShellWindow(hwnd)) return nullptr;

	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	if (cloaked != 0) return nullptr;

	if (!maximized && coversMonitor(hwnd)) return nullptr;
	if (isElevatedCached(hwnd)) return nullptr;

	return hwnd;
}

bool postWorker(UINT message, WPARAM wParam, LPARAM lParam) {
	auto id = gWorkerId.load();
	return id != 0 && PostThreadMessageW(id, message, wParam, lParam) != FALSE;
}

enum class DragPhase : uint8_t {
	Idle,
	Restoring,
	Dragging,
	Placing,
	Anchoring,
	Cancelling,
};

HWINEVENTHOOK gTargetHook = nullptr; // NOLINT
HWND gTargetHookWindow = nullptr;    // NOLINT

// Watches a maximized target until it reports the restore. The worker owns the
// hook, so the callback runs on the worker thread. Wake the loop through a
// thread message instead of touching the drag state here: events arrive in
// bursts and coalesce into one state check.
void CALLBACK targetEventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD /*event*/,
    HWND hwnd,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (idObject != OBJID_WINDOW || hwnd == nullptr || gTargetHookWindow == nullptr) return;
	if (hwnd != gTargetHookWindow && GetAncestor(hwnd, GA_ROOT) != gTargetHookWindow) return;

	if (!gTargetEventPending.exchange(true)) {
		postWorker(WM_SD_TARGET_EVENT, 0, 0);
	}
}

void removeTargetEvents() {
	if (gTargetHook != nullptr) UnhookWinEvent(gTargetHook);
	gTargetHook = nullptr;
	gTargetHookWindow = nullptr;
	gTargetEventPending.store(false);
}

void installTargetEvents(HWND hwnd) {
	removeTargetEvents();

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0) return;

	gTargetHookWindow = hwnd;
	gTargetHook = SetWinEventHook(
	    EVENT_OBJECT_DESTROY,
	    EVENT_OBJECT_LOCATIONCHANGE,
	    nullptr,
	    &targetEventProc,
	    pid,
	    0,
	    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
	);

	if (gTargetHook == nullptr) {
		gTargetHookWindow = nullptr;
		qCDebug(logSuperDrag) << "Could not watch window events for" << hwnd
		                      << ", the restore is only checked on input:" << GetLastError();
	}
}

// Placements run synchronously on their own thread. A synchronous SetWindowPos
// returns once the target has processed the change, so the rect read right
// after it is the target's answer, including an unchanged rect when the target
// refused the change (a window already at its minimum size). An asynchronous
// placement cannot tell a pending request from a refused one, and a refusal
// emits neither WM_WINDOWPOSCHANGED nor a location event.
//
// A hung target blocks SetWindowPos until it responds, so only this thread
// ever waits on a target. The drag worker keeps handling input and its
// deadline, and hands over one request at a time; newer targets coalesce in
// the worker. The thread is never joined: a call blocked on a hung window
// cannot be cancelled, and shutdown must not wait for it. It owns no state
// with a destructor and is reclaimed with the process.
struct PlacementRequest {
	HWND hwnd = nullptr;
	RECT rect {};
	// Set when the request restores a captured placement instead of moving the
	// window to `rect`. Esc uses it so a maximized drag gets its exact frame
	// back, including the normal rectangle Windows re-maximizes to.
	WINDOWPLACEMENT placement {};
	UINT flags = 0;
	uint32_t seq = 0;
	bool queued = false;
	bool restorePlacement = false;
};

struct PlacementAnswer {
	uint32_t seq = 0;
	bool ok = false;
	DWORD error = 0;
	RECT rect {};
};

SRWLOCK gPlacementLock = SRWLOCK_INIT; // NOLINT
PlacementRequest gPlacementRequest;    // NOLINT
PlacementAnswer gPlacementAnswer;      // NOLINT
HANDLE gPlacementEvent = nullptr;      // NOLINT

DWORD WINAPI placementMain(LPVOID /*param*/) {
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	for (;;) {
		WaitForSingleObject(gPlacementEvent, INFINITE);

		AcquireSRWLockExclusive(&gPlacementLock);
		auto request = gPlacementRequest;
		gPlacementRequest.queued = false;
		ReleaseSRWLockExclusive(&gPlacementLock);
		if (!request.queued) continue;

		const auto& rect = request.rect;
		auto answer = PlacementAnswer {.seq = request.seq};
		if (request.restorePlacement) {
			auto placed = SetWindowPlacement(request.hwnd, &request.placement) != FALSE;
			WINDOWPLACEMENT actual {};
			actual.length = sizeof(actual);
			auto read = placed && GetWindowPlacement(request.hwnd, &actual) != FALSE;
			answer.ok = dragPlacementRestored(request.placement, actual, read);
		} else {
			answer.ok = SetWindowPos(
			                request.hwnd,
			                nullptr,
			                rect.left,
			                rect.top,
			                widthOf(rect),
			                heightOf(rect),
			                request.flags
			            )
			     != FALSE;
		}
		if (answer.ok) answer.ok = GetWindowRect(request.hwnd, &answer.rect) != FALSE;
		if (!answer.ok) answer.error = GetLastError();

		AcquireSRWLockExclusive(&gPlacementLock);
		gPlacementAnswer = answer;
		ReleaseSRWLockExclusive(&gPlacementLock);
		postWorker(WM_SD_PLACED, 0, 0);
	}
}

bool startPlacementThread() {
	static const auto started = [] {
		gPlacementEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (gPlacementEvent == nullptr) return false;

		auto* thread = CreateThread(nullptr, 0, &placementMain, nullptr, 0, nullptr);
		if (thread == nullptr) return false;

		CloseHandle(thread);
		return true;
	}();

	return started;
}

void queuePlacement(HWND hwnd, const RECT& rect, UINT flags, uint32_t seq) {
	AcquireSRWLockExclusive(&gPlacementLock);
	gPlacementRequest =
	    PlacementRequest {.hwnd = hwnd, .rect = rect, .flags = flags, .seq = seq, .queued = true};
	ReleaseSRWLockExclusive(&gPlacementLock);
	SetEvent(gPlacementEvent);
}

void queuePlacement(HWND hwnd, const WINDOWPLACEMENT& placement, uint32_t seq) {
	AcquireSRWLockExclusive(&gPlacementLock);
	gPlacementRequest = PlacementRequest {
	    .hwnd = hwnd,
	    .placement = placement,
	    .seq = seq,
	    .queued = true,
	    .restorePlacement = true,
	};
	ReleaseSRWLockExclusive(&gPlacementLock);
	SetEvent(gPlacementEvent);
}

PlacementAnswer placementAnswer() {
	AcquireSRWLockShared(&gPlacementLock);
	auto answer = gPlacementAnswer;
	ReleaseSRWLockShared(&gPlacementLock);
	return answer;
}

// Numbers requests across drags, so a late answer to an earlier drag is never
// taken for the current one. Only the worker touches it.
uint32_t gPlacementSeq = 0; // NOLINT

struct Drag {
	HWND hwnd = nullptr;
	bool alive = false;
	bool notified = false;
	bool resize = false;
	bool tiled = false;
	bool left = false;
	bool top = false;
	bool ending = false;
	bool maximized = false;
	// Esc asked for the placement captured at drag start; it is applied only
	// after the restore and any outstanding placement have settled.
	bool cancelling = false;
	bool restoreSent = false;
	// The final placement for the end point has been sent.
	bool finalSent = false;
	DragPhase phase = DragPhase::Idle;
	DragWait wait;
	DragPlacement placement;
	POINT start {};
	POINT last {};
	POINT endPoint {};
	RECT origin {};
	RECT requested {};
	// The maximized frame the grab was measured against before the restore
	// moved it.
	RECT grabFrame {};
	// The exact placement the window had when the drag began. Restoring it
	// returns a maximized window to the frame Windows would otherwise
	// approximate with SW_MAXIMIZE.
	WINDOWPLACEMENT startPlacement {};
	DragLimits limits;
};

bool matchesRequest(const Drag& drag, const RECT& actual) {
	if (drag.resize) return EqualRect(&actual, &drag.requested) != FALSE;
	return actual.left == drag.requested.left && actual.top == drag.requested.top;
}

// Hand a placement to the placement thread. While one is outstanding the
// target is not queued: it coalesces, and the latest one is sent once the
// answer arrives.
void send(Drag& drag, const RECT& target, bool size) {
	if (!drag.placement.submit(++gPlacementSeq)) return;
	drag.requested = target;
	queuePlacement(drag.hwnd, target, POS_FLAGS | (size ? 0 : SWP_NOSIZE), drag.placement.seq);
}

void send(Drag& drag, const RECT& target) { send(drag, target, drag.resize); }

// The window has been restored; place its normal frame under the cursor.
// ShowWindowAsync has been requested and the caller waited for the state
// change, so this only reads the resulting placement. The grab was measured
// against the maximized frame captured before the restore, not the frame the
// restored window has now.
bool restoreGeometry(Drag& drag) {
	WINDOWPLACEMENT placement {};
	placement.length = sizeof(placement);
	if (!GetWindowPlacement(drag.hwnd, &placement)) return false;

	auto width = widthOf(placement.rcNormalPosition);
	auto height = heightOf(placement.rcNormalPosition);
	if (width <= 0 || height <= 0) return false;

	drag.origin = dragRestoredRect(placement.rcNormalPosition, drag.grabFrame, drag.start);
	return true;
}

void beginDrag(Drag& drag, HWND hwnd, bool resize, POINT point) {
	hotkeys::KeyboardHook::sendKey(hotkeys::KeyboardHook::MASK_KEY);

	drag = Drag();
	drag.hwnd = hwnd;
	drag.resize = resize;
	drag.start = point;
	drag.last = point;

	if (!GetWindowRect(hwnd, &drag.origin)) return;
	drag.requested = drag.origin;

	drag.startPlacement.length = sizeof(WINDOWPLACEMENT);
	if (!GetWindowPlacement(hwnd, &drag.startPlacement)) drag.startPlacement = {};

	auto maximized = IsZoomed(hwnd) != FALSE;
	drag.maximized = maximized;
	auto sizable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;

	qCDebug(logSuperDrag) << (resize ? "Resize" : "Move") << "drag of" << hwnd << "from" << point.x
	                      << point.y << "maximized" << maximized << "sizable" << sizable;

	if (resize && (maximized || !sizable)) return;

	drag.left = point.x < drag.origin.left + widthOf(drag.origin) / 2;
	drag.top = point.y < drag.origin.top + heightOf(drag.origin) / 2;

	auto dpi = GetDpiForWindow(hwnd);
	if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

	auto& limits = drag.limits;
	limits.minWidth = std::clamp(
	    static_cast<LONG>(GetSystemMetricsForDpi(SM_CXMINTRACK, dpi)),
	    LONG(1),
	    std::max(widthOf(drag.origin), LONG(1))
	);
	limits.minHeight = std::clamp(
	    static_cast<LONG>(GetSystemMetricsForDpi(SM_CYMINTRACK, dpi)),
	    LONG(1),
	    std::max(heightOf(drag.origin), LONG(1))
	);
	limits.maxWidth =
	    std::max(static_cast<LONG>(GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi)), limits.minWidth);
	limits.maxHeight =
	    std::max(static_cast<LONG>(GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi)), limits.minHeight);

	if (auto* gui = gGuiWindow.load()) {
		auto flags =
		    (resize ? FLAG_RESIZE : 0) | (drag.left ? FLAG_LEFT : 0) | (drag.top ? FLAG_TOP : 0);
		DWORD_PTR result = 0;

		auto sent = SendMessageTimeoutW(
		    gui,
		    WM_SD_STARTED,
		    reinterpret_cast<WPARAM>(hwnd),
		    flags,
		    SMTO_NORMAL | SMTO_ABORTIFHUNG,
		    GUI_TIMEOUT_MS,
		    &result
		);

		drag.notified = true;
		auto watched = false;
		if (sent != 0) {
			drag.tiled = (result & STARTED_TILED_RESIZE) != 0;
			watched = (result & STARTED_WATCHED) != 0;
		}

		qCDebug(logSuperDrag) << "Shell answered" << (sent != 0) << "tiled" << drag.tiled << "watched"
		                      << watched;
	}

	if (!drag.tiled) {
		constexpr UINT raiseFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
		auto topmost = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, raiseFlags);
		if (!topmost) SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, raiseFlags);
	}

	if (drag.tiled) {
		drag.alive = true;
		drag.phase = DragPhase::Dragging;
		return;
	}

	drag.phase = DragPhase::Dragging;
	drag.alive = !maximized;

	if (maximized) {
		installTargetEvents(hwnd);

		// Capture the frame the user grabbed before the restore moves it. The
		// DWM bounds exclude the invisible resize border GetWindowRect adds.
		drag.grabFrame = drag.origin;
		RECT frame {};
		if (SUCCEEDED(
		        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))
		    ))
		{
			drag.grabFrame = frame;
		}

		drag.phase = DragPhase::Restoring;
		drag.wait.begin(DragWaitKind::Restore, GetTickCount(), SETTLE_TIMEOUT_MS);
		ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
	}
}

// The target answered with the geometry it took: the requested one, its own
// (usually its minimum size) or its unchanged geometry when it refused. Settle
// on it and continue from there. A clamped resize teaches its bound through
// the limits. `origin` and `start` stay as the
// drag began, so the next target is still the original geometry plus the whole
// cursor displacement, clamped by what was learned. Replacing only the origin
// would count the displacement twice.
void acceptPlacement(Drag& drag, const RECT& actual) {
	if (drag.resize) dragLearnLimits(drag.limits, drag.requested, actual);
	drag.requested = actual;
}

RECT targetFor(Drag& drag, POINT point) {
	auto dx = point.x - drag.start.x;
	auto dy = point.y - drag.start.y;
	auto target = drag.origin;

	if (drag.resize) return dragResizedRect(drag.origin, drag.left, drag.top, dx, dy, drag.limits);

	OffsetRect(&target, dx, dy);

	RECT current {};
	auto originWidth = widthOf(drag.origin);
	auto originHeight = heightOf(drag.origin);
	if (GetWindowRect(drag.hwnd, &current) && originWidth > 0 && originHeight > 0
	    && (widthOf(current) != originWidth || heightOf(current) != originHeight))
	{
		auto fx = static_cast<double>(drag.start.x - drag.origin.left) / originWidth;
		auto fy = static_cast<double>(drag.start.y - drag.origin.top) / originHeight;
		target.left = point.x - std::lround(fx * widthOf(current));
		target.top = point.y - std::lround(fy * heightOf(current));
		target.right = target.left + widthOf(current);
		target.bottom = target.top + heightOf(current);
	}

	return target;
}

// Send the next placement once the target has answered the last one;
// until then the newest cursor position waits in the coalesced target.
void flushDrag(Drag& drag, POINT point) {
	if (drag.placement.pending) {
		drag.placement.dirty = true;
		return;
	}

	drag.placement.dirty = false;
	auto target = targetFor(drag, point);
	if (!matchesRequest(drag, target)) send(drag, target);
}

void updateDrag(Drag& drag, POINT point) {
	if (drag.phase == DragPhase::Idle) return;

	drag.last = point;
	if (!drag.alive || drag.phase != DragPhase::Dragging) return;

	auto dx = point.x - drag.start.x;
	auto dy = point.y - drag.start.y;

	if (drag.tiled) {
		gTiledDelta.store(pack(dx, dy));
		auto* gui = gGuiWindow.load();

		if (gui != nullptr && !gTiledPending.exchange(true)) {
			if (!PostMessageW(gui, WM_SD_TILED_RESIZE, 0, 0)) gTiledPending.store(false);
		}

		return;
	}

	flushDrag(drag, point);
}

bool anchorTarget(Drag& drag, LONG& x, LONG& y) {
	if (!drag.left && !drag.top) return false;

	RECT actual {};
	if (!GetWindowRect(drag.hwnd, &actual)) return false;

	x = drag.left ? drag.origin.right - widthOf(actual) : actual.left;
	y = drag.top ? drag.origin.bottom - heightOf(actual) : actual.top;
	return x != actual.left || y != actual.top;
}

void completeDrag(Drag& drag, DragWaitResult result) {
	if (result == DragWaitResult::Failed) {
		qCWarning(logSuperDrag) << "Window" << drag.hwnd
		                        << "did not take the requested state before the deadline;"
		                        << "finishing the drag";
	}

	if (drag.notified) {
		if (auto* gui = gGuiWindow.load()) {
			auto delta = pack(drag.last.x - drag.start.x, drag.last.y - drag.start.y);
			auto target = reinterpret_cast<WPARAM>(drag.hwnd);
			PostMessageW(gui, WM_SD_ENDED, target, static_cast<LPARAM>(delta));
		}
	}

	removeTargetEvents();
	drag = Drag();
}

void endDrag(Drag& drag, POINT point) {
	if (drag.phase == DragPhase::Idle) return;

	drag.ending = true;
	drag.last = point;
	drag.endPoint = point;

	if (drag.tiled) {
		completeDrag(drag, DragWaitResult::Done);
		return;
	}

	// The restore is still in flight. Its state event places the window, then
	// the end point is applied from the restored geometry.
	if (drag.phase == DragPhase::Restoring) return;
	if (drag.phase != DragPhase::Dragging) return;

	// The end point is placed once any outstanding placement has answered.
	drag.phase = DragPhase::Placing;
	drag.wait.begin(DragWaitKind::Placement, GetTickCount(), SETTLE_TIMEOUT_MS);
}

// Esc cancels like the native move loop does: the shell drops its plan and the
// window goes back where it started, maximized again if it was.
void cancelDrag(Drag& drag) {
	if (drag.phase == DragPhase::Idle || drag.ending) return;

	if (drag.notified) {
		if (auto* gui = gGuiWindow.load()) {
			PostMessageW(gui, WM_SD_CANCELLED, reinterpret_cast<WPARAM>(drag.hwnd), 0);
		}
	}

	if (drag.maximized && !drag.tiled && drag.startPlacement.length != 0) {
		// Restore the exact placement captured at drag start instead of asking
		// Windows to maximize again. The write must be last, so pumpDrag waits
		// for the restore and any outstanding placement before sending it. The
		// target watcher stays installed: its state event wakes the worker with
		// the restore answer instead of letting the cancel sleep to the deadline.
		drag.cancelling = true;
		drag.wait.begin(DragWaitKind::Placement, GetTickCount(), SETTLE_TIMEOUT_MS);
		drag.phase = DragPhase::Cancelling;
		return;
	}

	if (drag.maximized && !drag.tiled) {
		// The placement could not be captured; keep the old best effort.
		ShowWindowAsync(drag.hwnd, SW_MAXIMIZE);
		completeDrag(drag, DragWaitResult::Done);
		return;
	}

	endDrag(drag, drag.start);
}

// The placement thread answered. A failed placement ends the drag; any answer
// otherwise settles the geometry the target took, so the next placement can go.
void placementAnswered(Drag& drag) {
	auto answer = placementAnswer();
	if (!drag.placement.answer(answer.seq)) return;

	if (!answer.ok) {
		// A window closed during the drag has no further state to place.
		auto closed = IsWindow(drag.hwnd) == FALSE;
		if (!closed) qCWarning(logSuperDrag) << "Placing window" << drag.hwnd << "failed:" << answer.error;
		completeDrag(drag, closed ? DragWaitResult::Done : DragWaitResult::Failed);
		return;
	}

	acceptPlacement(drag, answer.rect);
}

void pumpDrag(Drag& drag) {
	if (drag.phase == DragPhase::Idle) return;

	if (drag.phase == DragPhase::Restoring) {
		auto result = dragRestoreWait(drag.wait, IsZoomed(drag.hwnd) != FALSE, GetTickCount());
		if (result == DragWaitResult::Waiting) return;
		if (result == DragWaitResult::Failed) {
			completeDrag(drag, result);
			return;
		}

		drag.wait.clear();
		removeTargetEvents();
		if (!restoreGeometry(drag)) {
			completeDrag(drag, DragWaitResult::Done);
			return;
		}

		drag.alive = true;
		drag.phase = DragPhase::Dragging;

		if (drag.ending) {
			endDrag(drag, drag.endPoint);
		} else {
			send(drag, drag.origin);
			return;
		}
	}

	if (drag.phase == DragPhase::Cancelling) {
		// The restore captured at drag start is the last write. It waits for
		// Windows to leave the maximized state and for a placement that was in
		// flight to answer. After it is sent, completion is its own answer: the
		// saved placement may be maximized, so the maximized state is never
		// consulted again.
		auto step = dragCancelStep(
		    drag.restoreSent,
		    IsZoomed(drag.hwnd) == FALSE,
		    drag.placement.pending,
		    drag.wait,
		    GetTickCount()
		);

		switch (step) {
		case DragCancelStep::WaitForRestore:
		case DragCancelStep::WaitForAnswer: return;
		case DragCancelStep::Failed: completeDrag(drag, DragWaitResult::Failed); return;
		case DragCancelStep::SendRestore:
			drag.restoreSent = true;
			drag.wait.begin(DragWaitKind::Placement, GetTickCount(), SETTLE_TIMEOUT_MS);
			{
				auto seq = ++gPlacementSeq;
				if (!drag.placement.submit(seq)) return;
				queuePlacement(drag.hwnd, drag.startPlacement, seq);
			}
			return;
		case DragCancelStep::Done: completeDrag(drag, DragWaitResult::Done); return;
		}

		return;
	}

	if (drag.phase == DragPhase::Placing || drag.phase == DragPhase::Anchoring) {
		auto result = dragPlacementWait(drag.wait, drag.placement.pending, GetTickCount());
		if (result == DragWaitResult::Waiting) return;
		if (result == DragWaitResult::Failed) {
			completeDrag(drag, result);
			return;
		}

		if (drag.phase == DragPhase::Placing) {
			if (!drag.finalSent) {
				drag.finalSent = true;
				auto target = targetFor(drag, drag.endPoint);
				if (!matchesRequest(drag, target)) {
					send(drag, target);
					return;
				}
			}

			// The answer was accepted, so its limit is learned before the
			// anchored edge is decided.
			LONG x = 0;
			LONG y = 0;
			if (drag.resize && anchorTarget(drag, x, y)) {
				auto anchored = drag.requested;
				OffsetRect(&anchored, x - anchored.left, y - anchored.top);
				drag.phase = DragPhase::Anchoring;
				send(drag, anchored, false);
				return;
			}
		}

		completeDrag(drag, DragWaitResult::Done);
		return;
	}

	if (drag.phase == DragPhase::Dragging && drag.placement.dirty) flushDrag(drag, drag.last);
}

DWORD dragWaitTimeout(const Drag& drag) {
	if (!drag.wait.active()) return INFINITE;

	auto elapsed = GetTickCount() - drag.wait.started;
	if (elapsed >= drag.wait.timeoutMs) return 0;
	return drag.wait.timeoutMs - elapsed;
}

} // namespace

SuperDragManager* SuperDragManager::instance() {
	static QPointer<SuperDragManager> manager; // NOLINT

	if (manager.isNull()) {
		manager = new SuperDragManager(QCoreApplication::instance());
	}

	return manager.data();
}

SuperDragManager::SuperDragManager(QObject* parent): QObject(parent) {
	WNDCLASSW wndClass {};
	wndClass.lpfnWndProc = &SuperDragManager::messageWindowProc;
	wndClass.hInstance = GetModuleHandleW(nullptr);
	wndClass.lpszClassName = MESSAGE_WINDOW_CLASS;
	RegisterClassW(&wndClass);

	this->messageWindow = CreateWindowExW(
	    0,
	    MESSAGE_WINDOW_CLASS,
	    L"",
	    0,
	    0,
	    0,
	    0,
	    0,
	    HWND_MESSAGE,
	    nullptr,
	    wndClass.hInstance,
	    nullptr
	);

	if (this->messageWindow == nullptr) {
		qCWarning(logSuperDrag) << "Failed to create the message window,"
		                        << "tiling won't follow Super drags.";
	}

	gGuiWindow.store(this->messageWindow);
}

SuperDragManager::~SuperDragManager() {
	gEnabled.store(false);
	gGuiWindow.store(nullptr);
	if (this->mEnabled) hotkeys::KeyboardHook::release();
	this->stopWorker();

	if (this->messageWindow != nullptr) {
		DestroyWindow(this->messageWindow);
		this->messageWindow = nullptr;
	}
}

void SuperDragManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;

	if (enabled && !this->startWorker()) {
		qCWarning(logSuperDrag) << "Failed to start the drag thread, Super drags are unavailable.";
		return;
	}

	this->mEnabled = enabled;
	gEnabled.store(enabled);

	auto* tracker = InputMaskTracker::instance();

	hotkeys::KeyboardHook::setEscapeFilter(enabled ? &SuperDragManager::onEscape : nullptr);

	if (enabled) {
		tracker->acquireHook();
		if (!tracker->hookActive()) {
			qCWarning(logSuperDrag) << "Mouse hook unavailable, Super drags won't work.";
		}

		// Esc cancels an active drag from the keyboard hook's filter, so the
		// drag holds the hook while it is enabled. The reference is independent
		// of the hotkey manager's binds: a user with no keybinds still gets Esc.
		if (!hotkeys::KeyboardHook::retain()) {
			qCWarning(logSuperDrag)
			    << "Keyboard hook unavailable, Esc won't cancel a Super drag.";
		}
	} else {
		tracker->releaseHook();
		hotkeys::KeyboardHook::release();
	}

	emit this->enabledChanged();
}

bool SuperDragManager::startWorker() {
	if (this->workerRunning) return true;
	if (!startPlacementThread()) return false;

	gWorkerId.store(0);
	gWorkerStop.store(false);
	this->worker = std::thread(&SuperDragManager::workerMain);

	this->workerRunning = true;
	return true;
}

void SuperDragManager::stopWorker() {
	if (!this->workerRunning) return;

	gWorkerStop.store(true);
	auto id = gWorkerId.exchange(0);
	if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);

	this->worker.join();
	this->workerRunning = false;
}

void SuperDragManager::workerMain() {
	MSG msg {};
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	prioritizeInputThread(THREAD_PRIORITY_HIGHEST);
	gWorkerId.store(GetCurrentThreadId());
	if (gWorkerStop.load()) return;

	auto drag = Drag();

	for (;;) {
		auto timeout = dragWaitTimeout(drag);
		auto wait = MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

		if (wait == WAIT_TIMEOUT) {
			pumpDrag(drag);
			continue;
		}

		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			switch (msg.message) {
			case WM_QUIT: completeDrag(drag, DragWaitResult::Done); return;
			case WM_SD_BEGIN_MOVE:
			case WM_SD_BEGIN_RESIZE:
				completeDrag(drag, DragWaitResult::Done);
				beginDrag(
				    drag,
				    reinterpret_cast<HWND>(msg.wParam), // NOLINT(performance-no-int-to-ptr)
				    msg.message == WM_SD_BEGIN_RESIZE,
				    unpack(static_cast<uint64_t>(msg.lParam))
				);
				break;
			case WM_SD_MOVE:
				gMovePending.store(false);
				updateDrag(drag, unpack(gCursor.load()));
				break;
			case WM_SD_END:
				gMovePending.store(false);
				endDrag(drag, unpack(static_cast<uint64_t>(msg.lParam)));
				break;
			case WM_SD_CANCEL:
				gMovePending.store(false);
				cancelDrag(drag);
				break;
			case WM_SD_TARGET_EVENT: gTargetEventPending.store(false); break;
			case WM_SD_PLACED: placementAnswered(drag); break;
			default: DispatchMessageW(&msg); break;
			}
			pumpDrag(drag);
		}
	}
}

bool SuperDragManager::onMouseHook(WPARAM message, const MSLLHOOKSTRUCT* info) {
	auto& hook = gHook;

	if (hook.active) {
		if (message == WM_MOUSEMOVE) {
			if (gCancelled.load()) return false;
			gCursor.store(pack(info->pt));
			if (!gMovePending.exchange(true) && !postWorker(WM_SD_MOVE, 0, 0)) gMovePending.store(false);
			return false;
		}

		if (message == (hook.resize ? WM_RBUTTONUP : WM_LBUTTONUP)) {
			hook.active = false;
			gDragLive.store(false);
			// A cancelled drag already ended; its release only needs swallowing.
			if (!gCancelled.exchange(false)) postWorker(WM_SD_END, 0, static_cast<LPARAM>(pack(info->pt)));
			return true;
		}

		return message == (hook.resize ? WM_RBUTTONDOWN : WM_LBUTTONDOWN);
	}

	auto resize = message == WM_RBUTTONDOWN;
	if (message != WM_LBUTTONDOWN && !resize) return false;
	if (!gEnabled.load(std::memory_order_relaxed) || !superAlone()) return false;

	auto* hwnd = dragTarget(info->pt);
	if (hwnd == nullptr) return false;

	gCursor.store(pack(info->pt));

	auto begin = resize ? WM_SD_BEGIN_RESIZE : WM_SD_BEGIN_MOVE;
	if (!postWorker(begin, reinterpret_cast<WPARAM>(hwnd), static_cast<LPARAM>(pack(info->pt)))) {
		return false;
	}

	hook.active = true;
	hook.resize = resize;
	gCancelled.store(false);
	gDragLive.store(true);
	hotkeys::KeyboardHook::noteSuperChord();
	return true;
}

// Keyboard hook thread: Esc during a Super drag cancels it and is consumed.
bool SuperDragManager::onEscape() {
	if (!gDragLive.load() || gCancelled.exchange(true)) return false;
	if (postWorker(WM_SD_CANCEL, 0, 0)) return true;
	gCancelled.store(false);
	return false;
}

void SuperDragManager::onHookStopped() {
	if (!gHook.active) return;
	gHook.active = false;
	gDragLive.store(false);
	if (!gCancelled.exchange(false)) postWorker(WM_SD_END, 0, static_cast<LPARAM>(gCursor.load()));
}

LRESULT CALLBACK
SuperDragManager::messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	auto* target = reinterpret_cast<HWND>(wParam); // NOLINT(performance-no-int-to-ptr)

	switch (msg) {
	case WM_SD_STARTED: return SuperDragManager::instance()->onStarted(target, lParam);
	case WM_SD_TILED_RESIZE: SuperDragManager::instance()->onTiledResize(); return 0;
	case WM_SD_ENDED: SuperDragManager::instance()->onEnded(target, lParam); return 0;
	case WM_SD_CANCELLED: SuperDragManager::instance()->onCancelled(target); return 0;
	default: return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

LRESULT SuperDragManager::onStarted(HWND hwnd, LPARAM flags) {
	this->finishGuiDrag(nullptr);

	auto* tiling = TilingManager::active();
	if (tiling == nullptr) return 0;

	auto* tracker = WindowTracker::instance();
	this->guiDrag.hwnd = hwnd;

	if ((flags & FLAG_RESIZE) != 0) {
		auto* window = tracker->windowFor(hwnd);
		auto left = (flags & FLAG_LEFT) != 0;
		auto top = (flags & FLAG_TOP) != 0;

		if (window != nullptr && tiling->beginEdgeDrag(window, left, top)) {
			this->guiDrag.tiledResize = true;
			return STARTED_TILED_RESIZE;
		}
	}

	tracker->noteMoveSize(hwnd, true);
	this->guiDrag.notified = true;
	return STARTED_WATCHED;
}

void SuperDragManager::onTiledResize() {
	gTiledPending.store(false);
	if (!this->guiDrag.tiledResize) return;

	auto delta = unpack(gTiledDelta.load());
	if (auto* tiling = TilingManager::active()) tiling->edgeDrag(QPoint(delta.x, delta.y));
}

void SuperDragManager::onEnded(HWND hwnd, LPARAM delta) {
	if (hwnd == nullptr || hwnd != this->guiDrag.hwnd) return;

	auto point = unpack(static_cast<uint64_t>(delta));
	this->finishGuiDrag(&point);
}

// The worker is putting the window back; the move commits nothing.
void SuperDragManager::onCancelled(HWND hwnd) {
	if (hwnd == nullptr || hwnd != this->guiDrag.hwnd || !this->guiDrag.notified) return;

	auto* window = WindowTracker::instance()->windowFor(hwnd);
	auto* tiling = TilingManager::active();
	if (window != nullptr && tiling != nullptr) tiling->cancelDrag(window);
}

void SuperDragManager::finishGuiDrag(const POINT* delta) {
	auto drag = this->guiDrag;
	this->guiDrag = GuiDrag();
	if (drag.hwnd == nullptr) return;

	if (drag.tiledResize) {
		if (auto* tiling = TilingManager::active()) {
			if (delta != nullptr) tiling->edgeDrag(QPoint(delta->x, delta->y));
			tiling->endEdgeDrag();
		}
	} else if (drag.notified) {
		WindowTracker::instance()->noteMoveSize(drag.hwnd, false);
	}
}

SuperDrag::SuperDrag(QObject* parent): QObject(parent) {
	QObject::connect(
	    SuperDragManager::instance(),
	    &SuperDragManager::enabledChanged,
	    this,
	    &SuperDrag::enabledChanged
	);
}

bool SuperDrag::enabled() const { return SuperDragManager::instance()->enabled(); }
void SuperDrag::setEnabled(bool enabled) { SuperDragManager::instance()->setEnabled(enabled); }

} // namespace qs::windows
