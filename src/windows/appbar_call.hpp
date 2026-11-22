#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <qt_windows.h>
#include <shellapi.h>

#include "util.hpp"

namespace qs::windows {

// SHAppBarMessage has no timeout. Reuse a small pool, with one active worker
// per shell owner. A replacement Explorer need not wait for a hung owner.
class AppBarChannel {
public:
	using Operation = std::function<UINT_PTR(DWORD, APPBARDATA*)>;
	static constexpr auto TIMEOUT = std::chrono::milliseconds(200);
	static constexpr size_t MAX_PENDING = 4;

	~AppBarChannel() {
		for (auto& worker: this->workers) {
			if (!worker.thread.joinable()) continue;
			auto& state = *worker.state;
			auto lock = std::unique_lock(state.mutex);
			state.stopping = true;
			state.queue.clear();
			auto active = state.active != nullptr;
			lock.unlock();
			state.ready.notify_one();
			// A stuck shell call cannot be cancelled. Only such an in-flight
			// worker may outlive the channel, with no references to it.
			if (active) worker.thread.detach();
			else worker.thread.join();
		}
	}

	std::optional<UINT_PTR> call(HWND shell, DWORD message, APPBARDATA& data, Operation operation) {
		return this->callWithLateSuccess(shell, message, data, std::move(operation), {});
	}

	std::optional<UINT_PTR> callWithLateSuccess(
	    HWND shell, DWORD message, APPBARDATA& data, Operation operation, Operation lateSuccess
	) {
		if (shell == nullptr || IsHungAppWindow(shell)) return std::nullopt;
		auto job = std::make_shared<Job>();
		job->finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!job->finished) return std::nullopt;
		job->message = message;
		job->data = data;
		job->operation = std::move(operation);
		job->lateSuccess = std::move(lateSuccess);
		if (!this->enqueue(shell, job)) return std::nullopt;

		// AppBar operations can synchronously notify the registered GUI
		// window. Dispatch sent messages, as the old GUI-thread SendMessage
		// did, but never consume posted Qt events or extend the deadline.
		auto deadline = GetTickCount64() + static_cast<ULONGLONG>(TIMEOUT.count());
		for (;;) {
			auto now = GetTickCount64();
			if (now >= deadline) break;
			auto status = MsgWaitForMultipleObjectsEx(
			    1, &job->finished, static_cast<DWORD>(deadline - now),
			    QS_SENDMESSAGE, MWMO_INPUTAVAILABLE
			);
			if (status == WAIT_OBJECT_0) break;
			if (status != WAIT_OBJECT_0 + 1) break;
			MSG message {};
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
		std::lock_guard lock(job->mutex);
		if (!job->done) job->abandoned = true;
		if (!job->result) return std::nullopt;
		data = job->data;
		return job->result;
	}

	bool notify(HWND shell, DWORD message, const APPBARDATA& data, Operation operation) {
		auto job = std::make_shared<Job>();
		job->message = message;
		job->data = data;
		job->operation = std::move(operation);
		return this->enqueue(shell, job);
	}

private:
	struct Job {
		~Job() {
			if (this->finished) CloseHandle(this->finished);
		}
		DWORD message = 0;
		APPBARDATA data {};
		Operation operation;
		Operation lateSuccess;
		std::mutex mutex;
		HANDLE finished = nullptr;
		bool done = false;
		bool abandoned = false;
		std::optional<UINT_PTR> result;
	};
	struct Owner {
		HWND shell;
		DWORD pid;
		DWORD tid;
		bool operator==(const Owner&) const = default;
	};
	struct State {
		std::mutex mutex;
		std::condition_variable ready;
		bool stopping = false;
		Owner owner {};
		std::deque<std::shared_ptr<Job>> queue;
		std::shared_ptr<Job> active;
	};
	struct Worker {
		std::shared_ptr<State> state;
		std::thread thread;
	};

