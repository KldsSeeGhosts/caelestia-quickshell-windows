#pragma once

#include <qt_windows.h>

namespace qs::windows::window_events {

// A top-level Z-order change is announced on the desktop parent, not
// necessarily on the reordered application. The desktop is never tracked.
constexpr bool isDesktopReorder(
    DWORD event, HWND hwnd, LONG object, LONG child, HWND desktop
) {
	return event == EVENT_OBJECT_REORDER && hwnd != nullptr && hwnd == desktop
	    && (object == OBJID_WINDOW || object == OBJID_CLIENT) && child == CHILDID_SELF;
}

} // namespace qs::windows::window_events
