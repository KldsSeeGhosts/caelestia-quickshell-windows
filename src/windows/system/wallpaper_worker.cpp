#include "wallpaper_worker.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <objbase.h>
#include <qhash.h>
#include <qpointer.h>
#include <qscopedvaluerollback.h>
#include <qtimer.h>

namespace qs::windows::sys::wallpaper_worker {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto guiBound = std::chrono::milliseconds(100);
constexpr auto stuckBound = std::chrono::milliseconds(5000);
constexpr int workerLimit = 4;

std::atomic<int>& liveWorkers() {
	static auto* count = new std::atomic<int>(0);
	return *count;
}

bool mutation(Kind kind) { return kind != Kind::Read && kind != Kind::ReadScreen; }

struct Job {
	Request request;
	// GUI-owned. A failure before the bool command returns is reported by
	// that return value, not by a second failure notification.
	bool synchronousWait = false;
	std::promise<Result> promise;
	std::shared_future<Result> result = promise.get_future().share();
	HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	Clock::time_point submitted = Clock::now();
	std::atomic<Clock::duration::rep> started = 0;
	~Job() {
		if (this->event) CloseHandle(this->event);
	}
	bool ready() const {
		return this->result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
	}
	bool stalled(std::chrono::milliseconds bound) const {
		const auto ticks = this->started.load();
		const auto since = ticks ? Clock::time_point(Clock::duration(ticks)) : this->submitted;
		return !this->ready() && Clock::now() - since >= bound;
	}
};

struct Worker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<Job>> queue;
	bool stopping = false;

	void stop() {
		{
			std::lock_guard lock(this->mutex);
			this->stopping = true;
			this->queue.clear();
		}
		this->wake.notify_one();
	}
	void submit(const std::shared_ptr<Job>& job) {
		{
			std::lock_guard lock(this->mutex);
			this->queue.push_back(job);
		}
		this->wake.notify_one();
	}
	void run(const Factory& factory) {
		const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		std::unique_ptr<Provider> provider;
		for (;;) {
			std::shared_ptr<Job> job;
			{
				std::unique_lock lock(this->mutex);
				this->wake.wait(lock, [&] { return this->stopping || !this->queue.empty(); });
				if (this->stopping) break;
				job = std::move(this->queue.front());
				this->queue.pop_front();
			}
			job->started.store(Clock::now().time_since_epoch().count());
			Result result;
			try {
				if (SUCCEEDED(apartment)) {
					if (!provider) provider = factory();
					if (provider) result = provider->execute(job->request);
				}
			} catch (...) {
				result.disconnected = true;
			}
			job->promise.set_value(std::move(result));
			SetEvent(job->event);
		}
		// Neither Release nor CoUninitialize may run on the GUI or be joined.
		provider.reset();
		if (SUCCEEDED(apartment)) CoUninitialize();
		--liveWorkers();
	}
};
} // namespace

struct Channel::Impl {
	Channel* owner;
	Factory factory;
	QPointer<QObject> context;
	QTimer poll;
	std::shared_ptr<Worker> worker;
	std::deque<std::shared_ptr<Job>> pending;
	std::optional<Request> latestBackground;
	QHash<QString, QString> cache;
	std::chrono::milliseconds stallLimit = stuckBound;
	Clock::time_point retryAt {};
	int backoff = 250;
	bool stopped = false;
	bool waiting = false;
	bool notifying = false;
	bool recovering = false;

	Impl(Channel* owner, Factory factory, QObject* context)
	    : owner(owner)
	    , factory(std::move(factory))
	    , context(context) {
		this->poll.setInterval(10);
		QObject::connect(&this->poll, &QTimer::timeout, context, [this] { this->collect(); });
	}
	~Impl() { this->stop(); }

