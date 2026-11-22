#include "seed.hpp"
#include <future>
#include <utility>

#include <objbase.h>
#include <shellapi.h>
#include <unknwn.h>

namespace qs::windows::services::systray {

namespace {

constexpr UINT RESYNC_MESSAGE = WM_APP + 1;

MIDL_INTERFACE("D782CCBA-AFB0-43F1-94DB-FDA3779EACCB")
INotificationCB: public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE Notify(ULONG event, const TrayNotifyItem* item) = 0;
};

MIDL_INTERFACE("D133CE13-3537-48BA-93A7-AFCD5D2053B4")
ITrayNotifyWin8: public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* callback, ULONG* handle) = 0;
	virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(ULONG* handle) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetPreference(const TrayNotifyItem* item) = 0;
	virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enable) = 0;
	virtual HRESULT STDMETHODCALLTYPE DoAction(BOOL action) = 0;
};

MIDL_INTERFACE("FB852B2C-6BAD-4605-9551-F15F87830935")
ITrayNotifyWin7: public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* callback) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetPreference(const TrayNotifyItem* item) = 0;
	virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enable) = 0;
};

constexpr CLSID CLSID_TRAY_NOTIFY =
    {0x25dead04, 0x1eac, 0x4911, {0x9e, 0x3a, 0xad, 0x0a, 0x4a, 0xb5, 0x60, 0xfd}};

class NotificationCallback: public INotificationCB {
public:
	explicit NotificationCallback(TrayNotifyBackend::Callback notify): notify(std::move(notify)) {}
	virtual ~NotificationCallback() = default;
	Q_DISABLE_COPY_MOVE(NotificationCallback);

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
		if (object == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(INotificationCB)) {
			*object = static_cast<INotificationCB*>(this);
			this->AddRef();
			return S_OK;
		}

		*object = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }

	ULONG STDMETHODCALLTYPE Release() override {
		auto refs = --this->refs;
		if (refs == 0) delete this;
		return refs;
	}

	HRESULT STDMETHODCALLTYPE Notify(ULONG event, const TrayNotifyItem* item) override {
		if (this->notify) this->notify(event, item);
		return S_OK;
	}

	void deactivate() { this->notify = {}; }

private:
	std::atomic<ULONG> refs = 1;
	TrayNotifyBackend::Callback notify;
};

class ExplorerTrayNotifyBackend: public TrayNotifyBackend {
public:
	Registration registerCallback(Callback notify) override {
		IUnknown* unknown = nullptr;
		auto hr = CoCreateInstance(
		    CLSID_TRAY_NOTIFY,
		    nullptr,
		    CLSCTX_LOCAL_SERVER,
		    IID_PPV_ARGS(&unknown)
		);
		if (FAILED(hr) || unknown == nullptr) return {FAILED(hr) ? hr : E_NOINTERFACE};

		hr = unknown->QueryInterface(IID_PPV_ARGS(&this->win8));
		if (hr == E_NOINTERFACE) hr = unknown->QueryInterface(IID_PPV_ARGS(&this->win7));
		unknown->Release();
		if (FAILED(hr) || (this->win8 == nullptr && this->win7 == nullptr))
			return {FAILED(hr) ? hr : E_NOINTERFACE};

		this->callback = new NotificationCallback(std::move(notify));
		auto hasHandle = this->win8 != nullptr;
		hr = hasHandle ? this->win8->RegisterCallback(this->callback, &this->handle)
		               : this->win7->RegisterCallback(this->callback);
		this->registered = SUCCEEDED(hr);
		return {hr, this->handle, hasHandle};
	}

	void unregisterCallback() override {
		if (this->callback != nullptr) this->callback->deactivate();

		if (this->registered) {
			if (this->win8 != nullptr) this->win8->UnregisterCallback(&this->handle);
			else this->win7->RegisterCallback(nullptr);
		}

		if (this->callback != nullptr) {
			CoDisconnectObject(this->callback, 0);
			std::exchange(this->callback, nullptr)->Release();
		}
		if (this->win8 != nullptr) std::exchange(this->win8, nullptr)->Release();
		if (this->win7 != nullptr) std::exchange(this->win7, nullptr)->Release();
		this->registered = false;
		this->handle = 0;
	}

private:
	ITrayNotifyWin8* win8 = nullptr;
	ITrayNotifyWin7* win7 = nullptr;
	NotificationCallback* callback = nullptr;
	ULONG handle = 0;
	bool registered = false;
};

