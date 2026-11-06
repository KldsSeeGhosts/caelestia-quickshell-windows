#pragma once

#include <functional>
#include <vector>

#include <qt_windows.h>

#include <qimage.h>
#include <qstring.h>
#include <quuid.h>

#include "hook_state.hpp"

namespace qs::windows::services::systray {

struct TrayIconMessage {
	DWORD message = 0;
	HWND hwnd = nullptr;
	UINT uid = 0;
	UINT flags = 0;
	UINT callbackMessage = 0;
	QImage icon;
	QString tip;
	DWORD state = 0;
	DWORD stateMask = 0;
	UINT version = 0;
	QUuid guid;

	bool seeded = false;
	QString exePath;
};

using TrayIconSink = std::function<void(TrayIconMessage)>;

class TrayHook {
public:
	static void start(
	    TrayIconSink sink,
	    std::function<void()> missedTraffic,
	    bool brief,
	    std::function<void(TrayHookState)> stateChanged = {}
	);
	static void stop();
	[[nodiscard]] static bool running();
	[[nodiscard]] static QVariantMap capability();
#ifdef QS_TRAY_HOOK_TEST
	static void setTestElevated(bool elevated);

	// Fixtures drive the real hook thread against a stand-in taskbar of the
	// fixture class instead of Explorer's taskbar. Without one, create() fails
	// as before, so a fixture never registers Shell_TrayWnd or sends anything
	// to Explorer.
	static void registerTestTrayClass();
	[[nodiscard]] static const wchar_t* testTrayClass();
	static void setTestTaskbar(HWND taskbar);
	static void setTestBriefFrontMs(ULONGLONG ms);
	static void testTaskbarCreated();
	[[nodiscard]] static HWND testMirror();
	[[nodiscard]] static int testRepairs();
	static void resetTestRepairs();
#endif

	static void announceTo(std::vector<HWND> owners);
};

void seedFromExplorer(TrayIconSink sink, std::function<void(bool ok)> done);
[[nodiscard]] QImage imageFromIcon(HICON icon);

struct ExplorerIconData {
	HWND hwnd = nullptr;
	UINT uid = 0;
	UINT callbackMessage = 0;
	UINT version = 0;
};

std::vector<ExplorerIconData> readExplorerToolbars();

} // namespace qs::windows::services::systray
