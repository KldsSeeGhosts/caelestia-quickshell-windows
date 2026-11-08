#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>
#include <qvariant.h>

#include "../../core/doc.hpp"
#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "connection.hpp"
#include "monitor.hpp"
#include "pin_support.hpp"

namespace qs::hyprland::ipc {

class HyprlandIpcQml: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(QString requestSocketPath READ requestSocketPath CONSTANT);
	Q_PROPERTY(QString eventSocketPath READ eventSocketPath CONSTANT);
	Q_PROPERTY(bool usingLua READ usingLua CONSTANT);
	Q_PROPERTY(int workspaceLimit READ workspaceLimit WRITE setWorkspaceLimit NOTIFY workspaceLimitChanged);
	Q_PROPERTY(bool pinSupported READ pinSupported NOTIFY pinSupportedChanged);
	Q_PROPERTY(bool desktopControlSupported READ desktopControlSupported NOTIFY desktopControlSupportedChanged);
	Q_PROPERTY(QObject* desktopControl READ desktopControl CONSTANT);
	Q_PROPERTY(qs::hyprland::ipc::HyprlandMonitor* focusedMonitor READ default NOTIFY focusedMonitorChanged BINDABLE bindableFocusedMonitor);
	Q_PROPERTY(qs::hyprland::ipc::HyprlandWorkspace* focusedWorkspace READ default NOTIFY focusedWorkspaceChanged BINDABLE bindableFocusedWorkspace);
	Q_PROPERTY(qs::hyprland::ipc::HyprlandToplevel* activeToplevel READ default NOTIFY activeToplevelChanged BINDABLE bindableActiveToplevel);
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandMonitor>*);
	Q_PROPERTY(UntypedObjectModel* monitors READ monitors CONSTANT);
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandWorkspace>*);
	Q_PROPERTY(UntypedObjectModel* workspaces READ workspaces CONSTANT);
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandToplevel>*);
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	// clang-format on
	QML_NAMED_ELEMENT(Hyprland);
	QML_SINGLETON;

public:
	explicit HyprlandIpcQml();

	Q_INVOKABLE static void dispatch(const QString& request);

	Q_INVOKABLE static HyprlandMonitor* monitorFor(QuickshellScreenInfo* screen);

	Q_INVOKABLE static void refreshMonitors();

	Q_INVOKABLE static void refreshWorkspaces();

	Q_INVOKABLE static void refreshToplevels();

	/// The visible application windows from the topmost down, as a list of
	/// `{ id, title, rects }` objects. Windows extension, not part of Hyprland.
	///
	/// `rects` is a list of `{ screen, rect }`, one per screen the window
	/// covers. `screen` is the screen's name (`ShellScreen.name`) and `rect` is
	/// the part of the visible frame on that screen, in Qt logical global
	/// coordinates (the space of `QuickshellScreenInfo` geometry). A window that
	/// spans monitors of different scale is cut at the monitor border and each
	/// part uses its own monitor's scale, so a picker window on a screen
	/// hit-tests only that screen's parts. `title` is empty for a window without
	/// a caption. `id` is the address a `HyprlandToplevel` of the same window
	/// reports. Cloaked, minimised and hidden windows, the desktop, the taskbars
	/// and other Windows shell surfaces, and this shell's own windows are not
	/// listed.
	///
	/// This is a one-shot, synchronous read for a picker that opens on demand.
	/// `toplevels` is the live set and has no stacking order.
	Q_INVOKABLE static QVariantList windowStack();

	[[nodiscard]] static QString requestSocketPath();
	[[nodiscard]] static QString eventSocketPath();
	[[nodiscard]] static int workspaceLimit();
	[[nodiscard]] static QObject* desktopControl();
	void setWorkspaceLimit(int count);
	[[nodiscard]] static bool usingLua() { return false; }
	[[nodiscard]] bool pinSupported() const { return this->pinSupport.pinSupported(); }
	// Requires a compatible in-process provider or loaded desktop accessor.
	[[nodiscard]] bool desktopControlSupported() const {
		return this->pinSupport.desktopControlSupported();
	}
	[[nodiscard]] static QBindable<HyprlandMonitor*> bindableFocusedMonitor();
	[[nodiscard]] static QBindable<HyprlandWorkspace*> bindableFocusedWorkspace();
	[[nodiscard]] static QBindable<HyprlandToplevel*> bindableActiveToplevel();
	[[nodiscard]] static ObjectModel<HyprlandMonitor>* monitors();
	[[nodiscard]] static ObjectModel<HyprlandWorkspace>* workspaces();
	[[nodiscard]] static ObjectModel<HyprlandToplevel>* toplevels();

signals:
	void rawEvent(qs::hyprland::ipc::HyprlandIpcEvent* event);

	void desktopControlSupportedChanged();
	void workspaceLimitChanged();

	void focusedMonitorChanged();
	void focusedWorkspaceChanged();
	void activeToplevelChanged();
	void pinSupportedChanged();

private:
	PinSupport pinSupport;
};

} // namespace qs::hyprland::ipc
