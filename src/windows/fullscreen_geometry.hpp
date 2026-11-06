#pragma once

#include <qrect.h>
#include <qtypes.h>

namespace qs::windows::fullscreen_geometry {

// DWM can round a borderless window a pixel or two inside the monitor, and a
// display-mode change can report the new monitor rectangle before the
// application has adopted it. A small tolerance keeps those windows fullscreen
// without mistaking an ordinary window for one.
inline constexpr int TOLERANCE = 2;

// A fullscreen application belongs to one monitor, but its frame border or
// shadow can reach a few physical pixels past that monitor's edge. A window
// that extends farther onto a neighbour is a desktop-spanning overlay (an
// Alt+Tab switcher or a multi-monitor wallpaper surface), not a fullscreen
// application on either monitor. Frame and monitor rectangles are physical
// pixels, so mixed display scaling compares the same space.
inline constexpr int OVERHANG_TOLERANCE = 8;

[[nodiscard]] inline bool covers(const QRect& frame, const QRect& area) {
	if (!frame.isValid() || !area.isValid()) return false;

	return frame.left() <= area.left() + TOLERANCE
	    && frame.top() <= area.top() + TOLERANCE
	    && frame.left() + frame.width() >= area.left() + area.width() - TOLERANCE
	    && frame.top() + frame.height() >= area.top() + area.height() - TOLERANCE;
}

// `covers` alone also accepts a window spanning the whole virtual desktop on
// every monitor it intersects. Per-monitor fullscreen requires the frame to
// stay on this monitor, within the border/shadow overhang.
[[nodiscard]] inline bool coversOnly(const QRect& frame, const QRect& monitor) {
	if (!covers(frame, monitor)) return false;

	return frame.left() >= monitor.left() - OVERHANG_TOLERANCE
	    && frame.top() >= monitor.top() - OVERHANG_TOLERANCE
	    && frame.left() + frame.width() <= monitor.left() + monitor.width() + OVERHANG_TOLERANCE
	    && frame.top() + frame.height() <= monitor.top() + monitor.height() + OVERHANG_TOLERANCE;
}

[[nodiscard]] inline bool sameFrame(const QRect& left, const QRect& right) {
	if (!left.isValid() || !right.isValid()) return false;

	return qAbs(left.left() - right.left()) <= TOLERANCE
	    && qAbs(left.top() - right.top()) <= TOLERANCE
	    && qAbs(left.width() - right.width()) <= TOLERANCE
	    && qAbs(left.height() - right.height()) <= TOLERANCE;
}

// The exact fullscreen episode: the frame that was classified fullscreen and
// the monitor it covered. The latch may only be used while the window still
// has that frame on that monitor, so a stale mode cannot keep a window
// fullscreen after it has restored an ordinary rectangle.
struct Latch {
	QRect frame;
	QRect monitor;
	quintptr monitorId = 0;

	[[nodiscard]] bool isValid() const { return frame.isValid() && monitor.isValid() && monitorId != 0; }
};

struct Detection {
	bool fullscreen = false;
	Latch latch;
};

// Classify a window as fullscreen and choose the latch to keep.
//
// `monitorId` identifies the HMONITOR the frame is nearest to. A window that
// covers that monitor but extends meaningfully onto another one is not
// fullscreen on either. A minimized window has no meaningful geometry, so the
// latch survives a minimize on the same monitor (a restore into fullscreen is
// still recognised) but not a monitor change. A maximized window is never
// fullscreen and clears it. A latch is only honoured while the frame is
// unchanged and the window is still on the monitor it was latched to; any
// other frame clears it.
[[nodiscard]] inline Detection detect(
    bool minimized,
    bool maximized,
    const QRect& frame,
    const QRect& monitor,
    quintptr monitorId,
    const Latch& latched
) {
	auto detection = Detection();

	if (maximized) return detection;

	if (minimized) {
		if (latched.isValid() && latched.monitorId == monitorId) detection.latch = latched;
		return detection;
	}

	if (coversOnly(frame, monitor)) {
		detection.fullscreen = true;
		detection.latch = Latch {.frame = frame, .monitor = monitor, .monitorId = monitorId};
		return detection;
	}

	if (latched.isValid() && latched.monitorId == monitorId && sameFrame(frame, latched.frame)
	    && coversOnly(latched.frame, latched.monitor))
	{
		detection.fullscreen = true;
		detection.latch = latched;
	}

	return detection;
}

} // namespace qs::windows::fullscreen_geometry
