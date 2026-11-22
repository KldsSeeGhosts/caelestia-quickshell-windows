#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "hook.hpp"

namespace qs::windows::services::systray {

enum class ComObserverState { Inactive, Connecting, Live, StubNoOp, Unavailable };

[[nodiscard]] inline const char* comObserverStateName(ComObserverState state) {
	switch (state) {
	case ComObserverState::Inactive: return "inactive";
	case ComObserverState::Connecting: return "connecting";
	case ComObserverState::Live: return "live";
	case ComObserverState::StubNoOp: return "stub-noop";
	case ComObserverState::Unavailable: return "unavailable";
	}
	Q_UNREACHABLE();
}

// Explorer's NOTIFYITEM layout. The strings and icon are borrowed for the
// duration of Notify; the observer copies what the host needs.
struct TrayNotifyItem {
	PWSTR exeName = nullptr;
	PWSTR tip = nullptr;
	HICON icon = nullptr;
	HWND hwnd = nullptr;
	DWORD preference = 0;
	UINT id = 0;
	GUID guid = {};
};

class TrayNotifyBackend {
public:
	using Callback = std::function<void(ULONG event, const TrayNotifyItem* item)>;
	struct Registration {
		HRESULT result;
		ULONG handle = 0;
		bool hasHandle = true;
	};

	virtual ~TrayNotifyBackend() = default;
	// Both calls and every callback run on the observer STA. Unregister must
	// release the callback even after an unsuccessful registration.
	virtual Registration registerCallback(Callback callback) = 0;
	virtual void unregisterCallback() = 0;
};

class TrayNotifyObserver {
public:
	explicit TrayNotifyObserver(std::unique_ptr<TrayNotifyBackend> backend = {});
	~TrayNotifyObserver();
	Q_DISABLE_COPY_MOVE(TrayNotifyObserver);

	// The owning thread calls start, resync and stop. Both callbacks run on
	// the worker and must not wait on the owning thread.
	void start(TrayIconSink sink, std::function<void(ComObserverState)> onState);
	void resync();
	void stop();
	[[nodiscard]] ComObserverState state() const { return this->mState.load(); }
	// Only transport failures that may clear on their own get timed retries.
	[[nodiscard]] bool canRetry() const { return this->retryable.load(); }

private:
	std::unique_ptr<TrayNotifyBackend> backend;
	std::thread worker;
	DWORD threadId = 0;
	std::atomic<bool> stopping = false;
	std::atomic<bool> retryable = false;
	std::atomic<ComObserverState> mState = ComObserverState::Inactive;
};

} // namespace qs::windows::services::systray
