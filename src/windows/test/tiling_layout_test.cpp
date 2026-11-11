#include <tuple>

#include <qlist.h>
#include <qjsondocument.h>
#include <qobject.h>
#include <qrect.h>
#include <qsize.h>
#include <qtest.h>
#include <qtestcase.h>
#include <qtemporarydir.h>

#include "tiling_layout.hpp"
#include "tiling_persistence.hpp"

using qs::windows::tiling::CenterMode;
using qs::windows::tiling::DwindleLayout;
using qs::windows::tiling::Edge;
using qs::windows::tiling::FitMode;
using qs::windows::tiling::LayoutEngine;
using qs::windows::tiling::Options;
using qs::windows::tiling::ScrollingLayout;

namespace {

using Id = LayoutEngine::Id;

constexpr Id A = 1;
constexpr Id B = 2;
constexpr Id C = 3;
constexpr Id D = 4;

const QRect AREA = QRect(0, 0, 1000, 800);

int rightOf(const QRect& rect) { return rect.left() + rect.width(); }
int bottomOf(const QRect& rect) { return rect.top() + rect.height(); }

bool touches(const QRect& a, const QRect& b) {
	return a.left() < rightOf(b) && b.left() < rightOf(a) && a.top() < bottomOf(b)
	    && b.top() < bottomOf(a);
}

void setup(
    ScrollingLayout& layout,
    const QList<QRect>& others = {},
    CenterMode mode = CenterMode::Overflow
) {
	auto options = Options();
	options.centerMode = mode;
	layout.configure(options);
	layout.setGaps(0, 0);
	layout.setMonitors(AREA, others);
	layout.compute(AREA);
}

void fill(ScrollingLayout& layout, const QList<Id>& ids) {
	auto previous = Id(0);
	for (auto id: ids) {
		layout.insert(id, previous, nullptr);
		previous = id;
	}
	layout.activate(ids.first());
	layout.compute(AREA);
}

}

class TestTilingLayout: public QObject {
	Q_OBJECT;

private slots:
	void restorePlanning_data();
	void restorePlanning();
	void snapshotHintsFailClosed();
	void snapshotRoundTrip();
	void layoutStateRoundTrip_data();
	void layoutStateRoundTrip();
	void staleLayoutLeavesCollapse_data();
	void staleLayoutLeavesCollapse();
	void dwindleDropSplitsTheChosenSide_data();
	void dwindleDropSplitsTheChosenSide();
	void dwindleDropFollowsTheTargetAspect_data();
	void dwindleDropFollowsTheTargetAspect();
	void dwindleDropCollapsesTheSource();
	void dwindleEmptyDestinationAcceptsDrop();
	void newColumnsGoRightOfTheFocusedOne();
	void gapsKeepWindowSizesWhileScrolling();
	void parksColumnsOffTheScreen();
	void focusingScrollsTheLeastNeeded();
	void centersWhenTheNeighbourDoesNotFit();
	void alwaysAndNeverCenter();
	void peeksOnlyWhereNoMonitorIsNext();
	void parkedWindowsStayOffEveryMonitor();
	void parkingKeepsTheOwnMonitorNearest();
	void removingAColumnClosesTheGap();
	void removingAColumnLeftOfTheViewKeepsTheView();
	void closingANewWindowReturnsToItsOpener();
	void movingAWindowSwapsOrExpels();
	void movingIntoAColumnStacks();
	void promoteAndSwapColumns();
	void widthPresetsCycle();
	void fullWidthToggles();
	void fitModes();
	void scrollingByColumnsAndPixels();
	void resizingAndEdges();
	void hiddenWindowsTakeNoSpace();
	void removingAHiddenWindowKeepsTheView();
	void neighboursWrapOnRequest();
	void swapKeepsTheView();
	void adoptedWindowsHaveNeighbours();
};

void TestTilingLayout::restorePlanning_data() {
	QTest::addColumn<QList<quintptr>>("discovery");
	QTest::addColumn<bool>("monitorPresent");
	QTest::addColumn<bool>("reused");
	QTest::addColumn<QList<quintptr>>("expected");
	QTest::addColumn<QList<qsizetype>>("unmatched");
	QTest::newRow("adopt-in-saved-order") << QList<quintptr>{3, 1, 2} << true << false
	    << QList<quintptr>{1, 2, 3} << QList<qsizetype>{};
	QTest::newRow("missing-monitor") << QList<quintptr>{3, 1, 2} << false << false
	    << QList<quintptr>{} << QList<qsizetype>{0, 1, 2};
	QTest::newRow("new-window-interleaved") << QList<quintptr>{3, 4, 1, 2} << true << false
	    << QList<quintptr>{1, 2, 3} << QList<qsizetype>{1};
	QTest::newRow("stale-entry") << QList<quintptr>{3, 1} << true << false
	    << QList<quintptr>{1, 3} << QList<qsizetype>{};
	QTest::newRow("hwnd-reused-by-another-process") << QList<quintptr>{3, 1, 2} << true << true
	    << QList<quintptr>{1, 3} << QList<qsizetype>{2};
}

