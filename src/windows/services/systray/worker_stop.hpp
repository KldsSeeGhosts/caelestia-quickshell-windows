#pragma once

#include <algorithm>
#include <chrono>
#include <thread>

#include <qt_windows.h>

namespace qs::windows::services::systray {

inline constexpr auto TRAY_WORKER_STOP_TIMEOUT = std::chrono::milliseconds(1000);

enum class WorkerStopResult {
	NotRunning,
	Joined,
	Detached,
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
