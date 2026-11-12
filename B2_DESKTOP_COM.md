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

Desktop list/current reads and window membership reads return cached values
without waiting. Registry and cloak notifications request a coalesced refresh.
An unknown window returns the existing conservative fallback until its query
completes. A snapshot notification updates window tracking and panel consumers
even when the desktop list/current desktop has not changed.

Switch and create keep their existing accepted/observed result contract. Move
keeps its boolean result and unobserved command result. All three submit exactly
once and wait at most 100 ms for the worker result, including queue time. A
healthy worker returns immediately when COM completes, with no fixed delay.
A timeout returns false with provider-timeout; it never retries the command.
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

At most four worker apartments exist at once, including retired workers stuck
inside COM or Release. At capacity recovery fails without allocating another
thread. Capacity returns when a retired worker exits. Shutdown signals stop
without joining or releasing an interface on the GUI. A stuck apartment can
therefore survive until process exit, but cannot cause unbounded thread growth.

## Verification

Extend the existing fake-provider fixtures to cover blocked activation,
enumeration, window queries, commands and Release, cached GUI reads, bounded
failure, stale-generation rejection, replacement, capacity and shutdown.
Tests do not activate or message Explorer. Build quickshell and fixtures with
Qt 6.11.3 and run desktop, tiling, Hyprland and wallpaper portable fixtures with
QT_QPA_PLATFORM=offscreen. Live desktop acceptance remains the owner's step.
