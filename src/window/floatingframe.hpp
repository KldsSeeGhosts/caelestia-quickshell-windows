#pragma once

#ifdef _WIN32

#include <qbytearray.h>
#include <qevent.h>
#include <qtypes.h>
#include <qwindow.h>

#include "proxywindow.hpp"

// Backing window of a FloatingWindow on Windows. It does nothing unless the window has
// Qt::FramelessWindowHint, which is what FloatingWindow.frameless sets.
//
// A frameless Qt window is a bare WS_POPUP, which Windows will not resize, snap, maximize or
// shadow. The frame style bits are put back after the native window is created and the
// non-client area is then removed again in WM_NCCALCSIZE, so the content fills the whole window
// while the system keeps its resize borders, Aero Snap, Win+arrows, minimize and maximize
// animations, taskbar entry, shadow and rounded corners.
class FloatingFrameWindow: public ProxiedWindow {
public:
	using ProxiedWindow::ProxiedWindow;

	// Reconcile the native window with Qt::FramelessWindowHint. Safe before the native window exists.
	static void applyFrame(QWindow* window);

protected:
	bool event(QEvent* event) override;
	bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
};

#endif
