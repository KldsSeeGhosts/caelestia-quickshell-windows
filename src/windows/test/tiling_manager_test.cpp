#include <cstddef>

#include <qguiapplication.h>
#include <qloggingcategory.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <qsignalspy.h>
#include <qscopeguard.h>
#include <qtest.h>

#include "../fullscreen_geometry.hpp"
#include "../hyprland/pin_support.hpp"
#include "../tiling.hpp"
#include "../tiling_scope.hpp"
#include "tiling_fake_desktop.hpp"

namespace qs::windows {

namespace {
QStringList completionLogs;
void captureCompletion(QtMsgType, const QMessageLogContext&, const QString& message) {
	if (message.startsWith(QStringLiteral("Completed placement"))) completionLogs.append(message);
}

class FakeScreen: public QPlatformScreen {
public:
	explicit FakeScreen(QRect geometry): rect(geometry) {}
	QRect geometry() const override { return this->rect; }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_RGB32; }
	QString name() const override { return QStringLiteral("fixture-secondary"); }
	qreal devicePixelRatio() const override { return 1.0; }
	QDpi logicalDpi() const override { return {96, 96}; }

private:
	QRect rect;
};
} // namespace

class TilingManagerTest: public QObject {
	Q_OBJECT;

	TilingManager* manager = nullptr;
	QList<TrackedWindow*> windows;

	TrackedWindow* add(quintptr id, QString app = QStringLiteral("editor")) {
		auto* window = new TrackedWindow(id);
		fakeDesktop.windows.insert(window->hwnd(), FakeWindow {});
		fakeWindow(window->hwnd()).appId = app;
		windows.append(window);
		WindowTracker::instance()->fixtures.append(window);
		return window;
	}

	void enable() {
		manager->setEnabled(true);
		manager->sync();
	}
	void drain() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
	// The scheduled look at a window that is waiting to settle.
	void recheck(TrackedWindow* window) {
		manager->markWindow(window);
		manager->sync();
	}
	void finishSnap() {
		fakeDesktop.now += 200;
		manager->stepSnaps();
	}
	// Run verification, easing, settle re-checks and deadlines for a few
	// seconds of fake time, as the manager's timers would.
	void drive(const QList<TrackedWindow*>& list, int steps = 12) {
		for (int i = 0; i < steps; i++) {
			fakeDesktop.now += 300;
			for (auto* window: list) {
				if (manager->managed.contains(window)) manager->onRectChanged(window);
			}
			manager->verify();
			manager->stepSnaps();
			manager->onDeadline();
			manager->sync();
		}
	}
	void close(TrackedWindow* window) {
		fakeWindow(window->hwnd()).exists = false;
		manager->unmanage(window);
		WindowTracker::instance()->fixtures.removeAll(window);
		manager->sync();
	}
	// A second 1920x1080 monitor to the left of the primary one.
	FakeScreen* addLeftMonitor() {
		auto* platform = new FakeScreen(QRect(-1920, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		fakeDesktop.monitors = {
		    {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true},
		    {{-1920, 0, 1920, 1080}, {-1920, 0, 1920, 1080}, true}};
		fakeDesktop.screens = {QGuiApplication::primaryScreen(), platform->screen()};
		return platform;
	}
	void removeMonitor(FakeScreen* platform) {
		manager->setEnabled(false);
		QWindowSystemInterface::handleScreenRemoved(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
	}

private slots:
	void acceptedPlacementNeedsAnObservation() {
		auto* window = add(1);
		fakeWindow(window->hwnd()).ignoreMoves = true;
		QSignalSpy reported(manager->telemetry(), &tiling::TilingTelemetry::placementReported);
		enable();
		drain();
		QCOMPARE(reported.count(), 1);
		const auto accepted = qvariant_cast<tiling::PlacementOutcome>(reported.first().first());
		QCOMPARE(accepted.disposition, tiling::PlacementDisposition::Accepted);
		QCOMPARE(accepted.identity.window, quintptr(1));
		QVERIFY(accepted.identity.lifetime != 0);
		QVERIFY(accepted.identity.layoutGeneration != 0);
		QVERIFY(accepted.frame != accepted.target);

		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 1);
		fakeWindow(window->hwnd()).frame = accepted.target;
		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 2);
		const auto observed = qvariant_cast<tiling::PlacementOutcome>(reported.last().first());
		QCOMPARE(observed.disposition, tiling::PlacementDisposition::Landed);
		QCOMPARE(observed.identity, accepted.identity);
		QCOMPARE(observed.frame, accepted.target);
		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 2);
	}

	void newLayoutSupersedesQueuedObservation_data() {
		QTest::addColumn<int>("callback");
		QTest::newRow("verification") << 0;
		QTest::newRow("deadline") << 1;
		QTest::newRow("animation") << 2;
	}

