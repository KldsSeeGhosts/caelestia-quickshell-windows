#include "session_bus_lock.hpp"
#include <utility>

namespace qs::windows::services::notifications {

#ifdef QS_NOTIFICATION_TEST_SEAMS
HANDLE (*SessionBusLock::testCreateObject)(const wchar_t* name, bool event) = nullptr;
#endif

namespace {
// The stop event is unnamed; the mutex is the named session object.
HANDLE createLockObject(const wchar_t* name, bool event) {
#ifdef QS_NOTIFICATION_TEST_SEAMS
	if (SessionBusLock::testCreateObject != nullptr) {
		return SessionBusLock::testCreateObject(name, event);
	}
#endif
	return event ? CreateEventW(nullptr, TRUE, FALSE, nullptr)
	             : CreateMutexW(nullptr, FALSE, name);
}
} // namespace

SessionBusLock::SessionBusLock(std::wstring name, Handler handler)
    : mName(std::move(name))
    , mHandler(std::move(handler)) {
	this->mStop = createLockObject(nullptr, true);
	if (this->mStop == nullptr) {
		if (this->mHandler) this->mHandler(Result::Failed, GetLastError());
		return;
	}

	this->mThread = std::thread([this] { this->waitForOwnership(); });
}

SessionBusLock::~SessionBusLock() {
	if (this->mStop != nullptr) SetEvent(this->mStop);
	if (this->mThread.joinable()) this->mThread.join();
	if (this->mStop != nullptr) CloseHandle(this->mStop);
	this->mStop = nullptr;
}

// The OS wakes this thread when the named mutex is free: the previous owner
// exited (WAIT_ABANDONED_0, still an acquisition) or released it. Until then
// the wait costs no CPU and needs no polling.
void SessionBusLock::waitForOwnership() {
	auto mutex = createLockObject(this->mName.c_str(), false);
	if (mutex == nullptr) {
		if (this->mHandler) this->mHandler(Result::Failed, GetLastError());
		return;
	}

	HANDLE handles[] = {mutex, this->mStop};
	auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED_0) {
		CloseHandle(mutex);
		return;
	}

	this->mOwned.store(true, std::memory_order_release);
	if (this->mHandler) this->mHandler(Result::Owned, 0);

	// Keep the mutex owned on this thread for the life of the object. On
	// shutdown the stop event releases the wait, then ownership so the next
	// shell can take over immediately.
	WaitForSingleObject(this->mStop, INFINITE);
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	this->mOwned.store(false, std::memory_order_release);
}

} // namespace qs::windows::services::notifications
