#include <qlogging.h>

#include "../services/message_window.hpp"

// qs::log::initLogCategoryLevel comes from portable_common_stubs.cpp.

namespace qs::windows::services {
ServiceMessageWindow* ServiceMessageWindow::instance() {
	qFatal("Synthetic display fixture tried to create a native message window");
}
void ServiceMessageWindow::addHandler(UINT, QObject*, Handler) {
	qFatal("Synthetic display fixture tried to observe native display changes");
}
} // namespace qs::windows::services
