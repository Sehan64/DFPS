# Security

DFPS is a root-resident daemon, so the main goal is to keep its runtime
behavior predictable and easy to restart.

## Hardening

- `mlockall(MCL_CURRENT | MCL_FUTURE)` keeps the working set resident.
- `prctl(PR_SET_TIMERSLACK, 0)` reduces wakeup jitter.
- `prctl(PR_SET_IO_FLUSHER, 1)` asks the kernel to flush dirty data quickly.
- `sched_setscheduler(..., SCHED_FIFO, 2)` requests real-time scheduling.
- If `SCHED_FIFO` is denied, DFPS falls back to `setpriority(..., -20)` and
  logs if that also fails.
- `prctl(PR_SET_NO_NEW_PRIVS, 1)` blocks gaining privileges via setuid
  helpers after startup.
- `prctl(PR_SET_DUMPABLE, 0)` prevents core dumps and ptrace attach by
  unprivileged processes.
- `prctl(PR_SET_KEEP_CAPS, 1)` is set so the reduced capability set (left to
  the init/SELinux context — see `docs/INIT.md`) is retained across the
  eventual drop.
- `SIGPIPE` and `SIGWINCH` are blocked.

## Runtime limits

- `@dfps` is an abstract socket that authenticates every peer with
  `SO_PEERCRED`. Only root (UID 0), the daemon's own UID, and the Android
  shell (AID_SHELL = 2000, i.e. `adb shell`) are accepted; any other UID is
  disconnected before any data is exchanged. This stops unprivileged local
  apps from learning the foreground package list.
- Authenticated clients may issue the read-only `STATUS` command (see
  `docs/CLIENT_PROTOCOL.md`) to fetch a one-line health snapshot.
- `dfps.conf` and `modes.map` are root-writable by design.
- The transaction-code cache is local-only and mode-specific.

## Failure handling

- If a critical Binder service dies, DFPS requests a graceful shutdown
  (sets the shutdown flag and wakes the event loop) instead of calling
  `exit(0)`. The event loop unwinds and `main()` returns, letting the
  supervising init respawn a fresh instance.
- If the singleton socket is already in use, startup fails instead of running
  a second copy.
- Missing config files now fall back to defaults rather than keeping stale
  state.

## Threat model

DFPS is intended for rooted devices. It does not protect against a root
attacker, but it no longer exposes the foreground-app list to arbitrary
non-privileged local apps over `@dfps`.
