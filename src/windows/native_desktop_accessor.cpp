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

struct Provider {
#ifdef QS_TEST
	NativeDesktopFactory factory;
	std::function<bool(HWND)> windowValidator;
#endif
	ComPtr<DesktopManager> manager;
	ComPtr<ViewCollection> views;
	ComPtr<IVirtualDesktopManager> publicApi;
	QList<ComPtr<Desktop>> snapshot;
	QList<GUID> ids;
	int current = -1;
	bool structuredException = false;
	bool privateEnabled = true;
	DesktopAvailability availability = DesktopAvailability::MissingProvider;
	HRESULT lastError = S_OK;

	void dropPrivate() {
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
			} else if (this->structuredException || this->lastError == RPC_E_DISCONNECTED
			           || this->lastError == CO_E_OBJNOTCONNECTED
			           || this->lastError == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
			{
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
	int result = -1;
	quint64 sequence = 0;
	bool publicReady = false;
};

struct Request {
	HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	~Request() {
		if (this->finished) CloseHandle(this->finished);
	}
	std::function<int()> operation;
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

struct Worker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<Request>> queue;
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

	std::future<Snapshot> submit(const std::shared_ptr<Request>& request) {
		auto result = request->completion.get_future();
		{
			std::lock_guard lock(this->mutex);
			// The GUI permits only one refresh and one synchronous command.
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
				snapshot.result = FAILED(apartment) ? -1 : request->operation();
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
			} catch (...) {
				snapshot.availability = DesktopAvailability::Disconnected;
				snapshot.error = E_UNEXPECTED;
			}
			request->completion.set_value(std::move(snapshot));
			SetEvent(request->finished);
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

struct Channel {
	enum class Kind { Command, Acquisition, Probe };
	struct Pending {
		std::shared_ptr<Request> request;
		std::future<Snapshot> result;
		Kind kind = Kind::Command;
		bool queryWindows = false;
	};
	std::shared_ptr<Worker> worker;
	Snapshot snapshot;
	QSet<quintptr> watchedWindows;
	QPointer<QObject> context;
	std::function<void()> recovered;
	std::function<void()> updated;
	QTimer poll;
	std::optional<Pending> refresh;
	std::optional<Pending> pending;
	std::chrono::milliseconds stallLimit = stuckBound;
	bool supported = false;
	bool privateEnabled = false;
	bool refusedBuild = false;
	bool refreshAgain = false;
	bool waiting = false;
	quint64 sequence = 0;
	quint64 applied = 0;
	quint64 windowRevision = 0;
#ifdef QS_TEST
	NativeDesktopFactory factory;
	std::function<bool(HWND)> validator;
#endif
	DesktopRecovery recovery {
	    [this] { return this->acquire(); },
	    [this] {
		    this->notify();
		    if (this->context && this->recovered) this->recovered();
	    }
	};

	Channel() {
		this->poll.setInterval(10);
		QObject::connect(&this->poll, &QTimer::timeout, &this->poll, [this] { this->collect(); });
		if (auto* app = QCoreApplication::instance()) {
			QObject::connect(app, &QCoreApplication::aboutToQuit, &this->poll, [this] {
				this->shutdown();
			});
		}
	}

	void shutdown() {
		this->poll.stop();
		this->recovery.reset();
		if (auto previous = std::exchange(this->worker, {})) previous->stop();
		this->refresh.reset();
		this->pending.reset();
		this->refreshAgain = false;
	}

	void notify() {
		// No event loop runs inside a bounded command wait. Notifications
		// always run later on the GUI, never reenter its command dispatch.
		if (this->context && this->updated) QTimer::singleShot(0, this->context, this->updated);
	}

	void retire(HRESULT error) {
		if (auto previous = std::exchange(this->worker, {})) previous->stop();
		this->refresh.reset();
		this->pending.reset();
		this->poll.stop();
		this->snapshot.availability = DesktopAvailability::Disconnected;
		this->snapshot.error = error;
		// Keep the last desktop/window values as explicitly stale state.
		this->recovery.disconnected();
		this->notify();
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
				this->recovery.disconnected();
				return false;
			}
			this->retire(error);
			this->snapshot.availability = state;
			return false;
		}
		this->snapshot = std::move(result);
		return true;
	}

	int execute(
	    std::function<int()> operation,
	    bool queryWindows = false,
	    bool publicOperation = false,
	    Kind kind = Kind::Command
	) {
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
		request->operation = std::move(operation);
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
			this->pending.emplace(Pending {request, std::move(result), kind, queryWindows});
			this->poll.start();
			return kind == Kind::Command ? 0 : -1;
		}
		try {
			auto value = result.get();
			const auto outcome = value.result;
			const auto publicReady = value.publicReady;
			// A command snapshot must not erase the last window membership.
			if (!queryWindows) value.windows = this->snapshot.windows;
			const auto available = this->apply(std::move(value));
			if (kind == Kind::Acquisition && available) ++this->windowRevision;
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
		request->windows = this->watchedWindows.values();
		this->refresh.emplace(Pending {request, this->worker->submit(request), Kind::Probe, true});
		this->poll.start();
	}

	bool stalled() const {
		const auto stalled = [this](const std::optional<Pending>& job) {
			return job && job->result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
			    && job->request->stalled(this->stallLimit);
		};
		return stalled(this->refresh) || stalled(this->pending);
	}

	void collectOne(std::optional<Pending>& slot) {
		if (!slot || slot->result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
			return;
		auto finished = std::move(*slot);
		slot.reset();
		try {
			auto value = finished.result.get();
			const auto success = value.result != -1;
			if (!finished.queryWindows) value.windows = this->snapshot.windows;
			if (value.sequence < this->applied) this->refreshAgain = true;
			const auto available = this->apply(std::move(value));
			if (finished.kind == Kind::Acquisition && success && available) {
				++this->windowRevision;
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
			this->retire(E_UNEXPECTED);
		}
	}

	void collect() {
		this->collectOne(this->refresh);
		this->collectOne(this->pending);
		if (this->stalled()) {
			this->retire(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			return;
		}
		if (!this->refresh && !this->pending) {
			this->poll.stop();
			if (std::exchange(this->refreshAgain, false)) this->requestRefresh();
		}
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
	const auto id = cachedId(index);
	return channel().execute([id] { return switchDesktop(workerIndex(id)); });
}
int boundedCreate() {
	return channel().execute([] { return createDesktop(); });
}
int boundedMove(HWND hwnd, int index) {
	const auto id = cachedId(index);
	const auto result = channel().execute([hwnd, id] { return moveWindow(hwnd, workerIndex(id)); });
	if (result == 0) channel().requestRefresh();
	return result;
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
	// Removal, pinning and notification hooks intentionally remain absent.
	p.shutdown();
	p.privateEnabled = true;
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
	QObject::connect(context, &QObject::destroyed, &c.poll, [&c] {
		c.shutdown();
		c.context = nullptr;
		c.recovered = {};
		c.updated = {};
	});
	if (!c.worker && !c.recovery.active() && !c.acquire() && !c.pending) c.recovery.disconnected();
}

void requestNativeDesktopRefresh() { channel().requestRefresh(); }
bool nativeDesktopCommandPending() {
	const auto& c = channel();
	return c.waiting || (c.pending && c.pending->kind == Channel::Kind::Command);
}
bool nativeDesktopRequestPending() {
	const auto& c = channel();
	return c.waiting || c.pending.has_value() || c.refresh.has_value();
}
quint64 nativeDesktopWindowSnapshotRevision() { return channel().windowRevision; }
GUID nativeWindowDesktopId(HWND hwnd) { return channel().window(hwnd).id; }
int nativeWindowOnCurrentDesktop(HWND hwnd) { return channel().window(hwnd).onCurrent; }
std::optional<bool> moveNativeDesktopOwnWindow(HWND hwnd, GUID desktop) {
	if (!channel().worker) return std::nullopt;
	const auto result = channel().execute(
	    [hwnd, desktop] {
		    auto& p = provider();
		    if (!p.publicApi) return -2;
		    return p.invoke(
		               [&] { return p.publicApi->MoveWindowToDesktop(hwnd, desktop); },
		               "MoveWindowToDesktop"
		           )
		             ? 0
		             : -1;
	    },
	    false,
	    true
	);
	if (result == 0) channel().requestRefresh();
	return result == -2 ? std::nullopt : std::optional<bool>(result == 0);
}
void shutdownNativeDesktopAccessor() { channel().shutdown(); }

#ifdef QS_TEST
void setNativeDesktopFactoryForTest(NativeDesktopFactory factory) {
	auto& p = channel();
	// Clear an injected test clock first: it may capture a caller's local that
	// is already out of scope when a RAII reset guard runs this.
	p.recovery.setAgeForTest({});
	auto previous = p.worker;
	p.shutdown();
	// Test fixtures own their fakes on the stack. A bounded teardown barrier
	// lets them verify cleanup before destroying those fakes.
	if (previous) previous->exit.wait_for(callBound);
	p.context = nullptr;
	p.recovered = {};
	p.updated = {};
	p.supported = false;
	p.privateEnabled = false;
	p.refusedBuild = false;
	p.applied = 0;
	p.windowRevision = 0;
	p.snapshot = {};
	p.watchedWindows.clear();
	p.factory = std::move(factory);
	p.validator = {};
	p.stallLimit = stuckBound;
}

void setNativeDesktopWindowValidatorForTest(std::function<bool(HWND)> validator) {
	auto& c = channel();
	c.validator = validator;
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
int nativeDesktopWorkerCountForTest() { return liveWorkers().load(); }
void setNativeDesktopStuckBoundForTest(int milliseconds) {
	channel().stallLimit = std::chrono::milliseconds(milliseconds);
}
#endif

} // namespace qs::windows
