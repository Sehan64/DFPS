# Configuration

DFPS reads two runtime files from `/data/local/tmp/dfps/`:

- `dfps.conf`
- `modes.map`

Both are hot-reloaded with inotify. If a file is missing, DFPS now falls back
to defaults instead of keeping stale state.

## `dfps.conf`

Format:

```text
key = value
```

Rules:

- keys are case-insensitive
- package names are case-sensitive
- `#` starts a comment
- blank lines are ignored
- unknown keys become per-app rules if the right-hand side is two integers

### Global keys

| Key | Meaning |
|---|---|
| `DEBUG` | enable verbose logging |
| `touchSlackMs` | keep the active rate for this many milliseconds after touch |
| `enableFrameRateFlex` | toggle SurfaceFlinger transaction `1036` |
| `enableMinBrightness` | enable brightness-based clamping |
| `minBrightnessThreshold` | brightness percentage that enables the clamp |
| `batterySaver` | respect system power-save mode and low-battery mode |
| `lowBatteryThreshold` | battery percentage that enters low-battery mode |
| `powerSaveMaxRate` | max active rate while power-saving |
| `defaultIdle` | fallback idle rate |
| `defaultActive` | fallback active rate |
| `offscreenRate` | rate used while the screen is off, or `-1` to disable |

Boolean values accept `true`, `false`, or `1`.

### Per-app rules

```text
com.example.game = 60 120
com.example.boring = 30 -1
```

- `-1` means inherit the current default for that side.
- The rule table is capped at 256 entries.
- Lookups are exact and case-sensitive.

## `modes.map`

Format:

```text
<refresh_rate_hz> <surfaceflinger_id>
```

Example:

```text
60 0
90 1
120 2
```

DFPS uses this map to translate a target Hz into the SurfaceFlinger config ID
required by transaction `1035`.

## Reload behavior

- `dfps.conf` reloads update rules and defaults.
- `modes.map` reloads update the Hz-to-ID map.
- If reload fails, the previous state is cleared and the built-in defaults are
  used.
