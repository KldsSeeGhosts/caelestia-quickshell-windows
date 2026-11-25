#include "gamma.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qscreen.h>
#include <qstring.h>

#include <powrprof.h>

#include "../../core/logcat.hpp"
#include "../redact.hpp"
#include "../screen_identity.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logGamma, "quickshell.windows.gamma", QtInfoMsg);

constexpr qreal BRIGHTNESS_STEP = 0.05;
constexpr int REAPPLY_DEBOUNCE_MS = 400;
constexpr size_t CRASH_SNAPSHOT_COUNT = 32;
constexpr size_t CRASH_KEY_CHARS = 384;
constexpr size_t CRASH_DEVICE_CHARS = 32;

struct CrashSnapshot {
	std::atomic<quint64> sequence {0};
	wchar_t key[CRASH_KEY_CHARS] {};
	wchar_t device[CRASH_DEVICE_CHARS] {};
	WORD ramp[3][256] {}; // NOLINT
};

CrashSnapshot gCrashSnapshots[CRASH_SNAPSHOT_COUNT]; // NOLINT
std::atomic<bool> gExitRestoreRegistered = false;     // NOLINT

void copyWide(wchar_t* target, size_t capacity, const std::wstring& value) {
	if (capacity == 0) return;
	auto length = std::min(value.size(), capacity - 1);
	std::memcpy(target, value.data(), length * sizeof(wchar_t));
	target[length] = L'\0';
}

void writeSnapshotKey(CrashSnapshot& snapshot, const QString& key) {
	copyWide(snapshot.key, CRASH_KEY_CHARS, key.toStdWString());
}

bool snapshotKeyIs(const CrashSnapshot& snapshot, const QString& key) {
	return key.compare(QString::fromWCharArray(snapshot.key), Qt::CaseInsensitive) == 0;
}

void factorsForTemperature(int kelvin, double factors[3]) { // NOLINT
	auto temp = std::clamp(kelvin, 1000, 40000) / 100.0;

	double red = 255.0;
	double green = 255.0;
	double blue = 255.0;

	if (temp > 66.0) red = 329.698727446 * std::pow(temp - 60.0, -0.1332047592);

	if (temp <= 66.0) green = 99.4708025861 * std::log(temp) - 161.1195681661;
	else green = 288.1221695283 * std::pow(temp - 60.0, -0.0755148492);

	if (temp >= 66.0) blue = 255.0;
	else if (temp <= 19.0) blue = 0.0;
	else blue = 138.5177312231 * std::log(temp - 10.0) - 305.0447927307;

	factors[0] = std::clamp(red, 0.0, 255.0) / 255.0;
	factors[1] = std::clamp(green, 0.0, 255.0) / 255.0;
	factors[2] = std::clamp(blue, 0.0, 255.0) / 255.0;
}

} // namespace

struct GammaController::PowerNotifications {
	explicit PowerNotifications(GammaController* controller) {
		constexpr auto windowClass = L"QuickshellGammaPowerNotifications";
		WNDCLASSW wc {};
		wc.lpfnWndProc = &PowerNotifications::windowProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = windowClass;
		RegisterClassW(&wc);
		this->window = CreateWindowExW(
		    0,
		    windowClass,
		    L"",
		    0,
		    0,
		    0,
		    0,
		    0,
		    HWND_MESSAGE,
		    nullptr,
		    wc.hInstance,
		    controller
		);
		if (this->window == nullptr) return;
		this->consoleDisplay = RegisterPowerSettingNotification(
		    this->window,
		    &GUID_CONSOLE_DISPLAY_STATE,
		    DEVICE_NOTIFY_WINDOW_HANDLE
		);
		this->monitorPower = RegisterPowerSettingNotification(
		    this->window,
		    &GUID_MONITOR_POWER_ON,
		    DEVICE_NOTIFY_WINDOW_HANDLE
		);
	}

	~PowerNotifications() {
		if (this->consoleDisplay != nullptr) UnregisterPowerSettingNotification(this->consoleDisplay);
		if (this->monitorPower != nullptr) UnregisterPowerSettingNotification(this->monitorPower);
		if (this->window != nullptr) DestroyWindow(this->window);
	}

