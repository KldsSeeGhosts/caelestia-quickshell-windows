#include "powerprofiles.hpp"

#include <qcontainerfwd.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtimer.h>

#include <powersetting.h>

#include "../../../core/logcat.hpp"

namespace qs::service::upower {

namespace {
QS_LOGGING_CATEGORY(logPowerProfiles, "quickshell.service.powerprofiles", QtWarningMsg);

const GUID GUID_OVERLAY_BEST_EFFICIENCY =
    {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
const GUID GUID_OVERLAY_BEST_PERFORMANCE =
    {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
const GUID GUID_OVERLAY_BALANCED = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

// Maps a known overlay GUID to its profile and reports whether the GUID was
// recognized. Unknown GUIDs yield the legacy Balanced mapping used by the
// profile property; callers detect them through the false result.
bool overlayGuidToProfile(const GUID& guid, PowerProfile::Enum& profile) {
	if (IsEqualGUID(guid, GUID_OVERLAY_BEST_EFFICIENCY)) {
		profile = PowerProfile::PowerSaver;
		return true;
	}
	if (IsEqualGUID(guid, GUID_OVERLAY_BEST_PERFORMANCE)) {
		profile = PowerProfile::Performance;
		return true;
	}
	if (IsEqualGUID(guid, GUID_OVERLAY_BALANCED)) {
		profile = PowerProfile::Balanced;
		return true;
	}
	profile = PowerProfile::Balanced;
	return false;
}

GUID overlayGuidFromProfile(PowerProfile::Enum profile) {
	switch (profile) {
	case PowerProfile::PowerSaver: return GUID_OVERLAY_BEST_EFFICIENCY;
	case PowerProfile::Performance: return GUID_OVERLAY_BEST_PERFORMANCE;
	default: return GUID_OVERLAY_BALANCED;
	}
}

QString overlayGuidToString(const GUID& guid) {
	return QString::asprintf(
	    "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
	    guid.Data1,
	    guid.Data2,
	    guid.Data3,
	    guid.Data4[0],
	    guid.Data4[1],
	    guid.Data4[2],
	    guid.Data4[3],
	    guid.Data4[4],
	    guid.Data4[5],
	    guid.Data4[6],
	    guid.Data4[7]
	);
}

void CALLBACK effectiveModeChanged(EFFECTIVE_POWER_MODE /*mode*/, void* context) {
	auto* self = static_cast<PowerProfilesQml*>(context);
	QMetaObject::invokeMethod(self, &PowerProfilesQml::refresh, Qt::QueuedConnection);
}

} // namespace

const PowerProfilesApi& PowerProfilesApi::instance() {
	static const auto api = []() {
		PowerProfilesApi result;
		if (auto* module = LoadLibraryW(L"powrprof.dll")) {
			result.getEffectiveOverlayScheme = reinterpret_cast<DWORD (WINAPI*)(GUID*)>( // NOLINT
			    GetProcAddress(module, "PowerGetEffectiveOverlayScheme")
			);
			result.setEffectiveOverlayScheme = reinterpret_cast<DWORD (WINAPI*)(GUID)>( // NOLINT
			    GetProcAddress(module, "PowerSetActiveOverlayScheme")
			);
			result.registerEffectivePowerModeNotifications =
			    reinterpret_cast<decltype(result.registerEffectivePowerModeNotifications)>( // NOLINT
			        GetProcAddress(module, "PowerRegisterForEffectivePowerModeNotifications")
			    );
			result.unregisterEffectivePowerModeNotifications =
			    reinterpret_cast<decltype(result.unregisterEffectivePowerModeNotifications)>( // NOLINT
			        GetProcAddress(module, "PowerUnregisterFromEffectivePowerModeNotifications")
			    );
		}
		return result;
	}();
	return api;
}

QString PowerProfile::toString(PowerProfile::Enum profile) {
	switch (profile) {
	case PowerProfile::PowerSaver: return QStringLiteral("PowerSaver");
	case PowerProfile::Balanced: return QStringLiteral("Balanced");
	case PowerProfile::Performance: return QStringLiteral("Performance");
	default: return QStringLiteral("Invalid");
	}
}

QString PerformanceDegradationReason::toString(PerformanceDegradationReason::Enum reason) {
	switch (reason) {
	case PerformanceDegradationReason::LapDetected: return QStringLiteral("LapDetected");
	case PerformanceDegradationReason::HighTemperature: return QStringLiteral("HighTemperature");
	default: return QStringLiteral("None");
	}
}

bool PowerProfileHold::operator==(const PowerProfileHold& other) const {
	return other.profile == this->profile && other.applicationId == this->applicationId
	    && other.reason == this->reason;
}

PowerProfilesQml::PowerProfilesQml(
    QObject* parent,
    const PowerProfilesApi* api,
    int fallbackRefreshIntervalMs
):
    QObject(parent) {
	this->api = api != nullptr ? api : &PowerProfilesApi::instance();

	// The overlay set API is what proves the platform exposes the performance
	// overlay; availability is derived from it instead of being hard-coded.
	auto overlaySetSupported = this->api->setEffectiveOverlayScheme != nullptr;
	this->bHasPerformanceProfile = overlaySetSupported;
	this->bWriteSupported = overlaySetSupported;
	this->bDegradationReason = PerformanceDegradationReason::None;
	this->bHolds = QList<PowerProfileHold> {};

	this->refresh();

	if (this->api->registerEffectivePowerModeNotifications != nullptr) {
		auto status = this->api->registerEffectivePowerModeNotifications(
		    EFFECTIVE_POWER_MODE_V1,
		    &effectiveModeChanged,
		    this,
		    &this->effectiveModeHandle
		);

		if (status == ERROR_SUCCESS) {
			this->bObserving = true;
		} else {
			qCWarning(logPowerProfiles)
			    << "PowerRegisterForEffectivePowerModeNotifications failed:" << status;
			this->effectiveModeHandle = nullptr;
		}
	} else {
		qCWarning(logPowerProfiles)
		    << "PowerRegisterForEffectivePowerModeNotifications is not available";
	}

	if (!this->bObserving.value()) {
		// Bounded fallback: without notifications, poll the effective overlay
		// so a missed transition cannot leave the observed profile stale
		// forever.
		this->fallbackRefreshTimer = new QTimer(this);
		this->fallbackRefreshTimer->setSingleShot(false);
		this->fallbackRefreshTimer->setInterval(fallbackRefreshIntervalMs);
		QObject::connect(
		    this->fallbackRefreshTimer,
		    &QTimer::timeout,
		    this,
		    &PowerProfilesQml::refresh
		);
		this->fallbackRefreshTimer->start();
	}
}

PowerProfilesQml::~PowerProfilesQml() {
	if (this->effectiveModeHandle != nullptr) {
		if (this->api->unregisterEffectivePowerModeNotifications != nullptr) {
			this->api->unregisterEffectivePowerModeNotifications(this->effectiveModeHandle);
		}
		this->effectiveModeHandle = nullptr;
	}
}

void PowerProfilesQml::refresh() {
	if (this->api->getEffectiveOverlayScheme == nullptr) {
		qCWarning(logPowerProfiles) << "PowerGetEffectiveOverlayScheme is not available";
		this->publish(
		    this->bProfile.value(),
		    false,
		    QString(),
		    QStringLiteral("PowerGetEffectiveOverlayScheme is not available")
		);
		return;
	}

	GUID activeGuid {};
	auto status = this->api->getEffectiveOverlayScheme(&activeGuid);

	if (status != ERROR_SUCCESS) {
		qCWarning(logPowerProfiles) << "PowerGetEffectiveOverlayScheme failed:" << status;
		this->publish(
		    this->bProfile.value(),
		    false,
		    QString(),
		    QStringLiteral("PowerGetEffectiveOverlayScheme failed: %1").arg(static_cast<qulonglong>(status))
		);
		return;
	}

	PowerProfile::Enum profile = PowerProfile::Balanced;
	auto known = overlayGuidToProfile(activeGuid, profile);
	this->publish(profile, known, overlayGuidToString(activeGuid), QString());
}

// Publishes one observation as a consistent snapshot: every value is stored
// before any change notification, so no handler sees a new profile paired with
// stale validity, GUID or error (or the reverse).
void PowerProfilesQml::publish(
    PowerProfile::Enum profile,
    bool known,
    const QString& rawGuid,
    const QString& error
) {
	const bool profileChanged = this->bProfile.value() != profile;
	const bool knownChanged = this->bProfileKnown.value() != known;
	const bool guidChanged = this->bRawOverlayGuid.value() != rawGuid;
	const bool errorChanged = this->bLastError.value() != error;

	this->bProfile.setValueBypassingBindings(profile);
	this->bProfileKnown.setValueBypassingBindings(known);
	this->bRawOverlayGuid.setValueBypassingBindings(rawGuid);
	this->bLastError.setValueBypassingBindings(error);

	if (profileChanged) this->bProfile.notify();
	if (knownChanged) this->bProfileKnown.notify();
	if (guidChanged) this->bRawOverlayGuid.notify();
	if (errorChanged) this->bLastError.notify();
}

void PowerProfilesQml::setProfile(PowerProfile::Enum profile) {
	if (profile < PowerProfile::PowerSaver || profile > PowerProfile::Performance) {
		qCWarning(logPowerProfiles) << "Tried to request invalid power profile" << profile;
		emit this->profileWriteResult(
		    static_cast<int>(profile),
		    false,
		    QStringLiteral("Invalid power profile requested")
		);
		return;
	}

	if (this->api->setEffectiveOverlayScheme == nullptr) {
		qCWarning(logPowerProfiles) << "PowerSetActiveOverlayScheme is not available";
		emit this->profileWriteResult(
		    static_cast<int>(profile),
		    false,
		    QStringLiteral("PowerSetActiveOverlayScheme is not available")
		);
		return;
	}

	auto guid = overlayGuidFromProfile(profile);
	auto status = this->api->setEffectiveOverlayScheme(guid);

	if (status != ERROR_SUCCESS) {
		qCWarning(logPowerProfiles) << "PowerSetActiveOverlayScheme failed:" << status;
		emit this->profileWriteResult(
		    static_cast<int>(profile),
		    false,
		    QStringLiteral("PowerSetActiveOverlayScheme failed: %1")
		        .arg(static_cast<qulonglong>(status))
		);
		return;
	}

	// Verify from the effective overlay instead of trusting the submitted
	// enum: ok only when the re-read maps to the requested profile.
	this->refresh();

	if (!this->bProfileKnown.value() || this->bProfile.value() != profile) {
		emit this->profileWriteResult(
		    static_cast<int>(profile),
		    false,
		    this->bLastError.value().isEmpty()
		        ? QStringLiteral("Effective overlay does not match the requested profile")
		        : this->bLastError.value()
		);
		return;
	}

	emit this->profileWriteResult(static_cast<int>(profile), true, QString());
}

} // namespace qs::service::upower
