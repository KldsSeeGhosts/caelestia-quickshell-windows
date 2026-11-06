#pragma once

#include <algorithm>
#include <functional>

#include <qelapsedtimer.h>
#include <qobject.h>
#include <qtimer.h>

namespace qs::windows {

// All callbacks run on the owning COM thread. No sleeping or nested event loop.
class DesktopRecovery: public QObject {
public:
	explicit DesktopRecovery(
	    std::function<bool()> acquire,
	    std::function<void()> recovered,
	    std::function<qint64()> age = {}
	)
	    : acquire(std::move(acquire))
	    , recovered(std::move(recovered))
	    , age(std::move(age)) {
		this->timer.setSingleShot(true);
		QObject::connect(&this->timer, &QTimer::timeout, this, [this] { this->attempt(); });
	}

	void disconnected() {
		if (this->recovering && this->elapsed.isValid()) return;
		this->recovering = true;
		this->elapsed.start();
		this->delay = 250;
		this->schedule();
	}

	// Once the bounded automatic window expires, a dispatch starts a new one.
	void retryOnDispatch() {
		if (this->recovering && !this->timer.isActive()) {
			this->elapsed.start();
			this->delay = 250;
			this->attempt();
		}
	}

	void attempt() {
		if (!this->recovering) return;
		this->timer.stop();
		if (this->acquire()) {
			this->timer.stop();
			this->recovering = false;
			this->recovered();
		} else {
			this->schedule();
		}
	}

	void reset() {
		this->timer.stop();
		this->recovering = false;
	}
	[[nodiscard]] int pendingDelay() const {
		return this->timer.isActive() ? this->timer.interval() : -1;
	}

	[[nodiscard]] bool active() const { return this->recovering; }

private:
	void schedule() {
		const auto remaining = 30000 - (this->age ? this->age() : this->elapsed.elapsed());
		if (remaining <= 0) {
			this->elapsed.invalidate();
			return;
		}
		this->timer.start(static_cast<int>(std::min<qint64>(this->delay, remaining)));
		this->delay = std::min(this->delay * 2, 4000);
	}

	std::function<bool()> acquire;
	std::function<void()> recovered;
	std::function<qint64()> age;
	QTimer timer;
	QElapsedTimer elapsed;
	int delay = 250;
	bool recovering = false;
};

} // namespace qs::windows
