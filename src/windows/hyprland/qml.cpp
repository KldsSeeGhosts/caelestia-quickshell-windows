#include "qml.hpp"

#include <qobject.h>
#include <qproperty.h>
#include <qvariant.h>

#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "../virtual_desktops.hpp"
#include "../window_stack.hpp"
#include "connection.hpp"
#include "monitor.hpp"
#include "pin_support.hpp"

namespace qs::hyprland::ipc {

HyprlandIpcQml::HyprlandIpcQml(): pinSupport(HyprlandIpc::instance()->desktops()) {
	auto* instance = HyprlandIpc::instance();

	QObject::connect(instance, &HyprlandIpc::rawEvent, this, &HyprlandIpcQml::rawEvent);

	QObject::connect(
	    instance,
	    &HyprlandIpc::focusedMonitorChanged,
	    this,
	    &HyprlandIpcQml::focusedMonitorChanged
	);

	QObject::connect(
	    instance,
	    &HyprlandIpc::focusedWorkspaceChanged,
	    this,
	    &HyprlandIpcQml::focusedWorkspaceChanged
	);

	QObject::connect(
	    instance,
	    &HyprlandIpc::activeToplevelChanged,
	    this,
	    &HyprlandIpcQml::activeToplevelChanged
	);

	// Notify on both initial load and loss after an accessor crash.
	QObject::connect(&this->pinSupport, &PinSupport::pinSupportedChanged,
	                 this, &HyprlandIpcQml::pinSupportedChanged);
	QObject::connect(
	    &this->pinSupport,
	    &PinSupport::desktopControlSupportedChanged,
	    this,
	    &HyprlandIpcQml::desktopControlSupportedChanged
	);
}

int HyprlandIpcQml::workspaceLimit() {
	return HyprlandIpc::instance()->desktops()->workspaceLimit();
}

QObject* HyprlandIpcQml::desktopControl() { return HyprlandIpc::instance()->desktops(); }

void HyprlandIpcQml::setWorkspaceLimit(int count) {
	auto* desktops = HyprlandIpc::instance()->desktops();
	auto before = desktops->workspaceLimit();
	desktops->setWorkspaceLimit(count);
	if (before != desktops->workspaceLimit()) {
		emit this->workspaceLimitChanged();
	}
}

void HyprlandIpcQml::dispatch(const QString& request) {
	HyprlandIpc::instance()->dispatch(request);
}

HyprlandMonitor* HyprlandIpcQml::monitorFor(QuickshellScreenInfo* screen) {
	return HyprlandIpc::instance()->monitorFor(screen);
}

void HyprlandIpcQml::refreshMonitors() { HyprlandIpc::instance()->refreshMonitors(); }
void HyprlandIpcQml::refreshWorkspaces() { HyprlandIpc::instance()->refreshWorkspaces(); }
void HyprlandIpcQml::refreshToplevels() { HyprlandIpc::instance()->refreshToplevels(); }

QVariantList HyprlandIpcQml::windowStack() {
	return qs::windows::window_stack::toVariantList(qs::windows::window_stack::snapshot());
}

QString HyprlandIpcQml::requestSocketPath() { return HyprlandIpc::instance()->requestSocketPath(); }
QString HyprlandIpcQml::eventSocketPath() { return HyprlandIpc::instance()->eventSocketPath(); }

QBindable<HyprlandMonitor*> HyprlandIpcQml::bindableFocusedMonitor() {
	return HyprlandIpc::instance()->bindableFocusedMonitor();
}

QBindable<HyprlandWorkspace*> HyprlandIpcQml::bindableFocusedWorkspace() {
	return HyprlandIpc::instance()->bindableFocusedWorkspace();
}

QBindable<HyprlandToplevel*> HyprlandIpcQml::bindableActiveToplevel() {
	return HyprlandIpc::instance()->bindableActiveToplevel();
}

ObjectModel<HyprlandMonitor>* HyprlandIpcQml::monitors() {
	return HyprlandIpc::instance()->monitors();
}

ObjectModel<HyprlandWorkspace>* HyprlandIpcQml::workspaces() {
	return HyprlandIpc::instance()->workspaces();
}

ObjectModel<HyprlandToplevel>* HyprlandIpcQml::toplevels() {
	return HyprlandIpc::instance()->toplevels();
}

} // namespace qs::hyprland::ipc
