// Two synthetic monitors whose vendor/model/friendly labels are identical.
// Brightness, gamma and the QML-facing names must route independently by the
// persistent target identity, not by the shared label. Nothing here touches
// real hardware: the native brightness backend and gamma calls are injected.

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qmutex.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <qsemaphore.h>
#include <qsignalspy.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qtest.h>

#include "../../core/qmlscreen.hpp"
#include "../screen_identity.hpp"
#include "../startup.hpp"
#include "../system/brightness_backend.hpp"
#include "../system/gamma.hpp"

using namespace qs::windows;
using namespace qs::windows::sys;

namespace {
QHash<QScreen*, HMONITOR> fixtureMonitors;
}

namespace qs::windows {
HMONITOR monitorForScreen(QScreen* screen) { return fixtureMonitors.value(screen, nullptr); }
} // namespace qs::windows

namespace qs::windows::startup {
bool settled() { return true; }
void afterFirstFrame(QObject*, std::function<void()> callback) { callback(); }
} // namespace qs::windows::startup

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace {

class FakeScreen: public QPlatformScreen {
public:
	QRect rect {0, 0, 1920, 1080};
	QString geometryName;
	QRect geometry() const override { return this->rect; }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_RGB32; }
	QString name() const override { return this->geometryName; }
	qreal devicePixelRatio() const override { return 1.0; }
	QDpi logicalDpi() const override { return {96, 96}; }
};

Probe ddcProbe() {
	Probe probe;
	probe.monitor.kind = PanelKind::External;
	MonitorTarget target;
	target.technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	target.instanceId = QStringLiteral("FIXTURE-EXTERNAL");
	probe.monitor.targets.push_back(target);
	probe.ddcAsked = true;
	probe.ddcOk = true;
	probe.ddcBrightness = 0.6;
	return probe;
}

Probe lcdProbe() {
	Probe probe;
	probe.monitor.kind = PanelKind::Internal;
	MonitorTarget target;
	target.technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
	target.instanceId = QStringLiteral("FIXTURE-INTERNAL");
	probe.monitor.targets.push_back(target);
	probe.monitor.internalTargets = 1;
	probe.lcdAsked = true;
	probe.lcd.usable = true;
	probe.lcd.current = 60;
	return probe;
}

class FakeBrightnessBackend final: public BrightnessBackend {
public:
	GammaController* gamma = nullptr;
	QList<HMONITOR> probes;
	QList<HMONITOR> writes;
	QList<BrightnessMethod> methods;
	QList<qreal> values;

	Probe probeScreen(HMONITOR monitor, bool) override {
		QMutexLocker lock(&this->mutex);
		this->probes.append(monitor);
		return monitor == this->fixtureDdc ? ddcProbe() : lcdProbe();
	}

	bool
	writeBrightness(const BrightnessRoute& route, HMONITOR monitor, qreal value, QString&) override {
		// A gated write stays in flight until the test hands it a permit.
		if (this->gated) this->writeGate.acquire();
		QMutexLocker lock(&this->mutex);
		this->writes.append(monitor);
		this->methods.append(route.method);
		this->values.append(value);
		return true;
	}

	int readBrightness(const BrightnessRoute&, QString&) override {
		++this->readsEntered;
		// A gated read stays in flight until the test hands it a permit.
		if (this->readsGated) this->readGate.acquire();
		++this->reads;
		return this->readValue;
	}
	bool gammaDims(const QString& screen) const override {
		return this->gamma != nullptr && this->gamma->dims(screen);
	}
	bool gammaSetBrightness(const QString& screen, qreal value) override {
		return this->gamma != nullptr && this->gamma->trySetBrightness(screen, value);
	}
	void startEventWatch(const std::function<void(const QString&, int)>&) override {}

	[[nodiscard]] QList<qreal> writtenValues() {
		QMutexLocker lock(&this->mutex);
		return this->values;
	}

	HMONITOR fixtureDdc = nullptr;
	std::atomic<bool> gated {false};
	QSemaphore writeGate;
	std::atomic<bool> readsGated {false};
	QSemaphore readGate;
	std::atomic<int> readsEntered {0};
	std::atomic<int> reads {0};
	std::atomic<int> readValue {60};

private:
	QMutex mutex;
};