void TestTilingLayout::restorePlanning() {
	using namespace qs::windows::tiling;
	QFETCH(QList<quintptr>, discovery);
	QFETCH(bool, monitorPresent);
	QFETCH(bool, reused);
	QFETCH(QList<quintptr>, expected);
	QFETCH(QList<qsizetype>, unmatched);
	QList<SavedWindow> saved;
	for (quintptr i = 1; i <= 3; ++i) {
		saved.append({{i, 10, 100, "app", QString::number(i)}, "monitor", "desktop"});
	}
	QList<WindowIdentity> live;
	for (auto hwnd: discovery) {
		live.append({hwnd, reused && hwnd == 2 ? 11U : 10U, 100, "app", QString::number(hwnd)});
	}
	auto plan = planRestore(
	    saved, live, monitorPresent ? QSet<QString>{"monitor"} : QSet<QString>{},
	    {"desktop"}
	);
	QList<quintptr> order;
	for (const auto& match: plan.matched) order.append(live.at(match.current).hwnd);
	QCOMPARE(order, expected);
	QCOMPARE(plan.unmatched, unmatched);
	// A removed virtual desktop must fall back just like a removed monitor.
	QVERIFY(planRestore(saved, live, {"monitor"}, {}).matched.isEmpty());
}

void TestTilingLayout::snapshotHintsFailClosed() {
	using namespace qs::windows::tiling;
	QList<SavedWindow> saved {{{1, 10, 100, "app", "title"}, "monitor", "desktop"}};
	auto plan = [&](const QList<WindowIdentity>& live) {
		return planRestore(saved, live, {"monitor"}, {"desktop"});
	};
	QCOMPARE(plan({{2, 10, 100, "app", "title"}}).matched.size(), 1);
	QVERIFY(plan({{2, 10, 101, "app", "title"}}).matched.isEmpty()); // PID reuse.
	QVERIFY(plan({{1, 11, 100, "app", "title"}, {2, 10, 100, "app", "title"}}).matched.isEmpty());
	QVERIFY(plan({{2, 10, 100, "app", "title"}, {3, 10, 100, "app", "title"}}).matched.isEmpty());
	QVERIFY(plan({{1, 10, 0, "app", "title"}}).matched.isEmpty());
	saved.append(saved.first());
	QVERIFY(plan({{2, 10, 100, "app", "title"}}).matched.isEmpty());
}

void TestTilingLayout::snapshotRoundTrip() {
	using namespace qs::windows::tiling;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	const auto path = directory.filePath("state/tiling-layout.json");
	LayoutSnapshot snapshot;
	snapshot.windows.append({
	    {0x123, 42, 0x123456789abcdef0ULL, persistenceHint("app"), persistenceHint("title")},
	    "monitor-hash", "desktop-guid", 1, true, false,
	});
	QVERIFY(writeSnapshot(path, snapshot));
	QCOMPARE(encodeSnapshot(readSnapshot(path)), encodeSnapshot(snapshot));
	QVERIFY(!QJsonDocument(encodeSnapshot(snapshot)).toJson().contains("\"title\":\"title\""));
	auto future = encodeSnapshot(snapshot);
	future.insert("version", 2);
	QVERIFY(decodeSnapshot(future).windows.isEmpty());
	snapshot.windows.clear();
	QVERIFY(writeSnapshot(path, snapshot));
	QVERIFY(readSnapshot(path).windows.isEmpty());
	QVERIFY(writeSnapshot({}, snapshot));
}

void TestTilingLayout::layoutStateRoundTrip_data() {
	QTest::addColumn<int>("kind");
	QTest::newRow("dwindle") << 0;
	QTest::newRow("master") << 1;
	QTest::newRow("scrolling") << 2;
}

