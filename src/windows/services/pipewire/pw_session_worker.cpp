#include "pw_session_worker.hpp"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <qcoreapplication.h>
#include <qcoreevent.h>
#include <qhash.h>
#include <qtimer.h>
#include <wrl/client.h>

#include "com_util.hpp"

namespace qs::windows::services::pipewire {
namespace {
using Microsoft::WRL::ComPtr;
QString takeComString(LPWSTR str) {
	if (!str) return {};
	auto result = QString::fromWCharArray(str);
	CoTaskMemFree(str);
	return result;
}
class SessionNotificationClient final
    : public IAudioSessionNotification
    , public ComCallbackTarget<PwSessionWorker> {
public:
	SessionNotificationClient(PwSessionWorker* target, QString endpointId)
	    : ComCallbackTarget(target)
	    , mEndpointId(std::move(endpointId)) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
		if (ppvObject == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
			*ppvObject = static_cast<IAudioSessionNotification*>(this);
			this->AddRef();
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return static_cast<ULONG>(InterlockedIncrement(&this->mRefCount));
	}

	ULONG STDMETHODCALLTYPE Release() override {
		auto rc = InterlockedDecrement(&this->mRefCount);
		if (rc == 0) delete this;
		return static_cast<ULONG>(rc);
	}

	HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* newSession) override {
		if (newSession == nullptr) return S_OK;

		auto endpointId = this->mEndpointId;
		// Core Audio callbacks and this worker share the process MTA. Retain the
		// session until the worker has adopted it (enumeration alone can miss it).
		this->post([session = Microsoft::WRL::ComPtr<IAudioSessionControl>(newSession),
		            endpointId](PwSessionWorker* worker) {
			worker->adoptSession(endpointId, session.Get());
		});

		return S_OK;
	}

private:
	volatile LONG mRefCount = 1;
	const QString mEndpointId;
};

class SessionEventsCallback final
    : public IAudioSessionEvents
    , public ComCallbackTarget<PwSessionWorker> {
public:
	SessionEventsCallback(PwSessionWorker* target, QString sessionKey)
	    : ComCallbackTarget(target)
	    , mSessionKey(std::move(sessionKey)) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
		if (ppvObject == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
			*ppvObject = static_cast<IAudioSessionEvents*>(this);
			this->AddRef();
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return static_cast<ULONG>(InterlockedIncrement(&this->mRefCount));
	}

	ULONG STDMETHODCALLTYPE Release() override {
		auto rc = InterlockedDecrement(&this->mRefCount);
		if (rc == 0) delete this;
		return static_cast<ULONG>(rc);
	}

	HRESULT STDMETHODCALLTYPE
	OnSimpleVolumeChanged(float /*newVolume*/, BOOL /*newMute*/, LPCGUID /*eventContext*/) override {
		auto key = this->mSessionKey;
		this->post([key](PwSessionWorker* backend) { backend->refreshSession(key); });
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState newState) override {
		auto key = this->mSessionKey;
		auto state = static_cast<int>(newState);
		this->post([key, state](PwSessionWorker* backend) {
			backend->sessionStateChanged(key, state);
		});
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override {
		auto key = this->mSessionKey;
		this->post([key, reason](PwSessionWorker* backend) {
			backend->sessionDisconnected(key, reason == DisconnectReasonServerShutdown);
		});
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE
	OnDisplayNameChanged(LPCWSTR /*newName*/, LPCGUID /*eventContext*/) override {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE
	OnIconPathChanged(LPCWSTR /*newPath*/, LPCGUID /*eventContext*/) override {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(
	    DWORD /*channelCount*/,
	    float* /*newVolumes*/,
	    DWORD /*changedChannel*/,
	    LPCGUID /*eventContext*/
	) override {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE
	OnGroupingParamChanged(LPCGUID /*newGroupingParam*/, LPCGUID /*eventContext*/) override {
		return S_OK;
	}

private:
	volatile LONG mRefCount = 1;
	const QString mSessionKey;
};

} // namespace

struct PwSessionWorker::Private {
	struct Endpoint {
		int flow = 0;
		ComPtr<IAudioSessionManager2> manager;
		SessionNotificationClient* notify = nullptr;
	};
	struct Session {
		SessionSnapshot snapshot;
		ComPtr<IAudioSessionControl2> control;
		ComPtr<ISimpleAudioVolume> volume;
		SessionEventsCallback* events = nullptr;
	};
	ManagerFactory factory;
	ComPtr<IMMDeviceEnumerator> enumerator;
	QHash<QString, Endpoint> endpoints;
	QHash<QString, Session> sessions;
	QTimer* retry = nullptr;
	bool comInitialized = false;
};

PwSessionWorker::PwSessionWorker(ManagerFactory factory): d(std::make_unique<Private>()) {
	this->d->factory = std::move(factory);
}
PwSessionWorker::~PwSessionWorker() = default;

void PwSessionWorker::start() {
	auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hr)) {
		emit this->error(audioControlError(QStringLiteral("Session MTA initialization"), hr));
		return;
	}
	this->d->comInitialized = true;
	this->d->retry = new QTimer(this);
	this->d->retry->setInterval(2000);
	connect(this->d->retry, &QTimer::timeout, this, [this] {
		const auto ids = this->d->endpoints.keys();
		for (const auto& id: ids) {
			if (!this->d->endpoints[id].manager) this->attachEndpoint(id, this->d->endpoints[id].flow);
		}
	});
	this->d->retry->start();
}

void PwSessionWorker::stop() {
	if (this->d->retry) this->d->retry->stop();
	const auto ids = this->d->endpoints.keys();
	for (const auto& id: ids) this->removeEndpoint(id);
	this->d->enumerator.Reset();
	// Detach callbacks before cancelling queued MTA closures holding COM refs.
	QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
	if (this->d->comInitialized) CoUninitialize();
	this->d->comInitialized = false;
}

void PwSessionWorker::attachEndpoint(const QString& id, int flow) {
	if (!this->d->comInitialized) return;
	auto& endpoint = this->d->endpoints[id];
	endpoint.flow = flow;
	if (endpoint.manager) return;
	auto fail = [this](const QString& operation, HRESULT hr) {
		emit this->error(audioControlError(operation, hr));
	};
	HRESULT hr = S_OK;
	if (this->d->factory) {
		hr = this->d->factory(id, endpoint.manager.GetAddressOf());
	} else {
		if (!this->d->enumerator) {
			hr = CoCreateInstance(
			    __uuidof(MMDeviceEnumerator),
			    nullptr,
			    CLSCTX_ALL,
			    IID_PPV_ARGS(&this->d->enumerator)
			);
			if (FAILED(hr)) {
				fail(QStringLiteral("Session device enumerator"), hr);
				return;
			}
		}
		ComPtr<IMMDevice> device;
		hr = this->d->enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device);
		if (FAILED(hr)) {
			this->d->enumerator.Reset();
			fail(QStringLiteral("Session GetDevice"), hr);
			return;
		}
		hr = device->Activate(
		    __uuidof(IAudioSessionManager2),
		    CLSCTX_ALL,
		    nullptr,
		    reinterpret_cast<void**>(endpoint.manager.GetAddressOf())
		);
	}
	if (FAILED(hr)) {
		fail(QStringLiteral("Activate(IAudioSessionManager2)"), hr);
		return;
	}
	auto* notify = new SessionNotificationClient(this, id);
	hr = endpoint.manager->RegisterSessionNotification(notify);
	if (FAILED(hr)) {
		notify->detach();
		notify->Release();
		endpoint.manager.Reset();
		fail(QStringLiteral("RegisterSessionNotification"), hr);
		return;
	}
	endpoint.notify = notify;
	this->scanEndpoint(id);
}

void PwSessionWorker::scanEndpoint(const QString& id) {
	auto it = this->d->endpoints.find(id);
	if (it == this->d->endpoints.end() || !it->manager) return;
	ComPtr<IAudioSessionEnumerator> sessions;
	auto hr = it->manager->GetSessionEnumerator(&sessions);
	int count = 0;
	if (SUCCEEDED(hr)) hr = sessions->GetCount(&count); // Arms new-session delivery.
	if (FAILED(hr)) {
		emit this->error(audioControlError(QStringLiteral("Enumerate audio sessions"), hr));
		const auto flow = it->flow;
		this->removeEndpoint(id);
		this->d->endpoints[id].flow = flow;
		return;
	}
	for (int i = 0; i < count; ++i) {
		ComPtr<IAudioSessionControl> control;
		if (SUCCEEDED(sessions->GetSession(i, &control))) this->adoptSession(id, control.Get());
	}
}

void PwSessionWorker::removeEndpoint(const QString& id) {
	auto it = this->d->endpoints.find(id);
	if (it == this->d->endpoints.end()) return;
	if (it->notify) {
		it->notify->detach();
		it->manager->UnregisterSessionNotification(it->notify);
		it->notify->Release();
	}
	this->d->endpoints.erase(it);
	const auto keys = this->d->sessions.keys();
	for (const auto& key: keys) {
		if (this->d->sessions[key].snapshot.endpointId == id) this->sessionDisconnected(key, false);
	}
}

void PwSessionWorker::adoptSession(const QString& endpointId, IAudioSessionControl* control) {
	auto endpoint = this->d->endpoints.constFind(endpointId);
	if (!control || endpoint == this->d->endpoints.cend() || !endpoint->manager) return;
	Private::Session entry;
	if (FAILED(control->QueryInterface(IID_PPV_ARGS(&entry.control)))) return;
	if (entry.control->IsSystemSoundsSession() == S_OK) return;
	AudioSessionState state = AudioSessionStateInactive;
	if (FAILED(entry.control->GetState(&state)) || state == AudioSessionStateExpired) return;
	DWORD pid = 0;
	if (FAILED(entry.control->GetProcessId(&pid)) || !pid) return;
	LPWSTR raw = nullptr;
	if (FAILED(entry.control->GetSessionInstanceIdentifier(&raw))) return;
	auto instance = takeComString(raw);
	auto& value = entry.snapshot;
	value.key = endpointId + QLatin1Char('|') + QString::number(pid) + QLatin1Char('-') + instance;
	if (this->d->sessions.contains(value.key)) return;
	value.endpointId = endpointId;
	value.flow = endpoint->flow;
	value.state = static_cast<int>(state);
	QString binary = QStringLiteral("unknown");
	if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t buffer[MAX_PATH];
		DWORD len = MAX_PATH;
		if (QueryFullProcessImageNameW(process, 0, buffer, &len)) {
			auto full = QString::fromWCharArray(buffer, static_cast<int>(len));
			binary = full.mid(full.lastIndexOf(QLatin1Char('\\')) + 1);
		}
		CloseHandle(process);
	}
	value.name = binary;
	auto dot = value.name.lastIndexOf(QLatin1Char('.'));
	if (dot > 0) value.name.truncate(dot);
	raw = nullptr;
	if (SUCCEEDED(entry.control->GetDisplayName(&raw))) value.nickname = takeComString(raw);
	value.description = value.nickname.isEmpty() ? value.name : value.nickname;
	value.properties = {
	    {QStringLiteral("application.name"), value.description},
	    {QStringLiteral("application.process.binary"), binary},
	    {QStringLiteral("application.icon-name"), value.name},
	    {QStringLiteral("node.name"), value.name}
	};
	auto hr = entry.control.As(&entry.volume);
	value.controlAvailable = SUCCEEDED(hr);
	if (FAILED(hr))
		value.error = audioControlError(QStringLiteral("QueryInterface(ISimpleAudioVolume)"), hr);
	auto* events = new SessionEventsCallback(this, value.key);
	hr = entry.control->RegisterAudioSessionNotification(events);
	if (FAILED(hr)) {
		events->detach();
		events->Release();
		emit this->error(audioControlError(QStringLiteral("RegisterAudioSessionNotification"), hr));
		// Retry this endpoint rather than silently retaining an unobservable list.
		const auto flow = endpoint->flow;
		this->removeEndpoint(endpointId);
		this->d->endpoints[endpointId].flow = flow;
		return;
	}
	entry.events = events;
	auto key = value.key;
	this->d->sessions.insert(key, std::move(entry));
	this->refreshSession(key);
}

void PwSessionWorker::refreshSession(const QString& key) {
	auto it = this->d->sessions.find(key);
	if (it == this->d->sessions.end()) return;
	auto& value = it->snapshot;
	if (it->volume) {
		BOOL muted = FALSE;
		float volume = 0;
		auto hr = it->volume->GetMute(&muted);
		if (SUCCEEDED(hr)) hr = it->volume->GetMasterVolume(&volume);
		value.stateAvailable = SUCCEEDED(hr);
		if (SUCCEEDED(hr)) {
			value.muted = muted != FALSE;
			value.volume = volume;
			value.error.clear();
		} else value.error = audioControlError(QStringLiteral("Read session volume"), hr);
	}
	emit this->sessionChanged(value);
}

void PwSessionWorker::setVolume(const QString& key, float volume) {
	auto it = this->d->sessions.find(key);
	if (it == this->d->sessions.end() || !it->volume) return;
	auto hr = it->volume->SetMasterVolume(volume, nullptr);
	this->refreshSession(key);
	if (FAILED(hr)) {
		it->snapshot.error = audioControlError(QStringLiteral("SetMasterVolume"), hr);
		emit this->sessionChanged(it->snapshot);
	}
}

void PwSessionWorker::setMuted(const QString& key, bool muted) {
	auto it = this->d->sessions.find(key);
	if (it == this->d->sessions.end() || !it->volume) return;
	auto hr = it->volume->SetMute(muted ? TRUE : FALSE, nullptr);
	this->refreshSession(key);
	if (FAILED(hr)) {
		it->snapshot.error = audioControlError(QStringLiteral("SetMute"), hr);
		emit this->sessionChanged(it->snapshot);
	}
}

void PwSessionWorker::sessionStateChanged(const QString& key, int state) {
	if (state == AudioSessionStateExpired) {
		this->sessionDisconnected(key, false);
		return;
	}
	auto it = this->d->sessions.find(key);
	if (it == this->d->sessions.end()) return;
	it->snapshot.state = state;
	this->refreshSession(key);
}

void PwSessionWorker::sessionDisconnected(const QString& key, bool serviceStopped) {
	auto it = this->d->sessions.find(key);
	if (it == this->d->sessions.end()) return;
	auto id = it->snapshot.endpointId;
	if (it->events) {
		it->events->detach();
		it->control->UnregisterAudioSessionNotification(it->events);
		it->events->Release();
	}
	this->d->sessions.erase(it);
	emit this->sessionRemoved(key);
	if (serviceStopped) {
		auto endpoint = this->d->endpoints.constFind(id);
		if (endpoint == this->d->endpoints.cend()) return;
		auto flow = endpoint->flow;
		this->removeEndpoint(id);
		this->d->endpoints[id].flow = flow;
		this->d->enumerator.Reset();
	}
}

} // namespace qs::windows::services::pipewire
