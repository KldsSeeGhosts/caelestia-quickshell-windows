#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qobject.h>
#include <qrect.h>
#include <qsignalspy.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtest.h>
#include <qtypes.h>

#include "hyprland/pin_support.hpp"
#include "tiling_scope.hpp"

using namespace qs::windows::tiling;
namespace tiling = qs::windows::tiling;

namespace {

QStringList capturedMessages;

void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
	capturedMessages.append(message);
}

// Fake window operations: no HWND, no monitor query, no desktop side effect.
class FakeWindowOps: public tiling::WindowOps {
public:
	QHash<quintptr, QRect> frames;
	QList<QPair<quintptr, QRect>> written;

	[[nodiscard]] bool readFrame(quintptr window, QRect& raw, QRect& frame) const override {
		if (!this->frames.contains(window)) return false;
		raw = this->frames.value(window);
		frame = raw;
		return true;
	}

	void setRawRect(quintptr window, const QRect& rect) override {
		this->written.append({window, rect});
		this->frames.insert(window, rect);
	}

	[[nodiscard]] bool iconicOrZoomed(quintptr window) const override { return false; }
	void restorePlacement(quintptr window, const QRect& rect) override {
		this->written.append({window, rect});
	}

	[[nodiscard]] QRect onScreen(const QRect& rect) const override { return rect; }
	[[nodiscard]] quintptr monitorFor(const QRect&) const override { return 1; }
	[[nodiscard]] QRect workArea(quintptr) const override { return {0, 0, 1920, 1080}; }
};

class FakePinAccessor: public qs::windows::AccessorState {
public:
	using AccessorState::setAccessorLoaded;
	using AccessorState::setPinExports;
	int reads = 0;
	int writes = 0;
	bool writeSucceeds = true;
	bool isWindowPinned(quintptr) {
		this->reads++;
		return true; // Deliberately stale when exports or the accessor are missing.
	}
	bool pinWindow(quintptr, bool pinned) {
		this->writes++;
		Q_ASSERT(!pinned);
		return this->writeSucceeds;
	}
};

tiling::WindowFacts editorFacts() {
	tiling::WindowFacts facts;
	facts.appId = QStringLiteral("org.example.Editor");
	facts.exePath = QStringLiteral("C:\\Program Files\\Example\\editor.exe");
	facts.title = QStringLiteral("Notes - Editor");
	facts.className = QStringLiteral("Chrome_WidgetWin_1");
	return facts;
}

} // namespace

class TilingObservabilityTest: public QObject {
	Q_OBJECT;

private slots:
	void refusedEnableSetsLastErrorAndEmits();
	void successfulEnableClearsLastError();
	void giveUpMinSizeEmitsFailure();
	void giveUpOffTargetEmitsFailure();
	void giveUpCorrectionLimitEmitsFailure();
	void unresponsiveFailureReason();
	void offscreenGiveUpIsNotAFailure();
	void dpiVirtualizedWindowKeepsItsTile();
	void landingToleranceFollowsTheBitmapScale_data();
	void landingToleranceFollowsTheBitmapScale();
	void emptyAllowlistKeepsCurrentBehavior();
	void allowlistFiltersWindows();
	void excludedWinsOverAllowlist();
	void titlePatternsMatchAllowlistedWindows();
	void ownProcessNeverManaged();
	void typedRulesAndMonitorExclusion();
	void invalidSelectorsAreIgnoredWithoutLoggingText();
	void allowlistOfOnlyInvalidEntriesFailsClosed();
	void invalidExclusionsAreDroppedWithoutBlockingWindows();
	void titleSelectorMatchingEmptyTitleIsRejected();
	void restorePrefersPreTilingOverParkedRect();
	void restoreIgnoresMissingPreTiling();
	void restoreValuesAreStable();
	void pinCapabilityWithoutAccessorIsFalse();
	void pinExports_data();
	void pinExports();
};