	void notify(const Request& request, bool ok, const QString& reason) {
		if (mutation(request.kind) && this->owner->finished) {
			QScopedValueRollback guard(this->notifying, true);
			this->owner->finished(request, ok, reason);
		}
	}
	void stop() {
		this->stopped = true;
		this->poll.stop();
		this->latestBackground.reset();
		this->pending.clear();
		if (this->worker) this->worker->stop();
		this->worker.reset();
	}
	void retire(const QString& reason) {
		if (this->stopped) return;
		if (this->worker) this->worker->stop();
		this->worker.reset();
		auto old = std::move(this->pending);
		this->pending.clear();
		// Never replay mutations from an abandoned generation.
		if (this->latestBackground) {
			old.push_back(std::make_shared<Job>());
			old.back()->request = std::move(*this->latestBackground);
			this->latestBackground.reset();
		}
		this->recovering = true;
		this->retryAt = Clock::now() + std::chrono::milliseconds(this->backoff);
		this->poll.setInterval(this->backoff);
		this->backoff = std::min(this->backoff * 2, 4000);
		this->poll.start();
		for (const auto& job: old)
			if (!job->synchronousWait) this->notify(job->request, false, reason);
	}
	bool acquire() {
		if (this->stopped || !this->context) return false;
		if (this->worker) return true;
		if (Clock::now() < this->retryAt) return false;
		auto count = liveWorkers().load();
		do {
			if (count >= workerLimit) return false;
		} while (!liveWorkers().compare_exchange_weak(count, count + 1));
		try {
			auto next = std::make_shared<Worker>();
			std::thread([next, factory = this->factory] { next->run(factory); }).detach();
			this->worker = std::move(next);
		} catch (...) {
			--liveWorkers();
			return false;
		}
		return true;
	}
	std::shared_ptr<Job> submit(Request request) {
		if (!this->acquire()) return {};
		auto job = std::make_shared<Job>();
		if (!job->event) return {};
		job->request = std::move(request);
		this->pending.push_back(job);
		this->worker->submit(job);
		this->poll.setInterval(10);
		this->poll.start();
		return job;
	}
	void apply(const Request& request, const Result& result, bool report) {
		if (result.ok) {
			if (request.kind == Kind::Read || request.kind == Kind::ReadScreen)
				this->cache.insert(request.target, result.path);
			else if (request.kind == Kind::Set || request.kind == Kind::Background) {
				this->cache.clear();
				this->cache.insert({}, request.path);
			} else if (request.kind == Kind::SetScreen) this->cache.insert(request.target, request.path);
			this->backoff = 250;
			this->recovering = false;
		}
		if (report)
			this->notify(request, result.ok, result.ok ? QString() : QStringLiteral("provider-refused"));
	}
	void collect() {
		if (this->stopped || this->waiting || this->notifying) return;
		while (!this->pending.empty() && this->pending.front()->ready()) {
			auto job = this->pending.front();
			this->pending.pop_front();
			const auto result = job->result.get();
			if (result.disconnected) {
				this->pending.push_front(job);
				this->retire(QStringLiteral("provider-disconnected"));
				return;
			}
			if (this->recovering && !result.ok && !mutation(job->request.kind)) {
				this->retire(QStringLiteral("provider-unavailable"));
				return;
			}
			this->apply(job->request, result, !job->synchronousWait || result.ok);
		}
		for (const auto& job: this->pending) {
			if (job->stalled(this->stallLimit)) {
				this->retire(QStringLiteral("provider-timeout"));
				return;
			}
		}
		if (this->pending.empty() && this->latestBackground) {
			auto request = std::move(*this->latestBackground);
			this->latestBackground.reset();
			if (!this->submit(request))
				this->notify(request, false, QStringLiteral("provider-unavailable"));
		}
		if (this->pending.empty() && this->recovering && Clock::now() >= this->retryAt) {
			if (!this->submit(Request {})) {
				this->retryAt = Clock::now() + std::chrono::milliseconds(250);
				this->poll.setInterval(250);
			}
		}
		if (this->pending.empty() && !this->recovering) this->poll.stop();
	}
	void wait(const std::shared_ptr<Job>& job) {
		if (!job) return;
		this->waiting = true;
		const auto generation = this->worker;
		const auto deadline = job->submitted + guiBound;
		while (generation == this->worker && !job->ready()) {
			const auto remaining = deadline - Clock::now();
			if (remaining <= Clock::duration::zero()) break;
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
			const auto status = MsgWaitForMultipleObjectsEx(
			    1,
			    &job->event,
			    static_cast<DWORD>(std::min<qint64>(ms + 1, 10)),
			    QS_SENDMESSAGE,
			    MWMO_INPUTAVAILABLE
			);
			if (status == WAIT_OBJECT_0) break;
			if (status == WAIT_TIMEOUT) continue;
			if (status != WAIT_OBJECT_0 + 1) break;
			MSG message {};
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
		this->waiting = false;
		this->collect();
	}
};

Channel::Channel(Factory factory, QObject* context)
    : impl(std::make_unique<Impl>(this, std::move(factory), context)) {}
Channel::~Channel() = default;

QString Channel::read(Request request) {
	auto& p = *this->impl;
	p.collect();
	if (!p.waiting && !p.notifying && p.pending.empty()) {
		auto job = p.submit(request);
		p.wait(job);
	}
	return p.cache.value(request.target);
}

bool Channel::command(Request request) {
	auto& p = *this->impl;
	p.collect();
	if (p.waiting || p.notifying || p.stopped) return false;
	if (p.pending.size() >= 2) return false;
	for (const auto& job: p.pending)
		if (mutation(job->request.kind) && job->request.kind != Kind::Background) return false;
	// A newer explicit global pick must not be undone by an older deferred
	// background pick once this command completes.
	if (request.kind == Kind::Set && p.latestBackground) {
		auto superseded = std::move(*p.latestBackground);
		p.latestBackground.reset();
		p.notify(superseded, false, QStringLiteral("superseded"));
	}
	auto job = p.submit(std::move(request));
	if (!job) return false;
	job->synchronousWait = true;
	const auto generation = p.worker;
	p.wait(job);
	// Capture readiness once. A completion racing the GUI deadline must either
	// be collected as synchronous failure, or belong to an accepted request.
	const auto completed = job->ready();
	if (completed) p.collect();
	job->synchronousWait = false;
	// The short GUI bound never rejects a request that is still owned by a
	// live generation. Its eventual failure or success uses finished.
	return generation == p.worker && (!completed || job->result.get().ok);
}

void Channel::background(Request request) {
	auto& p = *this->impl;
	if (p.stopped) return;
	auto superseded = std::move(p.latestBackground);
	p.latestBackground = std::move(request);
	if (superseded) p.notify(*superseded, false, QStringLiteral("superseded"));
	p.collect();
}
void Channel::invalidate() { this->impl->retire(QStringLiteral("explorer-restarted")); }
void Channel::shutdown() { this->impl->stop(); }

#ifdef QS_TEST
void Channel::setStuckBound(std::chrono::milliseconds bound) { this->impl->stallLimit = bound; }
int Channel::liveCount() { return liveWorkers().load(); }
#endif
} // namespace qs::windows::sys::wallpaper_worker