void TestTilingLayout::layoutStateRoundTrip() {
	using namespace qs::windows::tiling;
	QFETCH(int, kind);
	auto layout = LayoutEngine::create(static_cast<Kind>(kind));
	Options options;
	layout->configure(options);
	layout->insert(A, 0, nullptr);
	layout->compute(AREA);
	layout->insert(B, A, nullptr, Edge::Bottom);
	layout->compute(AREA);
	layout->insert(C, A, nullptr, Edge::Left);
	layout->compute(AREA);
	layout->insert(D, B, nullptr);
	layout->compute(AREA);
	layout->swap(A, C);
	layout->splitRatio(B, 1.3, true);
	layout->resize(B, 35, 25);
	if (kind == 0) {
		layout->toggleSplit(B);
		layout->swapSplit(D);
	} else if (kind == 2) {
		auto& scroll = static_cast<ScrollingLayout&>(*layout);
		scroll.setMonitors(AREA, {});
		scroll.moveInto(B, Edge::Left);
		scroll.setColumnWidth(A, 0.7, false);
		scroll.activate(C);
		scroll.scrollBy(65);
	}
	layout->compute(AREA);
	// Serialize stable HWND tokens, then adopt different in-memory IDs.
	QHash<Id, Id> tokens {{A, 101}, {B, 102}, {C, 103}, {D, 104}};
	QHash<Id, Id> live {{101, 21}, {102, 22}, {103, 23}, {104, 24}};
	auto state = layout->saveState(tokens);
	auto restored = LayoutEngine::restoreState(static_cast<Kind>(kind), state, live);
	restored->configure(options);
	if (kind == 2) static_cast<ScrollingLayout&>(*restored).setMonitors(AREA, {});
	restored->compute(AREA);
	QList<Id> expectedOrder;
	for (auto id: layout->ids()) expectedOrder.append(id + 20);
	QCOMPARE(restored->ids(), expectedOrder);
	for (auto id: {A, B, C, D}) QCOMPARE(restored->box(id + 20), layout->box(id));
	QHash<Id, Id> again {{21, 101}, {22, 102}, {23, 103}, {24, 104}};
	QCOMPARE(restored->saveState(again), state);
}

void TestTilingLayout::staleLayoutLeavesCollapse_data() { layoutStateRoundTrip_data(); }

void TestTilingLayout::staleLayoutLeavesCollapse() {
	using namespace qs::windows::tiling;
	QFETCH(int, kind);
	auto layout = LayoutEngine::create(static_cast<Kind>(kind));
	layout->insert(A, 0, nullptr);
	layout->compute(AREA);
	layout->insert(B, A, nullptr);
	layout->compute(AREA);
	layout->insert(C, B, nullptr);
	layout->compute(AREA);
	auto state = layout->saveState({{A, 1}, {B, 2}, {C, 3}});
	auto restored = LayoutEngine::restoreState(static_cast<Kind>(kind), state, {{1, 21}, {3, 23}});
	restored->compute(AREA);
	QCOMPARE(restored->ids(), (QList<Id>{21, 23}));
	QVERIFY(!restored->box(21).isEmpty());
	QVERIFY(!restored->box(23).isEmpty());
	QVERIFY(LayoutEngine::restoreState(static_cast<Kind>(kind), state, {})->isEmpty());
}

void TestTilingLayout::dwindleDropSplitsTheChosenSide_data() {
	QTest::addColumn<QPoint>("cursor");
	QTest::addColumn<Edge>("side");
	QTest::addColumn<QRect>("inserted");
	QTest::newRow("left") << QPoint(-1190, 150) << Edge::Left << QRect(-1200, 0, 600, 300);
	QTest::newRow("right") << QPoint(-10, 150) << Edge::Right << QRect(-600, 0, 600, 300);
	QTest::newRow("top") << QPoint(-850, 15) << Edge::Top << QRect(-1200, 0, 1200, 150);
	QTest::newRow("bottom") << QPoint(-850, 285) << Edge::Bottom << QRect(-1200, 150, 1200, 150);
}

void TestTilingLayout::dwindleDropSplitsTheChosenSide() {
	QFETCH(QPoint, cursor);
	QFETCH(Edge, side);
	QFETCH(QRect, inserted);
	const auto area = QRect(-1200, 0, 1200, 300);
	QCOMPARE(qs::windows::tiling::dropSide(area, cursor, true), side);
	for (auto preserve: {false, true}) {
		DwindleLayout layout;
		Options options;
		options.preserveSplit = preserve;
		options.forceSplit = 1;
		layout.configure(options);
		layout.insert(A, 0, nullptr);
		layout.compute(area);
		layout.insert(B, A, nullptr, side);
		layout.compute(area);
		QCOMPARE(layout.box(B), inserted);
		QCOMPARE(layout.box(A).united(layout.box(B)), area);
		QVERIFY(!touches(layout.box(A), layout.box(B)));
		layout.compute(area);
		QCOMPARE(layout.box(B), inserted);
	}
}

