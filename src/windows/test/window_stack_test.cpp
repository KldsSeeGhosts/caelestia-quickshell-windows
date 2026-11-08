// The window stack snapshot's decisions, without a desktop: which observed
// windows are listed, how a physical frame maps to logical global coordinates,
// and what QML receives. window_stack_native_test creates real windows in a
// known order and checks the enumeration.
#include <qcoreapplication.h>
#include <qrect.h>
#include <qtest.h>
#include <qvariant.h>

#include "../system_surface.hpp"
#include "../window_stack.hpp"

using namespace qs::windows::window_stack;
using qs::windows::system_surface::Identity;

namespace {

Identity application() {
	Identity identity;
	identity.windowClass = QStringLiteral("Chrome_WidgetWin_1");
	identity.processName = QStringLiteral("chrome.exe");
	identity.title = QStringLiteral("A page");
	identity.visible = true;
	identity.hasTitle = true;
	return identity;
}

} // namespace

class WindowStackTest: public QObject {
	Q_OBJECT;

private slots:
	void listsAnOrdinaryApplication() { QVERIFY(isStackWindow(application())); }

	void dropsWindowsThatAreNotOnScreen() {
		auto hidden = application();
		hidden.visible = false;
		QVERIFY(!isStackWindow(hidden));

		// Another virtual desktop and a suspended UWP app are both cloaked.
		auto cloaked = application();
		cloaked.cloaked = true;
		QVERIFY(!isStackWindow(cloaked));
	}

	void dropsChildAndUntitledWindows() {
		auto owned = application();
		owned.root = false;
		QVERIFY(!isStackWindow(owned));

		auto child = application();
		child.child = true;
		QVERIFY(!isStackWindow(child));

		auto untitled = application();
		untitled.hasTitle = false;
		untitled.title.clear();
		QVERIFY(!isStackWindow(untitled));
	}

	void dropsTheDesktopAndTheTaskbars() {
		for (const auto* name: {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}) {
			auto shell = application();
			shell.windowClass = QString::fromLatin1(name);
			shell.processName = QStringLiteral("explorer.exe");
			QVERIFY2(!isStackWindow(shell), name);
		}
	}

	void dropsOtherShellSurfaces() {
		auto start = application();
		start.windowClass = QStringLiteral("Windows.UI.Core.CoreWindow");
		start.processName = QStringLiteral("startmenuexperiencehost.exe");
		QVERIFY(!isStackWindow(start));

		auto switcher = application();
		switcher.windowClass = QStringLiteral("XamlExplorerHostIslandWindow");
		switcher.processName = QStringLiteral("explorer.exe");
		QVERIFY(!isStackWindow(switcher));

		// A UWP application also uses CoreWindow, outside a shell host.
		auto uwp = application();
		uwp.windowClass = QStringLiteral("Windows.UI.Core.CoreWindow");
		uwp.processName = QStringLiteral("calculatorapp.exe");
		QVERIFY(isStackWindow(uwp));

		// File Explorer shares a process with the taskbar and is an application.
		auto explorer = application();
		explorer.windowClass = QStringLiteral("CabinetWClass");
		explorer.processName = QStringLiteral("explorer.exe");
		QVERIFY(isStackWindow(explorer));
	}

	void dropsTooltipsAndPopups() {
		auto tool = application();
		tool.toolWindow = true;
		QVERIFY(!isStackWindow(tool));

		auto noActivate = application();
		noActivate.noActivate = true;
		QVERIFY(!isStackWindow(noActivate));

		// WS_EX_APPWINDOW forces a taskbar button, so the window is an application.
		auto forced = application();
		forced.toolWindow = true;
		forced.noActivate = true;
		forced.appWindow = true;
		QVERIFY(isStackWindow(forced));
	}

	// The taskbar rule drops an owned popup. A dialog is a window the user may
	// capture and it is above its owner, so it must stay in the list.
	void listsOwnedDialogs() {
		auto dialog = application();
		dialog.owned = true;
		QVERIFY(isStackWindow(dialog));
	}

	void mapsFramesLikeTheWindowTracker() {
		// One monitor at physical 0,0 with scale 1.
		QCOMPARE(
		    logicalFrame(QRect(100, 200, 640, 480), QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1080), 1.0),
		    QRect(100, 200, 640, 480)
		);

		// A second monitor at physical 3840,0, scale 1.5. Qt places it at logical
		// 2560,0. A frame 150 physical pixels in is 100 logical pixels in.
		QCOMPARE(
		    logicalFrame(
		        QRect(3840 + 150, 300, 900, 600),
		        QRect(3840, 0, 3840, 2160),
		        QRect(2560, 0, 2560, 1440),
		        1.5
		    ),
		    QRect(2560 + 100, 200, 600, 400)
		);

		// A monitor left of the primary has a negative origin in both spaces.
		QCOMPARE(
		    logicalFrame(
		        QRect(-1920 + 200, 100, 800, 500),
		        QRect(-1920, 0, 1920, 1080),
		        QRect(-1920, 0, 1920, 1080),
		        1.0
		    ),
		    QRect(-1920 + 200, 100, 800, 500)
		);

		// A 125% monitor rounds each edge, not the size.
		QCOMPARE(
		    logicalFrame(QRect(10, 10, 101, 51), QRect(0, 0, 2400, 1350), QRect(0, 0, 1920, 1080), 1.25),
		    QRect(8, 8, 81, 41)
		);
	}

	void convertsToQml() {
		auto entries = QList<Entry> {
		    {.id = QStringLiteral("1a2b"), .title = QStringLiteral("Top"), .rect = QRect(10, 20, 300, 200)},
		    {.id = QStringLiteral("ff"), .title = QStringLiteral("Below"), .rect = QRect(-5, 0, 100, 50)},
		};

		auto list = toVariantList(entries);
		QCOMPARE(list.size(), 2);

		auto first = list.at(0).toMap();
		QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("1a2b"));
		QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Top"));
		QCOMPARE(first.value(QStringLiteral("rect")).toRectF(), QRectF(10, 20, 300, 200));
		QCOMPARE(first.size(), 3);

		auto second = list.at(1).toMap();
		QCOMPARE(second.value(QStringLiteral("id")).toString(), QStringLiteral("ff"));
		QCOMPARE(second.value(QStringLiteral("rect")).toRectF(), QRectF(-5, 0, 100, 50));

		QVERIFY(toVariantList({}).isEmpty());
	}

	void idIsLowercaseHex() {
		QCOMPARE(windowId(reinterpret_cast<HWND>(static_cast<quintptr>(0xABCDEF))), QStringLiteral("abcdef"));
		QCOMPARE(windowId(nullptr), QStringLiteral("0"));
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	WindowStackTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "window_stack_test.moc"
