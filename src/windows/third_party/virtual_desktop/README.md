# Native virtual desktop ABI

`../../native_desktop_accessor.cpp` adapts only the COM interface prefixes from
Markus Scholtes' MIT-licensed VirtualDesktop 1.21 (2025-08-11):
https://github.com/MScholtes/VirtualDesktop/blob/a725cbd3cdb9e977678eeaf034a7cc96d2e74bc6/VirtualDesktop11-24H2.cs

Windows build families 26100, 26200 and 26300 are accepted only if QueryService
accepts the exact 24H2 manager and application-view collection GUIDs. Other
builds and mismatched interfaces are refused. No DLL is downloaded or loaded
by this provider. Removal, pinning, special workspaces and UI automation are
not implemented. Explorer disconnection disables the provider until restart.

Native control requires both CAELESTIA_WINDOWS_HYPR=1 and
CAELESTIA_WINDOWS_HYPR_DISPATCH=1. `Hyprland.workspaceLimit` bounds on-demand
creation (default 5, clamped 1..20); existing desktops above it are preserved
and remain addressable.
