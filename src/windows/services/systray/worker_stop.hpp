#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <qt_windows.h>

namespace qs::windows::services::systray {

inline constexpr auto TRAY_WORKER_STOP_TIMEOUT = std::chrono::milliseconds(1000);

enum class WorkerStopResult {
	NotRunning,
	Joined,
	Detached,
};

// Worker callbacks run under this gate. stop() first deactivates the gate, so
// once it returns a detached worker can no longer reach its owner's callback.
class CallbackGate {
public:
	using Ptr = std::shared_ptr<CallbackGate>;

	template <typename Callback>
	bool invoke(Callback&& callback) {
		const auto lock = std::lock_guard(this->mutex);
		if (!this->active) return false;
		std::invoke(std::forward<Callback>(callback));
		return true;
	}

	void deactivate() {
		const auto lock = std::lock_guard(this->mutex);
		this->active = false;
	}

private:
	std::mutex mutex;
	bool active = true;
};

// A wedged Win32 or COM call cannot be cancelled. Detach only after the
// deadline; callers must keep every object the thread can reach alive through
// shared ownership instead of capturing their own `this`.
inline WorkerStopResult stopWorker(std::thread& worker) {
	if (!worker.joinable()) return WorkerStopResult::NotRunning;

	auto timeout = static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(
	    TRAY_WORKER_STOP_TIMEOUT.count(),
	    static_cast<std::chrono::milliseconds::rep>(MAXDWORD)
	));
	auto* handle = reinterpret_cast<HANDLE>(worker.native_handle());

	if (WaitForSingleObject(handle, timeout) == WAIT_OBJECT_0) {
		worker.join();
		return WorkerStopResult::Joined;
	}

	worker.detach();
	return WorkerStopResult::Detached;
}

} // namespace qs::windows::services::systray
