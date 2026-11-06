#pragma once

#include <functional>
#include <optional>

#include <qlist.h>
#include <qpoint.h>
#include <qobject.h>
#include <qrect.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "window_selectors.hpp"

namespace qs::windows::tiling {

// Window facts the tiler needs to decide whether it may manage a window.
// Kept free of Win32 types so the policy can be tested without a desktop.
struct WindowFacts {
	QString appId;
	QString exePath;
	QString title;
	QString className;
	QString screenName;
	bool ownProcess = false;
	bool elevated = false;
	bool owned = false;
	bool toolWindow = false;
	bool topmost = false;
	bool noActivate = false;
	bool dialogFrame = false;
	bool hasThickFrame = true;
};

enum class FloatReason : quint8 {
	None,
	OwnProcess,
	Elevated,
	Owned,
	ExtendedStyle,
	NoThickFrame,
	Dialog,
	Excluded,
	NotIncluded,
};

[[nodiscard]] const char* floatReasonName(FloatReason reason);

// `excluded` and `included` share one wildcard dialect: title:, exe:, class:
// and screen: select a single fact; unprefixed patterns match app id,
// executable name/basename and class. Exclusions always win; a configured
// allowlist makes every non-matching window float. Invalid exclusions are
// dropped, but an allowlist fails closed: if the user wrote entries and none
// compiled, hasIncluded() stays true and nothing matches, so a typo can never
// turn the scoped tiler into an unrestricted one.
class ScopeRules {
public:
	void setExcluded(const QStringList& excluded);
	void setIncluded(const QStringList& included);

	[[nodiscard]] QStringList excluded() const { return this->mExcluded; }
	[[nodiscard]] QStringList included() const { return this->mIncluded; }
	[[nodiscard]] bool hasIncluded() const { return this->includedRules.configuredCount() > 0; }
	// Allowlist entries that survived validation; 0 with hasIncluded() means the
	// allowlist is configured but unusable.
	[[nodiscard]] qint32 includedRuleCount() const {
		return static_cast<qint32>(this->includedRules.compiledCount());
	}
	[[nodiscard]] bool hasTitleRules() const;

	[[nodiscard]] bool matchesExcluded(const WindowFacts& window) const;
	[[nodiscard]] bool matchesIncluded(const WindowFacts& window) const;
	[[nodiscard]] FloatReason floatReason(const WindowFacts& window) const;

private:
	QStringList mExcluded;
	QStringList mIncluded;
	WindowSelectors excludedRules;
	WindowSelectors includedRules;
};

// Why the tiler stopped trying to fit a window. The strings are stable and
// machine readable; QML may show them or map them to text.
enum class FailureReason : quint8 {
	None,
	MinSize,
	OffTarget,
	CorrectionLimit,
	Unresponsive,
};

[[nodiscard]] const char* failureReasonName(FailureReason reason);

enum class PlacementDisposition : quint8 {
	Idle,
	Accepted,
	Landed,
	Denied,
	Failed,
	SizeConstraint,
	OffTarget,
	CorrectionLimit,
	Vanished,
	Superseded,
	Deadline,
};

struct PlacementIdentity {
	quintptr window = 0;
	quint64 lifetime = 0;
	quint64 layoutGeneration = 0;

