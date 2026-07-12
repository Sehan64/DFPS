# Configuration reference

User-oriented rationale for each option lives in [`HELP.md`](./HELP.md).
This page is the normative format and reload rules.

Runtime directory (watched by inotify):

```text
/data/local/tmp/dfps/
  dfps.conf
  modes.map
```

## `dfps.conf`

```text
key = value
# comment
com.example.game = 60 120
```

Rules:

- Keys are case-insensitive; package names are case-sensitive.
- `#` starts a comment; blank lines are ignored.
- Unknown keys become per-app rules when the value is two integers.
- At most 256 per-app rules. Lookups are exact (hash table).

### Global keys

| Key | Type | Default | Notes |
|---|---|---|---|
| `DEBUG` | bool | `false` | Verbose `logcat` / stderr |
| `touchSlackMs` | int ms | `4000` | Hold active rate after last touch; out of `0`–`60000` → `4000` |
| `enableFrameRateFlex` | bool | `false` | Toggle SF transaction `1036` |
| `enableMinBrightness` | bool | `false` | Enable brightness clamp |
| `minBrightnessThreshold` | int % | `0` | Clamp when brightness ≤ this % (`0`–`100`) |
| `batterySaver` | bool | `false` | Honor system power-save + low battery |
| `lowBatteryThreshold` | int % | `10` | Enter low-battery mode at this % (`0`–`100`) |
| `powerSaveMaxRate` | int Hz | `60` | Cap on **active** rate while power-saving / low-batt |
| `defaultIdle` | int Hz | `60` | Fallback idle; absurd values → `60` |
| `defaultActive` | int Hz | `120` | Fallback active; absurd values → `120` |
| `offscreenRate` | int Hz | `-1` | Rate while screen off; `-1` = do not drive |

Booleans accept `true`, `false`, or `1` (and common truthy/falsey spellings the parser accepts).

### Per-app rules

```text
com.example.game = 60 120
com.example.boring = 30 -1
```

| Field | Meaning |
|---|---|
| first integer | Idle Hz while app is focused and not touching |
| second integer | Active Hz while touching (subject to power-save cap) |
| `-1` | Inherit current `defaultIdle` / `defaultActive` for that side |

There is no `*` default-rule token and no `-` offscreen token (use `offscreenRate`).

## `modes.map`

Maps target Hz to SurfaceFlinger config IDs used by vendor transaction `1035`.

```text
# Hz  SF-id
60  0
90  1
120 2
```

- One pair per line; `#` comments allowed.
- Rates must be in `(0, 1000]`; IDs must be ≥ 0.
- Closest-match within 30 Hz is used if an exact rate is missing.
- Empty or missing map **keeps the previously loaded map** so rate control does not freeze mid-run. First boot with no file starts empty (cannot change the panel until a valid map appears).

## Reload behavior

| File | Missing / empty | Valid content |
|---|---|---|
| `dfps.conf` | Reset globals to built-ins; clear per-app rules | Replace rules + globals atomically under the config lock |
| `modes.map` | Keep prior map if any; log a warning | Replace map; invalidate the one-entry rate→id cache |

No daemon restart is required. After a conf reload, rates for the current
foreground package are re-evaluated in the same epoll iteration.

## Example

```text
# /data/local/tmp/dfps/dfps.conf
DEBUG = false
touchSlackMs = 4000
defaultIdle = 60
defaultActive = 120
offscreenRate = -1
enableMinBrightness = true
minBrightnessThreshold = 10
batterySaver = true
lowBatteryThreshold = 15
powerSaveMaxRate = 60
enableFrameRateFlex = false

com.android.launcher3 = 60 90
com.miHoYo.Yuanshen = 60 60
```

```text
# /data/local/tmp/dfps/modes.map
60 0
90 1
120 2
```
