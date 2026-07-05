# Architecture

This document describes the internal design of the **Dynamic FPS Controller**
(`dfps` daemon) — its threads, file descriptors, state machines, hot/cold
splits, and synchronization primitives. It is intended for maintainers and
anyone who needs to reason about correctness, performance, or extension
points.

> **Audience.** You should already be familiar with the user-facing concepts
> in [`README.md`](../README.md) and [`CONFIGURATION.md`](CONFIGURATION.md).
> This document assumes C99 fluency and basic familiarity with Linux IPC
> (`epoll`, `inotify`, `eventfd`, `signalfd`, Netlink, abstract Unix sockets)
> and Android's Binder NDK.

---

## Table of Contents

1. [Design Goals](#1-design-goals)
2. [Process Layout](#2-process-layout)
3. [Thread Model](#3-thread-model)
4. [The epoll Event Loop](#4-the-epoll-event-loop)
5. [File Descriptor Tagging](#5-file-descriptor-tagging)
6. [Shared State and Synchronization](#6-shared-state-and-synchronization)
7. [The eventfd Wakeup Mechanism](#7-the-eventfd-wakeup-mechanism)
8. [Hot/Cold Binder Split](#8-hotcold-binder-split)
9. [Binder Transaction Code Resolution](#9-binder-transaction-code-resolution)
10. [Per-App Rule Hash Table](#10-per-app-rule-hash-table)
11. [Refresh-Rate Decision Logic](#11-refresh-rate-decision-logic)
12. [State Machines](#12-state-machines)
13. [Foreground-App Tracking](#13-foreground-app-tracking)
14. [IPC Surfaces](#14-ipc-surfaces)
15. [Lifecycle](#15-lifecycle)

---

## 1. Design Goals

The daemon is built around five non-negotiable constraints:

| Goal              | Implication                                                                            |
|-------------------|----------------------------------------------------------------------------------------|
| **Low latency**   | Touch → rate transition should be sub-frame. No blocking calls on the hot path.        |
| **Low overhead**  | Must be cheap enough to run 24/7 as a Magisk/KernelSU service. Pinned to slow cores.   |
| **Wide compat.**  | Must boot on every Android version from API 26 to 15. No private/vendor headers.       |
| **Hot reload**    | Configuration and modes map can change at runtime; the daemon never restarts to apply. |
| **No GC pauses**  | Pure C, no JVM. Reflection confined to the one-shot resolver.                          |

These constraints drive every architectural decision below.

---

## 2. Process Layout

The daemon is a **single process, two threads**, with an optional auxiliary
spawn for transaction-code resolution.

```
                  ┌───────────────────── dfps process (PID) ─────────────────────┐
                  │                                                                │
                  │   ┌──────────────────┐         ┌──────────────────────────┐    │
                  │   │   Main Thread     │         │   Touch / epoll Thread   │    │
                  │   │  (Binder pool)    │         │  (event loop)            │    │
                  │   │                   │         │                          │    │
                  │   │ • observer cb     │ ──evt──▶│ • epoll_wait()           │    │
                  │   │ • display cb      │         │ • touch events           │    │
                  │   │ • battery cb      │         │ • inotify (config)       │    │
                  │   │ • one-shot resolve│         │ • uevent (battery)       │    │
                  │   │                   │         │ • client @dfps socket    │    │
                  │   └────────┬──────────┘         └──────────────┬────────────┘    │
                  │            │   triggerPollerWakeup()           │                 │
                  │            └───────────────────────────────────┘                 │
                  │                                                                │
                  │   ┌──────────────────────────────────────────────────────┐      │
                  │   │              Shared State (atomics + rwlock)         │      │
                  │   │  g_screen_interactive · g_touching · g_curr_*_rate   │      │
                  │   │  g_battery_* · g_config_lock · g_client_lock         │      │
                  │   └──────────────────────────────────────────────────────┘      │
                  │                                                                │
                  │   (one-shot, exits) app_process → CodeResolver.jar            │
                  └────────────────────────────────────────────────────────────────┘
```

The main thread is consumed by the Binder thread pool (`joinThreadPool()`);
it services inbound Binder transactions from `ActivityManager`,
`DisplayManager`, and `BatteryPropertiesRegistrar`. The touch thread runs
the entire I/O event loop.

A single one-shot `app_process` is spawned **only** when the cached
transaction-code map is missing or stale (see
[§9](#9-binder-transaction-code-resolution)). It exits within a few
hundred milliseconds; the daemon never re-`fork`s afterwards.

---

## 3. Thread Model

### 3.1 Main Thread — Binder Pool

`main()` is intentionally a linear bootstrap followed by
`g_cold.joinThreadPool()`. After that, the thread is owned by
`libbinder_ndk.so` and dispatches inbound transactions to the callback
stubs defined in [`binder.c`](../dfpsd/src/binder.c):

| Callback                       | Triggered by                    | Action                                                                  |
|--------------------------------|---------------------------------|-------------------------------------------------------------------------|
| `observerOnTransact`           | `IProcessObserver` (every FG)   | Set `g_query_task_pending`; call `triggerPollerWakeup()`.              |
| `displayCallbackOnTransact`    | `IDisplayManagerCallback`       | Schedule `checkInteractiveAndPowerSave()` and/or `checkMinBrightness()`.|
| `batteryListenerOnTransact`    | `IBatteryPropertiesListener`    | Extract `level`; call `evaluateBatteryState()`.                         |

All three are marked `__attribute__((hot))` and **never** write to shared
state directly — they only flip atomics and signal the epoll thread. This
keeps the Binder pool non-blocking and prevents priority-inversion stalls
from backing up system-wide Binder traffic.

### 3.2 Touch / Epoll Thread

Created by `main()` with a 256 KB stack
(`pthread_attr_setstacksize(&attr, 256 * 1024)`), detached, and entered
through `touchListenerThread()`. This thread runs the **only** event loop
in the daemon.

It owns:

- All touch device file descriptors (`/dev/input/event*`).
- The inotify watch on the config directory.
- The abstract Unix socket server (`@dfps`).
- All accepted client sockets.
- The netlink uevent socket.
- The wakeup `eventfd`.

It is the **only** thread that calls `setRefreshRate()` in production.

---

## 4. The epoll Event Loop

The loop is a single `epoll_wait()` driven by a tagged-pointer scheme (see
[§5](#5-file-descriptor-tagging)). It is structured to be **allocation-free
in the steady state** and to process a complete `epoll_wait()` batch before
re-evaluating state.

```
┌──────────────────────────────────────────────────────────────────────┐
│                       touchListenerThread()                          │
│                                                                      │
│   while (true) {                                                     │
│       timeout = getPollTimeout(now)        ← computed from state     │
│       nfds   = epoll_wait(epfd, ev, 64, timeout)                     │
│                                                                      │
│       if (nfds == 0) {                                               │
│           checkInteractiveAndPowerSave();                            │
│           checkMinBrightness();                                      │
│           if (battery_saver_enabled) read & evaluate battery (≤30s)  │
│       }                                                              │
│                                                                      │
│       if (screen off) reset touch & debounce; re-attach fds          │
│                                                                      │
│       if (debounce_expired && screen on) poll each touch fd once;    │
│           if (BTN_TOUCH released) clear touching; else extend 50ms   │
│                                                                      │
│       for each event in ev[0..nfds):                                 │
│           dispatch by FdKind {                                       │
│               FD_TOUCH    → read ev batch, set g_touching,           │
│                             arm debounce                              │
│               FD_WAKEUP   → drain eventfd; if query pending →        │
│                             queryFocusedTask();                      │
│               FD_INOTIFY  → loadConfig(); re-evaluate current app   │
│               FD_UEVENT   → handleUevent() (battery)                 │
│               FD_SERVER   → accept4(); add to g_client_fds;          │
│                             send current foreground packages         │
│               FD_CLIENT   → read; EPOLLRDHUP → remove & close        │
│           }                                                          │
│                                                                      │
│       if (needs_rate_update) {                                       │
│           if (low_batt) close all touch fds; else if (recovered)     │
│               findTouchscreens() and re-attach                       │
│           updateRateState()                                          │
│       }                                                              │
│   }                                                                  │
└──────────────────────────────────────────────────────────────────────┘
```

Key design choices:

- **Batched epoll** — up to 64 events per `epoll_wait()` to amortize
  syscall cost.
- **Batched input read** — up to 64 `struct input_event` per `read()` per
  device.
- **`getPollTimeout()`** — pre-computes a precise `epoll_wait` timeout
  based on the slack expiry or the debounce window, so the loop sleeps
  exactly as long as needed and not a millisecond more.
- **Touch fds stay in epoll throughout** — there is no
  `EPOLL_CTL_ADD`/`EPOLL_CTL_DEL` churn on every touch (the original
  implementation had a polling debounce that did this; the current code
  keeps the fds registered and reads them when the debounce window
  expires).
- **`low_battery` mode still closes touch fds** — this is a power-saving
  measure: while in low-battery mode the daemon is content to let the
  panel run at the constrained rate and ignore further touch input. The
  fds are reopened when battery recovers.

---

## 5. File Descriptor Tagging

Each `epoll_event.data.ptr` is a **tagged pointer** that encodes both the
file descriptor and its kind in a single 64-bit word:

```c
/* FdKind uses 8 bits; fd occupies the low 24 bits (max 16M fds). */
static inline void*   tag_fd(FdKind kind, int fd) {
    return (void*)(((uintptr_t)(unsigned)kind << 24) | (unsigned int)fd);
}
static inline FdKind  get_kind(void* ptr) {
    return (FdKind)((uintptr_t)ptr >> 24);
}
static inline int     get_fd(void* ptr) {
    return (int)((uintptr_t)ptr & 0x00FFFFFF);
}
```

This avoids a secondary lookup table, keeps the dispatch loop
branch-predictor-friendly, and is safe on both 32-bit and 64-bit ABIs. The
24-bit fd ceiling (16,777,216) is generous — Android's default `fs.nr_open`
is `1048576` on most kernels.

---

## 6. Shared State and Synchronization

The state is split into two categories: **mutable atomics** for hot-path
state, and **lock-protected arrays** for the larger config blobs.

### 6.1 Atomics (relaxed by default)

| Variable                    | Type     | Notes                                         |
|-----------------------------|----------|-----------------------------------------------|
| `g_screen_interactive`      | `bool`   | Set by display callback.                      |
| `g_touching`                | `bool`   | Touch-down latch.                             |
| `g_last_touch_time`         | `uint64_t`| Monotonic ms of last touch transition.       |
| `g_debounce_active`         | `bool`   | True while the 50 ms debounce window is open.|
| `g_curr_idle_rate`          | `int32_t`| Per-app idle target.                          |
| `g_curr_active_rate`        | `int32_t`| Per-app active target.                        |
| `g_last_set_rate`           | `int32_t`| Last rate actually pushed to SurfaceFlinger.  |
| `g_power_save_mode`         | `bool`   | Mirrors `PowerManager.isPowerSaveMode()`.     |
| `g_low_battery_mode`        | `bool`   | True when level ≤ `lowBatteryThreshold`.      |
| `g_battery_level`           | `int32_t`| Most recent level (0–100).                    |
| `g_min_brightness_clamp`    | `bool`   | True when display is dimmed below threshold. |
| `g_touch_slack_ms`          | `int32_t`| Configurable slack window.                    |
| `g_enable_min_brightness`   | `bool`   | Feature toggle.                               |
| `g_min_brightness_threshold`| `int32_t`| % below which brightness clamp activates.     |
| `g_debug`                   | `bool`   | Verbose logcat.                               |
| `g_query_task_pending`      | `bool`   | Set by Binder callbacks; consumed by epoll.  |

These use `memory_order_relaxed` for the steady state, with two
exceptions:

- `g_screen_interactive` and `g_min_brightness_clamp` use
  `memory_order_acquire` on the read side because they gate decisions in
  `updateRateState()`.
- `g_query_task_pending` uses `memory_order_acq_rel` on the
  `atomic_exchange` so the wakeup is causally linked to the consumed
  value.

### 6.2 Why `relaxed` is Safe

The epoll thread is the **sole writer** for almost all of these atomics.
The Binder callbacks only set `g_query_task_pending` and never mutate
any rate or touch state directly. The hot path reads them inside a
single thread, so there is no cross-thread invariant that requires
stronger ordering. Acquires on `g_screen_interactive` and
`g_min_brightness_clamp` are belt-and-braces: they make sure the
slightly later `updateRateState()` sees the new value of any state that
was set in the same callback dispatch.

### 6.3 Locks

| Lock               | Type                  | Protects                                           | Held by                          |
|--------------------|-----------------------|----------------------------------------------------|----------------------------------|
| `g_config_lock`    | `pthread_rwlock_t`    | `g_rules[]`, `g_modes[]`, `g_rule_count`, …       | `loadConfig`/`loadModesMap` (W); `updateRateState`/`updateCurrentAppRates` (R) |
| `g_client_lock`    | `pthread_spinlock_t`  | `g_client_fds[]`, `g_client_count`, `g_last_package_prefixes[]` | `touchListenerThread` only (extremely brief) |

`g_client_lock` is a **spinlock** because its critical section is two or
three `memcpy`/`strlen` calls. A `pthread_mutex` would add two extra
syscalls per acquire/release; a spinlock on the same core costs ~10 ns.
The critical section is bounded to well under the duration of a typical
epoll batch.

---

## 7. The eventfd Wakeup Mechanism

`triggerPollerWakeup()` is the *only* way any thread can interrupt the
epoll loop. It writes to a non-blocking `eventfd` registered as
`FD_WAKEUP`:

```c
static inline void triggerPollerWakeup(void) {
    if (g_wakeup_fd >= 0) {
        if (!atomic_flag_test_and_set_explicit(&g_wakeup_pending,
                                                memory_order_acq_rel)) {
            uint64_t val = 1;
            ssize_t n = write(g_wakeup_fd, &val, sizeof(val));
            if (n != sizeof(val) && errno != EAGAIN) {
                /* Retry once — transient failure (e.g. signal interrupt) */
                n = write(g_wakeup_fd, &val, sizeof(val));
            }
            if (n != sizeof(val)) {
                /* Write truly failed — clear flag so next caller can retry */
                atomic_flag_clear_explicit(&g_wakeup_pending,
                                           memory_order_release);
            }
        }
    }
}
```

Three guarantees fall out of this:

1. **No lost wakeups.** `atomic_flag_test_and_set` is a
   test-and-test-and-set; if a wakeup is already pending, we skip the
   write. The consumer (`FD_WAKEUP` handler) **always** clears the flag
   *after* reading the eventfd, so the next caller will succeed.
2. **No redundant writes.** A second wakeup issued while the first is
   still pending collapses to a single eventfd write.
3. **Crash-safe retry.** If the `write()` fails (kernel backpressure,
   signal), we clear the flag so the next caller can reissue it. The
   initial call site (typically a Binder callback) does not deadlock.

The epoll-side handler:

```c
if (kind == FD_WAKEUP) {
    uint64_t val;
    read(fd, &val, sizeof(val));                /* drain */
    atomic_flag_clear(&g_wakeup_pending);      /* arm */
    if (atomic_exchange(&g_query_task_pending, false))
        queryFocusedTask();
    needs_rate_update = true;
}
```

---

## 8. Hot/Cold Binder Split

`libbinder_ndk.so` exports its API through a flat C ABI that the daemon
resolves at startup. To minimize indirection on the hot path, the resolved
pointers are split into two structs:

```c
/* Hot — read on every foreground change / rate change */
typedef struct __attribute__((aligned(64))) {
    prepareTransaction_t  prepareTransaction;
    transact_t            transact;
    writeInt32_t          writeInt32;
    writeInt64_t          writeInt64;
    readInt32_t           readInt32;
    readFloat_t           readFloat;
    readString_t          readString;
    deleteParcel_t        deleteParcel;
    transaction_code_t    resolvedFocusedTaskCode;
    /* …13 more resolved codes and event flags… */
} HotOps;

/* Cold — only touched during startup, registration, and death */
typedef struct {
    AIBinder*               observer;
    transaction_code_t      resolvedProcessObserverCode;
    void*                   lib;          /* dlopen handle */
    getService_t            getService;
    waitForService_t        waitForService;
    Class_define_t          Class_define;
    new_t                   AIBinder_new;
    associateClass_t        associateClass;
    /* …death recipient, thread pool control… */
} ColdBinderContext;
```

The two structs are placed in separate cache lines (`aligned(64)`) so the
hot path never accidentally dirties the cold line and triggers a
cache-line ping-pong with whatever else is touching it.

`HotBinders` (the acquired system-service binders) is similarly
cache-aligned and is the only state that the rate-change path reads.

---

## 9. Binder Transaction Code Resolution

`setRefreshRate()` issues Binder transaction **1035** to `SurfaceFlinger`,
but the codes for `getFocusedTask` (or `getStackInfo`), the foreground
callback, `isInteractive`, `isPowerSaveMode`, `getBrightness`, the display
event flags, the battery listener, and the `IProcessObserver` registration
all change across Android versions — and some even change between
factory images of the same version on the same vendor.

The daemon resolves them exactly once per build fingerprint:

### 9.1 Cache Layout

The cache file lives at `/data/local/tmp/tx_code.txt` and is keyed by the
Android `BUILD_FINGERPRINT` system property:

```
# DFPS resolver cache v8
# DO NOT EDIT — generated by dfps
# Build: google/panther/panther:14/AP1A.240505.005/12050363:userdebug/dev-keys

focused_task=8
foreground=6
api=ROOT_TASK_INFO
is_interactive=10
is_power_save=24
get_brightness=10
on_display_event=1
register_callback_with_mask=15
register_callback=14
register_battery_listener=3
battery_changed=3
event_display_changed=1
event_display_brightness=4
process_observer=2
```

The file is **0600** (root-only read/write) because the codes leak
implementation details of vendor Binder stubs.

### 9.2 Resolution Algorithm

`resolveTransactionCodes()`:

1. Read `ro.build.fingerprint`.
2. If the cache file exists and the first non-comment line ends with the
   same fingerprint, parse the 14 codes and return immediately.
3. Otherwise, materialize the embedded `resolver.jar` (linker symbol
   `resolver_jar[]`, defined in `resolver_bytes.h`) to a temp file
   `chmod 0600`.
4. `posix_spawn()` `app_process` with `CLASSPATH` set to the JAR and
   `LD_LIBRARY_PATH`/`LD_PRELOAD` **stripped** (so the resolver can't be
   hijacked via library injection). The classpath is built by
   `buildResolverEnv()`.
5. `app_process android.app.CodeResolver` reads the codes via reflection
   against the *system* classes (`IActivityManager`, `IDisplayManager`,
   `IBatteryPropertiesRegistrar`, `IProcessObserver`) and prints them on
   stdout, fingerprint-terminated.
6. Atomically replace the cache: write to a temp file, `fsync`, then
   `rename(2)` over the destination. This avoids a half-written cache
   if the daemon is killed mid-write.
7. Parse the output and populate `g_hot_ops` and `g_cold`.

### 9.3 Failure Modes

- **Cache present but unparseable** → treat as cache miss, re-resolve.
- **`app_process` exits non-zero** → log and continue with whatever codes
  were parsed so far; missing codes are checked at registration time.
- **Mandatory code is still zero** (`g_cold.resolvedProcessObserverCode`
  or `g_hot_ops.resolvedFocusedTaskCode`) → log fatal and exit so
  Magisk/KernelSU respawns the daemon.

### 9.4 Why Not AOT Reflection?

The Java reflection path requires the JVM startup cost, ART warmup, and
a writeable extraction directory. It's acceptable on first boot but not
on every restart. The fingerprint cache makes the common case free and
isolates the JVM into a one-shot child process so the long-lived
daemon stays pure native.

---

## 10. Per-App Rule Hash Table

`dfps.conf` can list up to `MAX_RULES = 256` packages. Looking up the
focused package on every foreground change was, in early versions, a
linear scan — fine for ten rules, but pathological at the limit.
`updateCurrentAppRates()` is on the hot path so a 256-entry strcmp loop
on every app switch was unacceptable.

The current code builds an **open-addressed hash table** with FNV-1a
hashing on every successful `loadConfig()`. The table is rebuilt under
the **write** side of `g_config_lock`, so the **read** side
(`updateCurrentAppRates`) sees a consistent snapshot.

### 10.1 Table Layout

```
┌──────────────────────────────────────────────────────┐
│  RuleHashSlot  slots[RULE_HASH_SLOTS = 512]          │
│  ┌──────────────┬──────────────┬─────────────────┐   │
│  │ hash (u32)   │ index (i32)  │ state (i8)      │   │
│  └──────────────┴──────────────┴─────────────────┘   │
│  state: 0 = empty, 1 = used, -1 = tombstone          │
└──────────────────────────────────────────────────────┘
```

Hash slots are sized to **2 × MAX_RULES** so the load factor stays
≤ 0.5 and linear probing amortizes to a handful of comparisons.

### 10.2 Hash Function

FNV-1a 32-bit:

```c
static inline uint32_t hash_string_fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}
```

### 10.3 Lookup

```c
int findRuleIndex(const char* pkg) {
    uint32_t h = hash_string_fnv1a(pkg);
    uint32_t mask = RULE_HASH_SLOTS - 1;          /* power of two */
    uint32_t i = h & mask;
    for (int probe = 0; probe < RULE_HASH_SLOTS; probe++) {
        RuleHashSlot* s = &g_rule_hash[i];
        if (s->state == 0)        return -1;       /* empty → miss */
        if (s->state == 1 && s->hash == h) {
            /* candidate — verify with strcmp to defeat hash collisions */
            int idx = s->index;
            if (strcmp(g_rules[idx].pkg, pkg) == 0) return idx;
        }
        i = (i + 1) & mask;
    }
    return -1;                                     /* table full */
}
```

Verified with `strcmp` because FNV-1a is not cryptographic; we treat
collisions as a correctness concern, not a security one. Verified misses
are essentially free: every comparison short-circuits on the first
mismatched byte.

### 10.4 Rebuild

`rebuildRuleHash()` is called from `loadConfig()` under
`pthread_rwlock_wrlock(&g_config_lock)`. It:

1. Memset the table to zeros.
2. For each rule, compute FNV-1a and probe until an empty slot is found.
3. Reject the rule (with a `LOGW`) if the table is full — the linear
   fallback in `updateCurrentAppRates()` will still find the rule
   because the **array** is still the source of truth; the table is
   only a cache.

(The linear fallback is left in by design: it makes the hash table a
performance optimization, not a correctness requirement.)

---

## 11. Refresh-Rate Decision Logic

`updateRateState()` is called at the end of every epoll iteration that
flagged `needs_rate_update`. It is the **only** function that calls
`setRefreshRate()` in production.

### 11.1 Precedence Order

The function evaluates a strict cascade. The first matching rule wins.

```
┌─────────────────────────────────────────────────────────────────────┐
│  Screen off            →  offscreenRate (or leave rate untouched)   │
│  Brightness clamped    →  min_physical_rate  (or defaultIdle)      │
│  Touching or in slack  →  curr_active_rate, capped by power-save    │
│  Otherwise             →  curr_idle_rate                            │
└─────────────────────────────────────────────────────────────────────┘
```

In pseudocode:

```text
interactive = g_screen_interactive
if not interactive:
    if offscreenRate > 0: set(offscreenRate)
    return

if g_min_brightness_clamp:
    target = min_physical_rate > 0 ? min_physical_rate : defaultIdle
    set(target)
    return

touching = g_touching
if touching or (now - last_touch < slack):
    target = curr_active_rate
    if power_save_mode or low_battery_mode:
        target = min(target, power_save_max_rate)
    set(target)
else:
    set(curr_idle_rate)
```

### 11.2 `setRefreshRate()`

```text
if rate <= 0: return
if rate > max_physical_rate: clamp to max_physical_rate
if rate == g_last_set_rate: return          /* no-op short-circuit */

id = resolveRootId(rate)                    /* with 1-entry cache */
if id < 0:
    log error
    return
transact(SurfaceFlinger, 1035, id)          /* 1035 = setActiveConfig */
g_last_set_rate = rate
```

`resolveRootId()` keeps a 1-entry cache: `(rate_in, id_out)`. A
single foreground change can produce several rate updates, and a typical
idle → active transition oscillates between exactly two rates, so this
cache turns an O(n) array scan into O(1) in the common case.

If no exact match exists, it returns the closest entry within **30 Hz**
of the requested rate. Anything further is rejected (returning `-1`)
rather than blindly conflating Hz with a different mode ID.

---

## 12. State Machines

### 12.1 Touch State

```
                        BTN_TOUCH value=1
            ┌──────────────────────────────────────────┐
            │                                          ▼
        ┌───────┐                              ┌────────────┐
        │ IDLE  │ ──touch up (slack expires)──▶│ TOUCHING   │
        └───────┘ ◀──────────┐                └────────────┘
            ▲                 │  debounce window expires,
            │                 │  no release event found
            │  debounce 50ms  │
            │  expires +      │
            │  release found  │
            │                 │
            └─────────────────┘
```

The debounce window's purpose is twofold:

1. **Hold detection** — distinguish a single tap from a sustained
   press. The original code polled for releases; the current code keeps
   the touch fds in epoll and re-reads them when the 50 ms window
   expires, extending the window if no release was seen.
2. **Burst absorption** — touchscreen drivers often emit a flurry of
   down/up events during a single physical tap. The debounce collapses
   them.

### 12.2 Screen-Interactive State

```
                 DisplayManager callback
                 (event 1 = DISPLAY_CHANGED)
        ┌─────────────────────────────────────────┐
        │                                         ▼
   ┌────────────┐   screen on          ┌──────────────────┐
   │ OFFSCREEN  │ ────────────────────▶│ INTERACTIVE       │
   └────────────┘ ◀────────────────────┘└──────────────────┘
        screen off                       re-attach touch fds
                                         clear touching & debounce
                                         evaluate brightness
```

On `interactive → off`, the daemon:

- Forces `g_touching = false` and `g_debounce_active = false`.
- Re-attaches touch fds (in case they were closed for low battery).
- Applies the offscreen rate if configured.

On `off → interactive`, it queries PowerManager and DisplayManager for
the current state and recomputes the rate.

### 12.3 Brightness Clamp

```
   DisplayManager.getBrightness()
   brightness <  g_min_brightness_threshold  ?
                │
                ├── no ──▶ g_min_brightness_clamp = false
                │
                └── yes ─▶ g_min_brightness_clamp = true
                                 │
                                 ▼
                         rate clamped to min_physical_rate
```

`g_min_brightness_threshold` is in the same 0–100 range as
`Display.getBrightness()`. The check is gated on
`g_enable_min_brightness` so it can be disabled entirely.

### 12.4 Power-Save Mode

```
   PowerManager.isPowerSaveMode() ────────────▶ g_power_save_mode
                                                            │
   battery_saver feature enabled?                            │
        ├── no  ──▶ g_power_save_mode ignored (always false)
        │
        └── yes ──▶ g_power_save_mode honoured
                          │
                          ▼
              if true: cap active rate at g_power_save_max_rate
```

When power-save is on, both `updateRateState()` and the foreground-rate
computation look at `g_power_save_mode` and clamp the active rate to
`g_power_save_max_rate`. The idle rate is **not** clamped — idle
content is by definition not user-perceived.

### 12.5 Low-Battery Mode (with Hysteresis)

```
                                          threshold
                                       ┌──────────────┐
   level 100 ─────── normal ──────────▶│ 10%          │
   level 50  ─────── normal ──────────▶│              │
   level 11  ─────── normal ──────────▶│              │
   level 10  ─────── enters low ──────▶│              │
   level  9  ─────── still low ───────▶│              │
   level 12  ─────── exits low ───────▶│ 12% (+2%)    │
   level 11  ─────── exits low ───────▶│              │
                                       └──────────────┘
```

Low-battery mode is entered when `level <= lowBatteryThreshold` and is
**not** exited until `level >= lowBatteryThreshold + 2`. The 2-point
hysteresis prevents mode-flap when the level is reported at a coarse
granularity by the kernel. The transition is implemented as a CAS loop
on `g_low_battery_mode` so simultaneous uevent and Binder-pushed level
updates never race.

While in low-battery mode the daemon:

- Closes all touch fds (saves the cost of buffering touch events on a
  device that is about to die).
- Caps the active rate at `g_power_save_max_rate`.

When the level rises back above the hysteresis, `findTouchscreens()` is
re-invoked and the fds are re-attached.

### 12.6 Modes-Map State

```
   modes.map present and parses? ──── no ──▶ every setRefreshRate() fails
        │                                            (log error)
        │ yes
        ▼
   resolveRootId(rate) → exact match? ─── yes ──▶ return id
        │ no
        ▼
   any entry within 30 Hz? ─── yes ──▶ return closest (with warn log)
        │ no
        ▼
   return -1 (setRefreshRate() logs and skips the transaction)
```

---

## 13. Foreground-App Tracking

The daemon needs to know which package is currently focused so it can
look up the matching per-app rule. It has two complementary sources:

### 13.1 `IProcessObserver` Callback

`ActivityManager` invokes `observerOnTransact` whenever a process state
transition occurs (foreground, background, importance change, etc.). The
callback **does not** receive the package name directly — it only
indicates "something changed". On receipt:

1. Set `g_query_task_pending = true`.
2. Call `triggerPollerWakeup()`.

The epoll thread, on the next wakeup, calls `queryFocusedTask()` which
hits `ActivityManager` for the actual list. This indirection keeps the
Binder callback sub-microsecond and avoids a Binder call from inside a
Binder call.

### 13.2 `queryFocusedTask()`

The function:

1. Picks the right transaction code (`resolvedFocusedTaskCode` or
   `resolvedForegroundCode`) based on `resolvedApi`, which is either
   `API_ROOT_TASK_INFO` or `API_STACK_INFO`. The choice is
   version-dependent and is decided by the resolver (see §9).
2. Builds the input parcel (a `NULL` for `getStackInfo` is the protocol;
   a request ID for `getRootTaskInfo`).
3. Sends the transaction, parses the reply.
4. Walks the returned task list, extracts each `taskName` (a
   `String`), and stores it in `g_child_task_names[]`. Task names on
   Android are package names (e.g. `com.android.chrome`).
5. For each task, takes the first 124 bytes (or up to the first `.` —
   either convention works as a "package prefix") and stores it in
   `g_last_package_prefixes[]`.
6. Sets `g_last_package_count`.
7. Calls `emitChangedForegroundPackages()` which diffs against the
   previous snapshot and:
   - Logs the transition.
   - Sends the new list to every connected `@dfps` client.
   - Calls `updateCurrentAppRates()` with the **topmost** package.

The parcel parser is hand-written (`AParcel_readInt32`,
`AParcel_readString`, `AParcel_readFloat`) because the Binder wire
format is a documented but verbose TLV stream.

### 13.3 Why Both a Callback and a Query?

The callback is **latency-critical** (every frame of every app switch).
The query is **correctness-critical** (you need the actual names). The
split is: callback says "wake up", query says "go look".

---

## 14. IPC Surfaces

| Surface                        | Direction    | Purpose                                                     |
|--------------------------------|--------------|-------------------------------------------------------------|
| **Abstract Unix socket @dfps** | Inbound      | Third-party clients subscribe to foreground-app transitions. |
| **Binder — IActivityManager**  | Outbound     | `IProcessObserver` registration; `getRootTaskInfo` / `getStackInfo`. |
| **Binder — ISurfaceComposer**  | Outbound     | `setActiveConfig` (transaction 1035) on rate transitions.    |
| **Binder — IPowerManager**     | Outbound     | `isInteractive`, `isPowerSaveMode`.                         |
| **Binder — IDisplayManager**   | Outbound     | `getBrightness`; `registerCallbackWithEventMask`.           |
| **Binder — IBatteryPropertiesRegistrar** | Outbound | `registerListener`.                                          |
| **Binder — IBatteryPropertiesListener**  | Inbound  | `batteryChanged` callback.                                  |
| **Binder — IProcessObserver**  | Inbound      | Foreground process change notification.                     |
| **Binder — IDisplayManagerCallback** | Inbound | Display change / brightness change events.                 |
| **inotify**                    | Filesystem   | `IN_CLOSE_WRITE \| IN_MOVED_TO` on `/data/local/tmp/dfps/`. |
| **Netlink (NETLINK_KOBJECT_UEVENT)** | Inbound | Battery capacity / status changes.                          |
| **eventfd**                    | Cross-thread | Wakeup from Binder callbacks into the epoll loop.           |
| **signalfd** (implicit)        | Signals      | `SIGTERM` / `SIGINT` → clean shutdown (cleanup fds, unlock).|

The abstract socket is documented in detail in
[`CLIENT_PROTOCOL.md`](CLIENT_PROTOCOL.md).

---

## 15. Lifecycle

### 15.1 Startup

```
main()
  │
  ├─ mlockall(MCL_CURRENT)                   ← pin pages
  ├─ prctl(PR_SET_TIMERSLACK, 0)             ← tight timer wakeups
  ├─ prctl(PR_SET_IO_FLUSHER, 1)             ← flush dirty data quickly
  ├─ sched_setscheduler(SCHED_FIFO, prio 2)  ← real-time
  ├─ setupCpuAffinity()                      ← pin to slow cores
  ├─ initLogging()                           ← dlopen liblog
  ├─ dlopen libbinder_ndk.so                 ← resolve all dlsyms
  ├─ getService(activity, power,
  │            SurfaceFlinger, display,
  │            batteryproperties)
  ├─ associateClass for each                 ← bind to dummy classes
  ├─ resolveTransactionCodes()               ← §9
  ├─ loadConfig() / loadModesMap()           ← §10
  ├─ setupAbstractSocket()                   ← @dfps listener
  ├─ netlink uevent socket (root only)
  ├─ register battery listener               ← via Binder
  ├─ readInitialBatteryLevel()
  ├─ pthread_create(touchListenerThread)     ← §4
  ├─ register display callback               ← via Binder
  ├─ checkInteractiveAndPowerSave()
  ├─ checkMinBrightness()
  ├─ register process observer               ← via Binder
  ├─ queryFocusedTask()                      ← initial state
  └─ joinThreadPool()                        ← hand off to Binder
```

### 15.2 Steady State

The two threads run independently and only synchronize through the
eventfd wakeup. The daemon consumes < 0.5% CPU on a typical idle
device (it spends most of its time blocked in `epoll_wait`).

### 15.3 Shutdown

1. Magisk/KernelSU sends `SIGTERM` (or the user runs `killall dfps`).
2. The signal handler (registered in `main()`) sets
   `g_shutdown_requested = true` and closes the wakeup fd, which
   unblocks the epoll thread.
3. The epoll thread exits its loop, closes all fds, and returns.
4. The main thread returns from `joinThreadPool()` (because the
   process is exiting).
5. `exit()` is called.

If a critical Binder service (ActivityManager, SurfaceFlinger,
DisplayManager, PowerManager) dies, the death recipient calls
`onBinderDied()` which **exits** the process — the module's `service.sh`
re-spawns it. This is intentional: a stale daemon with a dead Binder
client is a worse outcome than a fast restart.

---

## Appendix A — Constant Reference

| Constant                | Value | Source         | Meaning                                          |
|-------------------------|-------|----------------|--------------------------------------------------|
| `MAX_RULES`             | 256   | `dfps.h`       | Maximum per-app rules in `dfps.conf`.            |
| `MAX_MODES`             | 16    | `dfps.h`       | Maximum entries in `modes.map`.                  |
| `MAX_TOUCH_DEVICES`     | 4     | `dfps.h`       | Maximum concurrent touchscreen nodes.            |
| `MAX_CLIENTS`           | 8     | `dfps.h`       | Maximum concurrent `@dfps` socket clients.       |
| `MAX_TASKS`             | 8     | `dfps.h`       | Maximum tracked foreground tasks.                |
| `RULE_HASH_SLOTS`       | 512   | `dfps.h`       | Hash-table size (= 2 × MAX_RULES).               |
| `DEBOUNCE_INTERVAL_MS`  | 50    | `dfps.h`       | Touch debounce window.                           |
| Resolver cache version  | v8    | `binder.c`     | Bumped on each change to the cache format.       |
| Transaction 1035        | 1035  | `rate.c`       | `SurfaceFlinger.setActiveConfig`.                |

## Appendix B — File Layout

```
dfpsd/
├── Makefile
├── README.md
├── docs/                           ← this directory
└── src/
    ├── dfps.h                      ← shared types & globals
    ├── main.c                      ← entry, globals, CPU affinity
    ├── utils.c                     ← logging, abstract socket, env
    ├── config.c                    ← dfps.conf & modes.map parser
    ├── rate.c                      ← rate decision & SF transaction
    ├── binder.c                    ← callbacks, query, resolver
    ├── power.c                     ← battery, brightness, uevent
    ├── touch.c                     ← epoll loop, inotify, touch
    └── resolver/
        ├── CodeResolver.java       ← embedded as JAR
        └── build_resolver.py       ← regenerates resolver_bytes.h
```
