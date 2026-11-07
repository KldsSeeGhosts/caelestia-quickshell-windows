#include "tiling.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>

#include <dwmapi.h>
#include <qcoreapplication.h>
#include <chrono>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qoperatingsystemversion.h>
#include <qpoint.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtypes.h>
#include "transition_trace.hpp"
#include "redact.hpp"

#ifdef QS_TILING_FAKE_DESKTOP
#include "test/tiling_fake_desktop.hpp"
#else
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"
#endif
#include "fullscreen_geometry.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logTiling, "quickshell.windows.tiling", QtWarningMsg);

using tiling::Edge;
using tiling::PlacementDisposition;

constexpr int SYNC_DELAY_MS = 30;
constexpr int VERIFY_DELAY_MS = 150;
constexpr int FULLSCREEN_STYLE_RECHECK_MS = 120;
constexpr int FULLSCREEN_STYLE_GRACE_MS = 300;
constexpr int DEADLINE_MS = 2000;
// A self-resizing window must hold still this long before it is judged, and
// may keep resizing itself this long before corrections resume anyway. The
// poll is how often a waiting window is looked at again.
constexpr int SETTLE_QUIET_MS = 200;
constexpr int SETTLE_POLL_MS = 50;
constexpr int SETTLE_LIMIT_MS = 2000;
// A settled self-resized window eases back into its tile over this long,
// one step per frame. An easing is not a failed correction, so it does not
// spend the correction budget; at most this many run per burst, after which the
// ordinary corrections and give-up apply.
constexpr int SNAP_MS = 150;
constexpr int SNAP_STEP_MS = 16;
constexpr qint32 MAX_SNAPS = 2;
constexpr qint32 MAX_CORRECTIONS = 2;
constexpr qint64 CORRECTION_RESET_MS = 1500;
constexpr int EDGE_TOLERANCE = 2;
// A Super drop inserts within an edge band of 20% of the tile, at most this
// many dp. A drop zone or target is kept until the cursor is this far past it.
constexpr int DROP_BAND_DP = 120;
constexpr int DROP_HYSTERESIS_DP = 8;
// Frame bits a fullscreen window normally drops. Regaining either one after
// the suspension began confirms an exit even when the rectangle still covers
// the monitor (Windows restores the fullscreen-era normal placement).
constexpr LONG_PTR FULLSCREEN_FRAME_STYLE = WS_CAPTION | WS_THICKFRAME;

#ifndef QS_TILING_FAKE_DESKTOP
qint64 tilingNow() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool borderColoursSupported() {
	return QOperatingSystemVersion::current()
	    >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22000);
}
#endif

TilingManager* gManager = nullptr; // NOLINT

int rightOf(const QRect& rect) { return rect.left() + rect.width(); }
int bottomOf(const QRect& rect) { return rect.top() + rect.height(); }

QString windowClass(HWND hwnd) {
	wchar_t buffer[128] {};
	auto length = GetClassNameW(hwnd, buffer, 128);
	return QString::fromWCharArray(buffer, length);
}

bool dpiVirtualized(HWND hwnd) {
	auto context = GetWindowDpiAwarenessContext(hwnd);
	if (context == nullptr) return false;

	auto awareness = GetAwarenessFromDpiAwarenessContext(context);
	if (awareness < 0) return false;

	return tiling::dpiVirtualized(static_cast<quint32>(awareness));
}

// GetWindowRect and SetWindowPos speak the target window's DPI awareness space.
// For a window that is not per-monitor aware that space is a scaled 96-DPI
// grid, while DWMWA_EXTENDED_FRAME_BOUNDS and the tiler's tiles stay physical.
// Reading and writing from a per-monitor thread puts every call in physical
// pixels; the previous context is restored for the rest of the caller.
class PhysicalDpiScope {
public:
	explicit PhysicalDpiScope(HWND hwnd) {
		if (!dpiVirtualized(hwnd)) return;
		this->previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	}

	~PhysicalDpiScope() {
		if (this->previous != nullptr) SetThreadDpiAwarenessContext(this->previous);
	}

	PhysicalDpiScope(const PhysicalDpiScope&) = delete;
	PhysicalDpiScope& operator=(const PhysicalDpiScope&) = delete;

private:
	DPI_AWARENESS_CONTEXT previous = nullptr;
};

bool readFrame(HWND hwnd, QRect& raw, QRect& frame) {
	auto physical = PhysicalDpiScope(hwnd);

	RECT rawRect {};
	if (!GetWindowRect(hwnd, &rawRect)) return false;

	RECT frameRect {};
	auto hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
	if (FAILED(hr)) frameRect = rawRect;

	raw = toQRect(rawRect);
	frame = toQRect(frameRect);
	return true;
}

QRect currentFrame(HWND hwnd) {
	QRect raw;
	QRect frame;
	readFrame(hwnd, raw, frame);
	return frame;
}

std::optional<DWORD> setFrame(HWND hwnd, const QRect& target) {
	auto physical = PhysicalDpiScope(hwnd);

	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return GetLastError();

	auto left = frame.left() - raw.left();
	auto top = frame.top() - raw.top();
	auto right = rightOf(raw) - rightOf(frame);
	auto bottom = bottomOf(raw) - bottomOf(frame);

	if (!SetWindowPos(
	    hwnd,
	    nullptr,
	    target.left() - left,
	    target.top() - top,
	    target.width() + left + right,
	    target.height() + top + bottom,
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	)) return GetLastError();
	return std::nullopt;
}

void setRawRect(HWND hwnd, const QRect& rect) {
	auto physical = PhysicalDpiScope(hwnd);

	if (!SetWindowPos(
	    hwnd,
	    nullptr,
	    rect.left(),
	    rect.top(),
	    rect.width(),
	    rect.height(),
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	)) {
		qCWarning(logTiling) << "Restoring window geometry failed:" << GetLastError();
	}
}

// A window that is not per-monitor aware cannot land on every physical pixel
// when Windows bitmap-scales it, so its tile is accepted within that scale's
// rounding instead of being corrected. GetWindowRect read in the window's own
// DPI context returns its scaled grid; the ratio to the physical rectangle is
// the scale. Per-monitor aware windows keep the exact comparison.
struct BitmapScale {
	quint32 awareness = DPI_AWARENESS_PER_MONITOR_AWARE;
	qreal scale = 1.0;
};

BitmapScale bitmapScale(HWND hwnd) {
	auto context = GetWindowDpiAwarenessContext(hwnd);
	if (context == nullptr) return {};
	auto awareness = GetAwarenessFromDpiAwarenessContext(context);
	if (awareness < 0 || awareness == DPI_AWARENESS_PER_MONITOR_AWARE) return {};

	RECT physical {};
	RECT scaled {};
	auto* previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	auto read = GetWindowRect(hwnd, &physical) != FALSE;
	SetThreadDpiAwarenessContext(context);
	read = GetWindowRect(hwnd, &scaled) != FALSE && read;
	SetThreadDpiAwarenessContext(previous);

	auto width = scaled.right - scaled.left;
	auto height = scaled.bottom - scaled.top;
	if (!read || width <= 0 || height <= 0) return {};
	auto scale = width >= height ? qreal(physical.right - physical.left) / width
	                             : qreal(physical.bottom - physical.top) / height;
	return {static_cast<quint32>(awareness), scale};
}

int landingTolerance(HWND hwnd) {
	auto bitmap = bitmapScale(hwnd);
	return tiling::landingTolerance(bitmap.awareness, bitmap.scale);
}

bool landed(HWND hwnd, const QRect& frame, const QRect& target) {
	if (frame == target) return true;
	auto tolerance = landingTolerance(hwnd);
	return tiling::placementLanded(frame, target, tolerance > 0, tolerance);
}

QPoint cursorPos() {
	POINT point {};
	GetCursorPos(&point);
	return {point.x, point.y};
}

QRect physicalMonitor(QScreen* screen) { return monitorRects(monitorForScreen(screen)).monitor; }

qint32 toPhysical(qint32 logical, qreal dpr) {
	return static_cast<qint32>(std::lround(logical * dpr));
}

constexpr std::size_t PARK_SLOTS = 256;

struct ParkSlot {
	std::atomic<HWND> hwnd {nullptr};
	std::atomic<quint64> rect {0};
};

std::array<ParkSlot, PARK_SLOTS> gParkSlots {}; // NOLINT
std::atomic<ATOM> gParkAtom {0};               // NOLINT

LPCWSTR atomName(ATOM atom) {
	auto value = static_cast<ULONG_PTR>(atom);
	return reinterpret_cast<LPCWSTR>(value); // NOLINT(performance-no-int-to-ptr)
}

ATOM parkAtom() {
	auto atom = gParkAtom.load();
	if (atom != 0) return atom;

	atom = GlobalAddAtomW(L"QuickshellTilingParked");
	gParkAtom.store(atom);
	return atom;
}

quint64 packRect(const QRect& rect) {
	auto x = static_cast<quint64>(static_cast<quint16>(qBound(-32768, rect.left(), 32767)));
	auto y = static_cast<quint64>(static_cast<quint16>(qBound(-32768, rect.top(), 32767)));
	auto width = static_cast<quint64>(qBound(1, rect.width(), 65535));
	auto height = static_cast<quint64>(qBound(1, rect.height(), 65535));
	return x | (y << 16) | (width << 32) | (height << 48);
}

QRect unpackRect(quint64 packed) {
	auto x = static_cast<qint16>(packed & 0xFFFF);
	auto y = static_cast<qint16>((packed >> 16) & 0xFFFF);
	auto width = static_cast<int>((packed >> 32) & 0xFFFF);
	auto height = static_cast<int>((packed >> 48) & 0xFFFF);
	return {x, y, width, height};
}

void rememberParked(HWND hwnd, const QRect& restore) {
	auto packed = packRect(restore);
	ParkSlot* target = nullptr;

	for (auto& slot: gParkSlots) {
		auto* value = slot.hwnd.load();
		if (value == hwnd) {
			target = &slot;
			break;
		}
		if (value == nullptr && target == nullptr) target = &slot;
	}

	if (target != nullptr) {
		target->rect.store(packed);
		target->hwnd.store(hwnd);
	}

	if (auto atom = parkAtom(); atom != 0) {
		auto handle = static_cast<quintptr>(packed);
		SetPropW(hwnd, atomName(atom), reinterpret_cast<HANDLE>(handle)); // NOLINT
	}
}

void forgetParked(HWND hwnd) {
	for (auto& slot: gParkSlots) {
		if (slot.hwnd.load() == hwnd) slot.hwnd.store(nullptr);
	}

	if (auto atom = gParkAtom.load(); atom != 0) RemovePropW(hwnd, atomName(atom));
}

using tiling::clampInto;

QRect onScreen(const QRect& rect) {
	auto native = toRECT(rect);
	if (MonitorFromRect(&native, MONITOR_DEFAULTTONULL) != nullptr) return rect;
	return clampInto(rect, monitorRects(MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST)).work);
}

// Tiling rectangles are always in screen coordinates. WINDOWPLACEMENT uses
// workspace coordinates for ordinary top-level windows, but not tool windows.
QPoint placementOffset(HWND hwnd, HMONITOR monitor) {
	if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return {};
	auto rects = monitorRects(monitor);
	return rects.valid ? rects.work.topLeft() - rects.monitor.topLeft() : QPoint();
}

RECT screenToPlacement(HWND hwnd, const QRect& rect) {
	auto native = toRECT(rect);
	auto* monitor = MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST);
	return toRECT(rect.translated(-placementOffset(hwnd, monitor)));
}

void restorePlacement(HWND hwnd, const QRect& rect) {
	WINDOWPLACEMENT placement {};
	placement.length = sizeof(placement);
	if (!GetWindowPlacement(hwnd, &placement)) return;
	placement.rcNormalPosition = screenToPlacement(hwnd, rect);
	SetWindowPlacement(hwnd, &placement);
}

tiling::CenterMode centerModeOf(const QString& value) {
	if (value == QStringLiteral("always")) return tiling::CenterMode::Always;
	if (value == QStringLiteral("never")) return tiling::CenterMode::Never;
	return tiling::CenterMode::Overflow;
}

// Adapts the Win32 frame helpers to the restore seam in tiling_scope.
class Win32WindowOps: public tiling::WindowOps {
public:
	[[nodiscard]] bool readFrame(quintptr window, QRect& raw, QRect& frame) const override {
		return qs::windows::readFrame(reinterpret_cast<HWND>(window), raw, frame);
	}

	void setRawRect(quintptr window, const QRect& rect) override {
		qs::windows::setRawRect(reinterpret_cast<HWND>(window), rect);
	}

	[[nodiscard]] bool iconicOrZoomed(quintptr window) const override {
		auto* hwnd = reinterpret_cast<HWND>(window);
		return IsIconic(hwnd) || IsZoomed(hwnd);
	}

	void restorePlacement(quintptr window, const QRect& rect) override {
		qs::windows::restorePlacement(reinterpret_cast<HWND>(window), rect);
	}

	[[nodiscard]] QRect onScreen(const QRect& rect) const override {
		return qs::windows::onScreen(rect);
	}

	[[nodiscard]] quintptr monitorFor(const QRect& rect) const override {
		auto native = toRECT(rect);
		auto* monitor = MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST);
		return reinterpret_cast<quintptr>(monitor); // NOLINT(performance-no-int-to-ptr)
	}

	[[nodiscard]] QRect workArea(quintptr monitor) const override {
		return monitorRects(reinterpret_cast<HMONITOR>(monitor)).work; // NOLINT
	}
};

} // namespace

TilingManager* TilingManager::instance() {
	static auto* instance = new TilingManager(); // NOLINT
	return instance;
}

TilingManager* TilingManager::active() {
	return gManager != nullptr && gManager->mEnabled ? gManager : nullptr;
}

TilingManager::TilingManager(QString mutexName): mutexName(std::move(mutexName)) {
	gManager = this;
	this->mTelemetry = new tiling::TilingTelemetry(this);

	this->syncTimer.setSingleShot(true);
	this->syncTimer.setInterval(SYNC_DELAY_MS);
	QObject::connect(&this->syncTimer, &QTimer::timeout, this, &TilingManager::sync);

	this->verifyTimer.setSingleShot(true);
	this->verifyTimer.setInterval(VERIFY_DELAY_MS);
	QObject::connect(&this->verifyTimer, &QTimer::timeout, this, &TilingManager::verify);

	this->deadlineTimer.setSingleShot(true);
	this->deadlineTimer.setInterval(DEADLINE_MS);
	QObject::connect(&this->deadlineTimer, &QTimer::timeout, this, &TilingManager::onDeadline);

	this->snapTimer.setInterval(SNAP_STEP_MS);
	QObject::connect(&this->snapTimer, &QTimer::timeout, this, &TilingManager::stepSnaps);

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
			this->restoreBorderColours();
			this->unparkAll();
		});
	}

	TilingManager::rescueParked();
}

TilingManager::~TilingManager() {
	this->restoreBorderColours();
	if (gManager == this) gManager = nullptr;
}

void TilingManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	if (enabled) {
		auto error = this->start();
		if (!error.isEmpty()) {
			this->mEnabled = false;
			qCWarning(logTiling) << "Enabling tiling was refused:" << error;
			this->mTelemetry->refuseEnable(error);
			return;
		}
		this->mTelemetry->enableSucceeded();
	} else {
		this->stop();
	}

	emit this->enabledChanged();
}

quint32 TilingManager::claimGeneration() {
	// Skip zero on wraparound: it means "never claimed".
	if (++this->mOwner == 0) ++this->mOwner;
	return this->mOwner;
}

