# Wallpaper COM design

Plan item B2, wallpaper portion. Part of #41.

A dedicated detached MTA worker owns wallpaper providers. All activation,
enumeration, getters, setters, SPI calls and Release stay on that worker.
Requests carry copied paths, monitor identities and desktop GUIDs. Results
carry values only. No worker captures a QObject or GUI-owned output storage.

The existing boolean setters have no completion signal. Preserve their healthy
synchronous result with a 100 ms GUI wait, including queue time. A slow request
continues and reports its eventual outcome through an added result signal.
Reads use the same bound and return their last known value on timeout.
The background setter remains asynchronous and coalesces the latest path.

Only a request incomplete for 5 s retires its apartment. A 300 ms answer is
slow, not stuck. Worker identity is the generation. Explorer restart retires
the generation and discards its results without replaying mutations. Recovery
reacquires through a fresh worker. At most four live apartments may exist.
Shutdown stops polling and signals workers without joining. Release and
CoUninitialize also run on the worker and count against that cap.

Wallpaper Engine and desktop_host.cpp are out of scope. The virtual desktop
worker stays unchanged. Fake-provider fixtures must not contact Explorer or
change the real wallpaper.
