# Architecture

Event-driven root daemon: Binder callbacks on a small pool thread, all I/O and
rate decisions on one epoll thread.

```text
  Binder pool (1 extra thread)
    IProcessObserver ──► g_query_task_pending + eventfd
    IDisplayManagerCallback ──► dirty bits + eventfd
    IBatteryPropertiesListener ──► evaluateBatteryState (+ wake if changed)
    death recipient ──► g_shutdown_requested + eventfd

  epoll thread (touchListenerThread)
    /dev/input  inotify  @dfps  netlink uevent  timerfd(30s)  eventfd
         │         │       │           │              │          │
         └─────────┴───────┴───────────┴──────────────┴──────────┘
                                    │
                          updateRateState(now)
                                    │
                     SurfaceFlinger tx 1035 (Hz → modes.map id)
```

`main()` hardens the process, loads Binder + config, starts the pool and the
epoll thread, registers observers, then `pthread_join`s the epoll thread.

## Startup sequence

1. `mlockall`, timerslack 0, IO_FLUSHER, NO_NEW_PRIVS, undumpable, KEEP_CAPS
2. `SCHED_FIFO` priority 2 (else nice `-20`); pin to efficiency cores
3. `dlopen(libbinder_ndk.so)`, resolve symbols; pool max threads = **1**
4. Get services: `activity`, `power`, `SurfaceFlinger`, `display`,
   `batteryproperties`; link death on the critical ones
5. Resolve transaction codes (disk cache or one-shot `app_process` helper)
6. `loadConfig` / `loadModesMap`; bind abstract `@dfps` (singleton)
7. Netlink uevent (root); optional battery listener; spawn epoll thread
8. Register display callback (event mask `4|8` → CHANGED + BRIGHTNESS)
9. Register process observer; initial `queryFocusedTask`; join epoll thread

Fails fast if core AM codes are missing or `@dfps` is already bound.

## Shared state

| Mechanism | Protects |
|---|---|
| atomics | rates, touch flags, interactive, power/battery, dirty bits |
| `g_config_lock` (rwlock) | per-app rule table + modes map |
| `g_client_lock` (spinlock) | client fds + last package snapshot |

Config **scalars** used on the rate path are atomics so `updateRateState`
avoids taking the rwlock every tick.

## Rate selection (`updateRateState`)

Priority order:

1. Screen not interactive → `offscreenRate` if `> 0`, else no write
2. Brightness clamp → min physical rate (or `defaultIdle`)
3. Touching **or** within `touchSlackMs` of last touch → active rate  
   (capped by `powerSaveMaxRate` if power-save or low-battery)
4. Else → idle rate

`setRefreshRate` skips the Binder call when the target equals `g_last_set_rate`.
Hz maps through `modes.map` (exact, then closest within 30 Hz).

Touch contacts must last **`TOUCH_DEBOUNCE_MS` (50)** before engaging active
rate. Multi-device contacts are OR’d so dual digitizers do not clear each other.

## Hot-path rules

- Callbacks never issue blocking Binder from the pool thread; they set flags
  and write eventfd. The epoll thread performs the real queries.
- Power helpers return whether state changed. The epoll thread sets
  `needs_rate_update` instead of nesting another eventfd write.
- With the display callback registered, touch-slack / debounce timeouts do
  **not** re-probe power-save or battery (timerfd owns those).
- Touch epoll tags store **device index**, not the raw fd.
- Screen-off touch reset runs on the interactive **falling edge** only.
- One `CLOCK_MONOTONIC_COARSE` read per epoll iteration; passed into
  `updateRateState(now)`.
- Battery sysfs uses a cached capacity path + `open`/`read`.

## Inputs

| Source | Role |
|---|---|
| `IProcessObserver` | **Event trigger only** — uid/pid/fg fields are stale; package comes from `getFocusedRootTaskInfo` `childTaskNames` (`pkg/cls`, split on `/`) |
| `IDisplayManagerCallback` | event 2 interactive-ish display change; event 4 brightness (mask `4\|8`) |
| `IPowerManager` | `isInteractive` when no display callback / on dirty; `isPowerSaveMode` on timer / probes |
| Battery listener + uevent + sysfs | Level for low-battery mode |
| `/dev/input/event*` | Touch (root); BTN_TOUCH or MT tracking-id fallback |
| inotify | `dfps.conf` / `modes.map` reload |
| timerfd 30s | Power-save + battery poll when no better signal |

## Client socket

`@dfps`: SO_PEERCRED auth; push package list; `STATUS` health. See
[`CLIENT_PROTOCOL.md`](./CLIENT_PROTOCOL.md).

## Shutdown

`SIGTERM`/`SIGINT` set a signal flag and write eventfd; the loop promotes
that to `g_shutdown_requested` and closes epoll, sockets, timerfd, touch
fds. Binder death sets `g_shutdown_requested` **directly** (not via the
signal helper) and wakes the loop. Second signal → `_exit`.
