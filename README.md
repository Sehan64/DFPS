# DFPS — Dynamic FPS Controller for Android

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)
[![Platform: Android 8.0+](https://img.shields.io/badge/Platform-Android%208.0%2B-green.svg)](#-requirements)
[![Arch: arm64-v8a](https://img.shields.io/badge/Arch-arm64--v8a-red.svg)](#-requirements)
[![Language: C11](https://img.shields.io/badge/Language-C11-A8B9CC.svg)](#-architecture)
[![Module: Magisk / KernelSU / Axeron](https://img.shields.io/badge/Module-Magisk%20%7C%20KernelSU%20%7C%20Axeron-orange.svg)](docs/INSTALLATION.md)

A tiny, root-resident C daemon that switches your Android device's display
refresh rate in real time, based on touch activity, the foreground app,
screen brightness, and battery state. Shipped as a tri-compatible root module
(Magisk + KernelSU + Axeron) with a built-in WebUI.

> **Static binary. ~48 KB stripped. Two system libraries.** No daemons, no
> services, no extra processes beyond `dfps` itself.

---

## Table of Contents

- [Highlights](#highlights)
- [How it works](#how-it-works)
- [What you get](#what-you-get)
- [Quick install](#quick-install)
- [Quick start (from source)](#quick-start-from-source)
- [WebUI at a glance](#webui-at-a-glance)
- [Default behavior](#default-behavior)
- [Requirements](#requirements)
- [Configuration overview](#configuration-overview)
- [Client interface](#client-interface)
- [Performance & footprint](#performance--footprint)
- [Project layout](#project-layout)
- [Documentation](#documentation)
- [Compatibility matrix](#compatibility-matrix)
- [Contributing](#contributing)
- [License](#license)

---

## Highlights

- **Touch-responsive rate switching** — ramps to the active rate on touch,
  falls back to idle after a configurable delay (the *slack*).
- **Per-app rules** — pin any package to its own idle/active rates via
  `dfps.conf` or the WebUI.
- **Brightness clamp** — when the screen is dim, force the lowest rate to
  save power.
- **Battery saver / low-battery mode** — cap the maximum rate when battery
  saver is on or the battery drops below a threshold.
- **Live config reload** — edit `dfps.conf` and the daemon picks up changes
  within ~1 second via inotify, no restart required.
- **Abstract Unix socket** (`@dfps`) — companion apps, shells, Tasker, and
  Magisk modules can query the live foreground-package list.
- **Root & non-root modes** — runs as a privileged daemon under Magisk,
  KernelSU, or Axeron, but also works on unrooted devices with reduced
  functionality (Binder-only, no touch input).
- **Tri-compatible module** — one zip installs on Magisk, KernelSU, *and*
  Axeron without changes.

---

## How it works

The daemon holds an open Binder connection to `SurfaceFlinger` and issues a
single `setRefreshRate` transaction whenever the desired rate changes. State
changes are driven by:

| Source | What it tells the daemon |
|---|---|
| `/dev/input/event*` (touch) | "User is touching — go active." |
| `IProcessObserver` (Binder) | "Foreground app changed — apply per-app rule." |
| `IDisplayManagerCallback` (Binder) | "Screen turned on/off, brightness changed." |
| `IPowerManager` (Binder) | "Battery saver toggled, isInteractive changed." |
| `IBatteryPropertiesListener` (Binder) | "Battery level changed." |
| `NETLINK_KOBJECT_UEVENT` | "Power supply capacity changed." (root only) |
| inotify on `/data/local/tmp/dfps` | "Config was edited — reload." |
| abstract socket `@dfps` | "A client connected — send it the current packages." |

Two threads, one `epoll(7)` loop, a handful of atomic variables, and a small
hash table for per-app rules. No locks held during Binder transactions.

---

## What you get

The shipping zip is a single Magisk + KernelSU + Axeron module that contains:

- `system/bin/dfps` — the stripped ARM64 binary.
- `module.prop`, `customize.sh`, `service.sh`, `action.sh`, `uninstall.sh` —
  the standard root-module scripts. `customize.sh` auto-detects your
  display's `modes.map` by parsing `dumpsys SurfaceFlinger` and writes a
  sensible default `dfps.conf`.
- `webroot/index.html` — a self-contained WebUI (no external assets, no
  network calls) for KernelSU / Axeron that lists every installed app and
  lets you set per-app rates with two taps.

No companion app is required. A terminal is enough.

---

## Quick install

1. **Download** `dfps.zip` from the [Releases](../../releases) page.
2. **Open** your root manager:
   - **Magisk** → *Modules* → *Install from storage* → pick `dfps.zip` → *Reboot*.
   - **KernelSU** → *Modules* → install `dfps.zip` → reboot (or hot-reload).
   - **Axeron** → *Plugins* → install `dfps.zip` → reload.
3. **Verify** after boot:

   ```bash
   su -c /data/local/tmp/dfps/dfps  # starts manually if service.sh hasn't
   logcat -s DFPS_Daemon            # you should see "Initiated"
   ```

That's it. The default config auto-detected at install time works on most
devices out of the box. Open the WebUI in KernelSU / Axeron to tune
per-app rules.

> See [docs/INSTALLATION.md](docs/INSTALLATION.md) for full instructions,
> what the install scripts actually do, and how to uninstall.

---

## Quick start (from source)

```bash
# In Termux (or any Linux with clang):
pkg install clang make   # Termux
git clone https://github.com/Sehan64/DFPS.git
cd DFPS

# Drop the embedded resolver JAR into src/resolver_bytes.h (see
# docs/BUILDING.md for how to generate it), then:
make              # release build for current architecture
make install      # installs to /data/local/tmp/dfps/bin/dfps
su -c /data/local/tmp/dfps/bin/dfps
```

Build profiles:

| Command | Output |
|---|---|
| `make` | release build, `-O3 -flto -march=armv8-a`, stripped |
| `make debug` | debug build, ASan + UBSan, not stripped |
| `make check` | syntax check only |
| `make clean` | remove `bin/` |

See [docs/BUILDING.md](docs/BUILDING.md) for the full toolchain guide.

---

## WebUI at a glance

KernelSU / Axeron serve the bundled HTML at `ksu://webui/dfps` (or
`axeron://webui/dfps`). The UI lists every installed user app, lets you
search, filter *All / Custom / Default*, and edit per-app rules:

- Choose a custom *Idle* and *Active* rate, or select *Default* to inherit.
- Changes auto-save to `dfps.conf`; the daemon hot-reloads within ~1 s.
- Global settings (debug, slack, brightness clamp, battery saver) are
  exposed in the *Settings* sheet.

A *Reset to Default* option removes a per-app rule.

---

## Default behavior

The default `dfps.conf` written by `customize.sh` on first install:

- **Idle rate:** the lowest refresh rate advertised by SurfaceFlinger
  (typically 60 Hz).
- **Active rate:** the highest advertised rate (typically 90, 120, or 144 Hz
  on modern devices).
- **Touch slack:** 2000 ms — the screen stays at the active rate for 2 s
  after the last touch event.
- **Brightness clamp:** *off* (set the threshold in the WebUI to enable).
- **Frame-rate flexibility:** *off* (optional SurfaceFlinger policy override
  for devices that block cross-group switching).
- **Battery saver:** *off* (will engage automatically when the system
  reports it; cap configurable).
- **Low battery:** threshold 10 %, max rate 60 Hz when below it.
- **Offscreen rate:** equal to the idle rate.

Pre-baked example rules for popular games are included (Genshin, Arknights,
Mobile Legends) pinned to the device's max refresh rate for consistent
frame pacing.

---

## Requirements

- **Android** 8.0 (API 26) or newer.
- **arm64-v8a** (aarch64). The module and Makefile target this by default.
- **Root** for full functionality. Non-root builds can only use the Binder
  path (no touch input, no netlink battery events).
- One of: **Magisk** (≥ 24), **KernelSU** (any recent build), or
  **Axeron** (1.x or newer).
- A device whose `SurfaceFlinger` advertises more than one refresh rate on
  its primary display. (Otherwise the daemon is a no-op.)

---

## Configuration overview

All configuration lives in `/data/local/tmp/dfps/`:

| File | Purpose |
|---|---|
| `dfps.conf` | Daemon settings and per-app rules. Hot-reloaded. |
| `modes.map`  | Maps refresh rates (Hz) to SurfaceFlinger display IDs. Auto-generated by `customize.sh` on first install. |

Every key in `dfps.conf` is documented in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).
Quick reference:

| Key | Type | Default | Effect |
|---|---|---|---|
| `DEBUG` | bool | `false` | Verbose logging. |
| `touchSlackMs` | int | `2000` | Hold active rate this long after the last touch. |
| `enableFrameRateFlex` | bool | `false` | Enable SurfaceFlinger transaction `1036` to allow cross-group refresh switching. |
| `enableMinBrightness` | bool | `false` | Clamp to min rate when dim. |
| `minBrightnessThreshold` | 0–100 | `0` | Brightness % to trigger the clamp. |
| `batterySaver` | bool | `false` | Honor the system's power-save mode. |
| `lowBatteryThreshold` | 0–100 | `10` | Enter low-battery mode below this %. |
| `powerSaveMaxRate` | Hz | `60` | Max rate under power-save / low-battery. |
| `defaultIdle` | Hz | device min | Fallback idle rate. |
| `defaultActive` | Hz | device max | Fallback active rate. |
| `offscreenRate` | Hz or -1 | device min | Forced rate while screen is off; `-1` disables. |
| `<packageName>` | `idle active` | inherits defaults | Per-app rule. Use `-1` to inherit a side. |

---

## Client interface

Connect to the abstract Unix socket `@dfps` to receive the live
foreground-package list. Every change is pushed as a single newline-delimited
message: package names separated by spaces, each truncated at the first
`/` (process name). On connect, the current list is sent immediately.

```bash
# Show the current foreground app:
nc -U "@dfps"   # macOS / BSD nc
socat - UNIX-CONNECT:"@dfps" <<< ""   # Linux
```

Full protocol and examples in [docs/CLIENT_PROTOCOL.md](docs/CLIENT_PROTOCOL.md).

---

## Performance & footprint

| Metric | Value |
|---|---|
| Static binary size | ~48 KB stripped |
| Resident memory | < 4 MB RSS (heap + thread stacks) |
| Background CPU | ~0% (epoll blocks indefinitely between events) |
| Wakeup sources | touch, Binder callbacks, inotify, uevent, abstract socket |
| Binder roundtrips per second | 0 in steady state; spikes on screen/battery events |
| Files opened at boot | ~10, all under `/data/local/tmp/dfps/`, `/sys/class/power_supply/`, and `/dev/input/` |

The daemon pins itself to the efficiency cluster (lowest `cpuinfo_max_freq`)
when it can detect one, and uses `SCHED_FIFO` priority 2 with `mlockall` to
minimize scheduling latency.

---

## Project layout

```
DFPS/
├── Makefile                 # build system (release/debug profiles)
├── LICENSE                  # Apache-2.0
├── README.md                # this file
├── docs/                    # full documentation
└── src/                     # C source
    ├── dfps.h               # master header (types, globals, helpers)
    ├── main.c               # entry point, CPU affinity, shutdown, init
    ├── utils.c              # logging, abstract socket, resolver env
    ├── config.c             # dfps.conf and modes.map parsing, rule hash table
    ├── rate.c               # refresh-rate decision and application
    ├── binder.c             # Binder IPC, transaction resolution, callbacks
    ├── power.c              # battery, brightness, power-save, uevent
    ├── touch.c              # touch input, inotify, main event loop
    └── resolver_bytes.h     # embedded helper JAR (generated, not in repo)
```

---

## Documentation

| Document | Description |
|---|---|
| [docs/INSTALLATION.md](docs/INSTALLATION.md) | Magisk / KernelSU / Axeron install, verify, uninstall. |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Every `dfps.conf` key, `modes.map` format, hot-reload semantics. |
| [docs/BUILDING.md](docs/BUILDING.md) | Build from source, generate `resolver_bytes.h`, cross-compile, debug build. |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Threads, fds, state machines, hash table, wakeup, transaction resolution. |
| [docs/CLIENT_PROTOCOL.md](docs/CLIENT_PROTOCOL.md) | `@dfps` socket protocol and client examples. |
| [docs/SECURITY.md](docs/SECURITY.md) | Threat model, known limitations, hardening, vulnerability reporting. |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Common issues, log analysis, recovery procedures. |
| [docs/MODULE_PACKAGING.md](docs/MODULE_PACKAGING.md) | Magisk / KernelSU / Axeron module internals, version-code split, WebUI. |

---

## Compatibility matrix

| Component | Tested with | Notes |
|---|---|---|
| Android version | 8.0 (API 26) → 14 (API 34) | Uses NDK Binder API since API 29; falls back to libbinder_ndk loaded dynamically. |
| ABIs | `arm64-v8a` | The shipping module is arm64-only. x86_64 and armeabi-v7a are buildable via `make` but not shipped. |
| Root managers | Magisk 24+, KernelSU (any), Axeron 1.x+ | The same zip installs in all three. The `axeronPlugin=` line in `module.prop` opts in to Axeron's plugin system. |
| SoCs | Tested on Snapdragon, Dimensity, Exynos, Tensor | The efficiency-cluster detection works on all big.LITTLE designs we have data for. |
| Displays | 60 / 90 / 120 / 144 Hz, dual-rate and multi-rate | Any panel that exposes its rates through `dumpsys SurfaceFlinger` is supported. |

---

## Contributing

Contributions are welcome. To keep the project maintainable:

1. Fork the repository and create a feature branch.
2. Keep patches focused — one logical change per PR.
3. Test on a real device (root and non-root modes) before submitting.
4. Match the existing C style (see `src/` for conventions):
   - `-std=gnu11`, `-Wall -Wextra -Wno-unused-parameter`.
   - Avoid hidden global allocations in hot paths.
   - Prefer atomics over locks for single-word state.
5. Update `docs/` whenever behavior changes.
6. Run `make check` before pushing; full release builds should produce zero
   warnings.

For security reports, see [docs/SECURITY.md](docs/SECURITY.md).

---

## License

DFPS is licensed under the **Apache License, Version 2.0**.

```
Copyright 2024 Sehan64

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

See the full license text in [LICENSE](LICENSE).
