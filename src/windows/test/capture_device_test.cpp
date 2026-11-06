// Capture device recovery fixtures. The device factory and the removal probe are
// injected, so a device removal is forced through the same checks the capture
// worker runs after a GPU reset; the fixture never resets a real device.

#include <utility>

#include <d3d11.h>
#include <windows.h>

#include <QtTest>

#include "capture_device.hpp"

using namespace qs::windows::capture;

namespace {

// The worker holds only function pointers for both seams, so the fixture state
// they use is file-local. One test at a time drives it.
struct FactoryCalls {
	int calls = 0;
	int failOn = 0; // 1-based creation that fails; 0 fails none.
	QList<ID3D11Device*> created;
};

FactoryCalls* factoryCalls = nullptr;

HRESULT countingFactory(CaptureDeviceParts* parts) {
	++factoryCalls->calls;
	if (factoryCalls->calls == factoryCalls->failOn) return E_FAIL;

	auto hr = createCaptureDevice(parts);
	if (SUCCEEDED(hr)) factoryCalls->created.append(parts->d3d.get());
	return hr;
}

struct ProbeCalls {
	HRESULT reason = S_OK;
	QList<ID3D11Device*> asked;
};

ProbeCalls* probeCalls = nullptr;

HRESULT injectedProbe(ID3D11Device* device) {
	probeCalls->asked.append(device);
	return probeCalls->reason;
}

// Records the capture category's warnings only, so a failure inside the scope
// still reaches QTest's own handler.
class WarningRecorder {
public:
	WarningRecorder() {
		instance = this;
		previous = qInstallMessageHandler(&WarningRecorder::handle);
	}

	~WarningRecorder() {
		qInstallMessageHandler(previous);
		instance = nullptr;
	}

	Q_DISABLE_COPY_MOVE(WarningRecorder);

	[[nodiscard]] int count() const { return static_cast<int>(this->messages.size()); }
	[[nodiscard]] QString at(int index) const { return this->messages.at(index); }

private:
	static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
		auto category = context.category != nullptr && qstrcmp(context.category, "quickshell.windows.capture") == 0;
		if (type == QtWarningMsg && category) {
			instance->messages.append(message);
			return;
		}
		previous(type, context, message);
	}

	static inline QtMessageHandler previous = nullptr;
	static inline WarningRecorder* instance = nullptr;
	QStringList messages;
};

} // namespace

class CaptureDeviceTest: public QObject {
	Q_OBJECT;

private slots:
	void removalResultsAreRecognised();
	void aCallRemovalRecoversWithoutAGpuReset();
	void oneCreationPerRequest();
	void aRemovalIsReportedAndRecoveredOncePerEvent();
	void aFailedCreationIsReportedAndNotRetriedWithinTheRequest();
	void aQueuedRecoverySparesTheReplacementDevice();
};

void CaptureDeviceTest::removalResultsAreRecognised() {
	QVERIFY(deviceRemovalResult(DXGI_ERROR_DEVICE_REMOVED));
	QVERIFY(deviceRemovalResult(DXGI_ERROR_DEVICE_RESET));

	QVERIFY(!deviceRemovalResult(DXGI_ERROR_DEVICE_HUNG));
	QVERIFY(!deviceRemovalResult(E_FAIL));
	QVERIFY(!deviceRemovalResult(S_OK));
}

void CaptureDeviceTest::aCallRemovalRecoversWithoutAGpuReset() {
	FactoryCalls factory;
	ProbeCalls probe;
	factoryCalls = &factory;
	probeCalls = &probe;

	CaptureDevice device({.create = &countingFactory, .removedReason = &injectedProbe});
	if (!device.ensure()) QSKIP("No Direct3D 11 hardware device on this system.");

	// A frame-pool, texture or session call that returns DXGI's removal answer is
	// enough on its own, before GetDeviceRemovedReason is even consulted.
	QCOMPARE(
	    static_cast<quint32>(captureRemovalReason(device, DXGI_ERROR_DEVICE_REMOVED)),
	    static_cast<quint32>(DXGI_ERROR_DEVICE_REMOVED)
	);
	QCOMPARE(
	    static_cast<quint32>(captureRemovalReason(device, DXGI_ERROR_DEVICE_RESET)),
	    static_cast<quint32>(DXGI_ERROR_DEVICE_RESET)
	);

	// An ordinary failure leaves the device alone.
	QCOMPARE(static_cast<quint32>(captureRemovalReason(device, E_FAIL)), static_cast<quint32>(S_OK));

	// When the device knows why it is gone, that reason is the one reported.
	probe.reason = DXGI_ERROR_DEVICE_HUNG;
	QCOMPARE(
	    static_cast<quint32>(captureRemovalReason(device, E_FAIL)),
	    static_cast<quint32>(DXGI_ERROR_DEVICE_HUNG)
	);
}

