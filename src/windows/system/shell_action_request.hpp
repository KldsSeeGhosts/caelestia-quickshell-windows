#pragma once

#include <atomic>

#include <qtypes.h>
#include <qvariant.h>

namespace qs::windows::sys::detail {

inline quint64 nextShellActionRequestId() {
	static std::atomic<quint64> next {1};
	return next.fetch_add(1, std::memory_order_relaxed);
}

inline void addShellActionRequestId(QVariantMap& result, quint64 requestId) {
	result.insert(QStringLiteral("requestId"), QVariant::fromValue(requestId));
}

} // namespace qs::windows::sys::detail
