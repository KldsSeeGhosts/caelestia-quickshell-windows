#pragma once

#include <qt_windows.h>

namespace qs::windows {

// Shared by the native tracker and its fake-desktop refresh boundary.
struct WindowStyleSnapshot {
	bool initialized = false;
	LONG_PTR style = 0;
	LONG_PTR exStyle = 0;

	bool update(LONG_PTR nextStyle, LONG_PTR nextExStyle) {
		const auto changed = this->initialized
		                  && (this->style != nextStyle || this->exStyle != nextExStyle);
		this->initialized = true;
		this->style = nextStyle;
		this->exStyle = nextExStyle;
		return changed;
	}
};

} // namespace qs::windows