class FakeGammaApi {
public:
	FakeGammaApi() {
		Q_ASSERT(current == nullptr);
		current = this;
	}
	~FakeGammaApi() { current = nullptr; }

	GammaController::NativeApi api() { return {&createDc, &deleteDc, &getRamp, &setRamp}; }

	QStringList createdDevices;
	QHash<QString, QByteArray> ramps;
	// Runs once inside the next SetDeviceGammaRamp, as a message pumped by the
	// driver would.
	std::function<void()> onSetRamp;
	// Same, inside the next GetDeviceGammaRamp.
	std::function<void()> onGetRamp;

	QByteArray rampFor(const QString& device) const { return this->ramps.value(device); }

private:
	struct Device {
		QString name;
		WORD ramp[3][256] {};     // NOLINT
		WORD original[3][256] {}; // NOLINT
	};

	static inline FakeGammaApi* current = nullptr;

	static Device* deviceFor(HDC dc) { return reinterpret_cast<Device*>(dc); }

	static HDC WINAPI createDc(LPCWSTR, LPCWSTR name, LPCWSTR, const DEVMODEW*) {
		auto deviceName = name ? QString::fromWCharArray(name) : QString();
		auto* device = new Device;
		device->name = deviceName;
		for (auto channel = 0; channel < 3; channel++) {
			for (auto i = 0; i < 256; i++) {
				device->original[channel][i] = static_cast<WORD>(i * i + channel); // NOLINT
				device->ramp[channel][i] = device->original[channel][i];
			}
		}
		current->createdDevices.append(deviceName);
		return reinterpret_cast<HDC>(device);
	}

	static BOOL WINAPI deleteDc(HDC dc) {
		delete deviceFor(dc);
		return TRUE;
	}

	static BOOL WINAPI getRamp(HDC dc, LPVOID data) {
		auto* device = deviceFor(dc);
		std::memcpy(data, device->ramp, sizeof(device->ramp));
		if (current->onGetRamp) std::exchange(current->onGetRamp, {})();
		return TRUE;
	}

	static BOOL WINAPI setRamp(HDC dc, LPVOID data) {
		auto* device = deviceFor(dc);
		std::memcpy(device->ramp, data, sizeof(device->ramp));
		QByteArray ramp;
		ramp.resize(sizeof(device->ramp));
		std::memcpy(ramp.data(), device->ramp, sizeof(device->ramp));
		current->ramps.insert(device->name, ramp);
		if (current->onSetRamp) std::exchange(current->onSetRamp, {})();
		return TRUE;
	}
};

} // namespace

// The original ramp every fake device starts with.
QByteArray originalRamp() {
	WORD original[3][256] {}; // NOLINT
	for (auto channel = 0; channel < 3; channel++)
		for (auto i = 0; i < 256; i++)
			original[channel][i] = static_cast<WORD>(i * i + channel); // NOLINT
	return QByteArray(reinterpret_cast<const char*>(original), sizeof(original));
}

// One screen. Without withPath it is session-only: neither DisplayConfig nor
// EnumDisplayDevices gave it an interface path. gainPath() supplies one later,
// remove() unplugs it and reconnect() plugs the same monitor back in.
struct TestScreen {
	explicit TestScreen(quintptr handle, bool withPath = false): handle(handle), withPath(withPath) {
		this->plug();
	}
	~TestScreen() {
		if (this->platform != nullptr) QWindowSystemInterface::handleScreenRemoved(this->platform);
		fixtureMonitors.clear();
	}
	[[nodiscard]] QString sourceOf(QScreen* target) const {
		return target == this->screen ? QStringLiteral("DISPLAY1") : QString();
	}
	void gainPath(ScreenIdentityRegistry& registry) {
		this->display[0].devicePath = QStringLiteral("generic-path-a");
		registry.refresh();
	}
	void remove(ScreenIdentityRegistry& registry) {
		fixtureMonitors.remove(this->screen);
		QWindowSystemInterface::handleScreenRemoved(this->platform);
		this->platform = nullptr;
		this->screen = nullptr;
		this->display.clear();
		registry.refresh();
	}
	void reconnect(ScreenIdentityRegistry& registry) {
		this->withPath = true;
		this->plug();
		registry.refresh();
	}

