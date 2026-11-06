#include "../hyprland/workspace.hpp"

// Link-only stubs for the regression fixture. HyprlandMonitor's bindable
// `activeWorkspace` property makes AUTOMOC generate the HyprlandWorkspace
// metaobject, whose method table names these two out-of-line slots. The fixture
// never constructs a workspace or the HyprlandIpc singleton, so the real
// workspace implementation (and its window tracker) is not linked. Add a stub
// here if a new workspace slot starts appearing in that method table.
namespace qs::hyprland::ipc {
void HyprlandWorkspace::activate() {}
void HyprlandWorkspace::updateFullscreen() {}
} // namespace qs::hyprland::ipc
