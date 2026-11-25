#pragma once

#include <functional>
#include <optional>

#include <qhash.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>

#include "../tiling_result.hpp"
#include "../window_tracker.hpp"
#include "dispatcher_args.hpp"
#include "window_cycle.hpp"

namespace qs::hyprland::ipc {

class HyprlandIpc;

class Dispatcher {
public:
	explicit Dispatcher(HyprlandIpc* ipc);

#ifdef QS_TEST
	struct TestHooks {
		qs::windows::WindowTracker* tracker = nullptr;
		std::function<bool()> pinSupported;
		std::function<std::optional<bool>(HWND)> queryPin;
		std::function<bool(HWND, bool)> setPin;
		std::function<std::optional<qs::windows::FloatingChangeResult>(
		    qs::windows::TrackedWindow*, bool, bool)>
		    setFloating;
	};

	explicit Dispatcher(TestHooks hooks);
	void pinForTest(qs::windows::TrackedWindow* window, PinRequest request) { this->pin(window, request); }
	void toggleFloatingForTest(qs::windows::TrackedWindow* window, const QString& action) {
		this->toggleFloating(window, action);
	}
#endif

	void dispatch(const QString& request);

private:
	struct LuaCall {
		QString function;
		QString scalar;
		QHash<QString, QString> table;
		QStringList positional;
		bool ok = false;
	};

	static LuaCall parseLua(const QString& request);
	static QString unquote(QString value);

	void dispatchLua(const LuaCall& call);
	void dispatchClassic(const QString& name, const QString& args);

	[[nodiscard]] qsizetype resolveWorkspace(const WorkspaceTarget& target, bool& create) const;
	[[nodiscard]] qs::windows::TrackedWindow* resolveWindow(const QString& arg) const;

	void focusWorkspace(const QString& arg);
	void focusMonitor(const QString& arg);
	void cycleWindow(std::optional<bool> backward);
	void moveToWorkspace(qs::windows::TrackedWindow* window, const QString& arg, bool follow);
	void fullscreen(qs::windows::TrackedWindow* window, int mode);
	void pin(qs::windows::TrackedWindow* window, PinRequest request);
	void moveFocus(const QString& direction);
	void moveWindow(const QString& direction);
	void swapWindow(const QString& direction);
	void toggleFloating(qs::windows::TrackedWindow* window, const QString& action);
	void toggleSplit();
	void layoutMessage(const QString& message);
	void resizeActive(const QString& args);
	void centerWindow();
	void moveWindowPixel(qs::windows::TrackedWindow* window, const QString& x, const QString& y);
	void exec(const QString& command);

	[[nodiscard]] qs::windows::WindowTracker* actionTracker() const;
	[[nodiscard]] bool pinActionSupported() const;
	[[nodiscard]] std::optional<bool> pinState(HWND hwnd) const;
	bool setPinState(HWND hwnd, bool pinned);
	[[nodiscard]] std::optional<qs::windows::FloatingChangeResult>
	changeFloating(qs::windows::TrackedWindow* window, bool floating, bool toggle) const;

	void unknown(const QString& what);
	void unsupported(const QString& what);

	HyprlandIpc* ipc;
#ifdef QS_TEST
	TestHooks testHooks;
#endif
	QSet<QString> warned;
	WindowCycle<qs::windows::TrackedWindow> windowCycle;
};

} // namespace qs::hyprland::ipc
