#pragma once

#include <cstddef>

#include <guiddef.h>
#include <qstring.h>
#include <windows.h>
#include <wlanapi.h>

// Pure routing rules for the WLAN backend. Windows reports notifications for
// every WLAN interface to one callback, so the backend selects one interface
// explicitly and every observation is filtered by that interface's GUID.
namespace qs::windows::sys::wifi_route {

struct InterfaceCandidate {
	GUID guid {};
	WLAN_INTERFACE_STATE state = wlan_interface_state_not_ready;
};

struct Selection {
	bool present = false;
	bool changed = false;
	GUID guid {};
};

// A connected interface describes the connection the user sees; associating
// and authenticating are next, then any other state. Enumeration order breaks
// ties, so the selection stays deterministic when several adapters are idle.
inline int interfaceRank(WLAN_INTERFACE_STATE state) {
	switch (state) {
	case wlan_interface_state_connected: return 0;
	case wlan_interface_state_associating:
	case wlan_interface_state_authenticating:
	case wlan_interface_state_discovering: return 1;
	default: return 2;
	}
}

inline const InterfaceCandidate* primary(const InterfaceCandidate* items, size_t count) {
	if (items == nullptr || count == 0) return nullptr;

	const auto* best = &items[0];
	for (size_t i = 1; i < count; i++) {
		if (interfaceRank(items[i].state) < interfaceRank(best->state)) best = &items[i];
	}
	return best;
}

// Keeps the current interface while it is still enumerated, so another adapter
// arriving or leaving cannot move the selection or clear its state. When the
// current one disappeared, selects the primary replacement; when none remain,
// clears the selection.
inline Selection
nextSelection(const GUID& current, bool present, const InterfaceCandidate* items, size_t count) {
	Selection selection;

	if (present) {
		for (size_t i = 0; i < count; i++) {
			if (IsEqualGUID(items[i].guid, current)) {
				selection.present = true;
				selection.guid = current;
				return selection;
			}
		}
	}

	const auto* best = primary(items, count);
	selection.changed = true;
	if (best == nullptr) return selection;

	selection.present = true;
	selection.guid = best->guid;
	return selection;
}

// A notification describes exactly one interface. With no selection, or with a
// notification for another adapter, it carries no state for this backend.
inline bool matchesSelected(const GUID& selected, const GUID& notified) {
	return !IsEqualGUID(selected, GUID {}) && IsEqualGUID(selected, notified);
}

// A connection request stays pending until Windows completes it on the
// selected interface. Removing that interface ends the request: its completion
// can no longer arrive, so the backend reports the pending SSID as failed
// instead of leaving the shell stuck reporting a connection in progress.
struct PendingEnd {
	bool failed = false;
	QString ssid;
};

inline PendingEnd endPendingOnRemoval(bool connecting, const QString& ssid) {
	if (!connecting) return {};
	return {true, ssid};
}

} // namespace qs::windows::sys::wifi_route
