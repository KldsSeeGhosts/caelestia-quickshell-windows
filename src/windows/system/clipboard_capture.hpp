#pragma once

#include <array>
#include <atomic>
#include <optional>

#include <qbytearray.h>
#include <qtypes.h>

namespace qs::windows::sys {

// One worker owns the open snapshot. Availability and data reads must only
// happen between its successful open() and close(), including privacy markers.
// Kept at the Win32 boundary so fixtures exercise the actual capture pipeline.
class ClipboardReadProvider {
public:
	virtual ~ClipboardReadProvider() = default;
	virtual std::array<quint32, 3> privacyFormats() = 0;
	virtual bool open() = 0;
	virtual void close() = 0;
	virtual void retryPause() = 0;
	virtual quint32 sequence() = 0;
	virtual bool available(quint32 format) = 0;
	// nullopt means an inaccessible handle/lock/size, not an absent format.
	virtual std::optional<QByteArray> data(quint32 format) = 0;
};

// Identity is the generation. Replacing it on clear also invalidates workers
// that have not started and results already queued to the object's thread.
struct ClipboardCaptureGeneration {
	std::atomic_bool cancelled = false;
};

} // namespace qs::windows::sys
