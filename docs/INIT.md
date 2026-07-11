# Initialization

This documents how `dfps` comes up, what it does at startup, and how the
hardening it applies fits with the surrounding init / SELinux context.

## Boot launch

The shipped module starts the daemon at boot. Two equivalent paths exist:

- **service.sh** (default for the tri-module): runs from the root manager's
  boot trigger and execs `/data/local/tmp/dfps/dfps`.
- **dfps.rc** (native init): a direct `init` service. See `dfps.rc` in the
  repo root for the exact `service` stanza. It uses `class main`, `user root`,
  `group root input`, and `critical` so init respawns the daemon if it exits
  (the daemon exits gracefully when a critical Binder service dies).

The daemon itself:

- fails fast if the singleton `@dfps` abstract socket is already bound
  (prevents a second copy),
- resolves `libbinder_ndk.so` and the system Binder services
  (`activity`, `power`, `SurfaceFlinger`, `display`, `batteryproperties`),
- registers a process observer and (when supported) an IDisplayManager
  callback, then enters the event loop.

## What the daemon hardens itself

At startup the daemon applies, in order:

1. `mlockall(MCL_CURRENT | MCL_FUTURE)` — keep the working set resident
   (best-effort; logged and ignored if denied).
2. `prctl(PR_SET_TIMERSLACK, 0)` — minimize wakeup jitter.
3. `prctl(PR_SET_IO_FLUSHER, 1)` — ask the kernel to flush dirty data
   promptly.
4. `prctl(PR_SET_NO_NEW_PRIVS, 1)` — block gaining privileges via a setuid
   helper after startup.
5. `prctl(PR_SET_DUMPABLE, 0)` — no core dumps, no `ptrace` attach by
   unprivileged processes.
6. `prctl(PR_SET_KEEP_CAPS, 1)` — keep the needed capabilities across any
   future drop (see below).
7. `sched_setscheduler(..., SCHED_FIFO, 2)` — real-time scheduling; falls back
   to `setpriority(..., -20)` if denied.

## Capability reduction is left to init / SELinux

The in-process hardening deliberately stops at `PR_SET_KEEP_CAPS` rather than
calling `capset` to drop to a minimal set. The exact capabilities the daemon
needs depend on the device:

- `CAP_NET_ADMIN` — open the kernel uevent (`NETLINK_KOBJECT_UEVENT`)
  netlink socket used for display hotplug detection.
- `CAP_SYS_ADMIN` — `/dev/input` access and sysfs/refresh-rate control on some
  kernels.

Granting these is the job of the launching context, not the binary:

- under **init** (`dfps.rc`): list them in the `capabilities` line
  (`NET_ADMIN SYS_ADMIN`).
- under **SELinux**: run in a domain (the `su` domain is the permissive
  fallback) that allows Binder transacts, the uevent netlink socket, and
  `/dev/input` open/read.

Because the daemon sets `PR_SET_NO_NEW_PRIVS` and `PR_SET_KEEP_CAPS`, it
cannot regain dropped caps and a compromise cannot re-escalate through a
setuid binary.

## Socket authentication

Every `@dfps` connection is authenticated with `SO_PEERCRED` on accept. Only
peers running as root (UID 0), the daemon's own UID, or the Android shell
(`AID_SHELL = 2000`, i.e. `adb shell`) are accepted; any other UID is
disconnected before any data is exchanged. See `docs/CLIENT_PROTOCOL.md`.

## Graceful shutdown

On `SIGTERM` / `SIGINT`, or when a critical Binder service dies
(`onBinderDied`), the daemon sets the shutdown flag and wakes the event loop.
The loop unwinds (closes epoll / fds / sockets) and `main()` returns, so the
supervising init respawns a fresh instance. It does **not** call `exit(0)`
abruptly, which would skip cleanup.
