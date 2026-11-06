#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>

namespace qs::windows::services::notifications {

// Owns the per-session notification bus mutex. Exactly one qs process serves
// this user's notifications; every other process forwards to it. Ownership is
// a real mutex wait on a dedicated thread, so the OS hands the mutex over the
// moment the current owner exits or abandons it, and the GUI thread never
// blocks or polls.
class SessionBusLock {
public:
	// The lock thread's result. Contention is not a result: it is the absence
	// of an Owned report while the wait is pending. Failed means a Win32 call
	// could not open the lock at all, with the GetLastError value, and must not
	// be mistaken for another shell owning the bus.
	enum class Result {
		Owned,
		Failed,
	};

	// Called on the lock thread when ownership is acquired, or once when an
	// initialisation call fails.
	using Handler = std::function<void(Result result, DWORD error)>;

	SessionBusLock(std::wstring name, Handler handler);
	~SessionBusLock();
	SessionBusLock(const SessionBusLock&) = delete;
	SessionBusLock& operator=(const SessionBusLock&) = delete;
	SessionBusLock(SessionBusLock&&) = delete;
	SessionBusLock& operator=(SessionBusLock&&) = delete;

	[[nodiscard]] bool owned() const { return this->mOwned.load(std::memory_order_acquire); }

#ifdef QS_NOTIFICATION_TEST_SEAMS
	// Replaces the lock's Win32 object creation so a fixture can force the
	// failure path without exhausting handles or memory. Null creates a real
	// event/mutex. Compiled only into the test target.
	static HANDLE (*testCreateObject)(const wchar_t* name, bool event);
#endif

private:
	void waitForOwnership();

	std::wstring mName;
	Handler mHandler;
	HANDLE mStop = nullptr;
	std::thread mThread;
	std::atomic<bool> mOwned = false;
};

} // namespace qs::windows::services::notifications