void TilingObservabilityTest::typedRulesAndMonitorExclusion() {
	auto facts = editorFacts();
	facts.screenName = QStringLiteral("DISPLAY1");
	ScopeRules rules;
	rules.setIncluded({QStringLiteral("exe:EDITOR.EXE")});
	QVERIFY(rules.hasIncluded());
	QCOMPARE(rules.floatReason(facts), FloatReason::None);
	rules.setIncluded({QStringLiteral("class:editor.exe")});
	QCOMPARE(rules.floatReason(facts), FloatReason::NotIncluded);
	rules.setIncluded({QStringLiteral("class:Chrome_WidgetWin_*")});
	QCOMPARE(rules.floatReason(facts), FloatReason::None);
	rules.setExcluded({QStringLiteral("screen:DISPLAY1")});
	QCOMPARE(rules.floatReason(facts), FloatReason::Excluded);
	facts.screenName = QStringLiteral("DISPLAY2");
	QCOMPARE(rules.floatReason(facts), FloatReason::None);
	rules.setExcluded({QStringLiteral("exe:editor")});
	QCOMPARE(rules.floatReason(facts), FloatReason::Excluded);
	rules.setIncluded({QStringLiteral("exe: "), QStringLiteral("class:"), QStringLiteral("screen:")});
	QVERIFY(!rules.hasIncluded());
}

void TilingObservabilityTest::invalidSelectorsAreIgnoredWithoutLoggingText() {
	auto facts = editorFacts();
	capturedMessages.clear();
	auto previous = qInstallMessageHandler(&captureMessage);
	ScopeRules rules;
	rules.setIncluded({QStringLiteral("title:["), QStringLiteral("exe:editor.exe")});
	auto reason = rules.floatReason(facts);
	qInstallMessageHandler(previous);

	// The invalid title rule is dropped; the valid executable rule still
	// includes the window and its text never reaches a log.
	QCOMPARE(reason, FloatReason::None);
	QVERIFY(rules.hasIncluded());
	QVERIFY(!rules.hasTitleRules());

	const auto log = capturedMessages.join(QLatin1Char('\n'));
	QVERIFY(log.contains(QStringLiteral("invalid pattern")));
	QVERIFY(log.contains(QStringLiteral("selector 1")));
	QVERIFY(!log.contains(QStringLiteral("title:[")));
	QVERIFY(!log.contains(QLatin1Char('[')));
}

// An allowlist the user wrote but that did not compile must match nothing. It
// used to read as "no allowlist", which made `include: ["title:["]` turn on
// unrestricted tiling.
void TilingObservabilityTest::allowlistOfOnlyInvalidEntriesFailsClosed() {
	auto facts = editorFacts();
	capturedMessages.clear();
	auto previous = qInstallMessageHandler(&captureMessage);
	ScopeRules rules;
	rules.setIncluded({QStringLiteral("title:["), QStringLiteral("exe:[a")});
	auto reason = rules.floatReason(facts);
	qInstallMessageHandler(previous);

	QVERIFY(rules.hasIncluded());
	QCOMPARE(rules.includedRuleCount(), 0);
	QVERIFY(!rules.matchesIncluded(facts));
	QCOMPARE(reason, FloatReason::NotIncluded);

	// Whatever the window is, nothing passes.
	facts.title = QStringLiteral("anything");
	facts.exePath = QStringLiteral("C:\\other.exe");
	QCOMPARE(rules.floatReason(facts), FloatReason::NotIncluded);

	const auto log = capturedMessages.join(QLatin1Char('\n'));
	QVERIFY(!log.contains(QLatin1Char('[')));

	// One valid entry restores a usable allowlist; clearing the list removes it.
	rules.setIncluded({QStringLiteral("title:["), QStringLiteral("exe:editor.exe")});
	QCOMPARE(rules.includedRuleCount(), 1);
	rules.setIncluded({});
	QVERIFY(!rules.hasIncluded());
	QCOMPARE(rules.includedRuleCount(), 0);
	QCOMPARE(rules.floatReason(editorFacts()), FloatReason::None);
}

