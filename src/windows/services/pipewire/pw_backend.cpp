#include "pw_backend.hpp"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <processthreadsapi.h>
#include <propidl.h>
#include <propsys.h>
#include <qcontainerfwd.h>
#include <qhash.h>
#include <qlatin1stringview.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qvariant.h>
#include <wrl/client.h>

// Property-key definitions require initguid after the property-system headers.
// clang-format off
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
// clang-format on

#include "../../startup.hpp"
#include "../../redact.hpp"
#include "com_util.hpp"
#include "pipewire.hpp"
#include "policy_config.hpp"
#include "pw_link.hpp"
#include "pw_node.hpp"

namespace qs::windows::services::pipewire {

namespace {
Q_LOGGING_CATEGORY(logPwBackend, "quickshell.windows.pipewire.backend", QtWarningMsg);

HRESULT ensureComInitialized() {
	auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) return S_OK;
	qCWarning(logPwBackend) << "CoInitializeEx failed:" << Qt::hex << hr;
	return hr;
}

QString takeComString(LPWSTR str) {
	if (str == nullptr) return {};
	auto result = QString::fromWCharArray(str);
	CoTaskMemFree(str);
	return result;
}

QString propertyAsString(IPropertyStore* store, const PROPERTYKEY& key) {
	if (store == nullptr) return {};

	PROPVARIANT pv;
	PropVariantInit(&pv);

	QString result;
	if (SUCCEEDED(store->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
		result = QString::fromWCharArray(pv.pwszVal);
	}

	PropVariantClear(&pv);
	return result;
}

class MMNotificationClient final
    : public IMMNotificationClient
    , public ComCallbackTarget<PwBackend> {
public:
	explicit MMNotificationClient(PwBackend* target): ComCallbackTarget(target) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
		if (ppvObject == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
			*ppvObject = static_cast<IMMNotificationClient*>(this);
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

	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override {
		auto id = QString::fromWCharArray(deviceId);
		this->post([id, newState](PwBackend* backend) {
			backend->handleDeviceStateChanged(id, newState);
		});
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override {
		auto id = QString::fromWCharArray(deviceId);
		this->post([id](PwBackend* backend) { backend->handleDeviceAdded(id); });
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override {
		auto id = QString::fromWCharArray(deviceId);
		this->post([id](PwBackend* backend) { backend->handleDeviceRemoved(id); });
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE
	OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId) override {
		auto id = defaultDeviceId != nullptr ? QString::fromWCharArray(defaultDeviceId) : QString();
		auto flowInt = static_cast<int>(flow);
		auto roleInt = static_cast<int>(role);
		this->post([flowInt, roleInt, id](PwBackend* backend) {
			backend->handleDefaultDeviceChanged(flowInt, roleInt, id);
		});
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE
	OnPropertyValueChanged(LPCWSTR /*deviceId*/, const PROPERTYKEY /*key*/) override {
		return S_OK;
	}

private:
	volatile LONG mRefCount = 1;
};

} // namespace

PwBackend::PwBackend(Pipewire* owner): owner(owner) {
	this->sessionWorker = new PwSessionWorker;
	this->sessionWorker->moveToThread(&this->sessionThread);
	connect(&this->sessionThread, &QThread::started, this->sessionWorker, &PwSessionWorker::start);
	connect(&this->sessionThread, &QThread::finished, this->sessionWorker, &QObject::deleteLater);
	connect(
	    this->sessionWorker,
	    &PwSessionWorker::sessionChanged,
	    this,
	    &PwBackend::applySessionSnapshot,
	    Qt::QueuedConnection
	);
	connect(
	    this->sessionWorker,
	    &PwSessionWorker::sessionRemoved,
	    this,
	    &PwBackend::handleSessionDisconnected,
	    Qt::QueuedConnection
	);
	connect(
	    this->sessionWorker,
	    &PwSessionWorker::error,
	    this,
	    [this](const QString& error) { this->owner->backendSetError(error); },
	    Qt::QueuedConnection
	);
	this->sessionThread.setObjectName(QStringLiteral("CoreAudioSessionsMTA"));
	this->sessionThread.start();
}

PwBackend::~PwBackend() {
	// No worker waits for the GUI. Revoke callbacks and release every session
	// interface on the MTA before ending its COM apartment and event loop.
	QMetaObject::invokeMethod(
	    this->sessionWorker,
	    &PwSessionWorker::stop,
	    Qt::BlockingQueuedConnection
	);
	this->sessionThread.quit();
	this->sessionThread.wait();
	this->sessionWorker = nullptr;
	if (this->notificationClient != nullptr && this->enumerator != nullptr) {
		auto* client = static_cast<MMNotificationClient*>(this->notificationClient);
		this->enumerator->UnregisterEndpointNotificationCallback(client);
		client->detach();
		client->Release();
	}

	const auto sessionKeys = this->sessions.keys();
	for (const auto& key: sessionKeys) {
		this->removeSessionNode(key);
	}

	const auto endpointIds = this->endpoints.keys();
	for (const auto& id: endpointIds) {
		this->removeEndpoint(id);
	}

	if (this->policyConfig != nullptr) {
		static_cast<IPolicyConfig*>(this->policyConfig)->Release();
	}

	if (this->enumerator != nullptr) this->enumerator->Release();
}

void PwBackend::start() {
	auto comResult = ensureComInitialized();
	if (FAILED(comResult)) {
		this->owner->backendSetError(audioControlError(QStringLiteral("CoInitializeEx"), comResult));
		return;
	}

	auto hr = CoCreateInstance(
	    __uuidof(MMDeviceEnumerator),
	    nullptr,
	    CLSCTX_ALL,
	    __uuidof(IMMDeviceEnumerator),
	    reinterpret_cast<void**>(&this->enumerator)
	);

	if (FAILED(hr) || this->enumerator == nullptr) {
		this->owner->backendSetError(audioControlError(
		    QStringLiteral("CoCreateInstance(IMMDeviceEnumerator)"),
		    FAILED(hr) ? hr : E_NOINTERFACE
		));
		qCWarning(logPwBackend) << "Could not create IMMDeviceEnumerator:" << Qt::hex << hr;
		return;
	}

	auto* notify = new MMNotificationClient(this);
	hr = this->enumerator->RegisterEndpointNotificationCallback(notify);
	if (FAILED(hr)) {
		this->owner->backendSetError(
		    audioControlError(QStringLiteral("RegisterEndpointNotificationCallback"), hr)
		);
		notify->detach();
		notify->Release();
		return;
	}
	this->notificationClient = notify;

	this->addDefaultEndpoint(eRender);
	this->addDefaultEndpoint(eCapture);

	this->owner->backendSetReady(true);

	startup::afterFirstFrame(this, [this]() { this->startDeferred(); });
}

void PwBackend::startDeferred() {
	this->sessionsEnabled = true;
	const auto knownEndpoints = this->endpoints.keys();
	for (const auto& id: knownEndpoints) {
		this->attachSessions(id);
	}

	this->enumerateExistingDevices(eRender);
	this->enumerateExistingDevices(eCapture);

	this->refreshDefault(eRender);
	this->refreshDefault(eCapture);
}

void PwBackend::addDefaultEndpoint(int flow) {
	IMMDevice* device = nullptr;
	auto hr =
	    this->enumerator->GetDefaultAudioEndpoint(static_cast<EDataFlow>(flow), eConsole, &device);
	if (FAILED(hr) || device == nullptr) return;

	auto* node = this->createEndpointNode(device, flow);
	device->Release();

	if (flow == eRender) this->owner->backendSetDefaultSink(node);
	else this->owner->backendSetDefaultSource(node);
}

void PwBackend::enumerateExistingDevices(int flow) {
	IMMDeviceCollection* collection = nullptr;
	auto hr = this->enumerator->EnumAudioEndpoints(
	    static_cast<EDataFlow>(flow),
	    DEVICE_STATE_ACTIVE,
	    &collection
	);

	if (FAILED(hr) || collection == nullptr) {
		qCWarning(logPwBackend) << "EnumAudioEndpoints failed:" << Qt::hex << hr;
		return;
	}

	UINT count = 0;
	collection->GetCount(&count);

	for (UINT i = 0; i < count; i++) {
		IMMDevice* device = nullptr;
		if (SUCCEEDED(collection->Item(i, &device)) && device != nullptr) {
			this->createEndpointNode(device, flow);
			device->Release();
		}
	}

	collection->Release();
}

PwNode* PwBackend::createEndpointNode(IMMDevice* device, int flow) {
	LPWSTR rawId = nullptr;
	if (FAILED(device->GetId(&rawId)) || rawId == nullptr) return nullptr;
	auto deviceId = takeComString(rawId);

	if (this->endpoints.contains(deviceId)) return this->endpoints.value(deviceId).node;

	IPropertyStore* store = nullptr;
	QString description = deviceId;
	QString nickname;
	if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
		auto friendlyName = propertyAsString(store, PKEY_Device_FriendlyName);
		if (!friendlyName.isEmpty()) description = friendlyName;
		nickname = propertyAsString(store, PKEY_DeviceInterface_FriendlyName);
		store->Release();
	}

	auto* node = new PwNode(this->owner);
	node->setId(this->nextId++);
	node->setBackendKey(deviceId);
	node->setName(deviceId);
	node->setDescription(description);
	node->setNickname(nickname.isEmpty() ? description : nickname);
	node->setType(flow == eRender ? PwNodeType::AudioSink : PwNodeType::AudioSource);

	QVariantMap props;
	props[QStringLiteral("device.description")] = description;
	props[QStringLiteral("node.name")] = deviceId;
	props[QStringLiteral("media.class")] =
	    flow == eRender ? QStringLiteral("Audio/Sink") : QStringLiteral("Audio/Source");
	node->setProperties(props);

	IAudioEndpointVolume* endpointVolume = nullptr;
	auto volumeResult = device->Activate(
	    __uuidof(IAudioEndpointVolume),
	    CLSCTX_ALL,
	    nullptr,
	    reinterpret_cast<void**>(&endpointVolume)
	);
	if (SUCCEEDED(volumeResult) && endpointVolume != nullptr) {
		node->audio()->bindEndpoint(endpointVolume);
	} else {
		auto error = audioControlError(
		    QStringLiteral("Activate(IAudioEndpointVolume)"),
		    FAILED(volumeResult) ? volumeResult : E_NOINTERFACE
		);
		node->audio()->setControlError(error);
		qCWarning(logPwBackend) << error << "for" << qs::windows::logId(deviceId);
	}

	node->setMeterDevice(device);
	node->setReady(node->audio()->controlAvailable());

	EndpointEntry entry;
	entry.node = node;
	entry.device = device;
	entry.flow = flow;
	device->AddRef();

	this->endpoints.insert(deviceId, entry);
	this->owner->backendAddNode(node);

	if (this->sessionsEnabled) this->attachSessions(deviceId);

	return node;
}

void PwBackend::attachSessions(const QString& deviceId) {
	auto it = this->endpoints.constFind(deviceId);
	if (it == this->endpoints.cend() || !this->sessionWorker) return;
	auto flow = it->flow;
	QMetaObject::invokeMethod(
	    this->sessionWorker,
	    [worker = this->sessionWorker, deviceId, flow] { worker->attachEndpoint(deviceId, flow); },
	    Qt::QueuedConnection
	);
}

void PwBackend::removeEndpoint(const QString& deviceId) {
	auto it = this->endpoints.find(deviceId);
	if (it == this->endpoints.end()) return;
	auto entry = it.value();
	this->endpoints.erase(it);

	QStringList affectedSessions;
	for (auto sessionIt = this->sessions.cbegin(); sessionIt != this->sessions.cend(); ++sessionIt) {
		if (sessionIt.value().endpointId == deviceId) affectedSessions.append(sessionIt.key());
	}
	for (const auto& key: affectedSessions) this->removeSessionNode(key);

	if (this->sessionWorker) {
		QMetaObject::invokeMethod(
		    this->sessionWorker,
		    [worker = this->sessionWorker, deviceId] { worker->removeEndpoint(deviceId); },
		    Qt::QueuedConnection
		);
	}

	if (entry.device != nullptr) entry.device->Release();
	if (entry.node != nullptr) this->owner->backendRemoveNode(entry.node);
}

void PwBackend::applySessionSnapshot(const SessionSnapshot& value) {
	if (!this->endpoints.contains(value.endpointId)) return;
	auto it = this->sessions.find(value.key);
	if (it == this->sessions.end()) {
		auto* node = new PwNode(this->owner);
		node->setId(this->nextId++);
		node->setBackendKey(value.key);
		node->setName(value.name);
		node->setDescription(value.description);
		node->setNickname(value.nickname);
		node->setType(value.flow == eRender ? PwNodeType::AudioOutStream : PwNodeType::AudioInStream);
		node->setProperties(value.properties);
		node->audio()->bindRemoteSession();
		connect(
		    node->audio(),
		    &PwNodeAudio::sessionVolumeRequested,
		    this->sessionWorker,
		    [worker = this->sessionWorker, key = value.key](float volume) {
			    worker->setVolume(key, volume);
		    },
		    Qt::QueuedConnection
		);
		connect(
		    node->audio(),
		    &PwNodeAudio::sessionMuteRequested,
		    this->sessionWorker,
		    [worker = this->sessionWorker, key = value.key](bool muted) {
			    worker->setMuted(key, muted);
		    },
		    Qt::QueuedConnection
		);
		connect(
		    node->audio(),
		    &PwNodeAudio::sessionRefreshRequested,
		    this->sessionWorker,
		    [worker = this->sessionWorker, key = value.key] { worker->refreshSession(key); },
		    Qt::QueuedConnection
		);
		SessionEntry entry;
		entry.node = node;
		entry.endpointId = value.endpointId;
		it = this->sessions.insert(value.key, entry);
		// Publish a complete initial snapshot before adding to the QML model.
		node->audio()->applySessionState(
		    value.controlAvailable,
		    value.stateAvailable,
		    value.muted,
		    value.volume,
		    value.error
		);
		node->setReady(value.controlAvailable);
		this->owner->backendAddNode(node);
	} else {
		it->node->audio()->applySessionState(
		    value.controlAvailable,
		    value.stateAvailable,
		    value.muted,
		    value.volume,
		    value.error
		);
		it->node->setReady(value.controlAvailable);
	}
	this->handleSessionStateChanged(value.key, value.state);
}

void PwBackend::removeSessionNode(const QString& sessionKey) {
	auto it = this->sessions.find(sessionKey);
	if (it == this->sessions.end()) return;
	auto entry = it.value();
	this->sessions.erase(it);

	auto linkIt = this->captureLinkGroups.find(sessionKey);
	if (linkIt != this->captureLinkGroups.end()) {
		auto* group = linkIt.value();
		this->captureLinkGroups.erase(linkIt);
		this->owner->backendRemoveLinkGroup(group);
	}

	if (entry.node != nullptr) this->owner->backendRemoveNode(entry.node);
}

void PwBackend::refreshDefault(int flow) {
	IMMDevice* device = nullptr;
	auto hr =
	    this->enumerator->GetDefaultAudioEndpoint(static_cast<EDataFlow>(flow), eConsole, &device);

	if (FAILED(hr) || device == nullptr) {
		if (flow == eRender) this->owner->backendSetDefaultSink(nullptr);
		else this->owner->backendSetDefaultSource(nullptr);
		return;
	}

	LPWSTR rawId = nullptr;
	PwNode* node = nullptr;
	if (SUCCEEDED(device->GetId(&rawId)) && rawId != nullptr) {
		auto deviceId = takeComString(rawId);
		auto endpointIt = this->endpoints.find(deviceId);
		if (endpointIt != this->endpoints.end()) node = endpointIt.value().node;
	}
	device->Release();

	if (flow == eRender) this->owner->backendSetDefaultSink(node);
	else this->owner->backendSetDefaultSource(node);
}

void PwBackend::tryCreatePolicyConfig() {
	if (this->policyConfigAttempted) return;
	this->policyConfigAttempted = true;

	IPolicyConfig* config = nullptr;
	auto hr = CoCreateInstance(
	    CLSID_PolicyConfigClient,
	    nullptr,
	    CLSCTX_ALL,
	    IID_IPolicyConfig,
	    reinterpret_cast<void**>(&config)
	);

	if (FAILED(hr) || config == nullptr) {
		this->policyConfigError = audioControlError(
		    QStringLiteral("CoCreateInstance(IPolicyConfig)"),
		    FAILED(hr) ? hr : E_NOINTERFACE
		);
		qCWarning(
		    logPwBackend
		) << "Could not create IPolicyConfig; changing the default device will not work:"
		  << Qt::hex << hr;
		return;
	}

	this->policyConfig = config;
}

QString PwBackend::setPreferredDefault(PwNode* node, bool isSink) {
	if (node == nullptr) return QStringLiteral("No audio endpoint selected.");
	auto flow = isSink ? eRender : eCapture;
	auto endpoint = this->endpoints.constFind(node->backendKey());
	if (endpoint == this->endpoints.cend() || endpoint->node != node || endpoint->flow != flow) {
		return QStringLiteral(
		    "The selected node is not a current audio endpoint of the requested type."
		);
	}

	this->tryCreatePolicyConfig();
	if (this->policyConfig == nullptr) {
		qCWarning(logPwBackend) << "No IPolicyConfig available, cannot change the default"
		                        << (isSink ? "sink" : "source");
		return this->policyConfigError;
	}

	auto* config = static_cast<IPolicyConfig*>(this->policyConfig);
	auto error = setDefaultEndpointRoles(config, node->backendKey());
	if (!error.isEmpty()) qCWarning(logPwBackend) << error;
	// Roles can succeed independently. Publish only the observed console default,
	// including after partial failure, rather than assigning the requested node.
	this->refreshDefault(flow);
	return error;
}

void PwBackend::handleDefaultDeviceChanged(int flow, int role, const QString& deviceId) {
	if (role != eConsole) return;

	if (!deviceId.isEmpty() && !this->endpoints.contains(deviceId)) this->handleDeviceAdded(deviceId);

	PwNode* node = nullptr;
	if (!deviceId.isEmpty()) {
		auto it = this->endpoints.find(deviceId);
		if (it != this->endpoints.end()) node = it.value().node;
	}

	if (flow == eRender) this->owner->backendSetDefaultSink(node);
	else if (flow == eCapture) this->owner->backendSetDefaultSource(node);
}

void PwBackend::handleDeviceAdded(const QString& deviceId) {
	if (this->endpoints.contains(deviceId) || this->enumerator == nullptr) return;

	IMMDevice* device = nullptr;
	if (FAILED(this->enumerator->GetDevice(reinterpret_cast<LPCWSTR>(deviceId.utf16()), &device))
	    || device == nullptr)
	{
		return;
	}

	DWORD state = 0;
	device->GetState(&state);

	if (state == DEVICE_STATE_ACTIVE) {
		IMMEndpoint* endpoint = nullptr;
		if (SUCCEEDED(
		        device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&endpoint))
		    )
		    && endpoint != nullptr)
		{
			EDataFlow flow = eRender;
			endpoint->GetDataFlow(&flow);
			endpoint->Release();

			this->createEndpointNode(device, static_cast<int>(flow));
			this->refreshDefault(flow);
		}
	}

	device->Release();
}

void PwBackend::handleDeviceRemoved(const QString& deviceId) { this->removeEndpoint(deviceId); }

void PwBackend::handleDeviceStateChanged(const QString& deviceId, quint32 newState) {
	auto active = newState == DEVICE_STATE_ACTIVE;
	auto tracked = this->endpoints.contains(deviceId);

	if (active && !tracked) {
		this->handleDeviceAdded(deviceId);
	} else if (!active && tracked) {
		auto flow = this->endpoints.value(deviceId).flow;
		this->removeEndpoint(deviceId);
		this->refreshDefault(flow);
	}
}

void PwBackend::handleSessionStateChanged(const QString& sessionKey, int newState) {
	auto it = this->sessions.find(sessionKey);
	if (it == this->sessions.end()) return;
	auto& entry = it.value();

	if (newState == AudioSessionStateExpired) {
		this->removeSessionNode(sessionKey);
		return;
	}

	auto endpointIt = this->endpoints.find(entry.endpointId);
	auto isCapture = endpointIt != this->endpoints.end() && endpointIt.value().flow == eCapture;
	if (!isCapture) return;

	auto shouldCapture = newState == AudioSessionStateActive;
	if (shouldCapture == entry.capturing) return;
	entry.capturing = shouldCapture;

	if (shouldCapture) {
		auto* sourceNode = endpointIt.value().node;
		if (sourceNode == nullptr || entry.node == nullptr) return;
		auto* group = new PwLinkGroup(sourceNode, entry.node, this->owner);
		this->captureLinkGroups.insert(sessionKey, group);
		this->owner->backendAddLinkGroup(group);
	} else {
		auto linkIt = this->captureLinkGroups.find(sessionKey);
		if (linkIt != this->captureLinkGroups.end()) {
			auto* group = linkIt.value();
			this->captureLinkGroups.erase(linkIt);
			this->owner->backendRemoveLinkGroup(group);
		}
	}
}

void PwBackend::handleSessionDisconnected(const QString& sessionKey) {
	this->removeSessionNode(sessionKey);
}

} // namespace qs::windows::services::pipewire
