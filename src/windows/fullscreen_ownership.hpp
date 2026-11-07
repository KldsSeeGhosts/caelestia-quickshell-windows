#pragma once

#include <qt_windows.h>

namespace qs::windows::fullscreen_ownership {

struct Identity {
	DWORD process = 0;
	DWORD thread = 0;
	FILETIME created {};

	bool operator==(const Identity& other) const {
		return this->process == other.process && this->thread == other.thread
		    && this->created.dwLowDateTime == other.created.dwLowDateTime
		    && this->created.dwHighDateTime == other.created.dwHighDateTime;
	}
};

struct Snapshot {
	Identity identity;
	WINDOWPLACEMENT placement {.length = sizeof(WINDOWPLACEMENT)};
	RECT rect {};
	LONG_PTR style = 0;
	bool visible = false;
	bool minimized = false;
	bool maximized = false;
	bool fullscreen = false;
};

// Every write must revalidate both the process identity and the window-local
// marker. Process identity alone cannot detect HWND reuse within one process.
class Ops {
public:
	virtual ~Ops() = default;
	virtual bool read(HWND hwnd, Snapshot& snapshot) = 0;
	virtual bool mark(HWND hwnd, const Identity& identity, const void* token) = 0;
	virtual bool matches(HWND hwnd, const Identity& identity, const void* token) = 0;
	virtual void unmark(HWND hwnd, const Identity& identity, const void* token) = 0;
	virtual bool style(HWND hwnd, const Identity& identity, const void* token, LONG_PTR value) = 0;
	virtual bool placement(
	    HWND hwnd,
	    const Identity& identity,
	    const void* token,
	    const WINDOWPLACEMENT& value
	) = 0;
	// Null means a frame refresh only, without moving/resizing the window.
	virtual bool frame(HWND hwnd, const Identity& identity, const void* token, const RECT* rect) = 0;
};

enum class Release { NotOwned, Restored, IdentityLost, ApplicationChanged, Failed };

class Owner {
public:
	Owner(Ops& ops, HWND hwnd): ops(ops), hwnd(hwnd) {}
	~Owner() {
		(void) this->release();
		// A failed final restore is not success or a persisted recovery record.
		if (this->active) this->ops.unmark(this->hwnd, this->original.identity, this);
	}
	Owner(const Owner&) = delete;
	Owner& operator=(const Owner&) = delete;
	Owner(Owner&&) = delete;
	Owner& operator=(Owner&&) = delete;

	[[nodiscard]] bool owned() const { return this->active; }

