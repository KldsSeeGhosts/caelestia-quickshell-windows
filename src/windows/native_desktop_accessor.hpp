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
void requestNativeDesktopRefresh();
[[nodiscard]] bool nativeDesktopCommandPending();
// Observation must also wait for the snapshot that can confirm a command.
[[nodiscard]] bool nativeDesktopRequestPending();
[[nodiscard]] quint64 nativeDesktopWindowSnapshotRevision();
[[nodiscard]] GUID nativeWindowDesktopId(HWND hwnd);
[[nodiscard]] int nativeWindowOnCurrentDesktop(HWND hwnd);
std::optional<bool> moveNativeDesktopOwnWindow(HWND hwnd, GUID desktop);
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
void setNativeDesktopStuckBoundForTest(int milliseconds);
#endif

} // namespace qs::windows
