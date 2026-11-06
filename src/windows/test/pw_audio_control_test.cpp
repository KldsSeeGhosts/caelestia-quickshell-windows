#include <array>
#include <limits>

#include <audioclient.h>
#include <endpointvolume.h>
#include <qsignalspy.h>
#include <qt_windows.h>
#include <qtest.h>

#include "policy_config.hpp"
#include "pw_node.hpp"

using namespace qs::windows::services::pipewire;

namespace {

// Stack-owned COM fakes. No enumerator, activation, audio service or hardware is used.
template <typename Interface>
class FakeCom: public Interface {
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

class FakeSession final: public FakeCom<ISimpleAudioVolume> {
public:
	float volume = 0.5F;
	BOOL muted = FALSE;
	HRESULT writeResult = S_OK;
	HRESULT readResult = S_OK;
	int writes = 0;

	HRESULT STDMETHODCALLTYPE SetMasterVolume(float value, LPCGUID) override {
		++this->writes;
		if (SUCCEEDED(this->writeResult)) this->volume = value;
		return this->writeResult;
	}
	HRESULT STDMETHODCALLTYPE GetMasterVolume(float* value) override {
		*value = this->volume;
		return this->readResult;
	}
	HRESULT STDMETHODCALLTYPE SetMute(BOOL value, LPCGUID) override {
		++this->writes;
		if (SUCCEEDED(this->writeResult)) this->muted = value;
		return this->writeResult;
	}
	HRESULT STDMETHODCALLTYPE GetMute(BOOL* value) override {
		*value = this->muted;
		return S_OK;
	}
};

class FakeEndpoint final: public FakeCom<IAudioEndpointVolume> {
public:
	float volume = 0.5F;
	BOOL muted = FALSE;
	QVector<float> channels {0.5F, 0.5F};
	HRESULT writeResult = S_OK;
	HRESULT readResult = S_OK;
	HRESULT notificationResult = S_OK;
	int failedChannel = -1;
	int writes = 0;
	int unregistrations = 0;

