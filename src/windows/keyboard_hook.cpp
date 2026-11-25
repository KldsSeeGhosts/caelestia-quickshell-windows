#include "keyboard_hook.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qtimer.h>
#include <windows.h>
#include <wtsapi32.h>

#include "input_mask.hpp"
#include "util.hpp"

namespace qs::windows::hotkeys {

namespace {

Q_LOGGING_CATEGORY(logKeyboardHook, "quickshell.windows.hotkeys", QtInfoMsg);

std::atomic<std::shared_ptr<const HookSnapshot>> gSnapshot; // NOLINT
std::atomic<HWND> gTarget = nullptr;                         // NOLINT
std::atomic<UINT> gMessageBase = 0;                          // NOLINT
std::atomic<DWORD> gThreadId = 0;                            // NOLINT
std::atomic<bool> gStopRequested = false;                    // NOLINT
std::atomic<bool> gFailed = false;                           // NOLINT
// Compatibility mirror for tests and for the brief period before the keyboard
// hook thread starts. The running hook reads the sequenced publication below.
std::atomic<bool> gForegroundBlocked = false; // NOLINT
std::atomic<bool> gSuppressLoneSuperTap = false; // NOLINT
std::atomic<bool (*)()> gEscapeFilter = nullptr; // NOLINT
std::atomic<void (*)()> gSuperPressed = nullptr; // NOLINT

using hookwatch::detail::keyboardHookState;
using hookwatch::detail::mouseHookState;

std::thread* gThread = nullptr; // NOLINT
std::atomic<uint64_t> gHookGeneration = 1; // NOLINT
std::atomic<bool> gHookGenerationLive = false;     // NOLINT
thread_local uint64_t tHookGeneration = 0;         // NOLINT

struct HeldKey {
	uint32_t id = 0;
	uint32_t serial = 0;
	DWORD lastTime = 0;
	bool active = false;
};

std::array<HeldKey, 256> gHeld {}; // NOLINT
bool gMaskHeld[256] = {};          // NOLINT
uint8_t gTapCandidate = 0;         // NOLINT
DWORD gTapStart = 0;               // NOLINT
uint8_t gHoldActive = 0;           // NOLINT
// A Super chord was swallowed since the Windows key went down. If an earlier
// native chord exposed Win-down, keep the legacy masked-release fallback.
bool gSuperChordConsumed = false; // NOLINT
bool gEscapeClaimed = false; // NOLINT
TaskbarShortcutFilter gTaskbarFilter; // NOLINT
UINT_PTR gWinLeaseTimer = 0;          // NOLINT
// Suppressed Win downs do not update GetAsyncKeyState. Keep the two sides
// separately, and publish only the physical hold for the mouse hook.
struct WinKey {
	bool down = false;
	bool forwarded = false;
	// The hook swallowed this down, so its repeat and release belong to the
	// hook as well. A down that passed through must pass its release too.
	bool consumed = false;
	DWORD lastTime = 0;
};
std::array<WinKey, 2> gWin {}; // NOLINT
std::atomic<bool> gWinDown = false; // NOLINT
bool gReplayed[256] = {}; // NOLINT

// Set only while a caller waits for physical keys to come up. Posting every
// transition lets that caller see the release that matters and cancel on new
// input without a second hook or a polling loop.
std::atomic<HWND> gInputObserver = nullptr; // NOLINT
std::atomic<UINT> gInputObserverMessage = 0; // NOLINT
std::atomic<UINT> gInputObserverResetMessage = 0; // NOLINT
// Physical state as the hook sees it, including keys it swallows before
// Windows updates GetAsyncKeyState (the Win key and a hook-backed chord key).
// `gPhysicalSwallowed` marks a down the hook consumed, so reconciliation must
// leave it to the hook's own release instead of trusting the system state.
std::atomic<bool> gPhysicalDown[256] {}; // NOLINT
bool gPhysicalSwallowed[256] = {};       // NOLINT

// The hook thread's queue is the serialization point for physical state: a
// snapshot and its observer subscription are both taken there, so a release
// cannot slip between them.
constexpr UINT WM_QS_ARM_OBSERVER = WM_APP + 0x40;
constexpr DWORD HOOK_START_TIMEOUT_MS = 5000;
constexpr DWORD HOOK_STOP_TIMEOUT_MS = 250;
constexpr DWORD OBSERVER_TIMEOUT_MS = 250;

struct InputObserverRequest {
	enum class State { Pending, Armed, Cancelled };
	~InputObserverRequest() { if (this->done != nullptr) CloseHandle(this->done); }
	HWND target = nullptr;
	UINT keyMessage = 0;
	UINT resetMessage = 0;
	HANDLE done = nullptr;
	std::vector<uint8_t> keys;
	std::atomic<State> state = State::Pending;
};
std::atomic<std::shared_ptr<InputObserverRequest>> gObserverRequest; // NOLINT

struct ForegroundIntegritySnapshot {
	HWND hwnd = nullptr;
	bool blocked = false;
	bool valid = false;
};

// A sequence lock around the two foreground fields. The worker publishes both
// as one consistent state with three always-lock-free atomics, while the
// low-level hook never takes a mutex or waits on the worker's message loop.
class ForegroundIntegrityPublication {
public:
	void publish(HWND hwnd, bool blocked) { this->publish(hwnd, blocked, [] {}); }

	void publish(HWND hwnd, bool blocked, auto midway) {
		this->sequence.fetch_add(1); // odd: a write is in progress
		this->blockedValue.store(blocked);
		midway();
		this->hwndValue.store(hwnd);
		this->sequence.fetch_add(1); // even: the pair is consistent again
	}

	[[nodiscard]] ForegroundIntegritySnapshot load() const {
		for (auto attempt = 0; attempt < 3; ++attempt) {
			auto before = this->sequence.load();
			if ((before & 1) != 0) continue;
			auto snapshot = ForegroundIntegritySnapshot {
				.hwnd = this->hwndValue.load(),
				.blocked = this->blockedValue.load(),
				.valid = true,
			};
			if (before == this->sequence.load()) return snapshot;
		}
		return {};
	}

private:
	std::atomic<uint32_t> sequence = 0;
	std::atomic<HWND> hwndValue = nullptr;
	std::atomic<bool> blockedValue = false;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<HWND>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

ForegroundIntegrityPublication gForegroundIntegrity; // NOLINT

void publishForegroundIntegrity(HWND hwnd, bool blocked) {
	gForegroundIntegrity.publish(hwnd, blocked);
	// Keep the fallback state coherent for callers before the hook thread is
	// up. These mirrors are never used together by the hook callback.
	gForegroundBlocked.store(blocked);
}

std::thread* gForegroundThread = nullptr;                // NOLINT
std::atomic<DWORD> gForegroundThreadId = 0;              // NOLINT
std::atomic<HANDLE> gForegroundReadyEvent = nullptr;     // NOLINT
std::atomic<HANDLE> gForegroundExitEvent = nullptr;      // NOLINT
std::atomic<bool> gForegroundStopRequested = false;     // NOLINT
std::atomic<bool> gForegroundRunning = false;           // NOLINT
constexpr DWORD FOREGROUND_WORKER_START_TIMEOUT_MS = 1000;
constexpr DWORD FOREGROUND_WORKER_STOP_TIMEOUT_MS = 2000;
bool gInputHooksQuitting = false;                       // NOLINT

bool startForegroundIntegrityWorker();
void stopForegroundIntegrityWorker(DWORD timeout = FOREGROUND_WORKER_STOP_TIMEOUT_MS);
void stopHookThread(bool notifyReset = false);
void restartKeyboardHookThread();
bool inputDesktopIsForeign();
bool isHiddenFromUs(HWND hwnd);
bool foregroundAllowsReplay();

constexpr int HOOK_WATCHDOG_INTERVAL_MS = 2000;
constexpr uint64_t HOOK_STALE_INPUT_MS = 1500;
constexpr uint64_t HOOK_MAX_RETRY_MS = 16000;
constexpr uint64_t HOOK_UNMATCHED_GRACE_MS = HOOK_WATCHDOG_INTERVAL_MS;
constexpr int SESSION_SETTLE_MS = 500;
constexpr auto HOOK_MONITOR_CLASS = L"QuickshellLowLevelHookMonitor";

[[nodiscard]] constexpr bool hookMissedInput(uint64_t rawTick, uint64_t callbackTick, uint64_t installTick) {
	return rawTick > installTick && rawTick > callbackTick && rawTick - callbackTick > HOOK_STALE_INPUT_MS;
}

// GetMessageTime is a 32-bit tick. Choose the 64-bit tick closest to now so
// queued input keeps its original timestamp across a zero crossing.
[[nodiscard]] constexpr uint64_t tick64FromMessageTime(DWORD messageTime, uint64_t now) {
	const auto delta = static_cast<int32_t>(messageTime - static_cast<DWORD>(now));
	return static_cast<uint64_t>(static_cast<int64_t>(now) + delta);
}

[[nodiscard]] constexpr bool sessionConnectedAfterEvent(bool connected, WPARAM event) {
	if (event == WTS_CONSOLE_DISCONNECT || event == WTS_REMOTE_DISCONNECT) return false;
	if (event == WTS_CONSOLE_CONNECT || event == WTS_REMOTE_CONNECT) return true;
	return connected;
}

[[nodiscard]] constexpr bool sessionLockedAfterEvent(bool locked, WPARAM event) {
	if (event == WTS_SESSION_LOCK) return true;
	if (event == WTS_SESSION_UNLOCK) return false;
	return locked;
}

struct HookRecovery {
	uint64_t rawTick = 0;
	uint64_t unmatchedSince = 0;
	uint64_t unmatchedCallbackTick = 0;
	uint64_t nextAttempt = 0;
	uint64_t retryDelay = HOOK_WATCHDOG_INTERVAL_MS;
	bool sessionPending = false;

