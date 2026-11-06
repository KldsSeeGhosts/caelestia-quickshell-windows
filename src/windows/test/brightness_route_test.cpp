#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include <qcoreapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qmutex.h>
#include <qpointer.h>
#include <qscopeguard.h>
#include <qsemaphore.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../startup.hpp"
#include "../system/brightness_backend.hpp"
#include "../system/gamma.hpp"
#include "../util.hpp"

using namespace qs::windows::sys;

// Only injected brightness backends and gamma calls are used. No
// QGuiApplication, native monitor resolution, or QML shell runs.
namespace qs::windows {
HMONITOR monitorForScreen(QScreen*) {
	qFatal("Native monitor resolution in brightness route fixtures");
}
} // namespace qs::windows

// Controllable startup gate. Production defers query work until the first frame
// has settled; these fixtures let a test hold requests in the gate, settle it,
// and then release the deferred work after newer requests dispatched, which is
// how completions arrive out of request order.
namespace qs::windows::startup {
namespace Fixture {
bool gateSettled = true;

struct Deferred {
	QPointer<QObject> context;
	std::function<void()> callback;
};

QList<Deferred> deferred;
} // namespace Fixture

bool settled() { return Fixture::gateSettled; }

void afterFirstFrame(QObject* context, std::function<void()> callback) {
	Fixture::deferred.append({QPointer<QObject>(context), std::move(callback)});
}
} // namespace qs::windows::startup

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace {
const QString SCREEN = QStringLiteral("fixture-screen");

QString screenNumbered(int index) { return QStringLiteral("fixture-screen-%1").arg(index); }

void resetStartupGate() {
	qs::windows::startup::Fixture::gateSettled = true;
	qs::windows::startup::Fixture::deferred.clear();
}

// Releases work the startup gate deferred, in the order it was queued.
void runDeferredStartup() {
	const auto pending = qs::windows::startup::Fixture::deferred;
	qs::windows::startup::Fixture::deferred.clear();
	for (const auto& item: pending) {
		if (item.context.isNull()) continue;
		item.callback();
	}
}

class FakeGammaApi {
public:
	FakeGammaApi() {
		Q_ASSERT(current == nullptr);
		current = this;
		for (auto channel = 0; channel < 3; channel++) {
			for (auto i = 0; i < 256; i++) {
				// Deliberately non-linear so a linear neutral ramp is not a restore.
				this->original[channel][i] = static_cast<WORD>(i * i);
			}
		}
		std::memcpy(this->ramp, this->original, sizeof(this->ramp));
	}
	~FakeGammaApi() { current = nullptr; }

	bool createOk = true;
	bool getOk = true;
	bool setOk = true;
	// When non-negative, this many further setRamp calls succeed before setOk
	// is forced false, modelling a mixed native outcome within one operation.
	int setsBeforeFailure = -1;
	int gets = 0;
	int sets = 0;
	int restores = 0;
	int openDcs = 0;
	WORD original[3][256] {};
	WORD ramp[3][256] {};

	GammaController::NativeApi api() { return {&createDc, &deleteDc, &getRamp, &setRamp}; }
	bool restored() const { return std::memcmp(this->ramp, this->original, sizeof(this->ramp)) == 0; }

private:
	static inline FakeGammaApi* current = nullptr;

	static HDC WINAPI createDc(LPCWSTR, LPCWSTR, LPCWSTR, const DEVMODEW*) {
		if (!current->createOk) return nullptr;
		current->openDcs++;
		return reinterpret_cast<HDC>(current);
	}
	static BOOL WINAPI deleteDc(HDC dc) {
		Q_ASSERT(dc == reinterpret_cast<HDC>(current));
		current->openDcs--;
		return TRUE;
	}
	static BOOL WINAPI getRamp(HDC dc, LPVOID data) {
		Q_ASSERT(dc == reinterpret_cast<HDC>(current));
		current->gets++;
		if (!current->getOk) return FALSE;
		std::memcpy(data, current->ramp, sizeof(current->ramp));
		return TRUE;
	}
	static BOOL WINAPI setRamp(HDC dc, LPVOID data) {
		Q_ASSERT(dc == reinterpret_cast<HDC>(current));
		current->sets++;
		if (std::memcmp(data, current->original, sizeof(current->ramp)) == 0) current->restores++;
		if (current->setsBeforeFailure == 0) current->setOk = false;
		else if (current->setsBeforeFailure > 0) current->setsBeforeFailure--;
		if (!current->setOk) return FALSE;
		std::memcpy(current->ramp, data, sizeof(current->ramp));
		return TRUE;
	}
};

Probe probeFor(BrightnessMethod method) {
	Probe probe;
	probe.monitor.kind = PanelKind::Internal;
	MonitorTarget target;
	target.technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
	probe.monitor.targets.push_back(target);
	probe.monitor.internalTargets = 1;
	switch (method) {
	case BrightnessMethod::Wmi:
		probe.wmiAsked = true;
		probe.wmi.push_back(
		    {QStringLiteral("fixture-panel"), QStringLiteral("fixture-path"), true, 60}
		);
		// The route is only proven when the panel's device path matches.
		probe.monitor.targets[0].instanceId = QStringLiteral("FIXTURE-PANEL");
		break;
	case BrightnessMethod::Lcd:
		probe.lcdAsked = true;
		probe.lcd.usable = true;
		probe.lcd.current = 60;
		break;
	case BrightnessMethod::Ddc:
		probe.monitor.kind = PanelKind::External;
		probe.monitor.internalTargets = 0;
		probe.monitor.targets[0].technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
		probe.monitor.targets[0].instanceId = QStringLiteral("FIXTURE-EXTERNAL");
		probe.ddcAsked = true;
		probe.ddcOk = true;
		probe.ddcBrightness = 0.6;
		break;
	default: break; // Internal panel with no hardware route resolves to software.
	}
	return probe;
}

Probe ddcProbeAt(qreal brightness) {
	auto probe = probeFor(BrightnessMethod::Ddc);
	probe.ddcBrightness = brightness;
	return probe;
}

class FakeBackend final: public BrightnessBackend {
public:
	Probe observation;
	// When non-empty, each probe takes the next entry in dispatch order, so
	// concurrent requests through one backend can get distinct results.
	QList<Probe> queuedProbes;
	QHash<BrightnessMethod, bool> outcomes;
	bool dimmed = false;
	bool gammaOk = true;
	int whiteReadback = 3000;
	GammaController* gamma = nullptr;
	QList<qreal> gammaWrites;
	bool blockWrite = false;
	QSemaphore entered;
	QSemaphore release;

	Probe probeScreen(HMONITOR, bool) override {
		QMutexLocker lock(&this->mutex);
		if (!this->queuedProbes.isEmpty()) return this->queuedProbes.takeFirst();
		return this->observation;
	}

	bool
	writeBrightness(const BrightnessRoute& route, HMONITOR, qreal value, QString& error) override {
		{
			QMutexLocker lock(&this->mutex);
			this->methods.append(route.method);
			this->values.append(value);
		}
		this->entered.release();
		if (this->blockWrite && !this->release.tryAcquire(1, 5000)) {
			error = QStringLiteral("fixture gate timed out");
			return false;
		}
		if (this->outcomes.value(route.method, true)) return true;
		error = QStringLiteral("fixture denied");
		return false;
	}

	int readBrightness(const BrightnessRoute& route, QString&) override {
		return route.method == BrightnessMethod::SdrWhite ? this->whiteReadback : 60;
	}
	bool gammaDims(const QString& screen) const override {
		return this->gamma ? this->gamma->dims(screen) : this->dimmed;
	}
	bool gammaSetBrightness(const QString& screen, qreal value) override {
		this->gammaWrites.append(value);
		if (this->gamma) return this->gamma->trySetBrightness(screen, value);
		// Match the production transaction: failure preserves the previous factor,
		// including retry eligibility after a failed clear.
		if (this->gammaOk) this->dimmed = value < 1.0;
		return this->gammaOk;
	}
	void startEventWatch(const std::function<void(const QString&, int)>&) override {}

	QList<BrightnessMethod> writtenMethods() {
		QMutexLocker lock(&this->mutex);
		return this->methods;
	}

	QList<qreal> writtenValues() {
		QMutexLocker lock(&this->mutex);
		return this->values;
	}

private:
	QMutex mutex;
	QList<BrightnessMethod> methods;
	QList<qreal> values;
};
} // namespace

