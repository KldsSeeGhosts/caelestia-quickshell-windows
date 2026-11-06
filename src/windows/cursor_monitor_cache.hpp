#pragma once

#include <atomic>

#include <qt_windows.h>

namespace qs::windows {

// Read/written by the mouse hook, invalidated by the GUI topology reconciler.
// Kept independent of the hook so topology reuse can be tested without input.
class CursorMonitorCache {
public:
	[[nodiscard]] bool contains(POINT cursor) const {
		return cursor.x >= this->left.load(std::memory_order_relaxed)
		    && cursor.x < this->right.load(std::memory_order_relaxed)
		    && cursor.y >= this->top.load(std::memory_order_relaxed)
		    && cursor.y < this->bottom.load(std::memory_order_relaxed);
	}

	void update(const RECT& rect) {
		this->left.store(rect.left, std::memory_order_relaxed);
		this->top.store(rect.top, std::memory_order_relaxed);
		this->right.store(rect.right, std::memory_order_relaxed);
		this->bottom.store(rect.bottom, std::memory_order_relaxed);
	}

	void reset() { this->update({}); }

private:
	std::atomic<LONG> left = 0;
	std::atomic<LONG> top = 0;
	std::atomic<LONG> right = 0;
	std::atomic<LONG> bottom = 0;
};

} // namespace qs::windows