// Without smart_split the target's aspect picks the axis, as in Hyprland's
// dwindle; the cursor only picks the side, however near an edge it is.
void TestTilingLayout::dwindleDropFollowsTheTargetAspect_data() {
	QTest::addColumn<QRect>("target");
	QTest::addColumn<QPoint>("cursor");
	QTest::addColumn<Edge>("side");
	const auto landscape = QRect(-3717, 36, 3681, 2124);
	const auto portrait = QRect(0, 0, 960, 1080);
	QTest::newRow("landscape-title-bar-left") << landscape << QPoint(-3000, 60) << Edge::Left;
	QTest::newRow("landscape-title-bar-right") << landscape << QPoint(-400, 60) << Edge::Right;
	QTest::newRow("landscape-bottom-left") << landscape << QPoint(-3000, 2140) << Edge::Left;
	QTest::newRow("portrait-left-edge-top") << portrait << QPoint(5, 300) << Edge::Top;
	QTest::newRow("portrait-left-edge-bottom") << portrait << QPoint(5, 800) << Edge::Bottom;
	QTest::newRow("square-is-side-by-side") << QRect(0, 0, 800, 800) << QPoint(400, 10) << Edge::Right;
}

void TestTilingLayout::dwindleDropFollowsTheTargetAspect() {
	QFETCH(QRect, target);
	QFETCH(QPoint, cursor);
	QFETCH(Edge, side);
	QCOMPARE(qs::windows::tiling::dropSide(target, cursor, false), side);
}

void TestTilingLayout::dwindleDropCollapsesTheSource() {
	DwindleLayout layout;
	layout.insert(A, 0, nullptr);
	layout.compute(AREA);
	layout.insert(B, A, nullptr, Edge::Right);
	layout.insert(C, B, nullptr, Edge::Bottom);
	layout.compute(AREA);
	const auto target = layout.box(B);
	const auto side =
	    qs::windows::tiling::dropSide(target, QPoint(target.center().x(), target.top()), true);
	layout.remove(A);
	layout.insert(A, B, nullptr, side);
	layout.compute(AREA);
	QCOMPARE(layout.count(), 3);
	QCOMPARE(layout.box(A), QRect(0, 0, 1000, 200));
	QCOMPARE(layout.box(B), QRect(0, 200, 1000, 200));
	QCOMPARE(layout.box(C), QRect(0, 400, 1000, 400));
}

void TestTilingLayout::dwindleEmptyDestinationAcceptsDrop() {
	DwindleLayout source;
	source.insert(A, 0, nullptr);
	source.insert(B, A, nullptr);
	source.compute(AREA);
	source.remove(A);
	source.compute(AREA);
	QCOMPARE(source.box(B), AREA);
	DwindleLayout destination;
	destination.insert(A, 0, nullptr);
	const auto other = AREA.translated(-1000, 0);
	destination.compute(other);
	QCOMPARE(destination.box(A), other);
	QVERIFY(!source.contains(A));
	QVERIFY(!destination.contains(B));
}

void TestTilingLayout::newColumnsGoRightOfTheFocusedOne() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	layout.activate(A);
	layout.insert(D, A, nullptr);
	QCOMPARE(layout.ids(), (QList<Id> {A, D, B, C}));
	QCOMPARE(layout.active(), A);
	QCOMPARE(layout.columnWidth(1), 0.5);

	layout.compute(AREA);
	QCOMPARE(layout.box(A), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(D), QRect(500, 0, 500, 800));
	QVERIFY(layout.isParked(B));
	QVERIFY(layout.isParked(C));
}

void TestTilingLayout::gapsKeepWindowSizesWhileScrolling() {
	ScrollingLayout layout;
	setup(layout);
	layout.setGaps(5, 10);
	fill(layout, {A, B, C});

	auto frame = [&](Id id) { return layout.box(id).adjusted(5, 5, -5, -5); };

	QCOMPARE(frame(A), QRect(10, 10, 485, 780));
	QCOMPARE(frame(B), QRect(505, 10, 485, 780));
	QCOMPARE(rightOf(frame(B)), 990);

	layout.activate(C);
	layout.compute(AREA);
	QCOMPARE(frame(B), QRect(10, 10, 485, 780));
	QCOMPARE(frame(C), QRect(505, 10, 485, 780));
	QCOMPARE(layout.box(C).size(), layout.visibleBox(A).size());

	auto full = Options();
	full.columnWidth = 1.0;
	layout.configure(full);
	layout.insert(D, C, nullptr);
	layout.activate(D);
	layout.compute(AREA);
	QCOMPARE(frame(D), QRect(10, 10, 980, 780));
}

void TestTilingLayout::parksColumnsOffTheScreen() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C, D});

	QVERIFY(!layout.isParked(A));
	QVERIFY(!layout.isParked(B));
	QVERIFY(layout.isParked(C));
	QVERIFY(layout.isParked(D));
	QCOMPARE(layout.box(C), QRect(1000 + ScrollingLayout::PARK_MARGIN, 0, 500, 800));
	QCOMPARE(layout.visibleBox(C), QRect(1000, 0, 500, 800));

	layout.activate(D);
	layout.compute(AREA);
	QVERIFY(layout.isParked(A));
	QVERIFY(layout.isParked(B));
	QCOMPARE(layout.box(A).left(), -ScrollingLayout::PARK_MARGIN - 500);
	QCOMPARE(layout.box(C), QRect(0, 0, 500, 800));
}