void CaptureDeviceTest::oneCreationPerRequest() {
	FactoryCalls factory;
	ProbeCalls probe;
	factoryCalls = &factory;
	probeCalls = &probe;

	CaptureDevice device({.create = &countingFactory, .removedReason = &injectedProbe});
	if (!device.ensure()) QSKIP("No Direct3D 11 hardware device on this system.");

	QCOMPARE(factory.calls, 1);
	QCOMPARE(factory.created.size(), 1);
	QVERIFY(device.present());
	QVERIFY(device.d3d() != nullptr);
	QVERIFY(device.context() != nullptr);
	QVERIFY(device.winrtDevice());

	// A second request while the device is healthy reuses it.
	QVERIFY(device.ensure());
	QCOMPARE(factory.calls, 1);

	// The probe is asked about this device, not about a copy or a cached flag.
	QCOMPARE(static_cast<quint32>(device.removedReason()), static_cast<quint32>(S_OK));
	QCOMPARE(probe.asked.size(), 1);
	QVERIFY(probe.asked.first() == device.d3d().get());
}

void CaptureDeviceTest::aRemovalIsReportedAndRecoveredOncePerEvent() {
	FactoryCalls factory;
	ProbeCalls probe;
	factoryCalls = &factory;
	probeCalls = &probe;

	CaptureDevice device({.create = &countingFactory, .removedReason = &injectedProbe});
	if (!device.ensure()) QSKIP("No Direct3D 11 hardware device on this system.");

	// The GPU is gone: the device says why.
	probe.reason = DXGI_ERROR_DEVICE_REMOVED;
	auto reason = device.removedReason();
	QCOMPARE(static_cast<quint32>(reason), static_cast<quint32>(DXGI_ERROR_DEVICE_REMOVED));

	{
		WarningRecorder warnings;
		device.recover(reason);
		device.recover(reason); // one report and one release per removal event

		QCOMPARE(warnings.count(), 1);
		QVERIFY(warnings.at(0).contains(QStringLiteral("Capture device lost")));
		QVERIFY(warnings.at(0).toLower().contains(QStringLiteral("887a0005")));
	}

	// The dead device, its context and the WinRT wrapper are released.
	QVERIFY(!device.present());
	QVERIFY(device.d3d() == nullptr);
	QVERIFY(device.context() == nullptr);
	QVERIFY(!device.winrtDevice());

	// The next capture request creates a new device through the same factory.
	probe.reason = S_OK;
	QVERIFY(device.ensure());
	QCOMPARE(factory.calls, 2);
	QVERIFY(device.present());
	QVERIFY(device.context() != nullptr);
	QCOMPARE(static_cast<quint32>(device.removedReason()), static_cast<quint32>(S_OK));

	// A second removal event recovers again, once.
	probe.reason = DXGI_ERROR_DEVICE_RESET;
	auto next = device.removedReason();
	{
		WarningRecorder warnings;
		device.recover(next);
		QCOMPARE(warnings.count(), 1);
		QVERIFY(warnings.at(0).toLower().contains(QStringLiteral("887a0007")));
	}
	QVERIFY(device.ensure());
	QCOMPARE(factory.calls, 3);
}

void CaptureDeviceTest::aFailedCreationIsReportedAndNotRetriedWithinTheRequest() {
	FactoryCalls factory;
	factory.failOn = 1;
	ProbeCalls probe;
	factoryCalls = &factory;
	probeCalls = &probe;

	CaptureDevice device({.create = &countingFactory, .removedReason = &injectedProbe});

	QVERIFY(!device.ensure());
	QCOMPARE(static_cast<quint32>(device.error()), static_cast<quint32>(E_FAIL));
	QVERIFY(!device.present());
	QCOMPARE(factory.calls, 1);

	// No retry loop inside the request; the next request tries once and succeeds.
	QVERIFY(device.ensure());
	QCOMPARE(factory.calls, 2);
	QVERIFY(device.present());
	QCOMPARE(static_cast<quint32>(device.error()), static_cast<quint32>(S_OK));
}

// A recovery queued from the frame callback may run after a capture request
// already created a replacement device. It names the generation it failed on,
// so it must not release the healthy replacement.
void CaptureDeviceTest::aQueuedRecoverySparesTheReplacementDevice() {
	FactoryCalls factory;
	ProbeCalls probe;
	factoryCalls = &factory;
	probeCalls = &probe;

	CaptureDevice device({.create = &countingFactory, .removedReason = &injectedProbe});
	if (!device.ensure()) QSKIP("No Direct3D 11 hardware device on this system.");

	const auto failed = device.generation();
	probe.reason = DXGI_ERROR_DEVICE_REMOVED;
	const auto reason = device.removedReason();
	QVERIFY(recoveryStillApplies(device, failed));

	device.recover(reason);
	QVERIFY(!recoveryStillApplies(device, failed));

	// The request that replaced the device produced a healthy one.
	probe.reason = S_OK;
	QVERIFY(device.ensure());
	QVERIFY(device.generation() != failed);
	QVERIFY(!recoveryStillApplies(device, failed));

	// The stale recovery runs now and leaves the replacement in place.
	if (recoveryStillApplies(device, failed)) device.recover(reason);
	QVERIFY(device.present());
	QCOMPARE(factory.calls, 2);
	QVERIFY(device.context() != nullptr);

	// A recovery queued for the current device does apply.
	QVERIFY(recoveryStillApplies(device, device.generation()));
}

QTEST_MAIN(CaptureDeviceTest)
#include "capture_device_test.moc"
