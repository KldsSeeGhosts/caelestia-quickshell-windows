#include <qlogging.h>

#include "../../core/logcat.hpp"

// Shared link seam for the consolidated portable fixtures. Every domain
// runner resolves the logging-category hook to exactly this no-op; the
// identical per-fixture definitions were removed so several fixture object
// libraries can share one executable without duplicate symbols.
namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log