	bool enter(const RECT& rect, bool observedAppFullscreen) {
		if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
		Snapshot current;
		if (!this->ops.read(this->hwnd, current)) return false;
		if (this->active) {
			if (!this->styleApplied || !this->positioned) return false;
			// Retargeting an existing episode must not replace its original
			// placement or clobber intervening application style changes.
			if (!this->ops.matches(this->hwnd, this->original.identity, this)) {
				this->active = false;
				return false;
			}
			if (!this->unchanged(current) || !current.visible || current.minimized) return false;
			if (!this->ops.frame(this->hwnd, this->original.identity, this, &rect)) return false;
			this->ownedRect = rect;
			return true;
		}

		// Never adopt an application's own fullscreen episode, or show a
		// hidden/minimized window just because a custom command named it.
		if (observedAppFullscreen || current.fullscreen || !current.visible || current.minimized)
			return false;
		auto touched = current.style & (WS_CAPTION | WS_THICKFRAME);
		if (touched == 0) return false;
		if (!this->ops.mark(this->hwnd, current.identity, this)) return false;

		auto markedIdentity = current.identity;
		// Bind the saved placement to the marked HWND generation, not to the
		// pre-claim probe. Even same-process recycling before mark() must not
		// make a replacement inherit that earlier placement/style snapshot.
		if (!this->ops.matches(this->hwnd, markedIdentity, this) || !this->ops.read(this->hwnd, current)
		    || !(current.identity == markedIdentity)
		    || !this->ops.matches(this->hwnd, markedIdentity, this) || current.fullscreen
		    || !current.visible || current.minimized)
		{
			this->ops.unmark(this->hwnd, markedIdentity, this);
			return false;
		}
		touched = current.style & (WS_CAPTION | WS_THICKFRAME);
		if (touched == 0) {
			this->ops.unmark(this->hwnd, markedIdentity, this);
			return false;
		}
		this->original = current;
		this->touched = touched;
		this->active = true;
		this->positioned = false;
		this->styleApplied = false;

		if (current.maximized) {
			auto normal = current.placement;
			normal.showCmd = SW_SHOWNOACTIVATE;
			normal.flags &= ~WPF_RESTORETOMAXIMIZED;
			if (!this->ops.placement(this->hwnd, current.identity, this, normal)) {
				(void) this->release();
				return false;
			}
		}
		// Read again after normalization. WM_WINDOWPOSCHANGED handlers can
		// change other style bits; those do not belong to this feature.
		if (!this->ops.matches(this->hwnd, this->original.identity, this)
		    || !this->ops.read(this->hwnd, current) || !this->unchanged(current) || current.fullscreen
		    || !current.visible || current.minimized)
		{
			(void) this->release();
			return false;
		}
		if (!this->ops.style(this->hwnd, this->original.identity, this, current.style & ~touched)) {
			(void) this->release();
			return false;
		}
		this->styleApplied = true;
		auto beforeStyle = current;
		// WM_STYLECHANGED is synchronous and may let the app take over its
		// own fullscreen/placement, hide itself, or destroy the HWND.
		if (!this->ops.matches(this->hwnd, this->original.identity, this)
		    || !this->ops.read(this->hwnd, current) || !this->unchanged(current))
		{
			(void) this->release();
			return false;
		}
		if (current.fullscreen || (!current.minimized && !EqualRect(&beforeStyle.rect, &current.rect)))
		{
			this->forget();
			return false;
		}
		if (!current.visible || current.minimized) {
			(void) this->release();
			return false;
		}
		if (!this->ops.frame(this->hwnd, this->original.identity, this, &rect)) {
			(void) this->release();
			return false;
		}
		this->ownedRect = rect;
		this->positioned = true;
		return true;
	}

	Release release() {
		if (!this->active) return Release::NotOwned;
		if (!this->ops.matches(this->hwnd, this->original.identity, this)) {
			this->active = false;
			return Release::IdentityLost;
		}
		Snapshot current;
		if (!this->ops.read(this->hwnd, current)) return Release::Failed;
		if (!this->unchanged(current)) {
			this->forget();
			return Release::ApplicationChanged;
		}

		if (this->styleApplied) {
			// Restore only the bits we actually removed, not the old whole
			// style (which would overwrite unrelated application changes).
			if (!this->ops
			         .style(this->hwnd, this->original.identity, this, current.style | this->touched))
				return Release::Failed;
			this->styleApplied = false;
		}
		// The restoring style write can also run application callbacks. Use
		// the latest visibility/show state instead of resurrecting a window
		// which hid or minimized itself in that callback.
		if (!this->ops.read(this->hwnd, current)) return Release::Failed;
		if (!this->unchanged(current)) {
			this->forget();
			return Release::ApplicationChanged;
		}
		auto placement = this->original.placement;
		// Tracking removal can follow hide/minimize. Never resurrect the app.
		if (!current.visible) {
			placement.showCmd = SW_HIDE;
		} else if (current.minimized) {
			placement.showCmd = SW_SHOWMINNOACTIVE;
			if (this->original.maximized) placement.flags |= WPF_RESTORETOMAXIMIZED;
		} else if (placement.showCmd == SW_SHOWNORMAL || placement.showCmd == SW_RESTORE) {
			placement.showCmd = SW_SHOWNOACTIVATE;
		}
		if (!this->ops.placement(this->hwnd, this->original.identity, this, placement))
			return Release::Failed;
		// Placement has changed, so a retry must not demand fullscreen bounds.
		this->positioned = false;
		if (!this->ops.frame(this->hwnd, this->original.identity, this, nullptr))
			return Release::Failed;
		this->forget();
		return Release::Restored;
	}

private:
	bool unchanged(const Snapshot& current) const {
		if (!(current.identity == this->original.identity)) return false;
		if ((current.style & this->touched) != (this->styleApplied ? 0 : this->touched)) return false;
		// An application taking over placement ends our episode. A minimized
		// HWND has an iconic rect instead of its normal/fullscreen bounds.
		return !this->positioned || current.minimized
		    || (!current.maximized && EqualRect(&current.rect, &this->ownedRect));
	}

	void forget() {
		this->ops.unmark(this->hwnd, this->original.identity, this);
		this->active = false;
	}

