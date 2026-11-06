#include "tray_reorder.hpp"
#include <utility>

#include "../../window_event_filter.hpp"

namespace qs::windows::services::systray {

namespace {

TrayReorderWatch* gWatch = nullptr; // NOLINT

} // namespace

TrayReorderWatch::~TrayReorderWatch() { this->remove(); }

bool TrayReorderWatch::install(HWND taskbar, Repair repair) {
	this->remove();
	if (taskbar == nullptr || !repair) return false;

	DWORD pid = 0;
	auto thread = GetWindowThreadProcessId(taskbar, &pid);
	if (thread == 0) return false;

	// Scoped like the desktop host's hooks: to the process and thread that own
	// the window, so only Explorer's own restacks and moves arrive.
	this->reorderHook = SetWinEventHook(
	    EVENT_OBJECT_REORDER,
	    EVENT_OBJECT_REORDER,
	    nullptr,
	    &TrayReorderWatch::onEvent,
	    pid,
	    thread,
	    WINEVENT_OUTOFCONTEXT
	);

	this->moveHook = SetWinEventHook(
	    EVENT_OBJECT_LOCATIONCHANGE,
	    EVENT_OBJECT_LOCATIONCHANGE,
	    nullptr,
	    &TrayReorderWatch::onEvent,
	    pid,
	    thread,
	    WINEVENT_OUTOFCONTEXT
	);

	if (this->reorderHook == nullptr || this->moveHook == nullptr) {
		this->remove();
		return false;
	}

	this->taskbar = taskbar;
	this->pid = pid;
	this->thread = thread;
	this->repair = std::move(repair);
	gWatch = this;
	return true;
}

void TrayReorderWatch::remove() {
	if (this->reorderHook != nullptr) UnhookWinEvent(this->reorderHook);
	if (this->moveHook != nullptr) UnhookWinEvent(this->moveHook);
	this->reorderHook = nullptr;
	this->moveHook = nullptr;
	this->taskbar = nullptr;
	this->pid = 0;
	this->thread = 0;
	this->repair = {};
	if (gWatch == this) gWatch = nullptr;
}

bool TrayReorderWatch::installed() const { return this->reorderHook != nullptr; }

bool TrayReorderWatch::watches(HWND taskbar) const {
	if (!this->installed() || this->taskbar != taskbar) return false;

	// The hooks are scoped to the owning process and thread, so a window handle
	// that now belongs to another thread is not what they watch.
	DWORD pid = 0;
	return GetWindowThreadProcessId(taskbar, &pid) == this->thread && pid == this->pid;
}

void CALLBACK TrayReorderWatch::onEvent(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG object,
    LONG child,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	auto* watch = gWatch;
	if (watch == nullptr) return;

	if (event == EVENT_OBJECT_REORDER) {
		// A top-level z-order change is announced on the desktop parent, not on
		// the window that moved.
		if (!qs::windows::window_events::isDesktopReorder(
		        event,
		        hwnd,
		        object,
		        child,
		        GetDesktopWindow()
		    ))
		{
			return;
		}
	} else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
		// A move or resize is announced on the taskbar itself.
		if (hwnd != watch->taskbar || object != OBJID_WINDOW || child != CHILDID_SELF) return;
	} else {
		return;
	}

	watch->repair();
}

} // namespace qs::windows::services::systray