bool TilingManager::setEnabled(bool enabled, quint32 generation) {
	if (generation == 0 || generation != this->mOwner) return false;
	this->setEnabled(enabled);
	return true;
}

quint32 TilingLease::claim() {
	this->mGeneration = TilingManager::instance()->claimGeneration();
	return this->mGeneration;
}

void TilingLease::release() {
	if (this->mGeneration == 0) return;
	// The generation check makes this a no-op once a newer holder has claimed.
	TilingManager::instance()->setEnabled(false, this->mGeneration);
	this->mGeneration = 0;
}

TilingLease::~TilingLease() { this->release(); }

void TilingManager::setGapsIn(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsIn) return;
	this->mGapsIn = gaps;
	this->markAll();
	emit this->gapsInChanged();
}

void TilingManager::setGapsOut(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsOut) return;
	this->mGapsOut = gaps;
	this->markAll();
	emit this->gapsOutChanged();
}

void TilingManager::setSingleWindowGapsOut(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mSingleWindowGapsOut) return;
	this->mSingleWindowGapsOut = gaps;
	this->markAll();
	emit this->singleWindowGapsOutChanged();
}

void TilingManager::setBorderColours(bool enabled) {
	if (enabled == this->mBorderColours) return;
	this->mBorderColours = enabled;
	this->refreshBorderColours();
	emit this->borderColoursChanged();
}

void TilingManager::setActiveBorderColour(const QColor& colour) {
	if (colour == this->mActiveBorderColour) return;
	this->mActiveBorderColour = colour;
	this->refreshBorderColours();
	emit this->activeBorderColourChanged();
}

void TilingManager::setInactiveBorderColour(const QColor& colour) {
	if (colour == this->mInactiveBorderColour) return;
	this->mInactiveBorderColour = colour;
	this->refreshBorderColours();
	emit this->inactiveBorderColourChanged();
}

void TilingManager::restoreBorderColour(Managed& m) {
	if (!m.borderColour) return;
	if (IsWindow(m.window->hwnd())) {
		const COLORREF colour = DWMWA_COLOR_DEFAULT;
		DwmSetWindowAttribute(m.window->hwnd(), DWMWA_BORDER_COLOR, &colour, sizeof(colour));
	}
	m.borderColour.reset();
}

void TilingManager::restoreBorderColours() {
	for (auto& [window, m]: this->managed) this->restoreBorderColour(m);
}

void TilingManager::refreshBorderColours() {
	if (!borderColoursSupported()) return;
	for (auto& [window, m]: this->managed) {
		if (!this->mEnabled || !this->mBorderColours || m.layout == nullptr
		    || window->fullscreen() || this->fullscreenLike(m)
		    || this->stateOf(m) == State::Out)
		{
			this->restoreBorderColour(m);
			continue;
		}
		const auto& colour = window == this->tracker->activeWindow() ? this->mActiveBorderColour
		                                                             : this->mInactiveBorderColour;
		if (!colour.isValid()) {
			this->restoreBorderColour(m);
			continue;
		}
		auto native = RGB(colour.red(), colour.green(), colour.blue());
		if (m.borderColour == native) continue;
		if (SUCCEEDED(
		        DwmSetWindowAttribute(window->hwnd(), DWMWA_BORDER_COLOR, &native, sizeof(native))
		    ))
		{
			m.borderColour = native;
		}
	}
}

void TilingManager::setPreserveSplit(bool preserve) {
	if (preserve == this->mPreserveSplit) return;
	this->mPreserveSplit = preserve;
	this->markAll();
	emit this->preserveSplitChanged();
}

void TilingManager::setExcluded(const QStringList& excluded) {
	if (excluded == this->mExcluded) return;
	this->mExcluded = excluded;
	this->scopeRules.setExcluded(excluded);

	this->markAll();
	emit this->excludedChanged();
}

void TilingManager::setIncluded(const QStringList& included) {
	if (included == this->mIncluded) return;
	this->mIncluded = included;
	this->scopeRules.setIncluded(included);

	qCDebug(logTiling) << "Tiling allowlist changed to" << included.size()
	                   << "patterns; re-evaluating window membership.";
	this->markAll();
	emit this->includedChanged();
}

void TilingManager::setLayout(const QString& layout) {
	auto normalized = layout.trimmed().toLower();
	if (normalized != QStringLiteral("master") && normalized != QStringLiteral("scrolling")) {
		normalized = QStringLiteral("dwindle");
	}

	if (normalized == this->mLayout) return;
	this->mLayout = normalized;
	if (this->mEnabled) this->rebuildLayouts();
	emit this->layoutChanged();
}

void TilingManager::setSmartSplit(bool smart) {
	if (smart == this->mSmartSplit) return;
	this->mSmartSplit = smart;
	this->markAll();
	emit this->smartSplitChanged();
}

void TilingManager::setForceSplit(qint32 force) {
	force = qBound(0, force, 2);
	if (force == this->mForceSplit) return;
	this->mForceSplit = force;
	emit this->forceSplitChanged();
}

void TilingManager::setDefaultSplitRatio(double ratio) {
	ratio = qBound(0.1, ratio, 1.9);
	if (qFuzzyCompare(ratio, this->mDefaultSplitRatio)) return;
	this->mDefaultSplitRatio = ratio;
	emit this->defaultSplitRatioChanged();
}

void TilingManager::setMasterNewStatus(const QString& status) {
	auto value = status.trimmed().toLower();
	if (value != QStringLiteral("master") && value != QStringLiteral("inherit")) {
		value = QStringLiteral("slave");
	}

	if (value == this->mMasterNewStatus) return;
	this->mMasterNewStatus = value;
	this->markAll();
	emit this->masterNewStatusChanged();
}

void TilingManager::setMasterNewOnTop(bool onTop) {
	if (onTop == this->mMasterNewOnTop) return;
	this->mMasterNewOnTop = onTop;
	this->markAll();
	emit this->masterNewOnTopChanged();
}

void TilingManager::setMasterOrientation(const QString& orientation) {
	static const auto known = QStringList {
	    QStringLiteral("left"),
	    QStringLiteral("right"),
	    QStringLiteral("top"),
	    QStringLiteral("bottom"),
	    QStringLiteral("center"),
	};

	auto value = orientation.trimmed().toLower();
	if (!known.contains(value)) value = QStringLiteral("left");
	if (value == this->mMasterOrientation) return;
	this->mMasterOrientation = value;
	this->markAll();
	emit this->masterOrientationChanged();
}

void TilingManager::setMasterFactor(double factor) {
	factor = qBound(0.05, factor, 0.95);
	if (qFuzzyCompare(factor, this->mMasterFactor)) return;
	this->mMasterFactor = factor;
	this->markAll();
	emit this->masterFactorChanged();
}

void TilingManager::setScrollingColumnWidth(double width) {
	width = qBound(tiling::ScrollingLayout::MIN_WIDTH, width, tiling::ScrollingLayout::MAX_WIDTH);
	if (qFuzzyCompare(width, this->mScrollingColumnWidth)) return;
	this->mScrollingColumnWidth = width;
	this->markAll();
	emit this->scrollingColumnWidthChanged();
}

void TilingManager::setScrollingCenter(const QString& center) {
	auto value = center.trimmed().toLower();
	if (value != QStringLiteral("always") && value != QStringLiteral("never")) {
		value = QStringLiteral("overflow");
	}

	if (value == this->mScrollingCenter) return;
	this->mScrollingCenter = value;
	this->markAll();
	emit this->scrollingCenterChanged();
}

tiling::Kind TilingManager::layoutKind() const {
	if (this->mLayout == QStringLiteral("master")) return tiling::Kind::Master;
	if (this->mLayout == QStringLiteral("scrolling")) return tiling::Kind::Scrolling;
	return tiling::Kind::Dwindle;
}

tiling::Options TilingManager::options() const {
	auto options = tiling::Options();
	options.preserveSplit = this->mPreserveSplit;
	options.smartSplit = this->mSmartSplit;
	options.forceSplit = this->mForceSplit;
	options.defaultSplitRatio = this->mDefaultSplitRatio;
	options.newOnTop = this->mMasterNewOnTop;
	options.mfact = this->mMasterFactor;
	options.columnWidth = this->mScrollingColumnWidth;
	options.centerMode = centerModeOf(this->mScrollingCenter);

	if (this->fitToggled) {
		options.centerMode = options.centerMode == tiling::CenterMode::Always
		                       ? tiling::CenterMode::Overflow
		                       : tiling::CenterMode::Always;
	}

	if (this->mMasterNewStatus == QStringLiteral("master")) {
		options.newStatus = tiling::NewStatus::Master;
	} else if (this->mMasterNewStatus == QStringLiteral("inherit")) {
		options.newStatus = tiling::NewStatus::Inherit;
	}

	const auto& o = this->mMasterOrientation;
	if (o == QStringLiteral("right")) options.orientation = tiling::Orientation::Right;
	else if (o == QStringLiteral("top")) options.orientation = tiling::Orientation::Top;
	else if (o == QStringLiteral("bottom")) options.orientation = tiling::Orientation::Bottom;
	else if (o == QStringLiteral("center")) options.orientation = tiling::Orientation::Center;

	return options;
}

void TilingManager::rebuildLayouts() {
	auto kind = this->layoutKind();
	auto options = this->options();

	auto seed = options;
	seed.smartSplit = false;
	seed.forceSplit = 0;
	seed.newStatus = tiling::NewStatus::Slave;
	seed.newOnTop = false;

	for (auto& [key, layout]: this->layouts) {
		if (layout->tree->kind() == kind) continue;

		auto fresh = tiling::LayoutEngine::create(kind);
		fresh->configure(seed);
		auto area = layout->tree->area();
		if (area.isValid()) fresh->compute(area);

		auto previous = tiling::LayoutEngine::Id(0);
		for (auto id: layout->tree->ids()) {
			fresh->insert(id, previous, nullptr);
			previous = id;
		}

		fresh->configure(options);
		layout->tree = std::move(fresh);

		if (auto* scroll = TilingManager::scrollingOf(layout.get())) {
			this->prepareScrolling(layout.get());
			if (layout->lastFocused != nullptr) scroll->activate(idOf(layout->lastFocused));
		}
	}

	this->edgeDragState = EdgeDrag();
	this->relayout();
}

void TilingManager::relayout() {
	if (!this->mEnabled) return;

	for (auto& [window, m]: this->managed) {
		this->supersedePlacement(m);
		m.target = QRect();
		m.accepted = QRect();
		m.corrections = 0;
	}

	this->markAll();
}

void TilingManager::markAll() {
	if (!this->mEnabled) return;
	for (auto& [window, m]: this->managed) this->dirtyWindows.insert(window);
	for (auto& [key, layout]: this->layouts) this->dirtyLayouts.insert(layout.get());
	this->schedule();
}

bool TilingManager::isTiled(TrackedWindow* window) const {
	auto iter = this->managed.find(window);
	return iter != this->managed.end() && iter->second.layout != nullptr;
}

QString TilingManager::start() {
	auto name = this->mutexName.toStdWString();
	this->ownerMutex = CreateMutexW(nullptr, TRUE, name.c_str());
	if (this->ownerMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
		qCWarning(logTiling) << "Another Quickshell process already tiles windows;"
		                     << "tiling stays off in this one.";
		if (this->ownerMutex != nullptr) CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
		return QStringLiteral("tiler-mutex-held");
	}

	if (QGuiApplication::instance() == nullptr || QGuiApplication::screens().isEmpty()) {
		qCWarning(logTiling) << "No screen is available to tile; tiling stays off.";
		ReleaseMutex(this->ownerMutex);
		CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
		return QStringLiteral("no-screen");
	}

	this->tracker = WindowTracker::instance();
	this->desktops = this->tracker->desktops();

	auto& c = this->connections;
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowAdded, this, &TilingManager::manage)
	);
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &TilingManager::unmanage)
	);
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &TilingManager::onActiveWindowChanged
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeStarted,
	    this,
	    &TilingManager::onMoveSizeStarted
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeEnded,
	    this,
	    &TilingManager::onMoveSizeEnded
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::movingLocationChanged,
	    this,
	    &TilingManager::onDragMoved
	));
	c.append(QObject::connect(this->desktops, &VirtualDesktops::desktopsChanged, this, [this]() {
		this->markAll();
	}));

	if (auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
		auto changed = &TilingManager::onScreensChanged;
		c.append(QObject::connect(app, &QGuiApplication::screenAdded, this, changed));
		c.append(QObject::connect(app, &QGuiApplication::screenRemoved, this, changed));
	}

	this->onScreensChanged();

	this->starting = true;
	for (auto* window: this->tracker->windows()) this->manage(window);
	this->starting = false;

	auto windows = this->tracker->windows();
	std::stable_sort(windows.begin(), windows.end(), [](TrackedWindow* a, TrackedWindow* b) {
		auto ca = a->rect().center();
		auto cb = b->rect().center();
		return ca.x() != cb.x() ? ca.x() < cb.x() : ca.y() < cb.y();
	});

	QHash<Layout*, TrackedWindow*> previous;
	for (auto* window: windows) {
		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		auto* layout = this->layoutFor(window, true);
		if (layout == nullptr) continue;

		this->insertInto(m, layout, previous.value(layout), nullptr);
		previous.insert(layout, window);
	}

	this->onActiveWindowChanged();
	this->schedule();
	this->refreshManagedCount();
	return {};
}

void TilingManager::stop() {
	this->restoreBorderColours();
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();

	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
	}

	QList<TrackedWindow*> tiled;
	for (auto& [window, m]: this->managed) {
		this->supersedePlacement(m);
		QObject::disconnect(window, nullptr, this, nullptr);
		if (m.layout == nullptr) continue;

		tiled.append(window);
		this->restoreFloating(m);
	}

	this->managed.clear();
	this->layouts.clear();
	this->dirtyWindows.clear();
	this->dirtyLayouts.clear();
	this->unverified.clear();
	this->dragging = nullptr;
	this->clearDrag();
	this->edgeDragState = EdgeDrag();
	this->syncTimer.stop();
	this->verifyTimer.stop();
	this->deadlineTimer.stop();
	this->snapTimer.stop();

	if (this->ownerMutex != nullptr) {
		ReleaseMutex(this->ownerMutex);
		CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
	}

	for (auto* window: tiled) this->notifyTilingChanged(window);

	this->refreshManagedCount();
	this->mTelemetry->reset();
}

void TilingManager::manage(TrackedWindow* window) {
	if (this->managed.contains(window)) return;

	auto& m = this->managed[window];
	m.window = window;
	m.placement.window = reinterpret_cast<quintptr>(window->hwnd());
	m.placement.lifetime = ++this->lifetimeCounter;
	m.elevated = isMoreElevated(window->hwnd());
	if (!this->starting) m.placeOn = focusedScreen();

	if (m.elevated) {
		qCDebug(logTiling) << "Window" << window->addressHex() << qs::windows::logId(window->appId())
		                   << "belongs to a more privileged process; it floats.";
	}

	auto mark = [this, window]() { this->markWindow(window); };
	QObject::connect(window, &TrackedWindow::minimizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::appCloakedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::maximizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::fullscreenChanged, this, mark);
	QObject::connect(window, &TrackedWindow::styleChanged, this, mark);
	// A new monitor or desktop gives an automatic float one fresh attempt,
	// bounded by the tile's budget. Floating keeps a window on its monitor
	// (restoreTiledWindow), so a float cannot cause this retry itself.
	auto retry = [this, window]() {
		auto iter = this->managed.find(window);
		if (iter != this->managed.end()) iter->second.autoFloat = false;
		this->markWindow(window);
	};
	QObject::connect(window, &TrackedWindow::desktopChanged, this, retry);
	QObject::connect(window, &TrackedWindow::screenChanged, this, retry);
	QObject::connect(window, &TrackedWindow::titleChanged, this, [this, window]() {
		if (this->scopeRules.hasTitleRules()) this->markWindow(window);
	});
	QObject::connect(window, &TrackedWindow::rectChanged, this, [this, window]() {
		this->onRectChanged(window);
	});

	this->markWindow(window);
	if (!this->starting) {
		// A first show must not wait for the normal coalesced layout pass.
		this->dirtyWindows.remove(window);
		this->syncWindow(window);
		if (m.layout != nullptr) this->apply(m.layout);
	}
}

