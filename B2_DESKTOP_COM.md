# B2 desktop COM design

This PR completes virtual desktops. Wallpaper remains a separate B2 PR.

## Ownership and transport

One dedicated, detached MTA worker owns every private desktop interface and
the public IVirtualDesktopManager. Activation, QueryService, enumeration,
commands, window queries and every Release run in that apartment. No existing
GUI-owned interface moves to the worker.

Requests contain values, HWND identities and GUIDs, never borrowed output
storage or COM pointers. Completed requests contain immutable value snapshots.
The GUI applies completions only from its current worker generation.
Worker identity is the generation. Sequence numbers also reject an older
refresh after a newer command snapshot on the same worker.

Desktop list/current reads and window membership reads return cached values
without waiting. Registry and cloak notifications request a coalesced refresh.
An unknown window returns the existing conservative fallback until its query
completes. A snapshot notification updates window tracking and panel consumers
even when the desktop list/current desktop has not changed.
The tracker retains its visible/pinned-window current-desktop inference and
reconsiders cloaked candidates after their first query completes. Window
queries keep at most 4096 identities and prune destroyed windows.

Switch and create keep their existing accepted/observed result contract. Move
keeps its boolean result and unobserved command result. All three submit exactly
once and wait at most 100 ms for the worker result, including queue time. A
healthy worker returns immediately when COM completes, with no fixed delay.
A timeout returns false with a denied/provider-timeout command result; it never
retries the command. The wait dispatches sent native messages, but not posted
Qt events. Refreshes coalesce and use a 10 ms completion timer only while busy.
Removal and pinning remain unsupported, as on main. No keyboard fallback exists.
The own-window public-manager move also runs on the same worker.

## Failure, replacement and shutdown

A timed-out worker is retired. Its queued requests are discarded and its late
output cannot update the GUI. A request already inside COM cannot be cancelled
and may still change Explorer state. Recovery reacquires and refreshes state,
never replays a command.

TaskbarCreated retires the generation before scheduling the existing 250 ms
through 4 s recovery backoff, bounded to a 30 s automatic recovery window.
Unknown private-interface build families remain refused. Public window queries
remain available independently of the private-interface opt-in/build gate.
If private acquisition is refused but the public manager works, that apartment
continues to answer window queries and own-window moves during private recovery.
Observer installation does not bypass the recovery backoff.

At most four worker apartments exist at once, including retired workers stuck
inside COM or Release. At capacity recovery fails without allocating another
thread. Capacity returns when a retired worker exits. Shutdown signals stop
without joining or releasing an interface on the GUI. A stuck apartment can
therefore survive until process exit, but cannot cause unbounded thread growth.

## Blocking-site audit

| Previous site | New behavior |
| --- | --- |
| Native ImmersiveShell activation and both QueryService calls | Worker-owned acquisition, 100 ms GUI deadline. |
| Native public-manager activation | Same worker and deadline. Public-only mode does not try private interfaces. |
| GetDesktops, array GetCount/GetAt, desktop GetId, GetCurrentDesktop/GetId | Coalesced worker refresh. GUI count, IDs and current index read values only. |
| Public GetWindowDesktopId and IsWindowOnCurrentVirtualDesktop | Worker refresh of watched HWND identities. GUI reads cached GUID/membership, with the existing null/unknown fallback. |
| SwitchDesktop | One bounded request. The existing accepted/observed switch contract remains. A failed deadline does not replay the switch. |
| CreateDesktop and its enumeration | One bounded request. The existing ensureCount callback and accepted/observed creation contract remain. |
| GetViewForHwnd, CanViewMoveDesktops and MoveViewToDesktop | One bounded move request. Its boolean and completion-unobserved result remain. |
| Public MoveWindowToDesktop for own-process windows | Same worker and bounded boolean result, independent of private control support. |
| Native local, snapshot and provider Release calls | Worker apartment only, including retired-worker cleanup. |
| VirtualDesktops refresh, recovery, TaskbarCreated and destructor Release calls | Removed. They read snapshots or retire a worker without joining it. |
| Removal and pinning | Still unsupported by the native table. No added COM path or synthetic input. |
| Wallpaper COM and SystemParametersInfoW | Unchanged in this PR. The separate wallpaper B2 PR must finish this part of the campaign. |

## Verification

Extend the existing fake-provider fixtures to cover blocked activation,
enumeration, window queries, commands and Release, cached GUI reads, bounded
failure, stale-generation rejection, replacement, capacity and shutdown.
Tests do not activate or message Explorer. Build quickshell and fixtures with
Qt 6.11.3 and run desktop, tiling, Hyprland and wallpaper portable fixtures with
QT_QPA_PLATFORM=offscreen. Live desktop acceptance remains the owner's step.
