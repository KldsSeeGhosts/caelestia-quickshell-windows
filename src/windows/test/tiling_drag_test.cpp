#include <utility>

#include <qguiapplication.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../tiling.hpp"
#include "tiling_fake_desktop.hpp"

// Drag and drop of tiled windows against the production tiler and the fake
// desktop: what a release does, and that the preview the shell draws during
// the drag is exactly what the release then places.
namespace qs::windows {

namespace {

class DragScreen: public QPlatformScreen {
public:
	DragScreen(QRect geometry, qreal ratio, QString name)
	    : rect(geometry)
	    , ratio(ratio)
	    , label(std::move(name)) {}
	[[nodiscard]] QRect geometry() const override { return this->rect; }
	[[nodiscard]] int depth() const override { return 32; }
	[[nodiscard]] QImage::Format format() const override { return QImage::Format_RGB32; }
	[[nodiscard]] QString name() const override { return this->label; }
	[[nodiscard]] qreal devicePixelRatio() const override { return this->ratio; }
	[[nodiscard]] QDpi logicalDpi() const override { return {96, 96}; }

private:
	QRect rect;
	qreal ratio;
	QString label;
};

// The screen-local logical rectangle the shell draws for a physical one.
QRectF logical(const QRect& physical, const QRect& monitor, qreal dpr) {
	return {
	    (physical.x() - monitor.x()) / dpr,
	    (physical.y() - monitor.y()) / dpr,
	    physical.width() / dpr,
	    physical.height() / dpr
	};
}

} // namespace

class TilingDragTest: public QObject {
	Q_OBJECT;

	using Mode = TilingManager::DropMode;
	using Edge = TilingManager::Edge;

	TilingManager* manager = nullptr;
	QList<TrackedWindow*> windows;
	QList<DragScreen*> extraScreens;

	TrackedWindow* add(quintptr id, int monitor = 0) {
		auto* window = new TrackedWindow(id);
		fakeDesktop.windows.insert(window->hwnd(), FakeWindow {});
		fakeWindow(window->hwnd()).monitor = monitor;
		this->windows.append(window);
		WindowTracker::instance()->fixtures.append(window);
		return window;
	}

	QScreen* addMonitor(const QRect& physical, qreal dpr, const QString& name) {
		auto* platform = new DragScreen(QRect(physical.topLeft(), physical.size() / dpr), dpr, name);
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		if (fakeDesktop.screens.isEmpty()) fakeDesktop.screens = {QGuiApplication::primaryScreen()};
		fakeDesktop.monitors.append({physical, physical, true});
		fakeDesktop.screens.append(platform->screen());
		this->extraScreens.append(platform);
		return platform->screen();
	}

	void enable() {
		manager->setEnabled(true);
		manager->sync();
	}

	[[nodiscard]] QRect box(TrackedWindow* window) const {
		const auto& m = manager->managed.at(window);
		return m.layout == nullptr ? QRect() : m.layout->tree->box(TilingManager::idOf(window));
	}

	static QRect& frame(TrackedWindow* window) { return fakeWindow(window->hwnd()).frame; }

	// A title-bar drag holds the window 12 px below its top edge; a Super drag
	// grabs wherever the cursor is, here the middle of the window.
	void begin(TrackedWindow* window, bool super) {
		const auto& rect = frame(window);
		fakeDesktop.cursor = super ? rect.center() : QPoint(rect.center().x(), rect.top() + 12);
		emit WindowTracker::instance()->moveSizeStarted(window, super);
	}

	// The window follows the cursor, as in a native move loop or a Super drag,
	// and is on whichever monitor the cursor is on.
	void moveTo(TrackedWindow* window, const QPoint& cursor) {
		auto& native = fakeWindow(window->hwnd());
		native.frame.translate(cursor - fakeDesktop.cursor);
		fakeDesktop.cursor = cursor;
		for (qsizetype i = 0; i < fakeDesktop.monitors.size(); i++) {
			if (fakeDesktop.monitors.at(i).monitor.contains(cursor)) native.monitor = static_cast<int>(i);
		}
		emit WindowTracker::instance()->movingLocationChanged(window);
	}

	void finish(TrackedWindow* window) {
		emit WindowTracker::instance()->moveSizeEnded(window);
		manager->sync();
	}

	void dragTo(TrackedWindow* window, const QPoint& cursor, bool super) {
		this->begin(window, super);
		this->moveTo(window, cursor);
		this->finish(window);
	}