void TilingManager::unmanage(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto* from = iter->second.layout;
	auto refocus = from != nullptr && from->lastFocused == window
	            && TilingManager::scrollingOf(from) != nullptr;

	QObject::disconnect(window, nullptr, this, nullptr);
	if (iter->second.verifying || iter->second.snapping || iter->second.settleSince != 0) {
		this->reportPlacement(iter->second, PlacementDisposition::Vanished, {}, ERROR_INVALID_WINDOW_HANDLE);
	}
	this->takeOut(iter->second);
	if (iter->second.offscreen) forgetParked(window->hwnd());
	this->managed.erase(iter);

	this->dirtyWindows.remove(window);
	this->unverified.remove(window);
	if (this->dragging == window) {
		this->dragging = nullptr;
		this->clearDrag();
	} else if (this->drag.active && this->drag.plan.target == window) {
		// The drag goes on, home until the cursor moves to a fresh target.
		this->drag.plan = DropPlan();
		this->drag.active = false;
		this->invalidateDragTarget();
		emit this->dragChanged();
	}

	for (auto& [key, layout]: this->layouts) {
		if (layout->lastFocused == window) layout->lastFocused = nullptr;
	}

	// The closed window freed capacity in its layout. Windows that floated
	// automatically out of that layout get one fresh attempt.
	if (from != nullptr) {
		for (auto& [other, m]: this->managed) {
			if (!m.autoFloat || m.autoFloatScreen != from->screen
			    || !IsEqualGUID(m.autoFloatDesktop, from->desktop))
			{
				continue;
			}
			m.autoFloat = false;
			this->markWindow(other);
		}
	}

	if (refocus) this->refocus(from);
}

TilingManager::State TilingManager::stateOf(const Managed& m) const {
	auto* window = m.window;

	if (window->desktop() < 0 || window->screen() == nullptr) return State::Out;
	if (!QGuiApplication::screens().contains(window->screen())) return State::Out;
	if (m.elevated) return State::Out;
	if (this->outsideScope(m)) return State::Out;
	if (m.override == Override::Float) return State::Out;
	// Exclusions are configuration, not a transient fullscreen style. They
	// apply before fullscreen suspension, keeping the legacy explicit-tile override.
	if (m.override != Override::Tile && this->scopeRules.matchesExcluded(this->factsOf(m))) {
		return State::Out;
	}

	// A minimized or app-cloaked window keeps its slot but must not reserve
	// space. App cloaking preserves the fullscreen episode, so test visibility
	// before the fullscreen suspension. A shell cloak is another virtual
	// desktop and keeps the normal tile.
	if (window->hidden()) return State::Hidden;

	// Give an application that just regained its frame a chance to restore
	// its own rectangle. Geometry-confirmed exits need no extra delay. The
	// bounded fallback still recovers a fullscreen-era normal placement.
	if (window->fullscreen() && this->exitedFullscreen(m)
	    && m.fullscreenExitNotBefore > tilingNow())
	{
		return State::Suspended;
	}

	// Browsers can drop WS_THICKFRAME or become topmost for video fullscreen.
	// Suspend before applying floating style rules: restoring the pre-tiling
	// rectangle here would resize the browser and cancel its fullscreen.
	if (window->fullscreen() && !this->exitedFullscreen(m)) {
		if (m.layout == nullptr || m.target.isNull() || currentFrame(window->hwnd()) != m.target) {
			return State::Suspended;
		}
	}

	// A maximized window keeps its slot. Suspend before the same floating
	// style rules: a browser can add WS_EX_NOACTIVATE or WS_EX_TOPMOST while
	// maximizing, and restoring the pre-tiling rectangle would cancel that.
	if (window->maximized()) return State::Suspended;

	// Some browsers briefly expose a floating style after their fullscreen
	// geometry has already ended. Keep their existing tile until the bounded
	// style recheck; explicit scope/float decisions above still take priority.
	if (m.layout != nullptr && !m.autoFloat && m.fullscreenRecoveryUntil > tilingNow()
	    && this->floatsByRule(m))
	{
		return State::Suspended;
	}

	// A borderless or topmost window that fills the work area is in a
	// fullscreen-style transition the tracker cannot classify yet: it may have
	// adopted a smaller display mode, or the monitor may already report the
	// new mode. Floating it would restore the pre-tiling rectangle over it.
	if (this->fullscreenLike(m)) return State::Suspended;

	if (m.override != Override::Tile && (m.autoFloat || this->floatsByRule(m))) return State::Out;

	return State::Tiled;
}

bool TilingManager::fullscreenLike(const Managed& m) const {
	if (m.layout == nullptr || m.window->hidden() || m.window->maximized())
	{
		return false;
	}

	// Only windows that already look like a fullscreen application are
	// candidates; ordinary tiled windows keep their normal floating rules.
	auto facts = this->factsOf(m);
	if (!facts.topmost && !facts.noActivate && !facts.toolWindow && facts.hasThickFrame) return false;

	auto rects = monitorRects(monitorForScreen(m.window->screen()));
	if (!rects.valid) return false;

	auto frame = currentFrame(m.window->hwnd());
	// An explicit tile already at its slot is tiled, not fullscreen.
	if (m.override == Override::Tile && !m.target.isNull() && frame == m.target) return false;
	// The work area is the fallback for a window that sized itself inside the
	// reserved frame or adopted a display mode smaller than the monitor.
	// Even an explicit tile must not resize a monitor-covering fullscreen
	// re-entry whose tracker notification is still queued. The broader work
	// area fallback remains an automatic-tiling policy.
	return fullscreen_geometry::covers(frame, rects.monitor)
	    || (m.override != Override::Tile && fullscreen_geometry::covers(frame, rects.work));
}

bool TilingManager::exitedFullscreen(const Managed& m) const {
	if (!m.fullscreenStyleValid || m.window->hidden()) return false;
	if (!m.window->fullscreen()) return true;

	// Only bits the fullscreen window did not have count. An application that
	// keeps WS_CAPTION or WS_THICKFRAME while fullscreen falls back to the
	// geometry check, which is exactly what the recorded style then masks out.
	auto style = GetWindowLongPtrW(m.window->hwnd(), GWL_STYLE);
	return ((style & FULLSCREEN_FRAME_STYLE) & ~m.fullscreenStyle) != 0;
}

tiling::WindowFacts TilingManager::factsOf(const Managed& m) const {
	auto* hwnd = m.window->hwnd();

	auto* owner = GetWindow(hwnd, GW_OWNER);
	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	tiling::WindowFacts facts;
	facts.appId = m.window->appId();
	facts.exePath = m.window->exePath();
	facts.title = m.window->title();
	facts.className = windowClass(hwnd);
	facts.screenName = screenName(m.window->screen());
	facts.ownProcess = isOwnProcessWindow(hwnd);
	facts.elevated = m.elevated;
	facts.owned = owner != nullptr && IsWindowVisible(owner);
	facts.toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
	facts.topmost = (exStyle & WS_EX_TOPMOST) != 0;
	facts.noActivate = (exStyle & WS_EX_NOACTIVATE) != 0;
	facts.dialogFrame = (exStyle & WS_EX_DLGMODALFRAME) != 0;
	facts.hasThickFrame = (style & WS_THICKFRAME) != 0;

	return facts;
}

bool TilingManager::outsideScope(const Managed& m) const {
	if (isOwnProcessWindow(m.window->hwnd())) return true;
	if (!this->scopeRules.hasIncluded()) return false;
	auto facts = this->factsOf(m);
	// Explicit tiling retains legacy style/exclusion overrides when no staged
	// allowlist is active. In staged mode exclusions always win.
	return !this->scopeRules.matchesIncluded(facts) || this->scopeRules.matchesExcluded(facts);
}

bool TilingManager::floatsByRule(const Managed& m) const {
	return this->scopeRules.floatReason(this->factsOf(m)) != tiling::FloatReason::None;
}

tiling::LayoutEngine::Id TilingManager::idOf(TrackedWindow* window) {
	return reinterpret_cast<tiling::LayoutEngine::Id>(window);
}

TrackedWindow* TilingManager::windowOf(tiling::LayoutEngine::Id id) const {
	auto* window = reinterpret_cast<TrackedWindow*>(id); // NOLINT(performance-no-int-to-ptr)
	return this->managed.contains(window) ? window : nullptr;
}

TilingManager::Layout* TilingManager::layoutFor(TrackedWindow* window, bool create) {
	auto index = window->desktop();
	if (index < 0 || index >= this->desktops->count()) return nullptr;
	return this->layoutAt(this->desktops->desktops().at(index).id, window->screen(), create);
}

TilingManager::Layout* TilingManager::layoutAt(const GUID& desktop, QScreen* screen, bool create) {
	if (screen == nullptr || !QGuiApplication::screens().contains(screen)) return nullptr;

	// Layout ownership follows the live QScreen. A friendly-name correction
	// must not strand an existing layout under an obsolete name.
	auto key = VirtualDesktops::guidToString(desktop) + '|'
	         + QString::number(reinterpret_cast<quintptr>(screen), 16);
	auto iter = this->layouts.find(key);
	if (iter != this->layouts.end()) return iter->second.get();
	if (!create) return nullptr;

	auto layout = std::make_unique<Layout>();
	layout->desktop = desktop;
	layout->screen = screen;
	layout->tree = tiling::LayoutEngine::create(this->layoutKind());
	layout->tree->configure(this->options());
	this->prepareScrolling(layout.get());
	auto rects = monitorRects(monitorForScreen(screen));
	if (rects.valid) layout->tree->compute(rects.work);

	auto* raw = layout.get();
	this->layouts.emplace(key, std::move(layout));
	return raw;
}

bool TilingManager::layoutAlive(const Layout* layout) const {
	if (layout->screen == nullptr) return false;
	if (!QGuiApplication::screens().contains(layout->screen.data())) return false;
	return this->desktops->indexOf(layout->desktop) != -1;
}

void TilingManager::markWindow(TrackedWindow* window) {
	if (!this->mEnabled) return;
	this->dirtyWindows.insert(window);
	this->schedule();
}

void TilingManager::markLayout(Layout* layout) {
	this->dropGeneration++;
	if (!this->mEnabled || layout == nullptr) return;
	this->dirtyLayouts.insert(layout);
	this->schedule();
}

void TilingManager::schedule() {
	if (!this->syncTimer.isActive()) this->syncTimer.start();
}

void TilingManager::sync() {
	if (!this->mEnabled) return;
	this->dropGeneration++;

	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		auto* layout = iter->second.get();
		if (this->layoutAlive(layout)) {
			++iter;
			continue;
		}

		for (auto id: layout->tree->ids()) {
			if (auto* window = this->windowOf(id)) {
				auto& m = this->managed.at(window);
				this->supersedePlacement(m);
				this->unpark(m);
				m.layout = nullptr;
				m.target = QRect();
				this->dirtyWindows.insert(window);
			}
		}

		this->dirtyLayouts.remove(layout);
		iter = this->layouts.erase(iter);
	}

	auto windows = std::move(this->dirtyWindows);
	this->dirtyWindows.clear();

	for (auto* window: this->tracker->windows()) {
		if (windows.contains(window)) this->syncWindow(window);
	}

	this->dropEmptyLayouts();

	auto layouts = std::move(this->dirtyLayouts);
	this->dirtyLayouts.clear();

	for (auto& [key, layout]: this->layouts) {
		if (layouts.contains(layout.get())) this->apply(layout.get());
	}

	// Every layout, geometry and state change lands here, so the preview
	// follows it instead of showing a frame the release would not place.
	if (this->dragging != nullptr && this->drag.active) this->replanDrag();

	this->refreshManagedCount();
	this->refreshBorderColours();
}

void TilingManager::refreshManagedCount() {
	auto count = 0;
	for (const auto& [window, m]: this->managed) {
		if (m.layout != nullptr) count++;
	}

	this->mTelemetry->setManagedCount(count);
}