	bool poll(const hookwatch::detail::HookState& hook, uint64_t now, bool safe, auto attempt) {
		if (!hook.needed) {
			*this = {};
			return false;
		}
		if (!safe) {
			// Input while a protected window has focus is not evidence of eviction.
			this->rawTick = 0;
			this->unmatchedSince = 0;
			this->unmatchedCallbackTick = 0;
			return false;
		}
		if (hook.active) {
			this->nextAttempt = 0;
			this->retryDelay = HOOK_WATCHDOG_INTERVAL_MS;
			if (!this->sessionPending) {
				const auto callbackTick = hook.callbackTick.load();
				const auto missed = hookMissedInput(this->rawTick, callbackTick, hook.installTick.load());
				if (!missed) {
					this->unmatchedSince = 0;
					this->unmatchedCallbackTick = 0;
					return false;
				}
				// A stalled GUI can deliver old raw input long after the hook saw
				// it. Require the unmatched evidence to survive a watchdog grace
				// window before reinstalling either hook. If a callback supersedes
				// the original evidence, unmatched input after it starts a fresh
				// grace window instead of inheriting the expired one.
				if (this->unmatchedSince == 0 || this->unmatchedCallbackTick != callbackTick) {
					this->unmatchedSince = now;
					this->unmatchedCallbackTick = callbackTick;
					return false;
				}
				if (now - this->unmatchedSince < HOOK_UNMATCHED_GRACE_MS) return false;
			}
		}
		if (now < this->nextAttempt) return false;
		this->sessionPending = false;
		this->rawTick = 0;
		this->unmatchedSince = 0;
		this->unmatchedCallbackTick = 0;
		this->nextAttempt = attempt() + this->retryDelay;
		if (this->retryDelay < HOOK_MAX_RETRY_MS) this->retryDelay *= 2;
		return true;
	}
};

struct SessionRecovery {
	uint64_t pendingAt = 0;
	uint64_t completedAt = 0;
	bool resumed = false;

	bool request(uint64_t now) {
		if (this->pendingAt != 0 || (this->completedAt != 0 && now - this->completedAt < SESSION_SETTLE_MS))
			return false;
		this->pendingAt = now + SESSION_SETTLE_MS;
		return true;
	}

	bool resume(uint64_t now, bool automatic) {
		// The user-present broadcast can follow the automatic resume much later.
		auto alreadyResumed = std::exchange(this->resumed, true);
		return (automatic || !alreadyResumed) && this->request(now);
	}

	bool take(uint64_t now, bool safe) {
		if (!safe || this->pendingAt == 0 || now < this->pendingAt) return false;
		this->pendingAt = 0;
		this->completedAt = now;
		return true;
	}
};

class LowLevelHookMonitor: public QObject {
public:
	explicit LowLevelHookMonitor(QObject* parent): QObject(parent) {
		this->timer.setInterval(HOOK_WATCHDOG_INTERVAL_MS);
		this->sessionTimer.setSingleShot(true);
		this->sessionTimer.setTimerType(Qt::PreciseTimer);
		this->sessionTimer.setInterval(SESSION_SETTLE_MS);
		QObject::connect(&this->timer, &QTimer::timeout, this, &LowLevelHookMonitor::poll);
		QObject::connect(&this->sessionTimer, &QTimer::timeout, this, &LowLevelHookMonitor::poll);
		this->createListener();
	}

	~LowLevelHookMonitor() override {
		if (this->sessionRegistered) this->unregisterSession(this->window);
		if (this->wtsModule != nullptr) FreeLibrary(this->wtsModule);
		if (this->rawRegistered) this->registerRawInput(RIDEV_REMOVE, nullptr);
		if (this->window != nullptr) DestroyWindow(this->window);
		if (this->classRegistered) UnregisterClassW(HOOK_MONITOR_CLASS, GetModuleHandleW(nullptr));
	}

	Q_DISABLE_COPY_MOVE(LowLevelHookMonitor);

	void updateDemand() {
		if (!keyboardHookState.needed) this->keyboardRecovery = {};
		if (!mouseHookState.needed) this->mouseRecovery = {};
		if (!this->timer.isActive()) this->timer.start();
	}

private:
	bool safeToRearm() const {
		if (!this->sessionConnected || this->sessionLocked || inputDesktopIsForeign()) return false;
		auto* foreground = GetForegroundWindow();
		return foreground != nullptr && !isHiddenFromUs(foreground);
	}

	void poll() {
		startForegroundIntegrityWorker();
		if (!this->sessionRegistered || !this->sessionStateKnown) this->applyQueriedSessionState();
		auto safe = safeToRearm();
		auto now = GetTickCount64();
		if (this->sessionRecovery.take(now, safe)) {
			this->keyboardRecovery.sessionPending = keyboardHookState.needed;
			this->mouseRecovery.sessionPending = mouseHookState.needed;
		}
		auto triedKeyboard = this->keyboardRecovery.poll(keyboardHookState, now, safe, [] {
			restartKeyboardHookThread();
			return GetTickCount64();
		});
		// A keyboard install can wait on a thread. Recheck before touching
		// the mouse hook and preserve its session request if focus changed.
		if (triedKeyboard && mouseHookState.needed) safe = safeToRearm();
		this->mouseRecovery.poll(mouseHookState, GetTickCount64(), safe, [] {
			InputMaskTracker::recoverMouseHook();
			return GetTickCount64();
		});
	}

	static bool registerRawInput(DWORD flags, HWND target) {
		// Generic desktop mouse and keyboard. The flags leave legacy input on.
		const RAWINPUTDEVICE devices[] {{0x01, 0x02, flags, target}, {0x01, 0x06, flags, target}};
		return RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != 0;
	}

	void noteRawInput(HRAWINPUT input, uint64_t messageTick) {
		RAWINPUTHEADER header {};
		UINT size = sizeof(header);
		if (GetRawInputData(input, RID_HEADER, &header, &size, sizeof(header)) != sizeof(header))
			return;
		// Never read the payload. Without raw input, use session recovery and
		// failed-install retries alone. LASTINPUTINFO cannot identify a device.
		if (header.dwType == RIM_TYPEKEYBOARD && keyboardHookState.needed && foregroundAllowsReplay())
			this->keyboardRecovery.rawTick = messageTick;
		else if (header.dwType == RIM_TYPEMOUSE && mouseHookState.needed && foregroundAllowsReplay())
			this->mouseRecovery.rawTick = messageTick;
	}

	void applyQueriedSessionState() {
		if (this->querySessionInformation == nullptr || this->freeWtsMemory == nullptr) return;
		WTS_CONNECTSTATE_CLASS* state = nullptr;
		DWORD bytes = 0;
		if (!this->querySessionInformation(
		        WTS_CURRENT_SERVER_HANDLE,
		        WTS_CURRENT_SESSION,
		        WTSConnectState,
		        reinterpret_cast<LPWSTR*>(&state),
		        &bytes
		    )
		    || state == nullptr)
			return;
		this->sessionConnected = *state == WTSActive;
		this->sessionStateKnown = true;
		this->freeWtsMemory(state);
	}