	// Past the gaps (doubled at DPR 2), inside the tile's frame.
	static QPoint inBand(const QRect& box, int side) {
		auto point = box.center();
		switch (side) {
		case 0: point.setX(box.left() + 60); break;
		case 1: point.setX(box.right() - 60); break;
		case 2: point.setY(box.top() + 60); break;
		case 3: point.setY(box.bottom() - 60); break;
		default: break;
		}
		return point;
	}

	// A title-bar drag and a Super drag follow the same cursor rule.
	static void gestures() {
		QTest::addColumn<bool>("super");
		QTest::newRow("caption") << false;
		QTest::newRow("super") << true;
	}

private slots:
	void init() {
		fakeDesktop = {};
		manager = new TilingManager(QStringLiteral("Local\\QuickshellTiling-drag-fixture"));
	}

	void cleanup() {
		manager->setEnabled(false);
		delete manager;
		manager = nullptr;
		for (auto* platform: this->extraScreens) QWindowSystemInterface::handleScreenRemoved(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		this->extraScreens.clear();
		qDeleteAll(this->windows);
		this->windows.clear();
		WindowTracker::instance()->fixtures.clear();
	}

	// The reported case: two side-by-side tiles are each portrait, so the old
	// drop always stacked them. The tile's shape decides nothing now: the
	// centre swaps, and only an explicit edge band inserts on that side.
	void captionDropSideBySide_data() {
		QTest::addColumn<int>("spot");
		QTest::newRow("centre") << 0;
		QTest::newRow("upper-centre") << 1;
		QTest::newRow("top-band") << 2;
		QTest::newRow("corner") << 3;
	}

	void captionDropSideBySide() {
		QFETCH(int, spot);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto tileB = frame(b);
		QVERIFY(boxA.right() < boxB.left());
		QVERIFY(tileB.height() > tileB.width());
		const auto band = qMin(tileB.width() / 5, 120);
		auto cursor = tileB.center();
		// The upper half of a portrait tile, short of its top band.
		if (spot == 1) cursor.setY(tileB.top() + band + 20);
		if (spot == 2) cursor.setY(tileB.top() + 12);
		// Equally deep in the right and bottom bands: the tie goes across.
		if (spot == 3) cursor = tileB.bottomRight() - QPoint(11, 11);
		begin(a, false);
		moveTo(a, cursor);
		const auto plan = manager->drag.plan;
		QCOMPARE(plan.target, b);
		finish(a);
		const auto ra = box(a);
		const auto rb = box(b);
		QCOMPARE(manager->managed.at(a).layout->tree->ids().size(), 2);
		QCOMPARE(frame(a), plan.landing);
		switch (spot) {
		case 0:
		case 1:
			QCOMPARE(plan.mode, Mode::Swap);
			QCOMPARE(ra, boxB);
			QCOMPARE(rb, boxA);
			QVERIFY(frame(a).left() > frame(b).right());
			break;
		case 2:
			QCOMPARE(plan.mode, Mode::Insert);
			QVERIFY(plan.side == Edge::Top);
			QCOMPARE(ra.bottom() + 1, rb.top());
			QCOMPARE(ra.left(), rb.left());
			break;
		case 3:
			QCOMPARE(plan.mode, Mode::Insert);
			QVERIFY(plan.side == Edge::Right);
			QCOMPARE(rb.right() + 1, ra.left());
			QCOMPARE(ra.top(), rb.top());
			break;
		default: break;
		}
	}

	void centreSwaps_data() { gestures(); }

	void centreSwaps() {
		QFETCH(bool, super);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		dragTo(a, boxB.center(), super);
		QCOMPARE(box(a), boxB);
		QCOMPARE(box(b), boxA);
	}

	void edgeInserts_data() {
		QTest::addColumn<bool>("super");
		QTest::addColumn<bool>("landscape");
		QTest::addColumn<int>("side");
		const char* sides[] = {"left", "right", "top", "bottom"};
		for (auto super: {false, true}) {
			for (auto landscape: {false, true}) {
				for (int side = 0; side < 4; side++) {
					auto name = QByteArray(super ? "super-" : "caption-") + (landscape ? "landscape-" : "portrait-")
					          + sides[side];
					QTest::newRow(name.constData()) << super << landscape << side;
				}
			}
		}
	}

	void edgeInserts() {
		QFETCH(bool, super);
		QFETCH(bool, landscape);
		QFETCH(int, side);
		// A portrait monitor stacks two tiles, so each is landscape.
		if (landscape) fakeDesktop.monitors[0] = {{0, 0, 1080, 1920}, {0, 0, 1080, 1920}, true};
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto work = fakeDesktop.monitors[0].work;
		const auto boxB = box(b);
		QCOMPARE(boxB.width() > boxB.height(), landscape);
		begin(a, super);
		moveTo(a, inBand(boxB, side));
		QCOMPARE(manager->drag.plan.mode, Mode::Insert);
		QCOMPARE(manager->dragMode(), QStringLiteral("insert"));
		finish(a);
		const auto ra = box(a);
		const auto rb = box(b);
		switch (side) {
		case 0: QCOMPARE(ra.right() + 1, rb.left()); QCOMPARE(ra.top(), rb.top()); break;
		case 1: QCOMPARE(rb.right() + 1, ra.left()); QCOMPARE(ra.top(), rb.top()); break;
		case 2: QCOMPARE(ra.bottom() + 1, rb.top()); QCOMPARE(ra.left(), rb.left()); break;
		case 3: QCOMPARE(rb.bottom() + 1, ra.top()); QCOMPARE(ra.left(), rb.left()); break;
		default: break;
		}
		QCOMPARE(ra.united(rb), work);
	}

	// Jitter across a zone boundary and across a tile boundary: the 8 dp
	// hysteresis keeps the previous result, so the preview does not flicker.
	void hysteresisNoFlicker_data() { gestures(); }

	void hysteresisNoFlicker() {
		QFETCH(bool, super);
		auto* a = add(1);
		auto* b = add(2);
		auto* c = add(3);
		enable();
		// Tiles are hit by their frames, so measure those.
		const auto boxB = frame(b);
		const auto boxC = frame(c);
		// A | (B over C); the band is 20% of B's width, capped at 120 dp.
		QCOMPARE(box(b).bottom() + 1, box(c).top());
		const auto band = qMin(boxB.width() / 5, 120);
		const auto y = boxB.center().y();

		// A band is entered 8 dp past its boundary and left 8 dp past it.
		begin(a, super);
		moveTo(a, QPoint(boxB.left() + band + 20, y));
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);
		moveTo(a, QPoint(boxB.left() + band - 3, y));
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);
		moveTo(a, QPoint(boxB.left() + band - 10, y));
		QCOMPARE(manager->drag.plan.mode, Mode::Insert);
		QVERIFY(manager->drag.plan.side == Edge::Left);
		moveTo(a, QPoint(boxB.left() + band + 3, y));
		QCOMPARE(manager->drag.plan.mode, Mode::Insert);
		moveTo(a, QPoint(boxB.left() + band + 10, y));
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);
		manager->cancelDrag(a);
		finish(a);