class BrightnessRouteTest: public QObject {
	Q_OBJECT;

	void resolve(Brightness& brightness, FakeBackend& fake, BrightnessMethod method) {
		fake.observation = probeFor(method);
		QSignalSpy queried(&brightness, &Brightness::queried);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
	}

private slots:
	void init() { resetStartupGate(); }

	void hdrRouteAndReadback() {
		QVERIFY(!isHdrColorMode(0)); // SDR
		QVERIFY(!isHdrColorMode(1)); // WCG is not HDR
		QVERIFY(isHdrColorMode(2));
		QCOMPARE(sliderToSdrWhite(0), UINT32(1000));
		QCOMPARE(sliderToSdrWhite(0.4), UINT32(3000));
		QCOMPARE(sliderToSdrWhite(1), UINT32(6000));
		QCOMPARE(sliderToSdrWhite(0.734), UINT32(4650));
		QCOMPARE(sdrWhiteToSlider(1000), 0.0);
		QCOMPARE(sdrWhiteToSlider(3000), 0.4);
		QCOMPARE(sdrWhiteToSlider(6000), 1.0);
		FakeBackend fake;
		fake.observation = probeFor(BrightnessMethod::Ddc);
		auto& target = fake.observation.monitor.targets[0];
		target.colorKnown = true;
		target.advancedColor = true;
		target.hdrActive = true;
		target.whiteKnown = true;
		target.whiteLevel = 3000;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy writes(&brightness, &Brightness::brightnessWriteResult);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("sdr-white"));
		QCOMPARE(queried.at(0).at(3).toDouble(), 0.4);
		brightness.setBrightness(SCREEN, false, 0.8);
		QTRY_COMPARE(writes.count(), 1);
		QCOMPARE(writes.at(0).at(1).toDouble(), 0.4);
		QCOMPARE(writes.at(0).at(3).toString(), QStringLiteral("sdr-white"));
		// Advanced color remains active in WCG when HDR is turned off.
		fake.observation.monitor.targets[0].hdrActive = false;
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 2);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("ddc"));
		QCOMPARE(queried.at(1).at(3).toDouble(), 0.6);
		fake.observation.monitor.targets[0].hdrActive = true;
		fake.observation.monitor.targets[0].whiteLevel = 4650;
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 3);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("sdr-white"));
		QCOMPARE(queried.at(2).at(3).toDouble(), 0.73);
	}

	void queryRequestIdsAreUniqueAndCorrelated() {
		FakeBackend fake;
		fake.observation = probeFor(BrightnessMethod::Ddc);
		Brightness brightness(fake);
		QSignalSpy finished(&brightness, &Brightness::queryFinished);
		QSignalSpy queried(&brightness, &Brightness::queried);

		const auto screenA = screenNumbered(1);
		const auto screenB = screenNumbered(2);
		// None of these names exists in a QCoreApplication fixture, which also
		// covers the unknown-screen outcome.
		const auto first = brightness.queryRequest(screenA);
		const auto second = brightness.queryRequest(screenB);
		const auto third = brightness.queryRequest(SCREEN);
		QVERIFY(first > 0);
		QVERIFY(second > first);
		QVERIFY(third > second);
		QCOMPARE(finished.count(), 0); // Completion stays asynchronous.

		QTRY_COMPARE(finished.count(), 3);
		QCOMPARE(queried.count(), 3); // queryRequest still reports through queried().

		QHash<qint64, QVariantList> byId;
		for (const auto& row: finished) {
			const auto id = row.at(0).toLongLong();
			QVERIFY(id > 0);
			QVERIFY2(!byId.contains(id), "duplicate queryFinished for one request id");
			byId.insert(id, row);
		}
		QCOMPARE(byId.size(), 3);
		QCOMPARE(byId.value(first).at(1).toString(), screenA);
		QCOMPARE(byId.value(second).at(1).toString(), screenB);
		QCOMPARE(byId.value(third).at(1).toString(), SCREEN);
		for (const auto id: {first, second, third}) {
			QCOMPARE(byId.value(id).at(2).toBool(), true);
			QCOMPARE(byId.value(id).at(3).toBool(), true);
			QCOMPARE(byId.value(id).at(4).toDouble(), 0.6);
		}

		// Ids are process-wide, so a recreated controller continues the sequence.
		FakeBackend replacement;
		replacement.observation = probeFor(BrightnessMethod::Lcd);
		Brightness recreated(replacement);
		QSignalSpy recreatedFinished(&recreated, &Brightness::queryFinished);
		const auto fourth = recreated.queryRequest(SCREEN);
		QVERIFY(fourth > third);
		QTRY_COMPARE(recreatedFinished.count(), 1);
		QCOMPARE(recreatedFinished.at(0).at(0).toLongLong(), fourth);
	}

	void queryRequestReportsUnavailableOutcome() {
		FakeBackend fake;
		fake.observation = probeFor(BrightnessMethod::Unresolved);
		Brightness brightness(fake);
		QSignalSpy finished(&brightness, &Brightness::queryFinished);
		QSignalSpy queried(&brightness, &Brightness::queried);

		const auto id = brightness.queryRequest(SCREEN);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(queried.count(), 1);
		QCOMPARE(finished.at(0), QVariantList({id, SCREEN, false, true, 1.0}));
		QCOMPARE(queried.at(0), QVariantList({SCREEN, false, true, 1.0}));
	}

	// queryFinished() precedes queried(), so a queried() handler that destroys
	// the controller cannot suppress the correlated completion.
	void queryFinishedSurvivesControllerDeletedInQueried() {
		FakeBackend fake;
		fake.observation = probeFor(BrightnessMethod::Ddc);
		auto* brightness = new Brightness(fake);
		QSignalSpy finished(brightness, &Brightness::queryFinished);
		QStringList order;
		QObject::connect(brightness, &Brightness::queryFinished, [&order]() { order << QStringLiteral("finished"); });
		QObject::connect(brightness, &Brightness::queried, [&order, brightness]() {
			order << QStringLiteral("queried");
			brightness->deleteLater();
		});

		const auto id = brightness->queryRequest(SCREEN);
		QVERIFY(id > 0);
		QTRY_COMPARE(order.size(), 2);
		QCOMPARE(order, QStringList({QStringLiteral("finished"), QStringLiteral("queried")}));
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(0).toLongLong(), id);
	}

	// Destroying the controller cancels its outstanding requests: they never
	// report, and nothing touches the destroyed object.
	void destroyedControllerCancelsDeferredRequests() {
		FakeBackend fake;
		fake.observation = probeFor(BrightnessMethod::Ddc);
		auto* brightness = new Brightness(fake);
		auto finishedCount = 0;
		QObject::connect(brightness, &Brightness::queryFinished, [&finishedCount]() { finishedCount++; });

		qs::windows::startup::Fixture::gateSettled = false;
		brightness->queryRequest(SCREEN);
		delete brightness;
		qs::windows::startup::Fixture::gateSettled = true;
		runDeferredStartup();
		QTest::qWait(50);
		QCOMPARE(finishedCount, 0);
	}

	void plainQueryEmitsNoQueryFinished() {
		FakeBackend fake;
		fake.observation = probeFor(BrightnessMethod::Ddc);
		Brightness brightness(fake);
		QSignalSpy finished(&brightness, &Brightness::queryFinished);
		QSignalSpy queried(&brightness, &Brightness::queried);

		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(finished.count(), 0);
		brightness.probe(SCREEN);
		QTRY_COMPARE(queried.count(), 2);
		QCOMPARE(finished.count(), 0);

		const auto id = brightness.queryRequest(SCREEN);
		QTRY_COMPARE(queried.count(), 3);
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(0).toLongLong(), id);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("ddc"));
	}

	// Deferred requests keep their id while they wait in the startup gate, so a
	// request issued after the gate settles dispatches and completes first. The
	// ids still pair each request with its own probe result.
	void deferredStartupGatePairsIdsOutOfOrder() {
		FakeBackend fake;
		fake.queuedProbes = {ddcProbeAt(0.11), ddcProbeAt(0.22), ddcProbeAt(0.33)};
		Brightness brightness(fake);
		QSignalSpy finished(&brightness, &Brightness::queryFinished);
		QSignalSpy queried(&brightness, &Brightness::queried);

		const auto screenA = screenNumbered(1);
		const auto screenB = screenNumbered(2);
		const auto screenC = screenNumbered(3);

		qs::windows::startup::Fixture::gateSettled = false;
		const auto deferredA = brightness.queryRequest(screenA);
		const auto deferredB = brightness.queryRequest(screenB);
		QCOMPARE(finished.count(), 0);
		QCOMPARE(queried.count(), 0);

		qs::windows::startup::Fixture::gateSettled = true;
		const auto dispatched = brightness.queryRequest(screenC);
		runDeferredStartup();
		QTRY_COMPARE(finished.count(), 3);
		QCOMPARE(queried.count(), 3);

		// The live request reached the backend first, so it completes first even
		// though it was issued last.
		QCOMPARE(finished.at(0).at(0).toLongLong(), dispatched);
		QCOMPARE(finished.at(1).at(0).toLongLong(), deferredA);
		QCOMPARE(finished.at(2).at(0).toLongLong(), deferredB);
		QCOMPARE(finished.at(0).at(1).toString(), screenC);
		QCOMPARE(finished.at(1).at(1).toString(), screenA);
		QCOMPARE(finished.at(2).at(1).toString(), screenB);
		QCOMPARE(finished.at(0).at(4).toDouble(), 0.11);
		QCOMPARE(finished.at(1).at(4).toDouble(), 0.22);
		QCOMPARE(finished.at(2).at(4).toDouble(), 0.33);
	}

	void gammaFailedClearRemainsRetryable_data() {
		QTest::addColumn<bool>("createFailure");
		QTest::addColumn<bool>("recreate");
		QTest::newRow("create-dc") << true << false;
		QTest::newRow("restore-ramp") << false << false;
		QTest::newRow("create-dc-recreated") << true << true;
		QTest::newRow("restore-ramp-recreated") << false << true;
	}

	void gammaFailedClearRemainsRetryable() {
		QFETCH(bool, createFailure);
		QFETCH(bool, recreate);
		FakeGammaApi native;
		GammaController gamma(native.api());
		FakeBackend fake;
		fake.gamma = &gamma;
		auto brightness = std::make_unique<Brightness>(fake);
		brightness->setStrictRoutes(true);
		resolve(*brightness, fake, BrightnessMethod::Software);
		brightness->setBrightness(SCREEN, false, 0.25);
		QVERIFY(gamma.dims(SCREEN));
		QVERIFY(!native.restored());
		WORD dimmed[3][256];
		std::memcpy(dimmed, native.ramp, sizeof(dimmed));

		native.createOk = !createFailure;
		native.setOk = createFailure;
		brightness->clearSoftwareDimming(SCREEN);
		QVERIFY(gamma.dims(SCREEN));
		QCOMPARE(std::memcmp(dimmed, native.ramp, sizeof(dimmed)), 0);
		if (recreate) {
			brightness = std::make_unique<Brightness>(fake);
			brightness->setStrictRoutes(true);
			QVERIFY(brightness->methodFor(SCREEN).isEmpty());
		} else {
			// Failure also retains the last known successful slider value.
			QSignalSpy queried(brightness.get(), &Brightness::queried);
			brightness->query(SCREEN);
			QTRY_COMPARE(queried.count(), 1);
			QCOMPARE(queried.at(0).at(3).toDouble(), 0.25);
		}
		native.createOk = true;
		native.setOk = true;
		// Reapplication must still use the previous factor, not the failed clear.
		gamma.probeDimFloor(SCREEN);
		QCOMPARE(std::memcmp(dimmed, native.ramp, sizeof(dimmed)), 0);
		brightness->clearSoftwareDimming(SCREEN);
		QVERIFY(!gamma.dims(SCREEN));
		QVERIFY(native.restored());
		const auto sets = native.sets;
		brightness->clearSoftwareDimming(SCREEN);
		gamma.restoreAll();
		QCOMPARE(native.sets, sets); // Successful restoration relinquishes ownership.
		QCOMPARE(native.openDcs, 0);
	}

	void gammaOriginalReadFailureRefusesAllMutation() {
		FakeGammaApi native;
		GammaController gamma(native.api());
		native.getOk = false;
		QVERIFY(!gamma.trySetBrightness(SCREEN, 0.25));
		QVERIFY(!gamma.dims(SCREEN));
		// The direct probing entry point must also refuse any unsaved mutation.
		gamma.probeDimFloor(QStringLiteral("probe-screen"));
		gamma.setDim(QStringLiteral("dim-screen"), 0.5);
		gamma.setTemperature(4200); // QCoreApplication has no screens to mutate.
		QVERIFY(!gamma.trySetBrightness(QStringLiteral("temperature-screen"), 1.0));
		gamma.restoreAll();
		QVERIFY(native.gets > 0);
		QCOMPARE(native.sets, 0);
		QVERIFY(native.restored());
		QCOMPARE(native.openDcs, 0);
	}

	void gammaFailedWriteDoesNotOwnSnapshot() {
		FakeGammaApi native;
		GammaController gamma(native.api());
		native.setOk = false;
		QVERIFY(!gamma.trySetBrightness(SCREEN, 0.25));
		QCOMPARE(native.gets, 1);
		QVERIFY(native.sets > 0);
		QVERIFY(!gamma.dims(SCREEN));
		QVERIFY(native.restored());
		const auto sets = native.sets;
		native.setOk = true;
		gamma.restoreAll();
		gamma.restoreAll();
		QCOMPARE(native.sets, sets);
		QCOMPARE(native.restores, 0);
		QCOMPARE(native.openDcs, 0);
		// A failed probe must not cache a permanent floor of 1.0.
		QVERIFY(gamma.trySetBrightness(SCREEN, 0.25));
		QVERIFY(gamma.dims(SCREEN));
		QVERIFY(!native.restored());
		gamma.restoreAll();
		QVERIFY(native.restored());
		QCOMPARE(native.restores, 1);
	}

	void gammaPartialDimFailureRemainsClearable_data() {
		QTest::addColumn<bool>("recreate");
		QTest::newRow("same-controller") << false;
		QTest::newRow("recreated") << true;
	}

	void gammaPartialDimFailureRemainsClearable() {
		QFETCH(bool, recreate);
		FakeGammaApi native;
		GammaController gamma(native.api());
		FakeBackend fake;
		fake.gamma = &gamma;
		// The floor probe writes a dark ramp; every later write, including the
		// undo, fails.
		native.setsBeforeFailure = 1;
		QVERIFY(!gamma.trySetBrightness(SCREEN, 0.25));
		QVERIFY(!native.restored());
		QVERIFY(gamma.dims(SCREEN));
		native.setsBeforeFailure = -1;
		native.setOk = true;
		auto brightness = std::make_unique<Brightness>(fake);
		brightness->setStrictRoutes(true);
		if (recreate) {
			brightness = std::make_unique<Brightness>(fake);
			brightness->setStrictRoutes(true);
		}
		brightness->clearSoftwareDimming(SCREEN);
		QVERIFY(!gamma.dims(SCREEN));
		QVERIFY(native.restored());
		const auto sets = native.sets;
		gamma.restoreAll();
		QCOMPARE(native.sets, sets);
		QCOMPARE(native.openDcs, 0);
	}

	void gammaUnownedSnapshotIsNotReused() {
		FakeGammaApi native;
		GammaController gamma(native.api());
		native.setOk = false;
		QVERIFY(!gamma.trySetBrightness(SCREEN, 0.25));
		QVERIFY(!gamma.dims(SCREEN));
		// Another application installs a different ramp between attempts.
		for (auto channel = 0; channel < 3; channel++) {
			for (auto i = 0; i < 256; i++) native.original[channel][i] = static_cast<WORD>(i * 200);
		}
		std::memcpy(native.ramp, native.original, sizeof(native.ramp));
		native.setOk = true;
		QVERIFY(gamma.trySetBrightness(SCREEN, 0.25));
		QCOMPARE(native.gets, 2);
		gamma.restoreAll();
		QVERIFY(native.restored());
		QCOMPARE(native.openDcs, 0);
	}

	void gammaShutdownRestoreFailureRetainsOwnership_data() {
		QTest::addColumn<bool>("createFailure");
		QTest::newRow("create-dc") << true;
		QTest::newRow("restore-ramp") << false;
	}

	void gammaShutdownRestoreFailureRetainsOwnership() {
		QFETCH(bool, createFailure);
		FakeGammaApi native;
		GammaController gamma(native.api());
		QVERIFY(gamma.trySetBrightness(SCREEN, 0.25));
		native.createOk = !createFailure;
		native.setOk = createFailure;
		gamma.restoreAll();
		QVERIFY(gamma.dims(SCREEN));
		QVERIFY(!native.restored());
		native.createOk = true;
		native.setOk = true;
		gamma.restoreAll();
		QVERIFY(!gamma.dims(SCREEN));
		QVERIFY(native.restored());
		const auto sets = native.sets;
		gamma.restoreAll();
		QCOMPARE(native.sets, sets);
		QCOMPARE(native.openDcs, 0);
	}

	void fakeGammaClearFailureMatchesProduction() {
		FakeBackend fake;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, BrightnessMethod::Software);
		brightness.setBrightness(SCREEN, false, 0.25);
		fake.gammaOk = false;
		brightness.clearSoftwareDimming(SCREEN);
		QVERIFY(fake.dimmed);
		fake.gammaOk = true;
		brightness.clearSoftwareDimming(SCREEN);
		QVERIFY(!fake.dimmed);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.25, 1.0, 1.0}));
	}

	void completionHandlerCanDestroyController_data() {
		QTest::addColumn<bool>("strict");
		QTest::addColumn<int>("method");
		QTest::addColumn<bool>("ok");
		QTest::addColumn<bool>("onResult");
		for (auto strict: {false, true}) {
			for (auto method:
			     {BrightnessMethod::Unresolved,
			      BrightnessMethod::Software,
			      BrightnessMethod::Wmi,
			      BrightnessMethod::Lcd,
			      BrightnessMethod::Ddc})
			{
				for (auto ok: {false, true}) {
					for (auto onResult: {false, true}) {
						// Legacy emits only brightnessSetFinished, never brightnessWriteResult.
						if (!strict && onResult) continue;
						const auto name = QStringLiteral("%1-%2-%3-%4")
						                      .arg(strict ? "strict" : "legacy")
						                      .arg(int(method))
						                      .arg(ok ? "success" : "failure")
						                      .arg(onResult ? "result" : "finished")
						                      .toUtf8();
						QTest::newRow(name.constData()) << strict << int(method) << ok << onResult;
					}
				}
			}
		}
	}

	void completionHandlerCanDestroyController() {
		QFETCH(bool, strict);
		QFETCH(int, method);
		QFETCH(bool, ok);
		QFETCH(bool, onResult);
		auto nativeMethod = static_cast<BrightnessMethod>(method);
		FakeBackend fake;
		fake.gammaOk = ok;
		fake.outcomes.insert(nativeMethod, ok);
		auto brightness = std::make_unique<Brightness>(fake);
		brightness->setStrictRoutes(strict);
		if (nativeMethod != BrightnessMethod::Unresolved) resolve(*brightness, fake, nativeMethod);
		QPointer<Brightness> guard(brightness.get());
		int finished = 0;
		int results = 0;
		bool legacyPolicyHeld = false;
		connect(
		    brightness.get(),
		    &Brightness::brightnessSetFinished,
		    this,
		    [&] {
			    finished++;
			    if (!strict) {
				    QTest::ignoreMessage(
				        QtWarningMsg,
				        "Cannot enable strictRoutes while brightness writes are active; retry when idle"
				    );
				    brightness->setStrictRoutes(true);
				    legacyPolicyHeld = !brightness->strictRoutes();
			    }
			    if (!onResult) brightness.reset();
		    },
		    Qt::DirectConnection
		);
		connect(
		    brightness.get(),
		    &Brightness::brightnessWriteResult,
		    this,
		    [&] {
			    results++;
			    if (onResult) brightness.reset();
		    },
		    Qt::DirectConnection
		);
		brightness->setBrightness(SCREEN, false, 0.4);
		QTRY_VERIFY(guard.isNull());
		QCOMPARE(finished, 1);
		QCOMPARE(results, onResult ? 1 : 0);
		if (!strict) QVERIFY(legacyPolicyHeld);
	}

	void routeHandlerCanDestroyController_data() {
		QTest::addColumn<int>("method");
		QTest::newRow("probe") << int(BrightnessMethod::Unresolved);
		QTest::newRow("wmi-fallback") << int(BrightnessMethod::Wmi);
		QTest::newRow("software-fallback") << int(BrightnessMethod::Ddc);
	}

	void routeHandlerCanDestroyController() {
		QFETCH(int, method);
		auto nativeMethod = static_cast<BrightnessMethod>(method);
		FakeBackend fake;
		auto brightness = std::make_unique<Brightness>(fake);
		if (nativeMethod != BrightnessMethod::Unresolved) resolve(*brightness, fake, nativeMethod);
		fake.outcomes.insert(nativeMethod, false);
		QPointer<Brightness> guard(brightness.get());
		int finished = 0;
		int queried = 0;
		connect(brightness.get(), &Brightness::brightnessSetFinished, this, [&] { finished++; });
		connect(brightness.get(), &Brightness::queried, this, [&] { queried++; });
		connect(
		    brightness.get(),
		    &Brightness::routeChanged,
		    this,
		    [&] { brightness.reset(); },
		    Qt::DirectConnection
		);
		if (nativeMethod == BrightnessMethod::Unresolved) {
			fake.observation = probeFor(BrightnessMethod::Software);
			brightness->query(SCREEN);
		} else {
			brightness->setBrightness(SCREEN, false, 0.4);
		}
		QTRY_VERIFY(guard.isNull());
		QCOMPARE(finished, 0);
		QCOMPARE(queried, 0);
		QVERIFY(fake.gammaWrites.isEmpty());
	}

	void strictDefaultsOffAndNotifiesOnlyOnChange() {
		FakeBackend fake;
		Brightness brightness(fake);
		QSignalSpy changed(&brightness, &Brightness::strictRoutesChanged);
		QVERIFY(!brightness.property("strictRoutes").toBool());
		QVERIFY(brightness.setProperty("strictRoutes", true));
		brightness.setStrictRoutes(true);
		QCOMPARE(changed.count(), 1);
		QVERIFY(brightness.strictRoutes());
		QTest::ignoreMessage(QtWarningMsg, "strictRoutes cannot be disabled once enabled");
		brightness.setStrictRoutes(false);
		QTest::failOnWarning("strictRoutes cannot be disabled once enabled");
		brightness.setStrictRoutes(false); // The refusal warning is emitted only once.
		QVERIFY(brightness.strictRoutes());
		QCOMPARE(changed.count(), 1);
	}

	void strictEnableRefusedUntilLegacyWritesFinish_data() {
		QTest::addColumn<bool>("queued");
		QTest::newRow("in-flight") << false;
		QTest::newRow("in-flight-and-queued") << true;
	}

	void strictEnableRefusedUntilLegacyWritesFinish() {
		QFETCH(bool, queued);
		FakeBackend fake;
		fake.blockWrite = true;
		Brightness brightness(fake);
		const auto unblock = qScopeGuard([&] { fake.release.release(2); });
		resolve(brightness, fake, BrightnessMethod::Ddc);
		QSignalSpy changed(&brightness, &Brightness::strictRoutesChanged);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		brightness.setBrightness(SCREEN, true, 0.4);
		QTRY_COMPARE(fake.entered.available(), 1);
		// The policy guard must cover writes on any screen.
		if (queued) brightness.setBrightness(SCREEN, true, 0.8);
		QTest::ignoreMessage(
		    QtWarningMsg,
		    "Cannot enable strictRoutes while brightness writes are active; retry when idle"
		);
		brightness.setStrictRoutes(true);
		QVERIFY(!brightness.strictRoutes());
		QCOMPARE(changed.count(), 0);
		fake.release.release(2);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(result.count(), 0);
		QCOMPARE(fake.writtenValues(), queued ? QList<qreal>({0.4, 0.8}) : QList<qreal>({0.4}));
		brightness.setStrictRoutes(true);
		QVERIFY(brightness.strictRoutes());
		QCOMPARE(changed.count(), 1);
	}

	void strictEnableRefusedDuringLegacyFallback() {
		FakeBackend fake;
		fake.outcomes.insert(BrightnessMethod::Wmi, false);
		Brightness brightness(fake);
		resolve(brightness, fake, BrightnessMethod::Wmi);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		bool tried = false;
		connect(&brightness, &Brightness::routeChanged, this, [&](const QString&, const QString&) {
			tried = true;
			QTest::ignoreMessage(
			    QtWarningMsg,
			    "Cannot enable strictRoutes while brightness writes are active; retry when idle"
			);
			brightness.setStrictRoutes(true);
			QVERIFY(!brightness.strictRoutes());
		});
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QVERIFY(tried);
		QCOMPARE(
		    fake.writtenMethods(),
		    QList<BrightnessMethod>({BrightnessMethod::Wmi, BrightnessMethod::Lcd})
		);
		brightness.setStrictRoutes(true);
		QVERIFY(brightness.strictRoutes());
	}

	void strictRejectsUnresolved() {
		FakeBackend fake;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.setBrightness(SCREEN, true, 0.4);
		QCOMPARE(result.count(), 1);
		QCOMPARE(
		    result.at(0),
		    QVariantList(
		        {SCREEN,
		         0.4,
		         false,
		         QStringLiteral("unresolved"),
		         QStringLiteral("no route resolved for this screen; query it before writing")}
		    )
		);
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toBool(), false);
		QVERIFY(brightness.methodFor(SCREEN).isEmpty());
		QVERIFY(fake.writtenMethods().isEmpty());
		QVERIFY(fake.gammaWrites.isEmpty());
	}

	void strictHardwareResult_data() {
		QTest::addColumn<int>("method");
		QTest::addColumn<QString>("route");
		QTest::addColumn<bool>("ok");
		for (auto ok: {false, true}) {
			QTest::newRow(ok ? "wmi-success" : "wmi-denied")
			    << int(BrightnessMethod::Wmi) << QStringLiteral("wmi") << ok;
			QTest::newRow(ok ? "lcd-success" : "lcd-denied")
			    << int(BrightnessMethod::Lcd) << QStringLiteral("lcd") << ok;
			QTest::newRow(ok ? "ddc-success" : "ddc-denied")
			    << int(BrightnessMethod::Ddc) << QStringLiteral("ddc") << ok;
		}
	}

	void strictHardwareResult() {
		QFETCH(int, method);
		QFETCH(QString, route);
		QFETCH(bool, ok);
		auto nativeMethod = static_cast<BrightnessMethod>(method);
		FakeBackend fake;
		fake.outcomes.insert(nativeMethod, ok);
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, nativeMethod);
		QCOMPARE(brightness.methodFor(SCREEN), route);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		QSignalSpy changed(&brightness, &Brightness::routeChanged);
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(result.count(), 1);
		QCOMPARE(
		    result.at(0),
		    QVariantList({SCREEN, 0.4, ok, route, ok ? QString() : QStringLiteral("fixture denied")})
		);
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0), QVariantList({SCREEN, ok}));
		QCOMPARE(changed.count(), 0);
		QCOMPARE(brightness.methodFor(SCREEN), route);
		QCOMPARE(fake.writtenMethods(), QList<BrightnessMethod>({nativeMethod}));
		QVERIFY(fake.gammaWrites.isEmpty());
		// A subsequent observation cannot create a duplicate write result.
		resolve(brightness, fake, nativeMethod);
		QCOMPARE(result.count(), 1);
		QCOMPARE(finished.count(), 1);
	}

	void strictSoftwareIsExplicitAndReportsFailure_data() {
		QTest::addColumn<bool>("ok");
		QTest::newRow("success") << true;
		QTest::newRow("denied") << false;
	}

	void strictSoftwareIsExplicitAndReportsFailure() {
		QFETCH(bool, ok);
		FakeBackend fake;
		fake.gammaOk = ok;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, BrightnessMethod::Software);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("software"));
		QVERIFY(fake.gammaWrites.isEmpty());
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.setBrightness(SCREEN, false, 0.3);
		QCOMPARE(result.count(), 1);
		QCOMPARE(
		    result.at(0),
		    QVariantList(
		        {SCREEN,
		         0.3,
		         ok,
		         QStringLiteral("software"),
		         ok ? QString() : QStringLiteral("software gamma write failed")}
		    )
		);
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toBool(), ok);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.3}));
		QVERIFY(fake.writtenMethods().isEmpty());
		QSignalSpy queried(&brightness, &Brightness::queried);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(queried.at(0).at(1).toBool(), ok);
		QCOMPARE(queried.at(0).at(3).toDouble(), ok ? 0.3 : 1.0);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.3}));
		QCOMPARE(result.count(), 1);
	}

	void queryGammaPolicy_data() {
		QTest::addColumn<bool>("strict");
		QTest::addColumn<bool>("query");
		QTest::newRow("strict-query") << true << true;
		QTest::newRow("strict-probe") << true << false;
		QTest::newRow("legacy-query") << false << true;
		QTest::newRow("legacy-probe") << false << false;
	}

	void queryGammaPolicy() {
		QFETCH(bool, strict);
		QFETCH(bool, query);
		FakeBackend fake;
		fake.dimmed = true; // This process has already applied gamma dimming.
		fake.observation = probeFor(BrightnessMethod::Ddc);
		Brightness brightness(fake);
		brightness.setStrictRoutes(strict);
		QSignalSpy queried(&brightness, &Brightness::queried);
		if (query) brightness.query(SCREEN);
		else brightness.probe(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(fake.gammaWrites, strict ? QList<qreal>() : QList<qreal>({1.0}));
		QCOMPARE(fake.dimmed, strict);
	}

	void strictQueryPreservesExplicitSoftwareWrite() {
		FakeBackend fake;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, BrightnessMethod::Software);
		brightness.setBrightness(SCREEN, true, 0.25);
		resolve(brightness, fake, BrightnessMethod::Software);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.25}));
		brightness.clearSoftwareDimming(SCREEN);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.25, 1.0}));
		QSignalSpy queried(&brightness, &Brightness::queried);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(queried.at(0).at(3).toDouble(), 1.0);
	}

	void strictClearRetainedGamma_data() {
		QTest::addColumn<bool>("recreate");
		QTest::addColumn<bool>("hardware");
		QTest::newRow("pruned-unresolved") << false << false;
		QTest::newRow("pruned-hardware") << false << true;
		QTest::newRow("recreated-unresolved") << true << false;
		QTest::newRow("recreated-hardware") << true << true;
	}

	void strictClearRetainedGamma() {
		QFETCH(bool, recreate);
		QFETCH(bool, hardware);
		FakeBackend fake;
		auto brightness = std::make_unique<Brightness>(fake);
		brightness->setStrictRoutes(true);
		resolve(*brightness, fake, BrightnessMethod::Software);
		brightness->setBrightness(SCREEN, false, 0.25);
		QVERIFY(fake.dimmed);
		if (recreate) {
			brightness = std::make_unique<Brightness>(fake);
			brightness->setStrictRoutes(true);
		} else {
			// QCoreApplication has no screens. Probing another name prunes the
			// old route exactly as a disconnected screen is pruned.
			QSignalSpy queried(brightness.get(), &Brightness::queried);
			brightness->query(QStringLiteral("another-screen"));
			QTRY_COMPARE(queried.count(), 1);
		}
		QVERIFY(brightness->methodFor(SCREEN).isEmpty());
		if (hardware) {
			resolve(*brightness, fake, BrightnessMethod::Ddc);
			QCOMPARE(brightness->methodFor(SCREEN), QStringLiteral("ddc"));
		}
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.25}));
		QVERIFY(fake.dimmed);
		brightness->clearSoftwareDimming(SCREEN);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.25, 1.0}));
		QVERIFY(!fake.dimmed);
		brightness->clearSoftwareDimming(SCREEN);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.25, 1.0}));
		QVERIFY(fake.writtenMethods().isEmpty());
	}

	void legacyFallbackUnchanged_data() {
		QTest::addColumn<bool>("lcdOk");
		QTest::newRow("wmi-to-lcd") << true;
		QTest::newRow("wmi-to-lcd-to-gamma") << false;
	}

	void legacyFallbackUnchanged() {
		QFETCH(bool, lcdOk);
		FakeBackend fake;
		fake.outcomes.insert(BrightnessMethod::Wmi, false);
		fake.outcomes.insert(BrightnessMethod::Lcd, lcdOk);
		Brightness brightness(fake);
		resolve(brightness, fake, BrightnessMethod::Wmi);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		QSignalSpy changed(&brightness, &Brightness::routeChanged);
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toBool(), true);
		QCOMPARE(result.count(), 0);
		QCOMPARE(
		    fake.writtenMethods(),
		    QList<BrightnessMethod>({BrightnessMethod::Wmi, BrightnessMethod::Lcd})
		);
		QCOMPARE(
		    brightness.methodFor(SCREEN),
		    lcdOk ? QStringLiteral("lcd") : QStringLiteral("software")
		);
		QCOMPARE(changed.count(), lcdOk ? 1 : 2);
		QCOMPARE(fake.gammaWrites, lcdOk ? QList<qreal>() : QList<qreal>({0.4}));
		if (!lcdOk) {
			brightness.setBrightness(SCREEN, false, 0.2);
			QCOMPARE(fake.gammaWrites, QList<qreal>({0.4, 0.2}));
			QCOMPARE(finished.count(), 2);
		}
	}

	void legacyUnresolvedStillTriesDdcThenGamma() {
		FakeBackend fake;
		fake.outcomes.insert(BrightnessMethod::Ddc, false);
		Brightness brightness(fake);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toBool(), true);
		QCOMPARE(fake.writtenMethods(), QList<BrightnessMethod>({BrightnessMethod::Ddc}));
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.4}));
	}

	void legacyOverlapsStillCoalesce() {
		FakeBackend fake;
		fake.blockWrite = true;
		Brightness brightness(fake);
		const auto unblock = qScopeGuard([&] { fake.release.release(2); });
		resolve(brightness, fake, BrightnessMethod::Ddc);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		brightness.setBrightness(SCREEN, true, 0.4);
		QTRY_COMPARE(fake.entered.available(), 1);
		brightness.setBrightness(SCREEN, true, 0.6);
		brightness.setBrightness(SCREEN, true, 0.8);
		fake.release.release(2);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(fake.writtenValues(), QList<qreal>({0.4, 0.8}));
		QCOMPARE(result.count(), 0);
	}

	void strictClampsValues() {
		FakeBackend fake;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, BrightnessMethod::Ddc);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		brightness.setBrightness(SCREEN, true, -0.5);
		QTRY_COMPARE(result.count(), 1);
		brightness.setBrightness(SCREEN, true, 1.5);
		QTRY_COMPARE(result.count(), 2);
		QCOMPARE(fake.writtenValues(), QList<qreal>({0.0, 1.0}));
		QCOMPARE(result.at(0).at(1).toDouble(), 0.0);
		QCOMPARE(result.at(1).at(1).toDouble(), 1.0);
	}

	void strictOverlapsAreRejectedExactlyOnce_data() {
		QTest::addColumn<bool>("ok");
		QTest::newRow("first-succeeds") << true;
		QTest::newRow("first-fails") << false;
	}

	void strictOverlapsAreRejectedExactlyOnce() {
		QFETCH(bool, ok);
		FakeBackend fake;
		fake.outcomes.insert(BrightnessMethod::Ddc, ok);
		fake.blockWrite = true;
		Brightness brightness(fake);
		const auto unblock = qScopeGuard([&] { fake.release.release(); });
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, BrightnessMethod::Ddc);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.setBrightness(SCREEN, true, 0.4);
		QTRY_COMPARE(fake.entered.available(), 1);
		brightness.setBrightness(SCREEN, true, 0.4); // Equal values still need separate results.
		brightness.setBrightness(SCREEN, true, 0.8);
		QCOMPARE(result.count(), 2);
		for (const auto& rejected: result) {
			QCOMPARE(rejected.at(2).toBool(), false);
			QCOMPARE(rejected.at(3).toString(), QStringLiteral("ddc"));
			QVERIFY(rejected.at(4).toString().contains(QStringLiteral("in flight")));
		}
		QTest::ignoreMessage(QtWarningMsg, "strictRoutes cannot be disabled once enabled");
		brightness.setStrictRoutes(false);
		QVERIFY(brightness.strictRoutes());
		fake.release.release();
		QTRY_COMPARE(result.count(), 3);
		QCOMPARE(result.at(2).at(1).toDouble(), 0.4);
		QCOMPARE(result.at(2).at(2).toBool(), ok);
		QCOMPARE(finished.count(), 3);
		QCOMPARE(fake.writtenMethods(), QList<BrightnessMethod>({BrightnessMethod::Ddc}));
		QVERIFY(fake.gammaWrites.isEmpty());
	}

	void strictResultHandlerCanSubmitNextWrite_data() {
		QTest::addColumn<int>("method");
		QTest::addColumn<bool>("firstOk");
		for (auto ok: {false, true}) {
			QTest::newRow(ok ? "wmi-success" : "wmi-denied") << int(BrightnessMethod::Wmi) << ok;
			QTest::newRow(ok ? "ddc-success" : "ddc-denied") << int(BrightnessMethod::Ddc) << ok;
		}
	}

	void strictResultHandlerCanSubmitNextWrite() {
		QFETCH(int, method);
		QFETCH(bool, firstOk);
		const auto nativeMethod = static_cast<BrightnessMethod>(method);
		FakeBackend fake;
		fake.outcomes.insert(nativeMethod, firstOk);
		fake.blockWrite = true;
		Brightness brightness(fake);
		const auto unblock = qScopeGuard([&] { fake.release.release(3); });
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, nativeMethod);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		connect(
		    &brightness,
		    &Brightness::brightnessWriteResult,
		    this,
		    [&](const QString&, qreal value, bool, const QString&, const QString&) {
			    if (value != 0.4) return;
			    fake.outcomes.insert(nativeMethod, true);
			    brightness.setBrightness(SCREEN, true, 0.8);
			    // The new write is accepted, then owns the busy state immediately.
			    QCOMPARE(result.count(), 1);
			    brightness.setBrightness(SCREEN, true, 0.9);
			    QCOMPARE(result.count(), 2);
		    }
		);
		brightness.setBrightness(SCREEN, true, 0.4);
		QTRY_COMPARE(fake.entered.available(), 1);
		fake.release.release();
		QTRY_COMPARE(result.count(), 2);
		QCOMPARE(result.at(0).at(1).toDouble(), 0.4);
		QCOMPARE(result.at(0).at(2).toBool(), firstOk);
		QCOMPARE(result.at(1).at(1).toDouble(), 0.9);
		QCOMPARE(result.at(1).at(2).toBool(), false);
		QVERIFY(result.at(1).at(4).toString().contains(QStringLiteral("in flight")));
		QTRY_COMPARE(fake.entered.available(), 2);
		QCOMPARE(fake.writtenValues(), QList<qreal>({0.4, 0.8}));
		fake.release.release();
		QTRY_COMPARE(result.count(), 3);
		QCOMPARE(result.at(2).at(1).toDouble(), 0.8);
		QCOMPARE(result.at(2).at(2).toBool(), true);
		brightness.setBrightness(SCREEN, true, 0.2);
		fake.release.release();
		QTRY_COMPARE(result.count(), 4);
		QCOMPARE(result.at(3).at(1).toDouble(), 0.2);
		QCOMPARE(result.at(3).at(2).toBool(), true);
		QCOMPARE(finished.count(), 4);
		QCOMPARE(fake.writtenValues(), QList<qreal>({0.4, 0.8, 0.2}));
	}

	void strictSoftwareResultHandlerKeepsLatestValue() {
		FakeBackend fake;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		resolve(brightness, fake, BrightnessMethod::Software);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		connect(
		    &brightness,
		    &Brightness::brightnessWriteResult,
		    this,
		    [&](const QString&, qreal value, bool, const QString&, const QString&) {
			    if (value == 0.4) brightness.setBrightness(SCREEN, false, 0.8);
		    }
		);
		brightness.setBrightness(SCREEN, false, 0.4);
		QCOMPARE(result.count(), 2);
		QCOMPARE(finished.count(), 2);
		QCOMPARE(result.at(0).at(1).toDouble(), 0.4);
		QCOMPARE(result.at(1).at(1).toDouble(), 0.8);
		QCOMPARE(fake.gammaWrites, QList<qreal>({0.4, 0.8}));
		QSignalSpy queried(&brightness, &Brightness::queried);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(queried.at(0).at(3).toDouble(), 0.8);
		QCOMPARE(result.count(), 2);
	}

	// A WMI instance that does not share the monitor's device path is a
	// different panel. It must not be adopted just because it is the only
	// unclaimed instance, whether WMI calls it active or not.
	void missingWmiCorrelationIsNotGuessed_data() {
		QTest::addColumn<bool>("active");
		QTest::newRow("active") << true;
		QTest::newRow("inactive") << false;
	}

	void missingWmiCorrelationIsNotGuessed() {
		QFETCH(bool, active);
		FakeBackend fake;
		Probe probe;
		probe.monitor.kind = PanelKind::Internal;
		MonitorTarget target;
		target.technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
		target.instanceId = QStringLiteral("DISPLAY\\OTHER\\PANEL");
		probe.monitor.targets.push_back(target);
		probe.monitor.internalTargets = 1;
		probe.wmiAsked = true;
		probe.wmi.push_back(
		    {QStringLiteral("fixture-panel"), QStringLiteral("fixture-path"), active, 50}
		);
		fake.observation = probe;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("software"));
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QVERIFY(fake.writtenMethods().isEmpty());
	}

	// A screen with no resolvable display target proves nothing about which
	// panel it is. Neither WMI nor the global \\.\LCD route may be guessed.
	void unknownMonitorTargetIsRejected() {
		FakeBackend fake;
		Probe probe;
		probe.monitor.kind = PanelKind::Unknown;
		probe.wmiAsked = true;
		probe.wmi.push_back({QStringLiteral("fixture-panel"), QStringLiteral("fixture-path"), true, 50});
		probe.lcdAsked = true;
		probe.lcd.usable = true;
		probe.lcd.current = 50;
		fake.observation = probe;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy result(&brightness, &Brightness::brightnessWriteResult);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QVERIFY(brightness.methodFor(SCREEN).isEmpty());
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(result.count(), 1);
		QCOMPARE(result.at(0).at(2).toBool(), false);
		QVERIFY(fake.writtenMethods().isEmpty());
	}

	// Two WMI instances carry the monitor's device path prefix, so neither can
	// be proven to be its panel.
	void ambiguousWmiCorrelationIsRejected() {
		FakeBackend fake;
		Probe probe;
		probe.monitor.kind = PanelKind::Internal;
		MonitorTarget target;
		target.technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
		target.instanceId = QStringLiteral("FIXTURE-PANEL");
		probe.monitor.targets.push_back(target);
		probe.monitor.internalTargets = 1;
		probe.wmiAsked = true;
		probe.wmi.push_back(
		    {QStringLiteral("FIXTURE-PANEL_0"), QStringLiteral("path-0"), true, 40}
		);
		probe.wmi.push_back(
		    {QStringLiteral("FIXTURE-PANEL_1"), QStringLiteral("path-1"), true, 80}
		);
		fake.observation = probe;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("software"));
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QVERIFY(fake.writtenMethods().isEmpty());
	}

	// \\.\LCD is one global device. With two built-in panels it cannot be
	// proven to address this screen's panel, so it is not used.
	void lcdRouteRequiresTheOnlyBuiltInPanel() {
		FakeBackend fake;
		auto probe = probeFor(BrightnessMethod::Lcd);
		probe.monitor.internalTargets = 2;
		fake.observation = probe;
		Brightness brightness(fake);
		brightness.setStrictRoutes(true);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("software"));
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QVERIFY(fake.writtenMethods().isEmpty());
	}

	// DDC/CI has no assumed range: each physical monitor is written through the
	// maximum a successful VCP query proved for that exact target.
	void ddcWriteUsesEachProvenRange() {
		const std::vector<int> targets {1, 2};
		QList<DWORD> levels;
		auto report = ddcWriteTargets(
		    0.5,
		    targets,
		    [](int target, DWORD& maximum, DWORD&) {
			    maximum = target == 1 ? 100 : 400;
			    return true;
		    },
		    [&levels](int, DWORD level, DWORD&) {
			    levels.append(level);
			    return true;
		    }
		);
		QVERIFY(report.ok());
		QCOMPARE(report.written, quint32(2));
		QVERIFY(report.failures.empty());
		QCOMPARE(levels, QList<DWORD>({50, 200}));
	}

	void ddcWriteRefusesUnprovenRange() {
		QList<DWORD> levels;
		auto report = ddcWriteTargets(
		    0.5,
		    std::vector<int> {1},
		    [](int, DWORD& maximum, DWORD& error) {
			    error = ERROR_NOT_SUPPORTED;
			    maximum = 0;
			    return false;
		    },
		    [&levels](int, DWORD level, DWORD&) {
			    levels.append(level);
			    return true;
		    }
		);
		QVERIFY(!report.ok());
		QCOMPARE(report.written, quint32(0));
		QCOMPARE(int(report.failures.size()), 1);
		QCOMPARE(report.failures[0].target, quint32(1));
		QVERIFY(report.failures[0].range);
		QCOMPARE(report.failures[0].error, DWORD(ERROR_NOT_SUPPORTED));
		QVERIFY(levels.isEmpty());

		// A zero maximum is equally unproven and is never replaced by 100.
		report = ddcWriteTargets(
		    1.0,
		    std::vector<int> {1},
		    [](int, DWORD& maximum, DWORD&) {
			    maximum = 0;
			    return true;
		    },
		    [&levels](int, DWORD level, DWORD&) {
			    levels.append(level);
			    return true;
		    }
		);
		QVERIFY(!report.ok());
		QCOMPARE(report.written, quint32(0));
		QCOMPARE(int(report.failures.size()), 1);
		QVERIFY(report.failures[0].range);
		QCOMPARE(report.failures[0].error, DWORD(ERROR_INVALID_DATA));
		QVERIFY(levels.isEmpty());
	}

	void ddcWriteReportsEveryFailedTarget() {
		QList<QPair<int, DWORD>> writes;
		auto report = ddcWriteTargets(
		    1.0,
		    std::vector<int> {1, 2, 3},
		    [](int target, DWORD& maximum, DWORD& error) {
			    if (target == 2) {
				    error = ERROR_NOT_SUPPORTED;
				    return false;
			    }
			    maximum = target == 3 ? 200 : 100;
			    return true;
		    },
		    [&writes](int target, DWORD level, DWORD& error) {
			    if (target == 3) {
				    error = ERROR_ACCESS_DENIED;
				    return false;
			    }
			    writes.append({target, level});
			    return true;
		    }
		);
		// One successful target cannot stand for the whole write.
		QVERIFY(!report.ok());
		QCOMPARE(report.written, quint32(1));
		QCOMPARE(int(report.failures.size()), 2);
		QCOMPARE(report.failures[0].target, quint32(2));
		QVERIFY(report.failures[0].range);
		QCOMPARE(report.failures[0].error, DWORD(ERROR_NOT_SUPPORTED));
		QCOMPARE(report.failures[1].target, quint32(3));
		QVERIFY(!report.failures[1].range);
		QCOMPARE(report.failures[1].error, DWORD(ERROR_ACCESS_DENIED));
		const QList<QPair<int, DWORD>> expected {{1, 100}};
		QCOMPARE(writes, expected);
	}

	// A failed WMI write on an external monitor must not retry the global
	// \\.\LCD device, which addresses the built-in panel instead.
	void externalWmiFailureNeverFallsBackToLcd() {
		FakeBackend fake;
		auto probe = probeFor(BrightnessMethod::Wmi);
		probe.monitor.kind = PanelKind::External;
		probe.monitor.internalTargets = 0;
		probe.monitor.targets[0].technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
		fake.observation = probe;
		fake.outcomes.insert(BrightnessMethod::Wmi, false);
		Brightness brightness(fake);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		brightness.query(SCREEN);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("wmi"));
		brightness.setBrightness(SCREEN, false, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(fake.writtenMethods(), QList<BrightnessMethod>({BrightnessMethod::Wmi}));
		QCOMPARE(brightness.methodFor(SCREEN), QStringLiteral("software"));
	}
};

QTEST_GUILESS_MAIN(BrightnessRouteTest)

#include "brightness_route_test.moc"
