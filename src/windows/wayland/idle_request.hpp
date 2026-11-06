#pragma once

#include <windows.h>

namespace qs::windows::wayland {

struct IdleRequestState {
	bool display = false;
	bool system = false;

	[[nodiscard]] bool held() const { return this->display || this->system; }
	[[nodiscard]] bool fullyHeld() const { return this->display && this->system; }
};

struct IdleRequestFns {
	BOOL (*set)(HANDLE request, POWER_REQUEST_TYPE type) = nullptr;
	BOOL (*clear)(HANDLE request, POWER_REQUEST_TYPE type) = nullptr;
};

inline bool applyIdleRequestType(
    HANDLE request,
    POWER_REQUEST_TYPE type,
    bool want,
    bool& held,
    const IdleRequestFns& fns
) {
	if (held == want) return true;
	const BOOL ok = want ? fns.set(request, type) : fns.clear(request, type);
	if (!ok) return false;
	held = want;
	return true;
}

// Move both request types to `enable`. A type that is already in the desired
// state is not touched, so a retry cannot increment an outstanding count.
// A partial acquire is rolled back. A failed clear leaves that type held.
// Returns false when the requested end state was not reached.
inline bool applyIdleRequest(HANDLE request, bool enable, IdleRequestState& state, const IdleRequestFns& fns) {
	const bool displayOk =
	    applyIdleRequestType(request, PowerRequestDisplayRequired, enable, state.display, fns);
	if (enable && !displayOk) {
		applyIdleRequestType(request, PowerRequestDisplayRequired, false, state.display, fns);
		applyIdleRequestType(request, PowerRequestSystemRequired, false, state.system, fns);
		return false;
	}
	const bool systemOk =
	    applyIdleRequestType(request, PowerRequestSystemRequired, enable, state.system, fns);
	if (enable && !systemOk) {
		applyIdleRequestType(request, PowerRequestDisplayRequired, false, state.display, fns);
		applyIdleRequestType(request, PowerRequestSystemRequired, false, state.system, fns);
		return false;
	}
	return displayOk && systemOk;
}

} // namespace qs::windows::wayland
