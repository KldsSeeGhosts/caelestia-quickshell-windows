#include "../hyprland/monitor.hpp"
#include "../fullscreen_occluders.hpp"
#include "../util.hpp"

#include <qguiapplication.h>
#include <qpointer.h>
#include <qsignalspy.h>
#include <qtest.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>

#include <qt_windows.h>

using namespace qs::hyprland::ipc;

namespace {
class FakeScreen: public QPlatformScreen {
public:
	QRect rect {0, 0, 2560, 1440};
	qreal scale = 1.5;
	QRect geometry() const override { return this->rect; }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_RGB32; }
	QString name() const override { return QStringLiteral("Contoso OLED 32"); }
	qreal devicePixelRatio() const override { return this->scale; }
	QDpi logicalDpi() const override { return {96, 96}; }
};
} // namespace

// A hot-unplug destroys the QScreen, but HyprlandIpc::syncMonitors() only runs
// after the screen topology timer. The monitor must survive that gap without
// dereferencing the destroyed screen; a WinEvent-driven workspace/focus change
// reaches refreshIpcObject() in exactly that window (#215).
class HyprlandMonitorTest: public QObject {
	Q_OBJECT

private slots:
	void destroyedScreenIsIgnored() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		QVERIFY(screen != nullptr);

		// Both readers return before evaluating the monitor's IPC bindings, so
		// this fixture deliberately has no HyprlandIpc singleton (which would
		// start window tracking and desktop observation).
		HyprlandMonitor monitor(nullptr, screen);
		QCOMPARE(monitor.screen(), screen);

		// The live-screen path still runs without an IPC owner.
		monitor.updateFromScreen(7);
		monitor.refreshIpcObject();
		QCOMPARE(monitor.bindableId().value(), 7);

		QPointer<QScreen> guarded(screen);
		QWindowSystemInterface::handleScreenRemoved(platform);
		QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 1000);

		// Both calls must skip their whole body. Without the guard they
		// dereference the destroyed screen (the live fault) or, at minimum,
		// change the monitor below.
		monitor.updateFromScreen(9);
		monitor.refreshIpcObject();

		QCOMPARE(monitor.screen(), nullptr);
		QCOMPARE(monitor.bindableId().value(), 7);
	}

	// The AppBar verdict is published per screen, so QML can see a fullscreen
	// window that is hidden from the toplevels (tool window, no-activate).
	void publishesFullscreenOccluderPerScreen() {
		auto* first = new FakeScreen;
		auto* second = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(first);
		QWindowSystemInterface::handleScreenAdded(second);
		auto* screenA = first->screen();
		auto* screenB = second->screen();

		HyprlandMonitor monitorA(nullptr, screenA);
		HyprlandMonitor monitorB(nullptr, screenB);
		monitorA.updateFromScreen(1);
		monitorB.updateFromScreen(2);
		QCOMPARE(monitorA.lastIpcObject().value("fullscreenOccluder").toBool(), false);

		QSignalSpy changedA(&monitorA, &HyprlandMonitor::lastIpcObjectChanged);
		QSignalSpy changedB(&monitorB, &HyprlandMonitor::lastIpcObjectChanged);

		auto* occluders = qs::windows::FullscreenOccluders::instance();
		occluders->set(screenA, true);
		QVERIFY(monitorA.lastIpcObject().value("fullscreenOccluder").toBool());
		QCOMPARE(monitorB.lastIpcObject().value("fullscreenOccluder").toBool(), false);
		QCOMPARE(changedA.count(), 1);
		QCOMPARE(changedB.count(), 0);

		// Setting the same state again publishes nothing.
		occluders->set(screenA, true);
		QCOMPARE(changedA.count(), 1);

		occluders->set(screenA, false);
		QCOMPARE(monitorA.lastIpcObject().value("fullscreenOccluder").toBool(), false);
		QCOMPARE(changedA.count(), 2);

		// A destroyed screen forgets its state, so a reused address starts clean.
		occluders->set(screenB, true);
		QPointer<QScreen> guarded(screenB);
		QWindowSystemInterface::handleScreenRemoved(second);
		QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 1000);

		QWindowSystemInterface::handleScreenRemoved(first);
	}

	// The focused monitor follows the latest focus event, as Hyprland
	// publishes it to the shell: a foreground window and a click always win;
	// a cursor crossing only wins while focus-follows-mouse is enabled.
	void focusedMonitorFollowsLatestEvent() {
		auto* first = new FakeScreen;
		auto* second = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(first);
		QWindowSystemInterface::handleScreenAdded(second);
		auto* screenA = first->screen();
		auto* screenB = second->screen();

		QVERIFY(qs::windows::focusActiveWindow(screenA));
		QCOMPARE(qs::windows::focusedScreen(), screenA);

		// The cursor stays on A while the foreground moves to a window on B.
		QVERIFY(qs::windows::focusActiveWindow(screenB));
		QCOMPARE(qs::windows::focusedScreen(), screenB);

		// Focus-follows-mouse off: a bare cursor crossing does nothing.
		QVERIFY(!qs::windows::focusCursorMonitor(screenA, false));
		QCOMPARE(qs::windows::focusedScreen(), screenB);

		// Focus-follows-mouse on: the crossing wins.
		QVERIFY(qs::windows::focusCursorMonitor(screenA, true));
		QCOMPARE(qs::windows::focusedScreen(), screenA);

		// A click always wins, even with focus-follows-mouse off.
		QVERIFY(qs::windows::focusClickedMonitor(screenB));
		QCOMPARE(qs::windows::focusedScreen(), screenB);

		// The same event repeated does not move focus again.
		QVERIFY(!qs::windows::focusClickedMonitor(screenB));
		QCOMPARE(qs::windows::focusedScreen(), screenB);

		QWindowSystemInterface::handleScreenRemoved(second);
		QWindowSystemInterface::handleScreenRemoved(first);
	}
};

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	qputenv("QT_SCALE_FACTOR", "1");
	QGuiApplication app(argc, argv);
	HyprlandMonitorTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "hyprland_monitor_test.moc"
