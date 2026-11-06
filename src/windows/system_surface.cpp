#include "system_surface.hpp"
#include <iterator>
#include <string>

#include <dwmapi.h>
#include <qfileinfo.h>
#include <qreadwritelock.h>
#include <qset.h>

#include "fullscreen_geometry.hpp"
#include "window_selectors.hpp"

namespace qs::windows::system_surface {

namespace {

// Top-level classes created by the Windows shell while it switches windows,
// shows Task View, opens Start/Search and other flyouts, stages a foreground
// change, draws menus or owns the desktop. None of them is an application.
bool shellClassSet(const QString& windowClass) {
	static const auto classes = QSet<QString> {
	    // Desktop and taskbar
	    QStringLiteral("progman"),
	    QStringLiteral("workerw"),
	    QStringLiteral("shell_traywnd"),
	    QStringLiteral("shell_secondarytraywnd"),
	    QStringLiteral("notifyiconoverflowwindow"),
	    QStringLiteral("toplevelwindowforoverflowxamlisland"),
	    QStringLiteral("shell_inputswitchtoplevelwindow"),
	    // Task switcher, Task View, snap and thumbnail surfaces
	    QStringLiteral("xamlexplorerhostislandwindow"),
	    QStringLiteral("multitaskingviewframe"),
	    QStringLiteral("taskswitcherwnd"),
	    QStringLiteral("taskswitcheroverlaywnd"),
	    QStringLiteral("tasklistthumbnailwnd"),
	    QStringLiteral("thumbnaildevicehelperwnd"),
	    QStringLiteral("foregroundstaging"),
	    QStringLiteral("applicationmanager_desktopshellwindow"),
	    QStringLiteral("launchertipwnd"),
	    // XAML islands, menus and shell popups
	    QStringLiteral("windows.internal.shell.tabproxywindow"),
	    QStringLiteral("xaml_windowedpopupclass"),
	    QStringLiteral("dv2controlhost"),
	    QStringLiteral("#32768"),
	    // Desktop Window Manager helper windows
	    QStringLiteral("dwm"),
	    QStringLiteral("dwmwindow"),
	};

	return classes.contains(windowClass.toLower());
}

// Windows' own shell host executables. These are OS components, not third-party
// applications, so naming them is part of recognising the shell UI.
bool shellHostProcessSet(const QString& processName) {
	static const auto processes = QSet<QString> {
	    QStringLiteral("shellexperiencehost.exe"),
	    QStringLiteral("startmenuexperiencehost.exe"),
	    QStringLiteral("searchhost.exe"),
	    QStringLiteral("searchapp.exe"),
	    QStringLiteral("searchui.exe"),
	    QStringLiteral("shellhost.exe"),
	    QStringLiteral("textinputhost.exe"),
	    QStringLiteral("sihost.exe"),
	    QStringLiteral("lockapp.exe"),
	    QStringLiteral("dwm.exe"),
	};

	return processes.contains(processName.toLower());
}

// Windows.UI.Core.CoreWindow is shared by shell XAML islands and ordinary UWP
// or DirectX applications. Only the shell hosts make it a shell surface.
bool coreWindow(const QString& windowClass) {
	return windowClass.compare(QStringLiteral("windows.ui.core.corewindow"), Qt::CaseInsensitive)
	    == 0;
}

QReadWriteLock extraRulesLock;
WindowSelectors extraRules;
QStringList extraRuleList;

} // namespace

bool isShellClass(const QString& windowClass) { return shellClassSet(windowClass); }

bool isShellHostProcess(const QString& processName) { return shellHostProcessSet(processName); }

ShellSurfaceRuleConfig shellSurfaceRulesFromConfig(const QVariant& value) {
	auto config = ShellSurfaceRuleConfig();
	auto add = [&config](const QVariant& entry) {
		if (entry.typeId() != QMetaType::QString) {
			config.ignored++;
			return;
		}

		auto text = entry.toString().trimmed();
		if (!text.isEmpty()) config.rules.append(text);
	};

	if (!value.isValid() || value.isNull()) return config;

	if (value.typeId() == QMetaType::QString) {
		for (const auto& part: value.toString().split(QLatin1Char(';'))) {
			add(QVariant(part));
		}
		return config;
	}

	if (value.typeId() == QMetaType::QStringList) {
		for (const auto& entry: value.toStringList()) add(QVariant(entry));
		return config;
	}

	if (value.canConvert<QVariantList>()) {
		for (const auto& entry: value.toList()) add(entry);
		return config;
	}

	// A scalar or object where the list belongs is not a rule.
	config.ignored++;
	return config;
}

void setExtraShellSurfaceRules(const QStringList& rules) {
	QWriteLocker lock(&extraRulesLock);
	extraRules.setPatterns(rules, ScreenSelectors::Reject);
	extraRuleList = rules;
}

QStringList extraShellSurfaceRules() {
	QReadLocker lock(&extraRulesLock);
	return extraRuleList;
}

bool extraShellSurfaceRulesUseTitles() {
	QReadLocker lock(&extraRulesLock);
	return extraRules.hasTitleRules();
}

bool matchesExtraShellSurfaceRule(const Identity& identity) {
	QReadLocker lock(&extraRulesLock);
	if (extraRules.isEmpty()) return false;

	// A surface selector is not screen-scoped, and the identity has no app id.
	// The unprefixed dialect therefore matches executable and class here.
	return extraRules.matches(
	    QString(),
	    identity.processName,
	    identity.title,
	    identity.windowClass,
	    QString()
	);
}

bool titleChangeMakesShellSurface(const QString& title) {
	QReadLocker lock(&extraRulesLock);
	return extraRules.matchesTitle(title);
}

QString windowTitle(HWND hwnd) {
	auto length = GetWindowTextLengthW(hwnd);
	if (length <= 0) return {};

	auto title = std::wstring(static_cast<size_t>(length) + 1, L'\0');
	auto copied = GetWindowTextW(hwnd, title.data(), length + 1);
	return QString::fromWCharArray(title.data(), copied);
}

bool isTaskListWindow(const Identity& identity) {
	// WS_EX_APPWINDOW forces a taskbar button even on a tool or owned window.
	if (identity.appWindow) return true;

	return !identity.toolWindow && !identity.noActivate && !identity.owned;
}

bool isShellSurface(const Identity& identity) {
	if (isShellClass(identity.windowClass)) return true;

	if (coreWindow(identity.windowClass)) {
		auto processName = identity.processName.toLower();
		if (processName == QStringLiteral("explorer.exe") || isShellHostProcess(processName)) {
			return true;
		}
	}

	if (isShellHostProcess(identity.processName)) return true;

	// AppBar registration itself has no documented per-window query. A
	// registered AppBar that is not a known shell surface falls to this list.
	return matchesExtraShellSurfaceRule(identity);
}

bool isFullscreenOccluder(const Identity& identity, const QRect& frame, const QRect& monitor) {
	if (!fullscreen_geometry::coversOnly(frame, monitor)) return false;
	if (!identity.visible || !identity.root || identity.child || identity.cloaked) return false;

	// Taskbar eligibility is intentionally irrelevant: a tool-window or
	// no-activate fullscreen video still blocks the panel.
	return !isShellSurface(identity);
}

bool isDesktopSurface(const Identity& identity) {
	auto windowClass = identity.windowClass.toLower();
	return windowClass == QStringLiteral("progman") || windowClass == QStringLiteral("workerw");
}

bool isApplicationIdentity(const Identity& identity) {
	if (!identity.root || identity.child || !identity.visible) return false;
	if (!isTaskListWindow(identity)) return false;
	if (!identity.hasTitle) return false;

	return !isShellSurface(identity);
}

bool isApplicationWindow(const Identity& identity) {
	return isApplicationIdentity(identity) && !identity.cloaked;
}

Cloak cloakState(HWND hwnd) {
	DWORD flags = 0;
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &flags, sizeof(flags)))) {
		return Cloak::None;
	}

	return cloakFromFlags(flags);
}

