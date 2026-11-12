#pragma once

#include <functional>

#include <qobject.h>
#include <qstring.h>
#include <qtypes.h>

class QImage;

namespace qs::windows::sys {

// Win32 boundaries only. Fixtures run the real lifecycle and retry policy
// without opening, reading or changing the desktop clipboard.
class ClipboardWriteProvider {
public:
	virtual ~ClipboardWriteProvider() = default;
	virtual void* imageData(const QImage& image) = 0;
	virtual void* textData(const QString& text) = 0;
	virtual void freeData(void* data) = 0;
	virtual bool open() = 0;
	virtual bool empty() = 0;
	// Success transfers ownership to Windows. Failure retains caller ownership.
	virtual bool set(quint32 format, void* data) = 0;
	virtual void close() = 0;
	virtual quint32 sequence() = 0;
};

class ClipboardListenerProvider {
public:
	virtual ~ClipboardListenerProvider() = default;
	virtual bool start(QObject* context, std::function<void()> update) = 0;
	virtual void stop() = 0;
};

} // namespace qs::windows::sys