void TilingObservabilityTest::invalidExclusionsAreDroppedWithoutBlockingWindows() {
	ScopeRules rules;
	auto previous = qInstallMessageHandler(&captureMessage);
	rules.setExcluded({QStringLiteral("title:["), QStringLiteral("class:[b")});
	qInstallMessageHandler(previous);

	// Invalid exclusions simply drop; only valid ones exclude.
	QCOMPARE(rules.floatReason(editorFacts()), FloatReason::None);
	rules.setExcluded({QStringLiteral("title:["), QStringLiteral("exe:editor.exe")});
	QCOMPARE(rules.floatReason(editorFacts()), FloatReason::Excluded);
}

void TilingObservabilityTest::titleSelectorMatchingEmptyTitleIsRejected() {
	auto facts = editorFacts();
	facts.title = QString();

	capturedMessages.clear();
	auto previous = qInstallMessageHandler(&captureMessage);
	qs::windows::WindowSelectors selectors;
	selectors.setPatterns({QStringLiteral("title:*"), QStringLiteral("title:?*"), QStringLiteral("title:Notes*")});
	qInstallMessageHandler(previous);

	// `title:*` matches "" and is rejected, so a transient empty title cannot
	// match any rule. The other two compile, but never match an empty title.
	QCOMPARE(selectors.configuredCount(), 3);
	QCOMPARE(selectors.compiledCount(), 2);
	QVERIFY(!selectors.matchesTitle(QString()));
	QVERIFY(!selectors.matches(QString(), QString(), QString(), QString(), QString()));
	QVERIFY(selectors.matchesTitle(QStringLiteral("Notes - Editor")));
	QVERIFY(selectors.matchesTitle(QStringLiteral("x")));

	const auto log = capturedMessages.join(QLatin1Char('\n'));
	QVERIFY(log.contains(QStringLiteral("selector 1")));
	QVERIFY(log.contains(QStringLiteral("empty title")));
	QVERIFY(!log.contains(QStringLiteral("title:*")));

	// An empty-title rule used as an allowlist fails closed.
	ScopeRules rules;
	rules.setIncluded({QStringLiteral("title:*")});
	QVERIFY(rules.hasIncluded());
	QCOMPARE(rules.includedRuleCount(), 0);
	QCOMPARE(rules.floatReason(facts), FloatReason::NotIncluded);
}

void TilingObservabilityTest::refusedEnableSetsLastErrorAndEmits() {
	tiling::TilingTelemetry telemetry;
	QSignalSpy refused(&telemetry, &tiling::TilingTelemetry::enableRefused);
	QSignalSpy changed(&telemetry, &tiling::TilingTelemetry::lastErrorChanged);

	QVERIFY(telemetry.lastError().isEmpty());

	telemetry.refuseEnable(QStringLiteral("tiler-mutex-held"));
	QCOMPARE(telemetry.lastError(), QStringLiteral("tiler-mutex-held"));
	QTRY_COMPARE(refused.count(), 1);
	QCOMPARE(refused.at(0).at(0).toString(), QStringLiteral("tiler-mutex-held"));
	QCOMPARE(changed.count(), 1);

	// A repeat refusal is still signalled even when the error text is unchanged.
	telemetry.refuseEnable(QStringLiteral("tiler-mutex-held"));
	QTRY_COMPARE(refused.count(), 2);
	QCOMPARE(changed.count(), 1);

	telemetry.refuseEnable(QStringLiteral("no-screen"));
	QCOMPARE(telemetry.lastError(), QStringLiteral("no-screen"));
	QTRY_COMPARE(refused.count(), 3);
	QTRY_COMPARE(changed.count(), 2);
}

