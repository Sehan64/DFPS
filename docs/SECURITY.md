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
- `SIGPIPE` and `SIGWINCH` are blocked.

## Runtime limits

- `@dfps` is a public abstract socket with no authentication.
- `dfps.conf` and `modes.map` are root-writable by design.
- The transaction-code cache is local-only and mode-specific.

## Failure handling

- If a critical Binder service dies, DFPS exits so the supervisor can respawn
  a fresh instance.
- If the singleton socket is already in use, startup fails instead of running
  a second copy.
- Missing config files now fall back to defaults rather than keeping stale
  state.

## Threat model

DFPS is intended for rooted devices. It does not try to hide the foreground app
list from local processes, and it does not protect against a root attacker.
