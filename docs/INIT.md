# Initialization and hardening

How `dfps` comes up, what it hardens itself, and what must be granted by
init / SELinux.

## Boot launch

Module `service.sh` (or equivalent root-manager boot hook) execs:

```text
/data/local/tmp/dfps/dfps
```

The process:

1. Fails if abstract `@dfps` is already bound (singleton).
2. Resolves `libbinder_ndk.so` and system services (`activity`, `power`,
   `SurfaceFlinger`, `display`, `batteryproperties`).
3. Resolves Binder transaction codes (cache or `app_process` helper JAR).
4. Loads config, opens touch devices (root), inotify, uevent, timerfd.
5. Registers process observer and (when possible) display callback.
6. Enters the epoll loop on a dedicated thread; `main` joins it.

## Self-hardening (in order)

| Step | Intent |
|---|---|
| `mlockall(MCL_CURRENT \| MCL_FUTURE)` | Keep working set resident (best-effort) |
| `prctl(PR_SET_TIMERSLACK, 0)` | Reduce wakeup jitter |
| `prctl(PR_SET_IO_FLUSHER, 1)` | Prefer prompt dirty flush |
| `prctl(PR_SET_NO_NEW_PRIVS, 1)` | Block setuid re-escalation after start |
| `prctl(PR_SET_DUMPABLE, 0)` | No core dumps; no unprivileged ptrace |
| `prctl(PR_SET_KEEP_CAPS, 1)` | Retain caps across any later drop |
| `sched_setscheduler(SCHED_FIFO, 2)` | RT scheduling; else `setpriority(-20)` |
| CPU affinity | Efficiency cluster (lowest max freq) when heterogeneous |

Fatal signals (SEGV/ABRT/BUS/ILL/FPE) log a best-effort backtrace to stderr
then re-raise so the supervisor sees the real signal. `SIGPIPE` and
`SIGWINCH` are blocked.

## Capabilities (left to init / SELinux)

The binary does **not** `capset` down to a minimal set — required caps vary
by device:

| Cap | Typical need |
|---|---|
| `CAP_NET_ADMIN` | `NETLINK_KOBJECT_UEVENT` for power_supply events |
| `CAP_SYS_ADMIN` | `/dev/input` and some sysfs/refresh paths on certain kernels |

Grant via the service definition (`capabilities NET_ADMIN SYS_ADMIN`) and/or
an SELinux domain that allows Binder, uevent netlink, and input open/read.
With `NO_NEW_PRIVS` + `KEEP_CAPS`, a compromise cannot regain dropped caps
through a setuid helper.

## Runtime directory

`/data/local/tmp/dfps` holds conf, modes map, tx cache, and the resolver JAR.
Created as `0700` when missing; if group/other-writable, the daemon warns
and best-effort `chmod 0700` when euid is 0. Prefer root:root `0700` at
install time.

## Socket authentication

Every `@dfps` accept uses `SO_PEERCRED`: only UID 0, the daemon UID, or
AID_SHELL (2000). See [`CLIENT_PROTOCOL.md`](./CLIENT_PROTOCOL.md).

## Graceful shutdown

| Trigger | Mechanism |
|---|---|
| `SIGTERM` / `SIGINT` | Signal flag + eventfd; loop sets `g_shutdown_requested` |
| Critical Binder death | `onBinderDied` sets `g_shutdown_requested` **directly** + eventfd |
| Second signal | `_exit(128+sig)` |

The loop closes epoll, inotify, server socket, wakeup eventfd, uevent,
maintenance timerfd, and touch fds before returning so `main` exits cleanly
for supervisor respawn. It does not call `exit(0)` from the death path.
