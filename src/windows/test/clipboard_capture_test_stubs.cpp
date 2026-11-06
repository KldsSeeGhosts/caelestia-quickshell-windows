#include <qlogging.h>
#include <qthreadpool.h>

#include "../../core/backgroundpool.hpp"
#include "../../core/instanceinfo.hpp"
#include "../../core/logcat.hpp"
#include "../services/message_window.hpp"

// Tests link the real clipboard.cpp, but inject its private read-only provider
// constructor. Any accidental use of the normal listener/cache-pruning path
// must fail, not touch the desktop or the real clipboard.
InstanceInfo InstanceInfo::CURRENT;

QThreadPool* BackgroundThreadPool::instance() {
	qFatal("Clipboard fixture attempted production cache pruning");
}

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace qs::windows::services {
ServiceMessageWindow* ServiceMessageWindow::instance() {
	qFatal("Clipboard fixture attempted native message-window access");
}
void ServiceMessageWindow::addHandler(UINT, QObject*, Handler) {
	qFatal("Clipboard fixture attempted clipboard listener registration");
}
} // namespace qs::windows::services
