#pragma once

#include <qstring.h>

namespace qs::redact {

// Logs must never carry user or device identifiers (AGENTS.md). Wrap any such
// value in one of these helpers instead of streaming it directly.
//
// The salt is random per process, so a redacted value can be correlated inside
// one run but not across runs, and cannot be reversed without guessing the
// original.
[[nodiscard]] QString logId(const QString& value);

// Same contract as logId(), for file paths, URLs and other path-like values.
// Only a known file extension is kept so logs still tell a PNG from a video.
[[nodiscard]] QString logPath(const QString& path);

} // namespace qs::redact
