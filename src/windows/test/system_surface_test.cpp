#include <utility>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qrect.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtest.h>
#include <qvariant.h>

#include "../foreground_policy.hpp"
#include "../fullscreen_geometry.hpp"
#include "../system_surface.hpp"
#include "../window_event_filter.hpp"

using namespace qs::windows::system_surface;
using qs::windows::ForegroundAction;
using qs::windows::foregroundAction;
using qs::windows::isForegroundApplication;

namespace {

// Physical pixels: a 2560x1440 primary display with a taller 3840x2160
// display to its right, as the virtual desktop reports them.
const auto MONITOR_A = QRect(0, 0, 2560, 1440);
const auto MONITOR_B = QRect(2560, 0, 3840, 2160);

QStringList capturedMessages;

void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
	capturedMessages.append(message);
}

Identity app(QString windowClass, QString processName = QStringLiteral("chrome.exe")) {
	Identity identity;
	identity.windowClass = std::move(windowClass);
	identity.processName = std::move(processName);
	identity.title = QStringLiteral("window");
	identity.root = true;
	identity.visible = true;
	identity.hasTitle = true;
	return identity;
}

LRESULT CALLBACK testWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ensureClass(const QString& name) {
	static QSet<QString> registered;

	if (registered.contains(name)) return true;

	WNDCLASSW windowClass {};
	windowClass.lpfnWndProc = &testWindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = reinterpret_cast<LPCWSTR>(name.utf16());

	if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}

	registered.insert(name);
	return true;
}

HWND createWindow(
    const QString& windowClass,
    const QString& title = QStringLiteral("window"),
    DWORD style = WS_POPUP | WS_VISIBLE,
    DWORD exStyle = 0,
    HWND parent = nullptr
) {
	if (!ensureClass(windowClass)) return nullptr;

	return CreateWindowExW(
	    exStyle,
	    reinterpret_cast<LPCWSTR>(windowClass.utf16()),
	    reinterpret_cast<LPCWSTR>(title.utf16()),
	    style,
	    0,
	    0,
	    120,
	    80,
	    parent,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
}

} // namespace

class SystemSurfaceTest: public QObject {
	Q_OBJECT;

private slots:
	void shellClass_data() {
		QTest::addColumn<QString>("windowClass");

		QTest::newRow("task-switcher") << QStringLiteral("XamlExplorerHostIslandWindow");
		QTest::newRow("task-view") << QStringLiteral("MultitaskingViewFrame");
		QTest::newRow("task-switcher-win10") << QStringLiteral("TaskSwitcherWnd");
		QTest::newRow("task-switcher-overlay") << QStringLiteral("TaskSwitcherOverlayWnd");
		QTest::newRow("task-thumbnails") << QStringLiteral("TaskListThumbnailWnd");
		QTest::newRow("thumbnail-device") << QStringLiteral("ThumbnailDeviceHelperWnd");
		QTest::newRow("foreground-staging") << QStringLiteral("ForegroundStaging");
		QTest::newRow("desktop-shell") << QStringLiteral("ApplicationManager_DesktopShellWindow");
		QTest::newRow("launcher-tip") << QStringLiteral("LauncherTipWnd");
		QTest::newRow("tab-proxy") << QStringLiteral("Windows.Internal.Shell.TabProxyWindow");
		QTest::newRow("tray-overflow") << QStringLiteral("TopLevelWindowForOverflowXamlIsland");
		QTest::newRow("tray") << QStringLiteral("Shell_TrayWnd");
		QTest::newRow("tray-secondary") << QStringLiteral("Shell_SecondaryTrayWnd");
		QTest::newRow("input-switch") << QStringLiteral("Shell_InputSwitchTopLevelWindow");
		QTest::newRow("notification-overflow") << QStringLiteral("NotifyIconOverflowWindow");
		QTest::newRow("desktop") << QStringLiteral("Progman");
		QTest::newRow("wallpaper-host") << QStringLiteral("WorkerW");
		QTest::newRow("menu") << QStringLiteral("#32768");
		QTest::newRow("xaml-popup") << QStringLiteral("Xaml_WindowedPopupClass");
		QTest::newRow("control-host") << QStringLiteral("DV2ControlHost");
		QTest::newRow("dwm") << QStringLiteral("Dwm");
		QTest::newRow("dwm-window") << QStringLiteral("DwmWindow");
	}

	void cleanup() { setExtraShellSurfaceRules({}); }

	void shellClass() {
		QFETCH(QString, windowClass);
		QVERIFY(isShellClass(windowClass));
		QVERIFY(isShellClass(windowClass.toLower()));
		QVERIFY(isShellClass(windowClass.toUpper()));
	}