	bool operator==(const PlacementIdentity&) const = default;
};

// Accepted only acknowledges the asynchronous Win32 request. Landed requires
// a frame observation for this lifetime and layout generation.
struct PlacementOutcome {
	PlacementIdentity identity;
	PlacementDisposition disposition = PlacementDisposition::Idle;
	QRect target;
	QRect frame;
	quint32 nativeError = 0;
};

[[nodiscard]] const char* placementDispositionName(PlacementDisposition disposition);

struct GiveUpInput {
	QRect target;
	QRect frame;
	bool offscreen = false;
	int tolerance = 2;
	// Where a frame larger than the tile is expected to start: the accepted
	// frame's corner once its overflow was moved, otherwise the tile's.
	std::optional<QPoint> anchor;
};

struct GiveUpOutcome {
	// Keep the window tiled at the frame it settled on.
	bool accept = false;
	// Take the window out of the layout and restore its pre-tiling rectangle.
	bool autoFloat = false;
	FailureReason reason = FailureReason::None;
};

// Classifies the give-up path (tiling.cpp giveUp) after the correction budget
// is exhausted. An off-screen window is never a failure. A window moved away
// from its tile landed off target, checked first: a frame larger than the tile
// by whether it still starts at its anchor, a smaller one by its centre. A
// frame larger than the tile that did not move is a minimum-size window and
// stays tiled at its larger size. Anything else means the window was accepted
// with a gap after the correction limit.
[[nodiscard]] GiveUpOutcome classifyGiveUp(const GiveUpInput& input);

// DPI facts that decide how a window's rectangle may be read. `awareness` is a
// Win32 DPI_AWARENESS value (0 unaware, 1 system aware, 2 per monitor aware).
// A window that is not per-monitor aware is drawn through DPI virtualization:
// its own coordinate space is a scaled 96-DPI grid while the DWM frame bounds
// and the tiler's tiles stay in physical pixels. Placement code reads and
// writes such a window from a per-monitor thread so both sides agree.
[[nodiscard]] bool dpiVirtualized(quint32 awareness);

// Whether `frame` counts as sitting on `target`. A virtualized window is
// rounded onto its scaled pixel grid, so the compositor can miss the exact
// physical tile by a pixel and must not be corrected or floated for it.
// Per-monitor aware windows still have to match exactly.
[[nodiscard]] bool
placementLanded(const QRect& frame, const QRect& target, bool virtualized, int tolerance);

// How many physical pixels a placement may miss its tile by and still have
// landed. Windows bitmap-scales a window that is not per-monitor aware (an
// unaware one from 96 DPI, a system-aware one from the system DPI) by `scale`
// onto a monitor of another DPI, so it sits on whole units of that grid: up to
// one unit off. No scaling, or a per-monitor aware window, means exact pixels.
[[nodiscard]] int landingTolerance(quint32 awareness, qreal scale);

// The rectangle a window should return to when it stops being tiled. The
// captured pre-tiling frame wins over a parked rectangle, as before.
[[nodiscard]] QRect restoreTarget(const QRect& previous, const QRect& parkedRaw);
[[nodiscard]] QRect clampInto(QRect rect, const QRect& work);

// Moves a frame larger than its tile inside the work area, keeping its real
// size. On each axis the origin is clamped into [start, end - size]; an axis
// longer than the work area starts at its leading edge and overflows past the
// far one. That overflow is an accepted platform limit: nothing can fit a
// window whose minimum exceeds the work area, and covering reserved space
// such as the bar would be worse. `scale` is the window's bitmap scale (1 for
// per-monitor aware); the bounds are rounded inward by that scale's rounding.
[[nodiscard]] QRect fitOverflow(const QRect& frame, const QRect& work, qreal scale);

// Window operations used to restore a floating window. The seam keeps the
// restore policy testable without touching a real desktop.
class WindowOps {
public:
	virtual ~WindowOps() = default;

	[[nodiscard]] virtual bool readFrame(quintptr window, QRect& raw, QRect& frame) const = 0;
	virtual void setRawRect(quintptr window, const QRect& rect) = 0;
	[[nodiscard]] virtual bool iconicOrZoomed(quintptr window) const = 0;
	virtual void restorePlacement(quintptr window, const QRect& rect) = 0;
	[[nodiscard]] virtual QRect onScreen(const QRect& rect) const = 0;
	[[nodiscard]] virtual quintptr monitorFor(const QRect& rect) const = 0;
	[[nodiscard]] virtual QRect workArea(quintptr monitor) const = 0;
};

// Moves `window` back to its captured rectangle. Returns true when the window
// frame was written. `parkedRaw` is the raw rectangle recorded while the window
// was parked off screen, if any.
bool restoreTiledWindow(
    WindowOps& ops,
    quintptr window,
    const QRect& previous,
    const QRect& parkedRaw = QRect()
);

// Read-only tiling outcome surface for QML. It lives next to the manager so the
// policy above can be observed without a live tiler.
class TilingTelemetry: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged);
	Q_PROPERTY(qint32 managedCount READ managedCount NOTIFY managedCountChanged);
	Q_PROPERTY(qint32 failedCount READ failedCount NOTIFY failedCountChanged);
	// clang-format on

public:
	explicit TilingTelemetry(QObject* parent = nullptr);

	[[nodiscard]] QString lastError() const { return this->mLastError; }
	[[nodiscard]] qint32 managedCount() const { return this->mManagedCount; }
	[[nodiscard]] qint32 failedCount() const { return this->mFailedCount; }

	void enableSucceeded();
	void refuseEnable(const QString& reason);
	void noteFailure(const QString& address, const QString& reason);
	void notePlacement(const PlacementOutcome& outcome);
	void setManagedCount(qint32 count);
	void reset();

signals:
	void lastErrorChanged();
	void enableRefused(QString reason);
	void windowTilingFailed(QString address, QString reason);
	void managedCountChanged();
	void failedCountChanged();
	// C++ diagnostics only. The existing QML failure surface stays unchanged.
	void placementReported(qs::windows::tiling::PlacementOutcome outcome);

private:
	void setLastError(const QString& error);

	QString mLastError;
	qint32 mManagedCount = 0;
	qint32 mFailedCount = 0;
};

} // namespace qs::windows::tiling

Q_DECLARE_METATYPE(qs::windows::tiling::PlacementOutcome)
