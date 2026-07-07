# Architecture

DFPS is an event-driven daemon with two execution paths:

- Binder workers handle system callbacks.
- One epoll thread handles touch input, inotify reloads, uevents, and socket
  clients.

The main process bootstraps the daemon, starts the Binder thread pool with
`ABinderProcess_startThreadPool()`, and then waits for shutdown. The touch
thread owns the long-lived I/O loop.

## Startup

1. Harden the process with `mlockall(MCL_CURRENT | MCL_FUTURE)`,
   `prctl(PR_SET_TIMERSLACK, 0)`, `SCHED_FIFO`, and CPU affinity.
2. Load `libbinder_ndk.so` and resolve the Binder APIs used by the daemon.
3. Resolve Binder transaction codes via the cached map or the one-shot Java
   helper.
4. Load `dfps.conf` and `modes.map`.
5. Start the abstract socket, battery listeners, touch thread, and process
   observer.

Startup fails fast if the Binder map is incomplete or the singleton socket is
already bound.

## State

Shared state is kept in atomics plus two locks:

- `g_config_lock` protects rule and mode maps.
- `g_client_lock` protects socket clients and foreground-package snapshots.

The runtime state includes:

- screen interactive state
- touch state and slack timeout
- active and idle rates
- battery saver and low-battery mode
- brightness clamp
- shutdown state

## Rate selection

`updateRateState()` chooses a target rate from:

- screen off: optional offscreen rate
- brightness clamp: minimum physical rate or idle default
- touch: active rate
- idle: idle rate
- battery saver / low battery: cap the active rate

`setRefreshRate()` maps Hz to a SurfaceFlinger config ID and applies it through
Binder transaction `1035`.

## Input sources

- `IProcessObserver` signals foreground changes.
- `IDisplayManagerCallback` signals display and brightness changes.
- `IPowerManager` signals interactive and power-save changes.
- `IBatteryPropertiesListener` and netlink uevents signal battery changes.
- `/dev/input/event*` provides touch activity in root mode.
- `inotify` reloads `dfps.conf` and `modes.map`.

## Client socket

`@dfps` is a best-effort abstract Unix stream socket. Clients receive the
current foreground-package list on connect and every time the list changes.

## Shutdown

`SIGTERM` and `SIGINT` set shutdown state, wake the epoll thread, and let the
daemon exit normally. A second signal still falls through to `_exit()` if the
process is already inside the handler.