	void newLayoutSupersedesQueuedObservation() {
		QFETCH(int, callback);
		auto* window = add(1);
		QSignalSpy reported(manager->telemetry(), &tiling::TilingTelemetry::placementReported);
		enable();
		manager->onRectChanged(window);
		manager->setSingleWindowGapsOut(50);
		switch (callback) {
		case 0: manager->verify(); break;
		case 1:
			fakeDesktop.now += 2000;
			manager->onDeadline();
			break;
		case 2: manager->stepSnaps(); break;
		}
		drain();
		QCOMPARE(reported.count(), 3);
		const auto first = qvariant_cast<tiling::PlacementOutcome>(reported.at(0).first());
		const auto superseded = qvariant_cast<tiling::PlacementOutcome>(reported.at(1).first());
		const auto replacement = qvariant_cast<tiling::PlacementOutcome>(reported.at(2).first());
		QCOMPARE(first.disposition, tiling::PlacementDisposition::Accepted);
		QCOMPARE(superseded.disposition, tiling::PlacementDisposition::Superseded);
		QCOMPARE(superseded.identity, first.identity);
		QCOMPARE(replacement.disposition, tiling::PlacementDisposition::Accepted);
		QCOMPARE(replacement.identity.lifetime, first.identity.lifetime);
		QVERIFY(replacement.identity.layoutGeneration > first.identity.layoutGeneration);
		QCOMPARE(replacement.target, QRect(50, 50, 1820, 980));
		QCOMPARE(fakeWindow(window->hwnd()).frame, replacement.target);
		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 4);
		const auto observed = qvariant_cast<tiling::PlacementOutcome>(reported.last().first());
		QCOMPARE(observed.disposition, tiling::PlacementDisposition::Landed);
		QCOMPARE(observed.identity, replacement.identity);
		QCOMPARE(observed.frame, replacement.target);
		QCOMPARE(manager->telemetry()->failedCount(), 0);
	}

	void selfResizeStartsNewPlacementEpisode_data() {
		QTest::addColumn<bool>("newTile");
		QTest::newRow("correction-lands") << false;
		QTest::newRow("settle-superseded") << true;
	}

	void selfResizeStartsNewPlacementEpisode() {
		QFETCH(bool, newTile);
		auto* window = add(1);
		QSignalSpy reported(manager->telemetry(), &tiling::TilingTelemetry::placementReported);
		enable();
		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 2);
		auto previous = qvariant_cast<tiling::PlacementOutcome>(reported.last().first());
		QCOMPARE(previous.disposition, tiling::PlacementDisposition::Landed);

		for (int episode = 0; episode < 2; episode++) {
			reported.clear();
			fakeWindow(window->hwnd()).frame = QRect(40, 60, 600 + episode, 400);
			manager->onRectChanged(window);
			manager->verify();
			if (newTile) {
				manager->setSingleWindowGapsOut(50 + 10 * episode);
				manager->sync();
			} else {
				fakeDesktop.now += 250;
				recheck(window);
				finishSnap();
			}
			manager->onRectChanged(window);
			manager->verify();
			drain();
			QCOMPARE(reported.count(), newTile ? 3 : 2);
			const auto first = qvariant_cast<tiling::PlacementOutcome>(reported.first().first());
			QCOMPARE(first.disposition, newTile ? tiling::PlacementDisposition::Superseded
			                                  : tiling::PlacementDisposition::Accepted);
			QCOMPARE(first.identity.lifetime, previous.identity.lifetime);
			QVERIFY(first.identity.layoutGeneration > previous.identity.layoutGeneration);
			QCOMPARE(first.target, previous.target);
			const auto accepted = qvariant_cast<tiling::PlacementOutcome>(reported.at(newTile ? 1 : 0).first());
			QCOMPARE(accepted.disposition, tiling::PlacementDisposition::Accepted);
			if (newTile) QVERIFY(accepted.identity.layoutGeneration > first.identity.layoutGeneration);
			const auto landed = qvariant_cast<tiling::PlacementOutcome>(reported.last().first());
			QCOMPARE(landed.disposition, tiling::PlacementDisposition::Landed);
			QCOMPARE(landed.identity, accepted.identity);
			QCOMPARE(landed.frame, landed.target);
			previous = landed;
		}
	}

	void newTileReportsSupersededSnap() {
		auto* window = add(1);
		QSignalSpy reported(manager->telemetry(), &tiling::TilingTelemetry::placementReported);
		enable();
		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 2);
		const auto initial = qvariant_cast<tiling::PlacementOutcome>(reported.last().first());
		QCOMPARE(initial.disposition, tiling::PlacementDisposition::Landed);
		reported.clear();

		const auto resized = QRect(40, 60, 600, 400);
		fakeWindow(window->hwnd()).frame = resized;
		manager->onRectChanged(window);
		manager->verify();
		fakeDesktop.now += 250;
		recheck(window);
		fakeDesktop.now += 30;
		manager->stepSnaps();
		QVERIFY(fakeWindow(window->hwnd()).frame != resized);
		QVERIFY(fakeWindow(window->hwnd()).frame != initial.target);
		drain();
		QCOMPARE(reported.count(), 0);

		manager->setSingleWindowGapsOut(50);
		manager->sync();
		manager->onRectChanged(window);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 3);
		const auto superseded = qvariant_cast<tiling::PlacementOutcome>(reported.at(0).first());
		const auto accepted = qvariant_cast<tiling::PlacementOutcome>(reported.at(1).first());
		const auto landed = qvariant_cast<tiling::PlacementOutcome>(reported.at(2).first());
		QCOMPARE(superseded.disposition, tiling::PlacementDisposition::Superseded);
		QCOMPARE(superseded.identity.lifetime, initial.identity.lifetime);
		QVERIFY(superseded.identity.layoutGeneration > initial.identity.layoutGeneration);
		QCOMPARE(superseded.target, initial.target);
		QCOMPARE(accepted.disposition, tiling::PlacementDisposition::Accepted);
		QVERIFY(accepted.identity.layoutGeneration > superseded.identity.layoutGeneration);
		QCOMPARE(accepted.target, QRect(50, 50, 1820, 980));
		QCOMPARE(landed.disposition, tiling::PlacementDisposition::Landed);
		QCOMPARE(landed.identity, accepted.identity);
		QCOMPARE(landed.frame, accepted.target);
	}

	void slowSelfResizeCannotRenewItsCorrectionBudget() {
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		enable();
		manager->onRectChanged(window);
		manager->verify();
		native.ignoreMoves = true;
		const auto moves = native.moves;
		for (int i = 0; i < 20; i++) {
			fakeDesktop.now += 1700;
			native.frame = QRect(40, 60, 2400 + i, 1400);
			manager->onRectChanged(window);
			manager->verify();
			finishSnap();
		}
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		// It grew itself away from its slot's corner: off target, it floats.
		QCOMPARE(manager->managed.at(window).disposition, tiling::PlacementDisposition::OffTarget);
		QVERIFY(!manager->isTiled(window));
		// Two easings, two corrections, one floating restore.
		QVERIFY(native.moves - moves <= 5);
	}

	void reusedHandleCannotCompleteTheOldLifetime() {
		auto* old = add(1);
		QSignalSpy reported(manager->telemetry(), &tiling::TilingTelemetry::placementReported);
		enable();
		manager->onRectChanged(old);
		manager->unmanage(old);
		WindowTracker::instance()->fixtures.removeAll(old);
		auto* replacement = add(1);
		manager->manage(replacement);
		manager->verify();
		manager->onRectChanged(replacement);
		manager->verify();
		drain();
		QCOMPARE(reported.count(), 4);
		const auto initial = qvariant_cast<tiling::PlacementOutcome>(reported.at(0).first());
		const auto vanished = qvariant_cast<tiling::PlacementOutcome>(reported.at(1).first());
		const auto accepted = qvariant_cast<tiling::PlacementOutcome>(reported.at(2).first());
		const auto observed = qvariant_cast<tiling::PlacementOutcome>(reported.at(3).first());
		QCOMPARE(vanished.disposition, tiling::PlacementDisposition::Vanished);
		QCOMPARE(vanished.identity, initial.identity);
		QCOMPARE(accepted.disposition, tiling::PlacementDisposition::Accepted);
		QVERIFY(accepted.identity.lifetime != initial.identity.lifetime);
		QCOMPARE(observed.disposition, tiling::PlacementDisposition::Landed);
		QCOMPARE(observed.identity, accepted.identity);
	}

	void refusedPlacementIsReportedImmediately_data() {
		QTest::addColumn<quint32>("error");
		QTest::addColumn<QString>("reason");
		QTest::newRow("access-denied") << quint32(ERROR_ACCESS_DENIED) << QStringLiteral("placement-denied");
		QTest::newRow("api-failure") << quint32(ERROR_NOT_ENOUGH_MEMORY) << QStringLiteral("placement-failed");
	}

	void refusedPlacementReflowsNeighbours_data() {
		QTest::addColumn<quint32>("error");
		QTest::addColumn<bool>("easing");
		QTest::newRow("denied-direct") << quint32(ERROR_ACCESS_DENIED) << false;
		QTest::newRow("failed-direct") << quint32(ERROR_NOT_ENOUGH_MEMORY) << false;
		QTest::newRow("denied-easing") << quint32(ERROR_ACCESS_DENIED) << true;
		QTest::newRow("failed-easing") << quint32(ERROR_NOT_ENOUGH_MEMORY) << true;
	}

	void refusedPlacementReflowsNeighbours() {
		QFETCH(quint32, error);
		QFETCH(bool, easing);
		auto* neighbour = add(1);
		auto* refused = add(2);
		if (!easing) fakeWindow(refused->hwnd()).moveError = error;
		enable();
		if (easing) {
			manager->onRectChanged(refused);
			manager->verify();
			fakeWindow(refused->hwnd()).frame = QRect(40, 60, 600, 400);
			manager->onRectChanged(refused);
			manager->verify();
			fakeDesktop.now += 250;
			recheck(refused);
			fakeWindow(refused->hwnd()).moveError = error;
			fakeDesktop.now += 30;
			manager->stepSnaps();
		}
		manager->sync();
		QVERIFY(!manager->isTiled(refused));
		QVERIFY(manager->isTiled(neighbour));
		QCOMPARE(fakeWindow(neighbour->hwnd()).frame, QRect(20, 20, 1880, 1040));
		QCOMPARE(manager->telemetry()->managedCount(), 1);
		QCOMPARE(manager->telemetry()->failedCount(), 1);
	}

	void refusedPlacementIsReportedImmediately() {
		QFETCH(quint32, error);
		QFETCH(QString, reason);
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		const auto original = native.frame;
		native.moveError = error;
		QSignalSpy failed(manager->telemetry(), &tiling::TilingTelemetry::windowTilingFailed);
		QSignalSpy reported(manager->telemetry(), &tiling::TilingTelemetry::placementReported);
		enable();
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		QCOMPARE(native.frame, original);
		for (int i = 0; i < 8; i++) {
			manager->onRectChanged(window);
			manager->verify();
			fakeDesktop.now += 2000;
			manager->onDeadline();
		}
		QCOMPARE(native.moves, 2); // The refused placement and one floating restore.
		drain();
		QCOMPARE(failed.count(), 1);
		QCOMPARE(failed.first().at(1).toString(), reason);
		QCOMPARE(reported.count(), 1);
		const auto outcome = qvariant_cast<tiling::PlacementOutcome>(reported.first().first());
		QCOMPARE(outcome.disposition, error == ERROR_ACCESS_DENIED
		                                  ? tiling::PlacementDisposition::Denied
		                                  : tiling::PlacementDisposition::Failed);
		QCOMPARE(outcome.nativeError, error);
	}

	void hungPlacementHasAFiniteDeadline() {
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		native.ignoreMoves = true;
		native.hung = true;
		QSignalSpy failed(manager->telemetry(), &tiling::TilingTelemetry::windowTilingFailed);
		enable();
		fakeDesktop.now += 2000;
		manager->onDeadline();
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		manager->sync();
		// The deadline is telemetry: no floating restore, the window keeps its slot.
		QCOMPARE(native.moves, 1);
		QVERIFY(manager->isTiled(window));
		for (int i = 0; i < 8; i++) {
			fakeDesktop.now += 2000;
			manager->onDeadline();
		}
		QCOMPARE(native.moves, 1);
		QVERIFY(manager->isTiled(window));
		drain();
		QCOMPARE(failed.count(), 1);
		QCOMPARE(failed.first().at(1).toString(), QStringLiteral("unresponsive"));
		QCOMPARE(manager->managed.at(window).disposition, tiling::PlacementDisposition::Deadline);
	}

	void closingPlacementReportsFailureWithoutCorrection() {
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		native.ignoreMoves = true;
		QSignalSpy failed(manager->telemetry(), &tiling::TilingTelemetry::windowTilingFailed);
		enable();
		native.exists = false;
		manager->onRectChanged(window);
		manager->verify();
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		fakeDesktop.now += 4000;
		manager->onDeadline();
		QCOMPARE(native.moves, 1);
		drain();
		QCOMPARE(failed.count(), 1);
		QCOMPARE(failed.first().at(1).toString(), QStringLiteral("target-vanished"));
		QCOMPARE(manager->managed.at(window).disposition, tiling::PlacementDisposition::Vanished);
	}

	void constrainedPlacementNeverLogsCompletion() {
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		native.minimumSize = QSize(2400, 1400);
		enable();
		completionLogs.clear();
		QLoggingCategory::setFilterRules(QStringLiteral("quickshell.windows.tiling.debug=true"));
		auto previousHandler = qInstallMessageHandler(captureCompletion);
		const auto restoreLogging = qScopeGuard([&]() {
			qInstallMessageHandler(previousHandler);
			QLoggingCategory::setFilterRules(QString());
		});
		for (int i = 0; i < 30; i++) {
			fakeDesktop.now += 300;
			manager->onRectChanged(window);
			manager->verify();
			manager->stepSnaps();
			manager->onDeadline();
		}
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		QCOMPARE(manager->managed.at(window).disposition, tiling::PlacementDisposition::SizeConstraint);
		QVERIFY(manager->isTiled(window));
		QVERIFY(completionLogs.isEmpty());
		QVERIFY(native.moves <= 5);
	}

	void offTargetGiveUpNeverLogsCompletion() {
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		native.maximumSize = QSize(1800, 1000);
		enable();
		completionLogs.clear();
		QLoggingCategory::setFilterRules(QStringLiteral("quickshell.windows.tiling.debug=true"));
		auto previousHandler = qInstallMessageHandler(captureCompletion);
		const auto restoreLogging = qScopeGuard([&]() {
			qInstallMessageHandler(previousHandler);
			QLoggingCategory::setFilterRules(QString());
		});
		manager->giveUp(manager->managed.at(window), native.frame);
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		QCOMPARE(manager->managed.at(window).disposition, tiling::PlacementDisposition::CorrectionLimit);
		QVERIFY(completionLogs.isEmpty());
		QVERIFY(native.moves <= 5);
	}

	void dropsInsertBesideTarget_data() {
		QTest::addColumn<bool>("crossMonitor");
		QTest::addColumn<int>("side");
		for (auto cross: {false, true}) {
			for (int side = 0; side < 4; side++) {
				auto name = QByteArray(cross ? "cross-" : "same-") + QByteArray::number(side);
				QTest::newRow(name.constData()) << cross << side;
			}
		}
	}

	void dropsInsertBesideTarget() {
		QFETCH(bool, crossMonitor);
		QFETCH(int, side);
		auto* platform = new FakeScreen(QRect(-1920, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		const auto cleanupScreen = qScopeGuard([&]() {
			manager->setEnabled(false);
			QWindowSystemInterface::handleScreenRemoved(platform);
			QWindowSystemInterface::flushWindowSystemEvents();
		});
		fakeDesktop.monitors = {
		    {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true},
		    {{-1920, 0, 1920, 1080}, {-1920, 0, 1920, 1080}, true}};
		fakeDesktop.screens = {QGuiApplication::primaryScreen(), platform->screen()};
		// A Super drag into an edge band inserts on that side. A title-bar drop
		// swaps instead (titleBarDropOnLandscapeTileSwaps).

		auto* dragged = add(1);
		auto* sourceNeighbour = add(2);
		auto* target = add(3);
		auto* targetNeighbour = add(4);
		fakeWindow(dragged->hwnd()).monitor = 0;
		fakeWindow(sourceNeighbour->hwnd()).monitor = 0;
		fakeWindow(target->hwnd()).monitor = crossMonitor ? 1 : 0;
		fakeWindow(targetNeighbour->hwnd()).monitor = crossMonitor ? 1 : 0;
		enable();
		auto* source = manager->managed.at(dragged).layout;
		auto* destination = manager->managed.at(target).layout;
		const auto untouched = destination->tree->box(manager->idOf(targetNeighbour));
		// Just inside the target's frame; the gap around it is not the tile.
		const auto targetBox = fakeWindow(target->hwnd()).frame;
		auto cursor = targetBox.center();
		switch (side) {
		case 0: cursor.setX(targetBox.left() + 1); break;
		case 1: cursor.setX(targetBox.right() - 1); break;
		case 2: cursor.setY(targetBox.top() + 1); break;
		case 3: cursor.setY(targetBox.bottom() - 1); break;
		}
		manager->onMoveSizeStarted(dragged, true);
		QCOMPARE(manager->dragging, dragged);
		fakeDesktop.cursor = cursor;
		fakeWindow(dragged->hwnd()).frame.moveTopLeft(cursor - QPoint(100, 50));
		manager->onMoveSizeEnded(dragged);
		manager->sync();
		QCOMPARE(manager->managed.at(dragged).layout, destination);
		QCOMPARE(manager->managed.at(target).layout, destination);
		QCOMPARE(destination->tree->ids().size(), crossMonitor ? 3 : 4);
		if (crossMonitor) {
			QCOMPARE(source->tree->ids(), QList<quintptr> {manager->idOf(sourceNeighbour)});
			QCOMPARE(destination->tree->box(manager->idOf(targetNeighbour)), untouched);
			QCOMPARE(fakeWindow(sourceNeighbour->hwnd()).frame, QRect(20, 20, 1880, 1040));
		}
		auto a = destination->tree->box(manager->idOf(dragged));
		auto b = destination->tree->box(manager->idOf(target));
		switch (side) {
		case 0: QCOMPARE(a.right() + 1, b.left()); QCOMPARE(a.top(), b.top()); break;
		case 1: QCOMPARE(b.right() + 1, a.left()); QCOMPARE(a.top(), b.top()); break;
		case 2: QCOMPARE(a.bottom() + 1, b.top()); QCOMPARE(a.left(), b.left()); break;
		case 3: QCOMPARE(b.bottom() + 1, a.top()); QCOMPARE(a.left(), b.left()); break;
		}
	}

	void dropsOnEmptyMonitorOrOuterGap_data() {
		QTest::addColumn<bool>("empty");
		QTest::newRow("empty-monitor") << true;
		QTest::newRow("outer-gap") << false;
	}

	void dropsOnEmptyMonitorOrOuterGap() {
		QFETCH(bool, empty);
		auto* platform = new FakeScreen(QRect(1920, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		const auto cleanupScreen = qScopeGuard([&]() {
			manager->setEnabled(false);
			QWindowSystemInterface::handleScreenRemoved(platform);
			QWindowSystemInterface::flushWindowSystemEvents();
		});
		fakeDesktop.monitors = {
		    {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true},
		    {{1920, 0, 1920, 1080}, {1920, 0, 1920, 1080}, true}};
		fakeDesktop.screens = {QGuiApplication::primaryScreen(), platform->screen()};
		auto* dragged = add(1);
		auto* neighbour = add(2);
		fakeWindow(dragged->hwnd()).monitor = 0;
		fakeWindow(neighbour->hwnd()).monitor = 0;
		TrackedWindow* target = nullptr;
		if (!empty) {
			target = add(3);
			fakeWindow(target->hwnd()).monitor = 1;
		}
		enable();
		auto* source = manager->managed.at(dragged).layout;
		manager->onMoveSizeStarted(dragged);
		fakeDesktop.cursor = QPoint(1921, 1);
		fakeWindow(dragged->hwnd()).frame.moveTopLeft(QPoint(1821, -11));
		manager->onMoveSizeEnded(dragged);
		manager->sync();
		auto* destination = manager->managed.at(dragged).layout;
		QCOMPARE(destination->screen.data(), platform->screen());
		QCOMPARE(destination->tree->ids().size(), empty ? 1 : 2);
		if (empty) {
			QCOMPARE(fakeWindow(neighbour->hwnd()).frame, QRect(20, 20, 1880, 1040));
			QCOMPARE(fakeWindow(dragged->hwnd()).frame, QRect(1940, 20, 1880, 1040));
		} else {
			// The outer gap is not the target's frame: it is that monitor's empty
			// area, so the window moves there instead of swapping.
			QCOMPARE(manager->managed.at(target).layout, destination);
			QCOMPARE(source->tree->ids().size(), 1);
		}
	}

	void framedFullscreenDropsTilerBorderWithoutMoving() {
		auto* browser = add(1, QStringLiteral("dia"));
		fakeDesktop.active = browser->hwnd();
		manager->setActiveBorderColour(QColor("#123456"));
		enable();
		auto& native = fakeWindow(browser->hwnd());
		const auto tile = native.frame;
		const auto moves = native.moves;
		const auto writes = native.borderWrites;
		QCOMPARE(native.borderColour, COLORREF(RGB(18, 52, 86)));
		// F11-style fullscreen can keep native frame styles. It must not
		// retain the tiler's colour just because its layout slot survives.
		native.frame = fakeDesktop.monitors.first().monitor;
		native.fullscreen = true;
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(native.borderColour, COLORREF(0xffffffff));
		QCOMPARE(native.borderWrites, writes + 1);
		for (int i = 0; i < 4; i++) {
			manager->markWindow(browser);
			manager->sync();
		}
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.borderWrites, writes + 1);
		native.frame = tile;
		native.fullscreen = false;
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(native.borderColour, COLORREF(RGB(18, 52, 86)));
		QCOMPARE(native.borderWrites, writes + 2);
		QCOMPARE(native.moves, moves);
	}

	void diaTransitionReplay_data() {
		QTest::addColumn<bool>("explicitTile");
		QTest::addColumn<bool>("appRestores");
		QTest::newRow("automatic-app-restore") << false << true;
		QTest::newRow("explicit-app-restore") << true << true;
		QTest::newRow("automatic-stale-normal-placement") << false << false;
		QTest::newRow("explicit-stale-normal-placement") << true << false;
	}

	void diaTransitionReplay() {
		QFETCH(bool, explicitTile);
		QFETCH(bool, appRestores);
		// Physical two-monitor coordinates. Only geometry and relative intervals from
		// the trace are used; no titles, handles or browsing data.
		fakeDesktop.monitors = {
		    {{-3840, 0, 3840, 2160}, {-3735, 18, 3717, 2124}, true},
		    {{0, 0, 3840, 2160}, {90, 15, 3735, 2130}, true}};
		auto* browser = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();
		if (explicitTile) {
			QVERIFY(manager->setFloating(browser, false, false));
			manager->sync();
		}
		auto& native = fakeWindow(browser->hwnd());
		auto& m = manager->managed.at(browser);
		const auto tile = native.frame;
		const auto neighbour = fakeWindow(other->hwnd()).frame;
		const auto work = fakeDesktop.monitors.first().work;
		// Initial fullscreen episode, then five complete recorded episodes.
		const int dwellMs[] = {52253, 3120, 4720, 4498, 2309, 2444};
		for (auto dwell: dwellMs) {
			const auto moves = native.moves;
			const auto otherMoves = fakeWindow(other->hwnd()).moves;
			native.style = 0;
			native.frame = fakeDesktop.monitors.first().monitor;
			native.fullscreen = true;
			browser->refreshState();
			emit browser->fullscreenChanged();
			manager->sync();
			fakeDesktop.now += dwell;
			// Repeated AppBar notifications must not cause tiler placements.
			for (int callback = 0; callback < 4; callback++) {
				manager->markLayout(m.layout);
				manager->sync();
				manager->onDeadline();
			}
			QCOMPARE(native.moves, moves);
			QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
			QCOMPARE(fakeDesktop.monitors.first().work, work);

			// The observed exit returns frame styles before the raw rectangle.
			native.style = WS_CAPTION | WS_THICKFRAME;
			browser->refreshState();
			manager->sync();
			QCOMPARE(native.moves, moves);
			QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
			fakeDesktop.now += 119;
			manager->markWindow(browser);
			manager->sync();
			QCOMPARE(native.moves, moves);

			if (appRestores) {
				// Give Dia its own restoration; do not generate a redundant
				// placement when it already restored the exact saved tile.
				native.frame = tile;
				native.fullscreen = false;
				emit browser->fullscreenChanged();
			}
			fakeDesktop.now += 1;
			manager->markWindow(browser); // the production 120 ms recheck
			manager->sync();
			QCOMPARE(native.moves - moves, appRestores ? 0 : 1);
			QCOMPARE(native.frame, tile);
			native.fullscreen = false;
			emit browser->fullscreenChanged();
			manager->sync();
			manager->verify();
			fakeDesktop.now += 300;
			manager->markWindow(browser);
			manager->sync();
			QCOMPARE(native.moves - moves, appRestores ? 0 : 1);
			QCOMPARE(fakeWindow(other->hwnd()).moves, otherMoves);
			QCOMPARE(fakeWindow(other->hwnd()).frame, neighbour);
			QCOMPARE(fakeDesktop.monitors.first().work, work);
			QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		}
	}

	void explicitTileDoesNotPlaceBeforeFullscreenReentryNotification() {
		auto* browser = add(1, QStringLiteral("dia"));
		enable();
		QVERIFY(manager->setFloating(browser, false, false));
		manager->sync();
		auto& native = fakeWindow(browser->hwnd());
		// Native geometry/style already changed; the tracker notification is
		// still queued. A pending layout must not send a tile over that frame.
		native.fullscreen = false;
		native.style = 0;
		native.frame = fakeDesktop.monitors.first().monitor;
		const auto moves = native.moves;
		manager->markWindow(browser);
		manager->sync();
		manager->verify();
		fakeDesktop.now += 2000;
		manager->onDeadline();
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
		QCOMPARE(manager->stateOf(manager->managed.at(browser)), TilingManager::State::Suspended);
	}

	void c3ReenterBeforePlacement_data() {
		QTest::addColumn<bool>("explicitTile");
		QTest::newRow("automatic") << false;
		QTest::newRow("explicit") << true;
	}

	void c3ReenterBeforePlacement() {
		QFETCH(bool, explicitTile);
		auto* app = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();
		if (explicitTile) {
			QVERIFY(manager->setFloating(app, false, false));
			manager->sync();
		}
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		const auto otherFrame = fakeWindow(other->hwnd()).frame;
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QVERIFY(m.fullscreenSuspended);

		native.style = WS_CAPTION | WS_THICKFRAME;
		native.exStyle = WS_EX_NOACTIVATE;
		app->refreshState();
		manager->sync();
		QVERIFY(manager->exitedFullscreen(m));
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);

		native.style = 0;
		native.exStyle = 0;
		fakeDesktop.now += 1000;
		const auto moves = native.moves;
		app->refreshState();
		manager->sync();
		QVERIFY(!manager->exitedFullscreen(m));
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
		QCOMPARE(fakeWindow(other->hwnd()).frame, otherFrame);
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
	}

	void c3OverrideDuringReentry() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		native.style = WS_THICKFRAME;
		native.exStyle = WS_EX_NOACTIVATE;
		app->refreshState();
		manager->sync();
		QVERIFY(manager->exitedFullscreen(m));
		native.style = 0;
		native.exStyle = 0;
		fakeDesktop.now += 1000;
		QVERIFY(manager->setFloating(app, false, false));
		const auto moves = native.moves;
		manager->sync();
		QCOMPARE(native.moves, moves);
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
	}

	void c3StyleOnlyRefreshNeedsWakeup() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		const auto tile = native.frame;
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QVERIFY(m.fullscreenSuspended);
		QVERIFY(!manager->dirtyWindows.contains(app));
		native.style = WS_THICKFRAME;
		app->refreshState();
		QVERIFY(manager->dirtyWindows.contains(app));
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		fakeDesktop.now += 120;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(native.frame, tile);
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
	}

	void c3TrackerStyleRefreshSignalsChangesOnly() {
		auto* app = add(1);
		auto& native = fakeWindow(app->hwnd());
		QSignalSpy changed(app, &TrackedWindow::styleChanged);
		app->refreshState();
		app->refreshState();
		QCOMPARE(changed.count(), 0);
		native.exStyle |= WS_EX_TOPMOST;
		app->refreshState();
		QCOMPARE(changed.count(), 1);
		app->refreshState();
		QCOMPARE(changed.count(), 1);
		native.style |= WS_CAPTION;
		app->refreshState();
		QCOMPARE(changed.count(), 2);
	}

	void c3LayoutTransferPreservesFreshEvidence() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		native.style = WS_THICKFRAME;
		native.exStyle = WS_EX_NOACTIVATE;
		app->refreshState();
		manager->sync();
		QVERIFY(manager->exitedFullscreen(m));
		auto desktop = m.layout->desktop;
		desktop.Data1 += 1;
		auto* destination = manager->layoutAt(desktop, m.layout->screen, true);
		manager->moveToLayout(m, destination, nullptr);
		native.style = 0;
		native.exStyle = 0;
		QVERIFY(!manager->exitedFullscreen(m));
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QCOMPARE(m.layout, destination);
	}

	void init() {
		fakeDesktop = {};
		manager = new TilingManager(QStringLiteral("Local\\QuickshellTiling-fixture"));
	}
	void cleanup() {
		manager->setEnabled(false);
		delete manager;
		qDeleteAll(windows);
		windows.clear();
		WindowTracker::instance()->fixtures.clear();
	}

	void enableAppliesInitialLayout() {
		auto* a = add(1);
		auto* b = add(2);
		manager->setEnabled(true);
		QVERIFY(manager->enabled());
		QVERIFY(manager->dirtyLayouts.size() > 0);
		// Drive the real timer connection without waiting on wall-clock time.
		QVERIFY(QMetaObject::invokeMethod(&manager->syncTimer, "timeout", Qt::DirectConnection));
		QVERIFY(fakeWindow(a->hwnd()).moves > 0);
		QVERIFY(fakeWindow(b->hwnd()).moves > 0);
		QVERIFY(fakeWindow(a->hwnd()).frame != fakeWindow(b->hwnd()).frame);
		QCOMPARE(manager->telemetry()->managedCount(), 2);
		QCOMPARE(fakeDesktop.mutexName, QStringLiteral("Local\\QuickshellTiling-fixture"));
		QCOMPARE(fakeDesktop.rescues, 1); // Constructor used the fake rescue boundary.
	}

	void singleWindowGapsAndBorderLifecycle() {
		auto* a = add(1);
		fakeDesktop.active = a->hwnd();
		manager->setActiveBorderColour(QColor("#ff8040"));
		manager->setInactiveBorderColour(QColor("#202122"));
		enable();
		QCOMPARE(fakeWindow(a->hwnd()).frame, QRect(20, 20, 1880, 1040));
		QCOMPARE(fakeWindow(a->hwnd()).borderColour, COLORREF(RGB(255, 128, 64)));

		auto* b = add(2);
		manager->manage(b);
		manager->sync();
		QCOMPARE(manager->outerGap(manager->managed.at(a).layout), 10);
		const auto first = fakeWindow(a->hwnd()).frame;
		const auto second = fakeWindow(b->hwnd()).frame;
		QCOMPARE(
		    qAbs(first.center().x() - second.center().x()) - (first.width() + second.width()) / 2,
		    10
		);
		QCOMPARE(fakeWindow(b->hwnd()).borderColour, COLORREF(RGB(32, 33, 34)));

		fakeDesktop.active = b->hwnd();
		manager->onActiveWindowChanged();
		QCOMPARE(fakeWindow(a->hwnd()).borderColour, COLORREF(RGB(32, 33, 34)));
		QCOMPARE(fakeWindow(b->hwnd()).borderColour, COLORREF(RGB(255, 128, 64)));
		manager->setActiveBorderColour(QColor("#123456"));
		QCOMPARE(fakeWindow(b->hwnd()).borderColour, COLORREF(RGB(18, 52, 86)));

		fakeWindow(b->hwnd()).minimized = true;
		manager->markWindow(b);
		manager->sync();
		QCOMPARE(fakeWindow(a->hwnd()).frame, QRect(20, 20, 1880, 1040));
		manager->setSingleWindowGapsOut(30);
		manager->sync();
		QCOMPARE(fakeWindow(a->hwnd()).frame, QRect(30, 30, 1860, 1020));
		manager->setExcluded({QStringLiteral("editor")});
		manager->sync();
		QCOMPARE(fakeWindow(a->hwnd()).borderColour, COLORREF(0xffffffff));
		QCOMPARE(fakeWindow(b->hwnd()).borderColour, COLORREF(0xffffffff));
		manager->setExcluded({});
		manager->sync();
		manager->unmanage(b);
		QCOMPARE(fakeWindow(b->hwnd()).borderColour, COLORREF(0xffffffff));
		manager->setBorderColours(false);
		QCOMPARE(fakeWindow(a->hwnd()).borderColour, COLORREF(0xffffffff));
		manager->setBorderColours(true);
		QVERIFY(fakeWindow(a->hwnd()).borderColour != COLORREF(0xffffffff));
		manager->restoreBorderColours(); // Same cleanup used by aboutToQuit.
		QCOMPARE(fakeWindow(a->hwnd()).borderColour, COLORREF(0xffffffff));
		manager->refreshBorderColours();
		manager->setEnabled(false);
		QCOMPARE(fakeWindow(a->hwnd()).borderColour, COLORREF(0xffffffff));
		fakeDesktop.borderSupported = false;
		const auto writes = fakeWindow(a->hwnd()).borderWrites;
		enable();
		QCOMPARE(fakeWindow(a->hwnd()).borderWrites, writes);
	}

	// Windows opens a new window on the primary monitor. The tiler must move it
	// to the monitor the Hyprland compatibility layer published as focusedmon,
	// the same focusedMonitor the shell uses, when the window appears.
	void newWindowOpensOnFocusedMonitor() {
		auto* platform = new FakeScreen(QRect(1920, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		auto* screenA = QGuiApplication::primaryScreen();
		auto* screenB = platform->screen();
		QVERIFY(screenA != nullptr);
		QVERIFY(screenB != nullptr);
		QVERIFY(screenA != screenB);

		fakeDesktop.monitors = {
		    {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true},
		    {{1920, 0, 1920, 1080}, {1920, 0, 1920, 1080}, true}};
		fakeDesktop.screens = {screenA, screenB};

		enable();
		QCOMPARE(manager->managed.size(), std::size_t(0));

		// Cursor on A, then the foreground switches to an editor on B. The
		// window Windows opened on A must tile on the editor's monitor.
		QVERIFY(fakeDesktop.focus.activeWindow(screenA));
		QVERIFY(fakeDesktop.focus.activeWindow(screenB));

		auto* calculator = add(1, QStringLiteral("calculator"));
		fakeWindow(calculator->hwnd()).monitor = 0; // Windows opened it on A.
		emit WindowTracker::instance()->windowAdded(calculator);
		QCOMPARE(fakeWindow(calculator->hwnd()).frame, QRect(1940, 20, 1880, 1040));
		// Windows activates the new window on its launch monitor before the
		// tiler settles it; the placement decision was made on appearance.
		QVERIFY(fakeDesktop.focus.activeWindow(screenA));
		manager->sync();

		auto& calc = manager->managed.at(calculator);
		QVERIFY(calc.layout != nullptr);
		QCOMPARE(calc.layout->screen.data(), screenB);
		QCOMPARE(fakeWindow(calculator->hwnd()).frame, QRect(1940, 20, 1880, 1040));

		// With focus-follows-mouse off, crossing onto B does nothing; the
		// foreground window on A keeps focus, so the next window opens on A.
		QVERIFY(!fakeDesktop.focus.cursorEntered(screenB, false));
		QCOMPARE(fakeDesktop.focus.screen(), screenA);
		auto* viewer = add(2);
		fakeWindow(viewer->hwnd()).monitor = 0;
		emit WindowTracker::instance()->windowAdded(viewer);
		manager->sync();

		auto& view = manager->managed.at(viewer);
		QVERIFY(view.layout != nullptr);
		QCOMPARE(view.layout->screen.data(), screenA);
		QCOMPARE(fakeWindow(viewer->hwnd()).frame, QRect(20, 20, 1880, 1040));

		// With focus-follows-mouse on, crossing onto B focuses it.
		QVERIFY(fakeDesktop.focus.cursorEntered(screenB, true));
		auto* editor = add(3);
		fakeWindow(editor->hwnd()).monitor = 0;
		emit WindowTracker::instance()->windowAdded(editor);
		manager->sync();
		QCOMPARE(manager->managed.at(editor).layout->screen.data(), screenB);

		// A click always moves focus, even with focus-follows-mouse off.
		QVERIFY(fakeDesktop.focus.clicked(screenA));
		auto* terminal = add(4);
		fakeWindow(terminal->hwnd()).monitor = 0;
		emit WindowTracker::instance()->windowAdded(terminal);
		manager->sync();
		QCOMPARE(manager->managed.at(terminal).layout->screen.data(), screenA);

		manager->setEnabled(false);
		QWindowSystemInterface::handleScreenRemoved(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
	}

	void focusedMonitorFollowsOnlyRealWindowChanges_data() {
		QTest::addColumn<int>("event");
		QTest::addColumn<bool>("click");
		QTest::addColumn<bool>("switchWindow");
		QTest::newRow("title-after-cursor-crossing") << 0 << false << false;
		QTest::newRow("duplicate-foreground-after-cursor-crossing") << 1 << false << false;
		QTest::newRow("same-app-restored-after-panel") << 1 << true << false;
		QTest::newRow("different-window-activated") << 2 << false << true;
		QTest::newRow("different-app-restored-after-panel") << 2 << true << true;
	}

	// A title notification or a foreground event for the already-active app must
	// not undo a later cursor crossing/click. A real switch, even to another app
	// on the same screen as the previous app, must still move monitor focus and
	// new-window placement.
	void focusedMonitorFollowsOnlyRealWindowChanges() {
		QFETCH(int, event);
		QFETCH(bool, click);
		QFETCH(bool, switchWindow);
		auto* platform = new FakeScreen(QRect(1920, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
		auto* screenA = QGuiApplication::primaryScreen();
		auto* screenB = platform->screen();
		QVERIFY(screenB != nullptr);

		fakeDesktop.monitors = {
		    {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true},
		    {{1920, 0, 1920, 1080}, {1920, 0, 1920, 1080}, true}};
		fakeDesktop.screens = {screenA, screenB};

		auto* app = add(1);
		fakeWindow(app->hwnd()).monitor = 1;
		fakeDesktop.active = app->hwnd();
		enable();

		// The same wiring the Hyprland compatibility layer uses.
		QObject scope;
		int activeChanges = 0;
		connect(WindowTracker::instance(), &WindowTracker::activeWindowChanged, &scope, [&]() { activeChanges++; });
		auto follow = [](TrackedWindow* window) {
			(void) fakeDesktop.focus.activeWindow(window->screen());
		};
		followActiveWindow(WindowTracker::instance(), &scope, follow);

		QVERIFY(fakeDesktop.focus.activeWindow(screenB));

		if (click) {
			QVERIFY(fakeDesktop.focus.clicked(screenA));
		} else {
			QVERIFY(fakeDesktop.focus.cursorEntered(screenA, true));
		}
		QCOMPARE(fakeDesktop.focus.screen(), screenA);

		switch (event) {
		case 0: emit app->titleChanged(); break;
		case 1: WindowTracker::instance()->deliverForegroundEvent(app->hwnd()); break;
		case 2: {
			auto* other = add(2);
			fakeWindow(other->hwnd()).monitor = 1;
			WindowTracker::instance()->deliverForegroundEvent(other->hwnd());
			break;
		}
		}
		QCOMPARE(activeChanges, switchWindow ? 1 : 0);
		QCOMPARE(fakeDesktop.focus.screen(), switchWindow ? screenB : screenA);

		auto* next = add(3);
		fakeWindow(next->hwnd()).monitor = 0; // Windows opened it on A.
		emit WindowTracker::instance()->windowAdded(next);
		manager->sync();
		QCOMPARE(manager->managed.at(next).layout->screen.data(), switchWindow ? screenB : screenA);

		manager->setEnabled(false);
		QWindowSystemInterface::handleScreenRemoved(platform);
		QWindowSystemInterface::flushWindowSystemEvents();
	}

	void placeholderDesktopLayoutStaysAliveAcrossSyncs() {
		auto* a = add(1);
		auto* b = add(2);
		enable();

		QCOMPARE(manager->managed.size(), std::size_t(2));
		QCOMPARE(manager->layouts.size(), std::size_t(1));
		auto* layout = manager->managed.at(a).layout;
		QVERIFY(layout != nullptr);
		QVERIFY(IsEqualGUID(layout->desktop, GUID {}));
		QVERIFY(manager->layoutAlive(layout));
		QCOMPARE(manager->managed.at(b).layout, layout);
		QCOMPARE(layout->tree->ids().length(), qsizetype(2));
		QCOMPARE(manager->telemetry()->managedCount(), 2);

		// The initial apply reached the placement seam for both windows.
		auto movesA = fakeWindow(a->hwnd()).moves;
		auto movesB = fakeWindow(b->hwnd()).moves;
		QVERIFY(movesA > 0);
		QVERIFY(movesB > 0);

		// Repeated syncs must not erase the placeholder layout and re-insert the
		// windows into a fresh one every tick.
		for (auto i = 0; i < 5; i++) {
			QVERIFY(QMetaObject::invokeMethod(&manager->syncTimer, "timeout", Qt::DirectConnection));
			QCOMPARE(manager->layouts.size(), std::size_t(1));
			QVERIFY(manager->layoutAlive(layout));
			QCOMPARE(manager->managed.at(a).layout, layout);
			QCOMPARE(manager->managed.at(b).layout, layout);
			QCOMPARE(layout->tree->ids().length(), qsizetype(2));
		}

		// The layout stayed alive, so no extra placements were triggered.
		QCOMPARE(fakeWindow(a->hwnd()).moves, movesA);
		QCOMPARE(fakeWindow(b->hwnd()).moves, movesB);
	}

	void refusalRollsBack() {
		auto* a = add(1);
		fakeDesktop.mutexHeld = true;
		QSignalSpy refused(manager->telemetry(), &tiling::TilingTelemetry::enableRefused);
		QSignalSpy enabled(manager, &TilingManager::enabledChanged);
		bool callbackEnabled = true;
		connect(manager->telemetry(), &tiling::TilingTelemetry::enableRefused, this, [&]() {
			callbackEnabled = manager->enabled();
			manager->setEnabled(false);
		});
		manager->setEnabled(true);
		QVERIFY(!manager->enabled());
		QVERIFY(manager->managed.empty());
		QVERIFY(!fakeDesktop.mutexOpen);
		QCOMPARE(fakeWindow(a->hwnd()).moves, 0);
		QCOMPARE(refused.count(), 0);
		drain();
		QCOMPARE(refused.count(), 1);
		QVERIFY(!callbackEnabled);
		QCOMPARE(enabled.count(), 0);
		fakeDesktop.mutexHeld = false;
		enable();
		QVERIFY(manager->enabled());
		QVERIFY(manager->telemetry()->lastError().isEmpty());
		QVERIFY(fakeWindow(a->hwnd()).moves > 0);
	}

	void overridesRespectHostAndIncluded() {
		auto* host = add(1);
		auto* other = add(2, QStringLiteral("other"));
		auto* editor = add(3);
		fakeWindow(host->hwnd()).own = true;
		manager->setIncluded({QStringLiteral("editor")});
		enable();
		for (auto* window: {host, other}) {
			manager->setFloating(window, false, false); // settiled
			manager->sync();
			QVERIFY(!manager->isTiled(window));
			manager->setFloating(window, false, true); // togglefloating
			manager->sync();
			QVERIFY(!manager->isTiled(window));
			QCOMPARE(fakeWindow(window->hwnd()).moves, 0);
		}
		QVERIFY(manager->isTiled(editor));
	}

	void narrowedIncludedRemovesForcedTile() {
		auto* a = add(1);
		auto* b = add(2, QStringLiteral("terminal"));
		auto original = fakeWindow(b->hwnd()).frame;
		enable();
		manager->setFloating(b, false, false);
		manager->sync();
		manager->setIncluded({QStringLiteral("editor")});
		manager->sync();
		QVERIFY(manager->isTiled(a));
		QVERIFY(!manager->isTiled(b));
		QCOMPARE(fakeWindow(b->hwnd()).frame, original);
		QCOMPARE(manager->telemetry()->managedCount(), 1);
	}

	void legacyStyleOverrideAndStagedExclusion() {
		auto* a = add(1);
		fakeWindow(a->hwnd()).style = 0;
		manager->setExcluded({QStringLiteral("editor")});
		enable();
		QVERIFY(!manager->isTiled(a));
		manager->setFloating(a, false, false);
		manager->sync();
		QVERIFY(manager->isTiled(a)); // Legacy explicit style/exclusion override.
		manager->setIncluded({QStringLiteral("*")});
		manager->sync();
		QVERIFY(!manager->isTiled(a)); // Staged exclusions win.
	}

	void browserFullscreenRetainsTile_data() {
		QTest::addColumn<bool>("topmost");
		QTest::newRow("borderless") << false;
		QTest::newRow("borderless-topmost") << true;
	}

	void browserFullscreenRetainsTile() {
		QFETCH(bool, topmost);
		auto* browser = add(1, QStringLiteral("dia"));
		manager->setIncluded({QStringLiteral("exe:*")});
		enable();
		auto& native = fakeWindow(browser->hwnd());
		const auto tile = native.frame;
		auto* layout = manager->managed.at(browser).layout;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.fullscreen = true;
		native.style &= ~WS_THICKFRAME;
		if (topmost) native.exStyle |= WS_EX_TOPMOST;
		const auto fullscreenFrame = native.frame;
		const auto moves = native.moves;
		emit browser->fullscreenChanged();
		emit browser->rectChanged();
		manager->sync();
		manager->verify();
		fakeDesktop.now += 2000;
		manager->onDeadline();
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);
		QCOMPARE(manager->managed.at(browser).layout, layout);
		QCOMPARE(manager->stateOf(manager->managed.at(browser)), TilingManager::State::Suspended);
		// AppBar work-area changes must not resize the fullscreen browser.
		fakeDesktop.monitors.first().work.adjust(60, 10, -10, -10);
		manager->markLayout(layout);
		manager->sync();
		QCOMPARE(native.moves, moves);
		native.style |= WS_THICKFRAME;
		native.exStyle &= ~WS_EX_TOPMOST;
		native.fullscreen = false;
		native.frame = tile;
		emit browser->fullscreenChanged();
		emit browser->rectChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(browser)), TilingManager::State::Tiled);
		QCOMPARE(native.frame, QRect(80, 30, 1810, 1020));
	}

	void alreadyFullscreenBrowserIsNotResized() {
		auto* browser = add(1, QStringLiteral("dia"));
		auto& native = fakeWindow(browser->hwnd());
		native.frame = fakeDesktop.monitors.first().monitor;
		native.fullscreen = true;
		native.style = 0;
		enable();
		QCOMPARE(native.moves, 0);
		QVERIFY(!manager->isTiled(browser));
		native.style = WS_THICKFRAME;
		native.fullscreen = false;
		native.frame = QRect(100, 100, 800, 600);
		emit browser->fullscreenChanged();
		manager->sync();
		QVERIFY(manager->isTiled(browser));
		QCOMPARE(native.frame, QRect(20, 20, 1880, 1040));
	}

	void appCloakedWindowReleasesAndRestoresTile() {
		auto* app = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();

		auto& native = fakeWindow(app->hwnd());
		auto& otherNative = fakeWindow(other->hwnd());
		auto& m = manager->managed.at(app);
		const auto tile = native.frame;
		const auto otherTile = otherNative.frame;
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);

		// An app cloak is not a virtual desktop: the window must stop
		// reserving space and leave the workspace/task models, like a minimize.
		native.cloak = FakeWindow::Cloak::App;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Hidden);
		QVERIFY(app->hidden());
		QVERIFY(app->appCloaked());
		QCOMPARE(native.frame, tile);
		QVERIFY(otherNative.frame != otherTile);

		const auto expanded = otherNative.frame;
		native.cloak = FakeWindow::Cloak::None;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QVERIFY(!app->hidden());
		QCOMPARE(native.frame, tile);
		QCOMPARE(otherNative.frame, otherTile);
		QVERIFY(expanded != otherTile);
	}

	void shellCloakedWindowKeepsTile() {
		auto* app = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();

		auto& native = fakeWindow(app->hwnd());
		auto& otherNative = fakeWindow(other->hwnd());
		auto& m = manager->managed.at(app);
		const auto tile = native.frame;
		const auto otherTile = otherNative.frame;

		// A shell cloak legitimately belongs to another virtual desktop and
		// keeps today's mapped/tiled behavior.
		native.cloak = FakeWindow::Cloak::Shell;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QVERIFY(app->cloaked());
		QVERIFY(!app->appCloaked());
		QVERIFY(!app->hidden());
		QCOMPARE(native.frame, tile);
		QCOMPARE(otherNative.frame, otherTile);
	}

	void appCloakedBorderlessFullscreenPreservesEpisode() {
		auto* app = add(1, QStringLiteral("dia"));
		enable();
		QVERIFY(manager->setFloating(app, false, false));
		manager->sync();

		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		app->refreshFullscreenEpisode();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QVERIFY(m.fullscreenSuspended);

		const auto fullscreenFrame = native.frame;
		const auto moves = native.moves;

		// Hiding a fullscreen window releases its reserved space, but keeps
		// the episode and latch. It is not a fullscreen exit.
		native.cloak = FakeWindow::Cloak::App;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Hidden);
		QVERIFY(app->fullscreen());
		QVERIFY(app->appCloaked());
		QVERIFY(m.fullscreenSuspended);
		QVERIFY(!manager->exitedFullscreen(m));
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);

		native.cloak = FakeWindow::Cloak::None;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QVERIFY(m.fullscreenSuspended);
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);
	}

	void appCloakedFrameStyledFullscreenPreservesEpisode() {
		auto* app = add(1, QStringLiteral("dia"));
		enable();

		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		// Auto-tiled and still carrying its frame styles when it goes
		// fullscreen: the style-based exit check must not fire on a cloak.
		native.frame = fakeDesktop.monitors.first().monitor;
		native.exStyle |= WS_EX_TOPMOST;
		app->refreshFullscreenEpisode();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QVERIFY(m.fullscreenSuspended);

		const auto fullscreenFrame = native.frame;
		const auto moves = native.moves;

		native.cloak = FakeWindow::Cloak::App;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Hidden);
		QVERIFY(app->fullscreen());
		QVERIFY(m.fullscreenSuspended);
		QVERIFY(!manager->exitedFullscreen(m));
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);

		native.cloak = FakeWindow::Cloak::None;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QVERIFY(m.fullscreenSuspended);
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);
	}

	void fullscreenExitWaitsForTransientFloatStyle() {
		auto* browser = add(1, QStringLiteral("dia"));
		enable();
		auto& native = fakeWindow(browser->hwnd());
		auto* layout = manager->managed.at(browser).layout;
		const auto tile = native.frame;
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(browser)), TilingManager::State::Suspended);

		// Dia has restored its normal geometry and thick frame, but keeps
		// WS_EX_NOACTIVATE briefly after fullscreenChanged(false).
		native.fullscreen = false;
		native.style = WS_THICKFRAME;
		native.exStyle = WS_EX_NOACTIVATE;
		native.frame = tile.translated(2, -20);
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->managed.at(browser).layout, layout);
		QCOMPARE(manager->stateOf(manager->managed.at(browser)), TilingManager::State::Suspended);
		native.exStyle = 0;
		fakeDesktop.now += 300;
		manager->markWindow(browser);
		manager->sync();
		QCOMPARE(manager->managed.at(browser).layout, layout);
		QCOMPARE(native.frame, tile);
	}

	void exclusionDuringFullscreenReleasesTile() {
		auto* browser = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();
		auto& native = fakeWindow(browser->hwnd());
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		const auto fullscreenFrame = native.frame;
		const auto moves = native.moves;
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(browser)), TilingManager::State::Suspended);
		manager->setExcluded({QStringLiteral("dia")});
		manager->sync();
		QVERIFY(!manager->isTiled(browser));
		QVERIFY(manager->isTiled(other));
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);
	}

	void persistentFloatStyleAfterFullscreenStillFloats() {
		auto* browser = add(1, QStringLiteral("dia"));
		enable();
		auto& native = fakeWindow(browser->hwnd());
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		emit browser->fullscreenChanged();
		manager->sync();
		native.fullscreen = false;
		native.exStyle = WS_EX_NOACTIVATE;
		native.frame = QRect(100, 100, 600, 400);
		emit browser->fullscreenChanged();
		manager->sync();
		QVERIFY(manager->isTiled(browser));
		fakeDesktop.now += 300;
		manager->markWindow(browser);
		manager->sync();
		QVERIFY(!manager->isTiled(browser));
	}

	void monitorSizedTileStillRetilesAfterWorkAreaChange() {
		auto* window = add(1);
		manager->setGapsOut(0);
		manager->setSingleWindowGapsOut(0);
		enable();
		auto& native = fakeWindow(window->hwnd());
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
		// The tracker also calls an ordinary monitor-sized tile fullscreen.
		native.fullscreen = true;
		fakeDesktop.monitors.first().work.adjust(60, 0, 0, 0);
		manager->markLayout(manager->managed.at(window).layout);
		manager->sync();
		QCOMPARE(native.frame, QRect(60, 0, 1860, 1080));
	}

	// A fullscreen window keeps its slot. The neighbour must keep exactly the
	// tile it had before: suspended windows still occupy space, so the layout
	// may not fall back to the single-window gaps and reflow on both edges of
	// the transition. The same relayout also models a virtual-desktop switch,
	// which marks every window and layout dirty without moving any window.
	void fullscreenNeighbourKeepsItsTile() {
		auto* full = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();
		auto& native = fakeWindow(full->hwnd());
		const auto fullTile = native.frame;
		const auto otherTile = fakeWindow(other->hwnd()).frame;
		QVERIFY(fullTile != otherTile);

		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		const auto fullscreenFrame = native.frame;
		const auto moves = native.moves;
		emit full->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(full)), TilingManager::State::Suspended);
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);
		QCOMPARE(fakeWindow(other->hwnd()).frame, otherTile);

		manager->markAll();
		manager->sync();
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(fakeWindow(other->hwnd()).frame, otherTile);

		native.fullscreen = false;
		native.style |= WS_THICKFRAME;
		native.frame = fullTile;
		emit full->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(full)), TilingManager::State::Tiled);
		QCOMPARE(native.frame, fullTile);
		QCOMPARE(fakeWindow(other->hwnd()).frame, otherTile);
	}

	// Maximizing is not fullscreen, but a browser can add WS_EX_TOPMOST or
	// WS_EX_NOACTIVATE to its frame while maximizing. Floating it on that
	// transient style would restore the pre-tiling rectangle over the maximize.
	void maximizedWindowKeepsSlotAgainstTransientFloatStyle() {
		auto* browser = add(1, QStringLiteral("dia"));
		enable();
		auto& native = fakeWindow(browser->hwnd());
		auto& m = manager->managed.at(browser);
		const auto tile = native.frame;
		const auto moves = native.moves;

		native.maximized = true;
		native.exStyle = WS_EX_TOPMOST;
		emit browser->maximizedChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QVERIFY(manager->isTiled(browser));
		QCOMPARE(native.frame, tile);
		QCOMPARE(native.moves, moves);

		native.maximized = false;
		native.exStyle = 0;
		emit browser->maximizedChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, tile);
	}

	// Reviewer probe: minimizing is not evidence that a fullscreen application
	// exited. The tracker reports fullscreen=false whenever IsIconic is set, so
	// the tiler must keep the suspension and must not resize a borderless
	// application that is restored from the taskbar still in fullscreen.
	void newFullscreenEpisodeRecordsItsOwnStyle_data() {
		QTest::addColumn<bool>("explicitTile");
		QTest::newRow("automatic") << false;
		QTest::newRow("explicit") << true;
	}

	void newFullscreenEpisodeRecordsItsOwnStyle() {
		QFETCH(bool, explicitTile);
		auto* app = add(1);
		auto* neighbour = add(2);
		enable();
		if (explicitTile) {
			QVERIFY(manager->setFloating(app, false, false));
			manager->sync();
		}
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		const auto otherTile = fakeWindow(neighbour->hwnd()).frame;
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style = 0;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QVERIFY(m.fullscreenStyleValid);
		QVERIFY(m.fullscreenSuspended);

		// A genuine geometry-confirmed exit, held by the Dia-style grace.
		native.fullscreen = false;
		native.frame = QRect(100, 100, 800, 600);
		native.style = WS_CAPTION | WS_THICKFRAME;
		native.exStyle = WS_EX_NOACTIVATE;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);

		// A distinct entry keeps its frame style. It must snapshot this entry,
		// rather than interpreting the previous episode's missing bits as exit.
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.exStyle = 0;
		const auto moves = native.moves;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
		QCOMPARE(fakeWindow(neighbour->hwnd()).frame, otherTile);
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
	}

	void fullscreenKeepingCaptionAndFrameStaysSuspended() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		native.fullscreen = true;
		native.style = WS_CAPTION | WS_THICKFRAME;
		native.frame = fakeDesktop.monitors.first().monitor;
		const auto moves = native.moves;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		for (int i = 0; i < 100; i++) {
			app->refreshState();
			manager->sync();
		}
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Suspended);
	}

	void styleExitRechecksDoNotRearmGrace() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		native.fullscreen = true;
		native.style = 0;
		native.frame = fakeDesktop.monitors.first().monitor;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		native.style = WS_CAPTION | WS_THICKFRAME;
		native.exStyle = WS_EX_NOACTIVATE;
		app->refreshState();
		manager->sync();
		const auto deadline = m.fullscreenRecoveryUntil;
		QVERIFY(manager->exitedFullscreen(m));
		// Exactly the work done by the scheduled 120 ms recheck.
		for (int i = 0; i < 10; i++) {
			fakeDesktop.now += 120;
			manager->markWindow(app);
			manager->sync();
		}
		QCOMPARE(m.fullscreenRecoveryUntil, deadline);
	}

	void reviewMinimizeDoesNotImplyFullscreenExit() {
		auto* app = add(1);
		enable();
		QVERIFY(manager->setFloating(app, false, false));
		manager->sync();
		auto& native = fakeWindow(app->hwnd());
		const auto monitor = fakeDesktop.monitors.first().monitor;
		native.frame = monitor;
		native.fullscreen = true;
		native.style &= ~WS_THICKFRAME;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Suspended);

		native.minimized = true;
		native.fullscreen = false;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Hidden);

		// Restored after the grace period, still fullscreen.
		fakeDesktop.now += 1000;
		native.minimized = false;
		native.fullscreen = true;
		const auto moves = native.moves;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.frame, monitor);
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Suspended);
	}

	// Reviewer probe (round 2): the original minimize-on-exit regression. The
	// application restores its normal frame style, then minimizes before it
	// restores its rectangle, so Windows later restores the monitor-sized
	// rectangle. Regaining WS_THICKFRAME confirms the exit even though geometry
	// still looks fullscreen, and the window returns to its exact tile.
	void reviewOriginalMonitorSizedExitIntoMinimize() {
		auto* browser = add(1, QStringLiteral("dia"));
		enable();
		auto& native = fakeWindow(browser->hwnd());
		auto& m = manager->managed.at(browser);
		const auto tile = native.frame;

		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);

		// Normal style restored, minimized before restoring the rectangle.
		native.fullscreen = false;
		native.minimized = true;
		native.style |= WS_THICKFRAME;
		native.normal = native.frame;
		emit browser->fullscreenChanged();
		emit browser->minimizedChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Hidden);
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);

		// The restore keeps the fullscreen-era rectangle; the frame style is
		// the only evidence that the application left fullscreen.
		native.minimized = false;
		native.fullscreen = true;
		emit browser->minimizedChanged();
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		fakeDesktop.now += 120;
		manager->markWindow(browser);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, tile);
	}

	// A style-confirmed exit works without a minimize: the rect is still
	// monitor sized, but the application regained its frame.
	void reviewStyleRegainedExitWithMonitorSizedRect() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		auto& m = manager->managed.at(app);
		const auto tile = native.frame;

		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		app->refreshState();
		emit app->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);

		native.style |= WS_THICKFRAME;
		app->refreshState();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		fakeDesktop.now += 120;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, tile);
	}

	// A minimize-restore that did not regain the frame bits is still
	// fullscreen: geometry and style both say so, and nothing is resized.
	void reviewMinimizedRestoreStillFullscreenStaysSuspended() {
		auto* app = add(1);
		enable();
		auto& native = fakeWindow(app->hwnd());
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Suspended);

		native.fullscreen = false;
		native.minimized = true;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Hidden);

		fakeDesktop.now += 1000;
		native.minimized = false;
		native.fullscreen = true;
		const auto moves = native.moves;
		manager->markWindow(app);
		manager->sync();
		QCOMPARE(native.moves, moves);
		QCOMPARE(native.frame, fakeDesktop.monitors.first().monitor);
		QCOMPARE(manager->stateOf(manager->managed.at(app)), TilingManager::State::Suspended);
	}

	// A real exit that happens to coincide with a minimize restores a smaller
	// normal rectangle; the window then returns to its exact tile.
	void fullscreenExitIntoMinimizeRetilesOnRestore() {
		auto* browser = add(1, QStringLiteral("dia"));
		enable();
		auto& native = fakeWindow(browser->hwnd());
		auto& m = manager->managed.at(browser);
		const auto tile = native.frame;

		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		emit browser->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);

		// The application exits fullscreen, restores its normal placement, and
		// minimizes before the next refresh.
		native.fullscreen = false;
		native.minimized = true;
		native.style |= WS_THICKFRAME;
		native.frame = tile.translated(120, 80);
		emit browser->fullscreenChanged();
		emit browser->minimizedChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Hidden);

		native.minimized = false;
		emit browser->minimizedChanged();
		manager->sync();
		QVERIFY(manager->isTiled(browser));
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, tile);
	}

	// A tiled game can enter fullscreen before the tracker's monitor-sized
	// check passes: it may size itself to the work area, or the desktop may
	// already report a different display mode. Its floating style must not
	// float it out and restore the pre-tiling rectangle over it.
	void borderlessWorkAreaSizedGameKeepsTile() {
		fakeDesktop.monitors.first().work = {0, 0, 1920, 1040};
		auto* game = add(1, QStringLiteral("game"));
		enable();
		auto& native = fakeWindow(game->hwnd());
		auto& m = manager->managed.at(game);
		const auto tile = native.frame;
		const auto moves = native.moves;

		native.style &= ~WS_THICKFRAME;
		native.exStyle = WS_EX_TOPMOST;
		native.frame = fakeDesktop.monitors.first().work;
		const auto fullscreenFrame = native.frame;
		QVERIFY(native.fullscreen == false);
		emit game->rectChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QCOMPARE(native.frame, fullscreenFrame);
		QCOMPARE(native.moves, moves);

		native.style |= WS_THICKFRAME;
		native.exStyle = 0;
		native.frame = tile;
		emit game->rectChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, tile);
	}

	// Negative monitor origins and mixed DPI keep the same physical tile. The
	// DPI scope reads and writes in physical pixels, so a virtualized window's
	// fullscreen frame must never be corrected.
	void fullscreenOnNegativeOriginMixedDpiMonitor() {
		fakeDesktop.monitors.clear();
		fakeDesktop.monitors.append({{-1920, -1080, 1920, 1080}, {-1900, -1060, 1880, 1040}, true});
		auto* full = add(1);
		auto* other = add(2);
		auto& fullNative = fakeWindow(full->hwnd());
		auto& otherNative = fakeWindow(other->hwnd());
		fullNative.frame.moveTopLeft({-1820, -980});
		otherNative.frame.moveTopLeft({-900, -520});
		otherNative.dpiAwareness = DPI_AWARENESS_UNAWARE;
		otherNative.dpiScale = 1.5;
		fakeDesktop.threadAwareness = DPI_AWARENESS_SYSTEM_AWARE;
		enable();
		const auto fullTile = fullNative.frame;
		const auto otherTile = otherNative.frame;
		const auto fullMoves = fullNative.moves;
		const auto otherMoves = otherNative.moves;

		fullNative.fullscreen = true;
		fullNative.frame = fakeDesktop.monitors.first().monitor;
		fullNative.style &= ~WS_THICKFRAME;
		emit full->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(full)), TilingManager::State::Suspended);
		QCOMPARE(fullNative.frame, fakeDesktop.monitors.first().monitor);
		QCOMPARE(fullNative.moves, fullMoves);
		QCOMPARE(otherNative.frame, otherTile);
		QCOMPARE(otherNative.moves, otherMoves);

		fullNative.fullscreen = false;
		fullNative.style |= WS_THICKFRAME;
		fullNative.frame = fullTile;
		emit full->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(full)), TilingManager::State::Tiled);
		QCOMPARE(fullNative.frame, fullTile);
		QCOMPARE(otherNative.frame, otherTile);
	}

	// Several fullscreen cycles inside the post-exit grace must not consume or
	// extend the deferral incorrectly. Once the style settles the window lands
	// on its old tile and the neighbour never moves.
	void rapidFullscreenCyclesInsideGraceKeepTile() {
		auto* browser = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();
		auto& native = fakeWindow(browser->hwnd());
		auto& m = manager->managed.at(browser);
		const auto tile = native.frame;
		const auto otherTile = fakeWindow(other->hwnd()).frame;

		for (auto i = 0; i < 3; i++) {
			native.fullscreen = true;
			native.frame = fakeDesktop.monitors.first().monitor;
			native.style &= ~WS_THICKFRAME;
			native.exStyle = WS_EX_NOACTIVATE;
			emit browser->fullscreenChanged();
			manager->sync();
			QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);

			native.fullscreen = false;
			native.style |= WS_THICKFRAME;
			native.exStyle = WS_EX_NOACTIVATE; // Transient style after the exit.
			native.frame = tile.translated(2, -20);
			emit browser->fullscreenChanged();
			manager->sync();
			QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
			QCOMPARE(fakeWindow(other->hwnd()).frame, otherTile);
			fakeDesktop.now += 50;
		}

		native.exStyle = 0;
		native.frame = tile;
		fakeDesktop.now += 300;
		manager->markWindow(browser);
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, tile);
		QCOMPARE(fakeWindow(other->hwnd()).frame, otherTile);
	}

	// Closing a window while it is fullscreen releases only that slot. A later
	// window that reuses the same HWND starts with no stale fullscreen or
	// layout state.
	void closedFullscreenWindowReleasesOnlyItsSlot() {
		auto* gone = add(1, QStringLiteral("dia"));
		auto* other = add(2);
		enable();
		const auto hwnd = gone->hwnd();
		auto& native = fakeWindow(hwnd);
		native.fullscreen = true;
		native.frame = fakeDesktop.monitors.first().monitor;
		native.style &= ~WS_THICKFRAME;
		emit gone->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(gone)), TilingManager::State::Suspended);

		auto* layout = manager->managed.at(other).layout;
		manager->unmanage(gone);
		windows.removeOne(gone);
		delete gone;
		manager->markLayout(layout);
		manager->sync();
		QCOMPARE(fakeWindow(other->hwnd()).frame, QRect(20, 20, 1880, 1040));

		auto* reused = add(1, QStringLiteral("editor"));
		manager->manage(reused);
		manager->sync();
		QCOMPARE(manager->stateOf(manager->managed.at(reused)), TilingManager::State::Tiled);
		QVERIFY(manager->isTiled(reused));
		QCOMPARE(manager->managed.at(reused).layout, layout);
		const auto reusedFrame = fakeWindow(reused->hwnd()).frame;
		QVERIFY(reusedFrame != fakeDesktop.monitors.first().monitor);
		QVERIFY(fakeDesktop.monitors.first().work.contains(reusedFrame));
		QVERIFY(reusedFrame != fakeWindow(other->hwnd()).frame);
		auto& reusedNative = fakeWindow(reused->hwnd());
		QCOMPARE(reusedNative.fullscreen, false);
		QVERIFY(reusedNative.moves > 0);
	}

	// A new display mode changes the monitor rectangle while a window is
	// fullscreen. The window follows the mode, is never corrected, and returns
	// to the tile computed for the new work area.
	void monitorModeChangeWhileFullscreenKeepsTile() {
		auto* game = add(1, QStringLiteral("game"));
		enable();
		auto& native = fakeWindow(game->hwnd());
		auto& m = manager->managed.at(game);
		native.fullscreen = true;
		native.style &= ~WS_THICKFRAME;
		native.exStyle = WS_EX_TOPMOST;
		native.frame = fakeDesktop.monitors.first().monitor;
		emit game->fullscreenChanged();
		manager->sync();

		fakeDesktop.monitors.first() = {{0, 0, 1280, 720}, {0, 0, 1280, 720}, true};
		native.frame = fakeDesktop.monitors.first().monitor;
		const auto moves = native.moves;
		manager->onScreensChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QCOMPARE(native.frame, QRect(0, 0, 1280, 720));
		QCOMPARE(native.moves, moves);

		native.fullscreen = false;
		native.style |= WS_THICKFRAME;
		native.exStyle = 0;
		native.frame = QRect(100, 100, 800, 600);
		emit game->fullscreenChanged();
		manager->sync();
		QCOMPARE(manager->stateOf(m), TilingManager::State::Tiled);
		QCOMPARE(native.frame, QRect(20, 20, 1240, 680));
	}

	// The tracker's detection tolerance and its display-mode latch are a pure
	// seam: a borderless window may sit a pixel or two inside the monitor, and
	// a mode change can report a different monitor size than the window has
	// adopted.
	void fullscreenGeometryData_data() {
		QTest::addColumn<QRect>("frame");
		QTest::addColumn<QRect>("monitor");
		QTest::addColumn<bool>("minimized");
		QTest::addColumn<bool>("maximized");
		QTest::addColumn<QRect>("latchedFrame");
		QTest::addColumn<QRect>("latchedMonitor");
		QTest::addColumn<bool>("expected");

		const auto monitor = QRect(0, 0, 1920, 1080);
		const auto oldMode = QRect(0, 0, 1280, 720);
		QTest::newRow("exact")
		    << monitor << monitor << false << false << QRect() << QRect() << true;
		QTest::newRow("one-pixel-inset")
		    << QRect(1, 1, 1918, 1078) << monitor << false << false << QRect() << QRect() << true;
		QTest::newRow("three-pixel-inset")
		    << QRect(3, 3, 1914, 1074) << monitor << false << false << QRect() << QRect() << false;
		QTest::newRow("oversized")
		    << QRect(-8, -8, 1936, 1096) << monitor << false << false << QRect() << QRect() << true;
		QTest::newRow("minimized")
		    << monitor << monitor << true << false << QRect() << QRect() << false;
		QTest::newRow("maximized")
		    << monitor << monitor << false << true << QRect() << QRect() << false;
		QTest::newRow("negative-origin")
		    << QRect(-1920, -1080, 1920, 1080) << QRect(-1920, -1080, 1920, 1080) << false << false
		    << QRect() << QRect() << true;
		QTest::newRow("smaller-mode")
		    << oldMode << monitor << false << false << QRect() << QRect() << false;
		QTest::newRow("smaller-mode-latched")
		    << oldMode << monitor << false << false << oldMode << oldMode << true;
		QTest::newRow("smaller-mode-latched-changed-frame")
		    << QRect(0, 0, 1600, 900) << monitor << false << false << oldMode << oldMode << false;
		QTest::newRow("normal-rect-not-latched")
		    << QRect(100, 100, 800, 600) << monitor << false << false << oldMode << oldMode << false;
	}

	void fullscreenGeometryData() {
		QFETCH(QRect, frame);
		QFETCH(QRect, monitor);
		QFETCH(bool, minimized);
		QFETCH(bool, maximized);
		QFETCH(QRect, latchedFrame);
		QFETCH(QRect, latchedMonitor);
		QFETCH(bool, expected);

		auto latched = fullscreen_geometry::Latch();
		if (latchedFrame.isValid() && latchedMonitor.isValid()) {
			latched = {.frame = latchedFrame, .monitor = latchedMonitor, .monitorId = 1};
		}

		auto detection =
		    fullscreen_geometry::detect(minimized, maximized, frame, monitor, 1, latched);
		QCOMPARE(detection.fullscreen, expected);
	}

	// Reviewer probe (round 2): a 1280x720 fullscreen frame with a newly
	// reported 1920x1080 monitor is still fullscreen on every repeated refresh.
	// The latch must not be replaced by the uncovered monitor on the first one.
	void reviewModeLatchSurvivesRepeatedRefresh() {
		const auto oldMode = QRect(0, 0, 1280, 720);
		const auto newMode = QRect(0, 0, 1920, 1080);
		auto latch = fullscreen_geometry::Latch {.frame = oldMode, .monitor = oldMode, .monitorId = 1};
		for (auto refresh = 0; refresh < 3; refresh++) {
			auto detection = fullscreen_geometry::detect(false, false, oldMode, newMode, 1, latch);
			latch = detection.latch;
			QVERIFY2(detection.fullscreen, "An unchanged fullscreen frame must survive repeated mode-change refreshes");
			QCOMPARE(latch.frame, oldMode);
			QCOMPARE(latch.monitor, oldMode);
		}

		// Adopting the new mode moves the latch to the current monitor.
		auto adopted = fullscreen_geometry::detect(false, false, oldMode, newMode, 1, latch);
		QVERIFY(adopted.fullscreen);
		auto current = fullscreen_geometry::detect(false, false, newMode, newMode, 1, adopted.latch);
		QVERIFY(current.fullscreen);
		QCOMPARE(current.latch.frame, newMode);
		QCOMPARE(current.latch.monitor, newMode);
	}

	// Reviewer probe (round 2): an exit to a normal rectangle that happens to
	// cover the old fullscreen frame must clear the latch, not keep the window
	// fullscreen forever.
	void reviewLatchClearsOnDifferentNormalRect() {
		const auto oldMode = QRect(0, 0, 1280, 720);
		const auto newMode = QRect(0, 0, 1920, 1080);
		auto latch = fullscreen_geometry::Latch {.frame = oldMode, .monitor = oldMode, .monitorId = 1};
		for (auto i = 0; i < 4; i++) {
			auto detection = fullscreen_geometry::detect(false, false, oldMode, newMode, 1, latch);
			QVERIFY(detection.fullscreen);
			latch = detection.latch;
		}

		const auto normal = QRect(0, 0, 1600, 900);
		for (auto i = 0; i < 10; i++) {
			auto detection = fullscreen_geometry::detect(false, false, normal, newMode, 1, latch);
			QVERIFY2(!detection.fullscreen, "A changed normal rect must not retain the stale mode latch forever");
			QVERIFY(!detection.latch.isValid());
			latch = detection.latch;
		}
	}

	// The latch is tied to the exact frame and monitor: a window that moved to
	// another monitor, a maximized window and a different frame clear it, while
	// a minimize on the same monitor keeps it for the restore.
	void reviewLatchLifecycle() {
		const auto mode = QRect(0, 0, 1280, 720);
		const auto other = QRect(1280, 0, 1920, 1080);
		auto latch = fullscreen_geometry::Latch {.frame = mode, .monitor = mode, .monitorId = 1};

		auto moved = fullscreen_geometry::detect(false, false, QRect(1400, 100, 900, 600), other, 2, latch);
		QVERIFY(!moved.fullscreen);
		QVERIFY(!moved.latch.isValid());

		auto minimizedOther = fullscreen_geometry::detect(true, false, QRect(), other, 2, latch);
		QVERIFY(!minimizedOther.fullscreen);
		QVERIFY(!minimizedOther.latch.isValid());

		auto minimizedSame = fullscreen_geometry::detect(true, false, QRect(), mode, 1, latch);
		QVERIFY(!minimizedSame.fullscreen);
		QCOMPARE(minimizedSame.latch.frame, mode);
		QVERIFY(minimizedSame.latch.monitorId == 1);

		auto maximized = fullscreen_geometry::detect(false, true, mode, mode, 1, latch);
		QVERIFY(!maximized.fullscreen);
		QVERIFY(!maximized.latch.isValid());

		auto fresh = fullscreen_geometry::detect(false, false, mode, other, 2, fullscreen_geometry::Latch());
		QVERIFY(!fresh.fullscreen);
		QVERIFY(!fresh.latch.isValid());
	}

	void emptyIncludedKeepsAutomaticPolicyAndRestore() {
		auto* normal = add(1);
		auto original = fakeWindow(normal->hwnd()).frame;
		auto* tool = add(2);
		fakeWindow(tool->hwnd()).exStyle = WS_EX_TOOLWINDOW;
		auto* fixed = add(3);
		fakeWindow(fixed->hwnd()).style = 0;
		auto* dialog = add(4);
		fakeWindow(dialog->hwnd()).className = QStringLiteral("#32770");
		auto* elevated = add(5);
		fakeWindow(elevated->hwnd()).elevated = true;
		auto* excluded = add(6, QStringLiteral("excluded"));
		auto* minimized = add(7);
		fakeWindow(minimized->hwnd()).minimized = true;
		auto* maximized = add(8);
		fakeWindow(maximized->hwnd()).maximized = true;
		manager->setExcluded({QStringLiteral("excluded.exe")});
		enable();
		QVERIFY(manager->included().isEmpty());
		QVERIFY(manager->isTiled(normal));
		for (auto* window: {tool, fixed, dialog, elevated, excluded, minimized, maximized}) {
			QVERIFY(!manager->isTiled(window));
			QCOMPARE(fakeWindow(window->hwnd()).moves, 0);
		}
		QCOMPARE(fakeWindow(normal->hwnd()).frame, QRect(20, 20, 1880, 1040));
		manager->setEnabled(false);
		QCOMPARE(fakeWindow(normal->hwnd()).frame, original);
	}

	void completionLoggedOnceThroughPlaceAndDeadline() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		completionLogs.clear();
		QLoggingCategory::setFilterRules(QStringLiteral("quickshell.windows.tiling.debug=true"));
		auto previousHandler = qInstallMessageHandler(captureCompletion);
		manager->place(m, m.target);
		manager->place(m, m.target);
		// A second attempt finishes through the deadline success path.
		manager->send(m, m.target.translated(1, 1));
		fakeDesktop.now += 2000;
		manager->onDeadline();
		manager->onDeadline();
		qInstallMessageHandler(previousHandler);
		QLoggingCategory::setFilterRules(QString());
		QCOMPARE(completionLogs.size(), 2);
		for (const auto& message: completionLogs) {
			QVERIFY(message.contains(QStringLiteral("\"1\"")));
			QVERIFY(message.contains(QStringLiteral("final frame QRect(20,20 1880x1040)")));
			QVERIFY(message.contains(QStringLiteral("after 0 corrections")));
		}
	}

	// A window of a DPI-unaware process on a 150% monitor. Windows reports its
	// rectangle on the window's own scaled 96-DPI grid: reading and writing from
	// that grid never reaches the physical tile, so the old code exhausted its
	// corrections and floated the window as "ignores being moved".
	void virtualizedWindowLandsOnItsPhysicalTile() {
		// Odd work-area edges keep the tile off the 1.5x grid, so the compositor
		// really rounds it and the placement check has to allow that.
		fakeDesktop.monitors[0].work = {2, 2, 1916, 1076};
		auto* a = add(1);
		auto& window = fakeWindow(a->hwnd());
		window.dpiAwareness = DPI_AWARENESS_UNAWARE;
		window.dpiScale = 1.5;
		fakeDesktop.threadAwareness = DPI_AWARENESS_SYSTEM_AWARE;

		enable();
		auto& m = manager->managed.at(a);
		QVERIFY(manager->isTiled(a));
		QVERIFY(!m.autoFloat);
		QVERIFY(tiling::placementLanded(window.frame, m.target, true, 2));
		QCOMPARE(manager->telemetry()->failedCount(), 0);

		fakeDesktop.now += 2000;
		manager->onDeadline();
		QVERIFY(!m.autoFloat);
		QVERIFY(tiling::placementLanded(window.frame, m.target, true, 2));
		QCOMPARE(manager->telemetry()->failedCount(), 0);

		// The physical context is scoped to the call, not left on the thread.
		QVERIFY(fakeDesktop.awarenessSwitches > 0);
		QCOMPARE(fakeDesktop.threadAwareness, quint32(DPI_AWARENESS_SYSTEM_AWARE));

		// One rounding pixel is a landing, not another correction.
		auto moves = window.moves;
		manager->place(m, m.target);
		QCOMPARE(window.moves, moves);
		QVERIFY(!m.autoFloat);
	}

	void awareWindowKeepsExactPixels() {
		auto* a = add(1);
		enable();
		QCOMPARE(fakeDesktop.awarenessSwitches, 0);
		QCOMPARE(fakeWindow(a->hwnd()).frame, manager->managed.at(a).target);
	}

	void autoFloatDoesNotRetryOnWorkAreaOrMonitorSize() {
		auto* a = add(1);
		auto& native = fakeWindow(a->hwnd());
		native.moveError = ERROR_NOT_ENOUGH_MEMORY;
		enable();
		manager->sync();
		auto& m = manager->managed.at(a);
		QVERIFY(m.autoFloat);
		QVERIFY(!manager->isTiled(a));

		// Work-area changes alone, including fullscreen AppBar transitions, and
		// monitor mode changes must not repeatedly retry a window that failed.
		native.moveError = 0;
		const auto moves = native.moves;
		fakeDesktop.monitors[0].work.adjust(0, 0, -50, -50);
		manager->markAll();
		manager->sync();
		QVERIFY(m.autoFloat);
		QVERIFY(!manager->isTiled(a));
		fakeDesktop.monitors[0] = {{0, 0, 2560, 1440}, {0, 0, 2560, 1440}, true};
		manager->onScreensChanged();
		manager->sync();
		QVERIFY(m.autoFloat);
		QVERIFY(!manager->isTiled(a));
		QCOMPARE(native.moves, moves);
	}

	void autoFloatRetryRespectsExplicitFloatAndIsBounded() {
		auto* a = add(1);
		auto& native = fakeWindow(a->hwnd());
		native.moveError = ERROR_NOT_ENOUGH_MEMORY;
		enable();
		manager->sync();
		auto& m = manager->managed.at(a);
		QVERIFY(m.autoFloat);
		QVERIFY(manager->setFloating(a, true, false));
		manager->sync();
		native.moveError = 0;
		emit a->desktopChanged();
		manager->sync();
		QVERIFY(!manager->isTiled(a));
		QCOMPARE(m.override, TilingManager::Override::Float);

		// A fresh automatic failure cannot loop on later notifications.
		m.override = TilingManager::Override::None;
		native.moveError = ERROR_NOT_ENOUGH_MEMORY;
		emit a->desktopChanged();
		manager->sync();
		manager->sync();
		QVERIFY(m.autoFloat);
		QVERIFY(!manager->isTiled(a));
		const auto moves = native.moves;
		manager->markAll();
		manager->sync();
		drive({a});
		QCOMPARE(native.moves, moves);
		QVERIFY(!manager->isTiled(a));
	}

	void minimizedScopeRemovalRestoresNormalPlacement_data() {
		QTest::addColumn<QRect>("monitor");
		QTest::addColumn<QPoint>("offset");
		QTest::addColumn<bool>("tool");
		QTest::newRow("top-work-area") << QRect(0, 0, 1920, 1080) << QPoint(0, 40) << false;
		QTest::newRow("left-work-area") << QRect(0, 0, 1920, 1080) << QPoint(30, 0) << false;
		QTest::newRow("negative-target-monitor")
		    << QRect(-1920, -1080, 1920, 1080) << QPoint(30, 40) << false;
		QTest::newRow("tool-window") << QRect(0, 0, 1920, 1080) << QPoint(30, 40) << true;
	}

	void minimizedScopeRemovalRestoresNormalPlacement() {
		QFETCH(QRect, monitor);
		QFETCH(QPoint, offset);
		QFETCH(bool, tool);
		auto work = monitor.adjusted(offset.x(), offset.y(), 0, 0);
		if (monitor.topLeft() == QPoint()) fakeDesktop.monitors.clear();
		fakeDesktop.monitors.append({monitor, work, true});
		auto* a = add(1);
		fakeWindow(a->hwnd()).frame.moveTopLeft(monitor.topLeft() + QPoint(100, 100));
		auto original = fakeWindow(a->hwnd()).frame;
		enable();
		fakeWindow(a->hwnd()).normal = fakeWindow(a->hwnd()).frame;
		fakeWindow(a->hwnd()).minimized = true;
		// Tool windows can be explicitly tiled, or acquire the style while tiled.
		if (tool) fakeWindow(a->hwnd()).exStyle = WS_EX_TOOLWINDOW;
		manager->setIncluded({QStringLiteral("terminal")});
		manager->sync();
		QVERIFY(!manager->isTiled(a));
		QVERIFY(fakeWindow(a->hwnd()).minimized);
		QCOMPARE(fakeWindow(a->hwnd()).normal, original);
		QCOMPARE(fakeWindow(a->hwnd()).lastPlacement, original.translated(tool ? QPoint() : -offset));
		QCOMPARE(fakeWindow(a->hwnd()).placements, 1);
		manager->setEnabled(false);
		QCOMPARE(fakeWindow(a->hwnd()).placements, 1);
	}

	void unparkRestoresPlacement_data() {
		QTest::addColumn<bool>("tool");
		QTest::addColumn<bool>("maximized");
		QTest::newRow("minimized") << false << false;
		QTest::newRow("maximized") << false << true;
		QTest::newRow("tool-minimized") << true << false;
		QTest::newRow("tool-maximized") << true << true;
	}

	void unparkRestoresPlacement() {
		QFETCH(bool, tool);
		QFETCH(bool, maximized);
		fakeDesktop.monitors[0].work = {30, 40, 1890, 1040};
		auto* a = add(1);
		enable();
		auto& window = fakeWindow(a->hwnd());
		auto& m = manager->managed.at(a);
		auto restore = QRect(100, 100, 600, 400);
		manager->markOffscreen(m, true, restore);
		window.frame = window.normal = QRect(-10000, -10000, 600, 400);
		window.minimized = !maximized;
		window.maximized = maximized;
		if (tool) window.exStyle = WS_EX_TOOLWINDOW;
		manager->unpark(m);
		QCOMPARE(window.normal, restore);
		QCOMPARE(window.lastPlacement, restore.translated(tool ? QPoint() : QPoint(-30, -40)));
		QCOMPARE(window.placements, 1);
		QCOMPARE(window.minimized, !maximized);
		QCOMPARE(window.maximized, maximized);
		QVERIFY(!m.offscreen);
		QVERIFY(window.parked == nullptr);
	}

	void rescueRestoresPlacement_data() {
		QTest::addColumn<bool>("tool");
		QTest::addColumn<bool>("maximized");
		QTest::addColumn<bool>("visibleNormal");
		for (bool tool: {false, true}) {
			for (bool maximized: {false, true}) {
				for (bool visible: {false, true}) {
					auto name =
					    QStringLiteral("tool-%1-max-%2-visible-%3").arg(tool).arg(maximized).arg(visible);
					QTest::newRow(qPrintable(name)) << tool << maximized << visible;
				}
			}
		}
	}

	void rescueRestoresPlacement() {
		QFETCH(bool, tool);
		QFETCH(bool, maximized);
		QFETCH(bool, visibleNormal);
		fakeDesktop.monitors[0].work = {30, 40, 1890, 1040};
		auto* a = add(1);
		enable();
		auto& window = fakeWindow(a->hwnd());
		auto& m = manager->managed.at(a);
		auto restore = QRect(100, 100, 600, 400);
		manager->markOffscreen(m, true, restore);
		// Screen Y=10 is visible; workspace Y=-30 is wholly outside the monitor.
		auto normal = visibleNormal ? QRect(100, 10, 100, 20) : QRect(-10000, -10000, 600, 400);
		window.frame = window.normal = normal;
		window.minimized = !maximized;
		window.maximized = maximized;
		if (tool) window.exStyle = WS_EX_TOOLWINDOW;
		TilingManager::rescueParked();
		QCOMPARE(window.normal, visibleNormal ? normal : restore);
		QCOMPARE(window.placements, visibleNormal ? 0 : 1);
		if (!visibleNormal) {
			QCOMPARE(window.lastPlacement, restore.translated(tool ? QPoint() : QPoint(-30, -40)));
		}
		QCOMPARE(window.minimized, !maximized);
		QCOMPARE(window.maximized, maximized);
		QVERIFY(window.parked == nullptr);
		manager->markOffscreen(m, false, {}); // Clear the process-local crash slot too.
	}

	void failureCallbackDisablesAfterDeadlineTraversal() {
		auto* a = add(0x1a2b);
		auto* b = add(2);
		fakeWindow(a->hwnd()).ignoreMoves = true;
		fakeWindow(b->hwnd()).ignoreMoves = true;
		int callbacks = 0;
		connect(
		    manager->telemetry(),
		    &tiling::TilingTelemetry::windowTilingFailed,
		    this,
		    [&](const QString& address, const QString& reason) {
			    QVERIFY(address == QStringLiteral("1a2b") || address == QStringLiteral("2"));
			    QVERIFY(!address.startsWith(QStringLiteral("0x")));
			    QCOMPARE(reason, QStringLiteral("unresponsive"));
			    callbacks++;
			    manager->setEnabled(false);
		    }
		);
		enable();
		fakeDesktop.now += 2000;
		manager->onDeadline();
		QVERIFY(manager->enabled());
		QCOMPARE(callbacks, 0);
		QCOMPARE(manager->telemetry()->failedCount(), 2);
		drain();
		QCOMPARE(callbacks, 2);
		QVERIFY(!manager->enabled());
		QVERIFY(manager->managed.empty());
	}

	// A remote desktop client grows into fullscreen faster than corrections can
	// land. Fighting it exhausted the budget and floated the window as min-size.
	void selfResizingWindowIsNotFloatedBeforeItsFullscreenSettles() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);
		QCOMPARE(native.frame, m.target);
		const auto moves = native.moves;
		const auto monitor = fakeDesktop.monitors.first().monitor;

		for (auto width: {1000, 1400, 1800}) {
			native.frame = QRect(monitor.topLeft() + QPoint(40, 20), QSize(width, width * 9 / 16));
			manager->place(m, m.target);
			fakeDesktop.now += 160;
		}
		QCOMPARE(native.moves, moves);
		QVERIFY(!m.autoFloat);
		QCOMPARE(manager->telemetry()->failedCount(), 0);

		// Once it lands on the monitor it is fullscreen, not min-size.
		native.frame = monitor;
		a->refreshFullscreenEpisode();
		manager->markWindow(a);
		manager->sync();
		QVERIFY(native.fullscreen);
		QCOMPARE(manager->stateOf(m), TilingManager::State::Suspended);
		QCOMPARE(native.moves, moves);
		QVERIFY(!m.autoFloat);
		QCOMPARE(manager->telemetry()->failedCount(), 0);
	}

	// Growth in steps slower than the verify tick must not release the wait
	// after one unchanged look; the window has to be quiet for a while.
	void steppedGrowthWaitsForTheQuietInterval() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);
		const auto moves = native.moves;

		auto width = 900;
		for (auto step = 0; step < 4; step++) {
			native.frame = QRect(QPoint(40, 20), QSize(width += 100, 600));
			manager->place(m, m.target);
			fakeDesktop.now += 50;
			recheck(a); // The scheduled re-check sees the same frame.
			fakeDesktop.now += 50;
		}
		QCOMPARE(native.moves, moves);
		QVERIFY(!m.snapping);
		QVERIFY(!m.autoFloat);

		// Still oversize after the quiet interval: judged as before, by easing
		// the window back rather than jumping.
		fakeDesktop.now += 150;
		recheck(a);
		QVERIFY(m.snapping);
		QCOMPARE(native.moves, moves);
		finishSnap();
		QCOMPARE(native.frame, m.target);
		QVERIFY(!m.autoFloat);
	}

	// The window eases from where it settled into its tile in several steps,
	// each between the two, and ends exactly on the tile.
	void settledSelfResizeEasesIntoTheTile() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);

		const auto big = QRect(QPoint(0, 0), QSize(3000, 1900));
		native.frame = big;
		manager->place(m, m.target);
		fakeDesktop.now += 200;
		recheck(a);
		QVERIFY(m.snapping);

		auto last = big.width();
		auto steps = 0;
		for (auto tick = 0; tick < 4; tick++) {
			fakeDesktop.now += 30;
			manager->stepSnaps();
			QVERIFY(native.frame.width() < last);
			QVERIFY(native.frame.width() > m.target.width());
			last = native.frame.width();
			steps++;
		}
		QCOMPARE(steps, 4);

		finishSnap();
		QCOMPARE(native.frame, m.target);
		QVERIFY(!m.snapping);
		QVERIFY(!m.autoFloat);
		QCOMPARE(manager->telemetry()->failedCount(), 0);
	}

	// A new tile mid-easing replaces the easing with a direct placement.
	void newTileMidEasingPlacesDirectly() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);

		native.frame = QRect(QPoint(0, 0), QSize(3000, 1900));
		manager->place(m, m.target);
		fakeDesktop.now += 250;
		recheck(a);
		QVERIFY(m.snapping);

		const auto tile = m.target.adjusted(0, 0, -300, 0);
		manager->place(m, tile);
		QVERIFY(!m.snapping);
		QCOMPARE(m.target, tile);
		QCOMPARE(native.frame, tile);
	}

	// The window leaving the tiled state ends the easing.
	void easingStopsWhenTheWindowLeaves() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);

		native.frame = QRect(QPoint(0, 0), QSize(3000, 1900));
		manager->place(m, m.target);
		fakeDesktop.now += 200;
		recheck(a);
		QVERIFY(m.snapping);
		fakeDesktop.now += 30;
		manager->stepSnaps();
		const auto moves = native.moves;

		native.minimized = true;
		manager->stepSnaps();
		QVERIFY(!m.snapping);
		QCOMPARE(native.moves, moves);
	}

	// Waiting is bounded and spent once: a window that never stops changing is
	// corrected, then given up on, instead of being retried every two seconds.
	void selfResizingWindowIsGivenUpOnAfterTheSettleLimit() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);
		const auto moves = native.moves;

		auto width = 900;
		auto grow = [&]() {
			// Taller than the tile, so give-up reads it as min-size.
			native.frame = QRect(QPoint(40, 20), QSize(width += 50, 1200));
			manager->place(m, m.target);
		};
		for (auto step = 0; step < 4; step++) {
			grow();
			fakeDesktop.now += 600;
		}
		QCOMPARE(native.moves, moves);

		// Past the limit each change is acted on, not waited on again: two
		// easings that spend no correction, then the two corrections, then give-up.
		for (auto snap = 0; snap < 2; snap++) {
			grow();
			QVERIFY(m.snapping);
			finishSnap();
		}
		QCOMPARE(m.corrections, 0);
		for (auto correction = 0; correction < 2; correction++) {
			grow();
			QVERIFY(!m.snapping);
			QVERIFY(!m.autoFloat);
		}
		grow();
		// It grew itself away from its slot's corner: off target, it floats.
		QCOMPARE(m.disposition, tiling::PlacementDisposition::OffTarget);
		QVERIFY(m.autoFloat);
	}

	// Growth in steps slower than the check interval and the quiet interval
	// must not float the window: easings do not spend the correction budget.
	void slowSteppedGrowthIsNotFloatedAcrossEasings() {
		auto* a = add(1);
		enable();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		manager->place(m, m.target);

		auto advance = [&](int ms) {
			for (auto elapsed = 0; elapsed < ms; elapsed += 50) {
				fakeDesktop.now += 50;
				recheck(a);
				manager->stepSnaps();
			}
		};

		auto width = 2400;
		for (auto step = 0; step < 4; step++) {
			// Taller than the tile, so an exhausted budget would read as min-size.
			native.frame = QRect(QPoint(0, 0), QSize(width += 150, 1500));
			manager->place(m, m.target);
			advance(250);
		}
		QVERIFY(!m.autoFloat);
		QCOMPARE(manager->telemetry()->failedCount(), 0);

		advance(600);
		QCOMPARE(native.frame, m.target);
		QVERIFY(!m.autoFloat);
	}

	void failureCallbackDisablesAfterGiveUp() {
		auto* a = add(0xabcd);
		enable();
		auto& m = manager->managed.at(a);
		auto actual = m.target.adjusted(0, 0, 100, 0);
		fakeWindow(a->hwnd()).frame = actual;
		m.corrections = 2;
		m.snaps = 2;
		m.lastCorrection = fakeDesktop.now;
		int callbacks = 0;
		connect(
		    manager->telemetry(),
		    &tiling::TilingTelemetry::windowTilingFailed,
		    this,
		    [&](const QString& address, const QString& reason) {
			    QCOMPARE(address, QStringLiteral("abcd"));
			    QCOMPARE(reason, QStringLiteral("min-size"));
			    callbacks++;
			    manager->setEnabled(false);
		    }
		);
		// The first sighting of a changed frame waits for it to hold still.
		manager->place(m, m.target);
		QVERIFY(!m.autoFloat);
		fakeDesktop.now += 300;
		manager->place(m, m.target);
		// giveUp completed all mutation before publication.
		QCOMPARE(m.disposition, tiling::PlacementDisposition::SizeConstraint);
		QVERIFY(!m.accepted.isNull());
		QCOMPARE(callbacks, 0);
		drain();
		QCOMPARE(callbacks, 1);
		QVERIFY(!manager->enabled());
	}

	// P1-1. Hyprland keeps a window that cannot shrink to its slot tiled
	// (size_limits_tiled = false); it never floats it for its minimum size.
	void minSizeWindowKeepsItsTile() {
		auto* a = add(1);
		auto* b = add(2);
		fakeWindow(b->hwnd()).minimumSize = QSize(1000, 0);
		enable();
		const auto aTile = manager->managed.at(a).target;
		drive({a, b});
		auto& m = manager->managed.at(b);
		QVERIFY(manager->isTiled(b));
		QVERIFY(!m.autoFloat);
		QCOMPARE(m.disposition, tiling::PlacementDisposition::SizeConstraint);
		QCOMPARE(manager->telemetry()->failedCount(), 1);
		QCOMPARE(manager->managed.at(a).target, aTile);
		QCOMPARE(fakeWindow(a->hwnd()).frame, aTile);
		QVERIFY(fakeDesktop.monitors.first().work.contains(fakeWindow(b->hwnd()).frame));
	}

	void minSizeWindowLandsWhenSlotGrows() {
		auto* a = add(1);
		auto* b = add(2);
		fakeWindow(b->hwnd()).minimumSize = QSize(1000, 0);
		enable();
		drive({a, b});
		close(a);
		drive({b});
		QVERIFY(manager->isTiled(b));
		QCOMPARE(fakeWindow(b->hwnd()).frame, QRect(20, 20, 1880, 1040));
		QCOMPARE(manager->managed.at(b).disposition, tiling::PlacementDisposition::Landed);
	}

	// A short-lived second window pushes a real window under its minimum.
	// Once it closes, the real window takes its whole slot again.
	void transientWindowCannotFloatRealWindow() {
		auto* real = add(1);
		auto& native = fakeWindow(real->hwnd());
		native.minimumSize = QSize(1000, 0);
		enable();
		QCOMPARE(native.frame, QRect(20, 20, 1880, 1040));
		auto* transient = add(2, QStringLiteral("fixture"));
		manager->manage(transient);
		manager->sync();
		drive({real, transient});
		QVERIFY(manager->isTiled(real));
		close(transient);
		drive({real});
		QVERIFY(manager->isTiled(real));
		QVERIFY(!manager->managed.at(real).autoFloat);
		QCOMPARE(native.frame, QRect(20, 20, 1880, 1040));
	}

	// The larger frame stays anchored at its slot but inside the work area, and
	// that one write is not repeated by later layout passes.
	void minSizeOverflowStaysOnScreen() {
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(b->hwnd());
		native.minimumSize = QSize(1000, 0);
		enable();
		drive({a, b});
		const auto tile = manager->managed.at(b).target;
		const auto work = fakeDesktop.monitors.first().work;
		QVERIFY(manager->isTiled(b));
		QCOMPARE(native.frame.top(), tile.top());
		QCOMPARE(native.frame.width(), 1000);
		QVERIFY(native.frame.left() + native.frame.width() <= work.left() + work.width());
		const auto moves = native.moves;
		manager->markAll();
		manager->sync();
		drive({a, b});
		QCOMPARE(native.moves, moves);
	}

	// An accepted overflow that is later moved elsewhere left its tile. It is
	// judged off target before size and floats, with no new corrections.
	void movedOverflowWindowFloatsOffTarget() {
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(a->hwnd());
		native.minimumSize = QSize(1100, 400);
		enable();
		drive({a, b});
		auto& m = manager->managed.at(a);
		QCOMPARE(m.target, QRect(10, 10, 945, 1060));
		QCOMPARE(m.disposition, tiling::PlacementDisposition::SizeConstraint);
		QVERIFY(manager->isTiled(a));
		native.frame = QRect(1400, -100, 1100, 700);
		manager->onRectChanged(a);
		manager->verify();
		manager->sync();
		QCOMPARE(m.disposition, tiling::PlacementDisposition::OffTarget);
		QVERIFY(m.autoFloat);
		QVERIFY(!manager->isTiled(a));
	}

	// P1-1. Growth is not a move away: an accepted overflow that keeps growing
	// while anchored at its corner stays tiled and is not written again. The
	// anchor is the slot corner for a left slot and the fitted overflow corner
	// for a right slot, so the later judgement must use the accepted corner.
	void anchoredOverflowCanGrowWithoutFloating_data() {
		QTest::addColumn<bool>("rightSlot");
		QTest::newRow("at-original-slot-corner") << false;
		QTest::newRow("at-fitted-overflow-corner") << true;
	}

	void anchoredOverflowCanGrowWithoutFloating() {
		QFETCH(bool, rightSlot);
		auto* a = add(1);
		auto* b = add(2);
		auto* window = rightSlot ? b : a;
		auto& native = fakeWindow(window->hwnd());
		native.minimumSize = QSize(1100, 400);
		enable();
		drive({a, b});
		auto& m = manager->managed.at(window);
		QCOMPARE(m.disposition, tiling::PlacementDisposition::SizeConstraint);
		const auto anchor = native.frame.topLeft();
		if (rightSlot) QVERIFY(anchor != m.target.topLeft());
		else QCOMPARE(anchor, m.target.topLeft());
		const auto moves = native.moves;
		for (int i = 0; i < 20; ++i) {
			native.minimumSize.rwidth() += 5;
			native.frame.setWidth(native.frame.width() + 5);
			manager->onRectChanged(window);
			manager->verify();
			manager->sync();
			QVERIFY(manager->isTiled(window));
			QVERIFY(!m.autoFloat);
			QCOMPARE(m.disposition, tiling::PlacementDisposition::SizeConstraint);
			QCOMPARE(native.frame.topLeft(), anchor);
		}
		QCOMPARE(native.moves, moves);
	}

	// A minimum larger than the work area cannot fit. The frame starts at the
	// work area's leading edge, never in the reserved space before it, and
	// overflows past the far edge; the other axis stays inside. At 250% the
	// edge is rounded inward onto the window's grid.
	void infeasibleMinimumAnchorsAtWorkAreaLeadingEdge_data() {
		QTest::addColumn<bool>("vertical");
		QTest::addColumn<quint32>("awareness");
		QTest::addColumn<qreal>("scale");
		QTest::addColumn<QRect>("work");
		QTest::newRow("vertical") << true << quint32(2) << qreal(1.0) << QRect(0, 40, 1920, 1000);
		QTest::newRow("vertical-250") << true << quint32(1) << qreal(2.5) << QRect(0, 41, 1920, 999);
		QTest::newRow("horizontal") << false << quint32(2) << qreal(1.0) << QRect(40, 0, 1000, 1080);
		QTest::newRow("horizontal-250") << false << quint32(1) << qreal(2.5) << QRect(41, 0, 999, 1080);
	}

	void infeasibleMinimumAnchorsAtWorkAreaLeadingEdge() {
		QFETCH(bool, vertical);
		QFETCH(quint32, awareness);
		QFETCH(qreal, scale);
		QFETCH(QRect, work);
		fakeDesktop.monitors[0].work = work;
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(a->hwnd());
		native.dpiAwareness = awareness;
		native.dpiScale = scale;
		native.minimumSize = vertical ? QSize(900, work.height() + 60) : QSize(work.width() + 60, 0);
		enable();
		drive({a, b}, 40);
		QVERIFY(manager->isTiled(a));
		QCOMPARE(manager->managed.at(a).disposition, tiling::PlacementDisposition::SizeConstraint);
		const auto frame = native.frame;
		const auto lead = vertical ? frame.top() - work.top() : frame.left() - work.left();
		QVERIFY2(lead >= 0, "the overflow covers reserved space before the work area");
		QVERIFY(lead <= static_cast<int>(std::ceil(scale)) - 1);
		if (vertical) {
			QVERIFY(frame.left() >= work.left());
			QVERIFY(frame.left() + frame.width() <= work.left() + work.width());
		} else {
			QVERIFY(frame.top() >= work.top());
			QVERIFY(frame.top() + frame.height() <= work.top() + work.height());
		}
	}

	// At 250% the compositor rounds a bitmap-scaled window onto its grid. The
	// clamp to the trailing edge rounds inward, so it never lands in reserved space.
	void overflowClampRoundsInwardAt250Percent() {
		fakeDesktop.monitors[0].work = QRect(0, 0, 1919, 1080);
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(b->hwnd());
		native.dpiAwareness = DPI_AWARENESS_SYSTEM_AWARE;
		native.dpiScale = 2.5;
		native.minimumSize = QSize(1100, 0);
		enable();
		drive({a, b});
		QVERIFY(manager->isTiled(b));
		QCOMPARE(manager->managed.at(b).disposition, tiling::PlacementDisposition::SizeConstraint);
		QVERIFY(fakeDesktop.monitors[0].work.contains(native.frame));
	}

	// A busy app whose pre-tiling frame is larger than its slot has not
	// answered yet. It is not judged by that old frame: it keeps its tile.
	void busyAppWithALargerFrameKeepsItsTile() {
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(b->hwnd());
		native.frame = QRect(100, 80, 1600, 900);
		native.ignoreMoves = true;
		enable();
		fakeDesktop.now += 2000;
		manager->onDeadline();
		manager->sync();
		auto& m = manager->managed.at(b);
		QCOMPARE(m.disposition, tiling::PlacementDisposition::Deadline);
		QVERIFY(manager->isTiled(b));
		QVERIFY(!m.autoFloat);
		QCOMPARE(native.moves, 1);
		Q_UNUSED(a);
	}

	// P1-2. The user's explicit tile wins over every automatic float.
	void explicitTileSurvivesAutomaticFailure_data() {
		QTest::addColumn<bool>("refused");
		QTest::newRow("min-size") << false;
		QTest::newRow("failed") << true;
	}

	void explicitTileSurvivesAutomaticFailure() {
		QFETCH(bool, refused);
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(b->hwnd());
		if (refused) native.moveError = ERROR_NOT_ENOUGH_MEMORY;
		else native.minimumSize = QSize(1000, 0);
		enable();
		drive({a, b});
		QVERIFY(manager->setFloating(b, false, false));
		manager->sync();
		drive({a, b});
		QVERIFY(manager->isTiled(b));
		QCOMPARE(manager->managed.at(b).override, TilingManager::Override::Tile);
	}

	// An automatic float gets one fresh attempt when its window changes
	// monitor or desktop, never on a work-area change alone.
	void autoFloatRetriesOnNewMonitorOrDesktop_data() {
		QTest::addColumn<bool>("screen");
		QTest::newRow("screen") << true;
		QTest::newRow("desktop") << false;
	}

	void autoFloatRetriesOnNewMonitorOrDesktop() {
		QFETCH(bool, screen);
		auto* platform = addLeftMonitor();
		const auto cleanupScreen = qScopeGuard([&]() { removeMonitor(platform); });
		auto* a = add(1);
		auto& native = fakeWindow(a->hwnd());
		native.monitor = 0;
		native.moveError = ERROR_NOT_ENOUGH_MEMORY;
		enable();
		manager->sync();
		QVERIFY(!manager->isTiled(a));
		native.moveError = 0;
		fakeDesktop.monitors[0].work.adjust(0, 0, 0, -40);
		manager->markAll();
		manager->sync();
		QVERIFY(!manager->isTiled(a));

		if (screen) {
			native.monitor = 1;
			native.frame.translate(-1920, 0);
			emit a->screenChanged();
		} else {
			emit a->desktopChanged();
		}
		manager->sync();
		QVERIFY(manager->isTiled(a));
		QVERIFY(!manager->managed.at(a).autoFloat);
		QCOMPARE(native.frame, screen ? QRect(-1900, 20, 1880, 1040) : QRect(20, 20, 1880, 1000));
	}

	// A sibling closing frees capacity in the layout the window floated from.
	// That buys one fresh attempt, bounded by the tile's own budget.
	void autoFloatRetriesOnceWhenASiblingCloses_data() {
		QTest::addColumn<bool>("stillRefused");
		QTest::newRow("lands") << false;
		QTest::newRow("refused-again") << true;
	}

	void autoFloatRetriesOnceWhenASiblingCloses() {
		QFETCH(bool, stillRefused);
		auto* a = add(1);
		auto* b = add(2);
		auto& native = fakeWindow(b->hwnd());
		native.moveError = ERROR_NOT_ENOUGH_MEMORY;
		enable();
		manager->sync();
		QVERIFY(!manager->isTiled(b));
		const auto moves = native.moves;
		manager->markAll();
		manager->sync();
		QCOMPARE(native.moves, moves);

		if (!stillRefused) native.moveError = 0;
		close(a);
		manager->sync();
		if (stillRefused) {
			QVERIFY(!manager->isTiled(b));
			QCOMPARE(native.moves, moves + 2); // One placement, one floating restore.
			manager->markAll();
			manager->sync();
			QCOMPARE(native.moves, moves + 2);
		} else {
			QVERIFY(manager->isTiled(b));
			QCOMPARE(native.frame, QRect(20, 20, 1880, 1040));
		}
	}

	// F2. A placement that ended in its slot is judged again when the window
	// later moves, but with the budget it already spent: no new corrections.
	void settledPlacementIsJudgedAgainAfterALaterMove_data() {
		QTest::addColumn<int>("move");
		QTest::newRow("onto-the-tile") << 0;
		QTest::newRow("inside-the-tile") << 1;
		QTest::newRow("off-the-tile") << 2;
	}

	void settledPlacementIsJudgedAgainAfterALaterMove() {
		QFETCH(int, move);
		auto* a = add(1);
		auto* b = add(2);
		enable();
		manager->onRectChanged(a);
		manager->verify();
		auto& m = manager->managed.at(a);
		auto& native = fakeWindow(a->hwnd());
		const auto tile = m.target;
		native.frame = tile.adjusted(0, 0, -100, -50);
		manager->giveUp(m, native.frame);
		QVERIFY(manager->isTiled(a));
		QCOMPARE(m.disposition, tiling::PlacementDisposition::CorrectionLimit);
		const auto moves = native.moves;

		const QRect moved[] = {tile, QRect(200, 300, 400, 300), QRect(1200, 300, 400, 300)};
		native.frame = moved[move];
		manager->onRectChanged(a);
		manager->verify();
		manager->sync();
		switch (move) {
		case 0:
			QCOMPARE(m.disposition, tiling::PlacementDisposition::Landed);
			QVERIFY(manager->isTiled(a));
			QCOMPARE(native.moves, moves);
			break;
		case 1:
			QCOMPARE(m.disposition, tiling::PlacementDisposition::CorrectionLimit);
			QCOMPARE(m.accepted, moved[move]);
			QVERIFY(manager->isTiled(a));
			QCOMPARE(native.moves, moves);
			break;
		case 2:
			// It left its slot: an off-target window floats, as before.
			QCOMPARE(m.disposition, tiling::PlacementDisposition::OffTarget);
			QVERIFY(!manager->isTiled(a));
			QVERIFY(m.autoFloat);
			break;
		}
		Q_UNUSED(b);
	}

	// P1-3. A busy app runs the queued move after the deadline; it keeps its
	// tile, and the late landing completes the placement.
	void busyAppKeepsItsTile() {
		auto* window = add(1);
		auto& native = fakeWindow(window->hwnd());
		native.ignoreMoves = true; // Busy for 2 s, not yet hung (5 s).
		enable();
		auto& m = manager->managed.at(window);
		fakeDesktop.now += 2000;
		manager->onDeadline();
		manager->sync();
		QCOMPARE(m.disposition, tiling::PlacementDisposition::Deadline);
		QVERIFY(manager->isTiled(window));
		QCOMPARE(native.moves, 1);

		native.ignoreMoves = false;
		native.frame = m.target;
		manager->onRectChanged(window);
		manager->verify();
		QVERIFY(manager->isTiled(window));
		QVERIFY(!m.autoFloat);
		QCOMPARE(m.disposition, tiling::PlacementDisposition::Landed);
	}

	// F4. A system-aware window that Windows bitmap-scales by 1.75 lands on
	// whole units of its own grid, one pixel off the tile. That is a landing.
	void systemAwareWindowLandsWithinItsBitmapScale() {
		fakeDesktop.monitors[0].work = {2, 2, 1916, 1076};
		auto* a = add(1);
		auto& native = fakeWindow(a->hwnd());
		native.dpiAwareness = DPI_AWARENESS_SYSTEM_AWARE;
		native.dpiScale = 1.75;
		enable();
		auto& m = manager->managed.at(a);
		manager->onRectChanged(a);
		manager->verify();
		QVERIFY(native.frame != m.target);
		QCOMPARE(m.disposition, tiling::PlacementDisposition::Landed);
		QCOMPARE(native.moves, 1);
		fakeDesktop.now += 2000;
		manager->onDeadline();
		drive({a});
		QCOMPARE(m.disposition, tiling::PlacementDisposition::Landed);
		QCOMPARE(native.moves, 1);
		QCOMPARE(manager->telemetry()->failedCount(), 0);
		QCOMPARE(fakeDesktop.threadAwareness, quint32(DPI_AWARENESS_PER_MONITOR_AWARE));
	}

	// P2-1. A title-bar drag drops with the cursor in the upper half of a
	// landscape target, short of its top band. The tile's shape decides
	// nothing: the centre swaps the two windows, here across monitors.
	void titleBarDropOnLandscapeTileSwaps() {
		auto* platform = addLeftMonitor();
		const auto cleanupScreen = qScopeGuard([&]() { removeMonitor(platform); });
		auto* dragged = add(1);
		auto* neighbour = add(2);
		auto* target = add(3);
		fakeWindow(dragged->hwnd()).monitor = 0;
		fakeWindow(neighbour->hwnd()).monitor = 0;
		fakeWindow(target->hwnd()).monitor = 1;
		enable();
		auto* source = manager->managed.at(dragged).layout;
		auto* destination = manager->managed.at(target).layout;
		const auto from = source->tree->box(manager->idOf(dragged));
		const auto box = destination->tree->box(manager->idOf(target));
		QVERIFY(box.width() > box.height());
		const auto cursor = QPoint(box.left() + box.width() / 4, box.top() + box.height() / 3);
		manager->onMoveSizeStarted(dragged);
		fakeDesktop.cursor = cursor;
		fakeWindow(dragged->hwnd()).frame.moveTopLeft(cursor - QPoint(100, 15));
		manager->onMoveSizeEnded(dragged);
		manager->sync();
		QCOMPARE(manager->managed.at(dragged).layout, destination);
		QCOMPARE(manager->managed.at(target).layout, source);
		QCOMPARE(destination->tree->box(manager->idOf(dragged)), box);
		QCOMPARE(source->tree->box(manager->idOf(target)), from);
	}

	void countCallbackDisablesAfterStartup() {
		add(1);
		connect(manager->telemetry(), &tiling::TilingTelemetry::managedCountChanged, this, [&]() {
			manager->setEnabled(false);
		});
		enable();
		QVERIFY(manager->enabled());
		drain();
		QVERIFY(!manager->enabled());
		QVERIFY(manager->managed.empty());
	}

	void centersFloatingWindowOnTheWorkArea() {
		auto* window = add(1);
		fakeDesktop.monitors[0].work = QRect(10, 20, 1200, 800);
		fakeWindow(window->hwnd()).frame = QRect(100, 100, 400, 300);

		TilingManager::center(window);

		QCOMPARE(fakeWindow(window->hwnd()).frame, QRect(410, 270, 400, 300));
	}

	void accessorLossNotifiesPinObservers() {
		VirtualDesktops desktops;
		desktops.setPinExports(true, true, true);
		qs::hyprland::ipc::PinSupport pinObserver(&desktops);
		QSignalSpy supported(&pinObserver, &qs::hyprland::ipc::PinSupport::pinSupportedChanged);
		int pinnedRefreshes = 0;
		// The toplevel refresh subscribes to the same accessor transition.
		connect(&desktops, &VirtualDesktops::accessorStateChanged, &pinObserver, [&]() {
			pinnedRefreshes++;
		});
		QVERIFY(!pinObserver.pinSupported());
		desktops.setAccessorLoaded(true);
		drain();
		QVERIFY(pinObserver.pinSupported());
		QCOMPARE(supported.count(), 1);
		desktops.setAccessorLoaded(false);
		drain();
		QVERIFY(!pinObserver.pinSupported());
		QCOMPARE(supported.count(), 2);
		QCOMPARE(pinnedRefreshes, 2);
		auto pin = qs::hyprland::ipc::pinCapability(desktops.pinSupported(), true);
		QVERIFY(!pin.supported);
		QVERIFY(!pin.pinned);
		desktops.setAccessorLoaded(false);
		drain();
		QCOMPARE(supported.count(), 2);
	}
};

} // namespace qs::windows

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QGuiApplication app(argc, argv);
	qs::windows::TilingManagerTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "tiling_manager_test.moc"
