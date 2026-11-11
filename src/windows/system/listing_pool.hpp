#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace qs::windows::sys {

// Two process-wide workers. Each requester has at most one active operation
// and one pending operation, replaced by its latest request.
class ListingPool {
public:
	struct Slot {
		std::function<void()> pending;
		bool scheduled = false;
	};

	ListingPool() {
		for (int i = 0; i < 2; ++i) {
			try {
				std::thread([state = state] {
					for (;;) {
						std::shared_ptr<Slot> slot;
						std::function<void()> work;
						{
							std::unique_lock lock(state->mutex);
							state->ready.wait(lock, [&] { return !state->queue.empty(); });
							slot = state->queue.front().lock();
							state->queue.pop_front();
							if (!slot) continue;
							work = std::move(slot->pending);
						}
						work();
						{
							std::lock_guard lock(state->mutex);
							if (slot->pending) {
								state->queue.push_back(slot);
								state->ready.notify_one();
							} else {
								slot->scheduled = false;
							}
						}
					}
				}).detach();
			} catch (const std::system_error&) {
				// Keep a successfully started worker rather than retrying a
				// partial pool and leaving extra detached workers behind.
				if (i == 0) throw;
				break;
			}
		}
	}

	void submit(const std::shared_ptr<Slot>& slot, std::function<void()> work) {
		std::lock_guard lock(state->mutex);
		slot->pending = std::move(work);
		if (!slot->scheduled) {
			std::erase_if(state->queue, [](const auto& queued) { return queued.expired(); });
			slot->scheduled = true;
			state->queue.push_back(slot);
			state->ready.notify_one();
		}
	}

private:
	struct State {
		std::mutex mutex;
		std::condition_variable ready;
		std::deque<std::weak_ptr<Slot>> queue;
	};

	// Detached workers own the state. Destruction never joins a blocked share.
	std::shared_ptr<State> state = std::make_shared<State>();
};

} // namespace qs::windows::sys
