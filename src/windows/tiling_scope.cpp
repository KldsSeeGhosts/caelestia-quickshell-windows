#include "tiling_scope.hpp"

#include <cmath>

#include <qlogging.h>

namespace qs::windows::tiling {

namespace {
constexpr auto EDGE_TOLERANCE = 2;
// Win32 DPI_AWARENESS_UNAWARE and _SYSTEM_AWARE. The seam stays free of Windows headers.
constexpr quint32 DPI_AWARENESS_UNAWARE = 0;
constexpr quint32 DPI_AWARENESS_SYSTEM_AWARE = 1;
} // namespace

const char* floatReasonName(FloatReason reason) {
	switch (reason) {
	case FloatReason::None: return "none";
	case FloatReason::OwnProcess: return "own-process";
	case FloatReason::Elevated: return "elevated";
	case FloatReason::Owned: return "owned";
	case FloatReason::ExtendedStyle: return "extended-style";
	case FloatReason::NoThickFrame: return "no-thick-frame";
	case FloatReason::Dialog: return "dialog";
	case FloatReason::Excluded: return "excluded";
	case FloatReason::NotIncluded: return "not-included";
	}

	return "none";
}

void ScopeRules::setExcluded(const QStringList& excluded) {
	this->mExcluded = excluded;
	this->excludedRules.setPatterns(excluded);
}

void ScopeRules::setIncluded(const QStringList& included) {
	this->mIncluded = included;
	this->includedRules.setPatterns(included);
}

bool ScopeRules::matchesExcluded(const WindowFacts& window) const {
	return this->excludedRules
	    .matches(window.appId, window.exePath, window.title, window.className, window.screenName);
}

bool ScopeRules::matchesIncluded(const WindowFacts& window) const {
	return this->includedRules
	    .matches(window.appId, window.exePath, window.title, window.className, window.screenName);
}

bool ScopeRules::hasTitleRules() const {
	return this->excludedRules.hasTitleRules() || this->includedRules.hasTitleRules();
}

FloatReason ScopeRules::floatReason(const WindowFacts& window) const {
	// The shell never tiles itself, whatever the allowlist says.
	if (window.ownProcess) return FloatReason::OwnProcess;
	if (window.elevated) return FloatReason::Elevated;
	if (window.owned) return FloatReason::Owned;

	if (window.toolWindow || window.topmost || window.noActivate || window.dialogFrame) {
		return FloatReason::ExtendedStyle;
	}

	if (!window.hasThickFrame) return FloatReason::NoThickFrame;
	if (window.className == QStringLiteral("#32770")) return FloatReason::Dialog;

	// Exclusions always win over the allowlist.
	if (this->matchesExcluded(window)) return FloatReason::Excluded;
	if (this->hasIncluded() && !this->matchesIncluded(window)) return FloatReason::NotIncluded;

	return FloatReason::None;
}

const char* failureReasonName(FailureReason reason) {
	switch (reason) {
	case FailureReason::None: return "none";
	case FailureReason::MinSize: return "min-size";
	case FailureReason::OffTarget: return "off-target";
	case FailureReason::CorrectionLimit: return "correction-limit";
	case FailureReason::Unresponsive: return "unresponsive";
	}

	return "none";
}

const char* placementDispositionName(PlacementDisposition disposition) {
	switch (disposition) {
	case PlacementDisposition::Idle: return "idle";
	case PlacementDisposition::Accepted: return "accepted";
	case PlacementDisposition::Landed: return "observed";
	case PlacementDisposition::Denied: return "denied";
	case PlacementDisposition::Failed: return "failed";
	case PlacementDisposition::SizeConstraint: return "size-constraint";
	case PlacementDisposition::OffTarget: return "off-target";
	case PlacementDisposition::CorrectionLimit: return "correction-limit";
	case PlacementDisposition::Vanished: return "target-vanished";
	case PlacementDisposition::Superseded: return "superseded";
	case PlacementDisposition::Deadline: return "timed-out";
	}
	return "idle";
}

GiveUpOutcome classifyGiveUp(const GiveUpInput& input) {
	auto outcome = GiveUpOutcome();
	auto tolerance = input.tolerance > 0 ? input.tolerance : EDGE_TOLERANCE;

	if (input.offscreen) {
		outcome.accept = true;
		return outcome;
	}

	auto bigger = input.frame.width() > input.target.width() + tolerance
	           || input.frame.height() > input.target.height() + tolerance;
	// A frame larger than its tile cannot be judged by its centre. It grew in
	// place when it still starts at its anchor (the slot, or where its overflow
	// was moved into the work area); starting anywhere else, it was moved away.
	auto anchor = input.anchor.value_or(input.target.topLeft());
	auto away = bigger ? qAbs(input.frame.left() - anchor.x()) > tolerance
	                         || qAbs(input.frame.top() - anchor.y()) > tolerance
	                   : !input.target.contains(input.frame.center());

	if (away) {
		outcome.autoFloat = true;
		outcome.reason = FailureReason::OffTarget;
		return outcome;
	}

	// Hyprland's default (size_limits_tiled = false): a window that will not
	// shrink to its slot stays tiled at its larger size.
	if (bigger) {
		outcome.accept = true;
		outcome.reason = FailureReason::MinSize;
		return outcome;
	}

	// The frame is no larger than the tile and its centre is still in the tile. Keep
	// the window tiled with a gap, as before, but report that the correction
	// budget was exhausted without an exact fit.
	outcome.accept = true;
	if (input.frame != input.target) outcome.reason = FailureReason::CorrectionLimit;
	return outcome;
}

bool dpiVirtualized(quint32 awareness) { return awareness == DPI_AWARENESS_UNAWARE; }

int landingTolerance(quint32 awareness, qreal scale) {
	if (awareness > DPI_AWARENESS_SYSTEM_AWARE || !(scale > 0.0)) return 0;
	// Measured from whole-pixel rectangles, so allow a percent of noise.
	constexpr qreal NOISE = 0.01;
	if (qAbs(scale - 1.0) < NOISE) return 0;
	return qMax(1, static_cast<int>(std::ceil(scale - NOISE)));
}

bool placementLanded(const QRect& frame, const QRect& target, bool virtualized, int tolerance) {
	if (frame == target) return true;
	if (!virtualized) return false;

	auto slack = qMax(0, tolerance);
	return qAbs(frame.left() - target.left()) <= slack && qAbs(frame.top() - target.top()) <= slack
	    && qAbs(frame.width() - target.width()) <= slack
	    && qAbs(frame.height() - target.height()) <= slack;
}

QRect restoreTarget(const QRect& previous, const QRect& parkedRaw) {
	return previous.isNull() ? parkedRaw : previous;
}

namespace {
int fitAxis(int origin, int size, int start, int extent, qreal slack) {
	// Round the bounds inward: ceil the leading one, floor the trailing one.
	auto lead = static_cast<int>(std::ceil(start + slack));
	auto last = static_cast<int>(std::floor(start + extent - size - 2.0 * slack));
	// Longer than the work area: start at its leading edge and overflow past
	// the far one, so reserved space such as the bar is never covered.
	if (last < lead) return lead;
	return qBound(lead, origin, last);
}
} // namespace

QRect fitOverflow(const QRect& frame, const QRect& work, qreal scale) {
	if (!work.isValid()) return frame;
	// A bitmap-scaled window lands on whole units of its own grid, so each edge
	// can end up half a unit, plus a pixel of rounding, from the request. The
	// scale is measured from whole-pixel rectangles; within a percent of 1:1
	// the window is not scaled and keeps exact pixels.
	auto slack = scale > 0.0 && qAbs(scale - 1.0) >= 0.01 ? scale / 2.0 + 0.5 : 0.0;
	return {
	    QPoint(
	        fitAxis(frame.left(), frame.width(), work.left(), work.width(), slack),
	        fitAxis(frame.top(), frame.height(), work.top(), work.height(), slack)
	    ),
	    frame.size()
	};
}

QRect clampInto(QRect rect, const QRect& work) {
	if (!work.isValid()) return rect;
	rect.setSize(rect.size().boundedTo(work.size()));
	rect.moveLeft(qBound(work.left(), rect.left(), work.left() + work.width() - rect.width()));
	rect.moveTop(qBound(work.top(), rect.top(), work.top() + work.height() - rect.height()));
	return rect;
}

bool restoreTiledWindow(
    WindowOps& ops,
    quintptr window,
    const QRect& previous,
    const QRect& parkedRaw
) {
	auto target = tiling::restoreTarget(previous, parkedRaw);
	if (target.isNull()) return false;

	if (ops.iconicOrZoomed(window)) {
		ops.restorePlacement(window, ops.onScreen(target));
		return true;
	}

	QRect raw;
	QRect frame;
	if (!ops.readFrame(window, raw, frame)) return false;
	if (!parkedRaw.isNull()) raw = parkedRaw;

	auto rect = target;
	auto then = ops.monitorFor(target);
	auto now = ops.monitorFor(raw);

	if (then != now) {
		rect.moveCenter(raw.center());
		auto work = ops.workArea(now);
		rect = clampInto(rect, work);
	}

	ops.setRawRect(window, ops.onScreen(rect));
	return true;
}

TilingTelemetry::TilingTelemetry(QObject* parent): QObject(parent) {}

void TilingTelemetry::setLastError(const QString& error) {
	if (error == this->mLastError) return;
	this->mLastError = error;
	QMetaObject::invokeMethod(
	    this,
	    [this]() { emit this->lastErrorChanged(); },
	    Qt::QueuedConnection
	);
}

void TilingTelemetry::enableSucceeded() {
	this->setLastError(QString());
	if (this->mFailedCount != 0) {
		this->mFailedCount = 0;
		QMetaObject::invokeMethod(
		    this,
		    [this]() { emit this->failedCountChanged(); },
		    Qt::QueuedConnection
		);
	}
}

void TilingTelemetry::refuseEnable(const QString& reason) {
	this->setLastError(reason);
	QMetaObject::invokeMethod(
	    this,
	    [this, reason]() { emit this->enableRefused(reason); },
	    Qt::QueuedConnection
	);
}

void TilingTelemetry::noteFailure(const QString& address, const QString& reason) {
	this->mFailedCount++;
	// No outward callback may run while the manager holds Managed references
	// or layout/map iterators. Keep values current, publish on the next turn.
	QMetaObject::invokeMethod(
	    this,
	    [this, address, reason]() {
		    emit this->failedCountChanged();
		    emit this->windowTilingFailed(address, reason);
	    },
	    Qt::QueuedConnection
	);
}

void TilingTelemetry::notePlacement(const PlacementOutcome& outcome) {
	QMetaObject::invokeMethod(
	    this,
	    [this, outcome]() { emit this->placementReported(outcome); },
	    Qt::QueuedConnection
	);
}

void TilingTelemetry::setManagedCount(qint32 count) {
	count = qMax(0, count);
	if (count == this->mManagedCount) return;
	this->mManagedCount = count;
	QMetaObject::invokeMethod(
	    this,
	    [this]() { emit this->managedCountChanged(); },
	    Qt::QueuedConnection
	);
}

void TilingTelemetry::reset() {
	this->setLastError(QString());
	this->setManagedCount(0);
	if (this->mFailedCount != 0) {
		this->mFailedCount = 0;
		QMetaObject::invokeMethod(
		    this,
		    [this]() { emit this->failedCountChanged(); },
		    Qt::QueuedConnection
		);
	}
}

} // namespace qs::windows::tiling
