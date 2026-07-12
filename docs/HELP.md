# Configuration help

Practical guide to `dfps.conf` options. Full syntax and reload rules:
[`CONFIGURATION.md`](./CONFIGURATION.md).

This fork (dfpsd) diverges from upstream `yc9559/dfps` in several places —
see [Divergences](#divergences-from-upstream-yc9559dfps).

## Getting a good first setup

1. Let the module installer write `modes.map` from `dumpsys SurfaceFlinger`,
   or hand-write Hz → SF id pairs that match your panel.
2. Set `defaultIdle` / `defaultActive` to rates that exist in `modes.map`.
3. Add per-app rules only where defaults feel wrong (games, video, launchers).
4. Leave `batterySaver` / brightness clamp off until the basics work, then
   enable them if you care about OLED shift or battery.

## Feature options

### `touchSlackMs` (default `4000`)

After the last touch, keep the **active** rate for this many milliseconds
before dropping to idle. Avoids thrashing the panel on short gestures.

- Valid range `0`–`60000`; out-of-range values fall back to `4000`.
- Contacts shorter than 80 ms (hard-coded debounce) never engage active rate,
  so phantom multitouch noise does not ramp the panel.

### `enableMinBrightness` / `minBrightnessThreshold`

On OLED panels, rate switches at very low brightness can cause a visible
brightness or color shift. When enabled, brightness ≤ `minBrightnessThreshold`
(percent, `0`–`100`) forces the **minimum physical** rate from `modes.map`
(or `defaultIdle` if the min is unknown) and suppresses touch boost.

Default: off / threshold `0`.

### `enableFrameRateFlex`

dfpsd always drives the panel via SurfaceFlinger vendor transaction **`1035`**
and `modes.map`. Setting this true also toggles transaction **`1036`**
(“frame-rate flex”) on ROMs that block cross-group switches. Harmless no-op
when the ROM rejects `1036` (logged once as a warning).

There is **no** native `PEAK_REFRESH_RATE` settings path in this fork.

## Battery saver

Only active when `batterySaver = true`:

| Key | Role |
|---|---|
| `lowBatteryThreshold` | Enter low-battery mode at this battery % (exit at threshold+2 hysteresis) |
| `powerSaveMaxRate` | Cap applied to the **active** rate while system power-save **or** low-battery is on |

Idle rate is not capped. Power-save mode is polled every 30 s (no Binder
callback for it). Battery level comes from Binder listener, netlink uevent,
or a cached sysfs path.

## Per-app rules

```text
packageName = idleHz activeHz
```

- `-1` inherits the current global default for that side.
- Exact package match only (case-sensitive); max 256 rules.
- No upstream `-` (offscreen) or `*` (default) tokens — use `offscreenRate`
  and the global defaults instead.

```text
com.miHoYo.Yuanshen = 60 60
com.android.launcher3 = 60 90
com.example.reader = 30 -1
```

## Offscreen

`offscreenRate = N` forces `N` Hz while the screen is not interactive.
`-1` (default) means “do not drive the panel while off.”

## Divergences from upstream `yc9559/dfps`

| Area | Upstream | dfpsd |
|---|---|---|
| Brightness | `enableMinBrightness` 0–255 device table | `minBrightnessThreshold` as **percent** 0–100 |
| Rate method | Optional `PEAK_REFRESH_RATE` vs SF backdoor | Always SF `1035` + `modes.map`; `enableFrameRateFlex` → `1036` |
| Per-app syntax | `pkg idle active` (spaces) | `pkg = idle active` |
| Special tokens | `-` offscreen, `*` default | Globals only; `-1` inherits fork defaults |
| Battery | not present | `batterySaver` / `lowBatteryThreshold` / `powerSaveMaxRate` |
| Implementation | C++ | C11, no external runtime deps |
