#pragma once

#include <qjsonobject.h>
#include <qrect.h>
#include <qstring.h>
#include <qtypes.h>

namespace qs::windows::transition_trace {
bool enabled();
void write(const char* event, QJsonObject fields = {});
QJsonObject rect(const QRect& value);
QString handle(quintptr value);
}

// Do not evaluate fields or query clocks when tracing is disabled.
#define QS_TRANSITION_TRACE(event, ...) \
	do { if (qs::windows::transition_trace::enabled()) \
		qs::windows::transition_trace::write(event, __VA_ARGS__); } while (false)