		begin(a, super);
		moveTo(a, QPoint(boxB.left() + band + 20, boxB.center().y()));
		QVERIFY(manager->dragActive());
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);
		QCOMPARE(manager->drag.plan.target, b);
		QSignalSpy zone(manager, &TilingManager::dragChanged);
		for (int i = 0; i < 50; i++) {
			moveTo(a, QPoint(boxB.left() + band + (i % 2 == 0 ? -3 : 3), boxB.top() + 100 + i));
		}
		QVERIFY2(zone.count() <= 1, qPrintable(QString::number(zone.count())));
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);

		// Across the middle of the gap between B and C.
		const auto gap = (boxB.bottom() + boxC.top()) / 2;
		QSignalSpy tile(manager, &TilingManager::dragChanged);
		for (int i = 0; i < 50; i++) {
			moveTo(a, QPoint(boxB.center().x() + i, gap + (i % 2 == 0 ? -3 : 3)));
		}
		QVERIFY2(tile.count() <= 1, qPrintable(QString::number(tile.count())));
		finish(a);
	}

	// Master and scrolling `insert` ignore the side, so both gestures swap.
	void masterAndScrollingAlwaysSwap_data() {
		QTest::addColumn<QString>("layout");
		QTest::addColumn<bool>("super");
		for (const auto* layout: {"master", "scrolling"}) {
			for (auto super: {false, true}) {
				auto name = QByteArray(layout) + (super ? "-super" : "-caption");
				QTest::newRow(name.constData()) << QString::fromLatin1(layout) << super;
			}
		}
	}

	void masterAndScrollingAlwaysSwap() {
		QFETCH(QString, layout);
		QFETCH(bool, super);
		manager->setLayout(layout);
		auto* a = add(1);
		auto* b = add(2);
		auto* c = add(3);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto boxC = box(c);
		begin(a, super);
		moveTo(a, inBand(boxB, 0));
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);
		// These layouts reflow, so no ghost is drawn.
		QVERIFY(manager->dragDisplaced().isNull());
		finish(a);
		QCOMPARE(box(a), boxB);
		QCOMPARE(box(b), boxA);
		QCOMPARE(box(c), boxC);
	}

	// The preview is the result: what the shell is shown during the drag is
	// where sync() puts the windows after the release.
	void planEqualsCommit_data() {
		QTest::addColumn<QString>("layout");
		QTest::addColumn<QString>("op");
		QTest::addColumn<bool>("super");
		QTest::addColumn<qreal>("dpr");
		const char* ops[] = {
		    "swap", "insert-left", "insert-right", "insert-top", "insert-bottom",
		    "home", "move", "cross", "cross-insert",
		};
		for (const auto* layout: {"dwindle", "master", "scrolling"}) {
			for (const auto* op: ops) {
				for (auto super: {false, true}) {
					for (auto dpr: {2.0, 1.5, 1.75}) {
						auto name = QByteArray(layout) + '-' + op + (super ? "-super-" : "-caption-")
						          + QByteArray::number(dpr);
						QTest::newRow(name.constData())
						    << QString::fromLatin1(layout) << QString::fromLatin1(op) << super << qreal(dpr);
					}
				}
			}
		}
	}

	void planEqualsCommit() {
		QFETCH(QString, layout);
		QFETCH(QString, op);
		QFETCH(bool, super);
		QFETCH(qreal, dpr);
		manager->setLayout(layout);
		// A second monitor left of and above the primary one, at an odd
		// negative origin and a whole or fractional DPR.
		auto* hidpi = addMonitor(QRect(-3840, -403, 3840, 2160), dpr, QStringLiteral("fixture-hidpi"));
		QCOMPARE(hidpi->devicePixelRatio(), dpr);
		auto* a = add(1, 0);
		auto* b = add(2, 0);
		TrackedWindow* c = nullptr;
		if (op != QStringLiteral("move")) c = add(3, 1);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto dwindle = layout == QStringLiteral("dwindle");

		auto cursor = boxB.center();
		auto expected = Mode::Swap;
		TrackedWindow* target = b;
		if (op.startsWith(QStringLiteral("insert-"))) {
			const QStringList sides {"insert-left", "insert-right", "insert-top", "insert-bottom"};
			cursor = inBand(boxB, static_cast<int>(sides.indexOf(op)));
			if (dwindle) expected = Mode::Insert;
		} else if (op == QStringLiteral("home")) {
			cursor = boxA.center() + QPoint(40, 40);
			expected = Mode::Home;
			target = nullptr;
		} else if (op == QStringLiteral("move")) {
			cursor = QPoint(-1920, 600);
			expected = Mode::Move;
			target = nullptr;
		} else if (op == QStringLiteral("cross")) {
			cursor = box(c).center();
			target = c;
		} else if (op == QStringLiteral("cross-insert")) {
			cursor = inBand(box(c), 0);
			target = c;
			if (dwindle) expected = Mode::Insert;
		}

		begin(a, super);
		moveTo(a, cursor);
		QVERIFY(manager->dragActive());
		const auto plan = manager->drag.plan;
		QCOMPARE(plan.mode, expected);
		QCOMPARE(plan.target, target);
		const auto landing = manager->dragLanding();
		const auto displaced = manager->dragDisplaced();
		const auto screen = manager->dragScreen();
		finish(a);
		QVERIFY(!manager->dragActive());

		if (expected == Mode::Home) {
			QVERIFY(landing.isNull());
			QCOMPARE(box(a), boxA);
			QCOMPARE(box(b), boxB);
			return;
		}

		auto* destination = manager->managed.at(a).layout;
		QVERIFY(destination != nullptr);
		QCOMPARE(destination->screen->name(), screen);
		const auto index = fakeDesktop.screens.indexOf(destination->screen);
		const auto monitor = fakeDesktop.monitors.at(index < 0 ? 0 : index).monitor;
		const auto scale = destination->screen->devicePixelRatio();
		QCOMPARE(frame(a), plan.landing);
		QCOMPARE(landing, logical(plan.landing, monitor, scale));
		// A move to the empty monitor draws its landing too.
		QVERIFY(!landing.isEmpty());
		if (!plan.displaced.isNull()) {
			QVERIFY(target != nullptr);
			QCOMPARE(frame(target), plan.displaced);
			QCOMPARE(displaced, logical(plan.displaced, monitor, scale));
		} else {
			QVERIFY(displaced.isNull());
		}
		// A same-screen dwindle swap always shows where the other window goes;
		// master and scrolling never draw a ghost.
		if (op == QStringLiteral("swap") && dwindle) QVERIFY(!plan.displaced.isNull());
		if (!dwindle) QVERIFY(plan.displaced.isNull());
	}

	// The edge band is 120 dp at the target's DPR: one physical pixel inside it
	// inserts, its boundary swaps.
	void bandScalesWithTargetDpi_data() {
		QTest::addColumn<qreal>("dpr");
		QTest::addColumn<bool>("inside");
		for (auto dpr: {1.0, 1.5, 1.75, 2.0}) {
			for (auto inside: {true, false}) {
				auto name = QByteArray::number(dpr) + (inside ? "-inside" : "-boundary");
				QTest::newRow(name.constData()) << qreal(dpr) << inside;
			}
		}
	}

	void bandScalesWithTargetDpi() {
		QFETCH(qreal, dpr);
		QFETCH(bool, inside);
		addMonitor(QRect(-3840, -403, 3840, 2160), dpr, QStringLiteral("fixture-scaled"));
		auto* a = add(1, 0);
		add(2, 0);
		auto* c = add(3, 1);
		enable();
		const auto tile = frame(c);
		const auto cap = static_cast<int>(std::lround(120 * dpr));
		QVERIFY(tile.width() / 5 > cap && tile.height() / 5 > cap);
		begin(a, false);
		moveTo(a, QPoint(tile.left() + cap - (inside ? 1 : 0), tile.center().y()));
		QCOMPARE(manager->drag.plan.target, c);
		QCOMPARE(manager->drag.plan.mode, inside ? Mode::Insert : Mode::Swap);
		if (inside) QVERIFY(manager->drag.plan.side == Edge::Left);
		const auto landing = manager->drag.plan.landing;
		finish(a);
		QCOMPARE(frame(a), landing);
	}

	// Equally deep in two bands of a square-capped tile: the horizontal band
	// wins, the same way every time.
	void cornerTieIsDeterministic_data() {
		QTest::addColumn<int>("corner");
		QTest::newRow("top-left") << 0;
		QTest::newRow("top-right") << 1;
		QTest::newRow("bottom-left") << 2;
		QTest::newRow("bottom-right") << 3;
	}

	void cornerTieIsDeterministic() {
		QFETCH(int, corner);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto tile = frame(b);
		QVERIFY(tile.width() / 5 >= 120 && tile.height() / 5 >= 120);
		const auto right = corner % 2 == 1;
		const auto bottom = corner >= 2;
		const QPoint cursor(
		    right ? tile.right() - 5 : tile.left() + 5,
		    bottom ? tile.bottom() - 5 : tile.top() + 5
		);
		for (int attempt = 0; attempt < 2; attempt++) {
			begin(a, attempt == 1);
			moveTo(a, cursor);
			QCOMPARE(manager->drag.plan.mode, Mode::Insert);
			QVERIFY(manager->drag.plan.side == (right ? Edge::Right : Edge::Left));
			manager->cancelDrag(a);
			finish(a);
		}
	}

	// The release commits the plan the preview showed. The cursor moving on
	// without a location event, so with no new plan shown, changes nothing.
	void releaseCommitsShownPlan_data() {
		QTest::addColumn<bool>("super");
		QTest::addColumn<int>("drift");
		const char* drifts[] = {"swap-then-edge", "insert-then-centre", "swap-then-own-tile"};
		for (auto super: {false, true}) {
			for (int drift = 0; drift < 3; drift++) {
				auto name = QByteArray(super ? "super-" : "caption-") + drifts[drift];
				QTest::newRow(name.constData()) << super << drift;
			}
		}
	}

	void releaseCommitsShownPlan() {
		QFETCH(bool, super);
		QFETCH(int, drift);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto shown = drift == 1 ? Mode::Insert : Mode::Swap;
		begin(a, super);
		moveTo(a, drift == 1 ? inBand(boxB, 2) : boxB.center());
		QCOMPARE(manager->drag.plan.mode, shown);
		QCOMPARE(manager->drag.plan.target, b);
		const auto landing = manager->drag.plan.landing;
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		switch (drift) {
		case 0: fakeDesktop.cursor = inBand(boxB, 2); break;
		case 1: fakeDesktop.cursor = boxB.center(); break;
		case 2: fakeDesktop.cursor = boxA.center(); break;
		default: break;
		}
		finish(a);
		QCOMPARE(changed.count(), 1);
		QCOMPARE(frame(a), landing);
		const auto ra = box(a);
		const auto rb = box(b);
		if (shown == Mode::Swap) {
			QCOMPARE(ra, boxB);
			QCOMPARE(rb, boxA);
		} else {
			QCOMPARE(ra.bottom() + 1, rb.top());
			QCOMPARE(ra.left(), rb.left());
		}
	}

	// The target reflows under the still cursor so that the same cursor would
	// now insert instead of swap. The shown intent is gone: the drop is home,
	// and the release does not substitute the new one.
	void reflowedIntentIsHome_data() { gestures(); }

	void reflowedIntentIsHome() {
		QFETCH(bool, super);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		begin(a, super);
		// Just short of B's top band.
		moveTo(a, QPoint(frame(b).center().x(), frame(b).top() + 125));
		QCOMPARE(manager->drag.plan.mode, Mode::Swap);
		QCOMPARE(manager->drag.plan.target, b);
		const auto top = frame(b).top();
		// A larger outer gap moves B's top edge 30 px towards the cursor.
		manager->setGapsOut(40);
		manager->sync();
		QCOMPARE(frame(b).top(), top + 30);
		QCOMPARE(manager->drag.plan.mode, Mode::Home);
		QVERIFY(manager->dragLanding().isNull());
		const auto boxA = box(a);
		const auto boxB = box(b);
		finish(a);
		QCOMPARE(box(a), boxA);
		QCOMPARE(box(b), boxB);
		QVERIFY(box(a).right() < box(b).left());
	}

	// With window contents hidden while dragging there are no location events
	// and no preview; the release plans once, by the same cursor rule.
	void noPreviewPlansAtRelease_data() {
		QTest::addColumn<bool>("super");
		QTest::addColumn<bool>("edge");
		for (auto super: {false, true}) {
			for (auto edge: {false, true}) {
				auto name = QByteArray(super ? "super-" : "caption-") + (edge ? "edge" : "centre");
				QTest::newRow(name.constData()) << super << edge;
			}
		}
	}

	void noPreviewPlansAtRelease() {
		QFETCH(bool, super);
		QFETCH(bool, edge);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		begin(a, super);
		const auto cursor = edge ? inBand(boxB, 2) : boxB.center();
		frame(a).translate(cursor - fakeDesktop.cursor);
		fakeDesktop.cursor = cursor;
		finish(a);
		QCOMPARE(changed.count(), 0);
		const auto ra = box(a);
		const auto rb = box(b);
		if (edge) {
			QCOMPARE(ra.bottom() + 1, rb.top());
			QCOMPARE(ra.left(), rb.left());
		} else {
			QCOMPARE(ra, boxB);
			QCOMPARE(rb, boxA);
		}
	}

	// A shown plan withdrawn when its target closed is not planned again by
	// the release fallback, even after the cursor moved over another tile.
	void withdrawnPlanStaysHome_data() { gestures(); }

	void withdrawnPlanStaysHome() {
		QFETCH(bool, super);
		auto* a = add(1);
		auto* b = add(2);
		auto* c = add(3);
		enable();
		const auto boxA = box(a);
		begin(a, super);
		moveTo(a, box(b).center());
		QCOMPARE(manager->drag.plan.target, b);
		manager->unmanage(b);
		QVERIFY(!manager->dragActive());
		const auto cursor = box(c).center();
		frame(a).translate(cursor - fakeDesktop.cursor);
		fakeDesktop.cursor = cursor;
		finish(a);
		QCOMPARE(box(a), boxA);
		QCOMPARE(manager->managed.at(c).layout, manager->managed.at(a).layout);
		QVERIFY(box(c).left() > boxA.right());
	}

	// A window still maximized when its drag ends is left to Windows.
	void maximizedDragEndCommitsNothing_data() { gestures(); }

	void maximizedDragEndCommitsNothing() {
		QFETCH(bool, super);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		fakeWindow(a->hwnd()).maximized = true;
		emit a->maximizedChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(a)), TilingManager::State::Suspended);
		begin(a, super);
		QCOMPARE(manager->dragging, a);
		moveTo(a, boxB.center());
		finish(a);
		QCOMPARE(box(a), boxA);
		QCOMPARE(box(b), boxB);
	}

	// Aero Snap can resize the frame during a title-bar move. A drag that
	// started as a move still follows the plan instead of moving split edges.
	void snapResizedMoveFollowsPlan() {
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto tile = frame(a);
		begin(a, false);
		moveTo(a, boxB.center());
		QCOMPARE(manager->drag.kind, TilingManager::DragKind::Move);
		// Snap gives it B's upper half; its top edge stays where it was.
		frame(a) = QRect(boxB.left(), tile.top(), boxB.width(), boxB.height() / 2);
		emit WindowTracker::instance()->movingLocationChanged(a);
		finish(a);
		QCOMPARE(box(a), boxB);
		QCOMPARE(box(b), boxA);
	}

	void borderResizeKeepsMoveEdge() {
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		begin(a, false);
		// The right border follows the cursor 100 px into B.
		auto& native = fakeWindow(a->hwnd());
		native.frame.setWidth(native.frame.width() + 100);
		fakeDesktop.cursor = QPoint(native.frame.right(), native.frame.center().y());
		emit WindowTracker::instance()->movingLocationChanged(a);
		QCOMPARE(manager->drag.kind, TilingManager::DragKind::Resize);
		QVERIFY(!manager->dragActive());
		finish(a);
		QCOMPARE(changed.count(), 0);
		QCOMPARE(box(a).left(), boxA.left());
		QVERIFY(box(a).width() > boxA.width() + 50);
		QVERIFY(box(b).left() > boxA.right());
	}

	void noPlanWithoutMotion_data() {
		QTest::addColumn<bool>("escape");
		QTest::newRow("click") << false;
		QTest::newRow("escape") << true;
	}

	void noPlanWithoutMotion() {
		QFETCH(bool, escape);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto tile = frame(a);
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		begin(a, false);
		if (escape) {
			moveTo(a, boxB.center());
			QVERIFY(manager->dragActive());
			// Esc puts the frame back while the cursor is still over B.
			frame(a) = tile;
			emit WindowTracker::instance()->movingLocationChanged(a);
		} else {
			// A click inside the drag threshold is not a drag.
			fakeDesktop.cursor += QPoint(1, 1);
			emit WindowTracker::instance()->movingLocationChanged(a);
		}
		finish(a);
		QVERIFY(!manager->dragActive());
		QCOMPARE(changed.count(), escape ? 2 : 0);
		QCOMPARE(box(a), boxA);
		QCOMPARE(box(b), boxB);
		QCOMPARE(frame(a), tile);
	}

	// A | (B over C). Dragging A over B, C closes: B grows to the full height.
	// The preview follows, and the release places what it showed.
	void previewFollowsLayoutChange() {
		auto* a = add(1);
		auto* b = add(2);
		auto* c = add(3);
		enable();
		begin(a, false);
		moveTo(a, box(b).center());
		QCOMPARE(manager->drag.plan.target, b);
		const auto before = manager->drag.plan.landing;
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		manager->unmanage(c);
		manager->sync();
		QCOMPARE(changed.count(), 1);
		// One pixel inside the cached zone.
		moveTo(a, fakeDesktop.cursor + QPoint(1, 0));
		const auto plan = manager->drag.plan;
		QCOMPARE(plan.target, b);
		QVERIFY(plan.landing.height() > before.height());
		finish(a);
		QCOMPARE(frame(a), plan.landing);
	}

	// Minimizing the target during the drag expands C under the still cursor.
	// The release does not pick C: the drop is home.
	void invalidatedTargetIsHome_data() {
		QTest::addColumn<bool>("synced");
		QTest::newRow("synced") << true;
		QTest::newRow("unsynced") << false;
	}

	void invalidatedTargetIsHome() {
		QFETCH(bool, synced);
		auto* a = add(1);
		auto* b = add(2);
		auto* c = add(3);
		enable();
		const auto boxA = box(a);
		const auto tile = frame(a);
		begin(a, false);
		moveTo(a, box(b).center());
		QCOMPARE(manager->drag.plan.target, b);
		fakeWindow(b->hwnd()).minimized = true;
		emit b->minimizedChanged();
		if (synced) {
			manager->sync();
			QVERIFY(box(c).contains(fakeDesktop.cursor));
			QCOMPARE(manager->drag.plan.mode, Mode::Home);
			QVERIFY(manager->dragLanding().isNull());
		}
		finish(a);
		QCOMPARE(box(a), boxA);
		QCOMPARE(frame(a), tile);
	}

	// A location event for the still cursor after the invalidation reports the
	// dragged window, not motion: it must not pick the tile that expanded under
	// that cursor. Only real motion past the drag threshold re-plans.
	void stationaryLocationEventKeepsInvalidation() {
		auto* a = add(1);
		auto* b = add(2);
		auto* c = add(3);
		enable();
		const auto boxA = box(a);
		const auto tile = frame(a);
		begin(a, false);
		moveTo(a, box(b).center());
		QCOMPARE(manager->drag.plan.target, b);
		const auto still = fakeDesktop.cursor;
		fakeWindow(b->hwnd()).minimized = true;
		emit b->minimizedChanged();
		manager->sync();
		QCOMPARE(manager->drag.plan.mode, Mode::Home);
		QVERIFY(manager->drag.invalidated);
		QVERIFY(box(c).contains(still));

		// The delayed location event: same frame, same cursor.
		emit WindowTracker::instance()->movingLocationChanged(a);
		QVERIFY(manager->drag.invalidated);
		QCOMPARE(manager->drag.plan.mode, Mode::Home);
		QCOMPARE(manager->drag.plan.target, nullptr);

		// Real motion past the threshold does plan the tile under the cursor,
		// and the release places what the preview showed.
		const auto threshold = manager->drag.threshold;
		moveTo(a, still + QPoint(threshold.width() + 2, threshold.height() + 2));
		QVERIFY(!manager->drag.invalidated);
		QCOMPARE(manager->drag.plan.target, c);
		const auto plan = manager->drag.plan;
		finish(a);
		QCOMPARE(plan.target, c);
		QCOMPARE(frame(a), plan.landing);
		QVERIFY(box(a) != boxA);
		QVERIFY(frame(a) != tile);
	}

	// The gap between two frames is home: no tile is drawn there.
	void gapIsHome_data() { gestures(); }

	void gapIsHome() {
		QFETCH(bool, super);
		manager->setGapsIn(10);
		manager->setGapsOut(20);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto spot = QPoint(boxB.left() + 1, boxB.center().y());
		QVERIFY(!frame(b).contains(spot));
		begin(a, super);
		moveTo(a, spot);
		QCOMPARE(manager->drag.plan.mode, Mode::Home);
		finish(a);
		QCOMPARE(box(a), boxA);
		QCOMPARE(box(b), boxB);
	}

	// Esc during a Super drag: the preview clears, later motion plans nothing
	// and the release commits nothing.
	void superEscapeCancels() {
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		begin(a, true);
		moveTo(a, boxB.center());
		QVERIFY(manager->dragActive());
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		manager->cancelDrag(a);
		QVERIFY(!manager->dragActive());
		QCOMPARE(changed.count(), 1);
		moveTo(a, boxB.center() + QPoint(30, 30));
		QVERIFY(!manager->dragActive());
		finish(a);
		QCOMPARE(changed.count(), 1);
		QCOMPARE(box(a), boxA);
		QCOMPARE(box(b), boxB);
	}

	void dragClearsOnUnmanageStopDisable_data() {
		QTest::addColumn<int>("action");
		QTest::newRow("unmanage-dragged") << 0;
		QTest::newRow("unmanage-target") << 1;
		QTest::newRow("disable") << 2;
	}

	void dragClearsOnUnmanageStopDisable() {
		QFETCH(int, action);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		begin(a, false);
		moveTo(a, box(b).center());
		QVERIFY(manager->dragActive());
		QVERIFY(!manager->dragLanding().isNull());
		QSignalSpy changed(manager, &TilingManager::dragChanged);
		switch (action) {
		case 0: manager->unmanage(a); break;
		case 1: manager->unmanage(b); break;
		case 2: manager->setEnabled(false); break;
		default: break;
		}
		QVERIFY(!manager->dragActive());
		QVERIFY(manager->dragLanding().isNull());
		QCOMPARE(changed.count(), 1);
		if (action == 1) {
			// The drag goes on over the target's old slot: nothing to swap with.
			moveTo(a, fakeDesktop.cursor + QPoint(10, 10));
			QCOMPARE(manager->drag.plan.target, nullptr);
		}
	}
};

} // namespace qs::windows

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QGuiApplication app(argc, argv);
	qs::windows::TilingDragTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "tiling_drag_test.moc"
