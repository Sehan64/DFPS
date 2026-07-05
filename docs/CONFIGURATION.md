# Configuration

All runtime configuration lives in two files under `/data/local/tmp/dfps/`,
both of which are symlinks to files inside the module directory:

```text
/data/local/tmp/dfps/dfps.conf  →  $MODPATH/dfps.conf   (read by the daemon)
/data/local/tmp/dfps/modes.map  →  $MODPATH/modes.map   (read by the daemon)
```

The daemon watches its working directory with `inotify` for
`IN_CLOSE_WRITE | IN_MOVED_TO` events and reloads the config within
~1 second of any change. The WebUI exploits this by writing a new
`dfps.conf` and then `mv`-ing it twice to force a `MOVED_TO` event without
restarting the daemon.

---

## Table of Contents

- [File: `dfps.conf`](#file-dfpsconf)
  - [Syntax](#syntax)
  - [Global parameters](#global-parameters)
  - [Brightness clamp](#brightness-clamp)
  - [Battery saver](#battery-saver)
  - [System rules](#system-rules)
  - [Per-app rules](#per-app-rules)
  - [Full example](#full-example)
- [File: `modes.map`](#file-modesmap)
  - [Format](#format)
  - [Regenerating](#regenerating)
- [Hot-reload semantics](#hot-reload-semantics)
- [Validation and limits](#validation-and-limits)

---

## File: `dfps.conf`

### Syntax

`dfps.conf` is a flat key/value file, one entry per line, in the form:

```text
key = value
```

- **Case-insensitive** for keys (`DEBUG`, `debug`, `Debug` are identical).
- **Case-sensitive** for package names in per-app rules.
- **Comments** start with `#` and run to end of line.
- **Whitespace** around `=` and at the ends of lines is ignored.
- **Blank lines** are ignored.
- **Boolean values** accept `true` / `false` / `1` (case-insensitive for
  `true`/`false`).
- **Integer values** are parsed with `strtol`; out-of-range values are
  rejected and the default is used instead (with a warning).
- **Unknown keys** that have exactly two integer tokens on the right are
  treated as per-app rules.

The order of keys does not matter. Duplicate keys result in the last one
winning.

---

### Global parameters

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `DEBUG` | bool | `false` | `true`/`false`/`1` | If `true`, the daemon logs informational and warning messages to `logcat` (tag `DFPS_Daemon`) and stderr. If `false`, only errors are logged. |
| `touchSlackMs` | int (ms) | `4000` *(code)* / `2000` *(module default)* | `0`–`60000` | How long the display stays at the **active** rate after the last touch event before reverting to the **idle** rate. `0` reverts immediately on touch release. |

---

### Brightness clamp

When enabled, the daemon forces the lowest physical refresh rate while the
display is dim. This is useful for always-on displays, night reading, or
simply to save battery when the backlight is barely on.

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `enableMinBrightness` | bool | `false` | `true`/`false`/`1` | When `true`, the daemon queries brightness via `IDisplayManager.getBrightness()` (Binder) and applies the clamp when it is below the threshold. |
| `minBrightnessThreshold` | int (%) | `0` | `0`–`100` | Brightness percentage below which the clamp engages. `0` is a no-op even when the feature is enabled. |

The brightness value is queried once when the display fires a
`DisplayManagerCallback` event (screen on, brightness change, etc.), not on
every touch.

---

### Battery saver

Two related mechanisms, both opt-in.

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `batterySaver` | bool | `false` | `true`/`false`/`1` | Master switch. When `false`, neither the system power-save mode nor low-battery mode affect refresh rates. |
| `lowBatteryThreshold` | int (%) | `10` | `0`–`100` | Battery percentage below which the daemon enters low-battery mode and caps the active rate to `powerSaveMaxRate`. Has ±2 % hysteresis to avoid oscillation around the threshold. |
| `powerSaveMaxRate` | int (Hz) | `60` | `1`–`1000` | Maximum active rate allowed in either system power-save mode or low-battery mode. The active rate is reduced to this cap, the idle rate is left alone. |

System power-save mode is detected via `IPowerManager.isPowerSaveMode()`
(Binder). Low-battery mode is detected from the `IBatteryPropertiesListener`
callback *and* from `NETLINK_KOBJECT_UEVENT` (`SUBSYSTEM=power_supply` plus
`POWER_SUPPLY_CAPACITY=…`).

If both `batterySaver` is on and the battery is below the threshold, the
**stricter** of the two caps wins (they are the same value, so this is
effectively a single cap).

---

### System rules

The defaults used when no per-app rule applies, plus the screen-off behavior.

| Key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `defaultIdle` | int (Hz) | `60` *(code)* / device-min *(module)* | `1`–`1000` | Idle (low) refresh rate. Used when no per-app rule matches. Also used during the touch slack. |
| `defaultActive` | int (Hz) | `120` *(code)* / device-max *(module)* | `1`–`1000` | Active (high) refresh rate. Used during touch and for the first 1–4 s after. |
| `offscreenRate` | int (Hz or `-1`) | `-1` *(code)* / device-min *(module)* | `1`–`1000` or `-1` | Forced rate while the screen is off. `-1` disables the override and lets the system / OEM set whatever it wants. |

The `defaultIdle` / `defaultActive` are also the values inherited by a
per-app rule that uses `-1` for that side.

> **Note.** The module's `customize.sh` rewrites the install-time defaults
> based on the values in `modes.map`. So a device whose lowest rate is
> 30 Hz and whose highest is 144 Hz will get `defaultIdle=30` and
> `defaultActive=144` after install, not the compile-time defaults in the
> source.

---

### Per-app rules

A per-app rule pins a specific package name to a specific pair of rates.
The format is:

```text
<packageName> = <idleHz> <activeHz>
```

For example:

```text
com.miHoYo.Yuanshen = 60 120
com.android.launcher3 = 60 60
```

Rules for individual sides may be omitted by using `-1`, which means
"inherit the current default":

```text
# Always 120 Hz when touched, but use the device's default idle rate
com.example.app = -1 120

# Use the device's default active rate, but force 30 Hz when idle
com.example.boring = 30 -1
```

Notes on per-app rules:

- **Lookup is exact, case-sensitive.** The daemon's hash table is keyed on
  the full package name. A rule for `com.example.app` will not match
  `com.example.app.subprocess` — those need their own rule.
- **First match wins.** The hash table is built by reading `dfps.conf`
  top-to-bottom and inserting into slots. Order doesn't matter for lookups,
  but having duplicates for the same package is undefined behavior (the
  first one inserted wins).
- **Maximum 256 rules** (`MAX_RULES` in the source).
- **Set to `-1 -1` to remove a rule** (or delete the line and save). The
  WebUI provides a *Reset to Default* button for this.
- The package name field is treated as the *key* of an unknown config line;
  the daemon only treats it as a rule if the right-hand side parses as
  two integers.

---

### Full example

```text
# ==========================================
# Dynamic FPS Controller Configuration
# ==========================================

# --- Global Parameters ---
# Enable debug logging in logcat
DEBUG = false

# Delay (in ms) before reverting from Active to Idle rate after touch
touchSlackMs = 2000

# --- Brightness Controls ---
# Clamp to min rate and stop touch-boosting on low brightness
enableMinBrightness = false

# Brightness percentage to trigger the clamp (0-100%)
minBrightnessThreshold = 10

# --- Battery Saver ---
# Enable battery saving features (true/false)
batterySaver = false

# Battery percentage to trigger power save mode
lowBatteryThreshold = 10

# Max refresh rate when power save is on (Hz)
powerSaveMaxRate = 60

# --- System Rules ---
# Default rates used when an app has no specific rule
defaultIdle = 60
defaultActive = 120

# Refresh rate forced when the display turns off (-1 to do nothing)
offscreenRate = 60

# --- App-Specific Rules ---
# Format: packageName = idleRate activeRate
# Use -1 to inherit the default rates above.

# Games (locked for thermals and consistent frame pacing)
com.miHoYo.Yuanshen = 60 120
com.hypergryph.arknights = 60 120
com.mobile.legends = 60 120

# Launcher
com.android.launcher3 = 60 60
```

---

## File: `modes.map`

`modes.map` is a static lookup table that maps a refresh rate in Hz to the
internal SurfaceFlinger display configuration ID that the daemon needs to
pass to `setRefreshRate()`. It is **not** the same as the standard
`getSupportedModes()` index — the IDs here are the values that
`SurfaceFlinger.setActiveConfig()` accepts directly.

### Format

One entry per line, two integers separated by whitespace:

```text
<rate_hz> <sf_display_id>
```

- `rate_hz` is the human-facing refresh rate, e.g. `60`, `90`, `120`, `144`.
- `sf_display_id` is the configuration ID returned by `dumpsys
  SurfaceFlinger` and accepted by `SurfaceFlinger`'s `setActiveConfig` /
  `setRefreshRate` Binder transaction.

Example:

```text
60 0
90 1
120 2
144 3
```

Limits enforced by the daemon:

- Maximum **16 entries** (`MAX_MODES`).
- Rates must be in `1`–`1000` Hz; out-of-range lines are skipped with a
  warning.
- IDs must be non-negative.
- Duplicate rates are kept in order of appearance; the first exact match
  is used.
- If the requested rate has no exact match, the daemon picks the closest
  rate within **30 Hz** (capped). If nothing is within 30 Hz, the request
  fails with a log error.

### Regenerating

The module's `customize.sh` regenerates `modes.map` on every install by
running `dumpsys SurfaceFlinger` and pattern-matching every line that
contains an `id=` token and a `refreshRate=`, `Hz`, or `fps=` token. Three
display layouts are supported:

```text
{id=0, hwcId=0, ... refreshRate=60.00 Hz}
id=1, ... 120Hz
0: 1080x2400, ... refresh=60.000000
```

To force a regeneration after a display driver update, reinstall the
module. To regenerate manually:

```bash
dumpsys SurfaceFlinger | grep -E 'id=[0-9]+.*(refreshRate=|Hz|fps=)' | \
    sed -nE 's/.*id=([0-9]+).*refreshRate=([0-9]+).*/\2 \1/p' | \
    sort -u -k1,1n > /data/local/tmp/dfps/modes.map
```

The daemon will hot-reload it the next time you save `dfps.conf` (since
inotify watches the whole directory, not just the file).

If `modes.map` is missing or empty, the daemon will still run but will log
*"Failed mapping N Hz to an ID in modes.map!"* every time it tries to
change the rate. Nothing breaks; the screen just won't change.

---

## Hot-reload semantics

The daemon's inotify watch on `/data/local/tmp/dfps/` triggers on:

- `IN_CLOSE_WRITE` — a file was written and closed in place.
- `IN_MOVED_TO` — a file was moved into the directory.

For each event whose filename is `dfps.conf`, the daemon:

1. Re-parses the entire config file from scratch (no incremental updates).
2. Validates every value.
3. Commits the result under the config write lock.
4. Rebuilds the per-app rule hash table.
5. Re-evaluates the rate for the current foreground package, if any
   (otherwise it re-evaluates the defaults).

`modes.map` is only loaded at startup and on explicit reload via
`loadModesMap()` — it is not watched by inotify. To reload it, restart
the daemon or reinstall the module.

The WebUI saves the config with this sequence to force a `MOVED_TO` event
without a service restart:

```sh
echo "..." > /data/local/tmp/dfps/dfps.conf
mv /data/local/tmp/dfps/dfps.conf /data/local/tmp/dfps/dfps.conf.tmp
mv /data/local/tmp/dfps/dfps.conf.tmp /data/local/tmp/dfps/dfps.conf
```

`mv` from the watched directory and back is what produces the
`IN_MOVED_TO` event; a plain overwrite produces `IN_CLOSE_WRITE` which is
also accepted.

---

## Validation and limits

| Limit | Value | Where enforced | Behavior on violation |
|---|---|---|---|
| Max per-app rules | 256 | `MAX_RULES` in `dfps.h` | Additional rules are silently dropped. |
| Max SurfaceFlinger modes | 16 | `MAX_MODES` in `dfps.h` | Additional lines are silently dropped. |
| Max touch devices | 4 | `MAX_TOUCH_DEVICES` in `dfps.h` | Additional `/dev/input/event*` devices are ignored. |
| Max concurrent client connections | 8 | `MAX_CLIENTS` in `dfps.h` | New connections are refused with a log warning. |
| Max foreground tasks tracked | 8 | `MAX_TASKS` in `dfps.h` | The first 8 are kept; the rest are dropped. |
| `touchSlackMs` | 0–60000 ms | `config.c` | Out of range is reset to `4000`. |
| All `*Hz` values | 1–1000 | `config.c` | Out of range is reset to the key's compile-time default. |
| `lowBatteryThreshold` / `minBrightnessThreshold` | 0–100 | `config.c` | Out of range is reset to its default. |
| `offscreenRate` | 1–1000 or `-1` | `config.c` | Out of range is reset to `-1`. |

All limits are compile-time constants. To change them, edit `src/dfps.h` and
rebuild.