void TilingManager::syncWindow(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	if (window == this->dragging) return;

	// Remember the style a window had while it was fullscreen. A window that
	// regains WS_CAPTION or WS_THICKFRAME has left fullscreen even when Windows
	// still reports the monitor-sized rectangle it saved as the normal
	// placement (a fullscreen exit that minimized before restoring its rect).
	// Keep the original snapshot until placement is verified or the slot is lost.
	if (window->fullscreen() && m.layout != nullptr && !m.fullscreenStyleValid
	    && (m.target.isNull() || currentFrame(window->hwnd()) != m.target))
	{
		m.fullscreenStyle = GetWindowLongPtrW(window->hwnd(), GWL_STYLE);
		m.fullscreenExStyle = GetWindowLongPtrW(window->hwnd(), GWL_EXSTYLE);
		m.fullscreenStyleValid = true;
	}

	// A minimized window reports fullscreen=false because IsIconic excludes it
	// from the tracker's geometry check. That is not evidence that the
	// application left fullscreen: a borderless application restored from the
	// taskbar is still fullscreen, and arming the post-exit grace here would
	// make the tiler place (resize) it. Keep the suspension until a refresh
	// with a visible window confirms the exit, by geometry or by the frame
	// style the window regained.
	if (m.fullscreenSuspended && this->exitedFullscreen(m)) {
		auto styleExit = window->fullscreen();
		qCDebug(logTiling) << "Confirmed fullscreen exit for" << window->addressHex()
		                   << qs::windows::logId(window->appId()) << (styleExit ? "by frame style" : "by geometry")
		                   << "style" << m.fullscreenStyle
		                   << GetWindowLongPtrW(window->hwnd(), GWL_STYLE) << "exstyle"
		                   << m.fullscreenExStyle
		                   << GetWindowLongPtrW(window->hwnd(), GWL_EXSTYLE);

		m.fullscreenSuspended = false;
		m.fullscreenRecoveryUntil = tilingNow() + FULLSCREEN_STYLE_GRACE_MS;
		m.fullscreenExitNotBefore = styleExit ? tilingNow() + FULLSCREEN_STYLE_RECHECK_MS : 0;
		// A geometry exit ends this episode's style evidence, so a re-entry
		// before placement snapshots its own style. A style exit keeps it: the
		// window can still be monitor-sized until Windows restores its rect.
		if (!styleExit) m.fullscreenStyleValid = false;

		QPointer<TrackedWindow> guarded(window);
		auto recheck = [this, guarded]() {
			if (guarded != nullptr && this->managed.contains(guarded.data())) {
				this->markWindow(guarded.data());
			}
		};
		// Precise: a coarse timer may fire before notBefore and defer to the grace recheck.
		QTimer::singleShot(FULLSCREEN_STYLE_RECHECK_MS, Qt::PreciseTimer, this, recheck);
		QTimer::singleShot(FULLSCREEN_STYLE_GRACE_MS, this, recheck);
	}

	auto state = this->stateOf(m);
	// Decide the monitor once, when the window is first looked at, even if it
	// cannot be tiled yet. A window that is maximized or hidden at launch
	// keeps the monitor Windows gave it when it later becomes tiled.
	auto* placeOn = m.placeOn.data();
	m.placeOn = nullptr;
	if (transition_trace::enabled() && m.traceState != static_cast<int>(state)) {
		const char* names[] = {"Out", "Hidden", "Suspended", "Tiled"};
		const char* reason = "eligible";
		if (state == State::Suspended) {
			if (window->fullscreen()) reason = "fullscreen";
			else if (window->maximized()) reason = "maximized";
			else if (this->fullscreenLike(m)) reason = "fullscreen_style";
			else reason = "fullscreen_style_grace";
		} else if (state == State::Hidden) {
			reason = window->appCloaked() ? "cloaked" : "minimized";
		} else if (state == State::Out) {
			if (window->desktop() < 0 || window->screen() == nullptr
			    || !QGuiApplication::screens().contains(window->screen())) reason = "desktop_or_screen";
			else if (m.elevated) reason = "elevated";
			else if (this->outsideScope(m)) reason = "outside_scope";
			else if (m.override == Override::Float) reason = "explicit_float";
			else if (m.override != Override::Tile && this->scopeRules.matchesExcluded(this->factsOf(m))) reason = "excluded";
			else reason = m.autoFloat ? "auto_float" : "float_rule";
		}
		QS_TRANSITION_TRACE("tiler_state", {{"window", transition_trace::handle(reinterpret_cast<quintptr>(window->hwnd()))},
		    {"state", names[static_cast<int>(state)]}, {"reason", reason}});
		m.traceState = static_cast<int>(state);
	}
	if (state == State::Suspended && window->fullscreen() && m.layout != nullptr
	    && !this->exitedFullscreen(m))
	{
		m.fullscreenSuspended = true;
	}
	if (state != State::Tiled) this->supersedePlacement(m);

	if (state == State::Out) {
		if (m.layout == nullptr) return;

		auto floated = (!window->hidden() || this->outsideScope(m))
		            && window->desktop() >= 0 && !m.elevated;
		this->takeOut(m);
		// Restoring the pre-tiling rectangle would cancel the app's fullscreen.
		if (window->fullscreen()) {
			m.preTiling = QRect();
			this->unpark(m);
		} else if (floated) {
			this->restoreFloating(m);
		} else {
			this->unpark(m);
		}
		return;
	}

	auto index = window->desktop();
	if (index >= this->desktops->count()) return;
	auto desktop = this->desktops->desktops().at(index).id;

	Layout* want = nullptr;
	auto* current = m.layout;
	if (current != nullptr && this->layoutAlive(current) && IsEqualGUID(current->desktop, desktop)) {
		want = current;

		if (state == State::Tiled && !m.verifying && !m.offscreen
		    && window->screen() != current->screen)
		{
			this->moveToLayout(m, this->layoutFor(window, true), nullptr);
			return;
		}
	} else if (state == State::Tiled) {
		// A window that appeared while the tiler was running opens on the
		// monitor the Hyprland compatibility layer publishes as focused, the
		// same one the shell uses, not wherever Windows first put it.
		want = this->layoutAt(desktop, placeOn != nullptr ? placeOn : window->screen(), true);
	}

	if (want == m.layout) {
		if (m.parked && state == State::Tiled) m.target = QRect();
		m.parked = state != State::Tiled;
		this->markLayout(m.layout);
		return;
	}

	if (m.layout != nullptr) this->takeOut(m);

	if (want != nullptr) {
		auto cursor = cursorPos();
		this->insertInto(m, want, nullptr, &cursor);
	}
}

void TilingManager::insertInto(
    Managed& m,
    Layout* layout,
    TrackedWindow* target,
    const QPoint* cursor
) {
	auto* window = m.window;

	if (m.preTiling.isNull()) {
		QRect raw;
		QRect frame;
		if (readFrame(window->hwnd(), raw, frame)) m.preTiling = raw;
	}

	if (target == nullptr || !layout->tree->contains(idOf(target))) target = layout->lastFocused;
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	auto targetId = target == nullptr ? tiling::LayoutEngine::Id(0) : idOf(target);

	layout->tree->configure(this->options());
	layout->tree->insert(idOf(window), targetId, cursor);

	if (auto* scroll = TilingManager::scrollingOf(layout)) {
		this->prepareScrolling(layout);
		auto focused = this->tracker->activeWindow() == window
		            || GetForegroundWindow() == window->hwnd();
		if (focused) scroll->activate(idOf(window));
	}

	m.layout = layout;
	m.parked = false;
	m.target = QRect();
	m.accepted = QRect();
	m.corrections = 0;

	qCDebug(logTiling) << "Tiling" << window->addressHex() << qs::windows::logId(window->appId()) << "on"
	                   << VirtualDesktops::guidToString(layout->desktop)
	                   << qs::windows::logId(screenName(layout->screen));

	this->markLayout(layout);
	this->refreshManagedCount();
	this->notifyTilingChanged(window);
}

void TilingManager::takeOut(Managed& m) {
	this->supersedePlacement(m);
	this->restoreBorderColour(m);
	auto* layout = m.layout;
	if (layout == nullptr) return;

	layout->tree->remove(idOf(m.window));
	if (layout->lastFocused == m.window) layout->lastFocused = nullptr;

	m.layout = nullptr;
	m.target = QRect();
	m.accepted = QRect();
	m.verifying = false;
	m.fullscreenStyleValid = false;
	m.fullscreenExitNotBefore = 0;
	this->unverified.remove(m.window);

	this->markLayout(layout);
	this->refreshManagedCount();
	this->notifyTilingChanged(m.window);
}

void TilingManager::notifyTilingChanged(TrackedWindow* window) {
	QPointer<TrackedWindow> guarded(window);
	QMetaObject::invokeMethod(this, [this, guarded]() {
		if (guarded) emit this->windowTilingChanged(guarded);
	}, Qt::QueuedConnection);
}

void TilingManager::dropEmptyLayouts() {
	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		if (iter->second->tree->isEmpty()) {
			this->dirtyLayouts.remove(iter->second.get());
			iter = this->layouts.erase(iter);
		} else {
			++iter;
		}
	}
}

QRect TilingManager::computeTree(tiling::LayoutEngine& tree, QScreen* screen) const {
	auto rects = monitorRects(monitorForScreen(screen));
	if (!rects.valid) return {};

	tree.configure(this->options());
	this->prepareScrolling(tree, screen);

	for (auto id: tree.ids()) {
		auto* window = this->windowOf(id);
		auto hidden = window != nullptr && this->stateOf(this->managed.at(window)) == State::Hidden;
		tree.setHidden(id, hidden);
	}

	tree.compute(rects.work);
	return rects.work;
}

void TilingManager::apply(Layout* layout) {
	if (!this->layoutAlive(layout)) return;

	auto work = this->computeTree(*layout->tree, layout->screen);
	if (work.isNull()) return;

	auto* scroll = TilingManager::scrollingOf(layout);
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());

	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr || window == this->dragging) continue;

		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		auto parked = scroll != nullptr && scroll->isParked(id);
		auto restore = QRect();
		if (parked) restore = clampInto(scroll->visibleBox(id).adjusted(in, in, -in, -in), work);
		this->markOffscreen(m, parked, restore);

		this->place(m, this->tileRect(layout, window));
	}
}

qint32 TilingManager::outerGap(const Layout* layout) const { return this->outerGap(*layout->tree); }

qint32 TilingManager::outerGap(const tiling::LayoutEngine& tree) const {
	auto count = 0;
	for (auto id: tree.ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr) continue;

		auto state = this->stateOf(this->managed.at(window));
		// A suspended window (fullscreen, maximized, or deferring a transient
		// style) still owns its slot. Counting it keeps the other windows from
		// reflowing to the single-window gaps and back again on exit.
		if (state == State::Tiled || state == State::Suspended) count++;
		if (count > 1) break;
	}
	return count == 1 ? this->mSingleWindowGapsOut : this->mGapsOut;
}

QRect TilingManager::tileRect(const Layout* layout, TrackedWindow* window) const {
	return this->tileRect(*layout->tree, layout->screen, idOf(window));
}

QRect TilingManager::tileRect(
    const tiling::LayoutEngine& tree,
    QScreen* screen,
    tiling::LayoutEngine::Id id
) const {
	auto box = tree.box(id);
	auto area = tree.area();
	auto dpr = screen->devicePixelRatio();
	auto in = toPhysical(this->mGapsIn, dpr);
	auto out = toPhysical(this->outerGap(tree), dpr);

	if (tree.kind() == tiling::Kind::Scrolling) {
		return QRect(
		    box.left() + in,
		    box.top() + in,
		    qMax(1, box.width() - 2 * in),
		    qMax(1, box.height() - 2 * in)
		);
	}

	auto left = box.left() == area.left() ? out : in;
	auto top = box.top() == area.top() ? out : in;
	auto right = rightOf(box) == rightOf(area) ? out : in;
	auto bottom = bottomOf(box) == bottomOf(area) ? out : in;

	return QRect(
	    box.left() + left,
	    box.top() + top,
	    qMax(1, box.width() - left - right),
	    qMax(1, box.height() - top - bottom)
	);
}

void TilingManager::place(Managed& m, const QRect& tile) {
	auto* hwnd = m.window->hwnd();
	if (!this->placementAlive(m)) return;
	// Re-evaluate just before placement: a fullscreen re-entry can revoke
	// style-based exit evidence while a layout is waiting to settle.
	if (this->stateOf(m) != State::Tiled) return;

	// Our own easing moves the frame; leave it alone unless the tile changed.
	if (m.snapping && tile == m.target) return;

	auto frame = currentFrame(hwnd);
	auto now = tilingNow();
	auto previous = std::exchange(m.observed, frame);
	if (previous.isValid() && frame != previous) m.changedAt = now;

	if (tile != m.target) {
		this->supersedePlacement(m);
		m.placement.layoutGeneration = ++this->layoutGeneration;
		m.disposition = PlacementDisposition::Idle;
		m.target = tile;
		m.accepted = QRect();
		m.corrections = 0;
		m.overflowClamped = false;
		m.snaps = 0;
		m.changedAt = 0;
		m.settleSince = 0;
		m.settleSpent = false;
		m.selfResized = false;

		if (landed(hwnd, frame, tile)) this->completePlacement(m, frame);
		else this->send(m, frame);
		return;
	}

	// Outside a user move/size session, tiled windows follow their layout.
	// A terminal outcome spends this tile's budget. Only a new tile or an
	// explicit relayout may buy another one, not a location notification.
	switch (m.disposition) {
	case PlacementDisposition::Idle:
	case PlacementDisposition::Accepted:
	case PlacementDisposition::Landed: break;
	case PlacementDisposition::SizeConstraint:
	case PlacementDisposition::CorrectionLimit:
	case PlacementDisposition::OffTarget:
	case PlacementDisposition::Deadline:
		// The window kept its slot. A late landing completes the placement
		// (a busy app ran the queued move); a later move away from where it
		// was left is judged again, with the budget it already spent.
		if (landed(hwnd, frame, tile)) {
			m.accepted = QRect();
			this->completePlacement(m, frame);
		} else if (frame != m.before && (m.accepted.isNull() || !landed(hwnd, frame, m.accepted))) {
			this->giveUp(m, frame);
		}
		return;
	default: return; // Refused, vanished and superseded requests stay final (ERM-609).
	}

	if (landed(hwnd, frame, tile)) {
		this->completePlacement(m, frame);
		return;
	}

	if (!m.accepted.isNull() && frame == m.accepted) return;
	if (m.verifying && frame == m.before) return;
	if (m.disposition == PlacementDisposition::Landed) {
		if (now - m.lastCorrection > CORRECTION_RESET_MS) {
			m.corrections = 0;
			m.snaps = 0;
		}
		m.placement.layoutGeneration = ++this->layoutGeneration;
		m.disposition = PlacementDisposition::Idle;
	}

	// A window that moves itself (a remote desktop client growing into
	// fullscreen) outruns the correction budget, and judging it mid-transition
	// floats it as min-size. While its frame is still changing, wait until it has
	// held still for a quiet interval: stateOf then sees where it ended up, and a
	// fixed size still gives up below. The wait is spent at most once per episode,
	// so a window that never stops changing falls back to corrections and give-up.
	auto changing = m.changedAt != 0 && now - m.changedAt < SETTLE_QUIET_MS;
	if (changing && !m.settleSpent) {
		if (m.settleSince == 0) m.settleSince = now;
		if (now - m.settleSince < SETTLE_LIMIT_MS) {
			qCDebug(logTiling) << "Waiting for" << m.window->addressHex() << qs::windows::logId(m.window->appId())
			                   << "to settle at" << frame << "(was" << previous << ")";
			m.selfResized = true;
			QPointer<TrackedWindow> guarded(m.window);
			auto identity = m.placement;
			QTimer::singleShot(SETTLE_POLL_MS, this, [this, guarded, identity]() {
				if (!guarded) return;
				auto iter = this->managed.find(guarded.data());
				if (iter != this->managed.end() && iter->second.placement == identity) {
					this->markWindow(guarded.data());
				}
			});
			return;
		}
		m.settleSpent = true;
	}
	m.settleSince = 0;

	if (m.selfResized && m.snaps < MAX_SNAPS) {
		m.snaps++;
		m.lastCorrection = now;
		qCDebug(logTiling) << "Easing" << m.window->addressHex() << qs::windows::logId(m.window->appId()) << "towards"
		                   << m.target << "from" << frame;
		this->startSnap(m, frame);
		return;
	}

	if (m.corrections >= MAX_CORRECTIONS) {
		this->giveUp(m, frame);
		return;
	}

	m.corrections++;
	m.lastCorrection = now;
	qCDebug(logTiling) << "Correcting" << m.window->addressHex() << qs::windows::logId(m.window->appId()) << "towards"
	                   << m.target << "from" << frame << "correction" << m.corrections << "of"
	                   << MAX_CORRECTIONS;
	this->send(m, frame);
}

// Ease a window that resized itself back into its tile, instead of snapping it
// there in one frame. The last step goes through send(), so landing is still
// verified and corrected like any other placement.
void TilingManager::startSnap(Managed& m, const QRect& frame) {
	m.before = frame;
	m.verifying = false;
	m.snapping = true;
	m.snapFrom = frame;
	m.snapStart = tilingNow();
	if (!this->snapTimer.isActive()) this->snapTimer.start();
}

void TilingManager::stepSnaps() {
	if (!this->dirtyLayouts.isEmpty() || !this->dirtyWindows.isEmpty()) this->sync();
	auto now = tilingNow();
	auto active = false;

	for (auto& [window, m]: this->managed) {
		if (!m.snapping) continue;
		if (!this->placementAlive(m)) continue;

		if (m.layout == nullptr || m.target.isNull() || window == this->dragging
		    || this->stateOf(m) != State::Tiled)
		{
			this->supersedePlacement(m);
			continue;
		}

		auto progress = qBound(0.0, static_cast<double>(now - m.snapStart) / SNAP_MS, 1.0);
		if (progress >= 1.0) {
			m.snapping = false;
			m.observed = m.target;
			m.changedAt = 0;
			this->send(m, currentFrame(window->hwnd()));
			continue;
		}

		auto eased = 1.0 - std::pow(1.0 - progress, 3.0);
		auto mix = [&](int from, int to) { return from + qRound((to - from) * eased); };
		auto error = setFrame(
		    window->hwnd(),
		    QRect(
		        mix(m.snapFrom.left(), m.target.left()),
		        mix(m.snapFrom.top(), m.target.top()),
		        qMax(1, mix(m.snapFrom.width(), m.target.width())),
		        qMax(1, mix(m.snapFrom.height(), m.target.height()))
		    )
		);
		if (error) {
			auto disposition = !IsWindow(window->hwnd()) ? PlacementDisposition::Vanished
			                 : *error == ERROR_ACCESS_DENIED ? PlacementDisposition::Denied
			                                               : PlacementDisposition::Failed;
			this->reportPlacement(m, disposition,
			                      currentFrame(window->hwnd()), *error);
			continue;
		}
		active = true;
	}

	if (!active) this->snapTimer.stop();
}