	static void run(const std::shared_ptr<State>& state) {
		auto lock = std::unique_lock(state->mutex);
		for (;;) {
			state->ready.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
			if (state->stopping) return;
			auto job = std::move(state->queue.front());
			state->queue.pop_front();
			state->active = job;
			lock.unlock();
			bool skip = false;
			{
				auto jobLock = std::lock_guard(job->mutex);
				skip = job->abandoned;
			}
			std::optional<UINT_PTR> result;
			DWORD pid = 0;
			auto owner = state->owner;
			if (!skip && GetWindowThreadProcessId(owner.shell, &pid) == owner.tid && pid == owner.pid) {
				try {
					result = job->operation(job->message, &job->data);
				} catch (...) {}
			}
			auto late = false;
			{
				auto jobLock = std::lock_guard(job->mutex);
				late = job->abandoned && result.value_or(0) != 0 && job->lateSuccess;
				job->result = result;
				job->done = true;
			}
			if (late) {
				try {
					job->lateSuccess(job->message, &job->data);
				} catch (...) {}
			}
			lock.lock();
			state->active.reset();
			if (job->finished) SetEvent(job->finished);
		}
	}

	bool enqueue(HWND shell, const std::shared_ptr<Job>& job) {
		Owner owner {shell, 0, 0};
		if (shell == nullptr || !(owner.tid = GetWindowThreadProcessId(shell, &owner.pid))) return false;
		auto lock = std::lock_guard(this->mutex);
		Worker* idle = nullptr;
		for (auto& worker: this->workers) {
			if (!worker.state) {
				if (idle == nullptr) idle = &worker;
				continue;
			}
			auto& state = *worker.state;
			auto stateLock = std::lock_guard(state.mutex);
			if (state.active == nullptr && state.queue.empty()) {
				idle = &worker;
			} else if (state.owner == owner) {
				if (job->finished && state.active && state.active->finished) return false;
				for (const auto& waiting: state.queue) {
					if (job->finished && waiting->finished) return false;
					if (!job->finished && !waiting->finished && waiting->message == job->message
					    && waiting->data.hWnd == job->data.hWnd)
						return true;
				}
				if (!job->finished && state.queue.size() >= 128) return false;
				state.queue.push_back(job);
				state.ready.notify_one();
				return true;
			}
		}
		if (idle == nullptr) return false;
		if (!idle->state) {
			auto state = std::make_shared<State>();
			try {
				idle->thread = std::thread([state] { run(state); });
			} catch (...) {
				return false;
			}
			idle->state = std::move(state);
		}
		auto& state = *idle->state;
		auto stateLock = std::lock_guard(state.mutex);
		state.owner = owner;
		state.queue.push_back(job);
		state.ready.notify_one();
		return true;
	}

	std::mutex mutex;
	std::array<Worker, MAX_PENDING> workers;
};

inline AppBarChannel& appBarChannel() {
	static AppBarChannel channel;
	return channel;
}

inline UINT_PTR sendAppBarMessage(DWORD message, APPBARDATA* data) {
	// Moving the tray mirror may also wait for its owner thread.
	auto yield = TrayHookYield();
	return SHAppBarMessage(message, data);
}

inline AppBarChannel::Operation appBarOperation(HWND shell) {
	return [shell](DWORD message, APPBARDATA* data) -> UINT_PTR {
		if (explorerTaskbarWindow() != shell) return 0;
		return sendAppBarMessage(message, data);
	};
}

inline std::optional<UINT_PTR> boundedAppBarMessage(DWORD message, APPBARDATA& data) {
	auto shell = explorerTaskbarWindow();
	DWORD pid = 0;
	auto tid = shell ? GetWindowThreadProcessId(shell, &pid) : 0;
	AppBarChannel::Operation lateSuccess;
	if (message == ABM_NEW) {
		lateSuccess = [shell, pid, tid](DWORD, APPBARDATA* data) {
			DWORD currentPid = 0;
			if (explorerTaskbarWindow() != shell
			    || GetWindowThreadProcessId(shell, &currentPid) != tid || currentPid != pid)
				return UINT_PTR(0);
			return sendAppBarMessage(ABM_REMOVE, data);
		};
	}
	return appBarChannel().callWithLateSuccess(
	    shell, message, data, appBarOperation(shell), std::move(lateSuccess)
	);
}

} // namespace qs::windows
