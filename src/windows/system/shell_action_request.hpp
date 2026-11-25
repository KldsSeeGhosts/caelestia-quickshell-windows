#pragma once

#include <atomic>

#include <qchar.h>
#include <qstring.h>
#include <qtypes.h>
#include <qvariant.h>

#include "../process_launch.hpp"

namespace qs::windows::sys::detail {

inline quint64 nextShellActionRequestId() {
	static std::atomic<quint64> next {1};
	return next.fetch_add(1, std::memory_order_relaxed);
}

inline void addShellActionRequestId(QVariantMap& result, quint64 requestId) {
	result.insert(QStringLiteral("requestId"), QVariant::fromValue(requestId));
}

inline bool containsEmbeddedNul(const QString& value) {
	return value.contains(QChar(0));
}

template <typename ShellFn, typename ActivateFn>
inline DWORD launchAppsFolderTokenWithFallback(
    const QString& token,
    const QString& workingDirectory,
    const QString& arguments,
    ShellFn&& runShell,
    ActivateFn&& runActivate
) {
	if (token.isEmpty()) return ERROR_FILE_NOT_FOUND;
	if (containsEmbeddedNul(token) || containsEmbeddedNul(workingDirectory)
	    || containsEmbeddedNul(arguments)) {
		return ERROR_INVALID_PARAMETER;
	}

	const process::LaunchResult shell = runShell();
	if (shell.status == process::LaunchStatus::Started) return ERROR_SUCCESS;
	if (shell.status != process::LaunchStatus::Failed || shell.error == ERROR_INVALID_PARAMETER) {
		return shell.error != ERROR_SUCCESS ? shell.error : ERROR_GEN_FAILURE;
	}
	if (runActivate()) return ERROR_SUCCESS;
	return shell.error != ERROR_SUCCESS ? shell.error : ERROR_GEN_FAILURE;
}

} // namespace qs::windows::sys::detail