void TilingManager::send(Managed& m, const QRect& frame) {
	if (!this->placementAlive(m)) return;
	m.before = frame;
	QS_TRANSITION_TRACE("placement_request", {{"window", transition_trace::handle(reinterpret_cast<quintptr>(m.window->hwnd()))},
	    {"before", transition_trace::rect(frame)}, {"target", transition_trace::rect(m.target)}});
	auto error = setFrame(m.window->hwnd(), m.target);
	if (error) {
		auto disposition = !IsWindow(m.window->hwnd()) ? PlacementDisposition::Vanished
		                 : *error == ERROR_ACCESS_DENIED ? PlacementDisposition::Denied
		                                               : PlacementDisposition::Failed;
		this->reportPlacement(m, disposition, frame, *error);
		return;
	}
	m.verifying = true;
	m.deadline = tilingNow() + DEADLINE_MS;
	this->reportPlacement(m, PlacementDisposition::Accepted, frame);
	this->armDeadline();
}

void TilingManager::completePlacement(Managed& m, const QRect& frame) {
	if (!landed(m.window->hwnd(), frame, m.target)) return;
	m.verifying = false;
	m.changedAt = 0;
	m.settleSince = 0;
	m.settleSpent = false;
	m.selfResized = false;
	m.observed = frame;
	m.fullscreenStyleValid = false;
	m.fullscreenSuspended = false;
	m.fullscreenExitNotBefore = 0;
	if (m.disposition == PlacementDisposition::Landed) return;
	this->reportPlacement(m, PlacementDisposition::Landed, frame);
	QS_TRANSITION_TRACE("placement_verified", {{"window", transition_trace::handle(reinterpret_cast<quintptr>(m.window->hwnd()))},
	    {"frame", transition_trace::rect(frame)}, {"target", transition_trace::rect(m.target)},
	    {"landed", landed(m.window->hwnd(), frame, m.target)}});
	qCDebug(logTiling) << "Completed placement" << m.window->addressHex() << "final frame" << frame
	                   << "after" << m.corrections << "corrections";
}

void TilingManager::reportPlacement(
    Managed& m, PlacementDisposition disposition, const QRect& frame, quint32 nativeError
) {
	if (m.disposition == disposition && disposition != PlacementDisposition::Accepted) return;
	m.disposition = disposition;
	if (disposition != PlacementDisposition::Accepted) {
		m.verifying = false;
		m.snapping = false;
		m.settleSince = 0;
		this->unverified.remove(m.window);
	}

	auto outcome = tiling::PlacementOutcome {m.placement, disposition, m.target, frame, nativeError};
	this->mTelemetry->notePlacement(outcome);
	QS_TRANSITION_TRACE("placement_outcome", {
	    {"window", transition_trace::handle(m.placement.window)},
	    {"lifetime", QString::number(m.placement.lifetime)},
	    {"generation", QString::number(m.placement.layoutGeneration)},
	    {"disposition", tiling::placementDispositionName(disposition)},
	    {"target", transition_trace::rect(m.target)}, {"frame", transition_trace::rect(frame)},
	    {"native_error", qint64(nativeError)}});

	const char* reason = nullptr;
	switch (disposition) {
	case PlacementDisposition::Denied: reason = "placement-denied"; break;
	case PlacementDisposition::Failed: reason = "placement-failed"; break;
	case PlacementDisposition::SizeConstraint: reason = "min-size"; break;
	case PlacementDisposition::OffTarget: reason = "off-target"; break;
	case PlacementDisposition::CorrectionLimit: reason = "correction-limit"; break;
	case PlacementDisposition::Vanished: reason = "target-vanished"; break;
	case PlacementDisposition::Deadline: reason = "unresponsive"; break;
	case PlacementDisposition::Idle:
	case PlacementDisposition::Accepted:
	case PlacementDisposition::Landed:
	case PlacementDisposition::Superseded: break;
	}
	if (reason == nullptr) return;
	qCInfo(logTiling) << "Placement of" << m.window->addressHex()
	                  << "ended with" << reason << "native error" << nativeError;
	this->mTelemetry->noteFailure(m.window->addressHex(), QString::fromLatin1(reason));
	if ((disposition == PlacementDisposition::Denied || disposition == PlacementDisposition::Failed)
	    && !m.offscreen)
	{
		// The user's explicit tile keeps its slot even when Windows refuses it.
		if (m.override == Override::Tile) {
			m.accepted = frame;
			return;
		}
		this->autoFloatWindow(m);
		this->markWindow(m.window);
	}
}

void TilingManager::supersedePlacement(Managed& m) {
	if (m.verifying || m.snapping || m.settleSince != 0) {
		this->reportPlacement(m, PlacementDisposition::Superseded);
	}
	m.verifying = false;
	m.snapping = false;
	m.settleSince = 0;
	this->unverified.remove(m.window);
}

bool TilingManager::placementAlive(Managed& m) {
	if (IsWindow(m.window->hwnd())) return true;
	if (m.verifying || m.snapping || m.settleSince != 0
	    || (m.disposition == PlacementDisposition::Idle && m.target.isValid())) {
		this->reportPlacement(m, PlacementDisposition::Vanished, {}, ERROR_INVALID_WINDOW_HANDLE);
	}
	return false;
}

void TilingManager::armDeadline() {
	auto next = std::numeric_limits<qint64>::max();
	for (auto& [window, m]: this->managed) {
		if (!m.verifying) continue;
		if (!this->placementAlive(m)) continue;
		if (m.layout == nullptr || m.target.isNull() || window == this->dragging
		    || this->stateOf(m) != State::Tiled)
		{
			this->supersedePlacement(m);
			continue;
		}
		next = qMin(next, m.deadline);
	}

	if (next == std::numeric_limits<qint64>::max()) {
		this->deadlineTimer.stop();
		return;
	}

	auto remaining = next - tilingNow();
	remaining = qBound(qint64(0), remaining, qint64(DEADLINE_MS));
	this->deadlineTimer.start(static_cast<int>(remaining));
}

void TilingManager::restoreFloating(Managed& m) {
	auto* hwnd = m.window->hwnd();
	auto previous = m.preTiling;
	m.preTiling = QRect();

	auto parkedRaw = m.offscreen ? m.restore : QRect();
	if (m.offscreen) {
		forgetParked(hwnd);
		m.offscreen = false;
		m.restore = QRect();
	}

	Win32WindowOps ops;
	tiling::restoreTiledWindow(
	    ops,
	    reinterpret_cast<quintptr>(hwnd), // NOLINT(performance-no-int-to-ptr)
	    previous,
	    parkedRaw
	);
}

tiling::ScrollingLayout* TilingManager::scrollingOf(const Layout* layout) {
	if (layout == nullptr || layout->tree == nullptr) return nullptr;
	if (layout->tree->kind() != tiling::Kind::Scrolling) return nullptr;
	return static_cast<tiling::ScrollingLayout*>(layout->tree.get());
}

void TilingManager::prepareScrolling(Layout* layout) const {
	if (layout == nullptr || layout->tree == nullptr) return;
	this->prepareScrolling(*layout->tree, layout->screen);
}

void TilingManager::prepareScrolling(tiling::LayoutEngine& tree, QScreen* screen) const {
	if (tree.kind() != tiling::Kind::Scrolling || screen == nullptr) return;
	auto& scroll = static_cast<tiling::ScrollingLayout&>(tree);

	auto dpr = screen->devicePixelRatio();
	scroll.setGaps(toPhysical(this->mGapsIn, dpr), toPhysical(this->outerGap(tree), dpr));

	QList<QRect> others;
	for (auto* other: QGuiApplication::screens()) {
		if (other == screen) continue;
		auto rect = physicalMonitor(other);
		if (rect.isValid()) others.append(rect);
	}

	scroll.setMonitors(physicalMonitor(screen), others);
}

tiling::ScrollingLayout* TilingManager::scrollingFor(TrackedWindow* window, Layout** layout) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return nullptr;

	auto& m = iter->second;
	auto* scroll = TilingManager::scrollingOf(m.layout);
	if (scroll == nullptr || this->stateOf(m) != State::Tiled) return nullptr;

	*layout = m.layout;
	this->prepareScrolling(m.layout);
	if (scroll->active() != idOf(window)) scroll->activate(idOf(window));
	return scroll;
}

void TilingManager::settle(Layout* layout) {
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr) return;

	this->prepareScrolling(layout);
	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (rects.valid) scroll->compute(rects.work);
	this->markLayout(layout);

	auto* window = this->windowOf(scroll->active());
	if (window == nullptr) return;

	auto* foreground = this->tracker->windowFor(GetForegroundWindow());
	if (window == foreground) return;
	if (foreground == nullptr && window == this->tracker->activeWindow()) return;
	if (this->stateOf(this->managed.at(window)) == State::Out) return;

	layout->lastFocused = window;
	window->activate();
}

bool TilingManager::focusIdleStrip() {
	if (this->tracker == nullptr || this->desktops == nullptr) return false;
	if (this->tracker->windowFor(GetForegroundWindow()) != nullptr) return false;

	auto* active = this->tracker->activeWindow();
	if (active != nullptr && this->managed.contains(active)) return false;

	auto current = this->desktops->currentIndex();
	if (current < 0 || current >= this->desktops->count()) return false;

	auto point = POINT {};
	GetCursorPos(&point);
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
	if (screen == nullptr) screen = focusedScreen();

	auto* layout = this->layoutAt(this->desktops->desktops().at(current).id, screen, false);
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr) return false;

	auto* window = this->windowOf(scroll->active());
	if (window == nullptr || this->stateOf(this->managed.at(window)) != State::Tiled) return false;

	qCDebug(logTiling) << "No window has the focus; focusing the strip's column" << window->addressHex();
	this->settle(layout);
	return true;
}

void TilingManager::refocus(Layout* layout) {
	if (!this->layoutAlive(layout)) return;

	auto current = this->desktops->currentIndex();
	if (current < 0 || current >= this->desktops->count()) return;
	if (!IsEqualGUID(this->desktops->desktops().at(current).id, layout->desktop)) return;

	if (auto* active = this->tracker->activeWindow()) {
		auto iter = this->managed.find(active);
		if (iter == this->managed.end() || iter->second.layout != layout) return;
	}

	this->settle(layout);
}

void TilingManager::markOffscreen(Managed& m, bool offscreen, const QRect& restore) {
	auto* hwnd = m.window->hwnd();

	if (!offscreen) {
		if (!m.offscreen) return;
		m.offscreen = false;
		m.restore = QRect();
		forgetParked(hwnd);
		return;
	}

	auto raw = restore;
	QRect current;
	QRect frame;
	if (readFrame(hwnd, current, frame)) {
		raw = restore.adjusted(
		    current.left() - frame.left(),
		    current.top() - frame.top(),
		    rightOf(current) - rightOf(frame),
		    bottomOf(current) - bottomOf(frame)
		);
	}

	if (m.offscreen && raw == m.restore) return;
	m.offscreen = true;
	m.restore = raw;
	rememberParked(hwnd, raw);
}

void TilingManager::unpark(Managed& m) {
	if (!m.offscreen) return;

	auto* hwnd = m.window->hwnd();
	auto restore = m.restore;
	m.offscreen = false;
	m.restore = QRect();
	m.target = QRect();
	forgetParked(hwnd);

	if (restore.isNull() || !IsWindow(hwnd)) return;

	if (IsIconic(hwnd) || IsZoomed(hwnd)) restorePlacement(hwnd, onScreen(restore));
	else setRawRect(hwnd, onScreen(restore));
}

void TilingManager::unparkAll() {
	for (auto& [window, m]: this->managed) this->unpark(m);
}

void TilingManager::restoreForCrash() {
	auto atom = gParkAtom.load();

	for (auto& slot: gParkSlots) {
		auto* hwnd = slot.hwnd.exchange(nullptr);
		if (hwnd == nullptr || !IsWindow(hwnd) || IsIconic(hwnd)) continue;

		auto rect = unpackRect(slot.rect.load());
		setRawRect(hwnd, rect);

		if (atom != 0) RemovePropW(hwnd, atomName(atom));
	}
}

void TilingManager::rescueParked() {
	auto* owner = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\QuickshellTiling");
	if (owner != nullptr) {
		CloseHandle(owner);
		return;
	}

	auto atom = GlobalFindAtomW(L"QuickshellTilingParked");
	if (atom == 0) return;
	gParkAtom.store(atom);

	QList<HWND> windows;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    reinterpret_cast<QList<HWND>*>(param)->append(hwnd); // NOLINT(performance-no-int-to-ptr)
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&windows)
	);

	auto rescued = 0;
	for (auto* hwnd: windows) {
		auto* value = GetPropW(hwnd, atomName(atom));
		if (value == nullptr) continue;

		RemovePropW(hwnd, atomName(atom));
		auto rect = onScreen(unpackRect(static_cast<quint64>(reinterpret_cast<quintptr>(value))));

		if (IsIconic(hwnd) || IsZoomed(hwnd)) {
			WINDOWPLACEMENT placement {};
			placement.length = sizeof(placement);
			if (!GetWindowPlacement(hwnd, &placement)) continue;
			auto normal = toQRect(placement.rcNormalPosition);
			normal.translate(placementOffset(hwnd, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)));
			auto native = toRECT(normal);
			if (MonitorFromRect(&native, MONITOR_DEFAULTTONULL) != nullptr) continue;
			placement.rcNormalPosition = screenToPlacement(hwnd, rect);
			SetWindowPlacement(hwnd, &placement);
			rescued++;
			continue;
		}

		if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr) continue;
		setRawRect(hwnd, rect);
		rescued++;
	}

	if (rescued > 0) {
		qCInfo(logTiling) << "Brought back" << rescued
		                  << "windows that a previous run left off screen.";
	}
}

void TilingManager::onScreensChanged() {
#ifndef QS_TILING_FAKE_DESKTOP
	QObject::connect(ScreenIdentityRegistry::instance(), &ScreenIdentityRegistry::screenChanged,
	                 this, &TilingManager::markAll, Qt::UniqueConnection);
#endif
	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
		auto relayoutAll = [this]() {
			for (auto& [key, layout]: this->layouts) this->markLayout(layout.get());
		};
		QObject::connect(screen, &QScreen::availableGeometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::geometryChanged, this, [this]() { this->markAll(); });
		QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this]() { this->markAll(); });
	}

	this->markAll();
}

void TilingManager::onActiveWindowChanged() {
	this->refreshBorderColours();
	auto* window = this->tracker->activeWindow();
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto& m = iter->second;
	m.focusStamp = ++this->focusCounter;
	if (m.layout == nullptr) return;

	m.layout->lastFocused = window;

	if (auto* scroll = TilingManager::scrollingOf(m.layout)) {
		this->prepareScrolling(m.layout);
		if (scroll->activate(idOf(window))) this->markLayout(m.layout);
	}
}