	static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		if (message == WM_NCCREATE) {
			auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		} else if (message == WM_POWERBROADCAST
		           && (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND
		               || wParam == PBT_POWERSETTINGCHANGE))
		{
			auto* controller =
			    reinterpret_cast<GammaController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (controller != nullptr) controller->scheduleReapply();
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	HWND window = nullptr;
	HPOWERNOTIFY consoleDisplay = nullptr;
	HPOWERNOTIFY monitorPower = nullptr;
};

GammaController* GammaController::instance() {
	static QPointer<GammaController> controller; // NOLINT

	if (controller.isNull()) {
		controller = new GammaController(QCoreApplication::instance());
	}

	return controller.data();
}

GammaController::GammaController(QObject* parent)
    : GammaController({&CreateDCW, &DeleteDC, &GetDeviceGammaRamp, &SetDeviceGammaRamp}, parent) {
	this->powerNotifications = std::make_unique<PowerNotifications>(this);
}

GammaController::GammaController(NativeApi api, QObject* parent)
    : QObject(parent)
    , api(api)
    , identities(
          qobject_cast<QGuiApplication*>(QCoreApplication::instance())
              ? ScreenIdentityRegistry::instance()
              : nullptr
      ) {
	this->init();
}

GammaController::GammaController(
    NativeApi api,
    qs::windows::ScreenIdentityRegistry& identities,
    QObject* parent
)
    : QObject(parent)
    , api(api)
    , identities(&identities) {
	this->init();
}

void GammaController::init() {
	this->reapplyTimer.setSingleShot(true);
	this->reapplyTimer.setInterval(REAPPLY_DEBOUNCE_MS);
	QObject::connect(&this->reapplyTimer, &QTimer::timeout, this, &GammaController::reapplyActive);
	if (auto* app = QCoreApplication::instance()) {
		app->installNativeEventFilter(this);
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, &GammaController::restoreAll);
	}
	if (!gExitRestoreRegistered.exchange(true)) {
		std::atexit(&GammaController::restoreForCrash);
	}
	if (this->identities != nullptr) this->watchIdentities();
}

GammaController::~GammaController() {
	if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
}

bool GammaController::nativeEventFilter(const QByteArray&, void* message, qintptr*) {
	auto* msg = static_cast<MSG*>(message);
	if (msg == nullptr) return false;
	if (msg->message == WM_DISPLAYCHANGE
	    || (msg->message == WM_POWERBROADCAST
	        && (msg->wParam == PBT_APMRESUMEAUTOMATIC || msg->wParam == PBT_APMRESUMESUSPEND
	            || msg->wParam == PBT_POWERSETTINGCHANGE)))
	{
		this->scheduleReapply();
	}
	return false;
}

void GammaController::scheduleReapply() { this->reapplyTimer.start(); }

void GammaController::reapplyActive() {
	// `written` is the ownership set: a display reset does not remove the duty to
	// put this process's desired ramp back. Resolve every id again because GDI
	// display numbers may have been reordered underneath the same stable ids.
	auto keys = this->written;
	for (const auto& key: keys) this->reapply(key);
}

void GammaController::updateCrashSnapshot(const QString& key, const Ramp& ramp) {
	auto device = this->deviceFor(key);
	if (device.isEmpty()) return;

	CrashSnapshot* empty = nullptr;
	CrashSnapshot* target = nullptr;
	for (auto& snapshot: gCrashSnapshots) {
		if (snapshotKeyIs(snapshot, key)) {
			target = &snapshot;
			break;
		}
		if (empty == nullptr && snapshot.key[0] == L'\0') empty = &snapshot;
	}
	if (target == nullptr) target = empty;
	if (target == nullptr) return;

	// Odd means a partial update. A crash reader only accepts an unchanged even
	// sequence, so a crash during this copy loses this entry but never applies a
	// torn ramp to the wrong display.
	target->sequence.fetch_add(1, std::memory_order_acq_rel);
	writeSnapshotKey(*target, key);
	copyWide(target->device, CRASH_DEVICE_CHARS, device.toStdWString());
	std::memcpy(target->ramp, ramp.values, sizeof(target->ramp));
	target->sequence.fetch_add(1, std::memory_order_acq_rel);
}

void GammaController::removeCrashSnapshot(const QString& key) {
	for (auto& snapshot: gCrashSnapshots) {
		if (!snapshotKeyIs(snapshot, key)) continue;
		snapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
		snapshot.key[0] = L'\0';
		snapshot.device[0] = L'\0';
		snapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
	}
}

