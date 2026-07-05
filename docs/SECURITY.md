# Security Considerations

DFPS runs as a long-lived daemon with elevated privileges — either
**root** (via Magisk / KernelSU / Axeron) or via an **ADB shell**
session in non-root setups. Anything with that much privilege is a
meaningful security surface, and DFPS has been written with an explicit
threat model in mind. This document:

1. States the threat model in concrete terms.
2. Walks through the attack surface and the mitigations already in place.
3. Lists the **known limitations** that users should be aware of.
4. Provides a hardening checklist for paranoid environments.

> **Audience.** Auditors, security researchers, and operators deploying
> DFPS in environments with mixed trust.

---

## Table of Contents

1. [Threat Model](#1-threat-model)
2. [Trust Boundaries](#2-trust-boundaries)
3. [Attack Surface Inventory](#3-attack-surface-inventory)
4. [Mitigations In Place](#4-mitigations-in-place)
5. [Known Limitations](#5-known-limitations)
6. [Hardening Checklist](#6-hardening-checklist)
7. [Reporting a Vulnerability](#7-reporting-a-vulnerability)

---

## 1. Threat Model

### 1.1 What DFPS Trusts

| Trusted                          | Why                                                |
|----------------------------------|----------------------------------------------------|
| The Android kernel               | Standard assumption.                               |
| The Magisk / KernelSU / Axeron supervisor | Standard assumption — the supervisor is what spawns DFPS. |
| `libbinder_ndk.so`               | Provided by the OS, in `/system/lib[64]`.          |
| `liblog.so`                      | Same.                                              |
| The contents of `/data/local/tmp/dfps/` and `/data/local/tmp/tx_code.txt` | Written and read only by root processes (DFPS, the resolver child, the WebUI). |
| A properly fingerprinted cache file | Verified against `ro.build.fingerprint`.          |

### 1.2 What DFPS Does **Not** Trust

| Untrusted                              | Why                                       |
|----------------------------------------|-------------------------------------------|
| Any other process on the device        | No ambient authority is required to read the public socket, so other apps can connect. |
| The contents of `dfps.conf` / `modes.map` | Owned by the user / WebUI; if the user can't be trusted, neither can the config. |
| The contents of `ro.build.fingerprint` | Read but never interpreted as code.        |
| The system property `ro.product.model` | Used only for log messages.               |

### 1.3 Adversary Capabilities Considered

| Adversary                          | Capabilities                                          |
|------------------------------------|-------------------------------------------------------|
| **Unprivileged local app**         | Can `connect()` to the abstract socket. Cannot write to DFPS's files. |
| **Rooted local app**               | Can do anything the DFPS daemon can do. (In particular, the threat model **does not** protect against an attacker who already has root.) |
| **ADB shell user**                 | Can read DFPS's configuration; can send `SIGTERM`. Can connect to the socket. |
| **Network attacker**               | Out of scope — DFPS does not bind any network port. The abstract socket is local-only. |
| **A malicious modes.map**          | Can cause DFPS to request a non-existent SurfaceFlinger mode (which `setActiveConfig` will reject). Cannot crash the daemon. |
| **A malicious dfps.conf**          | Can specify nonsensical rates. All values are bounds-checked to the 1–1000 Hz range. Cannot execute code. |

### 1.4 Out of Scope

- **Defense against an attacker who has root before DFPS starts.**
  Once an attacker is root, DFPS is a piece of the system they
  control. No userspace process can defend against this.
- **Confidentiality of foreground-app names.** Android exposes them
  to any app holding `QUERY_ALL_PACKAGES`; DFPS does not make this
  worse.
- **Defense against a compromised kernel.** Out of scope for any
  userspace daemon.

---

## 2. Trust Boundaries

```
┌────────────────────────────────────────────────────────────────────┐
│                         Untrusted Zone                              │
│   ┌─────────────────────────┐    ┌──────────────────────────────┐  │
│   │  Other apps (no root)   │    │  ADB shell                   │  │
│   │  → can connect to @dfps │    │  → can read dfps.conf        │  │
│   │                         │    │  → can send SIGTERM          │  │
│   └─────────────────────────┘    └──────────────────────────────┘  │
└────────────────────┬───────────────────────────────────────────────┘
                     │ abstract socket (read-only)
                     ▼
┌────────────────────────────────────────────────────────────────────┐
│                       Privileged Zone (root)                        │
│   ┌──────────────────────────────────────────────────────────┐    │
│   │                        dfps daemon                       │    │
│   │  • reads dfps.conf, modes.map                            │    │
│   │  • reads /data/local/tmp/tx_code.txt                     │    │
│   │  • calls into libbinder_ndk (system)                     │    │
│   │  • touches /sys/devices/system/cpu/cpu*/cpufreq          │    │
│   │  • (root mode) opens /dev/input/event*                   │    │
│   │  • (root mode) binds NETLINK_KOBJECT_UEVENT              │    │
│   └──────────────────────────────────────────────────────────┘    │
│   ┌──────────────────────────────────────────────────────────┐    │
│   │  app_process child (one-shot, only on cache miss)       │    │
│   │  • spawned with stripped LD_LIBRARY_PATH / LD_PRELOAD    │    │
│   │  • reads /system/framework/* (read-only)                 │    │
│   └──────────────────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────────────────┘
```

The boundary between trusted and untrusted is the **`@dfps` socket**.
Crossing it is unidirectional (clients → daemon: nothing; daemon →
clients: snapshots).

---

## 3. Attack Surface Inventory

| Surface                                  | Type      | Notes                                          |
|------------------------------------------|-----------|------------------------------------------------|
| `@dfps` abstract socket                  | Inbound   | Read-only, no auth. 8-client cap.              |
| `/data/local/tmp/tx_code.txt`            | Read      | Keyed by `ro.build.fingerprint`.               |
| `/data/local/tmp/dfps/dfps.conf`         | Read      | Hot-reloaded via inotify.                      |
| `/data/local/tmp/dfps/modes.map`         | Read      | Hot-reloaded via inotify.                      |
| Binder (system services)                 | Inbound   | Three callbacks: observer, display, battery.   |
| Binder (system services)                 | Outbound  | `setActiveConfig` (1035), getStackInfo, etc.   |
| `/sys/devices/system/cpu/cpu*/cpufreq/...` | Read     | CPU affinity detection at startup.             |
| `/dev/input/event*`                      | Read      | Root mode only.                                |
| `NETLINK_KOBJECT_UEVENT`                 | Read      | Root mode only.                                |
| `/system/bin/app_process`                | Spawn     | One-shot, only on cache miss.                  |
| `libbinder_ndk.so`, `liblog.so`          | dlopen    | System libraries, no LD_PRELOAD.               |

There is **no** network surface, no filesystem write outside
`/data/local/tmp/dfps/` and `/data/local/tmp/`, and no IPC mechanism
that accepts commands from clients.

---

## 4. Mitigations In Place

### 4.1 Resource Limits

- `MAX_RULES = 256` — bounds `dfps.conf` parsing.
- `MAX_MODES = 16` — bounds `modes.map` parsing.
- `MAX_TOUCH_DEVICES = 4` — bounds `/dev/input` scan.
- `MAX_CLIENTS = 8` — bounds `@dfps` connection table.
- `MAX_TASKS = 8` — bounds per-snapshot payload.
- All numeric config values are range-checked to plausible bounds
  (rates 1–1000 Hz, thresholds 0–100, slack 0–60000 ms).
- Hash table for rule lookup is 512 slots with 2× load factor — a
  pathological config cannot degrade lookup to O(n) beyond
  `MAX_RULES`.

### 4.2 Process Hardening (the daemon)

```c
/* In main(): */
mlockall(MCL_CURRENT);                 /* pin pages — prevents paging attack */
prctl(PR_SET_TIMERSLACK, 0);           /* tight wakeups */
prctl(PR_SET_IO_FLUSHER, 1);           /* flush dirty data quickly */
sched_setscheduler(SCHED_FIFO, 2);    /* real-time, no preemption */
setpriority(PRIO_PROCESS, 0, -20);     /* fallback if SCHED_FIFO denied */
sigprocmask(SIG_BLOCK, [SIGPIPE, SIGWINCH]); /* ignore noisy signals */
```

- All file descriptors opened with `O_CLOEXEC` — no fd leaks to
  children.
- All `socket()` calls use `SOCK_CLOEXEC` and (where supported)
  `SOCK_NONBLOCK`.
- `accept4()` is used in lieu of `accept()` for the same reason.

### 4.3 Resolver Sandboxing

The one-shot `app_process` child is started with a custom environment:

```c
char** buildResolverEnv(const char* jar_path) {
    /* Strip CLASSPATH, LD_LIBRARY_PATH, LD_PRELOAD from inherited env. */
    /* Inject only CLASSPATH=<jar_path>. */
}
```

This prevents:

- **Library injection** — `LD_PRELOAD` is removed.
- **Library search path tampering** — `LD_LIBRARY_PATH` is removed.
- **Classpath injection** — only the resolver's own JAR is on the
  classpath.

The child writes the resolver JAR to `/data/local/tmp/resolver.jar`
mode `0600` and unlinks it on exit. The JAR contents are a literal
compiled from the trusted source in `dfpsd/src/resolver/`.

### 4.4 Cache Hardening

`/data/local/tmp/tx_code.txt`:

- Mode `0600` — root-only.
- Keyed by `ro.build.fingerprint` — an attacker cannot substitute
  codes from a different device build.
- Versioned (`v=7`) — old cache files are ignored.

### 4.5 No Network Surface

DFPS does not bind any port, does not call `getaddrinfo`, does not
perform DNS resolution, and does not read network configuration. The
abstract socket is local-only and `AF_UNIX`.

### 4.6 Stack Protector + RELRO + BIND_NOW

The default `Makefile` (release profile) builds with:

```make
CFLAGS += -fstack-protector-strong
LDFLAGS += -Wl,-z,relro -Wl,-z,now -Wl,--as-needed
```

This is enabled in the default build, not an opt-in flag.

### 4.7 Strict `epoll` / `read` / `send` Discipline

- All `send()`s to clients use `MSG_NOSIGNAL | MSG_DONTWAIT` —
  DFPS **never** dies from a client disconnect, and **never** blocks
  on a slow client.
- All `read()`s are bounded — no `read(fd, buf, BIG_NUMBER)`.
- `epoll_wait()` is bounded to 64 events per call.

### 4.8 Process Owner Discipline

- The daemon **exits** (and is respawned by the supervisor) on death
  of any critical Binder service. This is a security feature, not a
  bug: a daemon that lives on with stale Binder clients is worse than
  a fast restart.
- `mlockall(MCL_CURRENT)` ensures the daemon's working set stays in
  RAM, denying cold-boot attacks via swap.

---

## 5. Known Limitations

These are not vulnerabilities per se, but they are properties a
defensive operator should know.

### 5.1 Unauthenticated Public Socket

The `@dfps` socket has **no authentication**. Any process on the
device can connect and read the foreground-app list.

**Mitigation available to you:** restrict access to the abstract
namespace by running the daemon inside a network namespace
(`ip netns add …`); this is not done by default because it would
break the WebUI's ability to connect via root.

**Why this is acceptable:** Android already exposes the same
information via `dumpsys activity activities`, `cmd activity
get-task`, and the `GET_TASKS` permission. DFPS does not make the
situation worse.

### 5.2 `dfps.conf` and `modes.map` Are Root-Writable

The configuration files are owned by root. The WebUI writes them via
`ksu.exec()` (KernelSU) or `magisk_exec()` (Magisk). An attacker with
root on the device can replace them.

**Mitigation available to you:** use file capabilities
(`setcap cap_dac_override=ep /system/bin/dfps`) to give the daemon
explicit override rights without granting it full root. This is
out-of-scope for the default build.

**Why this is acceptable:** an attacker with root has already won.

### 5.3 Transaction-Code Cache Is a Covert Channel

The fingerprint-keyed cache file contains the Binder transaction
codes of the local system. These are not secret per se, but they
allow a reader to fingerprint the exact OS build of the device.

**Mitigation in place:** file mode `0600`; only the daemon and root
processes can read it.

### 5.4 Resource Exhaustion by Misconfiguration

A user-supplied `dfps.conf` with 256 valid per-app rules and a
64-rule `modes.map` is the worst-case for memory (≈ 32 KB +
≈ 1 KB). This is bounded. A user-supplied `dfps.conf` that sets
`touchSlackMs=60000` will simply keep the active rate for 60 s
after the last touch — annoying, but not a security issue.

### 5.5 `SCHED_FIFO` Privilege

The daemon requests `SCHED_FIFO` priority 2. On hardened kernels
(`grsec`, `PaX`), this may be denied. The fallback is
`setpriority(PRIO_PROCESS, 0, -20)`, which is a normal nice value.
Functionally equivalent for our purposes; latency will be higher.

### 5.6 Touch FDs Hold Process Privileges

In root mode, the daemon opens `/dev/input/event*` directly. On
kernels with `kptr_restrict` or input-device ACLs, this may require
SELinux policy adjustment. The default Magisk policy is
permissive-enough to allow it.

### 5.7 No App-Layer Trust Boundary

The daemon does not verify the **caller's** identity for the
abstract socket. A malicious local app with root could
masquerade-as-DFPS to a downstream consumer. This is mitigated
by the fact that the protocol is **read-only** — DFPS never
acts on a client request.

### 5.8 No Input Length Validation on Modes Map

`modes.map` is read with `fscanf("%d %d")` in a loop. Lines longer
than the input buffer are truncated by `fgets`, which is safe. The
`%d` conversion is bounded to `INT_MAX`. Out-of-range rates are
rejected (see [`CONFIGURATION.md`](CONFIGURATION.md) for the exact
bounds).

---

## 6. Hardening Checklist

For high-security environments:

- [ ] **Disable the WebUI** if you don't need it. The WebUI
      invokes the daemon via `ksu.exec()`/`magisk_exec()` and is
      therefore another path for a rooted local app to mutate
      `dfps.conf`. Remove or `chmod 0` the `webroot/` directory in
      the module.
- [ ] **Restrict the abstract socket.** Bind the daemon to a
      separate network namespace and only enter it from your
      monitoring process. (Out of the default scope; requires
      custom build.)
- [ ] **Audit `dfps.conf` regularly.** Any change to the file is
      reflected in logcat. A simple cron-based audit:
      ```sh
      adb shell sha256sum /data/local/tmp/dfps/dfps.conf
      ```
- [ ] **Pin the device's `ro.build.fingerprint`.** The
      transaction-code cache is keyed by it; a different fingerprint
      forces a one-shot `app_process` respawn.
- [ ] **Monitor for unexpected re-spawns.** Each `app_process`
      invocation is logged with the new fingerprint. If you see a
      re-spawn without a corresponding OTA, investigate.
- [ ] **Disable `batterySaver` mode if you don't need it.** It
      involves another IPC path and a periodic sysfs poll. With it
      off, the daemon doesn't read `/sys/class/power_supply/` at
      all.
- [ ] **Set `DEBUG=false`** in `dfps.conf` in production. The
      `LOGI` calls are no-ops when the flag is false, so the
      daemon's logcat noise drops to zero.
- [ ] **Set `enableMinBrightness=false`** if you don't need the
      feature. It removes the brightness-clamp IPC path entirely.

---

## 7. Reporting a Vulnerability

If you find a security issue:

1. **Do not** open a public GitHub issue.
2. Email the maintainer (see `README.md` for the contact address).
3. Include:
   - Device model and Android version.
   - `ro.build.fingerprint`.
   - DFPS version (`module.prop`).
   - Reproduction steps.
   - Proof-of-concept (if available).
4. Allow up to 90 days for a coordinated disclosure. The
   maintainer will acknowledge within 7 days.

A security advisory will be published on the GitHub repository once
a fix is available.
