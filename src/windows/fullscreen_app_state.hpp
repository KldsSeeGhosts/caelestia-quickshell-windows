#pragma once

#include <qtypes.h>

namespace qs::windows::fullscreen_app {

// Explorer's ABN_FULLSCREENAPP notification says that an application entered
// or left fullscreen, but it carries no HWND. The foreground can be the task
// switcher, Task View, Start/Search or a foreground staging window at that
// moment. Hold any unconfirmed opening notification pending and
// reconcile it when the foreground settles, without ever turning a delayed
// open into a permanent success after Explorer has sent a close.
class State {
public:
	struct OpenResult {
		bool lower = false;
		quint64 generation = 0;
	};

	// Starts a new episode. `defer` is true unless a fullscreen application
	// is confirmed on this monitor. Returns whether to lower now and the generation
	// that retries must carry.
	[[nodiscard]] OpenResult open(bool defer) {
		this->mSeeded = true;
		this->mGeneration++;

		if (defer) {
			this->mPending = true;
			return {.lower = false, .generation = this->mGeneration};
		}

		this->mPending = false;
		this->mLowered = true;
		return {.lower = true, .generation = this->mGeneration};
	}

	// A close always raises, cancels a pending open and makes every older
	// retry stale.
	void close() {
		this->mSeeded = true;
		this->mGeneration++;
		this->mPending = false;
		this->mLowered = false;
	}

	// A foreground change or bounded retry. `generation` must be the current
	// episode; `honor` is true only when the settled foreground is a
	// monitor-covering application. A pending open that is not yet honored
	// stays pending and only a close cancels it.
	bool reconcile(quint64 generation, bool honor) {
		if (generation != this->mGeneration || !this->mPending || !honor) return false;

		this->mPending = false;
		this->mLowered = true;
		return true;
	}

	// A settled normal foreground application proves that the shell should
	// be above it, even if Explorer missed the matching close notification.
	// Unknown/system foregrounds must be filtered out by the caller.
	bool settle(quint64 generation, bool fullscreen) {
		if (generation != this->mGeneration) return false;
		if (fullscreen) return this->reconcile(generation, true);
		if (!this->mLowered) return false;
		this->close();
		return true;
	}

	[[nodiscard]] bool pending() const { return this->mPending; }
	[[nodiscard]] bool lowered() const { return this->mLowered; }
	[[nodiscard]] quint64 generation() const { return this->mGeneration; }

	// Registration can happen during fullscreen. Seed each monitor once,
	// without overriding an opening/closing callback already received there.
	void seed(bool active) {
		if (this->mSeeded) return;
		this->mSeeded = true;
		this->mLowered = active;
	}

private:
	bool mSeeded = false;
	bool mPending = false;
	bool mLowered = false;
	quint64 mGeneration = 0;
};

} // namespace qs::windows::fullscreen_app
