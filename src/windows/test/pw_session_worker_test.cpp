#include <atomic>
#include <thread>

#include <audiopolicy.h>
#include <qmutex.h>
#include <qsignalspy.h>
#include <qtest.h>
#include <qthread.h>
#include <wrl/client.h>

#include "pw_node.hpp"
#include "pw_session_worker.hpp"

using namespace qs::windows::services::pipewire;
namespace {
struct Audit {
	std::atomic<int> wrongThread = 0;
	std::atomic<DWORD> worker = 0;
	void check() {
		APTTYPE type;
		APTTYPEQUALIFIER qualifier;
		if (FAILED(CoGetApartmentType(&type, &qualifier)) || type != APTTYPE_MTA
		    || GetCurrentThreadId() != this->worker)
			++this->wrongThread;
	}
};
template <typename Interface>
struct FakeCom: Interface {
	Audit* audit;
	std::atomic<ULONG> refs = 1;
	explicit FakeCom(Audit* audit): audit(audit) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
	ULONG STDMETHODCALLTYPE Release() override { return --this->refs; }
};
struct FakeSession
    : IAudioSessionControl2
    , ISimpleAudioVolume {
	Audit* audit;
	std::atomic<ULONG> refs = 1;
	QMutex mutex;
	IAudioSessionEvents* events = nullptr;
	float volume = 0.4F;
	BOOL muted = FALSE;
	HRESULT writes = S_OK;
	QString instance;
	explicit FakeSession(Audit* audit, QString instance)
	    : audit(audit)
	    , instance(std::move(instance)) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		this->audit->check();
		if (iid == __uuidof(IAudioSessionControl2) || iid == __uuidof(IAudioSessionControl)
		    || iid == __uuidof(IUnknown))
			*out = static_cast<IAudioSessionControl2*>(this);
		else if (iid == __uuidof(ISimpleAudioVolume)) *out = static_cast<ISimpleAudioVolume*>(this);
		else {
			*out = nullptr;
			return E_NOINTERFACE;
		}
		this->AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
	ULONG STDMETHODCALLTYPE Release() override { return --this->refs; }
	HRESULT copyString(const QString& value, LPWSTR* out) {
		this->audit->check();
		const auto bytes = (value.size() + 1) * sizeof(wchar_t);
		*out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
		memcpy(*out, value.utf16(), bytes);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetState(AudioSessionState* out) override {
		this->audit->check();
		*out = AudioSessionStateActive;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR* out) override {
		return this->copyString(QStringLiteral("Test app"), out);
	}
	HRESULT STDMETHODCALLTYPE SetDisplayName(LPCWSTR, LPCGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetIconPath(LPWSTR*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetIconPath(LPCWSTR, LPCGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetGroupingParam(GUID*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetGroupingParam(LPCGUID, LPCGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE RegisterAudioSessionNotification(IAudioSessionEvents* events) override {
		this->audit->check();
		QMutexLocker lock(&this->mutex);
		events->AddRef();
		this->events = events;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE
	UnregisterAudioSessionNotification(IAudioSessionEvents* events) override {
		this->audit->check();
		QMutexLocker lock(&this->mutex);
		if (this->events == events) {
			this->events->Release();
			this->events = nullptr;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetSessionIdentifier(LPWSTR* out) override {
		return this->copyString(this->instance, out);
	}
	HRESULT STDMETHODCALLTYPE GetSessionInstanceIdentifier(LPWSTR* out) override {
		return this->copyString(this->instance, out);
	}
	HRESULT STDMETHODCALLTYPE GetProcessId(DWORD* out) override {
		this->audit->check();
		*out = GetCurrentProcessId();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE IsSystemSoundsSession() override {
		this->audit->check();
		return S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE SetDuckingPreference(BOOL) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetMasterVolume(float value, LPCGUID) override {
		this->audit->check();
		if (SUCCEEDED(this->writes)) this->volume = value;
		return this->writes;
	}
	HRESULT STDMETHODCALLTYPE GetMasterVolume(float* out) override {
		this->audit->check();
		*out = this->volume;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetMute(BOOL value, LPCGUID) override {
		this->audit->check();
		if (SUCCEEDED(this->writes)) this->muted = value;
		return this->writes;
	}
	HRESULT STDMETHODCALLTYPE GetMute(BOOL* out) override {
		this->audit->check();
		*out = this->muted;
		return S_OK;
	}
	Microsoft::WRL::ComPtr<IAudioSessionEvents> retainEvents() {
		QMutexLocker lock(&this->mutex);
		return this->events;
	}
};
struct FakeEnumerator: FakeCom<IAudioSessionEnumerator> {
	FakeSession* session;
	bool armed = false;
	FakeEnumerator(Audit* audit, FakeSession* session): FakeCom(audit), session(session) {}
	HRESULT STDMETHODCALLTYPE GetCount(int* out) override {
		this->audit->check();
		this->armed = true;
		*out = 1;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetSession(int index, IAudioSessionControl** out) override {
		this->audit->check();
		if (index != 0) return E_INVALIDARG;
		this->session->AddRef();
		*out = this->session;
		return S_OK;
	}
};
struct FakeManager: FakeCom<IAudioSessionManager2> {
	FakeEnumerator enumerator;
	QMutex mutex;
	IAudioSessionNotification* notify = nullptr;
	HRESULT registration = S_OK;
	int registrations = 0;
	int unregistrations = 0;
	FakeManager(Audit* audit, FakeSession* session): FakeCom(audit), enumerator(audit, session) {}
	HRESULT STDMETHODCALLTYPE
	GetAudioSessionControl(LPCGUID, DWORD, IAudioSessionControl**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetSimpleAudioVolume(LPCGUID, DWORD, ISimpleAudioVolume**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetSessionEnumerator(IAudioSessionEnumerator** out) override {
		this->audit->check();
		this->enumerator.AddRef();
		*out = &this->enumerator;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE
	RegisterSessionNotification(IAudioSessionNotification* notify) override {
		this->audit->check();
		++this->registrations;
		if (FAILED(this->registration)) return this->registration;
		QMutexLocker lock(&this->mutex);
		notify->AddRef();
		this->notify = notify;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE
	UnregisterSessionNotification(IAudioSessionNotification* notify) override {
		this->audit->check();
		++this->unregistrations;
		QMutexLocker lock(&this->mutex);
		if (this->notify == notify) {
			this->notify->Release();
			this->notify = nullptr;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE
	RegisterDuckNotification(LPCWSTR, IAudioVolumeDuckNotification*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE UnregisterDuckNotification(IAudioVolumeDuckNotification*) override {
		return E_NOTIMPL;
	}
	Microsoft::WRL::ComPtr<IAudioSessionNotification> retainNotify() {
		QMutexLocker lock(&this->mutex);
		return this->notify;
	}
};
// A Core Audio callback runs on another MTA thread, never the GUI apartment.
template <typename Fn>
void nativeCallback(Fn fn) {
	std::thread callback([fn = std::move(fn)] {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		fn();
		CoUninitialize();
	});
	callback.join();
}
} // namespace

class PwSessionWorkerTest: public QObject {
	Q_OBJECT;
private slots:
	void arrivalControlsRemovalAndShutdownStayOnMta() {
		Audit audit;
		FakeSession initial(&audit, QStringLiteral("initial")),
		    arriving(&audit, QStringLiteral("arriving"));
		FakeManager manager(&audit, &initial);
		QThread thread;
		auto* worker = new PwSessionWorker([&](const QString&, IAudioSessionManager2** out) {
			audit.worker = GetCurrentThreadId();
			audit.check();
			manager.AddRef();
			*out = &manager;
			return S_OK;
		});
		worker->moveToThread(&thread);
		connect(&thread, &QThread::started, worker, &PwSessionWorker::start);
		connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
		struct Stop {
			QThread& thread;
			PwSessionWorker* worker;
			~Stop() {
				QMetaObject::invokeMethod(worker, &PwSessionWorker::stop, Qt::BlockingQueuedConnection);
				thread.quit();
				thread.wait();
			}
		};
		QSignalSpy changed(worker, &PwSessionWorker::sessionChanged),
		    removed(worker, &PwSessionWorker::sessionRemoved);
		thread.start();
		{
			Stop stop {thread, worker};
			QMetaObject::invokeMethod(
			    worker,
			    [worker] { worker->attachEndpoint(QStringLiteral("endpoint"), 1); },
			    Qt::QueuedConnection
			);
			QTRY_COMPARE(changed.size(), 1);
			auto first = qvariant_cast<SessionSnapshot>(changed.at(0).at(0));
			QVERIFY(first.controlAvailable);
			QVERIFY(first.stateAvailable);
			QCOMPARE(first.volume, 0.4F);
			QVERIFY(manager.enumerator.armed);
			nativeCallback([&] {
				auto callback = manager.retainNotify();
				callback->OnSessionCreated(&arriving);
			});
			QTRY_COMPARE(changed.size(), 2);
			auto second = qvariant_cast<SessionSnapshot>(changed.at(1).at(0));
			QVERIFY(first.key != second.key);
			QCOMPARE(second.flow, 1);
			PwNodeAudio guiAudio;
			guiAudio.bindRemoteSession();
			guiAudio.applySessionState(
			    second.controlAvailable,
			    second.stateAvailable,
			    second.muted,
			    second.volume,
			    second.error
			);
			connect(
			    &guiAudio,
			    &PwNodeAudio::sessionVolumeRequested,
			    worker,
			    [worker, key = second.key](float value) { worker->setVolume(key, value); },
			    Qt::QueuedConnection
			);
			connect(
			    &guiAudio,
			    &PwNodeAudio::sessionMuteRequested,
			    worker,
			    [worker, key = second.key](bool value) { worker->setMuted(key, value); },
			    Qt::QueuedConnection
			);
			guiAudio.setVolume(0.7F);
			guiAudio.setMuted(true);
			QTRY_COMPARE(changed.size(), 4);
			auto current = qvariant_cast<SessionSnapshot>(changed.last().at(0));
			QCOMPARE(current.volume, 0.7F);
			QVERIFY(current.muted);
			// GUI values change only when the observed snapshot arrives.
			QCOMPARE(guiAudio.volume(), 0.4F);
			guiAudio.applySessionState(
			    current.controlAvailable,
			    current.stateAvailable,
			    current.muted,
			    current.volume,
			    current.error
			);
			QCOMPARE(guiAudio.volume(), 0.7F);
			QVERIFY(guiAudio.isMuted());
			QMetaObject::invokeMethod(
			    worker,
			    [&] {
				    arriving.writes = E_ACCESSDENIED;
				    worker->setVolume(second.key, 0.2F);
			    },
			    Qt::QueuedConnection
			);
			QTRY_COMPARE(changed.size(), 6);
			current = qvariant_cast<SessionSnapshot>(changed.last().at(0));
			QCOMPARE(current.volume, 0.7F);
			QVERIFY(current.error.contains(QStringLiteral("80070005")));
			nativeCallback([&] {
				auto events = arriving.retainEvents();
				events->OnStateChanged(AudioSessionStateExpired);
			});
			QTRY_COMPARE(removed.size(), 1);
			QCOMPARE(removed.at(0).at(0).toString(), second.key);
			QCOMPARE(arriving.refs.load(), 1UL);
			// Server shutdown removes the endpoint sessions and retries registration.
			nativeCallback([&] {
				auto events = initial.retainEvents();
				events->OnSessionDisconnected(DisconnectReasonServerShutdown);
			});
			QTRY_COMPARE(removed.size(), 2);
			QTRY_VERIFY_WITH_TIMEOUT(changed.size() >= 7, 3500);
			QCOMPARE(manager.registrations, 2);
			// Queue another arrival immediately before stop. stop drains/releases all
			// retained COM payloads before it uninitializes the MTA.
			nativeCallback([&] {
				auto callback = manager.retainNotify();
				callback->OnSessionCreated(&arriving);
			});
		}
		QCOMPARE(audit.wrongThread.load(), 0);
		QCOMPARE(initial.refs.load(), 1UL);
		QCOMPARE(arriving.refs.load(), 1UL);
		QCOMPARE(manager.refs.load(), 1UL);
		QCOMPARE(manager.enumerator.refs.load(), 1UL);
		QCOMPARE(manager.registrations, manager.unregistrations);
	}
	void registrationFailureIsReportedAndCanRetry() {
		Audit audit;
		FakeSession session(&audit, QStringLiteral("initial"));
		FakeManager manager(&audit, &session);
		manager.registration = E_ACCESSDENIED;
		QThread thread;
		auto* worker = new PwSessionWorker([&](const QString&, IAudioSessionManager2** out) {
			audit.worker = GetCurrentThreadId();
			audit.check();
			manager.AddRef();
			*out = &manager;
			return S_OK;
		});
		worker->moveToThread(&thread);
		connect(&thread, &QThread::started, worker, &PwSessionWorker::start);
		connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
		QSignalSpy errors(worker, &PwSessionWorker::error),
		    changed(worker, &PwSessionWorker::sessionChanged);
		thread.start();
		QMetaObject::invokeMethod(
		    worker,
		    [worker] { worker->attachEndpoint(QStringLiteral("endpoint"), 0); },
		    Qt::QueuedConnection
		);
		// Avoid assertion exits until the thread has stopped.
		QTest::qWait(150);
		QMetaObject::invokeMethod(
		    worker,
		    [&] {
			    manager.registration = S_OK;
			    worker->attachEndpoint(QStringLiteral("endpoint"), 0);
		    },
		    Qt::BlockingQueuedConnection
		);
		QMetaObject::invokeMethod(worker, &PwSessionWorker::stop, Qt::BlockingQueuedConnection);
		thread.quit();
		thread.wait();
		QCOMPARE(errors.size(), 1);
		QVERIFY(errors.first().first().toString().contains(QStringLiteral("80070005")));
		QCOMPARE(changed.size(), 1);
		QCOMPARE(audit.wrongThread.load(), 0);
		QCOMPARE(manager.refs.load(), 1UL);
		QCOMPARE(session.refs.load(), 1UL);
	}
};
QTEST_GUILESS_MAIN(PwSessionWorkerTest)
#include "pw_session_worker_test.moc"
