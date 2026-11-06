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
void invalidateNativeDesktopAccessor();
[[nodiscard]] DesktopAvailability nativeDesktopAvailability();
[[nodiscard]] HRESULT nativeDesktopLastError();

} // namespace qs::windows
