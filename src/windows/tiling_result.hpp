#pragma once

#include <qtypes.h>

namespace qs::windows {

enum class FloatingChangeResult : quint8 {
	Changed,
	Noop,
	Deferred,
	Failed,
	Elevated,
	ScopeExcluded,
};

} // namespace qs::windows
