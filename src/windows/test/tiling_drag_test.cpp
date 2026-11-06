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
	// drop always stacked them. A title-bar drop anywhere over B swaps.
	void captionDropSwapsSideBySide_data() {
		QTest::addColumn<int>("spot");
		QTest::newRow("centre") << 0;
		QTest::newRow("top-12") << 1;
		QTest::newRow("corner") << 2;
	}

	void captionDropSwapsSideBySide() {
		QFETCH(int, spot);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		QVERIFY(boxA.right() < boxB.left());
		QVERIFY(boxB.height() > boxB.width());
		auto cursor = boxB.center();
		if (spot == 1) cursor.setY(boxB.top() + 12);
		if (spot == 2) cursor = boxB.bottomRight() - QPoint(12, 12);
		dragTo(a, cursor, false);
		QCOMPARE(box(a), boxB);
		QCOMPARE(box(b), boxA);
		QCOMPARE(manager->managed.at(a).layout->tree->ids().size(), 2);
		QVERIFY(frame(a).left() > frame(b).right());
	}

	void superCentreSwaps() {
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		dragTo(a, boxB.center(), true);
		QCOMPARE(box(a), boxB);
		QCOMPARE(box(b), boxA);
	}

	void superEdgeInserts_data() {
		QTest::addColumn<bool>("landscape");
		QTest::addColumn<int>("side");
		const char* sides[] = {"left", "right", "top", "bottom"};
		for (auto landscape: {false, true}) {
			for (int side = 0; side < 4; side++) {
				auto name = QByteArray(landscape ? "landscape-" : "portrait-") + sides[side];
				QTest::newRow(name.constData()) << landscape << side;
			}
		}
	}

	void superEdgeInserts() {
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
		dragTo(a, inBand(boxB, side), true);
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
	void superHysteresisNoFlicker() {
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

		begin(a, true);
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
		const char* ops[] = {
		    "swap", "insert-left", "insert-right", "insert-top", "insert-bottom",
		    "home", "move", "cross", "cross-insert",
		};
		for (const auto* layout: {"dwindle", "master", "scrolling"}) {
			for (const auto* op: ops) {
				auto name = QByteArray(layout) + '-' + op;
				QTest::newRow(name.constData()) << QString::fromLatin1(layout) << QString::fromLatin1(op);
			}
		}
	}

	void planEqualsCommit() {
		QFETCH(QString, layout);
		QFETCH(QString, op);
		manager->setLayout(layout);
		// A second monitor left of and above the primary one at DPR 2.
		auto* hidpi = addMonitor(QRect(-3840, -400, 3840, 2160), 2.0, QStringLiteral("fixture-hidpi"));
		QCOMPARE(hidpi->devicePixelRatio(), 2.0);
		auto* a = add(1, 0);
		auto* b = add(2, 0);
		TrackedWindow* c = nullptr;
		if (op != QStringLiteral("move")) c = add(3, 1);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto dwindle = layout == QStringLiteral("dwindle");

		auto super = op.contains(QStringLiteral("insert"));
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
		const auto dpr = destination->screen->devicePixelRatio();
		QCOMPARE(frame(a), plan.landing);
		QCOMPARE(landing, logical(plan.landing, monitor, dpr));
		if (!plan.displaced.isNull()) {
			QVERIFY(target != nullptr);
			QCOMPARE(frame(target), plan.displaced);
			QCOMPARE(displaced, logical(plan.displaced, monitor, dpr));
		} else {
			QVERIFY(displaced.isNull());
		}
		// A same-screen dwindle swap always shows where the other window goes;
		// master and scrolling never draw a ghost.
		if (op == QStringLiteral("swap") && dwindle) QVERIFY(!plan.displaced.isNull());
		if (!dwindle) QVERIFY(plan.displaced.isNull());
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
	void gapIsHome() {
		manager->setGapsIn(10);
		manager->setGapsOut(20);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		const auto boxA = box(a);
		const auto boxB = box(b);
		const auto spot = QPoint(boxB.left() + 1, boxB.center().y());
		QVERIFY(!frame(b).contains(spot));
		begin(a, false);
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
