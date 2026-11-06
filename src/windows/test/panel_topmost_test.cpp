#include <qcoreapplication.h>
#include <qtest.h>

#include "../panel_topmost.hpp"
#include "../window_event_filter.hpp"

class PanelTopmostTest: public QObject {
	Q_OBJECT;

private slots:
	void desktopReorderUsesParentNotTrackedApplication() {
		using qs::windows::window_events::isDesktopReorder;
		auto* desktop = reinterpret_cast<HWND>(1);
		auto* app = reinterpret_cast<HWND>(2);
		QVERIFY(isDesktopReorder(EVENT_OBJECT_REORDER, desktop, OBJID_WINDOW, CHILDID_SELF, desktop));
		QVERIFY(isDesktopReorder(EVENT_OBJECT_REORDER, desktop, OBJID_CLIENT, CHILDID_SELF, desktop));
		QVERIFY(!isDesktopReorder(EVENT_OBJECT_REORDER, app, OBJID_WINDOW, CHILDID_SELF, desktop));
		QVERIFY(!isDesktopReorder(EVENT_OBJECT_REORDER, desktop, OBJID_WINDOW, 1, desktop));
		QVERIFY(!isDesktopReorder(EVENT_OBJECT_REORDER, desktop, OBJID_MENU, CHILDID_SELF, desktop));
		QVERIFY(!isDesktopReorder(EVENT_OBJECT_LOCATIONCHANGE, desktop, OBJID_WINDOW, CHILDID_SELF, desktop));
		QVERIFY(!isDesktopReorder(EVENT_OBJECT_REORDER, nullptr, OBJID_WINDOW, CHILDID_SELF, nullptr));
	}

	void restoreOnlyWhenExpectedAndDemoted() {
		using qs::windows::panel_topmost::needsRestore;
		QVERIFY(!needsRestore(false, false));
		QVERIFY(!needsRestore(false, true));
		QVERIFY(needsRestore(true, false));
		QVERIFY(!needsRestore(true, true));
		// Another topmost application can cover the shell with its style intact.
		QVERIFY(needsRestore(true, true, true));
		QVERIFY(!needsRestore(false, true, true));
		QVERIFY(!needsRestore(false, false, true));
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	PanelTopmostTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "panel_topmost_test.moc"
