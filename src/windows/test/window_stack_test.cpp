// The window stack snapshot's decisions, without a desktop: which observed
// windows are listed, how a physical frame maps to logical global coordinates,
// and what QML receives. window_stack_native_test creates real windows in a
// known order and checks the enumeration.
#include <algorithm>

#include <qguiapplication.h>
#include <qhash.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <qrect.h>
#include <qtest.h>
#include <qvariant.h>

#include "../screen_identity.hpp"
#include "../system_surface.hpp"
#include "../window_stack.hpp"

using namespace qs::windows::window_stack;
using qs::windows::DisplayIdentity;
using qs::windows::ScreenIdentityRegistry;
using qs::windows::system_surface::Identity;

namespace {

// A screen Qt reports under one fixed name, as it does for identical monitors
// and for a QScreen it reuses for another monitor.
class FakeScreen: public QPlatformScreen {
public:
	QRect rect;
	explicit FakeScreen(QRect rect): rect(rect) {}
	QRect geometry() const override { return this->rect; }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_RGB32; }
	QString name() const override { return QStringLiteral("Qt Name"); }
	qreal devicePixelRatio() const override { return 1.0; }
	QDpi logicalDpi() const override { return {96, 96}; }
};

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

	void dropsNonRootAndChildWindows() {
		auto owned = application();
		owned.root = false;
		QVERIFY(!isStackWindow(owned));

		auto child = application();
		child.child = true;
		QVERIFY(!isStackWindow(child));
	}

	// Not every application window has a native caption. Electron and UWP
	// windows with a custom frame, and many games, have an empty title.
	void listsWindowsWithoutATitle() {
		auto untitled = application();
		untitled.hasTitle = false;
		untitled.title.clear();
		QVERIFY(isStackWindow(untitled));

		// The other rules still apply to it.
		untitled.toolWindow = true;
		QVERIFY(!isStackWindow(untitled));

		auto shell = application();
		shell.hasTitle = false;
		shell.title.clear();
		shell.windowClass = QStringLiteral("Shell_TrayWnd");
		QVERIFY(!isStackWindow(shell));
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

	// A primary monitor at 150% (3840x2160 physical, 2560x1440 logical) and a
	// 100% monitor to its right (1920x1080 at physical 3840,0). Qt places the
	// second screen at logical 2560,0, so the two scales meet at one border.
	static QList<Monitor> mixedScaleMonitors() {
		return {
		    {.screen = QStringLiteral("DP-1"),
		     .physical = QRect(0, 0, 3840, 2160),
		     .logical = QRect(0, 0, 2560, 1440),
		     .devicePixelRatio = 1.5},
		    {.screen = QStringLiteral("DP-2"),
		     .physical = QRect(3840, 0, 1920, 1080),
		     .logical = QRect(2560, 0, 1920, 1080),
		     .devicePixelRatio = 1.0},
		};
	}

	void cutsAStraddlingWindowAtTheMonitorBorder() {
		// 240 physical pixels on the primary, 260 on the second monitor.
		auto pieces = logicalPieces(QRect(3600, 150, 500, 600), mixedScaleMonitors());

		QCOMPARE(pieces.size(), 2);
		QCOMPARE(pieces.at(0).screen, QStringLiteral("DP-1"));
		// x 3600..3840 / 1.5 = 2400..2560, y 150..750 / 1.5 = 100..500
		QCOMPARE(pieces.at(0).rect, QRect(2400, 100, 160, 400));
		QCOMPARE(pieces.at(1).screen, QStringLiteral("DP-2"));
		// The second monitor is 1:1 and starts at logical 2560.
		QCOMPARE(pieces.at(1).rect, QRect(2560, 150, 260, 600));

		// The picker's window on the primary hit-tests a cursor at physical
		// 3750,300, which is 2500,200 in Qt coordinates there. Each piece ends
		// exactly at the border of its screen in logical space, which one rect
		// mapped through a single monitor cannot do: through the second monitor
		// the frame would start at 2320 and run to 2820, over the border.
		QVERIFY(pieces.at(0).rect.contains(QPoint(2500, 200)));
		QVERIFY(!pieces.at(1).rect.contains(QPoint(2500, 200)));
		QCOMPARE(pieces.at(0).rect.right() + 1, pieces.at(1).rect.left());
	}

	void keepsAWindowOnOneMonitorInOnePiece() {
		auto pieces = logicalPieces(QRect(300, 150, 600, 300), mixedScaleMonitors());
		QCOMPARE(pieces.size(), 1);
		QCOMPARE(pieces.at(0).screen, QStringLiteral("DP-1"));
		QCOMPARE(pieces.at(0).rect, QRect(200, 100, 400, 200));

		pieces = logicalPieces(QRect(4000, 100, 400, 300), mixedScaleMonitors());
		QCOMPARE(pieces.size(), 1);
		QCOMPARE(pieces.at(0).screen, QStringLiteral("DP-2"));
		QCOMPARE(pieces.at(0).rect, QRect(2720, 100, 400, 300));
	}

	void dropsAWindowOnNoMonitor() {
		QVERIFY(logicalPieces(QRect(9000, 0, 100, 100), mixedScaleMonitors()).isEmpty());
		QVERIFY(logicalPieces(QRect(0, 0, 100, 100), {}).isEmpty());
		// A frame that only touches a border overlaps nothing.
		QVERIFY(logicalPieces(QRect(5760, 0, 100, 100), mixedScaleMonitors()).isEmpty());
	}

	// ShellScreen.name comes from the identity registry. A picker filtering its
	// own screen's pieces by that name must find them under the same string, also
	// when two monitors share a label and the registry suffixes one of them.
	void namesPiecesLikeShellScreenForIdenticalMonitors() {
		auto* firstPlatform = new FakeScreen(QRect(0, 0, 1920, 1080));
		auto* secondPlatform = new FakeScreen(QRect(1920, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"), QStringLiteral("generic-path-b"), QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"), QStringLiteral("generic-path-a"), QStringLiteral("Generic Monitor")},
		};

		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [first, second](QScreen* target) {
				    if (target == first) return QStringLiteral("DISPLAY1");
				    if (target == second) return QStringLiteral("DISPLAY2");
				    return QString();
			    }
		    );

			auto physical = QHash<QScreen*, QRect> {
			    {first, QRect(0, 0, 1920, 1080)},
			    {second, QRect(1920, 0, 1920, 1080)},
			};
			auto source = ScreenSource {
			    .name = [&](QScreen* screen) { return qs::windows::displayName(registry.identity(screen)); },
			    .physical = [&](QScreen* screen) { return physical.value(screen); },
			};

			auto monitors = monitorsFor({first, second}, source);
			QCOMPARE(monitors.size(), 2);

			// The registry orders the suffix by target path, not by screen order.
			QCOMPARE(monitors.at(0).screen, QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(monitors.at(1).screen, QStringLiteral("Generic Monitor"));
			// Qt's own name is the same for both and is not what QML sees.
			QCOMPARE(first->name(), second->name());
			QVERIFY(monitors.at(0).screen != first->name());

			// A window across both monitors gets one piece under each QML name.
			auto pieces = logicalPieces(QRect(1800, 100, 400, 300), monitors);
			QCOMPARE(pieces.size(), 2);
			QCOMPARE(pieces.at(0).screen, QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(pieces.at(0).rect, QRect(1800, 100, 120, 300));
			QCOMPARE(pieces.at(1).screen, QStringLiteral("Generic Monitor"));
			QCOMPARE(pieces.at(1).rect, QRect(1920, 100, 280, 300));

			// A reordered source list cannot rename either piece.
			std::reverse(displays.begin(), displays.end());
			registry.refresh();
			QCOMPARE(monitorsFor({first, second}, source).at(0).screen, QStringLiteral("Generic Monitor (2)"));
		}

		QWindowSystemInterface::handleScreenRemoved(secondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);
	}

	// Qt can keep a QScreen and its name while the monitor behind it is replaced.
	// QScreen::name() then names the old monitor, and the registry the new one.
	void namesPiecesAfterAMonitorReplacementBehindAReusedScreen() {
		auto* platform = new FakeScreen(QRect(0, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"), QStringLiteral("oled-path"), QStringLiteral("Contoso OLED 32")},
		};

		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [screen](QScreen* target) {
				    return target == screen ? QStringLiteral("DISPLAY1") : QString();
			    }
		    );
			auto source = ScreenSource {
			    .name = [&](QScreen* target) { return qs::windows::displayName(registry.identity(target)); },
			    .physical = [](QScreen*) { return QRect(0, 0, 1920, 1080); },
			};

			QCOMPARE(monitorsFor({screen}, source).first().screen, QStringLiteral("Contoso OLED 32"));

			// Same QScreen and GDI source, another physical monitor.
			displays[0] =
			    {QStringLiteral("DISPLAY1"), QStringLiteral("lcd-path"), QStringLiteral("Fabrikam 27Q")};
			registry.refresh();

			QCOMPARE(screen->name(), QStringLiteral("Qt Name"));
			auto monitors = monitorsFor({screen}, source);
			QCOMPARE(monitors.size(), 1);
			QCOMPARE(monitors.first().screen, QStringLiteral("Fabrikam 27Q"));
			QCOMPARE(
			    logicalPieces(QRect(100, 100, 200, 200), monitors).first().screen,
			    QStringLiteral("Fabrikam 27Q")
			);
		}

		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	// A screen with no known monitor rectangle is left out, not given a zero rect.
	void skipsAScreenWithoutAMonitorRectangle() {
		auto* platform = new FakeScreen(QRect(0, 0, 1920, 1080));
		QWindowSystemInterface::handleScreenAdded(platform);

		auto source = ScreenSource {
		    .name = [](QScreen*) { return QStringLiteral("X"); },
		    .physical = [](QScreen*) { return QRect(); },
		};
		QVERIFY(monitorsFor({platform->screen()}, source).isEmpty());

		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	void cutsAWindowAcrossAVerticalStack() {
		// A monitor above the primary, with a negative logical origin.
		auto monitors = QList<Monitor> {
		    {.screen = QStringLiteral("A"),
		     .physical = QRect(0, 0, 1920, 1080),
		     .logical = QRect(0, 0, 1920, 1080),
		     .devicePixelRatio = 1.0},
		    {.screen = QStringLiteral("B"),
		     .physical = QRect(0, -2160, 3840, 2160),
		     .logical = QRect(0, -1080, 1920, 1080),
		     .devicePixelRatio = 2.0},
		};

		auto pieces = logicalPieces(QRect(200, -400, 600, 800), monitors);
		QCOMPARE(pieces.size(), 2);
		QCOMPARE(pieces.at(0).rect, QRect(200, 0, 600, 400));
		// y -400..0 / 2 on a screen whose logical origin is -1080 and physical -2160
		QCOMPARE(pieces.at(1).rect, QRect(100, -200, 300, 200));
	}

	void convertsToQml() {
		auto entries = QList<Entry> {
		    {.id = QStringLiteral("1a2b"),
		     .title = QStringLiteral("Top"),
		     .pieces = {
		         {.screen = QStringLiteral("DP-1"), .rect = QRect(10, 20, 300, 200)},
		         {.screen = QStringLiteral("DP-2"), .rect = QRect(310, 20, 50, 200)},
		     }},
		    {.id = QStringLiteral("ff"),
		     .title = QString(),
		     .pieces = {{.screen = QStringLiteral("DP-2"), .rect = QRect(-5, 0, 100, 50)}}},
		};

		auto list = toVariantList(entries);
		QCOMPARE(list.size(), 2);

		auto first = list.at(0).toMap();
		QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("1a2b"));
		QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Top"));
		QCOMPARE(first.size(), 3);
		QVERIFY(!first.contains(QStringLiteral("rect")));

		auto rects = first.value(QStringLiteral("rects")).toList();
		QCOMPARE(rects.size(), 2);
		auto piece = rects.at(0).toMap();
		QCOMPARE(piece.value(QStringLiteral("screen")).toString(), QStringLiteral("DP-1"));
		QCOMPARE(piece.value(QStringLiteral("rect")).toRectF(), QRectF(10, 20, 300, 200));
		QCOMPARE(piece.size(), 2);
		QCOMPARE(rects.at(1).toMap().value(QStringLiteral("rect")).toRectF(), QRectF(310, 20, 50, 200));

		auto second = list.at(1).toMap();
		QCOMPARE(second.value(QStringLiteral("id")).toString(), QStringLiteral("ff"));
		QVERIFY(second.value(QStringLiteral("title")).toString().isEmpty());
		auto secondRects = second.value(QStringLiteral("rects")).toList();
		QCOMPARE(secondRects.size(), 1);
		QCOMPARE(secondRects.at(0).toMap().value(QStringLiteral("rect")).toRectF(), QRectF(-5, 0, 100, 50));

		QVERIFY(toVariantList({}).isEmpty());
	}

	void idIsLowercaseHex() {
		QCOMPARE(windowId(reinterpret_cast<HWND>(static_cast<quintptr>(0xABCDEF))), QStringLiteral("abcdef"));
		QCOMPARE(windowId(nullptr), QStringLiteral("0"));
	}
};

int main(int argc, char** argv) {
	QGuiApplication app(argc, argv);
	WindowStackTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "window_stack_test.moc"
