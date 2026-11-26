#include "native_desktop_accessor.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <objectarray.h>
#include <qcoreapplication.h>
#include <qhash.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qscopedvaluerollback.h>
#include <servprov.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include "desktop_recovery.hpp"
#include "native_desktop_interfaces.hpp"
#include "util.hpp"

namespace qs::windows {
namespace {
Q_LOGGING_CATEGORY(logNativeDesktops, "quickshell.windows.desktops.native", QtWarningMsg);
using Microsoft::WRL::ComPtr;

using namespace native_desktop;

constexpr GUID immersiveShell =
    {0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
constexpr GUID desktopService =
    {0xc5e0cdca, 0x7b6e, 0x41b2, {0x9f, 0xc4, 0xd9, 0x39, 0x75, 0xcc, 0x46, 0x7b}};
constexpr GUID publicManager =
    {0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
// CLSID_VirtualDesktopPinnedApps, the immersive shell service for PinnedApps.
constexpr GUID pinnedAppsService =
    {0xb5a399e7, 0x1c87, 0x46b8, {0x88, 0xe9, 0xfc, 0x57, 0x47, 0xb1, 0x71, 0xbd}};

// Catch at the COM boundary, not around the whole accessor operation: an SEH
// unwind under /EHsc would otherwise skip local ComPtr destructors.
template <typename F>
bool sehCall(F&& call, HRESULT& result) {
	__try
	{
		result = call();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

template <typename T>
bool release(ComPtr<T>& pointer) {
	auto* detached = pointer.Detach();
	if (!detached) return true;
	HRESULT ignored = S_OK;
	// Detach first, so even a broken Release is attempted only once and cannot
	// leave a stale interface in the provider.
	return sehCall(
	    [detached] {
		    detached->Release();
		    return S_OK;
	    },
	    ignored
	);
}

template <typename T>
bool release(QList<ComPtr<T>>& pointers) {
	bool ok = true;
	for (auto& pointer: pointers) ok = release(pointer) && ok;
	pointers.clear();
	return ok;
}

#ifdef QS_TEST
struct TestViewsSlot {
	std::mutex mutex;
	ViewCollection* views = nullptr;

	void set(ViewCollection* next) {
		std::lock_guard lock(this->mutex);
		this->views = next;
	}

	ViewCollection* addRef() {
		std::lock_guard lock(this->mutex);
		if (this->views) this->views->AddRef();
		return this->views;
	}
};
#endif

bool isDisconnectError(HRESULT hr) {
	return hr == RPC_E_DISCONNECTED || hr == CO_E_OBJNOTCONNECTED
	    || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
	    || hr == RPC_E_SERVER_DIED || hr == RPC_E_SERVER_DIED_DNE
	    || hr == RPC_E_CONNECTION_TERMINATED;
}

DWORD liveExplorerTaskbarPid() {
	const auto selfPid = GetCurrentProcessId();
	HWND taskbar = nullptr;
	while ((taskbar = FindWindowExW(nullptr, taskbar, L"Shell_TrayWnd", nullptr)) != nullptr) {
		if (GetPropW(taskbar, TRAY_HOOK_PROP) != nullptr) continue;
		DWORD pid = 0;
		GetWindowThreadProcessId(taskbar, &pid);
		if (pid != 0 && pid != selfPid) return pid;
	}
	return 0;
}

struct Provider {
#ifdef QS_TEST
	NativeDesktopFactory factory;
	std::function<bool(HWND)> windowValidator;
	TestViewsSlot* testViews = nullptr;
#endif
	ComPtr<DesktopManager> manager;
	ComPtr<ViewCollection> views;
	ComPtr<IVirtualDesktopManager> publicApi;
	QList<ComPtr<Desktop>> snapshot;
	QList<GUID> ids;
	int current = -1;
	bool structuredException = false;
	bool privateEnabled = true;
	bool desktopRefreshed = false;
	DesktopAvailability availability = DesktopAvailability::MissingProvider;
	HRESULT lastError = S_OK;
	HRESULT operationError = S_OK;

	void dropPrivate() {
#ifdef QS_TEST
		if (this->testViews) this->testViews->set(nullptr);
#endif
		release(this->snapshot);
		this->ids.clear();
		this->current = -1;
		release(this->views);
		release(this->manager);
	}

	void drop() {
		this->dropPrivate();
		release(this->publicApi);
	}

	void exception(const char* operation) {
		qCWarning(logNativeDesktops) << operation << "raised a structured exception";
		this->structuredException = true;
		this->lastError = E_UNEXPECTED;
		this->operationError = E_UNEXPECTED;
		this->availability = DesktopAvailability::Disconnected;
		this->drop();
	}

	template <typename T>
	bool releaseLocal(T& pointer) {
		if (release(pointer)) return true;
		this->exception("Local COM Release");
		return false;
	}

	template <typename F>
	bool invoke(F&& call, const char* operation) {
		HRESULT hr = S_OK;
		if (sehCall(std::forward<F>(call), hr)) return this->check(hr, operation);
		this->exception(operation);
		return false;
	}

	bool check(HRESULT hr, const char* operation) {
		if (SUCCEEDED(hr)) return true;
		this->lastError = hr;
		this->operationError = hr;
		qCWarning(logNativeDesktops) << operation << "refused, HRESULT" << Qt::hex << hr;
		if (isDisconnectError(hr)) {
			this->drop();
			this->availability = DesktopAvailability::Disconnected;
		}
		return false;
	}

	bool acquire() {
		this->drop();
		this->lastError = S_OK;
		this->structuredException = false;
		this->availability = DesktopAvailability::MissingProvider;
		ComPtr<IServiceProvider> shell;
		bool ok;
		bool publicAcquired = false;
#ifdef QS_TEST
		if (this->factory) {
			ok = this->invoke(
			    [&] {
				    return this->factory(
				        this->manager.GetAddressOf(),
				        this->views.GetAddressOf(),
				        this->publicApi.GetAddressOf()
				    );
			    },
			    "Test desktop acquisition"
			);
			publicAcquired = ok && this->publicApi;
			if (ok && this->testViews) this->testViews->set(this->views.Get());
		} else
#endif
		{
			// Public window queries do not depend on the private ABI or its
			// opt-in. Retain them when private acquisition alone is refused.
			const auto publicOk = this->invoke(
			    [&] {
				    return CoCreateInstance(
				        publicManager,
				        nullptr,
				        CLSCTX_INPROC_SERVER,
				        IID_PPV_ARGS(&this->publicApi)
				    );
			    },
			    "PublicDesktopManager"
			);
			publicAcquired = publicOk && this->publicApi;
			const auto privateOk =
			    (!this->privateEnabled
			     || (this->invoke(
			             [&] {
				             return CoCreateInstance(
				                 immersiveShell,
				                 nullptr,
				                 CLSCTX_LOCAL_SERVER,
				                 IID_PPV_ARGS(&shell)
				             );
			             },
			             "ImmersiveShell"
			         )
			         && this->invoke(
			             [&] {
				             return shell->QueryService(desktopService, IID_PPV_ARGS(&this->manager));
			             },
			             "DesktopManager 24H2 GUID"
			         )
			         && this->invoke(
			             [&] {
				             return shell->QueryService(
				                 __uuidof(ViewCollection),
				                 IID_PPV_ARGS(&this->views)
				             );
			             },
			             "ApplicationViewCollection GUID"
			         )));
			ok = publicOk && privateOk;
		}
		ok = ok && (!this->privateEnabled || this->refresh());
		// Finish local cleanup before publishing acquisition success, including
		// timer-driven acquisitions which have no outer facade SEH guard.
		ok = this->releaseLocal(shell) && ok;
		if (!ok) {
			if (!publicAcquired || this->structuredException
			    || this->availability == DesktopAvailability::Disconnected)
				this->drop();
			else this->dropPrivate();
			if (this->lastError == E_ACCESSDENIED) {
				this->availability = DesktopAvailability::PermissionDenied;
			} else if (this->structuredException || isDisconnectError(this->lastError)) {
				this->availability = DesktopAvailability::Disconnected;
			} else {
				this->availability = DesktopAvailability::MissingProvider;
			}
		} else {
			this->availability = DesktopAvailability::Available;
		}
		return ok;
	}

	bool refresh() {
		if (!this->manager) return false;
		ComPtr<IObjectArray> array;
		QList<ComPtr<Desktop>> desktops;
		ComPtr<Desktop> desktop, active;
		QList<GUID> newIds;
		// Keep every local COM owner outside this body, so every early return
		// reaches guarded cleanup before an operation can report success.
		bool ok = [&] {
			UINT count = 0;
			if (!this->invoke([&] { return this->manager->GetDesktops(&array); }, "GetDesktops")
			    || !this->invoke([&] { return array->GetCount(&count); }, "GetDesktopCount"))
				return false;
			for (UINT i = 0; i < count; ++i) {
				GUID id {};
				if (!this->invoke([&] { return array->GetAt(i, IID_PPV_ARGS(&desktop)); }, "GetDesktop")
				    || !this->invoke([&] { return desktop->GetId(&id); }, "GetDesktopId"))
					return false;
				desktops.append(std::move(desktop));
				newIds.append(id);
			}
			int activeIndex = -1;
			if (count != 0) {
				GUID activeId {};
				if (!this->invoke(
				        [&] { return this->manager->GetCurrentDesktop(&active); },
				        "GetCurrentDesktop"
				    )
				    || !this->invoke([&] { return active->GetId(&activeId); }, "GetCurrentDesktopId"))
					return false;
				for (qsizetype i = 0; i < newIds.size(); ++i) {
					if (IsEqualGUID(newIds[i], activeId)) activeIndex = static_cast<int>(i);
				}
				if (activeIndex < 0) return false;
			}
			if (!this->releaseLocal(this->snapshot)) return false;
			this->snapshot = std::move(desktops);
			this->ids = std::move(newIds);
			this->current = activeIndex;
			return true;
		}();
		ok = this->releaseLocal(active) && ok;
		ok = this->releaseLocal(desktop) && ok;
		ok = this->releaseLocal(desktops) && ok;
		ok = this->releaseLocal(array) && ok;
		if (ok) this->desktopRefreshed = true;
		return ok;
	}

	bool validWindow(HWND window) {
#ifdef QS_TEST
		if (this->windowValidator) return this->windowValidator(window);
#endif
		return IsWindow(window);
	}

	Desktop* desktop(int index) {
		if (!this->manager || index < 0 || index >= this->snapshot.size()) return nullptr;
		return this->snapshot[index].Get();
	}
};

thread_local Provider* activeProvider = nullptr;
Provider& provider() {
	// Constructed and destroyed inside the worker's COM apartment.
	Q_ASSERT(activeProvider);
	return *activeProvider;
}

int desktopCount() {
	auto& p = provider();
	if (p.refresh()) return static_cast<int>(p.ids.size());
	if (p.availability != DesktopAvailability::Disconnected) {
		p.availability = p.lastError == E_ACCESSDENIED ? DesktopAvailability::PermissionDenied
		                                               : DesktopAvailability::MissingProvider;
		p.dropPrivate();
	}
	return -1;
}

int switchDesktop(int index) {
	auto& p = provider();
	auto desktop = p.desktop(index);
	return desktop && p.invoke([&] { return p.manager->SwitchDesktop(desktop); }, "SwitchDesktop")
	         ? 0
	         : -1;
}

int createDesktop() {
	auto& p = provider();
	ComPtr<Desktop> desktop;
	const auto ok =
	    p.manager && p.invoke([&] { return p.manager->CreateDesktop(&desktop); }, "CreateDesktop");
	if (!p.releaseLocal(desktop) || !ok) return -1;
	return desktopCount() - 1;
}

int moveWindow(HWND window, int index) {
	auto& p = provider();
	p.lastError = S_OK;
	auto desktop = p.desktop(index);
	if (!desktop || !p.validWindow(window)) return -1;
	ApplicationView* view = nullptr;
	const auto gotView =
	    p.invoke([&] { return p.views->GetViewForHwnd(window, &view); }, "GetViewForHwnd");
	// Failed calls do not transfer ownership of a potentially half-written out
	// pointer, including a structured exception in GetViewForHwnd.
	if (!gotView || !view) return -1;
	// The returned IApplicationView begins with IUnknown, even though its full
	// interface derives from IInspectable.
	ComPtr<IUnknown> unknown;
	unknown.Attach(reinterpret_cast<IUnknown*>(view));
	BOOL movable = FALSE;
	auto ok =
	    p.invoke(
	        [&] { return p.manager->CanViewMoveDesktops(view, &movable); },
	        "CanViewMoveDesktops"
	    )
	    && movable
	    && p.invoke([&] { return p.manager->MoveViewToDesktop(view, desktop); }, "MoveViewToDesktop");
	return p.releaseLocal(unknown) && ok ? 0 : -1;
}

using Clock = std::chrono::steady_clock;
constexpr auto callBound = std::chrono::milliseconds(100);
constexpr auto stuckBound = std::chrono::milliseconds(5000);
constexpr int workerLimit = 4;
constexpr qsizetype windowLimit = 4096;

struct WindowSnapshot {
	GUID id {};
	int onCurrent = -1;
	DWORD pid = 0;
	DWORD thread = 0;
	bool operator==(const WindowSnapshot& other) const {
		return IsEqualGUID(this->id, other.id) && this->onCurrent == other.onCurrent
		    && this->pid == other.pid && this->thread == other.thread;
	}
};

struct Snapshot {
	QList<GUID> ids;
	int current = -1;
	QHash<quintptr, WindowSnapshot> windows;
	DesktopAvailability availability = DesktopAvailability::MissingProvider;
	HRESULT error = S_OK;
	HRESULT operationError = S_OK;
	int result = -1;
	quint64 sequence = 0;
	bool publicReady = false;
	bool desktopRead = false;
	bool windowRead = false;
};

struct Request {
	HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	~Request() {
		if (this->finished) CloseHandle(this->finished);
	}
	std::function<int()> operation;
	std::function<void()> completed;
	QList<quintptr> windows;
	bool queryWindows = false;
	std::promise<Snapshot> completion;
	Clock::time_point submitted = Clock::now();
	std::atomic<Clock::duration::rep> started = 0;
	quint64 sequence = 0;

	bool stalled(std::chrono::milliseconds bound) const {
		const auto ticks = this->started.load();
		const auto since = ticks ? Clock::time_point(Clock::duration(ticks)) : this->submitted;
		return Clock::now() - since >= bound;
	}
};

// Intentionally process-lived. A retired apartment may still be inside RPC
// during static destruction. It owns no QObject or GUI-owned storage.
std::atomic<int>& liveWorkers() {
	static auto* count = new std::atomic<int>(0);
	return *count;
}

std::atomic<int>& livePinWorkers() {
	static auto* count = new std::atomic<int>(0);
	return *count;
}

struct Worker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<Request>> queue;
	bool stopping = false;
	std::promise<void> exited;
	std::shared_future<void> exit = exited.get_future().share();
#ifdef QS_TEST
	TestViewsSlot testViews;
#endif

	void stop() {
		{
			std::lock_guard lock(this->mutex);
			this->stopping = true;
			this->queue.clear();
		}
		this->wake.notify_one();
	}

	std::future<Snapshot> submit(const std::shared_ptr<Request>& request) {
		auto result = request->completion.get_future();
		{
			std::lock_guard lock(this->mutex);
			// The GUI permits at most one refresh and one command at a time.
			Q_ASSERT(this->queue.size() < 2);
			if (!this->stopping) this->queue.push_back(request);
		}
		this->wake.notify_one();
		return result;
	}

	void
	run(bool privateEnabled
#ifdef QS_TEST
	    ,
	    NativeDesktopFactory factory,
	    std::function<bool(HWND)> validator
#endif
	) {
		const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		Provider p;
		p.privateEnabled = privateEnabled;
		if (FAILED(apartment)) p.lastError = apartment;
#ifdef QS_TEST
		p.factory = std::move(factory);
		p.windowValidator = std::move(validator);
		p.testViews = &this->testViews;
#endif
		activeProvider = &p;
		for (;;) {
			std::shared_ptr<Request> request;
			{
				std::unique_lock lock(this->mutex);
				this->wake.wait(lock, [&] { return this->stopping || !this->queue.empty(); });
				if (this->stopping) break;
				request = std::move(this->queue.front());
				this->queue.pop_front();
			}
			request->started.store(Clock::now().time_since_epoch().count());
			Snapshot snapshot;
			snapshot.sequence = request->sequence;
			try {
				p.operationError = S_OK;
				p.desktopRefreshed = false;
				snapshot.result = FAILED(apartment) ? -1 : request->operation();
				snapshot.operationError = FAILED(apartment) ? apartment : p.operationError;
				for (const auto value: request->windows) {
					auto hwnd = reinterpret_cast<HWND>(value);
					WindowSnapshot window;
					window.thread = GetWindowThreadProcessId(hwnd, &window.pid);
					snapshot.windows.insert(value, window);
					if (!p.publicApi || !p.validWindow(hwnd)) continue;
					// A window disappearing is not a provider failure. Disconnects
					// and structured exceptions still invalidate the apartment.
					if (!p.invoke(
					        [&] { return p.publicApi->GetWindowDesktopId(hwnd, &window.id); },
					        "GetWindowDesktopId"
					    ))
						window.id = {};
					BOOL current = TRUE;
					if (p.publicApi
					    && p.invoke(
					        [&] { return p.publicApi->IsWindowOnCurrentVirtualDesktop(hwnd, &current); },
					        "IsWindowOnCurrentDesktop"
					    ))
						window.onCurrent = current != FALSE;
					DWORD pid = 0;
					if (window.thread == GetWindowThreadProcessId(hwnd, &pid) && window.pid == pid)
						snapshot.windows.insert(value, window);
				}
				snapshot.ids = p.ids;
				snapshot.current = p.current;
				snapshot.availability = p.availability;
				snapshot.error = p.lastError;
				snapshot.publicReady = p.publicApi != nullptr;
				snapshot.desktopRead = p.desktopRefreshed;
				snapshot.windowRead = request->queryWindows;
			} catch (...) {
				snapshot.availability = DesktopAvailability::Disconnected;
				snapshot.error = E_UNEXPECTED;
				snapshot.operationError = E_UNEXPECTED;
			}
			request->completion.set_value(std::move(snapshot));
			SetEvent(request->finished);
			if (request->completed) request->completed();
		}
		// Release and even CoUninitialize may block. Both belong to this
		// retired worker and count against the cap until they finish.
		p.drop();
		activeProvider = nullptr;
		if (SUCCEEDED(apartment)) CoUninitialize();
		--liveWorkers();
		this->exited.set_value();
	}
};

struct PinResult {
	// 0: requested pin/unpin confirmed, -1: refused, -2: unavailable.
	int result = -2;
	HRESULT error = S_OK;
	bool serviceAvailable = true;
	bool disconnected = false;
	bool optionalPinnedAppsFaulted = false;
	// True whenever the window is confirmed pinned or a pin mutation succeeded
	// without a confirmed UnpinView rollback (for example, late cancel during
	// Release or an UnpinView refusal/disconnect/fault).
	bool viewPinned = false;
	// True when PinView was invoked (or IsViewPinned returned non-S_OK success)
	// and a disconnect, fault, or non-S_OK result left the final pin state unknown.
	bool pinAttemptUncertain = false;
	// True when cancellation prevented UnpinView from being invoked on a pinned view.
	bool cancelledBeforeUnpin = false;
	// True only when Explorer confirmed the window is not pinned (IsViewPinned
	// returned S_OK with FALSE or UnpinView returned S_OK).
	bool unpinned = false;
	// Explorer PID verified around COM service acquisition for this pin worker.
	DWORD explorerPid = 0;
};

struct PinRequest {
	HWND window = nullptr;
	bool unpin = false;
	std::function<void()> completed;
	std::promise<PinResult> completion;
	Clock::time_point submitted = Clock::now();
	std::atomic<Clock::duration::rep> started = 0;
	std::atomic<bool> cancelled {false};
	std::atomic<bool> settled {false};

	bool stalled(std::chrono::milliseconds bound) const {
		const auto ticks = this->started.load();
		const auto since = ticks ? Clock::time_point(Clock::duration(ticks)) : this->submitted;
		return Clock::now() - since >= bound;
	}
};

// Dedicated apartment for optional panel pinning. Isolated from desktop
// switches, moves, creations and refreshes so a slow, faulted or stalled
// PinnedApps call can never delay or retire desktop control.
struct PinProvider {
#ifdef QS_TEST
	bool testMode = false;
	std::function<bool(HWND)> windowValidator;
	std::function<DWORD()> explorerPidProvider;
	PinnedApps* testPinnedApps = nullptr;
	IServiceProvider* pinnedAppsShell = nullptr;
#endif
	ComPtr<ViewCollection> views;
	ComPtr<PinnedApps> pinnedApps;
	bool acquired = false;
	bool serviceAvailable = true;
	bool disconnected = false;
	bool optionalPinnedAppsFaulted = false;
	HRESULT lastError = S_OK;
	DWORD explorerPid = 0;

	[[nodiscard]] DWORD currentExplorerPid() const {
#ifdef QS_TEST
		if (this->explorerPidProvider) return this->explorerPidProvider();
#endif
		return liveExplorerTaskbarPid();
	}

	void drop() {
		release(this->pinnedApps);
		release(this->views);
	}

	void fault(const char* operation) {
		qCWarning(logNativeDesktops) << operation << "raised a structured exception";
		this->optionalPinnedAppsFaulted = true;
		this->serviceAvailable = false;
		this->lastError = E_UNEXPECTED;
		this->drop();
	}

	template <typename T>
	bool releaseLocal(T& pointer) {
		if (release(pointer)) return true;
		this->fault("Local pin COM Release");
		return false;
	}

	template <typename F>
	bool invoke(F&& call, const char* operation, HRESULT* outHr = nullptr) {
		HRESULT hr = S_OK;
		if (outHr) *outHr = E_UNEXPECTED;
		if (!sehCall(std::forward<F>(call), hr)) {
			this->fault(operation);
			return false;
		}
		if (outHr) *outHr = hr;
		if (SUCCEEDED(hr)) return true;
		this->lastError = hr;
		qCWarning(logNativeDesktops) << operation << "refused, HRESULT" << Qt::hex << hr;
		if (isDisconnectError(hr)) {
			this->disconnected = true;
			this->drop();
		}
		return false;
	}

	bool acquirePinnedApps(IServiceProvider* shell) {
		if (!shell) {
			this->serviceAvailable = false;
			return false;
		}
		HRESULT hr = S_OK;
		PinnedApps* raw = nullptr;
		if (!sehCall(
		        [&] {
			        return shell->QueryService(
			            pinnedAppsService,
			            __uuidof(PinnedApps),
			            reinterpret_cast<void**>(&raw)
			        );
		        },
		        hr
		    ))
		{
			// Never adopt a half-written out pointer after a structured exception.
			this->fault("PinnedApps 24H2 GUID");
			return false;
		}
		if (SUCCEEDED(hr) && raw != nullptr) {
			this->pinnedApps.Attach(raw);
			return true;
		}
		// QueryService does not transfer ownership on failure.
		this->lastError = FAILED(hr) ? hr : E_NOINTERFACE;
		if (isDisconnectError(hr)) {
			this->disconnected = true;
			this->drop();
			return false;
		}
		this->serviceAvailable = false;
		qCWarning(logNativeDesktops) << "Desktop pinning unavailable, HRESULT" << Qt::hex << hr
		                             << "; panels will follow the current desktop";
		return false;
	}

	void ensureAcquired() {
		if (this->acquired) return;
		this->acquired = true;
		const auto pidBefore = this->currentExplorerPid();
#ifdef QS_TEST
		if (this->testMode) {
			if (!this->views) {
				this->serviceAvailable = false;
				return;
			}
			if (this->pinnedAppsShell != nullptr) {
				this->acquirePinnedApps(this->pinnedAppsShell);
			} else if (this->testPinnedApps != nullptr) {
				this->pinnedApps = this->testPinnedApps;
			} else {
				this->serviceAvailable = false;
			}
			const auto pidAfter = this->currentExplorerPid();
			if (pidBefore != 0 && pidAfter != 0 && pidBefore != pidAfter && !this->disconnected
			    && !this->optionalPinnedAppsFaulted)
			{
				this->explorerPid = pidBefore;
				this->disconnected = true;
				this->lastError = RPC_E_DISCONNECTED;
				this->drop();
				return;
			}
			this->explorerPid = pidAfter != 0 ? pidAfter : pidBefore;
			return;
		}
#endif
		IServiceProvider* rawShell = nullptr;
		ViewCollection* rawViews = nullptr;
		ComPtr<IServiceProvider> shell;
		auto ok = this->invoke(
		    [&] {
			    return CoCreateInstance(
			        immersiveShell,
			        nullptr,
			        CLSCTX_LOCAL_SERVER,
			        __uuidof(IServiceProvider),
			        reinterpret_cast<void**>(&rawShell)
			    );
		    },
		    "Pin ImmersiveShell"
		);
		if (ok && rawShell != nullptr) {
			shell.Attach(rawShell);
		} else {
			ok = false;
		}
		if (ok) {
			ok = this->invoke(
			    [&] {
				    return shell->QueryService(
				        __uuidof(ViewCollection),
				        __uuidof(ViewCollection),
				        reinterpret_cast<void**>(&rawViews)
				    );
			    },
			    "Pin ApplicationViewCollection GUID"
			);
			if (ok && rawViews != nullptr) {
				this->views.Attach(rawViews);
			} else {
				ok = false;
			}
		}
		if (ok) ok = this->acquirePinnedApps(shell.Get());
		ok = this->releaseLocal(shell) && ok;
		const auto pidAfter = this->currentExplorerPid();
		if (ok && pidBefore != 0 && pidAfter != 0 && pidBefore != pidAfter) {
			// Explorer restarted while acquiring the pin COM services; abort
			// before mutating pin state so this worker never tags a replacement
			// Explorer session with a stale PID.
			this->explorerPid = pidBefore;
			this->disconnected = true;
			this->lastError = RPC_E_DISCONNECTED;
			this->drop();
			return;
		}
		this->explorerPid = pidAfter != 0 ? pidAfter : pidBefore;
		if (!ok && !this->disconnected && !this->optionalPinnedAppsFaulted) {
			this->serviceAvailable = false;
			this->drop();
		}
	}

	bool validWindow(HWND window) {
#ifdef QS_TEST
		if (this->windowValidator) return this->windowValidator(window);
#endif
		return IsWindow(window);
	}

	PinResult pin(HWND window, bool unpin, const std::atomic<bool>& cancelled) {
		this->lastError = S_OK;
		PinResult out;
		out.explorerPid = this->explorerPid;
		if (cancelled.load()) {
			out.result = -2;
			return out;
		}
		this->ensureAcquired();
		out.explorerPid = this->explorerPid;
		out.serviceAvailable = this->serviceAvailable;
		out.disconnected = this->disconnected;
		out.optionalPinnedAppsFaulted = this->optionalPinnedAppsFaulted;
		out.error = this->lastError;
		if (cancelled.load() || this->optionalPinnedAppsFaulted || this->disconnected
		    || !this->serviceAvailable || !this->pinnedApps || !this->views)
		{
			out.result = -2;
			return out;
		}
		if (!this->validWindow(window)) {
			out.result = -1;
			return out;
		}
		ApplicationView* view = nullptr;
		const auto gotView =
		    this->invoke([&] { return this->views->GetViewForHwnd(window, &view); }, "GetViewForHwnd");
		if (!gotView || !view) {
			out.disconnected = this->disconnected;
			out.optionalPinnedAppsFaulted = this->optionalPinnedAppsFaulted;
			out.serviceAvailable = this->serviceAvailable;
			out.error = this->lastError;
			out.result = (this->disconnected || this->optionalPinnedAppsFaulted) ? -2 : -1;
			return out;
		}
		ComPtr<IUnknown> unknown;
		unknown.Attach(reinterpret_cast<IUnknown*>(view));
		BOOL pinned = FALSE;
		HRESULT isPinnedHr = E_UNEXPECTED;
		// Query first so a repeated call never toggles anything. Never start
		// PinView or UnpinView after the GUI has already retired or canceled this request.
		auto ok = !cancelled.load()
		       && this->invoke(
		           [&] { return this->pinnedApps->IsViewPinned(view, &pinned); },
		           "IsViewPinned",
		           &isPinnedHr
		       );
		const auto isPinnedExactOk = ok && isPinnedHr == S_OK;
		if (unpin) {
			auto unpinnedConfirmed = false;
			auto viewPinned = false;
			auto cancelledBeforeUnpin = false;
			if (isPinnedExactOk && !pinned) {
				unpinnedConfirmed = true;
			} else if (ok) {
				viewPinned = (isPinnedExactOk && pinned != FALSE);
				if (cancelled.load()) {
					cancelledBeforeUnpin = true;
					ok = false;
				} else {
					HRESULT unpinHr = E_UNEXPECTED;
					const auto didUnpin = this->invoke(
					    [&] { return this->pinnedApps->UnpinView(view); },
					    "UnpinView",
					    &unpinHr
					);
					if (didUnpin && unpinHr == S_OK) {
						viewPinned = false;
						unpinnedConfirmed = true;
					} else if (didUnpin) {
						// S_FALSE or another non-S_OK success does not prove Explorer
						// cleared the pin; re-check IsViewPinned before clearing the fence.
						BOOL stillPinnedAfter = TRUE;
						HRESULT recheckHr = E_UNEXPECTED;
						const auto rechecked = this->invoke(
						    [&] { return this->pinnedApps->IsViewPinned(view, &stillPinnedAfter); },
						    "IsViewPinned",
						    &recheckHr
						);
						if (rechecked && recheckHr == S_OK && !stillPinnedAfter) {
							viewPinned = false;
							unpinnedConfirmed = true;
						} else {
							if (rechecked && recheckHr == S_OK && stillPinnedAfter) viewPinned = true;
							ok = false;
						}
					} else {
						ok = false;
					}
				}
			}
			const auto released = this->releaseLocal(unknown);
			out.disconnected = this->disconnected;
			out.optionalPinnedAppsFaulted = this->optionalPinnedAppsFaulted;
			out.serviceAvailable = this->serviceAvailable;
			out.viewPinned = viewPinned;
			out.cancelledBeforeUnpin = cancelledBeforeUnpin;
			out.unpinned = unpinnedConfirmed;
			out.error = this->lastError;
			if (unpinnedConfirmed) {
				out.result = 0;
			} else if (this->disconnected || this->optionalPinnedAppsFaulted || !this->pinnedApps) {
				out.result = -2;
			} else if (cancelledBeforeUnpin) {
				out.result = -2;
			} else if (viewPinned) {
				out.result = -1;
			} else if (cancelled.load()) {
				out.result = -2;
			} else {
				out.result = (ok && released) ? 0 : -1;
			}
			return out;
		}
		auto pinnedByCall = false;
		auto pinAttemptUncertain = false;
		auto unpinnedConfirmed = (isPinnedExactOk && !pinned);
		if (isPinnedExactOk && !pinned) {
			if (cancelled.load()) {
				ok = false;
			} else {
				unpinnedConfirmed = false;
				HRESULT pinHr = E_UNEXPECTED;
				ok = this->invoke([&] { return this->pinnedApps->PinView(view); }, "PinView", &pinHr);
				if (ok && pinHr == S_OK) {
					pinnedByCall = true;
				} else if (ok) {
					BOOL pinnedAfter = FALSE;
					HRESULT recheckHr = E_UNEXPECTED;
					const auto rechecked = this->invoke(
					    [&] { return this->pinnedApps->IsViewPinned(view, &pinnedAfter); },
					    "IsViewPinned",
					    &recheckHr
					);
					if (rechecked && recheckHr == S_OK) {
						if (pinnedAfter) {
							pinnedByCall = true;
						} else {
							unpinnedConfirmed = true;
							ok = false;
						}
					} else {
						pinAttemptUncertain = true;
						ok = false;
					}
				} else if (this->disconnected || this->optionalPinnedAppsFaulted) {
					// PinView was invoked and lost its reply or faulted; Explorer may
					// have applied the pin before the failure.
					pinAttemptUncertain = true;
				}
			}
		} else if (ok && !isPinnedExactOk) {
			pinAttemptUncertain = true;
			ok = false;
		}
		auto viewPinned = (isPinnedExactOk && ok && pinned != FALSE) || pinnedByCall;
		if (pinnedByCall && cancelled.load()) {
			// Cancellation raced a PinView that had already started: roll back the
			// pin while we still hold the view. Only clear viewPinned when UnpinView
			// confirms rollback with S_OK (or a follow-up IsViewPinned confirms FALSE).
			// If UnpinView disconnects, faults, or returns a non-S_OK success whose
			// recheck is inconclusive, Explorer may already have removed the pin:
			// clear viewPinned and mark pinAttemptUncertain so embedding stays fenced
			// without reporting a confirmed Pinned state.
			HRESULT unpinHr = E_UNEXPECTED;
			const auto unpinned = this->invoke(
			    [&] { return this->pinnedApps->UnpinView(view); },
			    "UnpinView",
			    &unpinHr
			);
			if (unpinned && unpinHr == S_OK) {
				viewPinned = false;
				unpinnedConfirmed = true;
				ok = false;
			} else if (unpinned) {
				BOOL stillPinnedAfter = TRUE;
				HRESULT recheckHr = E_UNEXPECTED;
				const auto rechecked = this->invoke(
				    [&] { return this->pinnedApps->IsViewPinned(view, &stillPinnedAfter); },
				    "IsViewPinned",
				    &recheckHr
				);
				if (rechecked && recheckHr == S_OK && !stillPinnedAfter) {
					viewPinned = false;
					unpinnedConfirmed = true;
					ok = false;
				} else if (rechecked && recheckHr == S_OK && stillPinnedAfter) {
					viewPinned = true;
				} else {
					viewPinned = false;
					pinAttemptUncertain = true;
					ok = false;
				}
			} else if (this->disconnected || this->optionalPinnedAppsFaulted) {
				viewPinned = false;
				pinAttemptUncertain = true;
				ok = false;
			}
		}
		const auto released = this->releaseLocal(unknown);
		out.disconnected = this->disconnected;
		out.optionalPinnedAppsFaulted = this->optionalPinnedAppsFaulted;
		out.serviceAvailable = this->serviceAvailable;
		out.viewPinned = viewPinned;
		out.pinAttemptUncertain = pinAttemptUncertain;
		out.unpinned = !viewPinned && unpinnedConfirmed;
		out.error = this->lastError;
		if (this->disconnected || this->optionalPinnedAppsFaulted || !this->pinnedApps) {
			out.result = -2;
		} else if (out.viewPinned) {
			// Report Pinned honestly whenever the pin succeeded and was not rolled
			// back before Release (including cancellation arriving during Release
			// or an explicit UnpinView refusal leaving the pin intact).
			out.result = 0;
		} else if (out.pinAttemptUncertain) {
			out.result = -1;
		} else if (cancelled.load()) {
			out.result = -2;
		} else {
			out.result = (ok && released) ? 0 : -1;
		}
		return out;
	}
};

struct PinWorker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<PinRequest>> queue;
	bool stopping = false;
	std::promise<void> exited;
	std::shared_future<void> exit = exited.get_future().share();

	void stop() {
		{
			std::lock_guard lock(this->mutex);
			this->stopping = true;
			for (const auto& req: this->queue) {
				if (req) req->settled.store(true, std::memory_order_release);
			}
			this->queue.clear();
		}
		this->wake.notify_one();
	}

	std::future<PinResult> submit(const std::shared_ptr<PinRequest>& request) {
		auto result = request->completion.get_future();
		{
			std::lock_guard lock(this->mutex);
			Q_ASSERT(this->queue.empty());
			if (!this->stopping) this->queue.push_back(request);
			else request->settled.store(true, std::memory_order_release);
		}
		this->wake.notify_one();
		return result;
	}

	void run(
#ifdef QS_TEST
	    bool testMode,
	    ViewCollection* ownedViews,
	    std::function<bool(HWND)> validator,
	    std::function<DWORD()> explorerPidProvider,
	    PinnedApps* testPinnedApps,
	    IServiceProvider* pinnedAppsShell
#endif
	) {
		const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		PinProvider p;
#ifdef QS_TEST
		p.testMode = testMode;
		p.views.Attach(ownedViews);
		p.windowValidator = std::move(validator);
		p.explorerPidProvider = std::move(explorerPidProvider);
		p.testPinnedApps = testPinnedApps;
		p.pinnedAppsShell = pinnedAppsShell;
#endif
		for (;;) {
			std::shared_ptr<PinRequest> request;
			{
				std::unique_lock lock(this->mutex);
				this->wake.wait(lock, [&] { return this->stopping || !this->queue.empty(); });
				if (this->stopping) break;
				request = std::move(this->queue.front());
				this->queue.pop_front();
			}
			request->started.store(Clock::now().time_since_epoch().count());
			PinResult result;
			try {
				if (FAILED(apartment)) {
					result.result = -2;
					result.error = apartment;
					result.serviceAvailable = false;
				} else {
					result = p.pin(request->window, request->unpin, request->cancelled);
				}
			} catch (...) {
				result.result = -2;
				result.error = E_UNEXPECTED;
				result.serviceAvailable = false;
				result.optionalPinnedAppsFaulted = true;
			}
			request->completion.set_value(std::move(result));
			request->settled.store(true, std::memory_order_release);
			if (request->completed) request->completed();
		}
		p.drop();
		if (SUCCEEDED(apartment)) CoUninitialize();
		--livePinWorkers();
		this->exited.set_value();
	}
};

struct Channel {
	enum class Kind { Command, Acquisition, Probe };
	struct Pending {
		std::shared_ptr<Request> request;
		std::future<Snapshot> result;
		Kind kind = Kind::Command;
		bool queryWindows = false;
		bool publicOperation = false;
		HWND moveTarget = nullptr;
		bool switchCommand = false;
	};
	struct PinPending {
		std::shared_ptr<PinRequest> request;
		std::future<PinResult> result;
		HWND target = nullptr;
		quint64 requestId = 0;
		bool unpin = false;
		bool retiredOnDisconnect = false;
		DWORD explorerPid = 0;
	};
	std::shared_ptr<Worker> worker;
	std::shared_ptr<PinWorker> pinWorker;
	DWORD workerExplorerPid = 0;
	DWORD pinWorkerExplorerPid = 0;
	Snapshot snapshot;
	QSet<quintptr> watchedWindows;
	QPointer<QObject> context;
	std::function<void()> recovered;
	std::function<void()> updated;
	std::function<void(quint64, HWND, bool, DesktopAvailability, HRESULT)> moveCompleted;
	std::function<void(quint64, HWND, OwnWindowPin, bool, quint32)> pinCompleted;
	std::function<void(quint64, HWND, bool, bool, bool, bool, quint32)> pinDrained;
	std::function<void()> commandIdle;
	bool commandIdleScheduled = false;
	QTimer deadline;
	QTimer pinDeadline;
	std::atomic<bool> completionWake = false;
	std::atomic<bool> pinCompletionWake = false;
	std::optional<Pending> refresh;
	std::optional<Pending> pending;
	std::optional<PinPending> pin;
	std::vector<PinPending> drainingPins;
	QHash<quintptr, int> pendingPinDeliveries;
	quint64 pinDeliveryGeneration = 0;
	std::chrono::milliseconds stallLimit = stuckBound;
	std::chrono::milliseconds pinStallLimit = stuckBound;
	bool supported = false;
	bool privateEnabled = false;
	bool refusedBuild = false;
	bool skipOptionalPinnedApps = false;
	bool pinUnavailable = false;
	bool refreshAgain = false;
	bool waiting = false;
	quint64 sequence = 0;
	quint64 pinSequence = 0;
	quint64 applied = 0;
	quint64 desktopReadApplied = 0;
	quint64 windowReadApplied = 0;
	quint64 cancelledCommandSequence = 0;
	quint64 failedSwitchSequence = 0;
	HRESULT failedSwitchError = S_OK;
	quint64 windowRevision = 0;
#ifdef QS_TEST
	NativeDesktopFactory factory;
	std::function<bool(HWND)> validator;
	std::function<DWORD()> explorerPidProvider;
	PinnedApps* testPinnedApps = nullptr;
	IServiceProvider* pinnedAppsShell = nullptr;
#endif
	DesktopRecovery recovery {
	    [this] { return this->acquire(); },
	    [this] {
		    this->notify();
		    if (this->context && this->recovered) this->recovered();
	    }
	};

	[[nodiscard]] DWORD currentExplorerPid() const {
#ifdef QS_TEST
		if (this->explorerPidProvider) return this->explorerPidProvider();
#endif
		return liveExplorerTaskbarPid();
	}

	[[nodiscard]] DWORD currentWorkerExplorerPid() const {
		return this->workerExplorerPid != 0 ? this->workerExplorerPid : this->currentExplorerPid();
	}

	[[nodiscard]] DWORD activePinExplorerPid() const {
		if (this->pin && this->pin->explorerPid != 0) return this->pin->explorerPid;
		if (this->pinWorkerExplorerPid != 0) return this->pinWorkerExplorerPid;
		return this->currentWorkerExplorerPid();
	}

	Channel() {
		this->deadline.setSingleShot(true);
		this->deadline.setTimerType(Qt::PreciseTimer);
		QObject::connect(&this->deadline, &QTimer::timeout, &this->deadline, [this] {
			this->collect();
		});
		this->pinDeadline.setSingleShot(true);
		this->pinDeadline.setTimerType(Qt::PreciseTimer);
		QObject::connect(&this->pinDeadline, &QTimer::timeout, &this->pinDeadline, [this] {
			this->collectPin();
		});
		if (auto* app = QCoreApplication::instance()) {
			QObject::connect(app, &QCoreApplication::aboutToQuit, &this->deadline, [this] {
				this->shutdown();
			});
		}
	}

	void shutdown() {
		this->deadline.stop();
		this->pinDeadline.stop();
		this->recovery.reset();
		if (this->pin && this->pin->request) this->pin->request->cancelled.store(true);
		for (auto& entry: this->drainingPins) {
			if (entry.request) entry.request->cancelled.store(true);
		}
		if (auto previous = std::exchange(this->worker, {})) previous->stop();
		if (auto previousPin = std::exchange(this->pinWorker, {})) previousPin->stop();
		this->workerExplorerPid = 0;
		this->pinWorkerExplorerPid = 0;
		this->refresh.reset();
		this->pending.reset();
		this->pin.reset();
		this->drainingPins.clear();
		this->pendingPinDeliveries.clear();
		++this->pinDeliveryGeneration;
		this->commandIdleScheduled = false;
		this->refreshAgain = false;
	}

	[[nodiscard]] bool commandSlotBusy() const {
		return this->waiting || this->pending.has_value();
	}

	void notifyCommandIdle() {
		if (this->commandSlotBusy() || this->commandIdleScheduled || !this->context
		    || !this->commandIdle)
		{
			return;
		}
		this->commandIdleScheduled = true;
		QTimer::singleShot(0, this->context, [this] {
			this->commandIdleScheduled = false;
			if (!this->commandSlotBusy() && this->context && this->commandIdle) {
				this->commandIdle();
			}
		});
	}

	void notify() {
		// No event loop runs inside a bounded command wait. Notifications
		// always run later on the GUI, never reenter its command dispatch.
		if (this->context && this->updated) QTimer::singleShot(0, this->context, this->updated);
	}

	void notifyMoveCompleted(
	    quint64 seq,
	    HWND hwnd,
	    bool succeeded,
	    DesktopAvailability availability,
	    HRESULT error
	) {
		if (hwnd == nullptr || !this->context || !this->moveCompleted) return;
		QTimer::singleShot(0, this->context, [this, seq, hwnd, succeeded, availability, error] {
			if (this->context && this->moveCompleted) {
				this->moveCompleted(seq, hwnd, succeeded, availability, error);
			}
		});
	}

	void notifyPinCompleted(
	    quint64 requestId,
	    HWND hwnd,
	    OwnWindowPin result,
	    bool mayBePinned,
	    quint32 explorerPid
	) {
		if (requestId == 0 || hwnd == nullptr || !this->context || !this->pinCompleted) return;
		const auto key = reinterpret_cast<quintptr>(hwnd);
		const auto deliveryGen = this->pinDeliveryGeneration;
		++this->pendingPinDeliveries[key];
		QTimer::singleShot(
		    0,
		    this->context,
		    [this, deliveryGen, key, requestId, hwnd, result, mayBePinned, explorerPid] {
			    if (deliveryGen == this->pinDeliveryGeneration) {
				    auto it = this->pendingPinDeliveries.find(key);
				    if (it != this->pendingPinDeliveries.end()) {
					    if (--(*it) <= 0) this->pendingPinDeliveries.erase(it);
				    }
			    }
			    if (this->context && this->pinCompleted) {
				    this->pinCompleted(requestId, hwnd, result, mayBePinned, explorerPid);
			    }
		    }
		);
	}

	void notifyPinDrained(
	    quint64 requestId,
	    HWND hwnd,
	    bool stillPinned,
	    bool mayBePinned,
	    bool confirmedUnpinned,
	    bool disconnected,
	    quint32 explorerPid
	) {
		if (hwnd == nullptr || !this->context || !this->pinDrained) return;
		const auto key = reinterpret_cast<quintptr>(hwnd);
		const auto deliveryGen = this->pinDeliveryGeneration;
		++this->pendingPinDeliveries[key];
		QTimer::singleShot(
		    0,
		    this->context,
		    [this,
		     deliveryGen,
		     key,
		     requestId,
		     hwnd,
		     stillPinned,
		     mayBePinned,
		     confirmedUnpinned,
		     disconnected,
		     explorerPid] {
			    if (deliveryGen == this->pinDeliveryGeneration) {
				    auto it = this->pendingPinDeliveries.find(key);
				    if (it != this->pendingPinDeliveries.end()) {
					    if (--(*it) <= 0) this->pendingPinDeliveries.erase(it);
				    }
			    }
			    if (this->context && this->pinDrained) {
				    this->pinDrained(
				        requestId,
				        hwnd,
				        stillPinned,
				        mayBePinned,
				        confirmedUnpinned,
				        disconnected,
				        explorerPid
				    );
			    }
		    }
		);
	}

	void cancelPin(quint64 requestId) {
		if (requestId == 0) return;
		if (this->pin && this->pin->requestId == requestId && this->pin->request) {
			this->pin->request->cancelled.store(true);
		}
		for (auto& entry: this->drainingPins) {
			if (entry.requestId == requestId && entry.request) entry.request->cancelled.store(true);
		}
	}

	void cancelPinForHwnd(HWND hwnd) {
		if (hwnd == nullptr) return;
		if (this->pin && this->pin->target == hwnd && this->pin->request) {
			this->pin->request->cancelled.store(true);
		}
		for (auto& entry: this->drainingPins) {
			if (entry.target == hwnd && entry.request) entry.request->cancelled.store(true);
		}
	}

	[[nodiscard]] bool isPinActiveFor(HWND hwnd) const {
		if (hwnd == nullptr) return false;
		if (this->pin && this->pin->target == hwnd) return true;
		for (const auto& entry: this->drainingPins) {
			if (entry.target == hwnd) return true;
		}
		return this->pendingPinDeliveries.value(reinterpret_cast<quintptr>(hwnd), 0) > 0;
	}

	struct ClassifiedPinResult {
		OwnWindowPin outcome = OwnWindowPin::Unavailable;
		bool retireDisconnected = false;
		bool confirmedUnpinned = false;
		bool mayBePinned = false;
		bool faulted = false;
		bool refused = false;
		DWORD explorerPid = 0;
	};

	ClassifiedPinResult classifyReadyPinResult(
	    std::future<PinResult>& future,
	    bool isUnpin,
	    bool updateAvailability = true
	) {
		try {
			auto value = future.get();
			ClassifiedPinResult out;
			out.explorerPid = value.explorerPid;
			out.confirmedUnpinned = value.unpinned && !value.viewPinned;
			out.mayBePinned =
			    isUnpin ? !out.confirmedUnpinned : (value.viewPinned || value.pinAttemptUncertain);
			out.faulted = value.optionalPinnedAppsFaulted;
			if (value.optionalPinnedAppsFaulted) {
				qCWarning(logNativeDesktops)
				    << "Optional desktop pinning faulted; retiring pin worker and disabling pinning";
				this->skipOptionalPinnedApps = true;
				if (!isUnpin && value.viewPinned) out.outcome = OwnWindowPin::Pinned;
				else if (isUnpin && out.confirmedUnpinned) out.outcome = OwnWindowPin::Unpinned;
				else out.outcome = OwnWindowPin::Unavailable;
				return out;
			}
			if (value.disconnected) {
				qCWarning(logNativeDesktops)
				    << "Optional desktop pinning disconnected, HRESULT" << Qt::hex << value.error
				    << "; retiring pin worker without interrupting desktop control";
				out.retireDisconnected = true;
				if (!isUnpin && value.viewPinned) out.outcome = OwnWindowPin::Pinned;
				else if (isUnpin && out.confirmedUnpinned) out.outcome = OwnWindowPin::Unpinned;
				else out.outcome = OwnWindowPin::Disconnected;
				return out;
			}
			// Only the active worker may latch pinUnavailable; a retired worker
			// draining after recovery must not poison the replacement provider.
			if (updateAvailability && !value.serviceAvailable) this->pinUnavailable = true;
			if (isUnpin) {
				if (out.confirmedUnpinned) out.outcome = OwnWindowPin::Unpinned;
				else if (value.viewPinned) {
					out.outcome = OwnWindowPin::Pinned;
					out.refused = !value.cancelledBeforeUnpin;
				} else {
					out.outcome = value.result == -2 ? OwnWindowPin::Unavailable : OwnWindowPin::Refused;
					out.refused = (out.outcome == OwnWindowPin::Refused);
				}
				return out;
			}
			if (value.viewPinned) {
				out.outcome = OwnWindowPin::Pinned;
				return out;
			}
			out.outcome = value.result == 0
			                ? OwnWindowPin::Pinned
			                : (value.result == -2 ? OwnWindowPin::Unavailable : OwnWindowPin::Refused);
			out.refused = (out.outcome == OwnWindowPin::Refused);
			return out;
		} catch (...) {
			this->skipOptionalPinnedApps = true;
			return {OwnWindowPin::Unavailable, false, false, true, true, false, 0};
		}
	}

	void drainRetiredPins() {
		struct DrainedPin {
			quint64 requestId = 0;
			HWND target = nullptr;
			bool stillPinned = false;
			bool mayBePinned = false;
			bool confirmedUnpinned = false;
			bool disconnected = false;
			DWORD explorerPid = 0;
		};
		std::vector<DrainedPin> drained;
		auto it = this->drainingPins.begin();
		while (it != this->drainingPins.end()) {
			// request->settled becomes true once the retired request's per-HWND
			// COM pin/unpin operation finishes (before the worker drops its COM
			// proxies and exits).
			if (!it->request || it->request->settled.load(std::memory_order_acquire)) {
				auto stillPinned = false;
				auto mayBePinned = true;
				auto confirmedUnpinned = false;
				auto disconnected = it->retiredOnDisconnect;
				auto explorerPid = it->explorerPid;
				if (it->result.valid()
				    && it->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				{
					const auto classified = this->classifyReadyPinResult(it->result, it->unpin, false);
					confirmedUnpinned = classified.confirmedUnpinned;
					stillPinned =
					    !confirmedUnpinned && (classified.outcome == OwnWindowPin::Pinned);
					mayBePinned = it->unpin ? !confirmedUnpinned : classified.mayBePinned;
					disconnected =
					    classified.retireDisconnected
					    || (it->retiredOnDisconnect && !classified.faulted && !classified.refused
					        && !confirmedUnpinned);
					if (classified.explorerPid != 0) explorerPid = classified.explorerPid;
				}
				if (it->target != nullptr) {
					drained.push_back(DrainedPin {
					    it->requestId,
					    it->target,
					    stillPinned,
					    mayBePinned,
					    confirmedUnpinned,
					    disconnected,
					    explorerPid,
					});
				}
				it = this->drainingPins.erase(it);
			} else {
				++it;
			}
		}
		for (const auto& entry: drained) {
			this->notifyPinDrained(
			    entry.requestId,
			    entry.target,
			    entry.stillPinned,
			    entry.mayBePinned,
			    entry.confirmedUnpinned,
			    entry.disconnected,
			    static_cast<quint32>(entry.explorerPid)
			);
		}
	}

	void retirePinWorker(
	    bool disableOptional = false,
	    OwnWindowPin fallbackOutcome = OwnWindowPin::Unavailable
	) {
		if (disableOptional) this->skipOptionalPinnedApps = true;
		auto pendingPin = std::exchange(this->pin, std::nullopt);
		if (pendingPin && pendingPin->request) pendingPin->request->cancelled.store(true);
		if (auto previousPin = std::exchange(this->pinWorker, {})) previousPin->stop();
		this->pinWorkerExplorerPid = 0;
		this->pinDeadline.stop();
		if (pendingPin && pendingPin->target != nullptr) {
			auto pinOutcome = fallbackOutcome;
			auto mayBePinned = pendingPin->unpin;
			auto explorerPid = pendingPin->explorerPid;
			const auto retiredOnDisconnect =
			    !disableOptional && fallbackOutcome == OwnWindowPin::Disconnected;
			const auto settled =
			    !pendingPin->request || pendingPin->request->settled.load(std::memory_order_acquire);
			if (settled) {
				if (pendingPin->result.wait_for(std::chrono::milliseconds(0))
				    == std::future_status::ready)
				{
					const auto classified =
					    this->classifyReadyPinResult(pendingPin->result, pendingPin->unpin, false);
					pinOutcome = classified.outcome;
					mayBePinned = classified.mayBePinned;
					if (classified.explorerPid != 0) explorerPid = classified.explorerPid;
				}
			} else {
				// The retired worker is still executing this HWND's COM pin/unpin
				// operation: keep it fenced in drainingPins until request->settled
				// becomes true (before the worker drops its proxies and exits).
				this->drainingPins.push_back(PinPending {
				    pendingPin->request,
				    std::move(pendingPin->result),
				    pendingPin->target,
				    pendingPin->requestId,
				    pendingPin->unpin,
				    retiredOnDisconnect,
				    pendingPin->explorerPid,
				});
			}
			this->notifyPinCompleted(
			    pendingPin->requestId,
			    pendingPin->target,
			    pinOutcome,
			    mayBePinned,
			    static_cast<quint32>(explorerPid)
			);
		}
	}

	void retire(HRESULT error) {
		auto pendingMove = std::exchange(this->pending, std::nullopt);
		if (auto previous = std::exchange(this->worker, {})) previous->stop();
		this->workerExplorerPid = 0;
		this->refresh.reset();
		this->deadline.stop();
		this->retirePinWorker(false, OwnWindowPin::Disconnected);
		this->pinUnavailable = false;
		this->snapshot.availability = DesktopAvailability::Disconnected;
		this->snapshot.error = error;
		// Keep the last desktop/window values as explicitly stale state.
		this->recovery.disconnected();
		this->notify();
		if (pendingMove) this->notifyCommandIdle();
		if (pendingMove && pendingMove->moveTarget != nullptr) {
			const auto moveSequence = pendingMove->request ? pendingMove->request->sequence : 0;
			if (pendingMove->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				try {
					auto value = pendingMove->result.get();
					const auto available = value.availability == DesktopAvailability::Available;
					const auto moveOk = value.result == 0
					                 && (available || (pendingMove->publicOperation && value.publicReady));
					this->notifyMoveCompleted(
					    moveSequence,
					    pendingMove->moveTarget,
					    moveOk,
					    value.availability,
					    moveOk ? S_OK : value.operationError
					);
				} catch (...) {
					this->notifyMoveCompleted(
					    moveSequence,
					    pendingMove->moveTarget,
					    false,
					    DesktopAvailability::Disconnected,
					    E_UNEXPECTED
					);
				}
			} else {
				this->notifyMoveCompleted(
				    moveSequence,
				    pendingMove->moveTarget,
				    false,
				    DesktopAvailability::Disconnected,
				    error
				);
			}
		}
	}

	bool apply(Snapshot result) {
		if (result.sequence < this->applied) return true;
		this->applied = result.sequence;
		if (result.availability == DesktopAvailability::Available || result.publicReady) {
			if (result.windows != this->snapshot.windows) ++this->windowRevision;
			if (result.windowRead && result.sequence > this->windowReadApplied) {
				this->windowReadApplied = result.sequence;
			}
		}
		if (result.availability != DesktopAvailability::Available) {
			const auto state = result.availability;
			const auto error = result.error;
			if (result.publicReady) {
				this->snapshot.windows = std::move(result.windows);
				this->snapshot.availability = state;
				this->snapshot.error = error;
				this->retirePinWorker(false, OwnWindowPin::Disconnected);
				this->pinUnavailable = false;
				this->recovery.disconnected();
				return false;
			}
			this->retire(error);
			this->snapshot.availability = state;
			return false;
		}
		if (result.desktopRead && result.sequence > this->desktopReadApplied) {
			this->desktopReadApplied = result.sequence;
		}
		this->snapshot = std::move(result);
		return true;
	}

	int execute(
	    std::function<int()> operation,
	    bool queryWindows = false,
	    bool publicOperation = false,
	    Kind kind = Kind::Command,
	    HWND moveTarget = nullptr,
	    bool switchCommand = false,
	    quint64* outSequence = nullptr
	) {
		if (outSequence) *outSequence = 0;
		if (this->waiting) {
			this->snapshot.error = HRESULT_FROM_WIN32(ERROR_BUSY);
			return -1;
		}
		this->collect();
		if (!this->worker) return -1;
		if (this->pending) {
			this->snapshot.error = HRESULT_FROM_WIN32(ERROR_BUSY);
			return -1;
		}
		struct CommandIdleGuard {
			Channel* self;
			~CommandIdleGuard() {
				if (!this->self->commandSlotBusy()) this->self->notifyCommandIdle();
			}
		} idleGuard {this};
		QScopedValueRollback waiting(this->waiting, true);
		const auto owner = this->worker;
		auto request = std::make_shared<Request>();
		if (!request->finished) {
			this->snapshot.error = HRESULT_FROM_WIN32(GetLastError());
			return -1;
		}
		request->sequence = ++this->sequence;
		if (outSequence) *outSequence = request->sequence;
		request->operation = std::move(operation);
		request->completed = [this] { this->wakeOnCompletion(); };
		request->queryWindows = queryWindows;
		if (queryWindows) request->windows = this->watchedWindows.values();
		auto result = owner->submit(request);
		const auto guiDeadline = request->submitted + callBound;
		// COM can synchronously notify GUI windows. Preserve sent-message
		// dispatch without consuming posted Qt events or extending the bound.
		while (owner == this->worker
		       && result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		{
			if (request->stalled(this->stallLimit) || this->stalled()) break;
			const auto remaining = guiDeadline - Clock::now();
			if (remaining <= Clock::duration::zero()) break;
			const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
			const auto status = MsgWaitForMultipleObjectsEx(
			    1,
			    &request->finished,
			    static_cast<DWORD>(std::min<qint64>(milliseconds.count() + 1, 10)),
			    QS_SENDMESSAGE,
			    MWMO_INPUTAVAILABLE
			);
			if (status == WAIT_OBJECT_0) break;
			if (status == WAIT_TIMEOUT) continue;
			if (status != WAIT_OBJECT_0 + 1) break;
			MSG message {};
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
		if (owner != this->worker) return -1;
		if (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			if (request->stalled(this->stallLimit) || this->stalled()) {
				this->retire(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
				return -1;
			}
			// The request is still owned by this generation, not failed.
			// Keep its future so the GUI can apply the eventual result.
			this->pending.emplace(Pending {
			    request,
			    std::move(result),
			    kind,
			    queryWindows,
			    publicOperation,
			    moveTarget,
			    switchCommand,
			});
			this->armDeadline();
			return kind == Kind::Command ? 0 : -1;
		}
		try {
			auto value = result.get();
			const auto outcome = value.result;
			const auto publicReady = value.publicReady;
			// A command snapshot must not erase the last window membership.
			if (!queryWindows) value.windows = this->snapshot.windows;
			const auto available = this->apply(std::move(value));
			if (kind == Kind::Acquisition && available) {
				++this->windowRevision;
				this->workerExplorerPid = this->currentExplorerPid();
				this->pinUnavailable = false;
				this->retirePinWorker(false, OwnWindowPin::Disconnected);
			}
			return available || (publicOperation && publicReady) ? outcome : -1;
		} catch (...) {
			this->retire(E_UNEXPECTED);
			return -1;
		}
	}

	bool acquire() {
		if (this->pending) return false;
		if (this->worker) {
			if (this->snapshot.availability == DesktopAvailability::Available) return true;
			return this->execute(
			           [] { return provider().acquire() ? 0 : -1; },
			           true,
			           false,
			           Kind::Acquisition
			       )
			    == 0;
		}
		auto count = liveWorkers().load();
		do {
			if (count >= workerLimit) {
				this->snapshot.error = HRESULT_FROM_WIN32(ERROR_BUSY);
				return false;
			}
		} while (!liveWorkers().compare_exchange_weak(count, count + 1));
		std::shared_ptr<Worker> next;
		try {
			next = std::make_shared<Worker>();
			std::thread([next,
			             enabled = this->privateEnabled
#ifdef QS_TEST
			             ,
			             factory = this->factory,
			             validator = this->validator
#endif
			] {
				next->run(
				    enabled
#ifdef QS_TEST
				    ,
				    factory,
				    validator
#endif
				);
			}).detach();
		} catch (...) {
			--liveWorkers();
			this->snapshot.error = E_OUTOFMEMORY;
			return false;
		}
		this->worker = std::move(next);
		return this->execute(
		           [] { return provider().acquire() ? 0 : -1; },
		           true,
		           false,
		           Kind::Acquisition
		       )
		    == 0;
	}

	bool ensurePinWorker() {
		if (this->pinWorker) return true;
		auto count = livePinWorkers().load();
		do {
			if (count >= workerLimit) return false;
		} while (!livePinWorkers().compare_exchange_weak(count, count + 1));
#ifdef QS_TEST
		const bool testMode = static_cast<bool>(this->factory);
		ViewCollection* ownedViews =
		    testMode && this->worker ? this->worker->testViews.addRef() : nullptr;
		if (testMode && !ownedViews) {
			--livePinWorkers();
			return false;
		}
#endif
		std::shared_ptr<PinWorker> next;
		try {
			next = std::make_shared<PinWorker>();
			std::thread([next
#ifdef QS_TEST
			             ,
			             testMode,
			             ownedViews,
			             validator = this->validator,
			             explorerPidProvider = this->explorerPidProvider,
			             testPinned = this->testPinnedApps,
			             pinnedShell = this->pinnedAppsShell
#endif
			] {
				next->run(
#ifdef QS_TEST
				    testMode,
				    ownedViews,
				    validator,
				    explorerPidProvider,
				    testPinned,
				    pinnedShell
#endif
				);
			}).detach();
		} catch (...) {
#ifdef QS_TEST
			ComPtr<ViewCollection> guard;
			guard.Attach(ownedViews);
			release(guard);
#endif
			--livePinWorkers();
			return false;
		}
		this->pinWorker = std::move(next);
		const auto detectedPid = this->currentExplorerPid();
		this->pinWorkerExplorerPid = detectedPid != 0 ? detectedPid : this->workerExplorerPid;
		return true;
	}

	OwnWindowPin requestPin(HWND hwnd, bool unpin, quint64* outRequestId = nullptr) {
		if (outRequestId) *outRequestId = 0;
		if (this->waiting) return OwnWindowPin::Busy;
		this->collectPin();
		// Never admit a replacement request or fall back while this HWND still has
		// an active, draining, or undelivered pin/unpin request, even if the
		// channel is disconnected or optional pinning was disabled.
		if (this->isPinActiveFor(hwnd)) return OwnWindowPin::Busy;
		if (!this->worker || !this->privateEnabled
		    || this->snapshot.availability != DesktopAvailability::Available
		    || this->skipOptionalPinnedApps || this->pinUnavailable)
		{
			return OwnWindowPin::Unavailable;
		}
#ifdef QS_TEST
		if (this->factory && !this->pinnedAppsShell && !this->testPinnedApps) {
			return OwnWindowPin::Unavailable;
		}
#endif
		if (this->pin) return OwnWindowPin::Busy;
		if (!this->ensurePinWorker()) return OwnWindowPin::Unavailable;
		auto request = std::make_shared<PinRequest>();
		request->window = hwnd;
		request->unpin = unpin;
		request->completed = [this] { this->wakeOnPinCompletion(); };
		const auto reqId = ++this->pinSequence;
		const auto explorerPid =
		    this->pinWorkerExplorerPid != 0 ? this->pinWorkerExplorerPid : this->currentWorkerExplorerPid();
		if (this->pinWorkerExplorerPid == 0) this->pinWorkerExplorerPid = explorerPid;
		this->pin.emplace(PinPending {
		    request,
		    this->pinWorker->submit(request),
		    hwnd,
		    reqId,
		    unpin,
		    false,
		    explorerPid,
		});
		this->armPinDeadline();
		if (outRequestId) *outRequestId = reqId;
		return OwnWindowPin::Pending;
	}

	void pruneWindows() {
#ifdef QS_TEST
		if (this->validator) return;
#endif
		this->watchedWindows.removeIf([](quintptr value) {
			return !IsWindow(reinterpret_cast<HWND>(value));
		});
	}

	void requestRefresh() {
		if (!this->worker) return;
		if (this->refresh || this->pending) {
			this->refreshAgain = true;
			return;
		}
		this->pruneWindows();
		auto request = std::make_shared<Request>();
		if (!request->finished) {
			this->retire(HRESULT_FROM_WIN32(GetLastError()));
			return;
		}
		request->sequence = ++this->sequence;
		request->operation = [] {
			return provider().privateEnabled ? (desktopCount() < 0 ? -1 : 0) : 0;
		};
		request->completed = [this] { this->wakeOnCompletion(); };
		request->queryWindows = true;
		request->windows = this->watchedWindows.values();
		this->refresh.emplace(Pending {request, this->worker->submit(request), Kind::Probe, true});
		this->armDeadline();
	}

	void wakeOnCompletion() {
		// Channel and its receiver live for the process. No owner QObject is
		// dereferenced by a retired worker that completes after shutdown.
		if (this->completionWake.exchange(true)) return;
		if (!QMetaObject::invokeMethod(&this->deadline, [this] {
			this->completionWake.store(false);
			this->collect();
		}, Qt::QueuedConnection)) this->completionWake.store(false);
	}

	void wakeOnPinCompletion() {
		if (this->pinCompletionWake.exchange(true)) return;
		if (!QMetaObject::invokeMethod(&this->pinDeadline, [this] {
			this->pinCompletionWake.store(false);
			this->collectPin();
		}, Qt::QueuedConnection)) this->pinCompletionWake.store(false);
	}

	void armDeadline() {
		auto next = Clock::time_point::max();
		const auto consider = [&](const std::shared_ptr<Request>& request) {
			const auto ticks = request->started.load();
			const auto since = ticks ? Clock::time_point(Clock::duration(ticks)) : request->submitted;
			next = std::min(next, since + this->stallLimit);
		};
		if (this->refresh) consider(this->refresh->request);
		if (this->pending) consider(this->pending->request);
		if (next == Clock::time_point::max()) this->deadline.stop();
		else this->deadline.start(std::max(
		    std::chrono::ceil<std::chrono::milliseconds>(next - Clock::now()),
		    std::chrono::milliseconds(1)
		));
	}

	void armPinDeadline() {
		if (!this->pin) {
			this->pinDeadline.stop();
			return;
		}
		const auto ticks = this->pin->request->started.load();
		const auto since =
		    ticks ? Clock::time_point(Clock::duration(ticks)) : this->pin->request->submitted;
		const auto next = since + this->pinStallLimit;
		this->pinDeadline.start(std::max(
		    std::chrono::ceil<std::chrono::milliseconds>(next - Clock::now()),
		    std::chrono::milliseconds(1)
		));
	}

	bool stalled() const {
		const auto jobStalled = [this](const auto& job) {
			return job && job->result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
			    && job->request->stalled(this->stallLimit);
		};
		return jobStalled(this->refresh) || jobStalled(this->pending);
	}

	void collectOne(std::optional<Pending>& slot) {
		if (!slot || slot->result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
			return;
		auto finished = std::move(*slot);
		slot.reset();
		try {
			auto value = finished.result.get();
			const auto seq = value.sequence;
			const auto outcome = value.result;
			const auto success = outcome != -1;
			const auto operationError = value.operationError;
			const auto publicReady = value.publicReady;
			if (finished.switchCommand && outcome == -2
			    && operationError == HRESULT_FROM_WIN32(ERROR_CANCELLED)
			    && seq > this->cancelledCommandSequence)
			{
				this->cancelledCommandSequence = seq;
			}
			if (finished.switchCommand && outcome == -1 && seq > this->failedSwitchSequence) {
				this->failedSwitchSequence = seq;
				this->failedSwitchError = operationError;
			}
			if (!finished.queryWindows) value.windows = this->snapshot.windows;
			if (value.sequence < this->applied) this->refreshAgain = true;
			const auto available = this->apply(std::move(value));
			const auto availability = this->snapshot.availability;
			if (finished.moveTarget != nullptr) {
				const auto moveOk = outcome == 0 && (available || (finished.publicOperation && publicReady));
				const auto moveSequence = finished.request ? finished.request->sequence : 0;
				this->notifyMoveCompleted(
				    moveSequence,
				    finished.moveTarget,
				    moveOk,
				    availability,
				    moveOk ? S_OK : operationError
				);
			}
			if (finished.kind == Kind::Acquisition && success && available) {
				++this->windowRevision;
				this->workerExplorerPid = this->currentExplorerPid();
				this->pinUnavailable = false;
				this->retirePinWorker(false, OwnWindowPin::Disconnected);
				this->recovery.reset();
				const auto owner = this->worker;
				if (this->context)
					QTimer::singleShot(0, this->context, [this, owner] {
						if (owner == this->worker && this->recovered
						    && this->snapshot.availability == DesktopAvailability::Available)
							this->recovered();
					});
			}
			// Even a refused late command needs a fresh view of Explorer's
			// actual state. Never dispatch the mutation a second time.
			if (finished.kind == Kind::Command) this->refreshAgain = true;
			this->notify();
		} catch (...) {
			if (finished.switchCommand && finished.request
			    && finished.request->sequence > this->failedSwitchSequence)
			{
				this->failedSwitchSequence = finished.request->sequence;
				this->failedSwitchError = E_UNEXPECTED;
			}
			this->retire(E_UNEXPECTED);
			if (finished.moveTarget != nullptr) {
				const auto moveSequence = finished.request ? finished.request->sequence : 0;
				this->notifyMoveCompleted(
				    moveSequence,
				    finished.moveTarget,
				    false,
				    DesktopAvailability::Disconnected,
				    E_UNEXPECTED
				);
			}
		}
	}

	void collectPin() {
		this->drainRetiredPins();
		if (this->pin
		    && this->pin->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready
		    && (!this->pin->request || this->pin->request->settled.load(std::memory_order_acquire)))
		{
			auto finished = std::move(*this->pin);
			this->pin.reset();
			const auto wasDisabled = this->skipOptionalPinnedApps;
			const auto classified = this->classifyReadyPinResult(finished.result, finished.unpin);
			const auto explorerPid =
			    classified.explorerPid != 0 ? classified.explorerPid : finished.explorerPid;
			if (classified.explorerPid != 0 && this->pinWorker) {
				this->pinWorkerExplorerPid = classified.explorerPid;
			}
			if (!wasDisabled && this->skipOptionalPinnedApps) {
				this->retirePinWorker(true);
			} else if (classified.retireDisconnected) {
				this->retirePinWorker(false, OwnWindowPin::Disconnected);
			}
			this->notifyPinCompleted(
			    finished.requestId,
			    finished.target,
			    classified.outcome,
			    classified.mayBePinned,
			    static_cast<quint32>(explorerPid)
			);
			this->armPinDeadline();
			return;
		}
		if (this->pin && this->pin->request->stalled(this->pinStallLimit)) {
			qCWarning(logNativeDesktops)
			    << "Optional desktop pinning stalled; retiring pin worker and disabling pinning";
			this->retirePinWorker(true);
			return;
		}
		this->armPinDeadline();
	}

	void collect() {
		const auto hadPending = this->pending.has_value();
		this->collectOne(this->refresh);
		this->collectOne(this->pending);
		if (hadPending && !this->pending) this->notifyCommandIdle();
		if (this->stalled()) {
			this->retire(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			return;
		}
		if (!this->refresh && !this->pending) {
			if (std::exchange(this->refreshAgain, false)) this->requestRefresh();
		}
		this->armDeadline();
	}

	WindowSnapshot window(HWND hwnd) {
		if (!hwnd) return {};
		const auto value = reinterpret_cast<quintptr>(hwnd);
		DWORD pid = 0;
		const auto thread = GetWindowThreadProcessId(hwnd, &pid);
		auto entry = this->snapshot.windows.constFind(value);
		if (entry != this->snapshot.windows.cend() && entry->pid == pid && entry->thread == thread)
			return *entry;
		if (entry != this->snapshot.windows.cend()) {
			this->snapshot.windows.remove(value);
			this->watchedWindows.remove(value);
		}
		if (this->watchedWindows.size() >= windowLimit) this->pruneWindows();
		if (this->watchedWindows.size() < windowLimit && !this->watchedWindows.contains(value)) {
			this->watchedWindows.insert(value);
			this->requestRefresh();
		}
		return {};
	}
};

Channel& channel() {
	static auto* instance = new Channel;
	return *instance;
}

int cachedCount() {
	const auto& c = channel();
	return c.snapshot.availability == DesktopAvailability::Available
	         ? static_cast<int>(c.snapshot.ids.size())
	         : -1;
}
int cachedCurrent() { return channel().snapshot.current; }
GUID cachedId(int index) {
	const auto& ids = channel().snapshot.ids;
	return index >= 0 && index < ids.size() ? ids[index] : GUID {};
}
int cachedWindow(HWND hwnd) {
	const auto id = channel().window(hwnd).id;
	const auto& ids = channel().snapshot.ids;
	for (qsizetype i = 0; i < ids.size(); ++i)
		if (!IsEqualGUID(id, GUID {}) && IsEqualGUID(id, ids[i])) return static_cast<int>(i);
	return -1;
}
int cachedOnCurrent(HWND hwnd) { return channel().window(hwnd).onCurrent; }
int workerIndex(GUID id) {
	const auto& ids = provider().ids;
	for (qsizetype i = 0; i < ids.size(); ++i)
		if (!IsEqualGUID(id, GUID {}) && IsEqualGUID(id, ids[i])) return static_cast<int>(i);
	return -1;
}
int boundedSwitch(int index) {
	return switchNativeDesktopById(cachedId(index));
}
int boundedCreate() {
	return channel().execute([] { return createDesktop(); });
}
int boundedMove(HWND hwnd, int index) {
	return moveNativeDesktopWindowById(hwnd, cachedId(index));
}
} // namespace

bool loadNativeDesktopAccessor(
    VirtualDesktops::Accessor& accessor,
    QObject* context,
    std::function<void()> recovered
) {
	auto& p = channel();
	const auto build = windowsBuild();
	// These build families share the 24H2 ABI. Never guess a vtable on another
	// build, or try unrelated interface GUIDs after QueryService refuses it.
	if (build != 26100 && build != 26200 && build != 26300) {
		qCWarning(logNativeDesktops) << "Desktop dispatch refused on unrecognized Windows build"
		                             << build;
		p.snapshot.availability = DesktopAvailability::UnsupportedBuild;
		p.refusedBuild = true;
		p.supported = false;
		accessor = {};
		return false;
	}
	p.supported = true;
	p.refusedBuild = false;
	p.context = context;
	p.recovered = std::move(recovered);
	accessor.getDesktopCount = &cachedCount;
	accessor.getCurrentDesktopNumber = &cachedCurrent;
	accessor.getDesktopIdByNumber = &cachedId;
	accessor.getWindowDesktopNumber = &cachedWindow;
	accessor.isWindowOnCurrentVirtualDesktop = &cachedOnCurrent;
	accessor.goToDesktopNumber = &boundedSwitch;
	accessor.moveWindowToDesktopNumber = &boundedMove;
	accessor.createDesktop = &boundedCreate;
	// Removal, notification hooks and pinning of other applications' windows
	// intentionally remain absent. The shell pins only its own panels, through
	// pinNativeDesktopOwnWindow, so toplevel queries never wait on COM.
	p.shutdown();
	p.privateEnabled = true;
	p.skipOptionalPinnedApps = false;
	p.pinUnavailable = false;
	if (p.acquire()) return true;
	if (!p.pending) p.recovery.disconnected();
	return false;
}

bool retryNativeDesktopAccessor() {
	auto& p = channel();
	if (!p.supported) return false;
	p.recovery.retryOnDispatch();
	return p.snapshot.availability == DesktopAvailability::Available && p.worker
	    && !p.recovery.active();
}

void invalidateNativeDesktopAccessor(HRESULT error) {
	channel().retire(FAILED(error) ? error : RPC_E_DISCONNECTED);
}

DesktopAvailability nativeDesktopAvailability() {
	return channel().refusedBuild ? DesktopAvailability::UnsupportedBuild
	                              : channel().snapshot.availability;
}
HRESULT nativeDesktopLastError() { return channel().snapshot.error; }

void initializeNativeDesktopQueries(QObject* context, std::function<void()> updated) {
	auto& c = channel();
	c.context = context;
	c.updated = std::move(updated);
	QObject::connect(context, &QObject::destroyed, &c.deadline, [&c] {
		c.shutdown();
		c.context = nullptr;
		c.recovered = {};
		c.updated = {};
		c.moveCompleted = {};
		c.pinCompleted = {};
		c.pinDrained = {};
		c.commandIdle = {};
		c.commandIdleScheduled = false;
	});
	if (!c.worker && !c.recovery.active() && !c.acquire() && !c.pending) c.recovery.disconnected();
}

void setNativeDesktopMoveObserver(
    QObject* context,
    std::function<void(quint64, HWND, bool, DesktopAvailability, HRESULT)> onMoveCompleted
) {
	auto& c = channel();
	c.context = context;
	c.moveCompleted = std::move(onMoveCompleted);
}

void setNativeDesktopPinObserver(
    QObject* context,
    std::function<void(quint64, HWND, OwnWindowPin, bool, quint32)> onPinCompleted
) {
	auto& c = channel();
	c.context = context;
	c.pinCompleted = std::move(onPinCompleted);
}

void setNativeDesktopPinDrainObserver(
    QObject* context,
    std::function<void(quint64, HWND, bool, bool, bool, bool, quint32)> onPinDrained
) {
	auto& c = channel();
	c.context = context;
	c.pinDrained = std::move(onPinDrained);
}

void setNativeDesktopCommandIdleObserver(QObject* context, std::function<void()> onCommandIdle) {
	auto& c = channel();
	c.context = context;
	c.commandIdle = std::move(onCommandIdle);
}

void requestNativeDesktopRefresh() { channel().requestRefresh(); }
bool nativeDesktopCommandPending() {
	const auto& c = channel();
	return c.waiting || (c.pending && c.pending->kind == Channel::Kind::Command);
}
quint64 nativeDesktopPendingMoveSequence(HWND hwnd) {
	const auto& c = channel();
	if (!c.pending || c.pending->moveTarget == nullptr || !c.pending->request) return 0;
	if (hwnd != nullptr && c.pending->moveTarget != hwnd) return 0;
	return c.pending->request->sequence;
}
bool nativeDesktopRequestPending() {
	const auto& c = channel();
	return c.waiting || c.pending.has_value() || c.refresh.has_value();
}
quint64 nativeDesktopSubmittedSequence() { return channel().sequence; }
quint64 nativeDesktopReadSequence() { return channel().desktopReadApplied; }
quint64 nativeDesktopWindowReadSequence() { return channel().windowReadApplied; }
quint64 nativeDesktopCancelledSequence() { return channel().cancelledCommandSequence; }
quint64 nativeDesktopFailedSwitchSequence() { return channel().failedSwitchSequence; }
HRESULT nativeDesktopFailedSwitchError() { return channel().failedSwitchError; }
quint64 nativeDesktopWindowSnapshotRevision() { return channel().windowRevision; }
GUID nativeWindowDesktopId(HWND hwnd) { return channel().window(hwnd).id; }
int nativeWindowOnCurrentDesktop(HWND hwnd) { return channel().window(hwnd).onCurrent; }
int switchNativeDesktopById(GUID desktop, quint64* outSequence) {
	return channel().execute(
	    [desktop] { return switchDesktop(workerIndex(desktop)); },
	    false,
	    false,
	    Channel::Kind::Command,
	    nullptr,
	    true,
	    outSequence
	);
}
int switchNativeDesktopByIdIfCurrent(
    GUID desktop,
    GUID expectedCurrent,
    quint64* outSequence
) {
	return channel().execute(
	    [desktop, expectedCurrent] {
		    auto& p = provider();
		    p.lastError = S_OK;
		    if (!p.refresh()) return -1;
		    if (p.current < 0 || p.current >= p.ids.size()
		        || !IsEqualGUID(p.ids[p.current], expectedCurrent))
		    {
			    p.operationError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
			    return -2;
		    }
		    if (IsEqualGUID(desktop, expectedCurrent)) return 0;
		    p.desktopRefreshed = false;
		    return switchDesktop(workerIndex(desktop));
	    },
	    false,
	    false,
	    Channel::Kind::Command,
	    nullptr,
	    true,
	    outSequence
	);
}
int moveNativeDesktopWindowById(HWND hwnd, GUID desktop) {
	const auto result = channel().execute(
	    [hwnd, desktop] { return moveWindow(hwnd, workerIndex(desktop)); },
	    false,
	    false,
	    Channel::Kind::Command,
	    hwnd
	);
	if (result == 0) channel().requestRefresh();
	return result;
}

std::optional<bool> moveNativeDesktopOwnWindow(HWND hwnd, GUID desktop) {
	if (!channel().worker) return std::nullopt;
	const auto result = channel().execute(
	    [hwnd, desktop] {
		    auto& p = provider();
		    p.lastError = S_OK;
		    if (!p.publicApi) return -2;
		    return p.invoke(
		               [&] { return p.publicApi->MoveWindowToDesktop(hwnd, desktop); },
		               "MoveWindowToDesktop"
		           )
		             ? 0
		             : -1;
	    },
	    false,
	    true,
	    Channel::Kind::Command,
	    hwnd
	);
	if (result == 0) channel().requestRefresh();
	return result == -2 ? std::nullopt : std::optional<bool>(result == 0);
}
OwnWindowPin pinNativeDesktopOwnWindow(HWND hwnd, quint64* outRequestId) {
	return channel().requestPin(hwnd, false, outRequestId);
}
OwnWindowPin unpinNativeDesktopOwnWindow(HWND hwnd, quint64* outRequestId) {
	return channel().requestPin(hwnd, true, outRequestId);
}
void cancelNativeDesktopOwnWindowPin(quint64 requestId) { channel().cancelPin(requestId); }
void cancelNativeDesktopOwnWindowPinForHwnd(HWND hwnd) { channel().cancelPinForHwnd(hwnd); }
bool nativeDesktopOwnWindowPinActive(HWND hwnd) { return channel().isPinActiveFor(hwnd); }
quint32 nativeDesktopExplorerPid() { return static_cast<quint32>(channel().activePinExplorerPid()); }
void shutdownNativeDesktopAccessor() { channel().shutdown(); }

#ifdef QS_TEST
void setNativeDesktopFactoryForTest(NativeDesktopFactory factory) {
	auto& p = channel();
	// Clear an injected test clock first: it may capture a caller's local that
	// is already out of scope when a RAII reset guard runs this.
	p.recovery.setAgeForTest({});
	p.explorerPidProvider = {};
	auto previous = p.worker;
	auto previousPin = p.pinWorker;
	p.shutdown();
	// Test fixtures own their fakes on the stack. A bounded teardown barrier
	// lets them verify cleanup before destroying those fakes.
	if (previous) previous->exit.wait_for(callBound);
	if (previousPin) previousPin->exit.wait_for(callBound);
	p.context = nullptr;
	p.recovered = {};
	p.updated = {};
	p.moveCompleted = {};
	p.pinCompleted = {};
	p.pinDrained = {};
	p.commandIdle = {};
	p.commandIdleScheduled = false;
	p.supported = false;
	p.privateEnabled = false;
	p.refusedBuild = false;
	p.skipOptionalPinnedApps = false;
	p.pinUnavailable = false;
	p.applied = 0;
	p.desktopReadApplied = 0;
	p.windowReadApplied = 0;
	p.cancelledCommandSequence = 0;
	p.failedSwitchSequence = 0;
	p.failedSwitchError = S_OK;
	p.windowRevision = 0;
	p.snapshot = {};
	p.watchedWindows.clear();
	p.factory = std::move(factory);
	p.validator = {};
	p.explorerPidProvider = {};
	p.testPinnedApps = nullptr;
	p.pinnedAppsShell = nullptr;
	p.stallLimit = stuckBound;
	p.pinStallLimit = stuckBound;
}

void setNativeDesktopExplorerPidForTest(std::function<DWORD()> provider) {
	auto& c = channel();
	c.explorerPidProvider = std::move(provider);
	const auto detectedPid = c.currentExplorerPid();
	if (c.worker) c.workerExplorerPid = detectedPid;
	if (!c.pin) {
		auto previousPin = c.pinWorker;
		c.retirePinWorker(false);
		if (previousPin) previousPin->exit.wait_for(callBound);
	} else if (c.pinWorker) {
		c.pinWorkerExplorerPid = detectedPid;
	}
}

void setNativeDesktopPinnedAppsForTest(PinnedApps* pinned) {
	auto& c = channel();
	auto previousPin = c.pinWorker;
	c.retirePinWorker(false);
	if (previousPin) previousPin->exit.wait_for(callBound);
	c.testPinnedApps = pinned;
	c.pinUnavailable = (pinned == nullptr);
}

void setNativeDesktopPinnedAppsShellForTest(IServiceProvider* shell) {
	auto& c = channel();
	auto previousPin = c.pinWorker;
	c.retirePinWorker(false);
	if (previousPin) previousPin->exit.wait_for(callBound);
	c.pinnedAppsShell = shell;
	c.pinUnavailable = false;
}

void setNativeDesktopWindowValidatorForTest(std::function<bool(HWND)> validator) {
	auto& c = channel();
	c.validator = validator;
	if (!c.pin) {
		auto previousPin = c.pinWorker;
		c.retirePinWorker(false);
		if (previousPin) previousPin->exit.wait_for(callBound);
	}
	c.execute([validator = std::move(validator)] {
		provider().windowValidator = validator;
		return 0;
	});
}

void setNativeDesktopRecoveryAgeForTest(std::function<qint64()> age) {
	channel().recovery.setAgeForTest(std::move(age));
}

int nativeDesktopRecoveryPendingDelayForTest() { return channel().recovery.pendingDelay(); }

int refreshNativeDesktopForTest() {
	return channel().execute([] { return desktopCount(); }, true, false, Channel::Kind::Probe);
}
int nativeDesktopWorkerCountForTest() { return liveWorkers().load() + livePinWorkers().load(); }
int nativeDesktopPinWorkerCountForTest() { return livePinWorkers().load(); }
bool nativeDesktopPendingReadyForTest() {
	const auto& c = channel();
	return c.pending && c.pending->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}
bool nativeDesktopPinReadyForTest() {
	const auto& c = channel();
	return c.pin && c.pin->request
	    && c.pin->request->settled.load(std::memory_order_acquire)
	    && c.pin->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}
void setNativeDesktopStuckBoundForTest(int milliseconds) {
	auto& c = channel();
	c.stallLimit = std::chrono::milliseconds(milliseconds);
	c.pinStallLimit = std::chrono::milliseconds(milliseconds);
}
void setNativeDesktopPinStuckBoundForTest(int milliseconds) {
	channel().pinStallLimit = std::chrono::milliseconds(milliseconds);
}
#endif

} // namespace qs::windows