void TilingManager::onRectChanged(TrackedWindow* window) {
	if (window == this->dragging) return;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	const auto& m = iter->second;
	if (m.layout == nullptr || m.target.isNull()) return;

	this->unverified.insert(window, m.placement);
	if (!this->verifyTimer.isActive()) this->verifyTimer.start();
}

void TilingManager::verify() {
	auto pending = std::move(this->unverified);
	this->unverified.clear();
	// Apply a queued layout change before interpreting its old observations.
	if (!this->dirtyLayouts.isEmpty() || !this->dirtyWindows.isEmpty()) this->sync();

	for (auto iterPending = pending.cbegin(); iterPending != pending.cend(); ++iterPending) {
		auto* window = iterPending.key();
		auto iter = this->managed.find(window);
		if (iter == this->managed.end() || window == this->dragging) continue;

		auto& m = iter->second;
		if (m.placement != iterPending.value() || !this->placementAlive(m)) continue;
		if (m.layout == nullptr || m.target.isNull()) continue;
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, m.target);
	}
}

void TilingManager::giveUp(Managed& m, const QRect& frame) {
	m.verifying = false;

	// A larger frame is judged by its origin, within the landing tolerance,
	// against where it was accepted after its overflow was moved, or its slot.
	auto anchor = m.accepted.isNull() ? m.target.topLeft() : m.accepted.topLeft();
	auto tolerance = qMax(EDGE_TOLERANCE, landingTolerance(m.window->hwnd()));
	auto outcome = tiling::classifyGiveUp({m.target, frame, m.offscreen, tolerance, anchor});
	auto disposition = PlacementDisposition::CorrectionLimit;
	switch (outcome.reason) {
	case tiling::FailureReason::MinSize: disposition = PlacementDisposition::SizeConstraint; break;
	case tiling::FailureReason::OffTarget: disposition = PlacementDisposition::OffTarget; break;
	case tiling::FailureReason::None:
		if (landed(m.window->hwnd(), frame, m.target)) {
			this->completePlacement(m, frame);
			return;
		}
		break;
	case tiling::FailureReason::CorrectionLimit:
	case tiling::FailureReason::Unresponsive: break;
	}
	this->reportPlacement(m, disposition, frame);

	// The user's explicit tile wins over an automatic float.
	if (outcome.autoFloat && m.override != Override::Tile) {
		qCInfo(logTiling) << "Window" << m.window->addressHex() << "floats.";
		this->autoFloatWindow(m);
		this->markWindow(m.window);
		return;
	}

	m.accepted = frame;
	if (disposition != PlacementDisposition::SizeConstraint || m.overflowClamped
	    || m.layout == nullptr || m.layout->screen == nullptr)
	{
		return;
	}

	// A window that keeps a larger frame stays anchored at its slot, as in
	// Hyprland, but is moved back inside the work area once per tile. One
	// larger than the work area starts at its leading edge and overflows past
	// the far one: an accepted platform limit (see tiling::fitOverflow).
	m.overflowClamped = true;
	auto rects = monitorRects(monitorForScreen(m.layout->screen));
	if (!rects.valid) return;
	auto fit = tiling::fitOverflow(frame, rects.work, bitmapScale(m.window->hwnd()).scale);
	if (fit == frame || setFrame(m.window->hwnd(), fit)) return;
	m.before = frame;
	m.accepted = fit;
}

void TilingManager::onDeadline() {
	if (!this->dirtyLayouts.isEmpty() || !this->dirtyWindows.isEmpty()) this->sync();
	auto now = tilingNow();

	for (auto& [window, m]: this->managed) {
		if (!m.verifying) continue;
		if (!this->placementAlive(m)) continue;
		if (m.layout == nullptr || m.target.isNull() || window == this->dragging
		    || this->stateOf(m) != State::Tiled)
		{
			this->supersedePlacement(m);
			continue;
		}
		if (now < m.deadline) continue;

		auto* hwnd = window->hwnd();
		auto frame = currentFrame(hwnd);

		if (landed(hwnd, frame, m.target)) {
			this->completePlacement(m, frame);
			continue;
		}

		auto hung = IsHungAppWindow(hwnd);
		if (!hung && frame != m.before) {
			m.verifying = false;
			this->unverified.insert(window, m.placement);
			if (!this->verifyTimer.isActive()) this->verifyTimer.start();
			continue;
		}

		// Unchanged since the request but larger and already at the slot: it
		// cannot shrink. Unchanged anywhere else: it has not answered yet, and
		// must not be judged off target for the frame it had before tiling.
		auto bigger = frame.width() > m.target.width() + EDGE_TOLERANCE
		           || frame.height() > m.target.height() + EDGE_TOLERANCE;
		auto tolerance = qMax(EDGE_TOLERANCE, landingTolerance(hwnd));
		auto atSlot = qAbs(frame.left() - m.target.left()) <= tolerance
		           && qAbs(frame.top() - m.target.top()) <= tolerance;
		if (!hung && bigger && atSlot) {
			this->giveUp(m, frame);
			continue;
		}

		// Telemetry only: a busy or hung app keeps its slot, and place()
		// completes the placement if the queued move lands later.
		this->reportPlacement(m, PlacementDisposition::Deadline, frame);
	}

	this->armDeadline();
}

// The user's explicit float or tile is not touched: an explicit tile wins over
// an automatic float, which every caller checks first.
void TilingManager::autoFloatWindow(Managed& m) {
	m.autoFloat = true;
	m.autoFloatDesktop = m.layout != nullptr ? m.layout->desktop : GUID {};
	m.autoFloatScreen = m.layout != nullptr ? m.layout->screen : nullptr;
}

namespace {

// The physical rectangle `rect` as logical pixels local to `screen`.
QRectF screenLocal(const QRect& rect, QScreen* screen) {
	if (rect.isNull() || screen == nullptr) return {};
	auto origin = physicalMonitor(screen).topLeft();
	auto dpr = screen->devicePixelRatio();
	return {
	    (rect.x() - origin.x()) / dpr,
	    (rect.y() - origin.y()) / dpr,
	    rect.width() / dpr,
	    rect.height() / dpr
	};
}

} // namespace

QString TilingManager::dragMode() const {
	if (!this->drag.active) return {};
	switch (this->drag.plan.mode) {
	case DropMode::Home: return QStringLiteral("home");
	case DropMode::Swap: return QStringLiteral("swap");
	case DropMode::Insert: return QStringLiteral("insert");
	case DropMode::Move: return QStringLiteral("move");
	}
	return {};
}

QString TilingManager::dragScreen() const {
	return this->drag.active ? screenName(this->drag.plan.screen) : QString();
}

QRectF TilingManager::dragLanding() const {
	return this->drag.active ? screenLocal(this->drag.plan.landing, this->drag.plan.screen) : QRectF();
}

QRectF TilingManager::dragDisplaced() const {
	return this->drag.active ? screenLocal(this->drag.plan.displaced, this->drag.plan.screen) : QRectF();
}

// A title-bar drag and a Super drag drop by the same cursor rule, so the
// tracker's `super` flag no longer changes the plan.
void TilingManager::onMoveSizeStarted(TrackedWindow* window, [[maybe_unused]] bool super) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return;

	auto state = this->stateOf(iter->second);
	if (state != State::Tiled && !(state == State::Suspended && window->maximized())) return;

	this->dragging = window;
	this->clearDrag();
	this->drag.zoomed = IsZoomed(window->hwnd()) != FALSE;
	this->drag.frame = currentFrame(window->hwnd());
	this->drag.cursor = cursorPos();
	this->drag.threshold = QSize(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
	// Native and Super drags own geometry until their end notification. The
	// end policy moves split edges for a resize or commits the drop plan for
	// a move. Do not correct towards the pre-drag tile while the user drags.
	this->supersedePlacement(iter->second);
}

// Runs for every location change of the dragged window. Nothing here may
// allocate while the cursor stays inside the plan's zone.
void TilingManager::onDragMoved(TrackedWindow* window) {
	if (window != this->dragging || this->drag.kind == DragKind::Resize || this->drag.cancelled) return;

	if (this->drag.kind == DragKind::Unknown) {
		auto frame = currentFrame(window->hwnd());
		if (frame == this->drag.frame) return;
		auto resized = frame.size() != this->drag.frame.size() && !this->drag.zoomed;
		this->drag.kind = resized ? DragKind::Resize : DragKind::Move;
		if (resized) return;
	}

	auto cursor = cursorPos();
	if (!this->drag.active) {
		auto moved = cursor - this->drag.cursor;
		if (qAbs(moved.x()) < this->drag.threshold.width()
		    && qAbs(moved.y()) < this->drag.threshold.height())
		{
			return;
		}
	} else if (this->drag.plan.zone.contains(cursor)) {
		return;
	}

	// A location event can arrive after the plan was invalidated without the
	// cursor having moved: it reports the dragged window, not motion. Only
	// real motion past the drag threshold lifts the invalidation, or the
	// replacement tile under the still cursor would become the drop target.
	if (this->drag.invalidated) {
		auto moved = cursor - this->drag.invalidatedAt;
		if (qAbs(moved.x()) < this->drag.threshold.width()
		    && qAbs(moved.y()) < this->drag.threshold.height())
		{
			return;
		}
		this->drag.invalidated = false;
	}

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto previous = this->drag.active ? &this->drag.plan : nullptr;
	this->showDrag(this->planDrop(iter->second, cursor, previous));
}

void TilingManager::invalidateDragTarget() {
	this->drag.invalidated = true;
	this->drag.invalidatedAt = cursorPos();
}

// Publishes a plan, emitting only when what it shows changed.
void TilingManager::showDrag(const DropPlan& plan) {
	const auto& old = this->drag.plan;
	auto same = this->drag.active && plan.sameDrop(old) && plan.landing == old.landing
	         && plan.displaced == old.displaced;
	this->drag.plan = plan;
	this->drag.shown = true;
	if (same) return;
	this->drag.active = true;
	emit this->dragChanged();
}

// Re-plans after a layout change at the cursor sample the shown plan came
// from. The drop may change only by the user's own movement: a plan whose
// target was invalidated, or that would now be another drop (another target,
// a swap turned insert or the other way round), is home.
void TilingManager::replanDrag() {
	auto iter = this->managed.find(this->dragging);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	auto plan = this->homePlan(m);
	auto keep = !this->drag.invalidated && this->dropTargetValid(this->drag.plan);
	if (keep) {
		auto fresh = this->planDrop(m, this->drag.plan.cursor, &this->drag.plan);
		keep = fresh.sameDrop(this->drag.plan);
		if (keep) plan = fresh;
	}
	if (!keep) this->invalidateDragTarget();

	this->showDrag(plan);
}

// The planned target is still the same tiled window in the planned layout.
bool TilingManager::dropTargetValid(const DropPlan& plan) const {
	if (plan.target == nullptr) return true;
	auto iter = this->managed.find(plan.target);
	return iter != this->managed.end() && plan.layout != nullptr && iter->second.layout == plan.layout
	    && this->layoutAlive(plan.layout) && this->stateOf(iter->second) == State::Tiled;
}

TilingManager::DropPlan TilingManager::homePlan(const Managed& m) const {
	DropPlan plan;
	auto* from = m.layout;
	if (from == nullptr || from->screen == nullptr) return plan;
	plan.layout = from;
	plan.screen = from->screen;
	plan.zone = from->tree->box(idOf(m.window)).intersected(physicalMonitor(from->screen));
	return plan;
}

void TilingManager::cancelDrag(TrackedWindow* window) {
	if (window == nullptr || window != this->dragging) return;
	auto wasActive = this->drag.active;
	this->drag.cancelled = true;
	this->drag.active = false;
	this->drag.plan = DropPlan();
	if (wasActive) emit this->dragChanged();
}

void TilingManager::clearDrag() {
	auto wasActive = this->drag.active;
	this->drag = Drag();
	if (wasActive) emit this->dragChanged();
}

void TilingManager::onMoveSizeEnded(TrackedWindow* window) {
	if (window != this->dragging) return;
	this->dragging = nullptr;
	auto drag = this->drag;
	this->clearDrag();

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	auto* hwnd = window->hwnd();
	if (m.layout == nullptr || m.target.isNull() || IsIconic(hwnd) || IsZoomed(hwnd)) {
		this->markWindow(window);
		return;
	}

	auto frame = currentFrame(hwnd);
	const auto target = m.accepted.isNull() ? m.target : m.accepted;

	auto movedLeft = qAbs(frame.left() - target.left()) > EDGE_TOLERANCE;
	auto movedTop = qAbs(frame.top() - target.top()) > EDGE_TOLERANCE;
	auto movedRight = qAbs(rightOf(frame) - rightOf(target)) > EDGE_TOLERANCE;
	auto movedBottom = qAbs(bottomOf(frame) - bottomOf(target)) > EDGE_TOLERANCE;
	auto moved = int(movedLeft) + int(movedTop) + int(movedRight) + int(movedBottom);
	auto resized = qAbs(frame.width() - target.width()) > EDGE_TOLERANCE
	            || qAbs(frame.height() - target.height()) > EDGE_TOLERANCE;

	m.target = QRect();

	if (moved == 0 || drag.cancelled) {
		this->markLayout(m.layout);
		return;
	}

	// A move follows its plan whatever Aero Snap did to the frame meanwhile.
	// Without location events (window contents not shown while dragging) the
	// kind is unknown; a frame that kept its size or moved every edge moved.
	auto move = drag.kind == DragKind::Move
	         || (drag.kind == DragKind::Unknown && (!resized || moved == 4));
	if (move) {
		// The release commits the drop the preview shows, re-planned from the
		// current tree at the cursor sample it was planned from: the end event
		// arrives late, and the cursor may have moved on with no new plan shown.
		// If that is no longer the same drop, or its target was invalidated, the
		// drop is home; it is never replaced here. A drag that never showed a
		// plan (no location events) plans once, at the release cursor.
		auto plan = this->homePlan(m);
		this->dropGeneration++;
		if (drag.active && !drag.invalidated && this->dropTargetValid(drag.plan)) {
			auto fresh = this->planDrop(m, drag.plan.cursor, &drag.plan);
			if (fresh.sameDrop(drag.plan)) plan = fresh;
		} else if (!drag.shown && !drag.invalidated) {
			plan = this->planDrop(m, cursorPos(), nullptr);
		}
		this->commitDrop(m, plan);
		return;
	}

	auto* layout = m.layout;
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());
	auto id = idOf(window);

	if (movedLeft) layout->tree->moveEdge(id, Edge::Left, frame.left() - in);
	if (movedRight) layout->tree->moveEdge(id, Edge::Right, rightOf(frame) + in);
	if (movedTop) layout->tree->moveEdge(id, Edge::Top, frame.top() - in);
	if (movedBottom) layout->tree->moveEdge(id, Edge::Bottom, bottomOf(frame) + in);

	this->markLayout(layout);
}