quintptr GammaController::crashSnapshotCount() {
	quintptr count = 0;
	for (const auto& snapshot: gCrashSnapshots) {
		auto sequence = snapshot.sequence.load(std::memory_order_acquire);
		if (sequence != 0 && sequence % 2 == 0 && snapshot.key[0] != L'\0') ++count;
	}
	return count;
}

void GammaController::clearCrashSnapshots() {
	for (auto& snapshot: gCrashSnapshots) {
		snapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
		snapshot.key[0] = L'\0';
		snapshot.device[0] = L'\0';
		snapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
	}
}

void GammaController::restoreSnapshots(const NativeApi& api) {
	for (auto& snapshot: gCrashSnapshots) {
		auto before = snapshot.sequence.load(std::memory_order_acquire);
		if (before == 0 || before % 2 != 0) continue;

		wchar_t device[CRASH_DEVICE_CHARS] {};
		WORD ramp[3][256] {}; // NOLINT
		std::memcpy(device, snapshot.device, sizeof(device));
		std::memcpy(ramp, snapshot.ramp, sizeof(ramp));
		auto after = snapshot.sequence.load(std::memory_order_acquire);
		if (before != after || device[0] == L'\0') continue;

		auto* hdc = api.createDc(L"DISPLAY", device, nullptr, nullptr);
		if (hdc == nullptr) continue;
		api.setRamp(hdc, ramp);
		api.deleteDc(hdc);
	}
}

void GammaController::restoreForCrash() {
	restoreSnapshots({&CreateDCW, &DeleteDC, &GetDeviceGammaRamp, &SetDeviceGammaRamp});
}

void GammaController::watchIdentities() {
	QObject::connect(
	    this->identities,
	    &ScreenIdentityRegistry::identityRetired,
	    this,
	    [this](const QString& id, QScreen*) { this->retire(id); }
	);
	QObject::connect(
	    this->identities,
	    &ScreenIdentityRegistry::screenChanged,
	    this,
	    [this](QScreen*) { this->scheduleReapply(); }
	);
}

QString GammaController::keyFor(const QString& screenName) const {
	if (this->identities == nullptr) return screenName;
	auto* screen = this->identities->screenByName(screenName);
	if (screen == nullptr) return screenName;
	auto id = this->identities->identity(screen).stableId;
	return id.isEmpty() ? screenName : id;
}

void GammaController::setTemperature(int kelvin) {
	this->mKelvin = kelvin;

	for (auto* screen: QGuiApplication::screens()) {
		auto identity =
		    this->identities != nullptr ? this->identities->identity(screen) : DisplayIdentity {};
		auto key = identity.stableId.isEmpty() ? screenName(screen) : identity.stableId;
		this->reapply(key);
	}
}

void GammaController::setBrightness(const QString& screenName, qreal brightness) {
	this->trySetBrightness(screenName, brightness);
}

bool GammaController::trySetBrightness(const QString& screenName, qreal brightness) {
	auto key = this->keyFor(screenName);
	const auto lifetime = this->lifetimeOf(key);
	auto clamped = std::clamp(brightness, 0.0, 1.0);
	auto previous = this->backlightFactor.value(key, 1.0);
	if (clamped >= 1.0) this->backlightFactor.remove(key);
	else this->backlightFactor.insert(key, clamped);
	if (this->reapply(key)) return true;
	// The screen went away or changed identity during the writes. Its state was
	// dropped with it and a stale call must not bring any of it back.
	if (this->lifetimeOf(key) != lifetime) return false;

	// A failed dim can still leave a written ramp (e.g. a successful floor probe
	// whose undo failed). Keep the requested factor so a clear stays eligible.
	if (clamped < 1.0 && previous >= 1.0 && this->written.contains(key)) return false;

	// In particular, a failed clear must retain the previous factor so another
	// controller can discover and retry the outstanding restoration.
	if (previous >= 1.0) this->backlightFactor.remove(key);
	else this->backlightFactor.insert(key, previous);
	return false;
}

void GammaController::setDim(const QString& screenName, qreal factor) {
	auto key = this->keyFor(screenName);
	auto clamped = std::clamp(factor, 0.0, 1.0);
	if (clamped >= 1.0) this->dimFactor.remove(key);
	else this->dimFactor.insert(key, clamped);
	this->reapply(key);
}

