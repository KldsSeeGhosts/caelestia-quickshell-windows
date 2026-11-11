# Backend resource limits

These limits protect against accidental same-user resource exhaustion. They are
not privilege or authentication boundaries.

Directory listings use two shared detached workers for the process. Each
`FsUtils` requester can have one active listing and one pending listing.
A newer request replaces the pending listing. Results are discarded when their
request ID is stale. A stuck share can occupy a worker, but cannot create more
workers, and shutdown does not join them. If both workers stall, listings wait.

The IPC server accepts at most 128 concurrent connections, including long-lived
signal listeners. Excess connections are closed. One incomplete request may
buffer at most 4 MiB, with one extra byte reserved to detect overflow. A
connection has 30 seconds from acceptance to complete its first command.
Receiving fragments does not extend this deadline. A completed signal-listen
request has no idle deadline. Additional input starts another deadline.
These limits leave ordinary `qs ipc` requests and idle signal listeners intact.

Notification forwarding requires the window owner's process image path to
match the forwarding executable's image path, case-insensitively. Process-query
failure or a mismatch falls back to local delivery. Different installed backend
paths therefore do not forward to each other. This does not authenticate
same-user processes running the same executable, or eliminate window-handle
reuse races.