void TestTilingLayout::focusingScrollsTheLeastNeeded() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C, D});
	QCOMPARE(layout.view(), 0);

	layout.activate(B);
	QCOMPARE(layout.view(), 0);

	layout.activate(C);
	QCOMPARE(layout.view(), 500);

	layout.activate(D);
	QCOMPARE(layout.view(), 1000);

	layout.activate(A);
	QCOMPARE(layout.view(), 0);
}

void TestTilingLayout::centersWhenTheNeighbourDoesNotFit() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});
	for (auto id: {A, B, C}) layout.setColumnWidth(id, 2.0 / 3.0, false);
	layout.compute(AREA);
	QCOMPARE(layout.view(), 0);

	layout.activate(B);
	layout.compute(AREA);
	QCOMPARE(layout.view(), 500);
	QCOMPARE(layout.box(B), QRect(167, 0, 666, 800));
	QCOMPARE(layout.box(A), QRect(-500, 0, 667, 800));
	QVERIFY(!layout.isParked(A));
	QVERIFY(!layout.isParked(C));

	layout.activate(C);
	layout.compute(AREA);
	QCOMPARE(layout.view(), 1000);
	QCOMPARE(layout.box(C), QRect(333, 0, 667, 800));
}

void TestTilingLayout::alwaysAndNeverCenter() {
	ScrollingLayout always;
	setup(always, {}, CenterMode::Always);
	fill(always, {A, B, C});
	QCOMPARE(always.view(), -250);
	always.activate(B);
	QCOMPARE(always.view(), 250);
	always.compute(AREA);
	QCOMPARE(always.box(B), QRect(250, 0, 500, 800));

	ScrollingLayout never;
	setup(never, {}, CenterMode::Never);
	fill(never, {A, B, C});
	for (auto id: {A, B, C}) never.setColumnWidth(id, 2.0 / 3.0, false);
	never.activate(B);
	QCOMPARE(never.view(), 333);
}

void TestTilingLayout::peeksOnlyWhereNoMonitorIsNext() {
	auto left = QRect(-1920, 0, 1920, 1080);
	auto right = QRect(1000, 0, 1920, 1080);

	for (const auto& [others, parkA, parkC]: {
	         std::tuple {QList<QRect> {}, false, false},
	         std::tuple {QList<QRect> {left}, true, false},
	         std::tuple {QList<QRect> {right}, false, true},
	         std::tuple {QList<QRect> {left, right}, true, true},
	     })
	{
		ScrollingLayout layout;
		setup(layout, others);
		fill(layout, {A, B, C});
		for (auto id: {A, B, C}) layout.setColumnWidth(id, 2.0 / 3.0, false);
		layout.activate(B);
		layout.compute(AREA);

		QCOMPARE(layout.isParked(A), parkA);
		QCOMPARE(layout.isParked(C), parkC);
		QVERIFY(!layout.isParked(B));
		QCOMPARE(layout.box(B), QRect(167, 0, 666, 800));

		for (auto id: {A, C}) {
			for (const auto& other: others) QVERIFY(!touches(layout.box(id), other));
		}
	}
}

void TestTilingLayout::parkedWindowsStayOffEveryMonitor() {
	auto layouts = QList<QList<QRect>> {
	    {},
	    {QRect(-1920, 0, 1920, 1080)},
	    {QRect(1000, -200, 2560, 1440)},
	    {QRect(0, 800, 1000, 800)},
	    {QRect(0, -1080, 1920, 1080), QRect(1000, 0, 800, 600), QRect(-800, 300, 800, 600)},
	};

	for (const auto& others: layouts) {
		ScrollingLayout layout;
		setup(layout, others);
		fill(layout, {A, B, C, D});
		layout.activate(C);
		layout.compute(AREA);

		auto monitors = others;
		monitors.append(AREA);

		for (auto id: layout.ids()) {
			if (!layout.isParked(id)) continue;
			for (const auto& monitor: monitors) QVERIFY(!touches(layout.box(id), monitor));
			QCOMPARE(layout.box(id).size(), layout.visibleBox(id).size());
		}
	}
}

void TestTilingLayout::parkingKeepsTheOwnMonitorNearest() {
	ScrollingLayout layout;
	setup(layout, {QRect(-1920, 0, 1920, 1080)});
	fill(layout, {A, B, C});
	layout.activate(C);
	layout.compute(AREA);

	QVERIFY(layout.isParked(A));
	auto parked = layout.box(A);
	QVERIFY(bottomOf(parked) <= -ScrollingLayout::PARK_MARGIN);
	QCOMPARE(parked.size(), QSize(500, 800));

	ScrollingLayout single;
	setup(single);
	fill(single, {A, B, C});
	single.activate(C);
	single.compute(AREA);
	QCOMPARE(single.box(A), QRect(-ScrollingLayout::PARK_MARGIN - 500, 0, 500, 800));
}