void TilingObservabilityTest::successfulEnableClearsLastError() {
	tiling::TilingTelemetry telemetry;
	QSignalSpy changed(&telemetry, &tiling::TilingTelemetry::lastErrorChanged);
	QSignalSpy failed(&telemetry, &tiling::TilingTelemetry::failedCountChanged);

	telemetry.refuseEnable(QStringLiteral("tiler-mutex-held"));
	telemetry.noteFailure(QStringLiteral("10"), QStringLiteral("min-size"));
	QCOMPARE(telemetry.failedCount(), 1);

	telemetry.enableSucceeded();
	QVERIFY(telemetry.lastError().isEmpty());
	QCOMPARE(telemetry.failedCount(), 0);
	QTRY_COMPARE(changed.count(), 2);
	QTRY_COMPARE(failed.count(), 2);
}

void TilingObservabilityTest::giveUpMinSizeEmitsFailure() {
	tiling::TilingTelemetry telemetry;
	QSignalSpy failed(&telemetry, &tiling::TilingTelemetry::windowTilingFailed);

	// Reported as min-size, but the window stays tiled at its larger size.
	auto outcome = tiling::classifyGiveUp({{0, 0, 800, 600}, {0, 0, 1000, 600}, false, 2});
	QCOMPARE(outcome.autoFloat, false);
	QCOMPARE(outcome.accept, true);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(outcome.reason)),
	    QStringLiteral("min-size")
	);

	telemetry.noteFailure(
	    QStringLiteral("1a2b"),
	    QString::fromLatin1(tiling::failureReasonName(outcome.reason))
	);

	QCOMPARE(telemetry.failedCount(), 1);
	QTRY_COMPARE(failed.count(), 1);
	QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("1a2b"));
	QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("min-size"));
}

void TilingObservabilityTest::giveUpOffTargetEmitsFailure() {
	auto outcome = tiling::classifyGiveUp({{0, 0, 800, 600}, {2000, 2000, 400, 300}, false, 2});
	QCOMPARE(outcome.autoFloat, true);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(outcome.reason)),
	    QStringLiteral("off-target")
	);
}

void TilingObservabilityTest::giveUpCorrectionLimitEmitsFailure() {
	// Smaller than the tile and still centred: the window stays tiled, but the
	// correction budget was exhausted without an exact fit.
	auto outcome = tiling::classifyGiveUp({{0, 0, 800, 600}, {0, 0, 780, 600}, false, 2});
	QCOMPARE(outcome.accept, true);
	QCOMPARE(outcome.autoFloat, false);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(outcome.reason)),
	    QStringLiteral("correction-limit")
	);

	// An exact frame is not a failure.
	auto exact = tiling::classifyGiveUp({{0, 0, 800, 600}, {0, 0, 800, 600}, false, 2});
	QCOMPARE(exact.accept, true);
	QCOMPARE(exact.reason, tiling::FailureReason::None);
}

void TilingObservabilityTest::unresponsiveFailureReason() {
	tiling::TilingTelemetry telemetry;
	QSignalSpy failed(&telemetry, &tiling::TilingTelemetry::windowTilingFailed);

	telemetry.noteFailure(
	    QStringLiteral("22"),
	    QString::fromLatin1(tiling::failureReasonName(tiling::FailureReason::Unresponsive))
	);

	QTRY_COMPARE(failed.count(), 1);
	QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("unresponsive"));
}

void TilingObservabilityTest::offscreenGiveUpIsNotAFailure() {
	auto outcome = tiling::classifyGiveUp({{0, 0, 800, 600}, {5000, 5000, 100, 100}, true, 2});
	QCOMPARE(outcome.accept, true);
	QCOMPARE(outcome.autoFloat, false);
	QCOMPARE(outcome.reason, tiling::FailureReason::None);
}

