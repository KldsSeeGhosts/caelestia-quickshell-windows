#pragma once

#include <mutex>
#include <utility>

#include <qmetaobject.h>
#include <qobject.h>

namespace qs::windows {

// Native event revocation need not wait for a callback already in flight.
// Capture a shared gate in native callbacks, then close it on the receiver's
// thread before revoking events and destroying the receiver.
template <typename T>
class NativeCallbackGate {
public:
	explicit NativeCallbackGate(T* target): target(target) {}

	template <typename F>
	void post(F&& fn) {
		std::lock_guard lock(this->mutex);
		if (!this->target) return;
		auto* receiver = this->target;
		QMetaObject::invokeMethod(
		    receiver,
		    [receiver, fn = std::forward<F>(fn)]() mutable { fn(receiver); },
		    Qt::QueuedConnection
		);
	}

	void close() {
		std::lock_guard lock(this->mutex);
		this->target = nullptr;
	}

private:
	std::mutex mutex;
	T* target;
};

} // namespace qs::windows