	FakeScreen* platform = nullptr;
	QScreen* screen = nullptr;
	HMONITOR monitor = nullptr;
	QList<DisplayIdentity> display;

private:
	void plug() {
		this->platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(this->platform);
		this->screen = this->platform->screen();
		this->monitor = reinterpret_cast<HMONITOR>(this->handle);
		fixtureMonitors.insert(this->screen, this->monitor);
		this->display = {{
		    QStringLiteral("DISPLAY1"),
		    this->withPath ? QStringLiteral("generic-path-a") : QString(),
		    QStringLiteral("Generic Monitor"),
		}};
	}

	quintptr handle;
	bool withPath;
};

class DisplayIdentityTest: public QObject {
	Q_OBJECT

private slots:
	void init() {
		this->displays = {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-b"),
		     QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		};
	}

	void identicalLabelsRouteBrightnessAndGammaIndependently() {
		auto* firstPlatform = new FakeScreen;
		firstPlatform->rect = {0, 0, 1920, 1080};
		firstPlatform->geometryName = QStringLiteral("generic-left");
		auto* secondPlatform = new FakeScreen;
		secondPlatform->rect = {1920, 0, 1920, 1080};
		secondPlatform->geometryName = QStringLiteral("generic-right");
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();

		const auto firstMonitor = reinterpret_cast<HMONITOR>(quintptr(0x1001));
		const auto secondMonitor = reinterpret_cast<HMONITOR>(quintptr(0x1002));
		fixtureMonitors.insert(first, firstMonitor);
		fixtureMonitors.insert(second, secondMonitor);

		ScreenIdentityRegistry registry(
		    [this] { return this->displays; },
		    [first, second](QScreen* target) {
			    if (target == first) return QStringLiteral("DISPLAY1");
			    if (target == second) return QStringLiteral("DISPLAY2");
			    return QString();
		    }
		);

		QuickshellScreenInfo firstInfo(nullptr, first, &registry);
		QuickshellScreenInfo secondInfo(nullptr, second, &registry);
		QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor"));
		QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor (2)"));
		QCOMPARE(registry.screenByName(firstInfo.name()), first);
		QCOMPARE(registry.screenByName(secondInfo.name()), second);

		FakeGammaApi gammaApi;
		GammaController gamma(gammaApi.api(), registry);
		QVERIFY(gamma.trySetBrightness(secondInfo.name(), 0.5));
		QVERIFY(gamma.dims(secondInfo.name()));
		QVERIFY(!gamma.dims(firstInfo.name()));
		QVERIFY(gammaApi.createdDevices.contains(QStringLiteral("DISPLAY2")));
		QVERIFY(!gammaApi.createdDevices.contains(QStringLiteral("DISPLAY1")));
		const auto secondRamp = gammaApi.rampFor(QStringLiteral("DISPLAY2"));
		QVERIFY(!secondRamp.isEmpty());

		// The same friendly label but the other target gets its own factor,
		// ramp and native DISPLAY source.
		QVERIFY(gamma.trySetBrightness(firstInfo.name(), 0.75));
		QVERIFY(gamma.dims(firstInfo.name()));
		QVERIFY(gamma.dims(secondInfo.name()));
		QVERIFY(gammaApi.createdDevices.contains(QStringLiteral("DISPLAY1")));
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY2")), secondRamp);

		// Changing one target cannot rewrite the other's ramp.
		gamma.setDim(firstInfo.name(), 0.5);
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY2")), secondRamp);

		// Clearing one leaves the other dimmed.
		QVERIFY(gamma.trySetBrightness(secondInfo.name(), 1.0));
		QVERIFY(!gamma.dims(secondInfo.name()));
		QVERIFY(gamma.dims(firstInfo.name()));
		gamma.restoreAll();

		FakeBrightnessBackend backend;
		backend.gamma = &gamma;
		backend.fixtureDdc = firstMonitor;
		Brightness brightness(backend, registry);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);