void TilingObservabilityTest::dpiVirtualizedWindowKeepsItsTile() {
	// A plain WinForms window of a DPI-unaware process on a 150% monitor: Windows
	// reports its rectangle on the window's own scaled 96-DPI grid, so a 1600x1000
	// physical tile reads back as 1067x667 there, while the DWM frame stays
	// physical. Mixing the two spaces is why every verification saw a mismatch and
	// floated the window as "ignores being moved".
	const auto unaware = quint32(0);    // DPI_AWARENESS_UNAWARE
	const auto perMonitor = quint32(2); // DPI_AWARENESS_PER_MONITOR_AWARE
	QVERIFY(tiling::dpiVirtualized(unaware));
	QVERIFY(!tiling::dpiVirtualized(perMonitor));

	auto tile = QRect(-3400, 120, 1600, 1000);
	// The compositor rounds the placed window onto its 1.5x grid.
	auto placed = QRect(-3399, 121, 1600, 1000);
	QVERIFY(!tiling::placementLanded(placed, tile, true, 0)); // Not exact...
	QVERIFY(tiling::placementLanded(placed, tile, true, 2));  // ...but landed.
	QVERIFY(tiling::placementLanded(placed, placed, true, 0));

	// Per-monitor aware windows keep the exact comparison they always had.
	QVERIFY(!tiling::placementLanded(placed, tile, false, 2));
	QVERIFY(tiling::placementLanded(tile, tile, false, 2));

	// A frame that stayed on the virtualized grid is still a real mismatch.
	auto scaled = QRect(-2267, 80, 1067, 667);
	QVERIFY(!tiling::placementLanded(scaled, tile, true, 2));
}

void TilingObservabilityTest::landingToleranceFollowsTheBitmapScale_data() {
	QTest::addColumn<quint32>("awareness");
	QTest::addColumn<qreal>("scale");
	QTest::addColumn<int>("tolerance");
	// 0 unaware, 1 system aware, 2 per monitor aware.
	QTest::newRow("unaware-150") << quint32(0) << qreal(1.5) << 2;
	QTest::newRow("unaware-100") << quint32(0) << qreal(1.0) << 0;
	QTest::newRow("system-native-scale") << quint32(1) << qreal(1.0) << 0;
	QTest::newRow("system-on-175") << quint32(1) << qreal(1.75) << 2;
	QTest::newRow("system-measured-175") << quint32(1) << qreal(1876.0 / 1072.0) << 2;
	QTest::newRow("system-on-250") << quint32(1) << qreal(2.5) << 3;
	QTest::newRow("system-on-lower-dpi") << quint32(1) << qreal(96.0 / 144.0) << 1;
	QTest::newRow("per-monitor-175") << quint32(2) << qreal(1.75) << 0;
	QTest::newRow("unknown-scale") << quint32(1) << qreal(0.0) << 0;
}

void TilingObservabilityTest::landingToleranceFollowsTheBitmapScale() {
	QFETCH(quint32, awareness);
	QFETCH(qreal, scale);
	QFETCH(int, tolerance);
	QCOMPARE(tiling::landingTolerance(awareness, scale), tolerance);
	// The system-aware case from the audit: one pixel of rounding at 1.75.
	auto tile = QRect(-3695, 22, 1876, 1036);
	auto placed = tile.translated(1, 1);
	QCOMPARE(tiling::placementLanded(placed, tile, tolerance > 0, tolerance), tolerance > 0);
	// A genuinely off-target frame never lands, whatever the scale.
	QVERIFY(!tiling::placementLanded(tile.translated(40, 0), tile, tolerance > 0, tolerance));
}

void TilingObservabilityTest::emptyAllowlistKeepsCurrentBehavior() {
	tiling::ScopeRules rules;
	auto facts = editorFacts();

	QVERIFY(!rules.hasIncluded());
	QCOMPARE(rules.floatReason(facts), tiling::FloatReason::None);

	// Whitespace and empty entries do not turn the allowlist on.
	rules.setIncluded({QString(), QStringLiteral("   ")});
	QVERIFY(!rules.hasIncluded());
	QCOMPARE(rules.floatReason(facts), tiling::FloatReason::None);
}

