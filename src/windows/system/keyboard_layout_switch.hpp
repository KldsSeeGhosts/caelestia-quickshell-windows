#pragma once

#include <functional>

#include <qstring.h>
#include <qt_windows.h>

#include <dwmapi.h>

namespace qs::windows::sys {

// Picks the window whose layout the popout observes and changes. `isApp` is
// false for shell surfaces and this process's own windows. The foreground app
// wins normally; while shell UI owns the foreground, the retained application
// is used instead. A shell window is never returned.
inline HWND
resolveLayoutTarget(HWND foreground, bool foregroundIsApp, HWND retained, bool retainedIsApp) {
	if (foreground != nullptr && foregroundIsApp) return foreground;
	if (retained != nullptr && retainedIsApp) return retained;
	return nullptr;
}

// Process and thread the tracker recorded for the retained window.
struct LayoutTargetIdentity {
	DWORD processId = 0;
	DWORD threadId = 0;
};

// Facts about a window right now, read independently of the tracker so the
// check does not depend on the tracker having reconciled yet.
struct LayoutTargetState {
	bool exists = false;
	bool visible = false;
	bool minimized = false;
	bool cloaked = false;
	DWORD processId = 0;
	DWORD threadId = 0;
};

// A retained window may be used only while it is still the window the tracker
// recorded and can receive input: it exists, is visible, is not minimized and
// is not DWM-cloaked. A recycled handle fails the process/thread comparison.
inline bool
isLiveRetainedTarget(const LayoutTargetState& state, const LayoutTargetIdentity& recorded) {
	if (!state.exists || !state.visible || state.minimized || state.cloaked) return false;
	if (recorded.processId == 0 || recorded.threadId == 0) return false;
	return state.processId == recorded.processId && state.threadId == recorded.threadId;
}

inline LayoutTargetState observeLayoutTarget(HWND hwnd) {
	LayoutTargetState state;
	if (hwnd == nullptr || IsWindow(hwnd) == FALSE) return state;

	state.exists = true;
	state.visible = IsWindowVisible(hwnd) != FALSE;
	state.minimized = IsIconic(hwnd) != FALSE;

	// An unreadable cloak state is treated as cloaked, like the tracker does.
	DWORD cloaked = 0;
	state.cloaked =
	    FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked != 0;

	state.threadId = GetWindowThreadProcessId(hwnd, &state.processId);
	return state;
}

// Window and thread queries behind resolveLayoutInput, injectable so the
// resolution can be tested without real windows.
struct LayoutInputQueries {
	std::function<DWORD(HWND, DWORD*)> threadOf;         // thread id, optional process id
	std::function<bool(DWORD, HWND&, HWND&)> guiThread;  // focus and active window of a thread
	std::function<HWND(HWND)> rootOwnerOf;               // GA_ROOTOWNER
	std::function<QString(HWND)> classOf;                // window class name
	std::function<HWND(HWND)> coreWindowOf;              // hosted CoreWindow child, or null
	std::function<bool(HWND, HWND)> isChild;             // IsChild(parent, window)
	std::function<HWND(HWND)> ownerOf;                   // GW_OWNER, or null
};

struct LayoutInputTarget {
	HWND window = nullptr;
	DWORD threadId = 0;
};

// The window that actually receives keyboard input for a validated top-level
// application window, and its thread. Layouts are per thread and
// WM_INPUTLANGCHANGEREQUEST is handled by the focused window, so reading or
// posting to the top-level frame is wrong whenever input lives elsewhere. That
// is the case for hosted UWP apps: ApplicationFrameHost owns the frame while
// the app's CoreWindow, with the focus, belongs to another thread and process
// (the approach AutoHotkey uses for the same problem).
//
// Classic window: the input thread is the frame's. Its focus window is used,
// then its active window, then the frame, and only a window that shares the
// frame's root owner is accepted.
//
// Hosted UWP, once a CoreWindow on another process or thread is proven: the
// input subtree is that CoreWindow. A candidate (focus, then active) counts
// only if it is the CoreWindow, a descendant of it, or owned by a window in
// that subtree. The host's frame and anything outside the subtree never count;
// the fallback is the CoreWindow and its thread.
//
// UWP apps are not guaranteed to honour WM_INPUTLANGCHANGEREQUEST, so a
// switch posted here is still only a request.
inline LayoutInputTarget resolveLayoutInput(HWND frame, const LayoutInputQueries& query) {
	LayoutInputTarget fallback {frame, 0};
	if (frame == nullptr) return fallback;

	DWORD frameProcess = 0;
	auto frameThread = query.threadOf(frame, &frameProcess);
	fallback.threadId = frameThread;
	if (frameThread == 0) return fallback;

	HWND core = nullptr;
	auto inputThread = frameThread;
	if (query.classOf(frame) == QStringLiteral("ApplicationFrameWindow")) {
		auto* hosted = query.coreWindowOf(frame);
		if (hosted != nullptr) {
			DWORD coreProcess = 0;
			auto coreThread = query.threadOf(hosted, &coreProcess);
			if (coreThread != 0 && (coreThread != frameThread || coreProcess != frameProcess)) {
				core = hosted;
				inputThread = coreThread;
				fallback = {core, coreThread};
			}
		}
	}

	HWND focus = nullptr;
	HWND active = nullptr;
	if (!query.guiThread(inputThread, focus, active)) return fallback;

	auto* candidate = focus != nullptr ? focus : active;
	if (candidate == nullptr) return fallback;

	if (core != nullptr) {
		// A bounded walk up the owner chain; a cycle or a deep chain is refused.
		bool inSubtree = false;
		auto* window = candidate;
		for (int depth = 0; window != nullptr && depth < 16; ++depth) {
			if (window == core || query.isChild(core, window)) {
				inSubtree = true;
				break;
			}
			window = query.ownerOf(window);
		}
		if (!inSubtree) return fallback;
	} else if (candidate != frame && query.rootOwnerOf(candidate) != query.rootOwnerOf(frame)) {
		return fallback;
	}

	auto candidateThread = query.threadOf(candidate, nullptr);
	if (candidateThread == 0) return fallback;
	return {candidate, candidateThread};
}

// Position of the observed layout in the entry list. No target (nullptr), a
// layout that is not listed, and a handle shared by several entries all give
// -1. Several IMEs of one language switch through the same HKL, and another
// thread's active TSF profile cannot be read, so guessing one would mark the
// wrong entry active.
template <typename Layouts, typename HandleOf>
int layoutIndexOf(const Layouts& layouts, HKL observed, const HandleOf& handleOf) {
	if (observed == nullptr) return -1;

	int found = -1;
	int index = 0;
	for (const auto& layout: layouts) {
		if (handleOf(layout) == observed) {
			if (found != -1) return -1;
			found = index;
		}
		++index;
	}
	return found;
}

struct LayoutSwitchOutcome {
	QString result;
	DWORD error = 0;
};

using LayoutSwitchPost = std::function<bool(HWND, HKL, DWORD&)>;

// "requested" only means the message was posted. It is never confirmation that
// the target changed layout; UWP apps in particular are not guaranteed to
// honour WM_INPUTLANGCHANGEREQUEST. "denied" covers PostMessage failures, including
// UIPI's ERROR_ACCESS_DENIED for an elevated foreground app.
inline LayoutSwitchOutcome postLayoutSwitch(HWND target, HKL hkl, const LayoutSwitchPost& post) {
	if (target == nullptr) return {QStringLiteral("no-target"), 0};

	DWORD error = 0;
	if (!post(target, hkl, error)) return {QStringLiteral("denied"), error};
	return {QStringLiteral("requested"), 0};
}

} // namespace qs::windows::sys
