#include "floatingframe.hpp"

#include <dwmapi.h>
#include <qbytearray.h>
#include <qevent.h>
#include <qguiapplication.h>
#include <qstring.h>
#include <qtypes.h>
#include <qwindow.h>
#include <windows.h>
#include <windowsx.h>

namespace {

constexpr LONG_PTR FRAME_STYLE =
    WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

bool isFrameless(const QWindow* window) {
	return window->flags().testFlag(Qt::FramelessWindowHint);
}

// Thickness of the invisible resize border, in physical pixels, at the window's current DPI.
int resizeBorder(HWND hwnd, bool vertical) {
	auto dpi = GetDpiForWindow(hwnd);
	return GetSystemMetricsForDpi(vertical ? SM_CYSIZEFRAME : SM_CXSIZEFRAME, dpi)
	     + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT hitTest(HWND hwnd, const QWindow* window, LPARAM lparam) {
	if (IsZoomed(hwnd) || IsIconic(hwnd)) return HTCLIENT;
	// An axis that cannot change size has no resize border.
	auto sizeX = window->minimumWidth() != window->maximumWidth();
	auto sizeY = window->minimumHeight() != window->maximumHeight();
	if (!sizeX && !sizeY) return HTCLIENT;

	RECT rect {};
	GetWindowRect(hwnd, &rect);

	auto x = GET_X_LPARAM(lparam);
	auto y = GET_Y_LPARAM(lparam);
	auto bx = resizeBorder(hwnd, false);
	auto by = resizeBorder(hwnd, true);

	auto left = sizeX && x < rect.left + bx;
	auto right = sizeX && x >= rect.right - bx;
	auto top = sizeY && y < rect.top + by;
	auto bottom = sizeY && y >= rect.bottom - by;

	if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
	if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
	if (left) return HTLEFT;
	if (right) return HTRIGHT;
	return HTCLIENT;
}

} // namespace

void FloatingFrameWindow::applyFrame(QWindow* window) {
	if (window == nullptr || window->handle() == nullptr) return;
	// Other platform plugins (the offscreen one used by the smoke gates) have no HWND.
	if (!QGuiApplication::platformName().startsWith(QStringLiteral("windows"))) return;

	auto* hwnd = reinterpret_cast<HWND>(window->winId()); // NOLINT
	auto frameless = isFrameless(window);

	if (frameless) {
		auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		if ((style & FRAME_STYLE) != FRAME_STYLE)
			SetWindowLongPtrW(hwnd, GWL_STYLE, style | FRAME_STYLE);
	}

	// Both attributes are Windows 11 only and fail harmlessly on older builds. The default border
	// is a 1px system outline that would sit outside the shell's own edge.
	auto corner =
	    static_cast<DWM_WINDOW_CORNER_PREFERENCE>(frameless ? DWMWCP_ROUND : DWMWCP_DEFAULT);
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

	COLORREF border = frameless ? DWMWA_COLOR_NONE : DWMWA_COLOR_DEFAULT;
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));

	// Make the system recompute the non-client area under the new style.
	SetWindowPos(
	    hwnd,
	    nullptr,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
	);
}

bool FloatingFrameWindow::event(QEvent* event) {
	if (event->type() == QEvent::PlatformSurface) {
		auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event); // NOLINT
		auto handled = this->ProxiedWindow::event(event);

		if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
			FloatingFrameWindow::applyFrame(this);
		}

		return handled;
	}

	return this->ProxiedWindow::event(event);
}

bool FloatingFrameWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
	if (eventType == "windows_generic_MSG" && isFrameless(this)) {
		auto* msg = static_cast<MSG*>(message);

		if (msg->message == WM_NCCALCSIZE) {
			// The whole window is client area. A maximized window may overhang its monitor by the
			// resize border (Qt already caps frameless windows at the work area, Windows may not), so
			// clip it to the work area rather than subtracting a fixed inset.
			auto* rect = msg->wParam != 0
			               ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam)->rgrc[0] // NOLINT
			               : reinterpret_cast<RECT*>(msg->lParam);                       // NOLINT

			if (IsZoomed(msg->hwnd)) {
				MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
				if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
					RECT clipped {};
					if (IntersectRect(&clipped, rect, &info.rcWork)) *rect = clipped;
				}
			}

			*result = 0;
			return true;
		}

		if (msg->message == WM_NCHITTEST) {
			auto hit = hitTest(msg->hwnd, this, msg->lParam);

			// Everything but the resize borders is the content: let Qt deliver it as input.
			if (hit != HTCLIENT) {
				*result = hit;
				return true;
			}
		}
	}

	return this->ProxiedWindow::nativeEvent(eventType, message, result);
}