void TestTilingLayout::removingAColumnClosesTheGap() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	layout.remove(B);
	layout.compute(AREA);
	QCOMPARE(layout.ids(), (QList<Id> {A, C}));
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));
	QVERIFY(!layout.isParked(C));
	QCOMPARE(layout.active(), A);

	layout.remove(A);
	layout.compute(AREA);
	QCOMPARE(layout.active(), C);
	QCOMPARE(layout.box(C), QRect(0, 0, 500, 800));
}

void TestTilingLayout::removingAColumnLeftOfTheViewKeepsTheView() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C, D});
	layout.activate(D);
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(0, 0, 500, 800));

	layout.remove(A);
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(D), QRect(500, 0, 500, 800));

	layout.remove(D);
	layout.compute(AREA);
	QCOMPARE(layout.active(), C);
	QCOMPARE(layout.box(B), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));
}

void TestTilingLayout::closingANewWindowReturnsToItsOpener() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	layout.insert(D, A, nullptr);
	layout.activate(D);
	layout.remove(D);
	QCOMPARE(layout.active(), A);

	layout.insert(D, A, nullptr);
	layout.activate(D);
	layout.activate(B);
	layout.activate(D);
	layout.remove(D);
	QCOMPARE(layout.active(), B);
}

void TestTilingLayout::movingAWindowSwapsOrExpels() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	QVERIFY(layout.moveWindow(A, Edge::Right));
	QCOMPARE(layout.ids(), (QList<Id> {B, A, C}));
	QVERIFY(layout.moveWindow(A, Edge::Right));
	QCOMPARE(layout.ids(), (QList<Id> {B, C, A}));
	QVERIFY(!layout.moveWindow(A, Edge::Right));

	QVERIFY(layout.moveInto(A, Edge::Left));
	QCOMPARE(layout.column(1), (QList<Id> {C, A}));
	QVERIFY(layout.moveWindow(A, Edge::Top));
	QCOMPARE(layout.column(1), (QList<Id> {A, C}));
	QVERIFY(!layout.moveWindow(A, Edge::Top));

	QVERIFY(layout.moveWindow(A, Edge::Left));
	QCOMPARE(layout.columnCount(), 3);
	QCOMPARE(layout.column(1), (QList<Id> {A}));
	QCOMPARE(layout.column(2), (QList<Id> {C}));
}

void TestTilingLayout::movingIntoAColumnStacks() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});
	layout.activate(B);

	QVERIFY(layout.moveInto(B, Edge::Left));
	QCOMPARE(layout.columnCount(), 2);
	QCOMPARE(layout.column(0), (QList<Id> {A, B}));
	QCOMPARE(layout.active(), B);

	layout.compute(AREA);
	QCOMPARE(layout.box(A), QRect(0, 0, 500, 400));
	QCOMPARE(layout.box(B), QRect(0, 400, 500, 400));

	QVERIFY(layout.moveInto(B, Edge::Right));
	QCOMPARE(layout.column(0), (QList<Id> {A}));
	QCOMPARE(layout.column(1), (QList<Id> {C, B}));

	QVERIFY(layout.moveInto(B, Edge::Right));
	QCOMPARE(layout.columnCount(), 3);
	QCOMPARE(layout.column(2), (QList<Id> {B}));
}

void TestTilingLayout::promoteAndSwapColumns() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	QVERIFY(!layout.promote(B));
	layout.moveInto(B, Edge::Left);
	QVERIFY(layout.promote(B));
	QCOMPARE(layout.ids(), (QList<Id> {A, B, C}));
	QCOMPARE(layout.columnCount(), 3);

	QVERIFY(layout.swapColumn(A, Edge::Left));
	QCOMPARE(layout.ids(), (QList<Id> {B, C, A}));
	QVERIFY(layout.swapColumn(A, Edge::Right));
	QCOMPARE(layout.ids(), (QList<Id> {A, B, C}));
	QVERIFY(layout.swapColumn(A, Edge::Right));
	QCOMPARE(layout.ids(), (QList<Id> {B, A, C}));
}

