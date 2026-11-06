#pragma once

#include <qstring.h>

namespace qs::log {

// Home-directory and username scrubbing for every message that reaches the log
// sink. Called from LogManager::messageHandler() before a LogMessage is made,
// so files, stderr and the sparse log all see the same redacted text.
[[nodiscard]] QString scrubLogText(const QString& text);

// Forces the needle list to be built. Called once from LogManager::init().
void initLogScrubbing();

} // namespace qs::log
