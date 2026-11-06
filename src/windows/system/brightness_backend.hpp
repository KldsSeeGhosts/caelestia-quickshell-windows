#pragma once

// Injectable seam between the Brightness controller and the native WMI,
// \\.\LCD, DDC/CI and gamma-ramp facilities it drives. The probe data
// structures are shared with brightness.cpp's decision logic; tests implement
// this interface with fakes and never touch real hardware or the gamma ramp.

#include <functional>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>

#include "brightness.hpp"

namespace qs::windows::sys {

inline qreal sdrWhiteToSlider(UINT32 raw) {
	return std::clamp((double(raw) - 1000.0) / 5000.0, 0.0, 1.0);
}

inline UINT32 sliderToSdrWhite(qreal value) {
	return 1000 + UINT32(std::lround(std::clamp(value, 0.0, 1.0) * 100.0)) * 50;
}

// DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR is 2. Advanced color may instead be
// active in WCG/SDR mode, where SDR white is not a brightness control.
inline bool isHdrColorMode(UINT32 activeMode) { return activeMode == 2; }

enum class PanelKind : quint8 {
	Unknown,
	Internal,
	External,
};

struct MonitorTarget {
	QString instanceId;
	QString friendlyName;
	DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
	LUID adapterId {};
	UINT32 targetId = 0;
	bool colorKnown = false;
	bool advancedColor = false;
	bool hdrActive = false;
	bool whiteKnown = false;
	UINT32 whiteLevel = 0;
};

struct MonitorDescription {
	PanelKind kind = PanelKind::Unknown;
	std::vector<MonitorTarget> targets;
	// Active built-in targets in the whole display topology. The legacy
	// \\.\LCD device is global, so it only provably addresses a screen's panel
	// when that panel is the only built-in one.
	int internalTargets = 0;
};

struct WmiPanel {
	QString instanceName;
	QString methodsPath;
	bool active = true;
	int current = -1;
	int levels = 0;
};

struct LcdPanel {
	bool usable = false;
	DWORD error = ERROR_SUCCESS;
	int current = -1;
	std::vector<UCHAR> levels;
};

struct DdcWriteFailure {
	// 1-based position in the HMONITOR's physical monitor list.
	quint32 target = 0;
	// True when the VCP range query failed, so this target was not written.
	bool range = false;
	DWORD error = ERROR_SUCCESS;
};

struct DdcWriteReport {
	quint32 written = 0;
	std::vector<DdcWriteFailure> failures;

	// Every target was written through its own proven VCP range. A partial
	// result is a failure so it cannot pass for a full write.
	[[nodiscard]] bool ok() const { return this->written > 0 && this->failures.empty(); }
};

// Writes value to every physical target, one documented VCP exchange at a
// time: queryRange(target, maximum, error) must prove a non-zero maximum for
// that exact target before setValue(target, level, error) is called. There is
// no assumed range and no write without one; failures are collected per target.
template <typename Target, typename QueryRange, typename SetValue>
DdcWriteReport ddcWriteTargets(
    qreal value,
    const std::vector<Target>& targets,
    QueryRange queryRange,
    SetValue setValue
) {
	DdcWriteReport report;
	for (auto i = std::size_t(0); i < targets.size(); i++) {
		DWORD maximum = 0;
		DWORD error = ERROR_SUCCESS;
		if (!queryRange(targets[i], maximum, error) || maximum == 0) {
			report.failures.push_back(
			    {static_cast<quint32>(i) + 1,
			     true,
			     error == ERROR_SUCCESS ? DWORD(ERROR_INVALID_DATA) : error}
			);
			continue;
		}

		auto level =
		    static_cast<DWORD>(std::lround(std::clamp(value, 0.0, 1.0) * qreal(maximum)));
		if (!setValue(targets[i], level, error)) {
			report.failures.push_back(
			    {static_cast<quint32>(i) + 1,
			     false,
			     error == ERROR_SUCCESS ? DWORD(ERROR_GEN_FAILURE) : error}
			);
			continue;
		}

		report.written++;
	}

	return report;
}

struct Probe {
	MonitorDescription monitor;
	bool wmiAsked = false;
	HRESULT wmiResult = S_OK;
	std::vector<WmiPanel> wmi;
	bool lcdAsked = false;
	LcdPanel lcd;
	bool ddcAsked = false;
	bool ddcOk = false;
	qreal ddcBrightness = 1.0;
	DWORD ddcError = ERROR_SUCCESS;
};

class BrightnessBackend {
public:
	virtual ~BrightnessBackend() = default;
	Q_DISABLE_COPY_MOVE(BrightnessBackend);

	// Worker thread. Reads monitor description, WMI panels, \\.\LCD and (when
	// readDdc) DDC/CI as appropriate; never writes anything.
	virtual Probe probeScreen(HMONITOR monitor, bool readDdc) = 0;

	// Worker thread. Writes brightness through exactly the route given; a
	// software route never reaches this call (gamma goes through
	// gammaSetBrightness). Returns false and fills error on failure.
	virtual bool
	writeBrightness(const BrightnessRoute& route, HMONITOR monitor, qreal value, QString& error) = 0;

	// Worker thread. Reads the current level through the route given; a
	// negative result means failure and fills error. SDR white returns raw units;
	// the other routes return percent.
	virtual int readBrightness(const BrightnessRoute& route, QString& error) = 0;

	// Main thread. Software gamma (dimming) state and writes. gammaDims reports
	// dimming retained by this process, independently of controller routes/lifetime.
	[[nodiscard]] virtual bool gammaDims(const QString& screenName) const = 0;
	virtual bool gammaSetBrightness(const QString& screenName, qreal value) = 0;

	// Main thread. Spawns the process-wide WmiMonitorBrightnessEvent watch
	// thread delivering (WMI instance name, percent); the controller marshals
	// the callback to the main thread.
	virtual void startEventWatch(const std::function<void(const QString&, int)>& deliver) = 0;

protected:
	BrightnessBackend() = default;
};

} // namespace qs::windows::sys
