#pragma once

#include <qrect.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qtypes.h>
#include <qvariant.h>

namespace qs::windows::system_surface {

// DWMWA_CLOAKED flags. A shell cloak means the window belongs to another
// virtual desktop; an app or inherited cloak hides a window the app itself
// suspended. An explicit app cloak wins over a shell cloak, so a hidden window
// never reserves space when its desktop changes; an inherited cloak alone
// follows the owner and is a shell cloak whenever the shell bit is set.
enum class Cloak { None, Shell, App };

[[nodiscard]] constexpr Cloak cloakFromFlags(quint32 flags) {
	constexpr quint32 APP = 0x1;
	constexpr quint32 SHELL = 0x2;
	constexpr quint32 INHERITED = 0x4;

	if ((flags & APP) != 0) return Cloak::App;
	if ((flags & SHELL) != 0) return Cloak::Shell;
	if ((flags & INHERITED) != 0) return Cloak::App;
	return Cloak::None;
}

// What the shell may learn about a top-level window without changing it.
//
// The predicate takes an observation instead of an HWND so the classification
// can be unit tested with Windows' real window styles and class names but
// without depending on the live desktop.
struct Identity {
	QString windowClass;
	// Lower-case executable file name, for example "explorer.exe". Empty when
	// the process could not be queried.
	QString processName;
	QString title;
	quint32 processId = 0;
	bool root = true;
	bool child = false;
	bool visible = false;
	bool cloaked = false;
	bool toolWindow = false;
	bool appWindow = false;
	bool topmost = false;
	bool noActivate = false;
	// True when GW_OWNER names another window. An owned window is a popup or
	// dialog, not a taskbar toplevel unless it opts in with WS_EX_APPWINDOW.
	bool owned = false;
	bool hasTitle = false;
};

// Windows' own top-level UI. These are never application windows: the task
// switcher, Task View, Start/Search and other shell flyouts, tray overflow,
// menus, foreground staging and the desktop. Windows.UI.Core.CoreWindow is
// intentionally not here: it is a shell surface only when hosted by a shell
// process, because ordinary UWP and DirectX applications use it too.
[[nodiscard]] bool isShellClass(const QString& windowClass);

// Profile config normalization for the optional shell-surface list. Accepts a
// semicolon-separated string or an array of strings; any other element is
// ignored and counted. Invalid and `screen:` selectors are dropped later by
// the selector compiler.
struct ShellSurfaceRuleConfig {
	QStringList rules;
	qsizetype ignored = 0;
};

[[nodiscard]] ShellSurfaceRuleConfig shellSurfaceRulesFromConfig(const QVariant& value);

// Optional user selectors for windows the generic policy cannot distinguish.
// Uses the same typed wildcard dialect as the tiling allowlist (title:, exe:,
// class:, and unprefixed executable/class; `screen:` is rejected). Empty by
// default. A rule can add a positive shell identity but never changes the
// taskbar or fullscreen-occlusion rules.
void setExtraShellSurfaceRules(const QStringList& rules);
[[nodiscard]] QStringList extraShellSurfaceRules();
[[nodiscard]] bool extraShellSurfaceRulesUseTitles();
[[nodiscard]] bool matchesExtraShellSurfaceRule(const Identity& identity);

// Whether a window's new title newly makes it a shell surface through a user
// `title:` selector. This is the only question a title change may ask: it
// never looks at styles, so a transient empty title or a style toggle during
// fullscreen or maximise cannot cost an unrelated window its tracked state.
// An empty title never matches.
[[nodiscard]] bool titleChangeMakesShellSurface(const QString& title);

// The window's current title, read without any process query. Empty when the
// window has none.
[[nodiscard]] QString windowTitle(HWND hwnd);

// Processes that only host system shell UI. Explorer is deliberately absent:
// explorer.exe also owns ordinary File Explorer windows.
[[nodiscard]] bool isShellHostProcess(const QString& processName);

// The taskbar's style contract: WS_EX_APPWINDOW forces a button, otherwise a
// tool window, a no-activate window or an owned popup has none. This is not a
// shell-surface test; a fullscreen mpv with "show in taskbar" off is still an
// application window for occlusion and active-window purposes.
[[nodiscard]] bool isTaskListWindow(const Identity& identity);

// Positive shell identity only: Windows' own shell UI by class or host
// process, CoreWindow under a shell host, or a user extra selector. No
// executable name is special-cased and no generic tool/no-activate style is
// included here.
[[nodiscard]] bool isShellSurface(const Identity& identity);

// Fullscreen occlusion: any visible, uncloaked top-level that covers exactly
// one monitor is an occluder unless it has positive shell identity. A window
// spanning monitors, such as a desktop-wide task switcher or overlay, is not
// fullscreen on any one of them. Taskbar eligibility deliberately does not
// matter, so a tool-window fullscreen video still seeds the AppBar fullscreen
// state.
[[nodiscard]] bool isFullscreenOccluder(
    const Identity& identity,
    const QRect& frame,
    const QRect& monitor
);

// The surfaces that mean "no application is focused" when they own the
// foreground. The task switcher, Task View and Start/Search are overlays: the
// last application stays active, as it does on a compositor overlay.
[[nodiscard]] bool isDesktopSurface(const Identity& identity);

// Structural application test: top-level, visible, titled, taskbar-eligible
// under the WS_EX_APPWINDOW/tool/no-activate/owned rules, and not a positive
// shell surface. Cloaking is intentionally not considered here because the
// tracker keeps windows that live on other virtual desktops.
[[nodiscard]] bool isApplicationIdentity(const Identity& identity);

// A window that may be shown as the foreground application or a fullscreen
// application. A cloaked window has no current on-screen geometry.
[[nodiscard]] bool isApplicationWindow(const Identity& identity);

// DWM cloaking, the only reliable "not composed on this desktop" signal.
[[nodiscard]] Cloak cloakState(HWND hwnd);
[[nodiscard]] bool isCloaked(HWND hwnd);

// Read the fields above. Never injects input, changes focus or touches state.
[[nodiscard]] Identity observe(HWND hwnd);

} // namespace qs::windows::system_surface
