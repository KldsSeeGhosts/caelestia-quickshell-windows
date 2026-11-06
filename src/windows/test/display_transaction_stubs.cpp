#include <qlogging.h>

#include "../services/message_window.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace qs::windows::services {
ServiceMessageWindow* ServiceMessageWindow::instance() {
	qFatal("Synthetic display fixture tried to create a native message window");
}
void ServiceMessageWindow::addHandler(UINT, QObject*, Handler) {
	qFatal("Synthetic display fixture tried to observe native display changes");
}
} // namespace qs::windows::services
