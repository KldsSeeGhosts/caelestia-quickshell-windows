#pragma once

#include <qlist.h>
#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qt_windows.h>

#include <powersetting.h>

class QTimer;

namespace qs::service::upower {

class PowerProfile: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		PowerSaver = 0,
		Balanced = 1,
		Performance = 2,
	};
	Q_ENUM(Enum);

	Q_INVOKABLE static QString toString(qs::service::upower::PowerProfile::Enum profile);
};

class PerformanceDegradationReason: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		None = 0,
		LapDetected = 1,
		HighTemperature = 2,
	};
	Q_ENUM(Enum);

	// clang-format off
	Q_INVOKABLE static QString toString(qs::service::upower::PerformanceDegradationReason::Enum reason);
	// clang-format on
};

class PowerProfileHold {
	Q_GADGET;
	QML_VALUE_TYPE(powerProfileHold);
	Q_PROPERTY(qs::service::upower::PowerProfile::Enum profile MEMBER profile CONSTANT);
	Q_PROPERTY(QString applicationId MEMBER applicationId CONSTANT);
	Q_PROPERTY(QString reason MEMBER reason CONSTANT);

public:
	explicit PowerProfileHold() = default;

	PowerProfile::Enum profile = PowerProfile::Balanced;
	QString applicationId;
	QString reason;

	[[nodiscard]] bool operator==(const PowerProfileHold& other) const;
};

// Injectable powrprof seam. The default instance() resolves the overlay-scheme
// and effective-power-mode exports from powrprof.dll; tests substitute fake
// function pointers so no real power API is ever called.
struct PowerProfilesApi {
	DWORD (WINAPI* getEffectiveOverlayScheme)(GUID* overlayScheme) = nullptr;
	DWORD (WINAPI* setEffectiveOverlayScheme)(GUID overlayScheme) = nullptr;
	HRESULT (WINAPI* registerEffectivePowerModeNotifications)(
	    unsigned long version,
	    EFFECTIVE_POWER_MODE_CALLBACK* callback,
	    void* context,
	    void** registrationHandle
	) = nullptr;
	HRESULT (WINAPI* unregisterEffectivePowerModeNotifications)(void* registrationHandle) = nullptr;

	[[nodiscard]] static const PowerProfilesApi& instance();
};

class PowerProfilesQml: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(PowerProfiles);
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(qs::service::upower::PowerProfile::Enum profile READ default WRITE setProfile NOTIFY profileChanged BINDABLE bindableProfile);
	Q_PROPERTY(bool hasPerformanceProfile READ default NOTIFY hasPerformanceProfileChanged BINDABLE bindableHasPerformanceProfile);
	Q_PROPERTY(qs::service::upower::PerformanceDegradationReason::Enum degradationReason READ default NOTIFY degradationReasonChanged BINDABLE bindableDegradationReason);
	Q_PROPERTY(QList<qs::service::upower::PowerProfileHold> holds READ default NOTIFY holdsChanged BINDABLE bindableHolds);
	Q_PROPERTY(bool profileKnown READ default NOTIFY profileKnownChanged BINDABLE bindableProfileKnown);
	Q_PROPERTY(QString rawOverlayGuid READ default NOTIFY rawOverlayGuidChanged BINDABLE bindableRawOverlayGuid);
	Q_PROPERTY(QString lastError READ default NOTIFY lastErrorChanged BINDABLE bindableLastError);
	Q_PROPERTY(bool writeSupported READ default NOTIFY writeSupportedChanged BINDABLE bindableWriteSupported);
	Q_PROPERTY(bool observing READ default NOTIFY observingChanged BINDABLE bindableObserving);
	// clang-format on

public:
	// The api and fallbackRefreshIntervalMs parameters exist for tests; QML
	// keeps the default construction and a 10 second fallback poll.
	explicit PowerProfilesQml(
	    QObject* parent = nullptr,
	    const PowerProfilesApi* api = nullptr,
	    int fallbackRefreshIntervalMs = 10000
	);
	~PowerProfilesQml() override;
	Q_DISABLE_COPY_MOVE(PowerProfilesQml);

	void setProfile(PowerProfile::Enum profile);

	[[nodiscard]] QBindable<PowerProfile::Enum> bindableProfile() const { return &this->bProfile; }

	[[nodiscard]] QBindable<bool> bindableHasPerformanceProfile() const {
		return &this->bHasPerformanceProfile;
	}

	[[nodiscard]] QBindable<PerformanceDegradationReason::Enum> bindableDegradationReason() const {
		return &this->bDegradationReason;
	}

	[[nodiscard]] QBindable<QList<PowerProfileHold>> bindableHolds() const { return &this->bHolds; }

	// True only after a successful read that mapped to a known overlay GUID.
	// The profile property keeps its legacy behavior for compatibility: a
	// failed read retains its previous (zero-initialized PowerSaver) value and
	// an unknown GUID still surfaces as Balanced. profileKnown,
	// rawOverlayGuid and lastError distinguish those cases from real reads.
	[[nodiscard]] QBindable<bool> bindableProfileKnown() const { return &this->bProfileKnown; }

	[[nodiscard]] QBindable<QString> bindableRawOverlayGuid() const { return &this->bRawOverlayGuid; }

	[[nodiscard]] QBindable<QString> bindableLastError() const { return &this->bLastError; }

	// True when the overlay set API resolved, i.e. writes can be submitted.
	[[nodiscard]] QBindable<bool> bindableWriteSupported() const { return &this->bWriteSupported; }

	// True only while effective-power-mode notification registration
	// succeeded; a failed registration falls back to a bounded periodic
	// refresh instead.
	[[nodiscard]] QBindable<bool> bindableObserving() const { return &this->bObserving; }

	void refresh();

signals:
	void profileChanged();
	void hasPerformanceProfileChanged();
	void degradationReasonChanged();
	void holdsChanged();
	void profileKnownChanged();
	void rawOverlayGuidChanged();
	void lastErrorChanged();
	void writeSupportedChanged();
	void observingChanged();
	// Reported for every setProfile() request. ok is true only when the native
	// call succeeded and a re-read of the effective overlay mapped to the
	// requested profile.
	void profileWriteResult(int requestedProfile, bool ok, QString error);

private:
	void publish(PowerProfile::Enum profile, bool known, const QString& rawGuid, const QString& error);

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, PowerProfile::Enum, bProfile, &PowerProfilesQml::profileChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, bool, bHasPerformanceProfile, &PowerProfilesQml::hasPerformanceProfileChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, PerformanceDegradationReason::Enum, bDegradationReason, &PowerProfilesQml::degradationReasonChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, QList<PowerProfileHold>, bHolds, &PowerProfilesQml::holdsChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, bool, bProfileKnown, &PowerProfilesQml::profileKnownChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, QString, bRawOverlayGuid, &PowerProfilesQml::rawOverlayGuidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, QString, bLastError, &PowerProfilesQml::lastErrorChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, bool, bWriteSupported, &PowerProfilesQml::writeSupportedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(PowerProfilesQml, bool, bObserving, &PowerProfilesQml::observingChanged);
	// clang-format on

	const PowerProfilesApi* api = nullptr;
	QTimer* fallbackRefreshTimer = nullptr;

	HPOWERNOTIFY notificationHandle = nullptr;
	void* effectiveModeHandle = nullptr;
};

} // namespace qs::service::upower
