#pragma once

#include <qt_windows.h>

// Keeps a window out of screen capture with SetWindowDisplayAffinity.
//
// WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004 (build 19041). Older builds
// reject it with ERROR_INVALID_PARAMETER. The window is then captured normally,
// which is the same as having no exclusion. WDA_MONITOR is never used as a
// fallback because it shows a black rectangle instead of hiding the window.
//
// The affinity is a property of one native window. A new HWND starts with
// WDA_NONE, so the owner of the property applies it again after every surface
// creation. This header holds only the decision and the Win32 calls, so a
// fixture can drive it with fake calls and without a window.
//
// Window types, as measured on Windows 11 26300 (capture_native_test, and a
// probe with a Windows 8+ application manifest):
//   - A top-level window takes the affinity and reads it back. That includes a
//     WS_EX_LAYERED one, whether it is drawn with SetLayeredWindowAttributes or
//     UpdateLayeredWindow, and a WS_EX_NOREDIRECTIONBITMAP, tool, topmost or
//     no-activate window. Setting the affinity does not change the layered
//     alpha, and the window stays visible on the monitor.
//   - A child window is rejected with ERROR_INVALID_PARAMETER, layered or not,
//     and GetWindowDisplayAffinity fails on it too. A desktop panel embedded in
//     the desktop window (and QS_DESKTOP_LAYERED, which makes that child
//     WS_EX_LAYERED) therefore cannot be excluded. Layering is not the cause.
//     A panel that leaves the desktop window is a top-level window again, so
//     it is applied again then.
// Whether the exclusion hides the window's pixels in a real capture needs a
// composed desktop. That is a live check, not covered by the fixtures.

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace qs::windows::capture_affinity {

struct Api {
	BOOL (*set)(HWND, DWORD) = &SetWindowDisplayAffinity;
	BOOL (*get)(HWND, DWORD*) = &GetWindowDisplayAffinity;
	DWORD (*lastError)() = &GetLastError;
};

struct Outcome {
	// The window has the affinity the caller asked for after this call.
	bool matches = true;
	// Set was called.
	bool called = false;
	// Win32 error of a failed call, 0 otherwise.
	DWORD error = 0;
	// True for the first failure seen by this object, so the caller logs once.
	bool firstFailure = false;
};

class CaptureExclusion {
public:
	// Make the window's affinity match `exclude`. Reads the current affinity
	// first and calls Set only when it differs. A window that already has the
	// requested state, including every ordinary panel that never asked for
	// exclusion, therefore costs one read and no write. That also keeps a reload,
	// which adopts a window another instance may have excluded, correct in both
	// directions.
	Outcome apply(HWND hwnd, bool exclude, const Api& api = Api()) {
		auto outcome = Outcome();
		if (hwnd == nullptr) return outcome;

		const DWORD wanted = exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
		DWORD current = WDA_NONE;
		// A window type that cannot report an affinity (a child window) has none.
		if (!api.get(hwnd, &current)) current = WDA_NONE;
		if (current == wanted) return outcome;

		outcome.called = true;
		if (api.set(hwnd, wanted)) return outcome;

		outcome.matches = false;
		outcome.error = api.lastError();
		outcome.firstFailure = !this->failureSeen;
		this->failureSeen = true;
		return outcome;
	}

private:
	bool failureSeen = false;
};

} // namespace qs::windows::capture_affinity