	void normalClassesAreNotShell() {
		QVERIFY(!isShellClass(QStringLiteral("CabinetWClass")));
		QVERIFY(!isShellClass(QStringLiteral("ExploreWClass")));
		QVERIFY(!isShellClass(QStringLiteral("Chrome_WidgetWin_1")));
		QVERIFY(!isShellClass(QStringLiteral("ApplicationFrameWindow")));
		// Shared by shell islands and ordinary UWP/DirectX applications.
		QVERIFY(!isShellClass(QStringLiteral("Windows.UI.Core.CoreWindow")));
		// A dialog class is shared by every application, not a shell surface.
		QVERIFY(!isShellClass(QStringLiteral("#32770")));
		// Used by XAML input surfaces in ordinary applications.
		QVERIFY(!isShellClass(QStringLiteral("Windows.UI.Input.InputSite.WindowClass")));
	}

	void cloakFlagsAreClassified() {
		QVERIFY(cloakFromFlags(0) == Cloak::None);
		QVERIFY(cloakFromFlags(0x1) == Cloak::App);   // DWM_CLOAKED_APP
		QVERIFY(cloakFromFlags(0x4) == Cloak::App);   // DWM_CLOAKED_INHERITED
		QVERIFY(cloakFromFlags(0x5) == Cloak::App);   // APP | INHERITED
		QVERIFY(cloakFromFlags(0x2) == Cloak::Shell); // DWM_CLOAKED_SHELL
		QVERIFY(cloakFromFlags(0x3) == Cloak::App);   // SHELL | APP: app hidden wins
		QVERIFY(cloakFromFlags(0x6) == Cloak::Shell); // SHELL | INHERITED
		QVERIFY(cloakFromFlags(0x7) == Cloak::App);   // all bits: app hidden wins
	}

	void coreWindowIsSystemOnlyWithAShellHost() {
		const auto systemHosts = {
		    QStringLiteral("explorer.exe"),
		    QStringLiteral("StartMenuExperienceHost.exe"),
		    QStringLiteral("SearchHost.exe"),
		    QStringLiteral("ShellExperienceHost.exe"),
		    QStringLiteral("TextInputHost.exe"),
		    QStringLiteral("sihost.exe"),
		    QStringLiteral("LockApp.exe"),
		};

		for (const auto& process: systemHosts) {
			auto identity = app(QStringLiteral("Windows.UI.Core.CoreWindow"), process);
			QVERIFY2(isShellSurface(identity), qPrintable(process));
			QVERIFY2(!isApplicationWindow(identity), qPrintable(process));
		}

		const auto applicationHosts = {
		    QStringLiteral("SystemSettings.exe"),
		    QStringLiteral("ApplicationFrameHost.exe"),
		    QStringLiteral("explorer_test.exe"),
		};

		for (const auto& process: applicationHosts) {
			auto identity = app(QStringLiteral("Windows.UI.Core.CoreWindow"), process);
			QVERIFY2(!isShellSurface(identity), qPrintable(process));
			QVERIFY2(isApplicationWindow(identity), qPrintable(process));
		}
	}

	void altTabHostIsNeverAnApplication() {
		// The Windows 11 switcher: explorer.exe,
		// XamlExplorerHostIslandWindow, title "Task Switching",
		// WS_POPUP | WS_VISIBLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
		// WS_EX_NOREDIRECTIONBITMAP.
		auto identity =
		    app(QStringLiteral("XamlExplorerHostIslandWindow"), QStringLiteral("explorer.exe"));
		identity.toolWindow = true;

		QVERIFY(isShellSurface(identity));
		QVERIFY(!isApplicationIdentity(identity));
		QVERIFY(!isApplicationWindow(identity));
	}

	void stagingAndTaskViewAreNeverApplications() {
		auto staging = app(QStringLiteral("ForegroundStaging"), QStringLiteral("explorer.exe"));
		staging.visible = false;
		staging.hasTitle = false;
		staging.toolWindow = true;
		QVERIFY(!isApplicationWindow(staging));

		auto taskView = app(QStringLiteral("MultitaskingViewFrame"), QStringLiteral("explorer.exe"));
		QVERIFY(!isApplicationWindow(taskView));

		auto desktop = app(QStringLiteral("Progman"), QStringLiteral("explorer.exe"));
		QVERIFY(!isApplicationWindow(desktop));
		QVERIFY(isDesktopSurface(desktop));
		QVERIFY(!isDesktopSurface(taskView));
	}