	void createListener() {
		auto* instance = GetModuleHandleW(nullptr);
		WNDCLASSW windowClass {};
		windowClass.lpfnWndProc = &LowLevelHookMonitor::windowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = HOOK_MONITOR_CLASS;
		this->classRegistered = RegisterClassW(&windowClass) != 0;
		if (!this->classRegistered) {
			qCWarning(logKeyboardHook) << "Could not register the low-level hook monitor window class:"
			                           << GetLastError();
			return;
		}

		// Power broadcasts do not reach message-only windows. A hidden top-level
		// window receives them without becoming visible or taking input.
		this->window = CreateWindowExW(
		    0,
		    HOOK_MONITOR_CLASS,
		    L"",
		    WS_POPUP,
		    0,
		    0,
		    0,
		    0,
		    nullptr,
		    nullptr,
		    instance,
		    this
		);
		if (this->window == nullptr) {
			qCWarning(logKeyboardHook) << "Could not create the low-level hook monitor window:" << GetLastError();
			return;
		}

		this->rawRegistered = this->registerRawInput(RIDEV_INPUTSINK, this->window);
		if (!this->rawRegistered)
			qCWarning(logKeyboardHook) << "Could not register device activity for hook recovery:" << GetLastError();

		this->wtsModule = LoadLibraryExW(L"wtsapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (this->wtsModule == nullptr) {
			qCWarning(logKeyboardHook) << "Could not load session notifications for hook recovery:" << GetLastError();
			return;
		}
		auto registerSession = reinterpret_cast<BOOL(WINAPI*)(HWND, DWORD)>(
		    GetProcAddress(this->wtsModule, "WTSRegisterSessionNotification")
		);
		this->unregisterSession = reinterpret_cast<BOOL(WINAPI*)(HWND)>(
		    GetProcAddress(this->wtsModule, "WTSUnRegisterSessionNotification")
		);
		this->querySessionInformation = reinterpret_cast<BOOL(WINAPI*)(HANDLE, DWORD, WTS_INFO_CLASS, LPWSTR*, DWORD*)>(
		    GetProcAddress(this->wtsModule, "WTSQuerySessionInformationW")
		);
		this->freeWtsMemory = reinterpret_cast<void(WINAPI*)(PVOID)>(
		    GetProcAddress(this->wtsModule, "WTSFreeMemory")
		);
		// Register before querying so a reconnect cannot slip between the two.
		if (registerSession != nullptr && this->unregisterSession != nullptr)
			this->sessionRegistered = registerSession(this->window, NOTIFY_FOR_THIS_SESSION) != 0;
		if (!this->sessionRegistered) {
			qCWarning(logKeyboardHook) << "Could not observe session changes for hook recovery:" << GetLastError();
		}
		this->applyQueriedSessionState();
	}