// The cursor decides, for a title-bar and a Super drag alike. Over another
// tile, its centre swaps the two and, in dwindle, an edge band inserts on that
// side. Master and scrolling ignore the side, so they always swap. Another
// monitor's empty area moves the window there; anywhere else on its own
// monitor (its own slot, a gap, the bar) is home.
TilingManager::DropPlan
TilingManager::planDrop(const Managed& m, const QPoint& cursor, const DropPlan* previous) const {
	auto plan = this->homePlan(m);
	auto* from = m.layout;
	plan.cursor = cursor;
	if (plan.screen == nullptr) return plan;

	auto self = idOf(m.window);
	auto point = POINT {cursor.x(), cursor.y()};
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONULL));
	auto inflate = [](const QRect& rect, int by) { return rect.adjusted(-by, -by, by, by); };
	auto hysteresisOn = [](QScreen* on) {
		return toPhysical(DROP_HYSTERESIS_DP, on->devicePixelRatio());
	};

	// Tiles are hit where they are drawn, the frame tileRect() gives them; the
	// gaps between them are home. Keep the previous target while the cursor
	// stays within its frame plus the hysteresis, so a tile boundary does not
	// flicker.
	Layout* layout = nullptr;
	TrackedWindow* target = nullptr;
	QRect box;
	if (previous != nullptr && previous->target != nullptr) {
		auto iter = this->managed.find(previous->target);
		if (iter != this->managed.end() && iter->second.layout != nullptr
		    && iter->second.layout->screen == screen && this->stateOf(iter->second) == State::Tiled
		    && this->layoutAlive(iter->second.layout))
		{
			auto frame = this->tileRect(iter->second.layout, previous->target);
			if (inflate(frame, hysteresisOn(screen)).contains(cursor)) {
				layout = iter->second.layout;
				target = previous->target;
				box = frame;
			}
		}
	}

	if (target == nullptr && screen != nullptr) {
		for (const auto& [key, candidate]: this->layouts) {
			if (candidate->screen != screen || !IsEqualGUID(candidate->desktop, from->desktop)
			    || !this->layoutAlive(candidate.get()))
			{
				continue;
			}

			for (auto id: candidate->tree->ids()) {
				if (id == self) continue;
				auto* other = this->windowOf(id);
				if (other == nullptr || this->stateOf(this->managed.at(other)) != State::Tiled) continue;

				auto frame = this->tileRect(*candidate->tree, screen, id);
				if (!frame.contains(cursor)) continue;
				layout = candidate.get();
				target = other;
				box = frame;
				break;
			}
			if (target != nullptr) break;
		}
	}

	if (target != nullptr) {
		auto dpr = screen->devicePixelRatio();
		auto h = toPhysical(DROP_HYSTERESIS_DP, dpr);
		plan.layout = layout;
		plan.target = target;
		plan.screen = screen;
		plan.zone = inflate(box, h).intersected(physicalMonitor(screen));

		if (layout->tree->kind() == tiling::Kind::Dwindle) {
			auto cap = toPhysical(DROP_BAND_DP, dpr);
			const auto* kept = previous != nullptr && previous->target == target ? &previous->side : nullptr;
			plan.side = tiling::insertSide(box, cursor, cap, h, kept);

			// The cursor can go anywhere in the zone insertSide keeps: the band, or
			// the centre, widened by the hysteresis, short of the other bands.
			auto across = qMax(1, qMin(box.width() / 5, cap));
			auto down = qMax(1, qMin(box.height() / 5, cap));
			auto left = box.left();
			auto top = box.top();
			auto right = left + box.width() - 1;
			auto bottom = top + box.height() - 1;
			auto x0 = left + across - h;
			auto x1 = right - across + h;
			auto y0 = top + down - h;
			auto y1 = bottom - down + h;
			QRect zone(QPoint(x0, y0), QPoint(x1, y1));
			if (plan.side == Edge::Left) zone = QRect(QPoint(left - h, y0), QPoint(left + across + h - 1, y1));
			if (plan.side == Edge::Right) zone = QRect(QPoint(right - across - h + 1, y0), QPoint(right + h, y1));
			if (plan.side == Edge::Top) zone = QRect(QPoint(x0, top - h), QPoint(x1, top + down + h - 1));
			if (plan.side == Edge::Bottom) zone = QRect(QPoint(x0, bottom - down - h + 1), QPoint(x1, bottom + h));
			plan.zone &= zone;
		}

		plan.mode = plan.side ? DropMode::Insert : DropMode::Swap;
	} else if (screen != nullptr && screen != from->screen) {
		plan.mode = DropMode::Move;
		plan.screen = screen;
		plan.layout = nullptr;
		plan.zone = QRect();
		for (const auto& [key, candidate]: this->layouts) {
			if (candidate->screen == screen && IsEqualGUID(candidate->desktop, from->desktop)) {
				plan.layout = candidate.get();
			}
		}
		// With no tile there, the whole monitor moves the window.
		if (plan.layout == nullptr || plan.layout->tree->isEmpty()) plan.zone = physicalMonitor(screen);
	}

	// A previous plan's frames are reused only for the same drop on the same
	// trees; any layout change since bumped the generation.
	plan.generation = this->dropGeneration;
	auto unchanged = previous != nullptr && previous->generation == plan.generation
	              && previous->sameDrop(plan) && previous->layout == plan.layout;
	if (unchanged) {
		plan.landing = previous->landing;
		plan.displaced = previous->displaced;
	} else {
		this->simulateDrop(m, plan);
	}

	return plan;
}

// Runs commitDrop()'s tree operations on copies and computes them as sync()
// does, so the preview is the frame the release places.
void TilingManager::simulateDrop(const Managed& m, DropPlan& plan) const {
	if (plan.mode == DropMode::Home || plan.screen == nullptr) return;

	using Id = tiling::LayoutEngine::Id;
	auto* from = m.layout;
	auto self = idOf(m.window);
	auto targetId = plan.target != nullptr ? idOf(plan.target) : Id(0);
	auto same = plan.layout == from;

	auto source = from->tree->clone();
	std::unique_ptr<tiling::LayoutEngine> destination;
	if (!same) {
		destination = plan.layout != nullptr ? plan.layout->tree->clone()
		                                     : tiling::LayoutEngine::create(this->layoutKind());
	}

	// Each other tiled window's frame now, to tell which ones the drop moves.
	QHash<Id, QRect> before;
	auto record = [this, self, &before](tiling::LayoutEngine& tree, QScreen* screen) {
		this->computeTree(tree, screen);
		for (auto id: tree.ids()) {
			if (id != self) before.insert(id, this->tileRect(tree, screen, id));
		}
	};
	record(*source->clone(), from->screen);
	if (destination != nullptr && plan.layout != nullptr) record(*destination->clone(), plan.screen);
	if (destination != nullptr && plan.layout == nullptr) this->computeTree(*destination, plan.screen);

	auto& into = same ? *source : *destination;
	if (plan.mode == DropMode::Swap) {
		if (same) {
			source->swap(self, targetId);
		} else {
			source->replace(self, targetId);
			destination->replace(targetId, self);
		}
	} else {
		// moveToLayout(): a move goes beside the layout's last focused window.
		source->remove(self);
		auto anchor = targetId;
		if (plan.mode == DropMode::Move) {
			auto* last = plan.layout != nullptr ? plan.layout->lastFocused : nullptr;
			anchor = last != nullptr && !into.isHidden(idOf(last)) ? idOf(last) : Id(0);
		}
		into.configure(this->options());
		into.insert(self, anchor, nullptr, plan.side);
		if (into.kind() == tiling::Kind::Scrolling) {
			this->prepareScrolling(into, plan.screen);
			static_cast<tiling::ScrollingLayout&>(into).activate(self);
		}
	}

	this->computeTree(*source, from->screen);
	if (destination != nullptr) this->computeTree(*destination, plan.screen);
	plan.landing = this->tileRect(into, plan.screen, self);

	// The ghost: drawn only for dwindle swaps and inserts, when exactly one
	// other tile moves and it stays on this screen, since a partial picture of
	// a reflow would mislead. Master and scrolling reflow, so never there.
	auto moves = 0;
	QRect moved;
	QScreen* movedOn = nullptr;
	auto compare = [&](const tiling::LayoutEngine& tree, QScreen* screen) {
		for (auto id: tree.ids()) {
			auto* window = this->windowOf(id);
			if (id == self || window == nullptr) continue;
			if (this->stateOf(this->managed.at(window)) != State::Tiled) continue;
			auto rect = this->tileRect(tree, screen, id);
			if (before.value(id) == rect) continue;
			moves++;
			moved = rect;
			movedOn = screen;
		}
	};
	compare(*source, from->screen);
	if (destination != nullptr) compare(*destination, plan.screen);
	auto dwindle = into.kind() == tiling::Kind::Dwindle;
	if (dwindle && moves == 1 && movedOn == plan.screen) plan.displaced = moved;
}

void TilingManager::commitDrop(Managed& m, const DropPlan& plan) {
	switch (plan.mode) {
	case DropMode::Swap:
		if (auto iter = this->managed.find(plan.target); iter != this->managed.end()) {
			this->swapWindows(m, iter->second);
			return;
		}
		break;
	case DropMode::Insert:
		if (plan.layout != nullptr && plan.target != nullptr) {
			this->moveToLayout(m, plan.layout, plan.target, plan.side);
			return;
		}
		break;
	case DropMode::Move:
		if (m.layout != nullptr) {
			this->moveToLayout(m, this->layoutAt(m.layout->desktop, plan.screen, true), nullptr);
			return;
		}
		break;
	case DropMode::Home: break;
	}

	this->markLayout(m.layout);
}

TrackedWindow* TilingManager::focusedWindow() const {
	if (this->tracker == nullptr) return nullptr;

	auto* active = this->tracker->activeWindow();
	auto* hwnd = GetForegroundWindow();
	auto* foreground = this->tracker->windowFor(hwnd);
	if (foreground == nullptr || foreground == active) return active;

	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	if (cloaked != 0) return active;

	qCInfo(logTiling) << "The foreground window" << foreground->addressHex() << qs::windows::logId(foreground->appId())
	                  << "is not the tracked active window"
	                  << (active == nullptr ? QStringLiteral("(none)") : active->addressHex())
	                  << "; tiling commands use the foreground one.";
	this->tracker->refreshForeground();
	return foreground;
}

TilingManager::Managed* TilingManager::tiledActive() {
	if (this->tracker == nullptr) return nullptr;

	auto iter = this->managed.find(this->focusedWindow());
	if (iter == this->managed.end()) return nullptr;

	auto& m = iter->second;
	if (m.layout == nullptr || this->stateOf(m) != State::Tiled) return nullptr;
	return &m;
}

TilingManager::Managed* TilingManager::neighbour(const Managed& from, Edge direction) {
	if (from.layout == nullptr) return nullptr;

	auto source = from.layout->tree->box(idOf(from.window));
	Managed* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from.layout->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree->ids()) {
			auto* window = this->windowOf(id);
			if (window == nullptr || window == from.window) continue;

			auto& m = this->managed.at(window);
			if (this->stateOf(m) != State::Tiled || layout->tree->isParked(id)) continue;

			auto box = layout->tree->box(id);
			auto distance = 0;
			auto overlap = 0;

			switch (direction) {
			case Edge::Left:
				distance = source.left() - rightOf(box);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Right:
				distance = box.left() - rightOf(source);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Top:
				distance = source.top() - bottomOf(box);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			case Edge::Bottom:
				distance = box.top() - bottomOf(source);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			}

			if (distance < -EDGE_TOLERANCE || overlap <= 0) continue;

			if (best == nullptr || distance < bestDistance
			    || (distance == bestDistance && m.focusStamp > best->focusStamp))
			{
				best = &m;
				bestDistance = distance;
			}
		}
	}

	return best;
}

QScreen* TilingManager::screenInDirection(QScreen* from, Edge direction) const {
	auto source = physicalMonitor(from);
	QScreen* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto* screen: QGuiApplication::screens()) {
		if (screen == from) continue;
		auto rect = physicalMonitor(screen);
		auto distance = 0;
		auto overlap = 0;

		switch (direction) {
		case Edge::Left:
			distance = source.left() - rightOf(rect);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Right:
			distance = rect.left() - rightOf(source);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Top:
			distance = source.top() - bottomOf(rect);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		case Edge::Bottom:
			distance = rect.top() - bottomOf(source);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		}

		if (distance < 0 || overlap <= 0) continue;
		if (distance < bestDistance) {
			best = screen;
			bestDistance = distance;
		}
	}

	return best;
}

void TilingManager::swapWindows(Managed& a, Managed& b) {
	auto* layoutA = a.layout;
	auto* layoutB = b.layout;
	if (layoutA == nullptr || layoutB == nullptr) return;

	if (layoutA == layoutB) {
		layoutA->tree->swap(idOf(a.window), idOf(b.window));
	} else {
		layoutA->tree->replace(idOf(a.window), idOf(b.window));
		layoutB->tree->replace(idOf(b.window), idOf(a.window));
		a.layout = layoutB;
		b.layout = layoutA;
		if (layoutA->lastFocused == a.window) layoutA->lastFocused = b.window;
		if (layoutB->lastFocused == b.window) layoutB->lastFocused = a.window;
	}

	this->markLayout(layoutA);
	this->markLayout(layoutB);
}

void TilingManager::moveToLayout(
    Managed& m, Layout* layout, TrackedWindow* target, std::optional<Edge> side
) {
	auto* from = m.layout;
	if (from == nullptr || layout == nullptr || (from == layout && !side)) return;

	from->tree->remove(idOf(m.window));
	if (from->lastFocused == m.window) from->lastFocused = nullptr;
	this->markLayout(from);

	if (target == nullptr || !layout->tree->contains(idOf(target))) target = layout->lastFocused;
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	layout->tree->configure(this->options());
	layout->tree->insert(idOf(m.window), target == nullptr ? 0 : idOf(target), nullptr, side);
	layout->lastFocused = m.window;
	m.layout = layout;
	m.target = QRect();

	if (auto* scroll = TilingManager::scrollingOf(layout)) {
		this->prepareScrolling(layout);
		scroll->activate(idOf(m.window));
	}

	this->markLayout(layout);
}

bool TilingManager::focusDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return this->focusIdleStrip();

	if (auto* scroll = TilingManager::scrollingOf(m->layout)) {
		this->prepareScrolling(m->layout);
		auto id = scroll->neighbor(idOf(m->window), direction, false);
		if (id != 0 && this->windowOf(id) != nullptr) {
			scroll->activate(id);
			this->settle(m->layout);
			return true;
		}
	}

	auto* next = this->neighbour(*m, direction);
	if (next == nullptr) return false;

	next->window->activate();
	return true;
}

bool TilingManager::moveDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	if (auto* scroll = TilingManager::scrollingOf(m->layout)) {
		this->prepareScrolling(m->layout);
		if (scroll->moveWindow(idOf(m->window), direction)) {
			this->settle(m->layout);
		} else if (auto* screen = this->screenInDirection(m->layout->screen, direction)) {
			this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
		}
		return true;
	}

	auto* next = this->neighbour(*m, direction);

	if (next != nullptr && next->layout == m->layout) {
		this->swapWindows(*m, *next);
	} else if (next != nullptr) {
		this->moveToLayout(*m, next->layout, next->window);
	} else if (auto* screen = this->screenInDirection(m->layout->screen, direction)) {
		this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
	}

	return true;
}

bool TilingManager::swapDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	if (auto* scroll = TilingManager::scrollingOf(m->layout)) {
		this->prepareScrolling(m->layout);
		auto* other = this->windowOf(scroll->neighbor(idOf(m->window), direction, false));
		if (other != nullptr) {
			this->swapWindows(*m, this->managed.at(other));
			this->settle(m->layout);
			return true;
		}
	}

	if (auto* next = this->neighbour(*m, direction)) this->swapWindows(*m, *next);
	return true;
}

bool TilingManager::scrollColumns(TrackedWindow* window, qint32 delta) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->scrollColumns(delta);
	this->settle(layout);
	return true;
}

bool TilingManager::scrollView(TrackedWindow* window, qint32 pixels) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->scrollBy(toPhysical(pixels, layout->screen->devicePixelRatio()));
	this->settle(layout);
	return true;
}