qreal GammaController::probeDimFloor(const QString& screenName) {
	auto key = this->keyFor(screenName);
	auto floor = this->floorFor(key);
	this->reapply(key);
	return floor;
}

bool GammaController::dims(const QString& screenName) const {
	return this->backlightFactor.contains(this->keyFor(screenName));
}

qreal GammaController::floorFor(const QString& key) {
	auto iter = this->floors.constFind(key);
	if (iter != this->floors.constEnd()) return *iter;

	const auto lifetime = this->lifetimeOf(key);
	auto kelvin = std::exchange(this->mKelvin, 0);
	auto floor = 0.0;
	// A retry belongs to the lifetime it started in. If the id is retired during
	// a write (the monitor is removed and comes back under the same id), the next
	// write would land on the new screen: stop before it instead.
	while (floor < 1.0 && this->lifetimeOf(key) == lifetime && !this->apply(key, floor))
		floor += BRIGHTNESS_STEP;
	this->mKelvin = kelvin;

	floor = std::min(floor, 1.0);
	// Retired during the probe: nothing was learned about a screen that is gone.
	if (this->lifetimeOf(key) != lifetime) return 1.0;
	qCInfo(logGamma) << "Software brightness of" << qs::windows::logId(key) << "goes down to"
	                 << floor;
	// No successful probe means a transient native failure, not a known floor.
	if (floor < 1.0) this->floors.insert(key, floor);
	return floor;
}

bool GammaController::reapply(const QString& key) {
	auto ok = this->reapplyFactors(key);
	// A snapshot is only kept while restoration is owed. Otherwise another
	// application may change the ramp before the next attempt, which must then
	// save that ramp rather than restore a stale one over it.
	if (!this->written.contains(key)) this->originals.remove(key);
	return ok;
}

bool GammaController::reapplyFactors(const QString& key) {
	const auto lifetime = this->lifetimeOf(key);
	auto backlight = this->backlightFactor.value(key, 1.0);
	auto dim = this->dimFactor.value(key, 1.0);
	auto combined = std::clamp(backlight * dim, 0.0, 1.0);

	// Refuse the entire operation before probing or writing if its original
	// ramp cannot be saved. apply() also enforces this for direct floor probes.
	if ((combined < 1.0 || this->mKelvin > 0) && !this->originals.contains(key)) {
		auto* hdc = this->screenDc(key);
		if (hdc == nullptr) return false;
		auto saved = this->saveOriginal(key, hdc);
		this->api.deleteDc(hdc);
		if (!saved) return false;
	}

	auto floor = combined < 1.0 ? this->floorFor(key) : 1.0;
	if (this->lifetimeOf(key) != lifetime) return false;
	if (combined < 1.0 && floor >= 1.0) return false;
	auto wanted = floor + combined * (1.0 - floor);
	auto applied = wanted;

	auto ok = this->apply(key, applied);
	// Same rule as the floor probe: no retry once the id has been retired.
	while (!ok && applied < 1.0 && this->lifetimeOf(key) == lifetime) {
		applied = std::min(1.0, applied + BRIGHTNESS_STEP);
		ok = this->apply(key, applied);
	}

	if (applied > wanted) {
		qCDebug(logGamma) << "Windows refused dimming" << qs::windows::logId(key) << "to" << wanted
		                  << "; went to" << applied;
	}
	// A retired id has nothing left to apply or restore; never report success for it.
	return ok && this->lifetimeOf(key) == lifetime;
}

bool GammaController::apply(const QString& key, qreal brightness) {
	auto neutral = this->mKelvin <= 0 && brightness >= 1.0;
	if (neutral) return this->restore(key);

	const auto lifetime = this->lifetimeOf(key);
	auto* hdc = this->screenDc(key);
	if (hdc == nullptr) return false;

	auto ok = false;

	if (this->saveOriginal(key, hdc)) {
		double factors[3] = {1.0, 1.0, 1.0}; // NOLINT
		if (this->mKelvin > 0) factorsForTemperature(this->mKelvin, factors);

		Ramp ramp {};
		for (auto channel = 0; channel < 3; channel++) {
			for (auto i = 0; i < 256; i++) {
				auto value = static_cast<double>(i) * 257.0 * factors[channel] * brightness;
				ramp.values[channel][i] = static_cast<WORD>(std::clamp(value, 0.0, 65535.0));
			}
		}

		// A copy: a retire during the native call drops the stored snapshot.
		auto saved = this->originals.value(key);
		ok = this->api.setRamp(hdc, ramp.values) != FALSE;
		if (this->lifetimeOf(key) != lifetime) {
			// The id was retired while the write ran, so nothing owns it any more.
			// Put the saved ramp back rather than leave the screen dimmed with no
			// obligation left to undo it.
			if (ok) this->api.setRamp(hdc, saved.values);
			ok = false;
		} else if (ok) {
			this->written.insert(key);
			this->updateCrashSnapshot(key, saved);
		}
		if (!ok)
			qCDebug(logGamma) << "SetDeviceGammaRamp refused for" << qs::windows::logId(key)
			                  << brightness;
	}

	this->api.deleteDc(hdc);
	return ok;
}

