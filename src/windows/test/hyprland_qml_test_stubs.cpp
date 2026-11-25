#include <qlist.h>
#include <qstring.h>
#include <qvariant.h>

#include "../hyprland/connection.hpp"
#include "../hyprland/workspace.hpp"
#include "../tiling.hpp"
#include "../window_stack.hpp"
#include "../window_tracker.hpp"

namespace {

qs::windows::TilingManager* gTilingActive = nullptr;

} // namespace

namespace qs::windows {

TilingManager* TilingManager::active() { return gTilingActive; }

WindowTracker::WindowTracker() {}
WindowTracker::~WindowTracker() {}

WindowTracker* WindowTracker::instance() {
	static auto* tracker = new WindowTracker(); // NOLINT
	return tracker;
}

WindowTracker* WindowTracker::instanceIfCreated() { return WindowTracker::instance(); }

namespace window_stack {

QList<Entry> snapshot() { return {}; }
QVariantList toVariantList(const QList<Entry>&) { return {}; }

} // namespace window_stack

namespace test {

void setTilingManagerActive(bool active) {
	gTilingActive = active ? reinterpret_cast<TilingManager*>(static_cast<quintptr>(1)) : nullptr;
}

} // namespace test

} // namespace qs::windows

namespace qs::hyprland::ipc {

HyprlandIpc::HyprlandIpc() {}

HyprlandIpc* HyprlandIpc::instance() {
	static auto* instance = new HyprlandIpc(); // NOLINT
	return instance;
}

void HyprlandIpc::dispatch(const QString&) {}
HyprlandMonitor* HyprlandIpc::monitorFor(QuickshellScreenInfo*) { return nullptr; }
void HyprlandIpc::refreshMonitors() {}
void HyprlandIpc::refreshWorkspaces() {}
void HyprlandIpc::refreshToplevels() {}
ObjectModel<HyprlandMonitor>* HyprlandIpc::monitors() { return &this->mMonitors; }
ObjectModel<HyprlandWorkspace>* HyprlandIpc::workspaces() { return &this->mWorkspaces; }
ObjectModel<HyprlandToplevel>* HyprlandIpc::toplevels() { return &this->mToplevels; }

void HyprlandIpc::onWindowAdded(qs::windows::TrackedWindow*) {}
void HyprlandIpc::onWindowRemoved(qs::windows::TrackedWindow*) {}
void HyprlandIpc::onActiveWindowChanged() {}
void HyprlandIpc::onTrackerFlushed() {}
void HyprlandIpc::onDesktopsChanged() {}
void HyprlandIpc::onCurrentDesktopChanged() {}
void HyprlandIpc::onScreensChanged() {}
void HyprlandIpc::onButtonPressed(QPoint, quint32) {}
void HyprlandIpc::onCursorMonitorChanged(quintptr) {}
void HyprlandIpc::emitGeometryEvent() {}
void HyprlandIpc::emitTitleEvents() {}

QString HyprlandIpcEvent::nameStr() const { return QString::fromUtf8(this->name); }
QString HyprlandIpcEvent::dataStr() const { return QString::fromUtf8(this->data); }
QVector<QString> HyprlandIpcEvent::parse(qint32) const { return {}; }

void HyprlandWorkspace::updateFullscreen() {}
void HyprlandWorkspace::activate() {}

} // namespace qs::hyprland::ipc
