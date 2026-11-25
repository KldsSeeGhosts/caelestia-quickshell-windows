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
	    || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
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
		if (hr == RPC_E_DISCONNECTED || hr == CO_E_OBJNOTCONNECTED
		    || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
		{
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
};

struct Request {
	HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	~Request() {
		if (this->finished) CloseHandle(this->finished);
	}
	std::function<int()> operation;
	std::function<void()> completed;
	QList<quintptr> windows;
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
	// 0: pinned (or already pinned), -1: refused, -2: unavailable.
	int result = -2;
	HRESULT error = S_OK;
	bool serviceAvailable = true;
	bool disconnected = false;
	bool optionalPinnedAppsFaulted = false;
};

struct PinRequest {
	HWND window = nullptr;
	std::function<void()> completed;
	std::promise<PinResult> completion;
	Clock::time_point submitted = Clock::now();
	std::atomic<Clock::duration::rep> started = 0;

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
	bool invoke(F&& call, const char* operation) {
		HRESULT hr = S_OK;
		if (!sehCall(std::forward<F>(call), hr)) {
			this->fault(operation);
			return false;
		}
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
			return;
		}
#endif
		ComPtr<IServiceProvider> shell;
		auto ok = this->invoke(
		              [&] {
			              return CoCreateInstance(
			                  immersiveShell,
			                  nullptr,
			                  CLSCTX_LOCAL_SERVER,
			                  IID_PPV_ARGS(&shell)
			              );
		              },
		              "Pin ImmersiveShell"
		          )
		       && this->invoke(
		           [&] {
			           return shell->QueryService(__uuidof(ViewCollection), IID_PPV_ARGS(&this->views));
		           },
		           "Pin ApplicationViewCollection GUID"
		       );
		if (ok) ok = this->acquirePinnedApps(shell.Get());
		ok = this->releaseLocal(shell) && ok;
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

	PinResult pin(HWND window) {
		this->lastError = S_OK;
		this->ensureAcquired();
		PinResult out;
		out.serviceAvailable = this->serviceAvailable;
		out.disconnected = this->disconnected;
		out.optionalPinnedAppsFaulted = this->optionalPinnedAppsFaulted;
		out.error = this->lastError;
		if (this->optionalPinnedAppsFaulted || this->disconnected || !this->serviceAvailable
		    || !this->pinnedApps || !this->views)
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
		// Query first so a repeated call never toggles anything.
		auto ok =
		    this->invoke([&] { return this->pinnedApps->IsViewPinned(view, &pinned); }, "IsViewPinned")
		    && (pinned || this->invoke([&] { return this->pinnedApps->PinView(view); }, "PinView"));
		ok = this->releaseLocal(unknown) && ok;
		out.disconnected = this->disconnected;
		out.optionalPinnedAppsFaulted = this->optionalPinnedAppsFaulted;
		out.serviceAvailable = this->serviceAvailable;
		out.error = this->lastError;
		if (this->disconnected || this->optionalPinnedAppsFaulted || !this->pinnedApps) {
			out.result = -2;
		} else {
			out.result = ok ? 0 : -1;
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
		}
		this->wake.notify_one();
		return result;
	}

	void run(
#ifdef QS_TEST
	    bool testMode,
	    ViewCollection* ownedViews,
	    std::function<bool(HWND)> validator,
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
					result = p.pin(request->window);
				}
			} catch (...) {
				result.result = -2;
				result.error = E_UNEXPECTED;
				result.serviceAvailable = false;
				result.optionalPinnedAppsFaulted = true;
			}
			request->completion.set_value(std::move(result));
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
	};
	std::shared_ptr<Worker> worker;
	std::shared_ptr<PinWorker> pinWorker;
	Snapshot snapshot;
	QSet<quintptr> watchedWindows;
	QPointer<QObject> context;
	std::function<void()> recovered;
	std::function<void()> updated;
	std::function<void(quint64, HWND, bool, DesktopAvailability, HRESULT)> moveCompleted;
	std::function<void(quint64, HWND, OwnWindowPin)> pinCompleted;
	QTimer deadline;
	QTimer pinDeadline;
	std::atomic<bool> completionWake = false;
	std::atomic<bool> pinCompletionWake = false;
	std::optional<Pending> refresh;
	std::optional<Pending> pending;
	std::optional<PinPending> pin;
	std::chrono::milliseconds stallLimit = stuckBound;
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
	quint64 cancelledCommandSequence = 0;
	quint64 failedSwitchSequence = 0;
	HRESULT failedSwitchError = S_OK;
	quint64 windowRevision = 0;