	static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		if (message == WM_NCCREATE) {
			auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		}
		auto* self = reinterpret_cast<LowLevelHookMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (self != nullptr) {
			if (message == WM_INPUT) {
				self->noteRawInput(
				    reinterpret_cast<HRAWINPUT>(lParam),
				    tick64FromMessageTime(static_cast<DWORD>(GetMessageTime()), GetTickCount64())
				);
			} else if (message == WM_POWERBROADCAST) {
				if (wParam == PBT_APMSUSPEND) self->sessionRecovery.resumed = false;
				else if ((wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
				         && self->sessionRecovery.resume(GetTickCount64(), wParam == PBT_APMRESUMEAUTOMATIC))
					self->sessionTimer.start();
				return TRUE;
			} else if (message == WM_WTSSESSION_CHANGE) {
				self->sessionConnected = sessionConnectedAfterEvent(self->sessionConnected, wParam);
				self->sessionLocked = sessionLockedAfterEvent(self->sessionLocked, wParam);
				if (self->sessionConnected && !self->sessionLocked
				    && (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT
				        || wParam == WTS_REMOTE_CONNECT)
				    && self->sessionRecovery.request(GetTickCount64()))
					self->sessionTimer.start();
				return 0;
			}
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	QTimer timer;
	QTimer sessionTimer;
	HookRecovery keyboardRecovery;
	HookRecovery mouseRecovery;
	SessionRecovery sessionRecovery;
	HWND window = nullptr;
	HMODULE wtsModule = nullptr;
	BOOL (WINAPI* unregisterSession)(HWND) = nullptr;
	BOOL (WINAPI* querySessionInformation)(HANDLE, DWORD, WTS_INFO_CLASS, LPWSTR*, DWORD*) = nullptr;
	void (WINAPI* freeWtsMemory)(PVOID) = nullptr;
	bool classRegistered = false;
	bool rawRegistered = false;
	bool sessionRegistered = false;
	bool sessionConnected = false;
	bool sessionLocked = false;
	bool sessionStateKnown = false;
};

QPointer<LowLevelHookMonitor> gHookMonitor; // NOLINT

void hookwatchDemandChanged() {
#ifndef QS_TEST
	if (gInputHooksQuitting) return;
	auto* app = QCoreApplication::instance();
	if (app == nullptr) return;
	static const auto quitConnection = QObject::connect(app, &QCoreApplication::aboutToQuit, app, [] {
		gInputHooksQuitting = true;
		delete gHookMonitor.data();
		stopHookThread();
		stopForegroundIntegrityWorker(INFINITE);
	});
	static const bool offscreen = qEnvironmentVariable("QT_QPA_PLATFORM").startsWith("offscreen");
	if (keyboardHookState.needed || mouseHookState.needed) {
		startForegroundIntegrityWorker();
		if (!offscreen && gHookMonitor == nullptr) gHookMonitor = new LowLevelHookMonitor(app);
		if (gHookMonitor != nullptr) gHookMonitor->updateDemand();
	} else {
		delete gHookMonitor.data();
		stopForegroundIntegrityWorker();
	}
#endif
}

// input_mask.cpp can be linked without this implementation. The inline
// hookwatch state stays inert there, while the full keyboard implementation
// registers the worker/watchdog activation callback when it is present.
struct HookwatchCallbackRegistration {
	HookwatchCallbackRegistration() { hookwatch::detail::stateChanged = &hookwatchDemandChanged; }
};

const HookwatchCallbackRegistration gHookwatchCallbackRegistration; // NOLINT

bool foregroundIntegrityBlocked(HWND foreground) {
	if (foreground == nullptr) return false;
	auto state = gForegroundIntegrity.load();
	if (state.valid && state.hwnd == foreground) return state.blocked;

	// A foreground change can reach input before the worker's WinEvent callback.
	// Unknown foreign windows fail closed; our own windows cannot be elevated
	// relative to this process and fail open.
	DWORD pid = 0;
	GetWindowThreadProcessId(foreground, &pid);
	return pid != GetCurrentProcessId();
}

bool foregroundIsBlocked() {
	// Tests and callers before the hook thread starts historically use the
	// mirror. Once the hook runs, read one consistent worker publication.
	if (gThreadId.load() == 0) return gForegroundBlocked.load();
	return foregroundIntegrityBlocked(GetForegroundWindow());
}

bool foregroundAllowsReplay() {
	// A null foreground is unknown, including during desktop transitions.
#ifdef QS_TEST
	if (gThreadId.load() == 0) return !gForegroundBlocked.load();
#endif
	auto* foreground = GetForegroundWindow();
	return foreground != nullptr && !foregroundIntegrityBlocked(foreground);
}

// Signalled once the hook is installed (or failed), and once the thread has
// stopped touching physical state.
std::atomic<HANDLE> gReadyEvent = nullptr; // NOLINT
std::atomic<HANDLE> gExitEvent = nullptr;  // NOLINT

// Shared ownership: the hotkey manager's routing and any feature that needs
// the hook while it runs (Super drags need Esc) both keep it installed.
HookOwnership gOwnership; // NOLINT

// Set when routing stops, applied on the hook thread before the next key. It
// drops bind-owned keys so their releases pass through and a restarted map
// cannot match a key that is already held.
std::atomic<bool> gRoutingStopped = false; // NOLINT

// What the handlers match when no routing is configured. Physical and
// modifier state is still tracked through it; nothing is posted or swallowed.
const HookSnapshot gNoRouting; // NOLINT

constexpr DWORD STALE_REPEAT_MS = 1500;
// A hook ahead of us can eat a release, or Win+L can change input desktops.
// Do not retain local holds forever in that case. Win repeats refresh this
// lease; a motionless hold longer than this is deliberately cancelled.
constexpr DWORD WIN_LEASE_MS = 30000;

void updateWinLeaseTimer() {
	if (gWin[0].down || gWin[1].down) {
		if (gWinLeaseTimer == 0) gWinLeaseTimer = SetTimer(nullptr, 0, 1000, nullptr);
	} else if (gWinLeaseTimer != 0) {
		KillTimer(nullptr, gWinLeaseTimer);
		gWinLeaseTimer = 0;
	}
}

bool isDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

uint8_t heldModifiers() {
	uint8_t mods = 0;
	if (isDown(VK_CONTROL)) mods |= ModCtrl;
	if (isDown(VK_MENU)) mods |= ModAlt;
	if (isDown(VK_SHIFT)) mods |= ModShift;
	if (gWinDown.load() || isDown(VK_LWIN) || isDown(VK_RWIN)) mods |= ModSuper;
	return mods;
}

int otherSide(DWORD vk) {
	switch (vk) {
	case VK_LWIN: return VK_RWIN;
	case VK_RWIN: return VK_LWIN;
	case VK_LCONTROL: return VK_RCONTROL;
	case VK_RCONTROL: return VK_LCONTROL;
	case VK_LMENU: return VK_RMENU;
	case VK_RMENU: return VK_LMENU;
	case VK_LSHIFT: return VK_RSHIFT;
	case VK_RSHIFT: return VK_LSHIFT;
	default: return 0;
	}
}

bool isExtendedKey(uint8_t vk) {
	switch (vk) {
	case VK_INSERT:
	case VK_DELETE:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_LEFT:
	case VK_RIGHT:
	case VK_UP:
	case VK_DOWN:
	case VK_LWIN:
	case VK_RWIN:
	case VK_APPS:
	case VK_RCONTROL:
	case VK_RMENU:
	case VK_DIVIDE:
	case VK_SNAPSHOT: return true;
	default: return vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2;
	}
}

INPUT keyInput(uint8_t vk, bool up, WORD scan = 0, bool extended = false) {
	INPUT input {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk;
	input.ki.wScan = scan;
	input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
	input.ki.dwExtraInfo = KeyboardHook::INJECTION_MARKER;
	return input;
}

INPUT chordInput(uint8_t vk, bool up) {
	auto scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
	return keyInput(vk, up, scan, isExtendedKey(vk));
}

INPUT replayInput(const KBDLLHOOKSTRUCT* info) {
	return keyInput(
	    static_cast<uint8_t>(info->vkCode),
	    (info->flags & LLKHF_UP) != 0,
	    static_cast<WORD>(info->scanCode),
	    (info->flags & LLKHF_EXTENDED) != 0
	);
}

void injectMask() {
	std::array<INPUT, 2> inputs {
	    keyInput(KeyboardHook::MASK_KEY, false),
	    keyInput(KeyboardHook::MASK_KEY, true),
	};
	SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

bool injectMaskedRelease(const KBDLLHOOKSTRUCT* original) {
	std::array<INPUT, 3> inputs {
	    keyInput(KeyboardHook::MASK_KEY, false),
	    keyInput(KeyboardHook::MASK_KEY, true),
	    keyInput(
	        static_cast<uint8_t>(original->vkCode),
	        true,
	        static_cast<WORD>(original->scanCode),
	        (original->flags & LLKHF_EXTENDED) != 0
	    ),
	};
	// A swallowed release whose replay did not make it in would leave the
	// modifier logically down, so the caller has to know.
	return foregroundAllowsReplay()
	    && SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

uint64_t postGeneration() {
	return tHookGeneration != 0 ? tHookGeneration : gHookGeneration.load();
}

void post(HookEvent event, uint32_t id, uint32_t serial) {
	if (gStopRequested.load() && event != HookReset && event != HookStatusChanged) return;
	auto* target = gTarget.load();
	if (target == nullptr) return;
	const auto generation = postGeneration();
	PostMessageW(
	    target,
	    gMessageBase.load() + event,
	    hookEventWParam(id, generation),
	    hookEventLParam(serial, generation)
	);
}

void postObserverMessage(HWND observer, UINT message, uint8_t vk, bool up) {
	const auto generation = postGeneration();
	PostMessageW(
	    observer,
	    message,
	    inputObserverWParam(vk, up, generation),
	    hookEventLParam(0, generation)
	);
}

void postAll(
    const HookSnapshot& snapshot,
    HookTrigger::Kind kind,
    uint8_t lone,
    HookEvent event
) {
	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind == kind && trigger.combo.lone == lone) post(event, trigger.id, snapshot.serial);
	}
}

bool hasTrigger(const HookSnapshot& snapshot, HookTrigger::Kind kind, uint8_t lone) {
	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind == kind && trigger.combo.lone == lone) return true;
	}
	return false;
}

void cancelWin(const HookSnapshot* snapshot) {
	for (size_t i = 0; i < gWin.size(); i++) {
		if (gWin[i].forwarded) {
			auto input = chordInput(i == 0 ? VK_LWIN : VK_RWIN, true);
			if (foregroundAllowsReplay()) SendInput(1, &input, sizeof(INPUT));
		}
		gWin[i] = {};
	}
	gWinDown.store(false);
	updateWinLeaseTimer();
	if ((gHoldActive & ModSuper) != 0) {
		gHoldActive &= ~ModSuper;
		if (snapshot != nullptr) postAll(*snapshot, HookTrigger::Hold, ModSuper, HookReleased);
	}
	if (gTapCandidate == ModSuper) gTapCandidate = 0;
	gSuppressLoneSuperTap.store(false);
	gSuperChordConsumed = false;
}

void expireWin(DWORD now) {
	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	if (snapshot == nullptr) return;
	for (const auto& key: gWin) {
		if (key.down && now - key.lastTime >= WIN_LEASE_MS) {
			cancelWin(snapshot.get());
			break;
		}
	}
}

std::vector<uint8_t> physicalKeys() {
	auto keys = std::vector<uint8_t>();

	for (int vk = 0x08; vk <= 0xFE; ++vk) {
		// The generic modifier aliases duplicate a side-specific key the hook
		// reports separately. The mask key is injected by the hook itself.
		if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
		if (vk == KeyboardHook::MASK_KEY) continue;
		if (gPhysicalDown[vk].load(std::memory_order_acquire)) keys.push_back(static_cast<uint8_t>(vk));
	}

	return keys;
}

// The hook starts after a registered chord's keys may already be down, so seed
// its physical view from Windows once. A later rearm reconciles instead.
void seedPhysicalState() {
	for (int vk = 0x08; vk <= 0xFE; ++vk) {
		if (vk == KeyboardHook::MASK_KEY) continue;
		gPhysicalDown[vk].store((GetAsyncKeyState(vk) & 0x8000) != 0, std::memory_order_release);
		gPhysicalSwallowed[vk] = false;
	}
}

// A release can be lost to another hook or an input-desktop change. A key the
// hook did not swallow is still visible to GetAsyncKeyState, so clear only
// those; a swallowed key has no system state and keeps the hook's own hold.
void reconcilePhysicalState() {
	for (int vk = 0x08; vk <= 0xFE; ++vk) {
		if (vk == KeyboardHook::MASK_KEY) continue;
		if (!gPhysicalDown[vk].load(std::memory_order_acquire)) continue;
		if (gPhysicalSwallowed[vk]) continue;
		if ((GetAsyncKeyState(vk) & 0x8000) != 0) continue;
		gPhysicalDown[vk].store(false, std::memory_order_release);
	}
}

// Lock screen, UAC and switch user move the input to another desktop and take
// every hold with them without a release. Drop all local state, releasing any
// injected key still logically down, then let the observer cancel its wait.
void resetPhysicalState() {
	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	// Routing may already have been removed by unregisterAll. Physical input
	// ownership still has to be closed, including forwarded Win-downs.
	cancelWin(snapshot.get());

	gWin = {};
	gWinDown.store(false);
	gHeld.fill({});
	for (auto& masked: gMaskHeld) masked = false;
	gTapCandidate = 0;
	gHoldActive = 0;
	gSuppressLoneSuperTap.store(false);
	gSuperChordConsumed = false;
	gEscapeClaimed = false;
	gTaskbarFilter.reset();

	for (size_t vk = 0; vk < std::size(gReplayed); vk++) {
		if (!gReplayed[vk]) continue;
		auto input = chordInput(static_cast<uint8_t>(vk), true);
		if (foregroundAllowsReplay()) SendInput(1, &input, sizeof(INPUT));
		gReplayed[vk] = false;
	}

	for (auto& down: gPhysicalDown) down.store(false, std::memory_order_release);
	for (auto& swallowed: gPhysicalSwallowed) swallowed = false;
}

// Runs on the hook thread, where a key transition cannot interleave between
// the snapshot and the subscription.
void armObserver(InputObserverRequest& request) {
	using State = InputObserverRequest::State;
	if (request.state.load() != State::Pending) return;
	reconcilePhysicalState();
	request.keys = physicalKeys();
	gInputObserverMessage.store(request.keyMessage, std::memory_order_relaxed);
	gInputObserverResetMessage.store(request.resetMessage, std::memory_order_relaxed);
	gInputObserver.store(request.target, std::memory_order_release);
	auto pending = State::Pending;
	if (!request.state.compare_exchange_strong(pending, State::Armed))
		gInputObserver.store(nullptr, std::memory_order_release);
	if (request.done != nullptr) SetEvent(request.done);
}

void notifyInputReset() {
	if (auto snapshot = gSnapshot.load(std::memory_order_acquire)) {
		post(HookReset, 0, snapshot->serial);
	}

	if (auto* observer = gInputObserver.load(std::memory_order_acquire); observer != nullptr) {
		postObserverMessage(observer, gInputObserverResetMessage.load(std::memory_order_relaxed), 0, false);
	}
}

void CALLBACK inputDesktopChanged(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND /*hwnd*/,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (event != EVENT_SYSTEM_DESKTOPSWITCH || gStopRequested.load()) return;

	resetPhysicalState();
	notifyInputReset();
}

// Replay the deferred downs AND this event in one batch. Passing the physical
// key after SendInput would let it overtake the queued Win-down.
LRESULT forwardWin(const KBDLLHOOKSTRUCT* info, bool mask) {
	std::array<INPUT, 5> inputs {};
	UINT count = 0;
	for (size_t i = 0; i < gWin.size(); i++) {
		// Only a down this hook swallowed is deferred and still needs replaying.
		// A down that reached Windows directly must not be sent twice.
		if (gWin[i].down && gWin[i].consumed && !gWin[i].forwarded) {
			inputs[count++] = chordInput(i == 0 ? VK_LWIN : VK_RWIN, false);
		}
	}
	if (count == 0) return 0;
	inputs[count] = replayInput(info);
	UINT total = count + 1;
	if (mask) {
		inputs[total++] = keyInput(KeyboardHook::MASK_KEY, false);
		inputs[total++] = keyInput(KeyboardHook::MASK_KEY, true);
	}
	if (!foregroundAllowsReplay()) return 0;
	auto sent = SendInput(total, inputs.data(), sizeof(INPUT));
	// Even a partial insertion may have sent a modifier down. Its release
	// must be replayed (or passed if that fails); cancellation releases it too.
	for (UINT i = 0; i < sent && i < count; i++) {
		gWin[inputs[i].ki.wVk == VK_LWIN ? 0 : 1].forwarded = true;
	}
	// The key itself may have been inserted even if a trailing mask was not.
	if (sent <= count) return 0;
	gReplayed[info->vkCode] = true;
	return 1;
}

LRESULT onWin(
    const HookSnapshot& snapshot,
    const KBDLLHOOKSTRUCT* info,
    bool up,
    bool taskbarMaskedRelease = false
) {
	auto blocked = foregroundIsBlocked();
	auto index = info->vkCode == VK_LWIN ? 0 : 1;
	auto& key = gWin[index];
	auto& other = gWin[1 - index];
	if (!up) {
		// As with ordinary held binds, a new press after a typematic-sized
		// gap recovers from a release hidden by another hook/input desktop.
		if (key.down && info->time - key.lastTime > STALE_REPEAT_MS) cancelWin(&snapshot);
		if (key.down) {
			key.lastTime = info->time;
			// A repeat of a down this hook owns stays owned; one Windows saw
			// passes through like the down did.
			return key.consumed ? 1 : 0;
		}
		// Preserve the existing UIPI boundary. A sequence whose down passed
		// through is never claimed retrospectively on its release.
		if (blocked || isDown(static_cast<int>(info->vkCode))) return 0;
		auto ownsSuper = !snapshot.masks.empty();
		for (const auto& trigger: snapshot.triggers) {
			ownsSuper |= trigger.combo.lone == ModSuper || (trigger.combo.mods & ModSuper) != 0;
		}
		// Physical Super state is tracked even when routing owns no Super
		// chord: the mouse hook asks superDown() and a later routing start
		// must see the key that is really held. Only binding work is gated.
		if (ownsSuper) {
			if (!other.down) {
				gSuppressLoneSuperTap.store(false);
				gSuperChordConsumed = false;
			}
			if (gTapCandidate != 0) gTapCandidate = 0;
			else if (heldModifiers() == 0 && hasTrigger(snapshot, HookTrigger::Tap, ModSuper)) {
				gTapCandidate = ModSuper;
				gTapStart = info->time;
			}
		}
		key = {.down = true, .forwarded = false, .consumed = ownsSuper, .lastTime = info->time};
		gWinDown.store(true);
		if (!other.down) {
			if (auto* callback = gSuperPressed.load(std::memory_order_acquire)) callback();
		}
		if (ownsSuper) {
			if (!other.down) {
				gHoldActive |= ModSuper;
				postAll(snapshot, HookTrigger::Hold, ModSuper, HookPressed);
			}
			return 1;
		}
		return 0;
	}
	if (!key.down) return taskbarMaskedRelease && injectMaskedRelease(info) ? 1 : 0;
	auto forwarded = key.forwarded;
	auto consumed = key.consumed;
	auto chordConsumed = gSuperChordConsumed;
	key = {};
	gWinDown.store(other.down);
	// Decide how Windows gets the release before posting our own notifications.
	// If the foreground changed, pass the original release through instead.
	auto swallow = false;
	if (taskbarMaskedRelease && (forwarded || !consumed)) {
		swallow = injectMaskedRelease(info);
	} else if (forwarded && chordConsumed && !other.down) {
		swallow = injectMaskedRelease(info);
	} else if (forwarded) {
		// Queue after any pending replay of the Win-down.
		auto input = replayInput(info);
		swallow = foregroundAllowsReplay() && SendInput(1, &input, sizeof(INPUT)) == 1;
	} else {
		swallow = consumed && foregroundAllowsReplay();
	}
	if (!other.down) {
		if ((gHoldActive & ModSuper) != 0) {
			gHoldActive &= ~ModSuper;
			postAll(snapshot, HookTrigger::Hold, ModSuper, HookReleased);
		}
		if (gTapCandidate == ModSuper && !gSuppressLoneSuperTap.load() && foregroundAllowsReplay()
		    && info->time - gTapStart <= snapshot.tapTimeoutMs)
		{
			postAll(snapshot, HookTrigger::Tap, ModSuper, HookTapped);
		}
		gTapCandidate = 0;
		gSuppressLoneSuperTap.store(false);
		gSuperChordConsumed = false;
	}
	return swallow ? 1 : 0;
}

DWORD integrityLevel(HANDLE token) {
	DWORD size = 0;
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
	if (size == 0) return 0;

	auto buffer = std::make_unique<uint8_t[]>(size);
	if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.get(), size, &size)) return 0;

	auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.get());
	auto count = *GetSidSubAuthorityCount(label->Label.Sid);
	return count == 0 ? 0 : *GetSidSubAuthority(label->Label.Sid, count - 1);
}

DWORD ownIntegrityLevel() {
	static const DWORD level = []() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return DWORD(0);
		auto level = integrityLevel(token);
		CloseHandle(token);
		return level;
	}();

	return level;
}

bool isHiddenFromUs(HWND hwnd) {
	if (hwnd == nullptr) return false;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0) return true;
	if (pid == GetCurrentProcessId()) return false;

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return true;

	auto hidden = true;
	HANDLE token = nullptr;
	if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
		auto level = integrityLevel(token);
		hidden = level == 0 || level > ownIntegrityLevel();
		CloseHandle(token);
	}

	CloseHandle(process);
	return hidden;
}

void checkForeground(HWND hwnd) {
	publishForegroundIntegrity(hwnd, isHiddenFromUs(hwnd));
}

void CALLBACK foregroundChanged(
    HWINEVENTHOOK /*hook*/,
    DWORD /*event*/,
	HWND /*hwnd*/,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (idObject != OBJID_WINDOW) return;
	checkForeground(GetForegroundWindow());
}

void runForegroundIntegrityWorker(auto createQueue) {
	MSG msg {};
	createQueue(&msg);
	// Publish the id only after the queue exists. If stop raced queue
	// creation, either it posts WM_QUIT or the worker observes the request.
	gForegroundThreadId.store(GetCurrentThreadId());

	HWINEVENTHOOK foregroundHook = nullptr;
	if (!gForegroundStopRequested.load()) {
		foregroundHook = SetWinEventHook(
		    EVENT_SYSTEM_FOREGROUND,
		    EVENT_SYSTEM_FOREGROUND,
		    nullptr,
		    &foregroundChanged,
		    0,
		    0,
		    WINEVENT_OUTOFCONTEXT
		);
		if (foregroundHook == nullptr) {
			qCWarning(logKeyboardHook) << "Could not observe foreground integrity:" << GetLastError();
		} else {
			checkForeground(GetForegroundWindow());
			gForegroundRunning.store(!gForegroundStopRequested.load());
		}
	}

	if (auto ready = gForegroundReadyEvent.load(); ready != nullptr) SetEvent(ready);

	while (foregroundHook != nullptr && !gForegroundStopRequested.load()
	       && GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	gForegroundRunning.store(false);
	if (foregroundHook != nullptr) UnhookWinEvent(foregroundHook);
	publishForegroundIntegrity(nullptr, false);
	gForegroundThreadId.store(0);
	if (auto exited = gForegroundExitEvent.load(); exited != nullptr) SetEvent(exited);
}

void foregroundIntegrityWorkerMain() {
	runForegroundIntegrityWorker([](MSG* msg) {
		PeekMessageW(msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	});
}

bool reapForegroundIntegrityWorker(DWORD timeout) {
	if (gForegroundThread == nullptr) return true;
	auto exited = gForegroundExitEvent.load();
	if (exited == nullptr || WaitForSingleObject(exited, timeout) != WAIT_OBJECT_0) return false;
	gForegroundThread->join();
	delete gForegroundThread;
	gForegroundThread = nullptr;
	CloseHandle(gForegroundExitEvent.exchange(nullptr));
	CloseHandle(gForegroundReadyEvent.exchange(nullptr));
	return true;
}

bool startForegroundIntegrityWorker() {
	if (gForegroundRunning.load() && !gForegroundStopRequested.load()) return true;
	if (!reapForegroundIntegrityWorker(0)) return false;

	auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	auto exited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ready == nullptr || exited == nullptr) {
		if (ready != nullptr) CloseHandle(ready);
		if (exited != nullptr) CloseHandle(exited);
		return false;
	}

	gForegroundReadyEvent.store(ready);
	gForegroundExitEvent.store(exited);
	gForegroundThreadId.store(0);
	gForegroundStopRequested.store(false);
	try {
		gForegroundThread = new std::thread(&foregroundIntegrityWorkerMain);
	} catch (...) {
		CloseHandle(gForegroundReadyEvent.exchange(nullptr));
		CloseHandle(gForegroundExitEvent.exchange(nullptr));
		return false;
	}

	if (WaitForSingleObject(ready, FOREGROUND_WORKER_START_TIMEOUT_MS) != WAIT_OBJECT_0
	    || !gForegroundRunning.load())
	{
		stopForegroundIntegrityWorker();
		return false;
	}
	return true;
}

void stopForegroundIntegrityWorker(DWORD timeout) {
	if (gForegroundThread == nullptr) {
		publishForegroundIntegrity(nullptr, false);
		return;
	}
	gForegroundStopRequested.store(true);
	if (auto threadId = gForegroundThreadId.load(); threadId != 0) {
		PostThreadMessageW(threadId, WM_QUIT, 0, 0);
	}
	if (!reapForegroundIntegrityWorker(timeout) && timeout != 0) {
		qCWarning(logKeyboardHook) << "Foreground integrity worker did not stop within its deadline";
	}
}

LRESULT onModifier(
    const HookSnapshot& snapshot, const KBDLLHOOKSTRUCT* info, uint8_t bit, bool up, bool blocked
) {
	auto vk = static_cast<int>(info->vkCode);
	auto otherDown = isDown(otherSide(info->vkCode));

	if (!up) {
		if (isDown(vk)) return 0;

		if (gTapCandidate != 0) {
			gTapCandidate = 0;
		} else if (!blocked && heldModifiers() == 0 && hasTrigger(snapshot, HookTrigger::Tap, bit)) {
			gTapCandidate = bit;
			gTapStart = info->time;
		}

		if (!otherDown) {
			gHoldActive |= bit;
			postAll(snapshot, HookTrigger::Hold, bit, HookPressed);
		}

		return 0;
	}

	auto tap = gTapCandidate == bit;
	auto plan = planModifierRelease(bit, false, otherDown, tap, blocked || !foregroundAllowsReplay(), false);
	if (tap) gTapCandidate = 0;
	auto replayed = plan.maskAndReplay && injectMaskedRelease(info);

	if ((gHoldActive & bit) != 0 && !otherDown) {
		gHoldActive &= ~bit;
		postAll(snapshot, HookTrigger::Hold, bit, HookReleased);
	}
	if (plan.postTap && foregroundAllowsReplay() && info->time - gTapStart <= snapshot.tapTimeoutMs) {
		postAll(snapshot, HookTrigger::Tap, bit, HookTapped);
	}

	// A masked release is swallowed only when its replay made it in, so the
	// modifier cannot be left logically down; a Ctrl or Shift tap has no mask
	// and keeps passing the physical release through.
	return swallowModifierRelease(plan, replayed) ? 1 : 0;
}

LRESULT onKey(
    const HookSnapshot& snapshot,
    const KBDLLHOOKSTRUCT* info,
    bool up,
    TaskbarShortcutDecision taskbar,
    bool blocked
) {
	auto vk = static_cast<uint8_t>(info->vkCode);
	auto& held = gHeld.at(vk);

	// A fast repeat/up can already be waiting ahead of the injected down.
	// Queue the rest of this key sequence too, or an up could precede its down.
	if (gReplayed[vk]) {
		auto input = replayInput(info);
		if (up) gReplayed[vk] = false;
		return foregroundAllowsReplay() && SendInput(1, &input, sizeof(INPUT)) == 1 ? 1 : 0;
	}

	if (up) {
		gMaskHeld[vk] = false;
		if (!held.active) return taskbar.swallow ? 1 : 0;
		held.active = false;
		post(HookReleased, held.id, held.serial);
		return 1;
	}

	gTapCandidate = 0;

	if (held.active) {
		if (info->time - held.lastTime <= STALE_REPEAT_MS) {
			held.lastTime = info->time;
			post(HookRepeated, held.id, held.serial);
			return 1;
		}

		held.active = false;
	}

	if (blocked) return taskbar.swallow ? 1 : 0;

	auto mods = heldModifiers();

	if (vk == VK_DELETE && (mods & ModCtrl) != 0 && (mods & ModAlt) != 0) return 0;

	// A registered Super chord is consumed by RegisterHotKey, so the hook
	// cannot swallow it. Tap the masking key while the Windows key is still
	// down instead; Windows then treats the chord as used and does not open the
	// Start menu when the Windows key is released.
	bool registered = false;
	for (const auto& combo: snapshot.masks) {
		if (combo.vk != vk || combo.mods != mods) continue;
		registered = true;
		if (!gMaskHeld[vk] && (gWin[0].forwarded || gWin[1].forwarded)) {
			injectMask();
			gMaskHeld[vk] = true;
		}
		break;
	}

	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind != HookTrigger::Combo) continue;
		if (trigger.combo.vk != vk || trigger.combo.mods != mods) continue;

		held = HeldKey {.id = trigger.id, .serial = snapshot.serial, .lastTime = info->time, .active = true};
		post(HookPressed, trigger.id, snapshot.serial);

		if ((mods & ModAlt) != 0 || gWin[0].forwarded || gWin[1].forwarded) injectMask();
		if ((mods & ModSuper) != 0) gSuperChordConsumed = true;
		return 1;
	}

	// Both hook-backed and RegisterHotKey binds win over the taskbar policy.
	// Do not forward a deferred Win-down for a filtered chord.
	if (!registered && taskbar.swallow) {
		gSuperChordConsumed = true;
		return 1;
	}
	return forwardWin(info, registered);
}

LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code != HC_ACTION || gStopRequested.load()) return CallNextHookEx(nullptr, code, wParam, lParam);

	auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam); // NOLINT(performance-no-int-to-ptr)
	qs::windows::noteHookDelay(true, info->time);
	if ((info->flags & LLKHF_INJECTED) == 0) hookwatch::noteInput(keyboardHookState);

	if ((info->flags & LLKHF_INJECTED) != 0 && info->dwExtraInfo == KeyboardHook::INJECTION_MARKER) {
		if (taskbarShortcutFilterEnabled()) {
			gTaskbarFilter
			    .handle(static_cast<int>(info->vkCode), (info->flags & LLKHF_UP) != 0, true, 0, false);
		}
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	auto up = (info->flags & LLKHF_UP) != 0;
	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	auto& routing = snapshot != nullptr ? *snapshot : gNoRouting;
	auto blocked = foregroundIsBlocked();
	auto swallow = 0;
	TaskbarShortcutDecision taskbar;
	if (taskbarShortcutFilterEnabled()) {
		auto mods = heldModifiers();
		bool bound = false;
		if (!blocked) {
			for (const auto& trigger: routing.triggers) {
				bound |= trigger.kind == HookTrigger::Combo && trigger.combo.vk == info->vkCode
				      && trigger.combo.mods == mods;
			}
			for (const auto& combo: routing.masks) {
				bound |= combo.vk == info->vkCode && combo.mods == mods;
			}
		}
		taskbar = gTaskbarFilter.handle(
		    static_cast<int>(info->vkCode),
		    up,
		    false,
		    mods,
		    gWin[info->vkCode == VK_LWIN ? 1 : 0].down
		        || isDown(info->vkCode == VK_LWIN ? VK_RWIN : VK_LWIN),
		    bound
		);
	}

	// Routing stopped. Drop the keys its binds owned: their releases pass
	// through instead of being swallowed for a map that no longer exists, and
	// a restarted map can never match a key that is already held.
	if (gRoutingStopped.exchange(false, std::memory_order_acq_rel)) {
		gHeld.fill({});
		for (auto& masked: gMaskHeld) masked = false;
		gTapCandidate = 0;
		gHoldActive = 0;
	}

	// Esc that cancels a Super drag is consumed with its release. The filter
	// claims it only while such a drag is active, so Esc passes otherwise.
	if (info->vkCode == VK_ESCAPE && !up && !gEscapeClaimed) {
		auto* filter = gEscapeFilter.load(std::memory_order_acquire);
		gEscapeClaimed = filter != nullptr && filter();
		if (gEscapeClaimed) swallow = 1;
	} else if (info->vkCode == VK_ESCAPE && gEscapeClaimed) {
		// The claimed press's repeats and release.
		if (up) gEscapeClaimed = false;
		swallow = 1;
	}

	if (swallow == 0 && info->vkCode <= 0xFF) {
		// Physical and modifier state is tracked whenever the hook is
		// installed, routing or not: Super down/up feeds superDown(), and the
		// releases of keys the hook swallowed must still be handled. Only bind
		// matching and swallowing use the routing snapshot.
		static uint32_t routingSerial = 0;
		if (snapshot == nullptr) {
			routingSerial = 0;
		} else if (routingSerial != snapshot->serial) {
			routingSerial = snapshot->serial;
			gTapCandidate = gHoldActive = 0;
		}
		auto bit = modifierBit(info->vkCode);

		expireWin(info->time);
		swallow = bit == ModSuper ? onWin(routing, info, up, taskbar.maskedRelease)
		        : bit != 0        ? onModifier(routing, info, bit, up, blocked)
		                          : onKey(routing, info, up, taskbar, blocked);
	}
	updateWinLeaseTimer();

	// Record the physical transition after the decision, so a swallowed down
	// is known to have no GetAsyncKeyState counterpart, then notify the
	// observer. Both happen on the hook thread, in event order.
	if (info->vkCode <= 0xFF) {
		auto vk = static_cast<uint8_t>(info->vkCode);
		gPhysicalDown[vk].store(!up, std::memory_order_release);
		gPhysicalSwallowed[vk] = !up && swallow != 0;

		if (auto* observer = gInputObserver.load(std::memory_order_acquire);
		    observer != nullptr && !gStopRequested.load())
		{
			postObserverMessage(
			    observer,
			    gInputObserverMessage.load(std::memory_order_relaxed),
			    static_cast<uint8_t>(info->vkCode),
			    up
			);
		}
	}

	if (swallow != 0) return 1;

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void signalReady() {
	if (auto ready = gReadyEvent.load(); ready != nullptr) SetEvent(ready);
}

void hookThreadMain(uint64_t generation) {
	MSG msg {};
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	gThreadId.store(GetCurrentThreadId());
	tHookGeneration = generation;

	if (gStopRequested.load()) {
		gThreadId.store(0);
		signalReady();
		if (auto exited = gExitEvent.load(); exited != nullptr) SetEvent(exited);
		return;
	}

	prioritizeInputThread(THREAD_PRIORITY_TIME_CRITICAL);

	auto* hook = SetWindowsHookExW(WH_KEYBOARD_LL, &hookProc, GetModuleHandleW(nullptr), 0);

	if (hook == nullptr) {
		auto error = GetLastError();
		gFailed.store(true);
		qCWarning(logKeyboardHook) << "Failed to install the keyboard hook (error" << error
		                           << "), binds with the Windows key are unavailable.";
		gThreadId.store(0);
		signalReady();
		if (auto exited = gExitEvent.load(); exited != nullptr) SetEvent(exited);
		return;
	}

	gHookGenerationLive.store(true, std::memory_order_release);
	hookwatch::noteInstalled(keyboardHookState);

	// A secure desktop (lock screen, UAC, switch user) takes every hold away
	// without a release. Reset on the switch instead of retaining stale keys.
	auto* desktopHook = SetWinEventHook(
	    EVENT_SYSTEM_DESKTOPSWITCH,
	    EVENT_SYSTEM_DESKTOPSWITCH,
	    nullptr,
	    &inputDesktopChanged,
	    0,
	    0,
	    WINEVENT_OUTOFCONTEXT
	);

	seedPhysicalState();
	signalReady();

	while (!gStopRequested.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (msg.message == WM_QS_ARM_OBSERVER) {
			if (auto request = gObserverRequest.exchange(nullptr); request && !gStopRequested.load())
				armObserver(*request);
			continue;
		}
		if (msg.message == WM_TIMER && gWinLeaseTimer != 0 && msg.wParam == gWinLeaseTimer) {
			expireWin(GetTickCount());
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (desktopHook != nullptr) UnhookWinEvent(desktopHook);
	resetPhysicalState();
	UnhookWindowsHookEx(hook);

	// Last write to the thread id: a request that observes this event knows the
	// thread will not touch physical state or its request any more.
	gThreadId.store(0);
	if (auto exited = gExitEvent.load(); exited != nullptr) SetEvent(exited);
}

bool reapHookThread(DWORD timeout) {
	if (gThread == nullptr) return true;
	auto exited = gExitEvent.load();
	if (WaitForSingleObject(exited, timeout) != WAIT_OBJECT_0) return false;
	gThread->join();
	delete gThread;
	gThread = nullptr;
	CloseHandle(gExitEvent.exchange(nullptr));
	CloseHandle(gReadyEvent.exchange(nullptr));
	return true;
}

void setKeyboardActive(bool active) {
	if (std::exchange(keyboardHookState.active, active) != active) post(HookStatusChanged, 0, 0);
}

void stopHookThread(bool notifyReset) {
	gStopRequested.store(true);
	// Invalidate queued hook input before publishing the reset. Events posted
	// by a thread that outlives its join deadline keep its older generation.
	// Repeated teardown of the same hook must not invalidate the reset itself.
	if (gHookGenerationLive.exchange(false, std::memory_order_acq_rel))
		gHookGeneration.fetch_add(1, std::memory_order_acq_rel);
	auto threadId = gThreadId.load();
	if (threadId != 0) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
	if (!reapHookThread(HOOK_STOP_TIMEOUT_MS))
		qCWarning(logKeyboardHook) << "Keyboard hook did not stop within its deadline";
	if (notifyReset) notifyInputReset();
	gInputObserver.store(nullptr);
	gObserverRequest.store(nullptr);
	setKeyboardActive(false);
}

// Installs the thread unless it is already up. Routing target and message base
// stay whatever the last start()/retain() set, so a hook kept alive by another
// owner keeps answering the hotkey manager until it stops routing.
bool startHookThread() {
	if (KeyboardHook::isRunning()) return true;
	if (!reapHookThread(0)) return false;

	auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	auto exited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ready == nullptr || exited == nullptr) {
		if (ready != nullptr) CloseHandle(ready);
		if (exited != nullptr) CloseHandle(exited);
		return false;
	}

	gReadyEvent.store(ready);
	gExitEvent.store(exited);
	gThreadId.store(0);
	gStopRequested.store(false);
	gFailed.store(false);
	try {
		gThread = new std::thread(&hookThreadMain, gHookGeneration.load());
	} catch (...) {
		CloseHandle(gReadyEvent.exchange(nullptr));
		CloseHandle(gExitEvent.exchange(nullptr));
		stopHookThread();
		return false;
	}

	auto answered = WaitForSingleObject(ready, HOOK_START_TIMEOUT_MS) == WAIT_OBJECT_0;
	auto started = answered && !gFailed.load() && gThreadId.load() != 0;
	if (!started) {
		stopHookThread();
	} else {
		setKeyboardActive(true);
	}
	return started;
}

void restartKeyboardHookThread() {
	if (keyboardHookState.active) {
		qCWarning(logKeyboardHook) << "Re-arming the keyboard low-level hook";
		stopHookThread(true);
	}
	// Keep the routing target and owner demand even when the old thread has
	// not exited or installation of the replacement fails.
	if (!startHookThread()) qCWarning(logKeyboardHook) << "Could not re-arm the keyboard low-level hook";
}

bool inputDesktopIsForeign() {
	auto* input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
	if (input == nullptr) return true;
	auto* thread = GetThreadDesktop(GetCurrentThreadId());

	wchar_t inputName[256] {};
	wchar_t threadName[256] {};
	DWORD needed = 0;
	auto inputNamed = GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) != 0;
	needed = 0;
	auto threadNamed = thread != nullptr
	                 && GetUserObjectInformationW(thread, UOI_NAME, threadName, sizeof(threadName), &needed) != 0;
	CloseDesktop(input);
	return !inputNamed || !threadNamed || wcscmp(inputName, threadName) != 0;
}

} // namespace

