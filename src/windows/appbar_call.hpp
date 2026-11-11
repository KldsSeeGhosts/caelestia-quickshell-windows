#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <qt_windows.h>
#include <shellapi.h>

#include "util.hpp"

namespace qs::windows {

// SHAppBarMessage has no timeout. Keep its input/output on the worker, never
// join it, and allow only one outstanding call per shell window. A replacement
// Explorer can proceed without waiting for the old one. The global cap also
// bounds resource use if several successive Explorer processes hang.
class AppBarChannel {
public:
	using Operation = std::function<UINT_PTR(DWORD, APPBARDATA*)>;
	static constexpr auto TIMEOUT = std::chrono::milliseconds(200);
	static constexpr size_t MAX_PENDING = 4;

	std::optional<UINT_PTR> call(HWND shell, DWORD message, APPBARDATA& data, Operation operation) {
		DWORD pid = 0;
		auto tid = shell ? GetWindowThreadProcessId(shell, &pid) : 0;
		if (!tid || IsHungAppWindow(shell)) return std::nullopt;

		auto job = std::make_shared<Job>();
		job->finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!job->finished) return std::nullopt;
		job->shell = shell;
		job->pid = pid;
		job->tid = tid;
		job->data = data;
		{
			std::lock_guard lock(this->mutex);
			std::erase_if(this->pending, [](const auto& item) {
				std::lock_guard lock(item->mutex);
				return item->done;
			});
			if (this->pending.size() >= MAX_PENDING) return std::nullopt;
			for (const auto& item: this->pending) {
				if (item->shell == shell && item->pid == pid && item->tid == tid)
					return std::nullopt;
			}
			this->pending.push_back(job);
		}

		auto run = [job, message, operation = std::move(operation)] {
			// Do not publish partially written output if an injected operation
			// throws. No worker exception may terminate the shell.
			std::optional<UINT_PTR> result;
			try {
				result = operation(message, &job->data);
			} catch (...) {}
			{
				std::lock_guard lock(job->mutex);
				job->result = result;
				job->done = true;
			}
			SetEvent(job->finished);
		};
		try {
			std::thread(std::move(run)).detach();
		} catch (...) {
			std::lock_guard lock(job->mutex);
			job->done = true;
			return std::nullopt;
		}

		// AppBar operations can synchronously notify the registered GUI
		// window. Dispatch sent messages, as the old GUI-thread SendMessage
		// did, but never consume posted Qt events or extend the deadline.
		auto deadline = GetTickCount64() + static_cast<ULONGLONG>(TIMEOUT.count());
		for (;;) {
			auto now = GetTickCount64();
			if (now >= deadline) return std::nullopt;
			auto status = MsgWaitForMultipleObjectsEx(
			    1, &job->finished, static_cast<DWORD>(deadline - now),
			    QS_SENDMESSAGE, MWMO_INPUTAVAILABLE
			);
			if (status == WAIT_OBJECT_0) break;
			if (status != WAIT_OBJECT_0 + 1) return std::nullopt;
			MSG message {};
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
		std::lock_guard lock(job->mutex);
		if (!job->result) return std::nullopt;
		data = job->data;
		return job->result;
	}

private:
	struct Job {
		~Job() {
			if (this->finished) CloseHandle(this->finished);
		}
		HWND shell = nullptr;
		DWORD pid = 0;
		DWORD tid = 0;
		APPBARDATA data {};
		std::mutex mutex;
		HANDLE finished = nullptr;
		bool done = false;
		std::optional<UINT_PTR> result;
	};
	std::mutex mutex;
	std::vector<std::shared_ptr<Job>> pending;
};

inline std::optional<UINT_PTR> boundedAppBarMessage(DWORD message, APPBARDATA& data) {
	static AppBarChannel channel;
	return channel.call(explorerTaskbarWindow(), message, data, [](DWORD message, APPBARDATA* data) {
		// Yield on the worker too: moving the mirror window synchronously can
		// itself wait for its owner thread. The caller's deadline covers both.
		auto yield = TrayHookYield();
		return SHAppBarMessage(message, data);
	});
}

} // namespace qs::windows