void TilingObservabilityTest::allowlistFiltersWindows() {
	tiling::ScopeRules rules;
	auto editor = editorFacts();

	rules.setIncluded({QStringLiteral("*editor.exe")});
	QVERIFY(rules.hasIncluded());
	QCOMPARE(rules.floatReason(editor), tiling::FloatReason::None);

	auto other = editor;
	other.appId = QStringLiteral("org.example.Terminal");
	other.exePath = QStringLiteral("C:\\Program Files\\Example\\terminal.exe");
	other.title = QStringLiteral("Terminal");
	other.className = QStringLiteral("ConsoleWindowClass");
	QCOMPARE(rules.floatReason(other), tiling::FloatReason::NotIncluded);

	// Matching is case insensitive, like `excluded`.
	rules.setIncluded({QStringLiteral("*EDITOR.EXE")});
	QCOMPARE(rules.floatReason(editor), tiling::FloatReason::None);
}

void TilingObservabilityTest::excludedWinsOverAllowlist() {
	tiling::ScopeRules rules;
	auto editor = editorFacts();

	rules.setIncluded({QStringLiteral("*")});
	rules.setExcluded({QStringLiteral("org.example.*")});

	QCOMPARE(rules.floatReason(editor), tiling::FloatReason::Excluded);
	QVERIFY(rules.matchesExcluded(editor));
	QVERIFY(rules.matchesIncluded(editor));

	rules.setExcluded({});
	QCOMPARE(rules.floatReason(editor), tiling::FloatReason::None);
}

void TilingObservabilityTest::titlePatternsMatchAllowlistedWindows() {
	tiling::ScopeRules rules;
	auto editor = editorFacts();

	rules.setIncluded({QStringLiteral("title:*notes*")});
	QCOMPARE(rules.floatReason(editor), tiling::FloatReason::None);

	auto other = editor;
	other.title = QStringLiteral("Spreadsheet");
	QCOMPARE(rules.floatReason(other), tiling::FloatReason::NotIncluded);
}

void TilingObservabilityTest::ownProcessNeverManaged() {
	tiling::ScopeRules rules;
	rules.setIncluded({QStringLiteral("*")});
	rules.setExcluded({});

	auto own = editorFacts();
	own.ownProcess = true;

	// Even a window that matches the allowlist and would otherwise tile floats
	// when it belongs to the shell process.
	QCOMPARE(rules.floatReason(own), tiling::FloatReason::OwnProcess);
	QVERIFY(rules.matchesIncluded(own));

	own.ownProcess = false;
	QCOMPARE(rules.floatReason(own), tiling::FloatReason::None);
}

void TilingObservabilityTest::restorePrefersPreTilingOverParkedRect() {
	tiling::ScopeRules rules;
	QCOMPARE(
	    tiling::restoreTarget(QRect(10, 20, 300, 200), QRect(1, 2, 3, 4)),
	    QRect(10, 20, 300, 200)
	);
	QCOMPARE(tiling::restoreTarget(QRect(), QRect(1, 2, 300, 200)), QRect(1, 2, 300, 200));
}

void TilingObservabilityTest::restoreIgnoresMissingPreTiling() {
	FakeWindowOps ops;
	QVERIFY(!tiling::restoreTiledWindow(ops, 0x1, QRect()));
	QVERIFY(ops.written.isEmpty());
}

void TilingObservabilityTest::restoreValuesAreStable() {
	// The reason strings are a machine-readable contract for QML.
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(tiling::FailureReason::None)),
	    QStringLiteral("none")
	);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(tiling::FailureReason::MinSize)),
	    QStringLiteral("min-size")
	);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(tiling::FailureReason::OffTarget)),
	    QStringLiteral("off-target")
	);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(tiling::FailureReason::CorrectionLimit)),
	    QStringLiteral("correction-limit")
	);
	QCOMPARE(
	    QString::fromLatin1(tiling::failureReasonName(tiling::FailureReason::Unresponsive)),
	    QStringLiteral("unresponsive")
	);

	QCOMPARE(
	    QString::fromLatin1(tiling::floatReasonName(tiling::FloatReason::OwnProcess)),
	    QStringLiteral("own-process")
	);
	QCOMPARE(
	    QString::fromLatin1(tiling::floatReasonName(tiling::FloatReason::NotIncluded)),
	    QStringLiteral("not-included")
	);
}