void TestTilingLayout::widthPresetsCycle() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A});

	layout.cycleWidth(A, 1);
	QCOMPARE(layout.columnWidth(0), 2.0 / 3.0);
	layout.cycleWidth(A, 1);
	QCOMPARE(layout.columnWidth(0), 1.0);
	layout.cycleWidth(A, 1);
	QCOMPARE(layout.columnWidth(0), 1.0 / 3.0);
	layout.cycleWidth(A, -1);
	QCOMPARE(layout.columnWidth(0), 1.0);
	layout.cycleWidth(A, -1);
	QCOMPARE(layout.columnWidth(0), 2.0 / 3.0);

	layout.setColumnWidth(A, 0.6, false);
	layout.cycleWidth(A, 1);
	QCOMPARE(layout.columnWidth(0), 2.0 / 3.0);

	layout.setColumnWidth(A, -0.1, true);
	QVERIFY(qAbs(layout.columnWidth(0) - (2.0 / 3.0 - 0.1)) < 1e-9);
	layout.setColumnWidth(A, 5.0, false);
	QCOMPARE(layout.columnWidth(0), ScrollingLayout::MAX_WIDTH);
	layout.setColumnWidth(A, 0.0, false);
	QCOMPARE(layout.columnWidth(0), ScrollingLayout::MIN_WIDTH);
}

void TestTilingLayout::fullWidthToggles() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B});
	layout.activate(B);

	QVERIFY(layout.toggleFullWidth(B));
	layout.compute(AREA);
	QCOMPARE(layout.columnWidth(1), 1.0);
	QCOMPARE(layout.box(B), QRect(0, 0, 1000, 800));
	QVERIFY(layout.isParked(A));

	QVERIFY(layout.toggleFullWidth(B));
	layout.compute(AREA);
	QCOMPARE(layout.columnWidth(1), 0.5);
	QVERIFY(!layout.isParked(B));

	layout.setColumnWidth(B, 1.0, false);
	layout.toggleFullWidth(B);
	QCOMPARE(layout.columnWidth(1), 0.5);
}

void TestTilingLayout::fitModes() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C, D});
	for (auto id: {A, B, C, D}) layout.setColumnWidth(id, 0.4, false);
	layout.compute(AREA);
	QVERIFY(!layout.isParked(C));
	QVERIFY(layout.isParked(D));

	QVERIFY(layout.fit(A, FitMode::Visible));
	layout.compute(AREA);
	for (auto c = 0; c < 3; c++) QCOMPARE(layout.columnWidth(c), 1.0 / 3.0);
	QCOMPARE(layout.box(A).left(), 0);
	QCOMPARE(rightOf(layout.box(C)), 1000);
	QVERIFY(!layout.isParked(C));

	QVERIFY(layout.fit(A, FitMode::All));
	layout.compute(AREA);
	for (auto c = 0; c < 4; c++) QCOMPARE(layout.columnWidth(c), 0.25);
	QCOMPARE(rightOf(layout.box(D)), 1000);

	layout.activate(C);
	QVERIFY(layout.fit(C, FitMode::ToEnd));
	layout.compute(AREA);
	QCOMPARE(layout.columnWidth(2), 0.5);
	QCOMPARE(layout.box(C).left(), 0);
	QCOMPARE(rightOf(layout.box(D)), 1000);

	QVERIFY(layout.fit(C, FitMode::ToBeginning));
	layout.compute(AREA);
	QCOMPARE(layout.columnWidth(0), 1.0 / 3.0);
	QCOMPARE(rightOf(layout.box(C)), 1000);

	QVERIFY(layout.fit(C, FitMode::Active));
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(0, 0, 1000, 800));
}

void TestTilingLayout::scrollingByColumnsAndPixels() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C, D});

	QVERIFY(layout.scrollColumns(1));
	QCOMPARE(layout.active(), B);
	QCOMPARE(layout.view(), 0);
	QVERIFY(layout.scrollColumns(2));
	QCOMPARE(layout.active(), D);
	QCOMPARE(layout.view(), 1000);
	QVERIFY(!layout.scrollColumns(1));

	layout.activate(A);
	QCOMPARE(layout.view(), 0);
	QVERIFY(layout.scrollBy(600));
	QCOMPARE(layout.view(), 600);
	QCOMPARE(layout.active(), C);

	QVERIFY(layout.scrollBy(-50));
	QCOMPARE(layout.view(), 550);
	QCOMPARE(layout.active(), C);

	ScrollingLayout wide;
	setup(wide);
	fill(wide, {A, B});
	wide.setColumnWidth(A, 1.0, false);
	wide.setColumnWidth(B, 1.0, false);
	QVERIFY(wide.scrollBy(300));
	QCOMPARE(wide.active(), A);
	QCOMPARE(wide.view(), 0);
}

