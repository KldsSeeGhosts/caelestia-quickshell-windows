#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <qstring.h>
#include <qt_windows.h>

namespace qs::windows::sys {

struct DisplayTopology {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

// Mode indices and path ordering can change on a new QueryDisplayConfig.
// Compare the identities, geometry and target timing, not those array offsets.
inline bool sameDisplayTopology(const DisplayTopology& expected, const DisplayTopology& actual) {
	const auto luidEqual = [](const LUID& a, const LUID& b) {
		return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
	};
	const auto rateEqual = [](const DISPLAYCONFIG_RATIONAL& a, const DISPLAYCONFIG_RATIONAL& b) {
		if (a.Denominator == 0 || b.Denominator == 0)
			return a.Numerator == b.Numerator && a.Denominator == b.Denominator;
		return static_cast<std::uint64_t>(a.Numerator) * b.Denominator
		    == static_cast<std::uint64_t>(b.Numerator) * a.Denominator;
	};
	const auto modeAt = [](const DisplayTopology& topology, UINT32 index) {
		return index < topology.modes.size() ? &topology.modes[index] : nullptr;
	};
	if (expected.paths.size() != actual.paths.size() || expected.paths.empty()) return false;
	for (const auto& path: expected.paths) {
		const auto found = std::ranges::find_if(actual.paths, [&](const auto& other) {
			return luidEqual(path.sourceInfo.adapterId, other.sourceInfo.adapterId)
			    && path.sourceInfo.id == other.sourceInfo.id
			    && luidEqual(path.targetInfo.adapterId, other.targetInfo.adapterId)
			    && path.targetInfo.id == other.targetInfo.id;
		});
		if (found == actual.paths.end()) return false;
		const auto& target = path.targetInfo;
		const auto& otherTarget = found->targetInfo;
		if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != (found->flags & DISPLAYCONFIG_PATH_ACTIVE)
		    || target.rotation != otherTarget.rotation || target.scaling != otherTarget.scaling
		    || target.scanLineOrdering != otherTarget.scanLineOrdering
		    || !rateEqual(target.refreshRate, otherTarget.refreshRate))
			return false;
		const auto* sourceMode = modeAt(expected, path.sourceInfo.modeInfoIdx);
		const auto* otherSourceMode = modeAt(actual, found->sourceInfo.modeInfoIdx);
		if (!sourceMode || !otherSourceMode
		    || sourceMode->infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE
		    || otherSourceMode->infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
			return false;
		const auto& source = sourceMode->sourceMode;
		const auto& otherSource = otherSourceMode->sourceMode;
		if (source.width != otherSource.width || source.height != otherSource.height
		    || source.pixelFormat != otherSource.pixelFormat
		    || source.position.x != otherSource.position.x
		    || source.position.y != otherSource.position.y)
			return false;
		const auto* targetMode = modeAt(expected, target.modeInfoIdx);
		const auto* otherTargetMode = modeAt(actual, otherTarget.modeInfoIdx);
		if ((targetMode == nullptr) != (otherTargetMode == nullptr)) return false;
		if (targetMode) {
			if (targetMode->infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET
			    || otherTargetMode->infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
				return false;
			const auto& signal = targetMode->targetMode.targetVideoSignalInfo;
			const auto& otherSignal = otherTargetMode->targetMode.targetVideoSignalInfo;
			if (signal.pixelRate != otherSignal.pixelRate
			    || signal.activeSize.cx != otherSignal.activeSize.cx
			    || signal.activeSize.cy != otherSignal.activeSize.cy
			    || signal.totalSize.cx != otherSignal.totalSize.cx
			    || signal.totalSize.cy != otherSignal.totalSize.cy
			    || signal.scanLineOrdering != otherSignal.scanLineOrdering
			    || !rateEqual(signal.vSyncFreq, otherSignal.vSyncFreq)
			    || !rateEqual(signal.hSyncFreq, otherSignal.hSyncFreq))
				return false;
		}
	}
	return true;
}

// The same write/read-back boundary is used by Windows and synthetic providers.
// No "allow changes" or database-current fallback can stand in for the baseline.
template <typename Write, typename Query>
QString restoreDisplayTopology(DisplayTopology baseline, Write&& write, Query&& query) {
	auto supplied = baseline;
	auto error = write(supplied);
	if (!error.isEmpty()) return error;
	DisplayTopology observed;
	if (!query(observed) || !sameDisplayTopology(baseline, observed))
		return QStringLiteral("Restoring the previous display layout could not be confirmed");
	return {};
}

// No QObject, facade pointer or event-loop completion is owned by the worker.
// The worker holds this transaction through apply, confirmation and recovery.
class DisplayTransaction {
public:
	using Clock = std::chrono::steady_clock;
	using Job = std::function<QString()>;
	using Lease = std::uint64_t;
	struct Operations {
		std::function<bool(DisplayTopology&)> snapshot;
		std::function<QString(DisplayTopology)> apply;
		std::function<QString(DisplayTopology)> restore;
		Job save;
	};
	struct State {
		bool busy = false;
		bool pending = false;
		int secondsLeft = 0;
		QString error;
		std::uint64_t revision = 0;
		std::optional<Clock::time_point> deadline;
	};

	static std::shared_ptr<DisplayTransaction>
	create(Operations operations, Clock::duration timeout = std::chrono::seconds(15)) {
		auto owner =
		    std::shared_ptr<DisplayTransaction>(new DisplayTransaction(std::move(operations), timeout));
		std::thread([owner] { owner->work(); }).detach();
		return owner;
	}

	Lease acquire() {
		std::lock_guard lock(this->mutex);
		// A replacement is a new lease, not permission to keep an older preview.
		if (this->lease != 0) this->cancel();
		this->lease = ++this->generation;
		this->wake.notify_all();
		return this->lease;
	}

	void release(Lease token) {
		std::lock_guard lock(this->mutex);
		if (token != this->lease) return;
		this->lease = 0;
		this->cancel();
		this->wake.notify_all();
	}

	bool submit(Lease token, Job job, bool confirm, QString& rejection) {
		std::unique_lock lock(this->mutex);
		rejection = QStringLiteral("Another display change is still running");
		if (!this->writable(token) || this->expired() || this->cancelled) return false;
		if (confirm && !this->previous) {
			// Preserve the public bool contract: snapshot refusal is immediate.
			// Reserve the writer, but never hold its mutex across a native call.
			this->preparing = true;
			this->running = true;
			lock.unlock();
			DisplayTopology baseline;
			const bool captured = this->operations.snapshot(baseline) && !baseline.paths.empty();
			lock.lock();
			this->preparing = false;
			this->running = false;
			this->wake.notify_all();
			if (!captured) {
				rejection = QStringLiteral("Couldn't read the current display layout");
				this->error = rejection;
				this->clear();
				++this->revision;
				return false;
			}
			if (token != this->lease || this->stopping || this->cancelled) {
				// There has been no native write, so no restoration is necessary.
				this->clear();
				return false;
			}
			this->previous = std::move(baseline);
		}
		this->error.clear();
		this->task = Task {Kind::Apply, std::move(job)};
		this->running = true;
		++this->revision;
		this->wake.notify_all();
		return true;
	}

	bool keep(Lease token) {
		std::lock_guard lock(this->mutex);
		if (!this->writable(token) || !this->previous || this->cancelled || this->expired())
			return false;
		this->error.clear();
		this->task = Task {Kind::Save, {}};
		this->running = true;
		++this->revision;
		this->wake.notify_all();
		return true;
	}

	bool revert(Lease token) {
		std::lock_guard lock(this->mutex);
		if (!this->writable(token) || !this->previous) return false;
		// Explicit recovery can retry a failed restore. Automatic recovery never loops.
		this->cancelled = true;
		this->restoreRequested = true;
		this->running = true;
		this->error.clear();
		++this->revision;
		this->wake.notify_all();
		return true;
	}

	[[nodiscard]] State state() const {
		std::lock_guard lock(this->mutex);
		int seconds = 0;
		if (this->previous && this->deadline) {
			seconds = static_cast<int>(std::max<std::int64_t>(
			    0,
			    std::chrono::ceil<std::chrono::seconds>(*this->deadline - Clock::now()).count()
			));
		}
		return {
		    this->running,
		    this->previous.has_value(),
		    seconds,
		    this->error,
		    this->revision,
		    this->deadline,
		};
	}

	QString apply(DisplayTopology topology) const {
		return this->operations.apply(std::move(topology));
	}

	// At most budget on the caller (including aboutToQuit), never an unbounded join.
	// A native call cannot be safely cancelled. If it outlives the budget it still
	// owns recovery and will restore when it returns, provided the process survives.
	bool shutdown(Clock::duration budget = std::chrono::milliseconds(250)) {
		std::unique_lock lock(this->mutex);
		this->stopping = true;
		this->lease = 0;
		this->cancel();
		this->wake.notify_all();
		return this->wake.wait_for(lock, budget, [this] { return this->stopped; }) && !this->previous;
	}

private:
	enum class Kind { Apply, Save, Restore };
	struct Task {
		Kind kind;
		Job job;
	};
	DisplayTransaction(Operations operations, Clock::duration timeout)
	    : operations(std::move(operations))
	    , timeout(timeout) {}

	bool writable(Lease token) const {
		return token != 0 && token == this->lease && !this->stopping && !this->running;
	}
	bool expired() const { return this->deadline && Clock::now() >= *this->deadline; }
	void cancel() {
		if (this->cancelled) return;
		this->cancelled = true;
		if (this->previous || this->running) {
			this->restoreRequested = true;
			this->running = true;
		}
		// A lease with no transaction must not poison the next lease.
		else
			this->cancelled = false;
	}
	void clear() {
		this->previous.reset();
		this->deadline.reset();
		this->cancelled = false;
		this->restoreRequested = false;
	}

	void work() {
		SetThreadDescription(GetCurrentThread(), L"qs display transaction");
		std::unique_lock lock(this->mutex);
		for (;;) {
			if (this->preparing) {
				this->wake.wait(lock);
				continue;
			}
			if (this->expired()) this->cancel();
			if (!this->task && !this->restoreRequested) {
				this->running = false;
				if (this->stopping) {
					this->stopped = true;
					this->wake.notify_all();
					return;
				}
				if (this->deadline && !this->cancelled) this->wake.wait_until(lock, *this->deadline);
				else this->wake.wait(lock);
				continue;
			}

			auto next = this->task ? std::move(*this->task) : Task {Kind::Restore, {}};
			this->task.reset();
			if (next.kind == Kind::Restore) {
				this->restoreRequested = false;
				this->deadline.reset();
				if (!this->previous) {
					this->cancelled = false;
					continue;
				}
			}
			this->running = true;
			auto baseline = this->previous;
			const bool skipApply = next.kind == Kind::Apply && (this->cancelled || this->stopping);
			lock.unlock();

			QString result;
			if (next.kind == Kind::Apply && !skipApply) result = next.job();
			else if (next.kind == Kind::Save) result = this->operations.save();
			else if (next.kind == Kind::Restore) result = this->operations.restore(*baseline);

			lock.lock();
			if (!result.isEmpty()) this->error = result;
			if (next.kind == Kind::Restore) {
				if (result.isEmpty()) this->clear();
				// Failure retains the exact baseline, unconfirmed, for explicit retry.
			} else if (next.kind == Kind::Save) {
				if (result.isEmpty() && !this->cancelled && !this->stopping && !this->expired())
					this->clear();
			} else if (this->previous) {
				if (!result.isEmpty() || skipApply) this->cancel();
				if (!this->deadline && !this->cancelled) this->deadline = Clock::now() + this->timeout;
			}
			if (this->expired()) this->cancel();
			++this->revision;
			this->running = this->task.has_value() || this->restoreRequested;
			this->wake.notify_all();
		}
	}

	const Operations operations;
	const Clock::duration timeout;
	mutable std::mutex mutex;
	std::condition_variable wake;
	std::optional<DisplayTopology> previous;
	std::optional<Clock::time_point> deadline;
	std::optional<Task> task;
	Lease generation = 0;
	Lease lease = 0;
	std::uint64_t revision = 0;
	QString error;
	bool running = false;
	bool preparing = false;
	bool cancelled = false;
	bool restoreRequested = false;
	bool stopping = false;
	bool stopped = false;
};

} // namespace qs::windows::sys