QString GammaController::deviceFor(const QString& key) const {
	auto device = key;
	if (this->identities != nullptr) {
		if (auto* screen = this->identities->screenById(key)) {
			auto identity = this->identities->identity(screen);
			if (!identity.deviceName.isEmpty()) device = identity.deviceName;
		} else if (key.startsWith(QStringLiteral("path:")) || key.startsWith(QStringLiteral("target:"))
		           || key.startsWith(QStringLiteral("source:"))
		           || key.startsWith(QStringLiteral("session:"))
		           || key.startsWith(QStringLiteral("unresolved:")))
		{
			// The target is gone. Never hand an opaque identity to GDI.
			return {};
		}
	}
	return device;
}

HDC GammaController::screenDc(const QString& key) const {
	auto device = this->deviceFor(key);
	if (device.isEmpty()) return nullptr;
	// GDI needs the current DISPLAY source, not its friendly monitor label.
	auto name = device.toStdWString();
	return this->api.createDc(L"DISPLAY", name.c_str(), nullptr, nullptr);
}

bool GammaController::saveOriginal(const QString& key, HDC hdc) {
	if (this->originals.contains(key)) return true;
	const auto lifetime = this->lifetimeOf(key);
	Ramp original {};
	if (!this->api.getRamp(hdc, original.values)) return false;
	// Retired while reading: this ramp belongs to a state that no longer exists.
	// Recording it would leave a snapshot with no owner.
	if (this->lifetimeOf(key) != lifetime) return false;
	this->originals.insert(key, original);
	return true;
}

bool GammaController::restore(const QString& key) {
	if (!this->written.contains(key)) return true;
	auto saved = this->originals.constFind(key);
	if (saved == this->originals.constEnd()) {
		// An obligation without its snapshot cannot be honoured, and writing a
		// blank ramp instead would black the display out.
		qCWarning(logGamma) << "Dropping a gamma restoration that has no saved ramp";
		this->written.remove(key);
		return true;
	}
	auto* hdc = this->screenDc(key);
	if (hdc == nullptr) return false;
	// A copy: a retire during the native call erases the stored entry.
	auto ramp = saved.value();
	auto ok = this->api.setRamp(hdc, ramp.values) != FALSE;
	this->api.deleteDc(hdc);
	if (ok) {
		this->written.remove(key);
		this->originals.remove(key);
		this->removeCrashSnapshot(key);
	}
	return ok;
}

void GammaController::retire(const QString& id) {
	// The id still resolves to the live screen: put its original ramp back before
	// anything is dropped. If that fails the screen is gone or refuses, and
	// there is nothing more to hold on to.
	if (!this->restore(id))
		qCWarning(logGamma) << "Could not restore the gamma ramp of a retired screen";
	++this->lifetimes[id];
	this->backlightFactor.remove(id);
	this->dimFactor.remove(id);
	this->floors.remove(id);
	this->originals.remove(id);
	this->written.remove(id);
	this->removeCrashSnapshot(id);
}

quint64 GammaController::lifetimeOf(const QString& key) const {
	return this->lifetimes.value(key, 0);
}

void GammaController::restoreAll() {
	// Iterate a copy: successful restores release ownership; failures retain it.
	auto screens = this->written;
	for (const auto& screen: screens) this->restore(screen);
	this->mKelvin = 0;
	this->backlightFactor.removeIf([this](auto entry) {
		return !this->written.contains(entry.key());
	});
	this->dimFactor.removeIf([this](auto entry) { return !this->written.contains(entry.key()); });
}

} // namespace qs::windows::sys
