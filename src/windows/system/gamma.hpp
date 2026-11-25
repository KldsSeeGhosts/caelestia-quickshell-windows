#pragma once

#include <memory>

#include <qabstractnativeeventfilter.h>
#include <qhash.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtimer.h>

namespace qs::windows {
class ScreenIdentityRegistry;
}

namespace qs::windows::sys {

namespace gamma_detail {

[[nodiscard]] bool parseCrashTargetIdentity(const wchar_t* identity, LUID& adapter, UINT32& target);

} // namespace gamma_detail

class GammaController
    : public QObject
    , public QAbstractNativeEventFilter {
	Q_OBJECT;

public:
	static GammaController* instance();

	// Inject only the native calls; fixtures exercise the same ownership state.
	struct NativeApi {
		decltype(&CreateDCW) createDc;
		decltype(&DeleteDC) deleteDc;
		decltype(&GetDeviceGammaRamp) getRamp;
		decltype(&SetDeviceGammaRamp) setRamp;
		// Crash-time identity resolution. It maps a saved stable target identity
		// to the current GDI device, or fails when the target cannot be verified.
		using ResolveDevice = BOOL(WINAPI*)(const wchar_t*, wchar_t*, DWORD);
		ResolveDevice resolveDevice = nullptr;
	};
	explicit GammaController(NativeApi api, QObject* parent = nullptr);
	// Test-only injection of the same identity registry production uses.
	explicit GammaController(
	    NativeApi api,
	    qs::windows::ScreenIdentityRegistry& identities,
	    QObject* parent = nullptr
	);
	~GammaController() override;

	bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;
	// Reapplies every ramp this process still owns. Native display/power events call
	// this through a debounce; tests may call it directly.
	void reapplyActive();
	// Crash/exit-only restore over plain static snapshots. It intentionally uses no Qt.
	static void restoreForCrash();
	// Internal seam for the snapshot fixture: same scan and sequence validation as
	// restoreForCrash, with the native entry points supplied by the fixture.
	static void restoreSnapshots(const NativeApi& api);
	static quintptr crashSnapshotCount();
	static void clearCrashSnapshots();

	void setTemperature(int kelvin);
	void setBrightness(const QString& screenName, qreal brightness);
	// Same application path as setBrightness, with the final native apply result.
	bool trySetBrightness(const QString& screenName, qreal brightness);
	void setDim(const QString& screenName, qreal factor);
	qreal probeDimFloor(const QString& screenName);
	[[nodiscard]] bool dims(const QString& screenName) const;

	void restoreAll();

private:
	explicit GammaController(QObject* parent);
	void init();

	struct Ramp {
		WORD values[3][256]; // NOLINT
	};

	// Public methods take the unique QML name for upstream compatibility.
	// Every factor, floor and saved ramp is keyed by the opaque stable target id.
	[[nodiscard]] QString keyFor(const QString& screenName) const;
	bool reapply(const QString& key);
	bool reapplyFactors(const QString& key);
	bool apply(const QString& key, qreal brightness);
	bool saveOriginal(const QString& key, HDC hdc);
	bool restore(const QString& key);
	HDC screenDc(const QString& key) const;
	[[nodiscard]] QString deviceFor(const QString& key) const;
	qreal floorFor(const QString& key);
	void watchIdentities();
	void scheduleReapply();
	void updateCrashSnapshot(const QString& key, const Ramp& ramp);
	void removeCrashSnapshot(const QString& key);
	// An id stops naming its screen (removal, or another identity). Puts the
	// original ramp back while the id still resolves, then drops all of its state
	// and invalidates native calls still running for it.
	void retire(const QString& id);
	[[nodiscard]] quint64 lifetimeOf(const QString& key) const;

	NativeApi api;
	qs::windows::ScreenIdentityRegistry* identities = nullptr;
	int mKelvin = 0;
	QHash<QString, qreal> backlightFactor;
	QHash<QString, qreal> dimFactor;
	QHash<QString, qreal> floors;
	QHash<QString, Ramp> originals;
	// A saved snapshot alone creates no restoration obligation.
	QSet<QString> written;
	// Keys with an outstanding user-requested dimming factor. Unlike `written`,
	// this survives a transient write failure so the next display wake retries it.
	QSet<QString> dimmingRequested;
	// The requested backlight factor while `backlightFactor` is rolled back to
	// the last owned state after a failed write.
	QHash<QString, qreal> retryBacklightFactor;
	// Bumped each time an id is retired; a native call that spans a bump aborts.
	QHash<QString, quint64> lifetimes;
	QTimer reapplyTimer;
	struct PowerNotifications;
	std::unique_ptr<PowerNotifications> powerNotifications;
};

} // namespace qs::windows::sys