uint8_t modifierBit(DWORD vk) {
	switch (vk) {
	case VK_LWIN:
	case VK_RWIN: return ModSuper;
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL: return ModCtrl;
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU: return ModAlt;
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT: return ModShift;
	default: return 0;
	}
}

bool KeyboardHook::start(HWND target, UINT messageBase) {
	if (gInputHooksQuitting) return false;
	gTarget.store(target);
	gMessageBase.store(messageBase);
	gOwnership.setRouting(true);
	hookwatch::setNeeded(keyboardHookState, true);
	return startHookThread();
}

void KeyboardHook::stop() {
	// Stop routing, but keep the hook while another owner still needs it. An
	// active Super drag holds it so Esc can cancel without configured binds.
	gOwnership.setRouting(false);

	// Routing is over. A holder may keep the hook installed for its own filter;
	// physical state tracking continues, but no stale bind may match a key.
	setSnapshot(nullptr);
	gRoutingStopped.store(true, std::memory_order_release);
	gTarget.store(nullptr);

	// Opted-in taskbar filtering owns the hook for the process lifetime.
	if (gOwnership.needed() || taskbarShortcutFilterEnabled()) return;

	stopHookThread();
	hookwatch::setNeeded(keyboardHookState, false);
}

bool KeyboardHook::retain() {
	gOwnership.hold();
	if (gInputHooksQuitting) return false;

	// A feature's reference is independent of the hotkey manager's routing, so
	// it is enough on its own for the hook to be installed.
	hookwatch::setNeeded(keyboardHookState, true);
	return startHookThread();
}