	void taskSwitchingWindowOnThePrimaryMonitorIsRejected() {
		// The installed tracker accepted this identity: explorer.exe, a visible
		// titled "Task Switching" window, not a tool window, covering the
		// monitor where Windows places the switcher. It became a Hyprland
		// toplevel with fullscreen=2 on DP-1 only, so only DP-1's QML
		// ContentWindow ran its hide animation.
		auto taskSwitching =
		    app(QStringLiteral("MultitaskingViewFrame"), QStringLiteral("explorer.exe"));
		QVERIFY(isShellSurface(taskSwitching));
		QVERIFY(!isApplicationIdentity(taskSwitching));
		QVERIFY(!isApplicationWindow(taskSwitching));

		auto overlay =
		    app(QStringLiteral("TaskSwitcherOverlayWnd"), QStringLiteral("explorer.exe"));
		QVERIFY(isShellSurface(overlay));
		QVERIFY(!isApplicationIdentity(overlay));
		QVERIFY(!isApplicationWindow(overlay));
	}

	void shellHostProcessesAreNeverApplications() {
		const auto processes = {
		    QStringLiteral("ShellExperienceHost.exe"),
		    QStringLiteral("StartMenuExperienceHost.exe"),
		    QStringLiteral("SearchHost.exe"),
		    QStringLiteral("SearchApp.exe"),
		    QStringLiteral("SearchUI.exe"),
		    QStringLiteral("ShellHost.exe"),
		    QStringLiteral("TextInputHost.exe"),
		    QStringLiteral("sihost.exe"),
		    QStringLiteral("LockApp.exe"),
		};

		for (const auto& process: processes) {
			auto identity = app(QStringLiteral("ApplicationFrameWindow"), process);
			QVERIFY2(isShellHostProcess(process), qPrintable(process));
			QVERIFY2(isShellSurface(identity), qPrintable(process));
			QVERIFY2(!isApplicationWindow(identity), qPrintable(process));
		}
	}

	void taskListEligibilityFollowsTaskbarRules() {
		auto normal = app(QStringLiteral("Chrome_WidgetWin_1"));
		QVERIFY(isTaskListWindow(normal));

		auto tool = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("mpv.exe"));
		tool.toolWindow = true;
		QVERIFY(!isTaskListWindow(tool));

		// WS_EX_APPWINDOW forces a button even on a tool or owned window.
		tool.appWindow = true;
		QVERIFY(isTaskListWindow(tool));

		auto noActivate = app(QStringLiteral("Chrome_WidgetWin_1"));
		noActivate.noActivate = true;
		QVERIFY(!isTaskListWindow(noActivate));