void TestTilingLayout::resizingAndEdges() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});
	layout.activate(B);

	QVERIFY(layout.resize(A, 100, 0));
	QCOMPARE(layout.columnWidth(0), 0.6);

	layout.setColumnWidth(A, 0.5, false);
	layout.activate(C);
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));

	QVERIFY(layout.hasEdge(C, Edge::Left));
	QVERIFY(!layout.hasEdge(C, Edge::Top));
	QVERIFY(layout.moveEdge(C, Edge::Left, 400));
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(400, 0, 600, 800));

	QVERIFY(layout.moveEdge(C, Edge::Right, 900));
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(400, 0, 500, 800));

	layout.moveInto(C, Edge::Left);
	layout.compute(AREA);
	QVERIFY(layout.hasEdge(C, Edge::Top));
	QVERIFY(!layout.hasEdge(C, Edge::Bottom));
	QVERIFY(layout.moveEdge(B, Edge::Bottom, 300));
	layout.compute(AREA);
	QCOMPARE(layout.box(B).height(), 300);
	QCOMPARE(layout.box(C).height(), 500);

	QVERIFY(layout.resize(C, 0, 100));
	layout.compute(AREA);
	QCOMPARE(layout.box(C).height(), 600);
	QCOMPARE(layout.box(B).height(), 200);
}

void TestTilingLayout::hiddenWindowsTakeNoSpace() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	layout.setHidden(B, true);
	layout.compute(AREA);
	QCOMPARE(layout.box(B), QRect());
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));
	QCOMPARE(layout.neighbor(A, Edge::Right, false), C);

	layout.setHidden(B, false);
	layout.compute(AREA);
	QCOMPARE(layout.box(B), QRect(500, 0, 500, 800));
	QVERIFY(layout.isParked(C));
}

void TestTilingLayout::removingAHiddenWindowKeepsTheView() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C, D});
	layout.activate(C);
	layout.compute(AREA);
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));

	layout.setHidden(B, true);
	layout.compute(AREA);
	QCOMPARE(layout.box(A), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));

	layout.remove(B);
	layout.compute(AREA);
	QCOMPARE(layout.box(A), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));
	QVERIFY(layout.isParked(D));
}

void TestTilingLayout::neighboursWrapOnRequest() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});

	QCOMPARE(layout.neighbor(A, Edge::Left, false), Id(0));
	QCOMPARE(layout.neighbor(A, Edge::Left, true), C);
	QCOMPARE(layout.neighbor(C, Edge::Right, true), A);
	QCOMPARE(layout.neighbor(A, Edge::Top, true), Id(0));

	layout.moveInto(B, Edge::Left);
	layout.activate(A);
	QCOMPARE(layout.neighbor(A, Edge::Bottom, false), B);
	QCOMPARE(layout.neighbor(B, Edge::Bottom, true), A);

	layout.activate(B);
	QCOMPARE(layout.neighbor(C, Edge::Left, false), B);
}

void TestTilingLayout::swapKeepsTheView() {
	ScrollingLayout layout;
	setup(layout);
	fill(layout, {A, B, C});
	layout.activate(B);
	layout.compute(AREA);

	layout.swap(B, A);
	layout.compute(AREA);
	QCOMPARE(layout.ids(), (QList<Id> {B, A, C}));
	QCOMPARE(layout.box(B), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(A), QRect(500, 0, 500, 800));
	QCOMPARE(layout.active(), B);

	QVERIFY(layout.replace(B, D));
	QCOMPARE(layout.active(), D);
	QCOMPARE(layout.ids(), (QList<Id> {D, A, C}));
}

void TestTilingLayout::adoptedWindowsHaveNeighbours() {
	ScrollingLayout layout;
	setup(layout, {QRect(1000, 0, 1000, 800)});

	layout.insert(A, 0, nullptr);
	layout.insert(B, 0, nullptr);
	layout.insert(C, 0, nullptr);
	layout.activate(C);
	layout.insert(D, 0, nullptr);
	layout.compute(AREA);

	QCOMPARE(layout.ids(), (QList<Id> {A, C, D, B}));
	QCOMPARE(layout.active(), C);
	QCOMPARE(layout.box(A), QRect(0, 0, 500, 800));
	QCOMPARE(layout.box(C), QRect(500, 0, 500, 800));

	QCOMPARE(layout.neighbor(C, Edge::Left, false), A);
	QCOMPARE(layout.neighbor(C, Edge::Right, false), D);
	QCOMPARE(layout.neighbor(A, Edge::Right, false), C);
	QCOMPARE(layout.neighbor(B, Edge::Left, false), D);
	QCOMPARE(layout.neighbor(A, Edge::Left, false), Id(0));

	QVERIFY(layout.activate(A));
	layout.compute(AREA);
	QCOMPARE(layout.view(), 0);
	QVERIFY(layout.activate(D));
	layout.compute(AREA);
	QCOMPARE(layout.box(D), QRect(500, 0, 500, 800));
	QVERIFY(layout.isParked(A));
}

QTEST_APPLESS_MAIN(TestTilingLayout)

#include "tiling_layout_test.moc"