	Ops& ops;
	HWND hwnd;
	Snapshot original;
	LONG_PTR touched = 0;
	RECT ownedRect {};
	bool active = false;
	bool positioned = false;
	bool styleApplied = false;
};

class NativeOps final: public Ops {
public:
	bool read(HWND hwnd, Snapshot& snapshot) override {
		if (IsHungAppWindow(hwnd) || !identity(hwnd, snapshot.identity)) return false;
		SetLastError(ERROR_SUCCESS);
		snapshot.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		if (snapshot.style == 0 && GetLastError() != ERROR_SUCCESS) return false;
		snapshot.placement.length = sizeof(WINDOWPLACEMENT);
		if (!GetWindowPlacement(hwnd, &snapshot.placement) || !GetWindowRect(hwnd, &snapshot.rect))
			return false;
		snapshot.visible = IsWindowVisible(hwnd) != FALSE;
		snapshot.minimized = IsIconic(hwnd) != FALSE;
		snapshot.maximized = IsZoomed(hwnd) != FALSE;
		MONITORINFO monitor {.cbSize = sizeof(MONITORINFO)};
		snapshot.fullscreen =
		    !snapshot.minimized && !snapshot.maximized
		    && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)
		    && EqualRect(&snapshot.rect, &monitor.rcMonitor);
		Identity after;
		return identity(hwnd, after) && after == snapshot.identity;
	}

	bool mark(HWND hwnd, const Identity& expected, const void* token) override {
		Identity current;
		if (!identity(hwnd, current) || !(current == expected) || GetPropW(hwnd, PROPERTY))
			return false;
		if (!SetPropW(hwnd, PROPERTY, reinterpret_cast<HANDLE>(const_cast<void*>(token)))) return false;
		if (this->matches(hwnd, expected, token)) return true;
		this->unmark(hwnd, expected, token);
		return false;
	}

	bool matches(HWND hwnd, const Identity& expected, const void* token) override {
		Identity current;
		return GetPropW(hwnd, PROPERTY) == token && identity(hwnd, current) && current == expected;
	}

	void unmark(HWND hwnd, const Identity& expected, const void* token) override {
		if (this->matches(hwnd, expected, token)) RemovePropW(hwnd, PROPERTY);
	}

	bool style(HWND hwnd, const Identity& expected, const void* token, LONG_PTR value) override {
		if (!this->matches(hwnd, expected, token)) return false;
		SetLastError(ERROR_SUCCESS);
		return SetWindowLongPtrW(hwnd, GWL_STYLE, value) != 0 || GetLastError() == ERROR_SUCCESS;
	}

	bool placement(
	    HWND hwnd,
	    const Identity& expected,
	    const void* token,
	    const WINDOWPLACEMENT& value
	) override {
		auto synchronous = value;
		// This is a call-delivery flag, not permission to queue a restore
		// after the HWND ownership check/marker has expired.
		synchronous.flags &= ~WPF_ASYNCWINDOWPLACEMENT;
		return this->matches(hwnd, expected, token) && SetWindowPlacement(hwnd, &synchronous);
	}

	bool frame(HWND hwnd, const Identity& expected, const void* token, const RECT* rect) override {
		if (!this->matches(hwnd, expected, token)) return false;
		auto flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
		if (rect == nullptr) flags |= SWP_NOMOVE | SWP_NOSIZE;
		// No asynchronous write may outlive the marker or race a later release.
		return SetWindowPos(
		    hwnd,
		    nullptr,
		    rect ? rect->left : 0,
		    rect ? rect->top : 0,
		    rect ? rect->right - rect->left : 0,
		    rect ? rect->bottom - rect->top : 0,
		    flags
		);
	}

private:
	static constexpr auto PROPERTY = L"Quickshell.OptionalFullscreen.Owner";

	static bool identity(HWND hwnd, Identity& result) {
		if (!IsWindow(hwnd)) return false;
		result.thread = GetWindowThreadProcessId(hwnd, &result.process);
		if (!result.thread || !result.process) return false;
		auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.process);
		if (process == nullptr) return false;
		FILETIME exit {}, kernel {}, user {};
		auto ok = GetProcessTimes(process, &result.created, &exit, &kernel, &user) != FALSE;
		CloseHandle(process);
		return ok;
	}
};

} // namespace qs::windows::fullscreen_ownership