		auto owned = app(QStringLiteral("#32770"));
		owned.owned = true;
		QVERIFY(!isTaskListWindow(owned));
	}

	void shellLikeOverlaysAreNotTaskListWindows() {
		// A third-party shell's task switcher can be an ordinary titled,
		// monitor-sized toplevel. It marks itself as a non-activating overlay
		// with WS_EX_NOACTIVATE instead of WS_EX_TOOLWINDOW. That is taskbar
		// ineligibility, not positive shell identity.
		auto switcher = app(QStringLiteral("Tauri Window"), QStringLiteral("third-party-shell.exe"));
		switcher.title = QStringLiteral("Task Switcher");
		switcher.topmost = true;
		switcher.noActivate = true;
		QVERIFY(!isTaskListWindow(switcher));
		QVERIFY(!isShellSurface(switcher));
		QVERIFY(!isApplicationIdentity(switcher));
		QVERIFY(!isApplicationWindow(switcher));

		// Bars, taskbars and tray popups use a tool window.
		auto tray = app(QStringLiteral("Tauri Window"), QStringLiteral("third-party-shell.exe"));
		tray.toolWindow = true;
		tray.topmost = true;
		QVERIFY(!isTaskListWindow(tray));
		QVERIFY(!isShellSurface(tray));
		QVERIFY(!isApplicationWindow(tray));

		// Desktop widgets and wallpaper hosts refuse activation even when they
		// are not topmost.
		auto widget = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("widget.exe"));
		widget.noActivate = true;
		QVERIFY(!isTaskListWindow(widget));
		QVERIFY(!isApplicationWindow(widget));

		// A topmost owned flyout has no taskbar button of its own.
		auto flyout = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("app.exe"));
		flyout.topmost = true;
		flyout.owned = true;
		QVERIFY(!isTaskListWindow(flyout));
		QVERIFY(!isApplicationWindow(flyout));
	}

	void mpvToolWindowFullscreenStillSeedsAppBar() {
		// mpv with "show in taskbar" off is WS_EX_TOOLWINDOW. It is not a
		// task-list window, but it is still a fullscreen application that must
		// hide the panel.
		auto mpv = app(QStringLiteral("mpv"), QStringLiteral("mpv.exe"));
		mpv.title = QStringLiteral("movie.mkv");
		mpv.toolWindow = true;
		QVERIFY(!isTaskListWindow(mpv));
		QVERIFY(!isShellSurface(mpv));
		QVERIFY(isFullscreenOccluder(mpv, MONITOR_A, MONITOR_A));

		// Positive shell identity still never seeds fullscreen.
		auto shell = app(QStringLiteral("XamlExplorerHostIslandWindow"), QStringLiteral("explorer.exe"));
		shell.toolWindow = true;
		QVERIFY(isShellSurface(shell));
		QVERIFY(!isFullscreenOccluder(shell, MONITOR_A, MONITOR_A));

		// Invisible, cloaked, child and non-covering windows are not occluders.
		auto hidden = mpv;
		hidden.visible = false;
		QVERIFY(!isFullscreenOccluder(hidden, MONITOR_A, MONITOR_A));

		auto cloaked = mpv;
		cloaked.cloaked = true;
		QVERIFY(!isFullscreenOccluder(cloaked, MONITOR_A, MONITOR_A));

		auto child = mpv;
		child.child = true;
		child.root = false;
		QVERIFY(!isFullscreenOccluder(child, MONITOR_A, MONITOR_A));

		QVERIFY(!isFullscreenOccluder(mpv, QRect(0, 0, 100, 100), MONITOR_A));
	}

	void fullscreenOcclusionIsPerMonitor() {
		// A third-party Alt+Tab switcher can be a topmost, taskbar-eligible
		// window whose rectangle is the whole virtual desktop. It covers the
		// monitor it is nearest to, but it is not fullscreen on one monitor.
		auto switcher = app(QStringLiteral("Tauri Window"), QStringLiteral("switcher.exe"));
		switcher.title = QStringLiteral("Task Switcher");
		switcher.topmost = true;
		switcher.appWindow = true;

		const auto desktop = MONITOR_A.united(MONITOR_B);
		QVERIFY(!isFullscreenOccluder(switcher, desktop, MONITOR_A));
		QVERIFY(!isFullscreenOccluder(switcher, desktop, MONITOR_B));

		// A fullscreen application on either monitor still occludes there.
		QVERIFY(isFullscreenOccluder(switcher, MONITOR_A, MONITOR_A));
		QVERIFY(isFullscreenOccluder(switcher, MONITOR_B, MONITOR_B));

		// A border or shadow may overhang the monitor edge by a few pixels,
		// including onto the neighbouring monitor.
		QVERIFY(isFullscreenOccluder(switcher, MONITOR_A.adjusted(-4, -4, 6, 4), MONITOR_A));
		QVERIFY(isFullscreenOccluder(switcher, MONITOR_B.adjusted(-6, -4, 4, 4), MONITOR_B));

		// Extending meaningfully onto the neighbour is not per-monitor
		// fullscreen, even though the window still covers this monitor.
		auto intoB = MONITOR_A;
		intoB.setWidth(MONITOR_A.width() + 64);
		QVERIFY(!isFullscreenOccluder(switcher, intoB, MONITOR_A));

		// The tracker's fullscreen episode uses the same rule, so a tracked
		// spanning window cannot hide the panel through its toplevel either.
		const auto none = qs::windows::fullscreen_geometry::Latch();
		QVERIFY(!qs::windows::fullscreen_geometry::detect(false, false, desktop, MONITOR_A, 1, none)
		             .fullscreen);
		QVERIFY(qs::windows::fullscreen_geometry::detect(false, false, MONITOR_A, MONITOR_A, 1, none)
		            .fullscreen);
		QVERIFY(qs::windows::fullscreen_geometry::detect(false, false, MONITOR_B, MONITOR_B, 2, none)
		            .fullscreen);
		QVERIFY(qs::windows::fullscreen_geometry::detect(
		            false, false, MONITOR_A.adjusted(-4, -4, 6, 4), MONITOR_A, 1, none
		        )
		            .fullscreen);
	}

	void activatableTopmostApplicationsStayApplications() {
		// "Always on top" is not a shell property on its own: an ordinary app
		// can stay topmost, keep focus and keep its taskbar button.
		auto player = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("player.exe"));
		player.topmost = true;
		QVERIFY(!isShellSurface(player));
		QVERIFY(isApplicationWindow(player));

		// WS_EX_APPWINDOW opts a tool window back into the application list.
		auto chooser = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("chooser.exe"));
		chooser.toolWindow = true;
		chooser.appWindow = true;
		QVERIFY(isTaskListWindow(chooser));
		QVERIFY(!isShellSurface(chooser));
		QVERIFY(isApplicationWindow(chooser));
	}

	void ownedWindowsWithoutOptInAreNotApplications() {
		auto dialog = app(QStringLiteral("#32770"), QStringLiteral("editor.exe"));
		dialog.owned = true;
		QVERIFY(!isTaskListWindow(dialog));
		QVERIFY(!isApplicationIdentity(dialog));
		QVERIFY(!isApplicationWindow(dialog));

		dialog.appWindow = true;
		QVERIFY(isTaskListWindow(dialog));
		QVERIFY(isApplicationWindow(dialog));
	}

	void extraSelectorsClassifyThirdPartyShellWindows() {
		auto shell = app(QStringLiteral("CustomShellWindow"), QStringLiteral("custom-shell.exe"));
		shell.title = QStringLiteral("Custom Switcher");
		QVERIFY(!isShellSurface(shell));

		setExtraShellSurfaceRules({QStringLiteral("exe:custom-shell.exe")});
		QVERIFY(isShellSurface(shell));
		QVERIFY(!isApplicationWindow(shell));

		setExtraShellSurfaceRules({QStringLiteral("title:Custom *")});
		QVERIFY(isShellSurface(shell));
		QVERIFY(extraShellSurfaceRulesUseTitles());

		setExtraShellSurfaceRules({QStringLiteral("class:CustomShellWindow")});
		QVERIFY(isShellSurface(shell));

		setExtraShellSurfaceRules({QStringLiteral("custom-shell")});
		QVERIFY(isShellSurface(shell));

		auto ordinary = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("chrome.exe"));
		QVERIFY(!isShellSurface(ordinary));

		setExtraShellSurfaceRules({});
		QVERIFY(!isShellSurface(shell));
	}

	void screenSelectorsAreRejectedForShellSurfaces() {
		setExtraShellSurfaceRules({QStringLiteral("screen:*")});

		// Without the rejection, an empty screen name makes this match every
		// window.
		QVERIFY(!isShellSurface(app(QStringLiteral("Chrome_WidgetWin_1"))));
		QVERIFY(!isShellSurface(app(QStringLiteral("CustomShellWindow"))));
		QVERIFY(!isShellSurface(
		    app(QStringLiteral("Windows.UI.Core.CoreWindow"), QStringLiteral("Settings.exe"))
		));

		capturedMessages.clear();
		auto previous = qInstallMessageHandler(&captureMessage);
		setExtraShellSurfaceRules({QStringLiteral("screen:*")});
		qInstallMessageHandler(previous);

		const auto log = capturedMessages.join(QLatin1Char('\n'));
		QVERIFY(log.contains(QStringLiteral("screen selectors are not supported")));
		QVERIFY(!log.contains(QStringLiteral("screen:*")));
	}

	void malformedSelectorsAreIgnoredWithoutLoggingText() {
		capturedMessages.clear();
		auto previous = qInstallMessageHandler(&captureMessage);
		setExtraShellSurfaceRules({
		    QStringLiteral("title:["),
		    QStringLiteral("exe:custom-shell.exe"),
		});
		qInstallMessageHandler(previous);

		// The valid selector still works; the invalid one matches nothing and
		// never reaches QRegularExpression::match.
		QVERIFY(isShellSurface(
		    app(QStringLiteral("CustomShellWindow"), QStringLiteral("custom-shell.exe"))
		));
		QVERIFY(!isShellSurface(
		    app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("chrome.exe"))
		));
		QVERIFY(!extraShellSurfaceRulesUseTitles());

		const auto log = capturedMessages.join(QLatin1Char('\n'));
		QVERIFY(log.contains(QStringLiteral("invalid pattern")));
		QVERIFY(log.contains(QStringLiteral("selector 1")));
		QVERIFY(!log.contains(QStringLiteral("title:[")));
		QVERIFY(!log.contains(QLatin1Char('[')));
	}

	void titleChangeTogglesShellIdentity() {
		setExtraShellSurfaceRules({QStringLiteral("title:Task Switcher")});
		QVERIFY(extraShellSurfaceRulesUseTitles());

		auto window = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("chrome.exe"));
		window.title = QStringLiteral("Normal document");
		QVERIFY(!isShellSurface(window));
		QVERIFY(isApplicationWindow(window));

		window.title = QStringLiteral("Task Switcher");
		QVERIFY(isShellSurface(window));
		QVERIFY(!isApplicationWindow(window));

		// The tracker asks only this question when a tracked window's title
		// changes. A title that leaves the rule stops matching; the window then
		// returns through the untracked candidate path.
		QVERIFY(titleChangeMakesShellSurface(QStringLiteral("Task Switcher")));
		QVERIFY(!titleChangeMakesShellSurface(QStringLiteral("Normal document")));
	}

	// An unrelated window must keep its tracked state through a transient empty
	// title and through a style toggle. The tracker's per-window name-change
	// decision is exactly windowTitle() followed by titleChangeMakesShellSurface();
	// neither reads styles, and neither matches an empty title, so nothing here
	// can request the removal that deletes the tiler's slot and history.
	void unrelatedWindowSurvivesEmptyTitleAndStyleToggle() {
		setExtraShellSurfaceRules({QStringLiteral("title:Task Switcher"), QStringLiteral("title:*")});

		WNDCLASSW windowClass {};
		windowClass.lpfnWndProc = DefWindowProcW;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = L"QsSurfaceTitleTest";
		RegisterClassW(&windowClass);

		auto* hwnd = CreateWindowExW(
		    0, windowClass.lpszClassName, L"Quarterly report", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100,
		    nullptr, nullptr, windowClass.hInstance, nullptr
		);
		QVERIFY(hwnd != nullptr);

		auto removes = [&]() {
			return extraShellSurfaceRulesUseTitles()
			    && titleChangeMakesShellSurface(windowTitle(hwnd));
		};

		QVERIFY(extraShellSurfaceRulesUseTitles());
		QVERIFY(!removes());

		// An empty title for a moment (the app is mid state change).
		SetWindowTextW(hwnd, L"");
		QVERIFY(windowTitle(hwnd).isEmpty());
		QVERIFY(!removes());

		// Task-list-ineligible styles during fullscreen or maximise.
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST);
		SetWindowTextW(hwnd, L"");
		QVERIFY(!removes());
		SetWindowTextW(hwnd, L"Quarterly report");
		QVERIFY(!removes());

		// Only a title that newly matches a rule removes it.
		SetWindowTextW(hwnd, L"Task Switcher");
		QVERIFY(removes());
		SetWindowTextW(hwnd, L"Quarterly report");
		QVERIFY(!removes());

		DestroyWindow(hwnd);
		UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
	}

	void titleRuleMatchingEmptyTitleIsRejectedForShellSurfaces() {
		setExtraShellSurfaceRules({QStringLiteral("title:*")});
		QVERIFY(!extraShellSurfaceRulesUseTitles());

		auto window = app(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("chrome.exe"));
		window.title = QString();
		QVERIFY(!isShellSurface(window));
	}

	void configNormalizationIgnoresNonStrings() {
		auto list = QVariantList {
		    QVariant(QStringLiteral("exe:custom-shell.exe")),
		    QVariant(3),
		    QVariant(QVariantMap {{QStringLiteral("class"), QStringLiteral("Ignored")}}),
		    QVariant(QStringLiteral("class:CustomShellWindow")),
		};
		auto config = shellSurfaceRulesFromConfig(list);
		QCOMPARE(config.rules.size(), qsizetype(2));
		QCOMPARE(config.rules.at(0), QStringLiteral("exe:custom-shell.exe"));
		QCOMPARE(config.rules.at(1), QStringLiteral("class:CustomShellWindow"));
		QCOMPARE(config.ignored, qsizetype(2));

		auto text = shellSurfaceRulesFromConfig(QVariant(QStringLiteral(
		    "exe:custom-shell.exe; title:Custom Switcher ; "
		)));
		QCOMPARE(text.rules.size(), qsizetype(2));
		QCOMPARE(text.ignored, qsizetype(0));

		auto stringList = shellSurfaceRulesFromConfig(QVariant(QStringList {
		    QStringLiteral("exe:list.exe"),
		}));
		QCOMPARE(stringList.rules.size(), qsizetype(1));
		QCOMPARE(stringList.ignored, qsizetype(0));

		auto scalar = shellSurfaceRulesFromConfig(QVariant(42));
		QVERIFY(scalar.rules.isEmpty());
		QCOMPARE(scalar.ignored, qsizetype(1));
	}

	void cloakedAndToolWindowsAreNeverFullscreenApplications() {
		auto cloaked = app(QStringLiteral("Chrome_WidgetWin_1"));
		cloaked.cloaked = true;
		QVERIFY(isApplicationIdentity(cloaked));
		QVERIFY(!isApplicationWindow(cloaked));

		auto tool = app(QStringLiteral("Chrome_WidgetWin_1"));
		tool.toolWindow = true;
		QVERIFY(!isApplicationWindow(tool));

		// WS_EX_APPWINDOW opts a tool window into the task switcher.
		tool.appWindow = true;
		QVERIFY(isApplicationWindow(tool));

		auto child = app(QStringLiteral("Chrome_WidgetWin_1"));
		child.child = true;
		child.root = false;
		QVERIFY(!isApplicationWindow(child));

		auto untitled = app(QStringLiteral("Chrome_WidgetWin_1"));
		untitled.hasTitle = false;
		QVERIFY(!isApplicationWindow(untitled));

		auto hidden = app(QStringLiteral("Chrome_WidgetWin_1"));
		hidden.visible = false;
		QVERIFY(!isApplicationWindow(hidden));
	}

	void realApplicationsStayApplications() {
		QVERIFY(isApplicationWindow(app(QStringLiteral("Chrome_WidgetWin_1"))));
		QVERIFY(isApplicationWindow(app(QStringLiteral("Dia"))));
		QVERIFY(isApplicationWindow(
		    app(QStringLiteral("ApplicationFrameWindow"), QStringLiteral("ApplicationFrameHost.exe"))
		));

		// File Explorer folder windows run in explorer.exe but are applications.
		QVERIFY(
		    isApplicationWindow(app(QStringLiteral("CabinetWClass"), QStringLiteral("explorer.exe")))
		);
		QVERIFY(
		    isApplicationWindow(app(QStringLiteral("ExploreWClass"), QStringLiteral("explorer.exe")))
		);
	}

	void observedShellClassIsRejected() {
		auto* window =
		    createWindow(QStringLiteral("MultitaskingViewFrame"), QStringLiteral("Task View"));
		QVERIFY(window != nullptr);

		auto identity = observe(window);
		QCOMPARE(identity.windowClass, QStringLiteral("MultitaskingViewFrame"));
		QVERIFY(identity.root);
		QVERIFY(identity.visible);
		QVERIFY(identity.hasTitle);
		QVERIFY(isShellSurface(identity));
		QVERIFY(!isApplicationWindow(identity));
		QVERIFY(!isCloaked(window));

		DestroyWindow(window);
	}

	void observedApplicationIsAccepted() {
		auto* window = createWindow(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("document"));
		QVERIFY(window != nullptr);

		auto identity = observe(window);
		QCOMPARE(identity.windowClass, QStringLiteral("Chrome_WidgetWin_1"));
		QVERIFY(identity.root);
		QVERIFY(identity.visible);
		QVERIFY(identity.hasTitle);
		QVERIFY(!identity.toolWindow);
		QVERIFY(!isShellSurface(identity));
		QVERIFY(isApplicationWindow(identity));
		QVERIFY(identity.processName.endsWith(QStringLiteral(".exe")));

		DestroyWindow(window);
	}

	void observedOrdinaryCoreWindowIsAccepted() {
		// The test process is not a shell host, so this is the ordinary UWP or
		// DirectX CoreWindow case even though the class name is shared.
		auto* window =
		    createWindow(QStringLiteral("Windows.UI.Core.CoreWindow"), QStringLiteral("Settings"));
		QVERIFY(window != nullptr);

		auto identity = observe(window);
		QCOMPARE(identity.windowClass, QStringLiteral("Windows.UI.Core.CoreWindow"));
		QVERIFY(!isShellSurface(identity));
		QVERIFY(isApplicationWindow(identity));

		DestroyWindow(window);
	}

	void observedNonActivatingOverlayIsRejected() {
		// The exact style combination a third-party shell uses for its task
		// switcher or bar: topmost, no activation, no tool-window opt-out.
		auto* window = createWindow(
		    QStringLiteral("Chrome_WidgetWin_1"),
		    QStringLiteral("Task Switcher"),
		    WS_POPUP | WS_VISIBLE,
		    WS_EX_TOPMOST | WS_EX_NOACTIVATE
		);
		QVERIFY(window != nullptr);

		auto identity = observe(window);
		QVERIFY(identity.topmost);
		QVERIFY(identity.noActivate);
		QCOMPARE(identity.title, QStringLiteral("Task Switcher"));
		QVERIFY(!isTaskListWindow(identity));
		QVERIFY(!isShellSurface(identity));
		QVERIFY(!isApplicationWindow(identity));

		DestroyWindow(window);
	}

	void observedOwnedPopupIsNotAnApplication() {
		auto* owner = createWindow(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("owner"));
		QVERIFY(owner != nullptr);
		auto* popup = createWindow(
		    QStringLiteral("Chrome_WidgetWin_1"),
		    QStringLiteral("popup"),
		    WS_POPUP | WS_VISIBLE,
		    WS_EX_TOPMOST,
		    owner
		);
		QVERIFY(popup != nullptr);

		auto identity = observe(popup);
		QVERIFY(identity.owned);
		QVERIFY(!isTaskListWindow(identity));
		QVERIFY(!isShellSurface(identity));
		QVERIFY(!isApplicationIdentity(identity));

		DestroyWindow(popup);
		DestroyWindow(owner);
	}

	void observedToolWindowFullscreenIsAnOccluder() {
		auto* window = createWindow(
		    QStringLiteral("mpv"),
		    QStringLiteral("movie.mkv"),
		    WS_POPUP | WS_VISIBLE,
		    WS_EX_TOOLWINDOW
		);
		QVERIFY(window != nullptr);

		auto identity = observe(window);
		QVERIFY(identity.toolWindow);
		QVERIFY(!isTaskListWindow(identity));
		QVERIFY(!isShellSurface(identity));
		QVERIFY(isFullscreenOccluder(identity, MONITOR_A, MONITOR_A));

		DestroyWindow(window);
	}

	void foregroundActionFollowsTheWindowClassAndFocusOwner() {
		QVERIFY(foregroundAction(app(QStringLiteral("Chrome_WidgetWin_1")), false)
		        == ForegroundAction::Adopt);
		// Windows shell overlays keep the application the user was using.
		QVERIFY(foregroundAction(app(QStringLiteral("TaskSwitcherWnd")), false)
		        == ForegroundAction::Keep);
		QVERIFY(foregroundAction(app(QStringLiteral("XamlExplorerHostIslandWindow")), false)
		        == ForegroundAction::Keep);
		// The desktop means nothing is focused.
		QVERIFY(foregroundAction(app(QStringLiteral("Progman")), false) == ForegroundAction::Clear);
		QVERIFY(foregroundAction(app(QStringLiteral("WorkerW")), false) == ForegroundAction::Clear);
		// The shell's own panels keep the last application.
		QVERIFY(foregroundAction(app(QStringLiteral("Chrome_WidgetWin_1")), true)
		        == ForegroundAction::Keep);

		auto tool = app(QStringLiteral("Chrome_WidgetWin_1"));
		tool.toolWindow = true;
		QVERIFY(foregroundAction(tool, false) == ForegroundAction::Clear);

		// A modal dialog gets no taskbar button but is still what the user is
		// using. The foreground observer follows it; the tracker keeps its
		// stricter taskbar eligibility for tiling.
		auto dialog = app(QStringLiteral("#32770"));
		dialog.owned = true;
		QVERIFY(!isApplicationWindow(dialog));
		QVERIFY(isForegroundApplication(dialog));
		QVERIFY(foregroundAction(dialog, false) == ForegroundAction::Adopt);

		auto cloaked = app(QStringLiteral("Chrome_WidgetWin_1"));
		cloaked.cloaked = true;
		QVERIFY(foregroundAction(cloaked, false) == ForegroundAction::Clear);

		auto child = app(QStringLiteral("Chrome_WidgetWin_1"));
		child.root = false;
		child.child = true;
		QVERIFY(foregroundAction(child, false) == ForegroundAction::Clear);
	}

	void observedStylesAreClassified() {
		auto* tool = createWindow(
		    QStringLiteral("Chrome_WidgetWin_1"),
		    QStringLiteral("tool"),
		    WS_POPUP | WS_VISIBLE,
		    WS_EX_TOOLWINDOW
		);
		QVERIFY(tool != nullptr);
		QVERIFY(!isApplicationWindow(observe(tool)));

		auto* untitled = createWindow(QStringLiteral("Chrome_WidgetWin_1"), QString());
		QVERIFY(untitled != nullptr);
		QVERIFY(!isApplicationWindow(observe(untitled)));

		auto* parent = createWindow(QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("parent"));
		QVERIFY(parent != nullptr);
		auto* child = createWindow(
		    QStringLiteral("Chrome_WidgetWin_1"),
		    QStringLiteral("child"),
		    WS_CHILD | WS_VISIBLE,
		    0,
		    parent
		);
		QVERIFY(child != nullptr);
		auto childIdentity = observe(child);
		QVERIFY(childIdentity.child);
		QVERIFY(!childIdentity.root);
		QVERIFY(!isApplicationWindow(childIdentity));

		DestroyWindow(child);
		DestroyWindow(parent);
		DestroyWindow(untitled);
		DestroyWindow(tool);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	SystemSurfaceTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "system_surface_test.moc"