		brightness.query(firstInfo.name());
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(queried.at(0).at(0).toString(), firstInfo.name());
		QCOMPARE(brightness.methodFor(firstInfo.name()), QStringLiteral("ddc"));
		QCOMPARE(brightness.methodFor(secondInfo.name()), QString());

		brightness.query(secondInfo.name());
		QTRY_COMPARE(queried.count(), 2);
		QCOMPARE(queried.at(1).at(0).toString(), secondInfo.name());
		QCOMPARE(brightness.methodFor(secondInfo.name()), QStringLiteral("lcd"));
		// Querying the second target did not overwrite the first route.
		QCOMPARE(brightness.methodFor(firstInfo.name()), QStringLiteral("ddc"));
		QCOMPARE(backend.probes, QList<HMONITOR>({firstMonitor, secondMonitor}));

		brightness.setBrightness(firstInfo.name(), true, 0.4);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(backend.writes, QList<HMONITOR>({firstMonitor}));
		QCOMPARE(backend.methods, QList<BrightnessMethod>({BrightnessMethod::Ddc}));
		QCOMPARE(backend.values, QList<qreal>({0.4}));

		brightness.setBrightness(secondInfo.name(), false, 0.7);
		QTRY_COMPARE(finished.count(), 2);
		QCOMPARE(backend.writes, QList<HMONITOR>({firstMonitor, secondMonitor}));
		QCOMPARE(backend.methods.at(1), BrightnessMethod::Lcd);

