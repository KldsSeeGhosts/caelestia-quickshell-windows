#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qt_windows.h>

namespace qs::windows {
class ScreenIdentityRegistry;
}

namespace qs::windows::sys {

class GammaController: public QObject {
	Q_OBJECT;

public:
	static GammaController* instance();

	// Inject only the native calls; fixtures exercise the same ownership state.
	struct NativeApi {
		decltype(&CreateDCW) createDc;
		decltype(&DeleteDC) deleteDc;
		decltype(&GetDeviceGammaRamp) getRamp;
		decltype(&SetDeviceGammaRamp) setRamp;
	};
	explicit GammaController(NativeApi api, QObject* parent = nullptr);
	// Test-only injection of the same identity registry production uses.
	explicit GammaController(
	    NativeApi api,
	    qs::windows::ScreenIdentityRegistry& identities,
	    QObject* parent = nullptr
	);

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
	qreal floorFor(const QString& key);
	void watchIdentities();
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
	// Bumped each time an id is retired; a native call that spans a bump aborts.
	QHash<QString, quint64> lifetimes;
};

} // namespace qs::windows::sys