void TilingObservabilityTest::pinCapabilityWithoutAccessorIsFalse() {
	// Without VirtualDesktopAccessor there is no truthful pin state: the
	// Hyprland singleton must report pinSupported=false and pinned=false even
	// if a stale value claims otherwise.
	auto missing = qs::hyprland::ipc::pinCapability(false, true);
	QVERIFY(!missing.supported);
	QVERIFY(!missing.pinned);

	auto loaded = qs::hyprland::ipc::pinCapability(true, true);
	QVERIFY(loaded.supported);
	QVERIFY(loaded.pinned);

	auto unpinned = qs::hyprland::ipc::pinCapability(true, false);
	QVERIFY(unpinned.supported);
	QVERIFY(!unpinned.pinned);
}

void TilingObservabilityTest::pinExports_data() {
	QTest::addColumn<int>("exports");
	for (int mask = 0; mask < 8; mask++) {
		QTest::newRow(qPrintable(QStringLiteral("exports-%1").arg(mask, 3, 2, QLatin1Char('0'))))
		    << mask;
	}
}

void TilingObservabilityTest::pinExports() {
	using namespace qs::hyprland::ipc;
	QFETCH(int, exports);
	FakePinAccessor accessor;
	accessor.setPinExports(exports & 1, exports & 2, exports & 4);
	PinSupport observer(&accessor);
	QSignalSpy changed(&observer, &PinSupport::pinSupportedChanged);
	auto drain = []() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); };

	QVERIFY(!observer.pinSupported());
	QVERIFY(!pinCapability(accessor.pinSupported(), true).pinned);
	QCOMPARE(dispatchPin(accessor, quintptr(1)), PinDispatchResult::Unsupported);
	accessor.setAccessorLoaded(true);
	drain();
	auto supported = exports == 7;
	QCOMPARE(observer.pinSupported(), supported);
	QCOMPARE(changed.count(), 1); // Loading is still observable without pin exports.
	auto projected = pinCapability(accessor.pinSupported(), true);
	QCOMPARE(projected.supported, supported);
	QCOMPARE(projected.pinned, supported); // Missing exports must clear stale true.
	QCOMPARE(
	    dispatchPin(accessor, quintptr(1)),
	    supported ? PinDispatchResult::Succeeded : PinDispatchResult::Unsupported
	);
	QCOMPARE(accessor.reads, supported ? 1 : 0);
	QCOMPARE(accessor.writes, supported ? 1 : 0);

	// A loaded accessor can gain or lose its complete pin API; observers refresh.
	accessor.setPinExports(!supported, !supported, !supported);
	drain();
	QCOMPARE(observer.pinSupported(), !supported);
	QCOMPARE(changed.count(), 2);
	accessor.setPinExports(true, true, true);
	accessor.writeSucceeds = false;
	QCOMPARE(dispatchPin(accessor, quintptr(1)), PinDispatchResult::Failed);
	drain();
	auto beforeLoss = changed.count();
	accessor.setAccessorLoaded(false);
	drain();
	QVERIFY(!observer.pinSupported());
	QVERIFY(!pinCapability(accessor.pinSupported(), true).pinned);
	QCOMPARE(dispatchPin(accessor, quintptr(1)), PinDispatchResult::Unsupported);
	QCOMPARE(changed.count(), beforeLoss + 1);
}

QTEST_GUILESS_MAIN(TilingObservabilityTest)

#include "tiling_observability_test.moc"