#ifdef QS_TEST
	NativeDesktopFactory factory;
	std::function<bool(HWND)> validator;
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
		if (auto previous = std::exchange(this->worker, {})) previous->stop();
		if (auto previousPin = std::exchange(this->pinWorker, {})) previousPin->stop();
		this->refresh.reset();
		this->pending.reset();
		this->pin.reset();
		this->refreshAgain = false;
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

	void notifyPinCompleted(quint64 requestId, HWND hwnd, OwnWindowPin result) {
		if (requestId == 0 || hwnd == nullptr || !this->context || !this->pinCompleted) return;
		QTimer::singleShot(0, this->context, [this, requestId, hwnd, result] {
			if (this->context && this->pinCompleted) {
				this->pinCompleted(requestId, hwnd, result);
			}
		});
	}

	void retirePinWorker(bool disableOptional = false) {
		if (disableOptional) this->skipOptionalPinnedApps = true;
		auto pendingPin = std::exchange(this->pin, std::nullopt);
		if (auto previousPin = std::exchange(this->pinWorker, {})) previousPin->stop();
		this->pinDeadline.stop();
		if (pendingPin && pendingPin->target != nullptr) {
			auto pinOutcome = OwnWindowPin::Unavailable;
			if (pendingPin->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				try {
					auto value = pendingPin->result.get();
					if (!value.disconnected && !value.optionalPinnedAppsFaulted) {
						pinOutcome = value.result == 0
						               ? OwnWindowPin::Pinned
						               : (value.result == -2 ? OwnWindowPin::Unavailable
						                                     : OwnWindowPin::Refused);
					}
				} catch (...) {
					pinOutcome = OwnWindowPin::Unavailable;
				}
			}
			this->notifyPinCompleted(pendingPin->requestId, pendingPin->target, pinOutcome);
		}
	}

	void retire(HRESULT error) {
		auto pendingMove = std::exchange(this->pending, std::nullopt);
		if (auto previous = std::exchange(this->worker, {})) previous->stop();
		this->refresh.reset();
		this->deadline.stop();
		this->pinUnavailable = false;
		this->retirePinWorker(false);
		this->snapshot.availability = DesktopAvailability::Disconnected;
		this->snapshot.error = error;
		// Keep the last desktop/window values as explicitly stale state.
		this->recovery.disconnected();
		this->notify();
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
		if ((result.availability == DesktopAvailability::Available || result.publicReady)
		    && result.windows != this->snapshot.windows)
			++this->windowRevision;
		if (result.availability != DesktopAvailability::Available) {
			const auto state = result.availability;
			const auto error = result.error;
			if (result.publicReady) {
				this->snapshot.windows = std::move(result.windows);
				this->snapshot.availability = state;
				this->snapshot.error = error;
				this->pinUnavailable = false;
				this->retirePinWorker(false);
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
				this->pinUnavailable = false;
				this->retirePinWorker(false);
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
			             testPinned = this->testPinnedApps,
			             pinnedShell = this->pinnedAppsShell
#endif
			] {
				next->run(
#ifdef QS_TEST
				    testMode,
				    ownedViews,
				    validator,
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
		return true;
	}

	OwnWindowPin requestPin(HWND hwnd, quint64* outRequestId = nullptr) {
		if (outRequestId) *outRequestId = 0;
		if (this->waiting) return OwnWindowPin::Busy;
		this->collectPin();
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
		request->completed = [this] { this->wakeOnPinCompletion(); };
		const auto reqId = ++this->pinSequence;
		this->pin.emplace(PinPending {request, this->pinWorker->submit(request), hwnd, reqId});
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
		const auto next = since + this->stallLimit;
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
				this->pinUnavailable = false;
				this->retirePinWorker(false);
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
		if (this->pin
		    && this->pin->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
		{
			auto finished = std::move(*this->pin);
			this->pin.reset();
			try {
				auto value = finished.result.get();
				if (value.optionalPinnedAppsFaulted) {
					qCWarning(logNativeDesktops)
					    << "Optional desktop pinning faulted; retiring pin worker and disabling pinning";
					this->retirePinWorker(true);
					this->notifyPinCompleted(
					    finished.requestId,
					    finished.target,
					    OwnWindowPin::Unavailable
					);
					return;
				}
				if (value.disconnected) {
					this->retire(value.error);
					this->notifyPinCompleted(
					    finished.requestId,
					    finished.target,
					    OwnWindowPin::Unavailable
					);
					return;
				}
				if (!value.serviceAvailable) this->pinUnavailable = true;
				const auto outcome = value.result == 0
				                       ? OwnWindowPin::Pinned
				                       : (value.result == -2 ? OwnWindowPin::Unavailable
				                                             : OwnWindowPin::Refused);
				this->notifyPinCompleted(finished.requestId, finished.target, outcome);
			} catch (...) {
				this->retirePinWorker(true);
				this->notifyPinCompleted(
				    finished.requestId,
				    finished.target,
				    OwnWindowPin::Unavailable
				);
			}
			this->armPinDeadline();
			return;
		}
		if (this->pin && this->pin->request->stalled(this->stallLimit)) {
			qCWarning(logNativeDesktops)
			    << "Optional desktop pinning stalled; retiring pin worker and disabling pinning";
			this->retirePinWorker(true);
			return;
		}
		this->armPinDeadline();
	}

	void collect() {
		this->collectOne(this->refresh);
		this->collectOne(this->pending);
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
    std::function<void(quint64, HWND, OwnWindowPin)> onPinCompleted
) {
	auto& c = channel();
	c.context = context;
	c.pinCompleted = std::move(onPinCompleted);
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
	return channel().requestPin(hwnd, outRequestId);
}
void shutdownNativeDesktopAccessor() { channel().shutdown(); }

#ifdef QS_TEST
void setNativeDesktopFactoryForTest(NativeDesktopFactory factory) {
	auto& p = channel();
	// Clear an injected test clock first: it may capture a caller's local that
	// is already out of scope when a RAII reset guard runs this.
	p.recovery.setAgeForTest({});
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
	p.supported = false;
	p.privateEnabled = false;
	p.refusedBuild = false;
	p.skipOptionalPinnedApps = false;
	p.pinUnavailable = false;
	p.applied = 0;
	p.desktopReadApplied = 0;
	p.cancelledCommandSequence = 0;
	p.failedSwitchSequence = 0;
	p.failedSwitchError = S_OK;
	p.windowRevision = 0;
	p.snapshot = {};
	p.watchedWindows.clear();
	p.factory = std::move(factory);
	p.validator = {};
	p.testPinnedApps = nullptr;
	p.pinnedAppsShell = nullptr;
	p.stallLimit = stuckBound;
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
bool nativeDesktopPendingReadyForTest() {
	const auto& c = channel();
	return c.pending && c.pending->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}
void setNativeDesktopStuckBoundForTest(int milliseconds) {
	channel().stallLimit = std::chrono::milliseconds(milliseconds);
}
#endif

} // namespace qs::windows
