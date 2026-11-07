#pragma once

#include "virtual_desktops.hpp"

namespace qs::windows {

// In-process provider for the existing desktop accessor table. No DLL or
// synthetic input is involved. Call only on the COM-initialized GUI thread.
// Native COM calls catch SEH before returning through the facade, drop owned
// interfaces and report Disconnected/E_UNEXPECTED. Recovery uses the existing
// 30s backoff window and always reacquires interfaces, never replays a command.
bool loadNativeDesktopAccessor(
    VirtualDesktops::Accessor& accessor,
    QObject* context,
    std::function<void()> recovered
);
bool retryNativeDesktopAccessor();
void invalidateNativeDesktopAccessor(HRESULT error = S_OK);
[[nodiscard]] DesktopAvailability nativeDesktopAvailability();
[[nodiscard]] HRESULT nativeDesktopLastError();

#ifdef QS_TEST
// Allows generated handles to exercise the production move path without
// creating a real window. Reset together with the injected provider factory.
void setNativeDesktopWindowValidatorForTest(std::function<bool(HWND)> validator);
// Uses the existing elapsed-time seam; an empty callback restores the real clock.
// Reset together with the injected provider factory.
void setNativeDesktopRecoveryAgeForTest(std::function<qint64()> age);
[[nodiscard]] int nativeDesktopRecoveryPendingDelayForTest();
#endif

} // namespace qs::windows