void deliverIcon(const TrayIconSink& sink, ULONG event, const TrayNotifyItem* item, bool seeded) {
	if (!sink || item == nullptr || event > 2) return;
	if (event != 2 && (item->hwnd == nullptr || !IsWindow(item->hwnd))) return;

	TrayIconMessage message;
	message.message = event == 0 ? NIM_ADD : event == 1 ? NIM_MODIFY : NIM_DELETE;
	message.seeded = seeded;
	message.fromObserver = true;
	message.hwnd = item->hwnd;
	message.uid = item->id;

	if (item->guid != GUID_NULL) {
		message.flags |= NIF_GUID;
		message.guid = QUuid(item->guid);
	}

	if (event != 2) {
		// NOTIFYITEM has no callback message or notify icon version.
		message.flags |= NIF_ICON | NIF_TIP;
		message.icon = imageFromIcon(item->icon);
		if (item->tip != nullptr) message.tip = QString::fromWCharArray(item->tip);
		if (item->exeName != nullptr) message.exePath = QString::fromWCharArray(item->exeName);
	}

	sink(std::move(message));
}

bool transientFailure(HRESULT result) {
	switch (result) {
	case CO_E_SERVER_EXEC_FAILURE:
	case RPC_E_CALL_REJECTED:
	case RPC_E_DISCONNECTED:
	case RPC_E_SERVER_DIED:
	case RPC_E_SERVER_DIED_DNE:
	case RPC_E_SERVERCALL_RETRYLATER:
	case HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE):
	case HRESULT_FROM_WIN32(ERROR_TIMEOUT): return true;
	default: return false;
	}
}

} // namespace

TrayNotifyObserver::TrayNotifyObserver(std::unique_ptr<TrayNotifyBackend> backend)
    : backend(backend ? std::move(backend) : std::make_unique<ExplorerTrayNotifyBackend>()) {}

TrayNotifyObserver::~TrayNotifyObserver() { this->stop(); }

void TrayNotifyObserver::start(TrayIconSink sink, std::function<void(ComObserverState)> onState) {
	if (this->worker.joinable()) return;

	this->stopping.store(false);
	this->mState.store(ComObserverState::Connecting);
	std::promise<DWORD> ready;
	auto queueId = ready.get_future();
	this->worker = std::thread([this, ready = std::move(ready), sink = std::move(sink), onState = std::move(onState)]() mutable {
		SetThreadDescription(GetCurrentThread(), L"qs tray notify");

		MSG msg {};
		PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
		ready.set_value(GetCurrentThreadId());

		auto publish = [&](ComObserverState state) {
			this->mState.store(state);
			if (onState && !this->stopping.load()) onState(state);
		};

		const auto initialized =
		    SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
		auto connected = false;
		auto registering = false;
		auto receivedInitialCallback = false;

		auto disconnect = [&] {
			if (!connected) return;
			connected = false;
			this->backend->unregisterCallback();
		};

		auto connect = [&] {
			this->retryable.store(false);
			publish(ComObserverState::Connecting);
			if (!initialized) {
				publish(ComObserverState::Unavailable);
				return;
			}

			connected = true;
			registering = true;
			receivedInitialCallback = false;
			auto registration = this->backend->registerCallback([&](ULONG event, const TrayNotifyItem* item) {
				if (registering) receivedInitialCallback = true;
				if (connected && !this->stopping.load()) deliverIcon(sink, event, item, registering);
			});
			registering = false;

			auto state = ComObserverState::Unavailable;
			if (SUCCEEDED(registration.result)) {
				if (registration.handle != 0 || receivedInitialCallback) state = ComObserverState::Live;
				else if (registration.hasHandle) state = ComObserverState::StubNoOp;
			}
			if (state != ComObserverState::Live) disconnect();
			this->retryable.store(transientFailure(registration.result));
			publish(state);
		};

		if (!this->stopping.load()) connect();
		while (!this->stopping.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
			if (msg.hwnd == nullptr && msg.message == RESYNC_MESSAGE) {
				disconnect();
				if (!this->stopping.load()) connect();
			} else {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}

		disconnect();
		if (initialized) CoUninitialize();
	});
	this->threadId = queueId.get();
}

void TrayNotifyObserver::resync() {
	if (this->threadId != 0) PostThreadMessageW(this->threadId, RESYNC_MESSAGE, 0, 0);
}

void TrayNotifyObserver::stop() {
	if (!this->worker.joinable()) return;

	this->stopping.store(true);
	PostThreadMessageW(this->threadId, WM_QUIT, 0, 0);
	this->worker.join();
	this->threadId = 0;
	this->retryable.store(false);
	this->mState.store(ComObserverState::Inactive);
}

} // namespace qs::windows::services::systray
