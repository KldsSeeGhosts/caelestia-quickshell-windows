#pragma once

#include <cstdint>
#include <optional>

namespace qs::windows {

enum class PanelKeyboardFocus: std::uint8_t {
	None = 0,
	Exclusive = 1,
	OnDemand = 2,
};

namespace panel_focus_style {

// These are the Win32 values for WS_EX_TOOLWINDOW, WS_EX_NOACTIVATE and
// MA_NOACTIVATE. Keeping them local lets the focus policy compile and test
// without Qt, windows.h or a live HWND.
inline constexpr std::uint32_t toolWindow = 0x00000080u;
inline constexpr std::uint32_t noActivate = 0x08000000u;
inline constexpr std::intptr_t noActivateMouseResult = 3;

// Apply only the panel's focus-related extended styles. Every other bit is
// preserved, including bits Qt or another shell feature adds independently.
constexpr std::uint32_t applyExtendedStyle(
    std::uint32_t exStyle, PanelKeyboardFocus focus
) {
	exStyle |= toolWindow;
	if (focus == PanelKeyboardFocus::None) return exStyle | noActivate;
	return exStyle & ~noActivate;
}

constexpr bool allowsKeyboardFocusGrab(PanelKeyboardFocus focus) {
	return focus != PanelKeyboardFocus::None;
}

// WM_MOUSEACTIVATE must decline activation for a non-focusable panel without
// discarding the click. Focusable panels keep Windows' normal activation path.
constexpr std::optional<std::intptr_t> mouseActivationResult(PanelKeyboardFocus focus) {
	if (focus == PanelKeyboardFocus::None) return noActivateMouseResult;
	return std::nullopt;
}

} // namespace panel_focus_style

} // namespace qs::windows