		QWindowSystemInterface::handleScreenRemoved(secondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);
		fixtureMonitors.clear();
	}

	void pathAppearingLaterIsARemovalAndAnAddition() {
		TestScreen fixture(0x3001);
		ScreenIdentityRegistry registry(
		    [&fixture] { return fixture.display; },
		    [&fixture](QScreen* target) { return fixture.sourceOf(target); }
		);
		QuickshellScreenInfo info(nullptr, fixture.screen, &registry);
		const auto name = info.name();
		const auto sessionId = registry.identity(fixture.screen).stableId;
		QVERIFY(!registry.identity(fixture.screen).persistent);

		FakeGammaApi gammaApi;
		GammaController gamma(gammaApi.api(), registry);
		FakeBrightnessBackend backend;
		backend.fixtureDdc = fixture.monitor;
		backend.gamma = &gamma;
		Brightness brightness(backend, registry);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy finished(&brightness, &Brightness::brightnessSetFinished);
		QSignalSpy retired(&registry, &ScreenIdentityRegistry::identityRetired);
		brightness.query(name);
		QTRY_COMPARE(queried.count(), 1);

		// Dimmed through the gamma ramp, with a brightness write still in flight.
		QVERIFY(gamma.trySetBrightness(name, 0.5));
		const auto dimmed = gammaApi.rampFor(QStringLiteral("DISPLAY1"));
		QVERIFY(dimmed != originalRamp());
		backend.gated = true;
		brightness.setBrightness(name, true, 0.4);

		// The interface path appears: the session id is retired as a removal.
		fixture.gainPath(registry);
		QVERIFY(registry.identity(fixture.screen).persistent);
		QCOMPARE(retired.count(), 1);
		QCOMPARE(retired.at(0).at(0).toString(), sessionId);
		QCOMPARE(retired.at(0).at(1).value<QScreen*>(), fixture.screen);

		// The obligation was honoured first: the saved ramp is back, nothing is
		// left dimmed, and the new identity starts from nothing.
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY1")), originalRamp());
		QVERIFY(!gamma.dims(info.name()));
		QCOMPARE(brightness.methodFor(info.name()), QString());

		// The write that was running finishes against the retired id: dropped and
		// reported as failed, without leaving the new identity busy.
		backend.writeGate.release(1);
		QTRY_COMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toBool(), false);
		backend.gated = false;
		brightness.query(info.name());
		QTRY_COMPARE(queried.count(), 2);
		brightness.setBrightness(info.name(), true, 0.7);
		QTRY_COMPARE(finished.count(), 2);
		QCOMPARE(finished.at(1).at(1).toBool(), true);
		QCOMPARE(backend.writtenValues(), QList<qreal>({0.4, 0.7}));
	}

	void removalDuringAnInFlightWriteDropsItAndAReconnectCanWrite() {
		TestScreen fixture(0x3002, true);
		ScreenIdentityRegistry registry(
		    [&fixture] { return fixture.display; },
		    [&fixture](QScreen* target) { return fixture.sourceOf(target); }
		);
		auto info = std::make_unique<QuickshellScreenInfo>(nullptr, fixture.screen, &registry);
		const auto name = info->name();
		FakeBrightnessBackend backend;
		backend.fixtureDdc = fixture.monitor;
		Brightness brightness(backend, registry);
		brightness.setStrictRoutes(true);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy results(&brightness, &Brightness::brightnessWriteResult);
		brightness.query(name);
		QTRY_COMPARE(queried.count(), 1);

		backend.gated = true;
		brightness.setBrightness(name, true, 0.4);
		QCOMPARE(results.count(), 0);
		info.reset();
		fixture.remove(registry);

		backend.writeGate.release(1);
		QTRY_COMPARE(results.count(), 1);
		QCOMPARE(results.at(0).at(2).toBool(), false);

		// The same monitor comes back with the same persistent id. The write that
		// was in flight when it left must not have left it busy.
		fixture.reconnect(registry);
		QVERIFY(registry.identity(fixture.screen).persistent);
		backend.fixtureDdc = fixture.monitor;
		backend.gated = false;
		QuickshellScreenInfo again(nullptr, fixture.screen, &registry);
		brightness.query(again.name());
		QTRY_COMPARE(queried.count(), 2);
		brightness.setBrightness(again.name(), true, 0.7);
		QTRY_COMPARE(results.count(), 2);
		QCOMPARE(results.at(1).at(2).toBool(), true);
		QCOMPARE(backend.writtenValues(), QList<qreal>({0.4, 0.7}));
	}

	void removalDuringAReadBackPublishesNothing() {
		TestScreen fixture(0x3003, true);
		ScreenIdentityRegistry registry(
		    [&fixture] { return fixture.display; },
		    [&fixture](QScreen* target) { return fixture.sourceOf(target); }
		);
		auto info = std::make_unique<QuickshellScreenInfo>(nullptr, fixture.screen, &registry);
		const auto name = info->name();
		FakeBrightnessBackend backend; // No DDC monitor: this one resolves to the LCD route.
		backend.readValue = 80;
		Brightness brightness(backend, registry);
		QSignalSpy queried(&brightness, &Brightness::queried);
		QSignalSpy changed(&brightness, &Brightness::systemBrightnessChanged);
		brightness.query(name);
		QTRY_COMPARE(queried.count(), 1);
		QCOMPARE(brightness.methodFor(name), QStringLiteral("lcd"));

		// The read-back is running (and held there) when the monitor is unplugged.
		backend.readsGated = true;
		brightness.refresh(name);
		QTRY_COMPARE(backend.readsEntered.load(), 1);
		info.reset();
		fixture.remove(registry);
		backend.readGate.release(1);

		// One worker thread runs the tasks in order, so once the next probe has
		// completed the stale read-back's completion has been handled.
		fixture.reconnect(registry);
		QuickshellScreenInfo again(nullptr, fixture.screen, &registry);
		brightness.query(again.name());
		QTRY_COMPARE(queried.count(), 2);
		QCOMPARE(backend.reads.load(), 1);
		QCOMPARE(changed.count(), 0);

		// A read-back of the reconnected screen is published as usual.
		backend.readsGated = false;
		brightness.refresh(again.name());
		QTRY_COMPARE(changed.count(), 1);
		QCOMPARE(changed.at(0).at(0).toString(), again.name());
		QCOMPARE(changed.at(0).at(1).toDouble(), 0.8);
	}

	void identityChangingDuringTheGammaReadKeepsNoSnapshot() {
		TestScreen fixture(0x3004);
		ScreenIdentityRegistry registry(
		    [&fixture] { return fixture.display; },
		    [&fixture](QScreen* target) { return fixture.sourceOf(target); }
		);
		QuickshellScreenInfo info(nullptr, fixture.screen, &registry);
		const auto name = info.name();
		FakeGammaApi gammaApi;
		GammaController gamma(gammaApi.api(), registry);

		// The interface path appears while the original ramp is being read.
		gammaApi.onGetRamp = [&] { fixture.gainPath(registry); };
		QVERIFY(!gamma.trySetBrightness(name, 0.5));
		QVERIFY(registry.identity(fixture.screen).persistent);
		// Nothing was written and nothing is owed.
		QVERIFY(gammaApi.rampFor(QStringLiteral("DISPLAY1")).isEmpty());
		QVERIFY(!gamma.dims(info.name()));

		// The new identity takes its own snapshot, so restoring gives the real
		// original back, never a blank ramp.
		QVERIFY(gamma.trySetBrightness(info.name(), 0.5));
		QVERIFY(gamma.trySetBrightness(info.name(), 1.0));
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY1")), originalRamp());
	}

	void identityChangingDuringTheGammaWriteUndoesTheWrite() {
		TestScreen fixture(0x3005);
		ScreenIdentityRegistry registry(
		    [&fixture] { return fixture.display; },
		    [&fixture](QScreen* target) { return fixture.sourceOf(target); }
		);
		QuickshellScreenInfo info(nullptr, fixture.screen, &registry);
		const auto name = info.name();
		FakeGammaApi gammaApi;
		GammaController gamma(gammaApi.api(), registry);

		// The interface path appears inside the first SetDeviceGammaRamp.
		gammaApi.onSetRamp = [&] { fixture.gainPath(registry); };
		QVERIFY(!gamma.trySetBrightness(name, 0.5));
		QVERIFY(registry.identity(fixture.screen).persistent);
		// The write that was running is put back: nothing stays dimmed.
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY1")), originalRamp());
		QVERIFY(!gamma.dims(info.name()));

		// And the new identity works normally.
		QVERIFY(gamma.trySetBrightness(info.name(), 0.5));
		QVERIFY(gamma.dims(info.name()));
		QVERIFY(gamma.trySetBrightness(info.name(), 1.0));
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY1")), originalRamp());
	}

	void gammaRetryNeverCrossesIntoAReconnectedScreen() {
		TestScreen fixture(0x3006, true);
		ScreenIdentityRegistry registry(
		    [&fixture] { return fixture.display; },
		    [&fixture](QScreen* target) { return fixture.sourceOf(target); }
		);
		const auto name = registry.identity(fixture.screen).uniqueName;
		const auto id = registry.identity(fixture.screen).stableId;
		FakeGammaApi gammaApi;
		GammaController gamma(gammaApi.api(), registry);

		// The first floor probe is inside SetDeviceGammaRamp when the monitor is
		// unplugged and plugged back in under the same persistent id.
		gammaApi.onSetRamp = [&] {
			fixture.remove(registry);
			fixture.reconnect(registry);
		};
		QVERIFY(!gamma.trySetBrightness(name, 0.5));
		QCOMPARE(registry.identity(fixture.screen).stableId, id);

		// The operation that started before the reconnect writes nothing to the
		// screen that came back. The write that was running was put back, and the
		// probe's retry at the next step did not follow it.
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY1")), originalRamp());
		QVERIFY(!gamma.dims(registry.identity(fixture.screen).uniqueName));

		// The reconnected screen is fully usable under its new lifetime.
		const auto reconnectedName = registry.identity(fixture.screen).uniqueName;
		QVERIFY(gamma.trySetBrightness(reconnectedName, 0.5));
		QVERIFY(gammaApi.rampFor(QStringLiteral("DISPLAY1")) != originalRamp());
		QVERIFY(gamma.trySetBrightness(reconnectedName, 1.0));
		QCOMPARE(gammaApi.rampFor(QStringLiteral("DISPLAY1")), originalRamp());
	}

private:
	QList<DisplayIdentity> displays;
};

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	qputenv("QT_SCALE_FACTOR", "1");
	QGuiApplication app(argc, argv);
	DisplayIdentityTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "display_identity_test.moc"
