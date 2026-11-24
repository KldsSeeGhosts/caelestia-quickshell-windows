#pragma once

#include <qstring.h>

namespace qs::windows {

// The window property store is a synchronous shell call. Explorer and its
// shell hosts are skipped before the responsiveness probe, because hanging
// taskbar or shell windows are the usual source of this stall. The callbacks
// keep the policy testable without constructing windows.
template <typename IsResponding, typename IsShellHost, typename IsShellClass>
[[nodiscard]] bool shouldQueryWindowPropertyStore(
    const QString& processName,
    const QString& windowClass,
    IsResponding isResponding,
    IsShellHost isShellHost,
    IsShellClass isShellClass
) {
	if (processName.compare(QStringLiteral("explorer.exe"), Qt::CaseInsensitive) == 0
	    || isShellHost(processName) || isShellClass(windowClass))
	{
		return false;
	}

	return isResponding();
}

} // namespace qs::windows
