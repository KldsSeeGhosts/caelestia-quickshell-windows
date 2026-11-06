#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <qt_windows.h>
#include <qjsonobject.h>
#include <qstring.h>

namespace qs::windows::transition_trace {

struct Record {
	const char* event = nullptr;
	QJsonObject fields;
	qint64 ticks = 0;
};

// Producers never wait for the worker or for another producer. The worker
// owns all sink I/O; its shared state outlives a timed-out shutdown.
class Queue {
	struct State {
		std::mutex mutex;
		std::condition_variable wake;
		std::vector<Record> records;
		size_t head = 0;
		size_t size = 0;
		std::atomic<quint64> dropped {0};
		std::atomic<bool> stopping {false};
		std::mutex doneMutex;
		std::condition_variable doneWake;
		bool done = false;
		std::function<void(Record)> sink;
	};

public:
	explicit Queue(size_t capacity, std::function<void(Record)> sink):
	    state(std::make_shared<State>()) {
		this->state->records.resize(capacity);
		this->state->sink = std::move(sink);
		this->worker = std::thread([state = this->state] { run(state); });
	}
	Queue(const Queue&) = delete;
	Queue& operator=(const Queue&) = delete;

	~Queue() { this->stop(std::chrono::milliseconds(250)); }

	void push(const char* event, QJsonObject fields) {
		auto& s = *this->state;
		std::unique_lock lock(s.mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			s.dropped.fetch_add(1, std::memory_order_relaxed);
			s.wake.notify_one();
			return;
		}
		if (s.stopping.load(std::memory_order_relaxed) || s.size >= s.records.size()) {
			s.dropped.fetch_add(1, std::memory_order_relaxed);
			lock.unlock();
			s.wake.notify_one();
			return;
		}
		LARGE_INTEGER ticks {};
		QueryPerformanceCounter(&ticks);
		s.records[(s.head + s.size) % s.records.size()] = {event, std::move(fields), ticks.QuadPart};
		++s.size;
		lock.unlock();
		s.wake.notify_one();
	}

	void stop(std::chrono::milliseconds timeout) {
		if (!this->worker.joinable()) return;
		auto& s = *this->state;
		s.stopping.store(true, std::memory_order_relaxed);
		s.wake.notify_one();
		std::unique_lock lock(s.doneMutex);
		s.doneWake.wait_for(lock, timeout, [&s] { return s.done; });
		lock.unlock();
		// The shared state keeps the worker's resources alive if I/O is stuck.
		// Detaching also avoids an unbounded join after the drain deadline.
		this->worker.detach();
	}

private:
	static void run(const std::shared_ptr<State>& s) {
		for (;;) {
			std::unique_lock lock(s->mutex);
			// A drop can happen without taking mutex; a bounded timed wait
			// prevents a notification race from hiding its report indefinitely.
			s->wake.wait_for(lock, std::chrono::milliseconds(50), [&] {
				return s->stopping.load(std::memory_order_relaxed) || s->size != 0
				    || s->dropped.load(std::memory_order_relaxed) != 0;
			});
			if (s->size != 0) {
				auto record = std::move(s->records[s->head]);
				s->head = (s->head + 1) % s->records.size();
				--s->size;
				lock.unlock();
				s->sink(std::move(record));
			} else if (auto count = s->dropped.exchange(0, std::memory_order_relaxed)) {
				LARGE_INTEGER ticks {};
				QueryPerformanceCounter(&ticks);
				Record record {"trace_dropped", {{"count", QString::number(count)}}, ticks.QuadPart};
				lock.unlock();
				s->sink(std::move(record));
			} else if (s->stopping.load(std::memory_order_relaxed)) {
				break;
			}
		}
		{
			std::lock_guard lock(s->doneMutex);
			s->done = true;
		}
		s->doneWake.notify_one();
	}

	std::shared_ptr<State> state;
	std::thread worker;
};
}