bool isCloaked(HWND hwnd) { return cloakState(hwnd) != Cloak::None; }

Identity observe(HWND hwnd) {
	auto identity = Identity();
	if (hwnd == nullptr) return identity;

	wchar_t windowClass[128] {};
	auto classLength = GetClassNameW(hwnd, windowClass, static_cast<int>(std::size(windowClass)));
	identity.windowClass = QString::fromWCharArray(windowClass, classLength);

	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	identity.visible = IsWindowVisible(hwnd) != FALSE;
	identity.root = GetAncestor(hwnd, GA_ROOT) == hwnd;
	identity.child = (style & WS_CHILD) != 0;
	identity.toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
	identity.appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
	identity.topmost = (exStyle & WS_EX_TOPMOST) != 0;
	identity.noActivate = (exStyle & WS_EX_NOACTIVATE) != 0;
	identity.owned = GetWindow(hwnd, GW_OWNER) != nullptr;
	identity.cloaked = isCloaked(hwnd);

	identity.title = windowTitle(hwnd);
	identity.hasTitle = GetWindowTextLengthW(hwnd) > 0;

	DWORD processId = 0;
	GetWindowThreadProcessId(hwnd, &processId);
	identity.processId = processId;
	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);

	if (process != nullptr) {
		wchar_t path[1024] {};
		DWORD length = static_cast<DWORD>(std::size(path));

		if (QueryFullProcessImageNameW(process, 0, path, &length)) {
			identity.processName =
			    QFileInfo(QString::fromWCharArray(path, static_cast<qsizetype>(length)))
			        .fileName()
			        .toLower();
		}

		CloseHandle(process);
	}

	return identity;
}

} // namespace qs::windows::system_surface
