#pragma once

namespace qs::windows::panel_topmost {

// Never undo an intentional fullscreen lowering.
constexpr bool needsRestore(bool expected, bool topmost, bool covered = false) {
	return expected && (!topmost || covered);
}

} // namespace qs::windows::panel_topmost
