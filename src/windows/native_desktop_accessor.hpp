#pragma once

#include "virtual_desktops.hpp"

namespace qs::windows {

// GUI facade. All COM interfaces belong to a detached worker apartment.
// Reads use snapshots. Commands wait at most 100 ms, then remain accepted in
// flight. Only a 5 s stall retires an apartment. Recovery never replays commands.
bool loadNativeDesktopAccessor(
    VirtualDesktops::Accessor& accessor,
    QObject* context,
    std::function<void()> recovered
);
bool retryNativeDesktopAccessor();
void invalidateNativeDesktopAccessor(HRESULT error = S_OK);
[[nodiscard]] DesktopAvailability nativeDesktopAvailability();
[[nodiscard]] HRESULT nativeDesktopLastError();
void initializeNativeDesktopQueries(QObject* context, std::function<void()> updated);
void setNativeDesktopMoveObserver(
    QObject* context,
    std::function<void(quint64, HWND, bool, DesktopAvailability, HRESULT)> onMoveCompleted
);
void setNativeDesktopPinObserver(
    QObject* context,
    std::function<void(quint64, HWND, OwnWindowPin)> onPinCompleted
);
void requestNativeDesktopRefresh();
[[nodiscard]] bool nativeDesktopCommandPending();
[[nodiscard]] quint64 nativeDesktopPendingMoveSequence(HWND hwnd = nullptr);
// Observation must also wait for the snapshot that can confirm a command.
[[nodiscard]] bool nativeDesktopRequestPending();
[[nodiscard]] quint64 nativeDesktopSubmittedSequence();
[[nodiscard]] quint64 nativeDesktopReadSequence();
[[nodiscard]] quint64 nativeDesktopCancelledSequence();
[[nodiscard]] quint64 nativeDesktopFailedSwitchSequence();
[[nodiscard]] HRESULT nativeDesktopFailedSwitchError();
[[nodiscard]] quint64 nativeDesktopWindowSnapshotRevision();
[[nodiscard]] GUID nativeWindowDesktopId(HWND hwnd);
[[nodiscard]] int nativeWindowOnCurrentDesktop(HWND hwnd);
int switchNativeDesktopById(GUID desktop, quint64* outSequence = nullptr);
int switchNativeDesktopByIdIfCurrent(
    GUID desktop,
    GUID expectedCurrent,
    quint64* outSequence = nullptr
);
int moveNativeDesktopWindowById(HWND hwnd, GUID desktop);
std::optional<bool> moveNativeDesktopOwnWindow(HWND hwnd, GUID desktop);
// Submits an asynchronous pin for one of this process's windows on a dedicated
// pinning worker without blocking the GUI thread or desktop commands. Returns
// Pending when submitted (writing a unique request ID to outRequestId and
// delivering the result to the pin observer), Busy if another pin is in flight,
// or Unavailable when pinning is absent.
[[nodiscard]] OwnWindowPin pinNativeDesktopOwnWindow(HWND hwnd, quint64* outRequestId = nullptr);
void shutdownNativeDesktopAccessor();

#ifdef QS_TEST
// Allows generated handles to exercise the production move path without
// creating a real window. Reset together with the injected provider factory.
void setNativeDesktopWindowValidatorForTest(std::function<bool(HWND)> validator);
// Uses the existing elapsed-time seam; an empty callback restores the real clock.
// Reset together with the injected provider factory.
void setNativeDesktopRecoveryAgeForTest(std::function<qint64()> age);
[[nodiscard]] int nativeDesktopRecoveryPendingDelayForTest();
[[nodiscard]] int refreshNativeDesktopForTest();
[[nodiscard]] int nativeDesktopWorkerCountForTest();
[[nodiscard]] bool nativeDesktopPendingReadyForTest();
void setNativeDesktopStuckBoundForTest(int milliseconds);
#endif

} // namespace qs::windows