void KeyboardHook::release() {
	gOwnership.drop();
	if (gOwnership.needed() || taskbarShortcutFilterEnabled()) return;

	stopHookThread();
	hookwatch::setNeeded(keyboardHookState, false);
}

bool KeyboardHook::isRunning() {
	return keyboardHookState.active && gThreadId.load() != 0 && !gStopRequested.load();
}
bool KeyboardHook::foregroundBlocked() { return foregroundIsBlocked(); }
uint64_t KeyboardHook::activeHookGeneration() { return gHookGeneration.load(); }
bool KeyboardHook::superDown() {
	return gWinDown.load() || isDown(VK_LWIN) || isDown(VK_RWIN);
}

void KeyboardHook::setSnapshot(std::shared_ptr<const HookSnapshot> snapshot) {
	gSnapshot.store(std::move(snapshot), std::memory_order_release);
}

void KeyboardHook::noteSuperChord() { gSuppressLoneSuperTap.store(true); }
void KeyboardHook::setEscapeFilter(bool (*filter)()) { gEscapeFilter.store(filter, std::memory_order_release); }

void KeyboardHook::setSuperPressedCallback(void (*callback)()) {
	gSuperPressed.store(callback, std::memory_order_release);
}

bool KeyboardHook::armInputObserver(
    HWND target,
    UINT message,
    UINT resetMessage,
    std::vector<uint8_t>& keys
) {
	auto threadId = gThreadId.load();
	auto exited = gExitEvent.load();
	if (!isRunning() || threadId == 0 || exited == nullptr) return false;

	auto request = std::make_shared<InputObserverRequest>();
	request->target = target;
	request->keyMessage = message;
	request->resetMessage = resetMessage;
	request->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!request->done) return false;
	gObserverRequest.store(request);
	if (!PostThreadMessageW(threadId, WM_QS_ARM_OBSERVER, 0, 0)) {
		gObserverRequest.store(nullptr);
		return false;
	}

	HANDLE handles[] = {request->done, exited};
	auto wait = WaitForMultipleObjects(2, handles, FALSE, OBSERVER_TIMEOUT_MS);
	using State = InputObserverRequest::State;
	if (wait != WAIT_OBJECT_0) {
		if (request->state.exchange(State::Cancelled) == State::Armed) clearInputObserver();
		gObserverRequest.store(nullptr);
		return false;
	}
	keys = std::move(request->keys);
	return true;
}

void KeyboardHook::clearInputObserver() { gInputObserver.store(nullptr, std::memory_order_release); }

bool KeyboardHook::sendKey(uint8_t vk) {
	auto scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
	auto extended = isExtendedKey(vk);

	std::array<INPUT, 2> inputs {
	    keyInput(vk, false, scan, extended),
	    keyInput(vk, true, scan, extended),
	};

	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

bool KeyboardHook::sendChord(uint8_t mods, uint8_t vk, int times) {
	struct Group {
		uint8_t bit;
		uint8_t left;
		uint8_t right;
	};

	static constexpr std::array<Group, 4> groups {{
	    {.bit = ModCtrl, .left = VK_LCONTROL, .right = VK_RCONTROL},
	    {.bit = ModAlt, .left = VK_LMENU, .right = VK_RMENU},
	    {.bit = ModShift, .left = VK_LSHIFT, .right = VK_RSHIFT},
	    {.bit = ModSuper, .left = VK_LWIN, .right = VK_RWIN},
	}};

	std::vector<INPUT> lift;
	std::vector<INPUT> press;
	std::vector<INPUT> restore;
	auto masked = false;

	for (const auto& group: groups) {
		if ((mods & group.bit) != 0) {
			if (!isDown(group.left) && !isDown(group.right)) {
				press.push_back(chordInput(group.left, false));
			}

			continue;
		}

		for (auto key: {group.left, group.right}) {
			if (!isDown(key)) continue;
			lift.push_back(chordInput(key, true));
			restore.push_back(chordInput(key, false));
			masked = masked || group.bit == ModSuper || group.bit == ModAlt;
		}
	}

	std::vector<INPUT> inputs;
	auto mask = [&inputs]() {
		inputs.push_back(keyInput(KeyboardHook::MASK_KEY, false));
		inputs.push_back(keyInput(KeyboardHook::MASK_KEY, true));
	};

	if (masked) mask();
	inputs.insert(inputs.end(), lift.begin(), lift.end());
	inputs.insert(inputs.end(), press.begin(), press.end());

	for (auto i = 0; i < times; i++) {
		inputs.push_back(chordInput(vk, false));
		inputs.push_back(chordInput(vk, true));
	}

	for (auto input = press.rbegin(); input != press.rend(); ++input) {
		input->ki.dwFlags |= KEYEVENTF_KEYUP;
		inputs.push_back(*input);
	}

	inputs.insert(inputs.end(), restore.begin(), restore.end());
	if (masked) mask();

	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

} // namespace qs::windows::hotkeys
