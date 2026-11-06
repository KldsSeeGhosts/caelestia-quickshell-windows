#include "gamma.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qscreen.h>
#include <qstring.h>

#include "../../core/logcat.hpp"
#include "../redact.hpp"
#include "../screen_identity.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logGamma, "quickshell.windows.gamma", QtInfoMsg);

constexpr qreal BRIGHTNESS_STEP = 0.05;

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

GammaController* GammaController::instance() {
	static QPointer<GammaController> controller; // NOLINT

	if (controller.isNull()) {
		controller = new GammaController(QCoreApplication::instance());
	}

	return controller.data();
}

GammaController::GammaController(QObject* parent)
    : GammaController({&CreateDCW, &DeleteDC, &GetDeviceGammaRamp, &SetDeviceGammaRamp}, parent) {}

GammaController::GammaController(NativeApi api, QObject* parent)
    : QObject(parent)
    , api(api)
    , identities(
          qobject_cast<QGuiApplication*>(QCoreApplication::instance())
              ? ScreenIdentityRegistry::instance()
              : nullptr
      ) {
	QObject::connect(
	    QCoreApplication::instance(),
	    &QCoreApplication::aboutToQuit,
	    this,
	    &GammaController::restoreAll
	);
	if (this->identities != nullptr) this->watchIdentities();
}

GammaController::GammaController(
    NativeApi api,
    qs::windows::ScreenIdentityRegistry& identities,
    QObject* parent
)
    : QObject(parent)
    , api(api)
    , identities(&identities) {
	QObject::connect(
	    QCoreApplication::instance(),
	    &QCoreApplication::aboutToQuit,
	    this,
	    &GammaController::restoreAll
	);
	this->watchIdentities();
}

void GammaController::watchIdentities() {
	QObject::connect(
	    this->identities,
	    &ScreenIdentityRegistry::identityRetired,
	    this,
	    [this](const QString& id, QScreen*) { this->retire(id); }
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
		}
		if (!ok)
			qCDebug(logGamma) << "SetDeviceGammaRamp refused for" << qs::windows::logId(key)
			                  << brightness;
	}

	this->api.deleteDc(hdc);
	return ok;
}

HDC GammaController::screenDc(const QString& key) const {
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
			return nullptr;
		}
	}
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
