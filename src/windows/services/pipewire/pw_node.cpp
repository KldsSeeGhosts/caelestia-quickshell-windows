#include "pw_node.hpp"
#include <cmath>
#include <utility>

#include <qcontainerfwd.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qtypes.h>
#include <qvector.h>

#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include "com_util.hpp"
#include "../../redact.hpp"

namespace qs::windows::services::pipewire {

namespace {
Q_LOGGING_CATEGORY(logPwNode, "quickshell.windows.pipewire.node", QtWarningMsg);

class EndpointVolumeCallback final
    : public IAudioEndpointVolumeCallback
    , public ComCallbackTarget<PwNodeAudio> {
public:
	explicit EndpointVolumeCallback(PwNodeAudio* target): ComCallbackTarget(target) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
		if (ppvObject == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
			*ppvObject = static_cast<IAudioEndpointVolumeCallback*>(this);
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

	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
		if (data == nullptr) return S_OK;

		this->post([](PwNodeAudio* audio) { audio->refreshFromDevice(); });
		return S_OK;
	}

private:
	volatile LONG mRefCount = 1;
};

} // namespace

PwNodeAudio::~PwNodeAudio() {
	if (this->mEndpointVolume != nullptr) {
		auto* callback = static_cast<EndpointVolumeCallback*>(this->mEndpointCallback);
		if (callback != nullptr) {
			this->mEndpointVolume->UnregisterControlChangeNotify(callback);
			callback->detach();
			callback->Release();
		}

		this->mEndpointVolume->Release();
	}

	if (this->mSessionVolume != nullptr) {
		this->mSessionVolume->Release();
	}
}

void PwNodeAudio::bindEndpoint(IAudioEndpointVolume* endpointVolume) {
	this->mEndpointVolume = endpointVolume;
	emit this->controlAvailableChanged();
	this->readState();

	auto* callback = new EndpointVolumeCallback(this);
	auto hr = endpointVolume->RegisterControlChangeNotify(callback);
	if (FAILED(hr)) {
		qCWarning(logPwNode) << "RegisterControlChangeNotify failed:" << Qt::hex << hr;
		this->setControlError(audioControlError(QStringLiteral("RegisterControlChangeNotify"), hr));
		callback->detach();
		callback->Release();
		return;
	}

	this->mEndpointCallback = callback;
}

void PwNodeAudio::bindSession(ISimpleAudioVolume* sessionVolume) {
	this->mSessionVolume = sessionVolume;
	emit this->controlAvailableChanged();
	this->readState();
}

void PwNodeAudio::bindRemoteSession() { this->mRemoteSession = true; }

void PwNodeAudio::applySessionState(
    bool control,
    bool available,
    bool muted,
    float volume,
    const QString& error
) {
	if (!this->mRemoteSession) return;
	if (this->mRemoteControl != control) {
		this->mRemoteControl = control;
		emit this->controlAvailableChanged();
	}
	this->setStateAvailable(available);
	this->setControlError(error);
	this->mMuted = muted;
	this->mVolume = volume;
	emit this->mutedChanged();
	emit this->volumeChanged();
}

bool PwNodeAudio::controlAvailable() const {
	if (this->mRemoteSession) return this->mRemoteControl;
	return this->mEndpointVolume != nullptr || this->mSessionVolume != nullptr;
}

void PwNodeAudio::setControlError(const QString& error) {
	if (this->mControlError == error) return;
	this->mControlError = error;
	emit this->controlErrorChanged();
}

void PwNodeAudio::setStateAvailable(bool available) {
	if (this->mStateAvailable == available) return;
	this->mStateAvailable = available;
	emit this->stateAvailableChanged();
}

bool PwNodeAudio::readState() {
	if (!this->controlAvailable()) {
		this->setStateAvailable(false);
		this->setControlError(QStringLiteral("Audio control is unavailable."));
		return false;
	}

	auto check = [this](const QString& operation, HRESULT result) {
		if (SUCCEEDED(result)) return true;
		this->setStateAvailable(false);
		this->setControlError(audioControlError(operation, result));
		return false;
	};

	BOOL muted = FALSE;
	float volume = 0.0F;
	QVector<float> volumes;
	if (this->mEndpointVolume != nullptr) {
		if (!check(QStringLiteral("GetMute"), this->mEndpointVolume->GetMute(&muted))) return false;
		if (!check(
		        QStringLiteral("GetMasterVolumeLevelScalar"),
		        this->mEndpointVolume->GetMasterVolumeLevelScalar(&volume)
		    ))
			return false;

		UINT count = 0;
		if (!check(QStringLiteral("GetChannelCount"), this->mEndpointVolume->GetChannelCount(&count)))
			return false;

		volumes.reserve(static_cast<qsizetype>(count));
		for (UINT i = 0; i < count; i++) {
			float level = 0.0F;
			if (!check(
			        QStringLiteral("GetChannelVolumeLevelScalar"),
			        this->mEndpointVolume->GetChannelVolumeLevelScalar(i, &level)
			    ))
				return false;
			volumes.append(level);
		}
	} else {
		if (!check(QStringLiteral("GetMute"), this->mSessionVolume->GetMute(&muted))) return false;
		if (!check(QStringLiteral("GetMasterVolume"), this->mSessionVolume->GetMasterVolume(&volume)))
			return false;
	}

	// Commit only a complete native snapshot. A failed read must not invent zero/mute state.
	this->mMuted = muted != FALSE;
	this->mVolume = volume;
	this->mVolumes = std::move(volumes);
	this->setStateAvailable(true);
	this->setControlError({});
	return true;
}

bool PwNodeAudio::isMuted() const { return this->mMuted; }

void PwNodeAudio::setMuted(bool muted) {
	if (this->mRemoteSession) {
		if (this->mRemoteControl) emit this->sessionMuteRequested(muted);
		else this->setControlError(QStringLiteral("Audio control is unavailable."));
		return;
	}
	HRESULT hr = S_OK;
	if (this->mEndpointVolume != nullptr) {
		hr = this->mEndpointVolume->SetMute(muted ? TRUE : FALSE, nullptr);
	} else if (this->mSessionVolume != nullptr) {
		hr = this->mSessionVolume->SetMute(muted ? TRUE : FALSE, nullptr);
	} else {
		this->setControlError(QStringLiteral("Audio control is unavailable."));
		return;
	}

	if (FAILED(hr)) {
		qCWarning(logPwNode) << "SetMute failed:" << Qt::hex << hr;
		this->setControlError(audioControlError(QStringLiteral("SetMute"), hr));
		return;
	}

	this->setControlError({});
	this->mMuted = muted;
	emit this->mutedChanged();
}

float PwNodeAudio::volume() const { return this->mVolume; }

void PwNodeAudio::setVolume(float volume) {
	if (!std::isfinite(volume)) {
		this->setControlError(QStringLiteral("Volume must be finite."));
		return;
	}
	if (volume < 0.0F) volume = 0.0F;
	if (volume > 1.0F) volume = 1.0F;

	if (this->mRemoteSession) {
		if (this->mRemoteControl) emit this->sessionVolumeRequested(volume);
		else this->setControlError(QStringLiteral("Audio control is unavailable."));
		return;
	}

	HRESULT hr = S_OK;
	if (this->mEndpointVolume != nullptr) {
		hr = this->mEndpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
	} else if (this->mSessionVolume != nullptr) {
		hr = this->mSessionVolume->SetMasterVolume(volume, nullptr);
	} else {
		this->setControlError(QStringLiteral("Audio control is unavailable."));
		return;
	}

	if (FAILED(hr)) {
		qCWarning(logPwNode) << "SetMasterVolume failed:" << Qt::hex << hr;
		this->setControlError(audioControlError(QStringLiteral("SetMasterVolume"), hr));
		return;
	}

	this->refreshFromDevice();
}

QVector<PwAudioChannel::Enum> PwNodeAudio::channels() const {
	return QVector<PwAudioChannel::Enum>(this->mVolumes.size(), PwAudioChannel::Unknown);
}

QVector<float> PwNodeAudio::volumes() const { return this->mVolumes; }

void PwNodeAudio::setVolumes(const QVector<float>& volumes) {
	if (this->mEndpointVolume == nullptr) {
		this->setControlError(QStringLiteral("Per-channel audio control is unavailable."));
		return;
	}
	if (volumes.size() > this->mVolumes.size()) {
		this->setControlError(QStringLiteral("Invalid audio channel count."));
		return;
	}
	for (auto level: volumes) {
		if (!std::isfinite(level)) {
			this->setControlError(QStringLiteral("Volume must be finite."));
			return;
		}
	}

	for (qsizetype i = 0; i < volumes.length(); i++) {
		auto level = volumes.at(i);
		if (level < 0.0F) level = 0.0F;
		if (level > 1.0F) level = 1.0F;
		auto hr =
		    this->mEndpointVolume->SetChannelVolumeLevelScalar(static_cast<UINT>(i), level, nullptr);
		if (FAILED(hr)) {
			// Earlier channels may have changed. Read back reality, retaining the write failure.
			this->refreshFromDevice();
			this->setControlError(audioControlError(QStringLiteral("SetChannelVolumeLevelScalar"), hr));
			return;
		}
	}

	this->refreshFromDevice();
}

void PwNodeAudio::refreshFromDevice() {
	if (this->mRemoteSession) {
		emit this->sessionRefreshRequested();
		return;
	}
	auto channelCount = this->mVolumes.size();
	if (!this->readState()) return;

	emit this->volumeChanged();
	emit this->mutedChanged();
	emit this->volumesChanged();
	if (this->mVolumes.size() != channelCount) emit this->channelsChanged();
}

PwNode::PwNode(QObject* parent): QObject(parent) {
	this->mAudio = new PwNodeAudio(this);
}

PwNode::~PwNode() {
	if (this->mMeter != nullptr) this->mMeter->Release();
	if (this->mMeterDevice != nullptr) this->mMeterDevice->Release();
}

void PwNode::setProperties(const QVariantMap& properties) {
	if (this->mProperties == properties) return;
	this->mProperties = properties;
	emit this->propertiesChanged();
}

void PwNode::setReady(bool ready) {
	if (this->mReady == ready) return;
	this->mReady = ready;
	emit this->readyChanged();
}

void PwNode::setMeterDevice(IMMDevice* device) {
	if (this->mMeterDevice != nullptr) this->mMeterDevice->Release();
	this->mMeterDevice = device;
	if (device != nullptr) device->AddRef();
}

IAudioMeterInformation* PwNode::meterInformation() {
	if (this->mMeter != nullptr || this->mMeterDevice == nullptr) return this->mMeter;

	auto* device = std::exchange(this->mMeterDevice, nullptr);
	IAudioMeterInformation* meter = nullptr;
	auto hr = device->Activate(
	    __uuidof(IAudioMeterInformation),
	    CLSCTX_ALL,
	    nullptr,
	    reinterpret_cast<void**>(&meter)
	);
	device->Release();

	if (FAILED(hr) || meter == nullptr) {
		qCDebug(logPwNode) << "Activate(IAudioMeterInformation) failed for"
	                   << qs::windows::logId(this->mBackendKey);
		return nullptr;
	}

	this->mMeter = meter;
	return meter;
}

} // namespace qs::windows::services::pipewire