bool TilingManager::columnWidth(TrackedWindow* window, double value, bool relative) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->setColumnWidth(idOf(window), value, relative);
	this->settle(layout);
	return true;
}

bool TilingManager::cycleColumnWidth(TrackedWindow* window, qint32 direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->cycleWidth(idOf(window), direction);
	this->settle(layout);
	return true;
}

bool TilingManager::allColumnsWidth(TrackedWindow* window, double value) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->setAllWidths(value);
	this->settle(layout);
	return true;
}

bool TilingManager::fitColumns(TrackedWindow* window, tiling::FitMode mode) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->fit(idOf(window), mode);
	this->settle(layout);
	return true;
}

bool TilingManager::focusColumn(TrackedWindow* window, Edge direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	auto id = scroll->neighbor(idOf(window), direction, true);
	if (id != 0) scroll->activate(id);
	this->settle(layout);
	return true;
}

bool TilingManager::moveIntoColumn(TrackedWindow* window, Edge direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->moveInto(idOf(window), direction);
	this->settle(layout);
	return true;
}

bool TilingManager::swapColumn(TrackedWindow* window, Edge direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->swapColumn(idOf(window), direction);
	this->settle(layout);
	return true;
}

bool TilingManager::promote(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->promote(idOf(window));
	this->settle(layout);
	return true;
}

bool TilingManager::centerColumn(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->center(idOf(window));
	this->settle(layout);
	return true;
}

bool TilingManager::toggleFit(TrackedWindow* window) {
	Layout* layout = nullptr;
	if (this->scrollingFor(window, &layout) == nullptr) return false;

	this->fitToggled = !this->fitToggled;
	this->markAll();
	return true;
}

bool TilingManager::toggleFullWidth(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->toggleFullWidth(idOf(window));
	this->settle(layout);
	return true;
}

bool TilingManager::toggleSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->toggleSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::swapSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->swapSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::splitRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->splitRatio(idOf(window), value, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::masterRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->kind() != tiling::Kind::Master) return false;
	if (layout->tree->splitRatio(idOf(window), value * 2.0, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::swapWithMaster(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.layout->tree->kind() != tiling::Kind::Master) return false;

	const auto& engine = static_cast<const tiling::MasterLayout&>(*m.layout->tree);
	auto master = engine.master();
	auto* other = this->windowOf(master == idOf(window) ? engine.firstInStack() : master);
	if (other != nullptr && other != window) this->swapWindows(m, this->managed.at(other));
	return true;
}

bool TilingManager::focusMaster(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.layout->tree->kind() != tiling::Kind::Master) return false;

	const auto& engine = static_cast<const tiling::MasterLayout&>(*m.layout->tree);
	auto master = engine.master();
	auto* target = this->windowOf(master == idOf(window) ? engine.firstInStack() : master);
	if (target != nullptr && target != window) target->activate();
	return true;
}

bool TilingManager::resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	auto dpr = layout->screen->devicePixelRatio();
	if (layout->tree->resize(idOf(window), toPhysical(dx, dpr), toPhysical(dy, dpr))) {
		this->markLayout(layout);
	}

	return true;
}

bool TilingManager::setFloating(TrackedWindow* window, bool floating, bool toggle) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return false;

	auto& m = iter->second;
	if (toggle) floating = m.layout != nullptr;

	if (!floating && m.elevated) {
		qCInfo(logTiling) << "Window" << window->addressHex()
		                  << "belongs to a more privileged process and can't be tiled.";
		return false;
	}

	m.override = floating ? Override::Float : Override::Tile;
	if (!floating) m.autoFloat = false;

	this->markWindow(window);
	return true;
}

bool TilingManager::beginEdgeDrag(TrackedWindow* window, bool left, bool top) {
	this->edgeDragState = EdgeDrag();

	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (this->stateOf(m) != State::Tiled) return false;

	auto* layout = m.layout;
	auto id = idOf(window);
	auto pick = [&](Edge preferred, Edge other) -> std::optional<Edge> {
		if (layout->tree->hasEdge(id, preferred)) return preferred;
		if (layout->tree->hasEdge(id, other)) return other;
		return std::nullopt;
	};

	auto& drag = this->edgeDragState;
	drag.window = window;
	drag.layout = layout;
	drag.box = layout->tree->box(id);
	drag.horizontal = left ? pick(Edge::Left, Edge::Right) : pick(Edge::Right, Edge::Left);
	drag.vertical = top ? pick(Edge::Top, Edge::Bottom) : pick(Edge::Bottom, Edge::Top);
	return true;
}

void TilingManager::edgeDrag(const QPoint& delta) {
	auto& drag = this->edgeDragState;
	if (drag.window == nullptr) return;

	auto iter = this->managed.find(drag.window);
	if (iter == this->managed.end() || iter->second.layout != drag.layout
	    || !this->layoutAlive(drag.layout))
	{
		drag = EdgeDrag();
		return;
	}

	auto edgeOf = [&drag](Edge edge) {
		switch (edge) {
		case Edge::Left: return drag.box.left();
		case Edge::Right: return rightOf(drag.box);
		case Edge::Top: return drag.box.top();
		case Edge::Bottom: return bottomOf(drag.box);
		}
		return 0;
	};

	auto& tree = *drag.layout->tree;
	auto id = idOf(drag.window);
	auto changed = false;

	if (drag.horizontal) {
		changed |= tree.moveEdge(id, *drag.horizontal, edgeOf(*drag.horizontal) + delta.x());
	}

	if (drag.vertical) {
		changed |= tree.moveEdge(id, *drag.vertical, edgeOf(*drag.vertical) + delta.y());
	}

	if (changed) this->markLayout(drag.layout);
}

void TilingManager::endEdgeDrag() { this->edgeDragState = EdgeDrag(); }

void TilingManager::resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact) {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto dpr = window->screen()->devicePixelRatio();
	auto frame = currentFrame(hwnd);
	auto width = toPhysical(dx, dpr);
	auto height = toPhysical(dy, dpr);
	if (!exact) {
		width += frame.width();
		height += frame.height();
	}

	setFrame(hwnd, QRect(frame.topLeft(), QSize(qMax(1, width), qMax(1, height))));
}

void TilingManager::center(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto frame = currentFrame(hwnd);
	frame.moveTo(
	    work.left() + (work.width() - frame.width()) / 2,
	    work.top() + (work.height() - frame.height()) / 2
	);
	setFrame(hwnd, frame);
}

void TilingManager::moveFloating(TrackedWindow* window, Edge direction) const {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto gap = toPhysical(this->mGapsOut, window->screen()->devicePixelRatio());
	work.adjust(gap, gap, -gap, -gap);

	auto frame = currentFrame(hwnd);
	switch (direction) {
	case Edge::Left: frame.moveLeft(work.left()); break;
	case Edge::Right: frame.moveRight(work.right()); break;
	case Edge::Top: frame.moveTop(work.top()); break;
	case Edge::Bottom: frame.moveBottom(work.bottom()); break;
	}

	setFrame(hwnd, frame);
}

Tiling::Tiling(QObject* parent): QObject(parent) {
	auto* manager = TilingManager::instance();
	QObject::connect(manager, &TilingManager::enabledChanged, this, &Tiling::enabledChanged);
	QObject::connect(manager, &TilingManager::gapsInChanged, this, &Tiling::gapsInChanged);
	QObject::connect(manager, &TilingManager::gapsOutChanged, this, &Tiling::gapsOutChanged);
	QObject::connect(
	    manager,
	    &TilingManager::singleWindowGapsOutChanged,
	    this,
	    &Tiling::singleWindowGapsOutChanged
	);
	QObject::connect(
	    manager,
	    &TilingManager::borderColoursChanged,
	    this,
	    &Tiling::borderColoursChanged
	);
	QObject::connect(
	    manager,
	    &TilingManager::activeBorderColourChanged,
	    this,
	    &Tiling::activeBorderColourChanged
	);
	QObject::connect(
	    manager,
	    &TilingManager::inactiveBorderColourChanged,
	    this,
	    &Tiling::inactiveBorderColourChanged
	);
	// clang-format off
	QObject::connect(manager, &TilingManager::preserveSplitChanged, this, &Tiling::preserveSplitChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::excludedChanged, this, &Tiling::excludedChanged);
	QObject::connect(manager, &TilingManager::includedChanged, this, &Tiling::includedChanged);
	QObject::connect(manager, &TilingManager::layoutChanged, this, &Tiling::layoutChanged);
	QObject::connect(manager, &TilingManager::smartSplitChanged, this, &Tiling::smartSplitChanged);
	QObject::connect(manager, &TilingManager::forceSplitChanged, this, &Tiling::forceSplitChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::defaultSplitRatioChanged, this, &Tiling::defaultSplitRatioChanged);
	QObject::connect(manager, &TilingManager::masterNewStatusChanged, this, &Tiling::masterNewStatusChanged);
	QObject::connect(manager, &TilingManager::masterNewOnTopChanged, this, &Tiling::masterNewOnTopChanged);
	QObject::connect(manager, &TilingManager::masterOrientationChanged, this, &Tiling::masterOrientationChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::masterFactorChanged, this, &Tiling::masterFactorChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::scrollingColumnWidthChanged, this, &Tiling::scrollingColumnWidthChanged);
	QObject::connect(manager, &TilingManager::scrollingCenterChanged, this, &Tiling::scrollingCenterChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::dragChanged, this, &Tiling::dragChanged);

	auto* telemetry = manager->telemetry();
	// clang-format off
	QObject::connect(telemetry, &tiling::TilingTelemetry::lastErrorChanged, this, &Tiling::lastErrorChanged);
	QObject::connect(telemetry, &tiling::TilingTelemetry::enableRefused, this, &Tiling::enableRefused);
	QObject::connect(telemetry, &tiling::TilingTelemetry::windowTilingFailed, this, &Tiling::windowTilingFailed);
	QObject::connect(telemetry, &tiling::TilingTelemetry::managedCountChanged, this, &Tiling::managedCountChanged);
	QObject::connect(telemetry, &tiling::TilingTelemetry::failedCountChanged, this, &Tiling::failedCountChanged);
	// clang-format on
}

void Tiling::relayout() { TilingManager::instance()->relayout(); }

bool Tiling::enabled() const { return TilingManager::instance()->enabled(); }
void Tiling::setEnabled(bool enabled) {
	// The first write claims this generation. A wrapper that never wrote does
	// not supersede the holder, so a reload into a mode that does not enable
	// still lets the outgoing generation's lease stop the session.
	if (this->mLease.generation() == 0) this->mLease.claim();
	TilingManager::instance()->setEnabled(enabled, this->mLease.generation());
}
Tiling::~Tiling() { this->mLease.release(); }
qint32 Tiling::gapsIn() const { return TilingManager::instance()->gapsIn(); }
void Tiling::setGapsIn(qint32 gaps) { TilingManager::instance()->setGapsIn(gaps); }
qint32 Tiling::gapsOut() const { return TilingManager::instance()->gapsOut(); }
void Tiling::setGapsOut(qint32 gaps) { TilingManager::instance()->setGapsOut(gaps); }
qint32 Tiling::singleWindowGapsOut() const {
	return TilingManager::instance()->singleWindowGapsOut();
}
void Tiling::setSingleWindowGapsOut(qint32 gaps) {
	TilingManager::instance()->setSingleWindowGapsOut(gaps);
}
bool Tiling::borderColours() const { return TilingManager::instance()->borderColours(); }
void Tiling::setBorderColours(bool enabled) {
	TilingManager::instance()->setBorderColours(enabled);
}
QColor Tiling::activeBorderColour() const {
	return TilingManager::instance()->activeBorderColour();
}
void Tiling::setActiveBorderColour(const QColor& colour) {
	TilingManager::instance()->setActiveBorderColour(colour);
}
QColor Tiling::inactiveBorderColour() const {
	return TilingManager::instance()->inactiveBorderColour();
}
void Tiling::setInactiveBorderColour(const QColor& colour) {
	TilingManager::instance()->setInactiveBorderColour(colour);
}
bool Tiling::preserveSplit() const { return TilingManager::instance()->preserveSplit(); }
void Tiling::setPreserveSplit(bool preserve) {
	TilingManager::instance()->setPreserveSplit(preserve);
}
QStringList Tiling::excluded() const { return TilingManager::instance()->excluded(); }
void Tiling::setExcluded(const QStringList& excluded) {
	TilingManager::instance()->setExcluded(excluded);
}
QStringList Tiling::included() const { return TilingManager::instance()->included(); }
void Tiling::setIncluded(const QStringList& included) {
	TilingManager::instance()->setIncluded(included);
}
qint32 Tiling::includedRuleCount() const {
	return TilingManager::instance()->includedRuleCount();
}
QString Tiling::lastError() const { return TilingManager::instance()->telemetry()->lastError(); }
qint32 Tiling::managedCount() const {
	return TilingManager::instance()->telemetry()->managedCount();
}
qint32 Tiling::failedCount() const {
	return TilingManager::instance()->telemetry()->failedCount();
}

QString Tiling::layout() const { return TilingManager::instance()->layout(); }
void Tiling::setLayout(const QString& layout) { TilingManager::instance()->setLayout(layout); }
bool Tiling::smartSplit() const { return TilingManager::instance()->smartSplit(); }
void Tiling::setSmartSplit(bool smart) { TilingManager::instance()->setSmartSplit(smart); }
qint32 Tiling::forceSplit() const { return TilingManager::instance()->forceSplit(); }
void Tiling::setForceSplit(qint32 force) { TilingManager::instance()->setForceSplit(force); }
double Tiling::defaultSplitRatio() const { return TilingManager::instance()->defaultSplitRatio(); }
void Tiling::setDefaultSplitRatio(double ratio) {
	TilingManager::instance()->setDefaultSplitRatio(ratio);
}
QString Tiling::masterNewStatus() const { return TilingManager::instance()->masterNewStatus(); }
void Tiling::setMasterNewStatus(const QString& status) {
	TilingManager::instance()->setMasterNewStatus(status);
}
bool Tiling::masterNewOnTop() const { return TilingManager::instance()->masterNewOnTop(); }
void Tiling::setMasterNewOnTop(bool onTop) { TilingManager::instance()->setMasterNewOnTop(onTop); }
QString Tiling::masterOrientation() const { return TilingManager::instance()->masterOrientation(); }
void Tiling::setMasterOrientation(const QString& orientation) {
	TilingManager::instance()->setMasterOrientation(orientation);
}
double Tiling::masterFactor() const { return TilingManager::instance()->masterFactor(); }
void Tiling::setMasterFactor(double factor) { TilingManager::instance()->setMasterFactor(factor); }
double Tiling::scrollingColumnWidth() const {
	return TilingManager::instance()->scrollingColumnWidth();
}
void Tiling::setScrollingColumnWidth(double width) {
	TilingManager::instance()->setScrollingColumnWidth(width);
}
QString Tiling::scrollingCenter() const { return TilingManager::instance()->scrollingCenter(); }
void Tiling::setScrollingCenter(const QString& center) {
	TilingManager::instance()->setScrollingCenter(center);
}
bool Tiling::dragActive() const { return TilingManager::instance()->dragActive(); }
QString Tiling::dragMode() const { return TilingManager::instance()->dragMode(); }
QString Tiling::dragScreen() const { return TilingManager::instance()->dragScreen(); }
QRectF Tiling::dragLanding() const { return TilingManager::instance()->dragLanding(); }
QRectF Tiling::dragDisplaced() const { return TilingManager::instance()->dragDisplaced(); }

} // namespace qs::windows