	HRESULT STDMETHODCALLTYPE RegisterControlChangeNotify(IAudioEndpointVolumeCallback*) override {
		return this->notificationResult;
	}
	HRESULT STDMETHODCALLTYPE UnregisterControlChangeNotify(IAudioEndpointVolumeCallback*) override {
		++this->unregistrations;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetChannelCount(UINT* count) override {
		*count = static_cast<UINT>(this->channels.size());
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetMasterVolumeLevel(float, LPCGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetMasterVolumeLevelScalar(float value, LPCGUID) override {
		++this->writes;
		if (SUCCEEDED(this->writeResult)) this->volume = value;
		return this->writeResult;
	}
	HRESULT STDMETHODCALLTYPE GetMasterVolumeLevel(float*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetMasterVolumeLevelScalar(float* value) override {
		*value = this->volume;
		return this->readResult;
	}
	HRESULT STDMETHODCALLTYPE SetChannelVolumeLevel(UINT, float, LPCGUID) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE
	SetChannelVolumeLevelScalar(UINT channel, float value, LPCGUID) override {
		++this->writes;
		if (channel == this->failedChannel) return E_ACCESSDENIED;
		if (SUCCEEDED(this->writeResult)) this->channels[channel] = value;
		return this->writeResult;
	}
	HRESULT STDMETHODCALLTYPE GetChannelVolumeLevel(UINT, float*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetChannelVolumeLevelScalar(UINT channel, float* value) override {
		*value = this->channels.at(channel);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetMute(BOOL value, LPCGUID) override {
		++this->writes;
		if (SUCCEEDED(this->writeResult)) this->muted = value;
		return this->writeResult;
	}
	HRESULT STDMETHODCALLTYPE GetMute(BOOL* value) override {
		*value = this->muted;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetVolumeStepInfo(UINT*, UINT*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE VolumeStepUp(LPCGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE VolumeStepDown(LPCGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetVolumeRange(float*, float*, float*) override { return E_NOTIMPL; }
};

class FakePolicy final: public FakeCom<IPolicyConfig> {
public:
	std::array<HRESULT, 3> results {S_OK, S_OK, S_OK};
	QVector<ERole> calls;
	QString lastId;

	HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) override {
		this->calls.append(role);
		this->lastId = QString::fromWCharArray(id);
		return this->results.at(role);
	}
	HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) override { return E_NOTIMPL; }
};

} // namespace

class PwAudioControlTest: public QObject {
	Q_OBJECT;

private slots:
	void unavailableControl() {
		PwNode node;
		QVERIFY(!node.isReady());
		QVERIFY(!node.audio()->controlAvailable());
		QVERIFY(!node.audio()->stateAvailable());
		QVERIFY(!node.audio()->controlError().isEmpty());
		QSignalSpy changed(node.audio(), &PwNodeAudio::volumeChanged);
		node.audio()->setVolume(0.8F);
		QCOMPARE(changed.count(), 0);
		QCOMPARE(node.audio()->volume(), 0.0F);
	}

	void sessionFailureKeepsState() {
		FakeSession session;
		PwNodeAudio audio;
		QSignalSpy availability(&audio, &PwNodeAudio::controlAvailableChanged);
		QSignalSpy errors(&audio, &PwNodeAudio::controlErrorChanged);
		audio.bindSession(&session);
		QVERIFY(audio.controlAvailable());
		QVERIFY(audio.stateAvailable());
		QVERIFY(audio.controlError().isEmpty());
		QCOMPARE(availability.count(), 1);
		QCOMPARE(session.writes, 0);
		QSignalSpy volumes(&audio, &PwNodeAudio::volumeChanged);
		QSignalSpy mutes(&audio, &PwNodeAudio::mutedChanged);
		session.writeResult = E_ACCESSDENIED;
		audio.setVolume(0.8F);
		QVERIFY(audio.controlError().contains(QStringLiteral("0x80070005")));
		QCOMPARE(audio.volume(), 0.5F);
		QCOMPARE(volumes.count(), 0);
		audio.setMuted(true);
		QVERIFY(audio.controlError().startsWith(QStringLiteral("SetMute")));
		QVERIFY(!audio.isMuted());
		QCOMPARE(mutes.count(), 0);

		session.writeResult = S_OK;
		audio.setVolume(2.0F);
		QCOMPARE(audio.volume(), 1.0F);
		QVERIFY(audio.controlError().isEmpty());
		QVERIFY(errors.count() >= 3);
	}

	void sessionReadFailureKeepsSnapshot() {
		FakeSession session;
		PwNodeAudio audio;
		audio.bindSession(&session);
		session.volume = 0.9F;
		session.muted = TRUE;
		session.readResult = AUDCLNT_E_DEVICE_INVALIDATED;
		QSignalSpy changed(&audio, &PwNodeAudio::volumeChanged);
		audio.refreshFromDevice();
		QVERIFY(!audio.stateAvailable());
		QCOMPARE(audio.volume(), 0.5F);
		QVERIFY(!audio.isMuted());
		QCOMPARE(changed.count(), 0);
		QVERIFY(audio.controlError().contains(QStringLiteral("0x88890004")));
		session.readResult = S_OK;
		audio.refreshFromDevice();
		QVERIFY(audio.stateAvailable());
		QCOMPARE(audio.volume(), 0.9F);
		QVERIFY(audio.isMuted());
		QVERIFY(audio.controlError().isEmpty());
	}

	void endpointFailuresAndPartialChannels() {
		FakeEndpoint endpoint;
		PwNodeAudio audio;
		audio.bindEndpoint(&endpoint);
		QVERIFY(audio.controlAvailable());
		QCOMPARE(endpoint.writes, 0);
		endpoint.writeResult = E_ACCESSDENIED;
		audio.setVolume(0.8F);
		QCOMPARE(audio.volume(), 0.5F);
		QVERIFY(audio.controlError().contains(QStringLiteral("0x80070005")));
		audio.setMuted(true);
		QVERIFY(!audio.isMuted());
		QVERIFY(audio.controlError().startsWith(QStringLiteral("SetMute")));
		endpoint.writeResult = S_OK;
		endpoint.failedChannel = 1;
		audio.setVolumes({0.8F, 0.9F});
		QCOMPARE(audio.volumes(), (QVector<float> {0.8F, 0.5F}));
		QVERIFY(audio.controlError().startsWith(QStringLiteral("SetChannelVolumeLevelScalar")));

		endpoint.readResult = E_ACCESSDENIED;
		endpoint.volume = 0.9F;
		audio.refreshFromDevice();
		QCOMPARE(audio.volume(), 0.5F);
		QVERIFY(audio.controlError().startsWith(QStringLiteral("GetMasterVolumeLevelScalar")));
	}

	void notificationFailure() {
		FakeEndpoint endpoint;
		endpoint.notificationResult = E_ACCESSDENIED;
		{
			PwNodeAudio audio;
			audio.bindEndpoint(&endpoint);
			QVERIFY(audio.controlAvailable());
			QVERIFY(audio.controlError().startsWith(QStringLiteral("RegisterControlChangeNotify")));
		}
		QCOMPARE(endpoint.unregistrations, 0);
	}

	void invalidVolumeDoesNotWrite() {
		FakeEndpoint endpoint;
		PwNodeAudio audio;
		audio.bindEndpoint(&endpoint);
		audio.setVolume(std::numeric_limits<float>::quiet_NaN());
		audio.setVolumes({0.8F, std::numeric_limits<float>::infinity()});
		audio.setVolumes({0.1F, 0.2F, 0.3F});
		QCOMPARE(endpoint.writes, 0);
		QVERIFY(!audio.controlError().isEmpty());
	}

	void defaultRoles_data() {
		QTest::addColumn<int>("failureMask");
		for (int mask = 0; mask < 8; ++mask) {
			QTest::newRow(qPrintable(QString::number(mask))) << mask;
		}
	}

	void defaultRoles() {
		QFETCH(int, failureMask);
		FakePolicy policy;
		for (int role = 0; role < 3; ++role) {
			policy.results[role] = (failureMask & (1 << role)) ? E_ACCESSDENIED : S_OK;
		}
		auto error = setDefaultEndpointRoles(&policy, QStringLiteral("fixture-device"));
		QCOMPARE(policy.calls, (QVector<ERole> {eConsole, eMultimedia, eCommunications}));
		QCOMPARE(policy.lastId, QStringLiteral("fixture-device"));
		QCOMPARE(error.isEmpty(), failureMask == 0);
		const std::array names {"eConsole", "eMultimedia", "eCommunications"};
		for (int role = 0; role < 3; ++role) {
			QCOMPARE(error.contains(QString::fromLatin1(names[role])), bool(failureMask & (1 << role)));
		}
		if (failureMask != 0) QVERIFY(error.contains(QStringLiteral("0x80070005")));
	}
};

QTEST_GUILESS_MAIN(PwAudioControlTest)

#include "pw_audio_control_test.moc"
