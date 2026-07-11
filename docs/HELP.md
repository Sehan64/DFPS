# Dynamic refresh rate configuration help

> Adapted from the upstream `yc9559/dfps` help
> (`magisk/config/dfps_help_en.md`, Apache-2.0). This fork (**dfpsd**) has
> diverged in a few important ways — see
> [Divergences from upstream](#divergences-from-upstream-yc9559dfps) at the end.
> Where this document and upstream disagree, **this document describes dfpsd
> behavior**.
>
> For the full key/value reference and file format, see
> [`CONFIGURATION.md`](./CONFIGURATION.md).

## Feature options

### `touchSlackMs`

While you interact with the screen, the refresh rate switches to
`defaultActive`, then waits `touchSlackMs` milliseconds after the last input
before dropping back to `defaultIdle`.

This delay avoids rapidly toggling the refresh rate, which would waste power
through switching overhead.

- **Default:** `4000`
- **Valid range:** `0`–`60000` (values outside this range are clamped back to
  `4000`)

### `enableMinBrightness`

On OLED panels, switching the refresh rate at very low brightness can cause a
visible brightness/color shift that looks bad. `enableMinBrightness` clamps the
daemon to the minimum rate and stops touch-boosting whenever the screen
brightness drops below `minBrightnessThreshold`.

- **Default:** `false`
- **Threshold:** `minBrightnessThreshold` is a **percentage** (`0`–`100`), e.g.
  `minBrightnessThreshold = 10` means "clamp when brightness is below 10%".

### `enableFrameRateFlex`

dfpsd always sets the refresh rate through the SurfaceFlinger transaction
`1035`, using `modes.map` to translate a target Hz into the SurfaceFlinger
config ID. `enableFrameRateFlex` additionally toggles SurfaceFlinger transaction
`1036` (a "frame rate flex" policy) on some devices that block cross-group
switching.

- **Default:** `false`
- There is **no** native `PEAK_REFRESH_RATE` code path in dfpsd. See
  [Divergences](#divergences-from-upstream-yc9559dfps).

## Battery saver (dfpsd addition)

These keys are not present in upstream dfps:

- `batterySaver` (`true`/`false`) — when on, respect the system power-save mode
  and low-battery mode.
- `lowBatteryThreshold` (`0`–`100`) — battery percentage that enters low-battery
  mode.
- `powerSaveMaxRate` (`Hz`) — maximum active rate while power-saving is active.

They have no effect unless `batterySaver = true`.

## Per-app configuration

Format:

```text
packageName = idleValue activeValue
```

- `idleValue` — refresh rate (Hz) used when the app is shown but not being
  touched.
- `activeValue` — refresh rate (Hz) used while you are touching the screen.
- `-1` on either side means **inherit the current default** (`defaultIdle` /
  `defaultActive`) for that side.

Example:

```text
com.miHoYo.Yuanshen = 60 60
com.hypergryph.arknights = 60 60
com.android.launcher3 = 60 60
```

The rule table is capped at 256 entries; lookups are exact and case-sensitive.

> **Note:** dfpsd does **not** use the upstream `-` (offscreen) or `*` (default)
> rule tokens. Offscreen behavior is controlled by the global `offscreenRate`
> key instead (see [`CONFIGURATION.md`](./CONFIGURATION.md)).

## Divergences from upstream `yc9559/dfps`

| Area | Upstream `yc9559/dfps` | dfpsd (this fork) |
|---|---|---|
| Brightness scale | `enableMinBrightness` is `0`–`255` (device-specific table) | `minBrightnessThreshold` is a **percentage** `0`–`100` |
| Rate-switch method | `useSfBackdoor`: `0` = native `PEAK_REFRESH_RATE` (value ≥ 20 Hz), `1` = SurfaceFlinger backdoor (value < 20 = config index) | Always SurfaceFlinger `1035` + `modes.map`; `enableFrameRateFlex` toggles `1036`. **No** native `PEAK_REFRESH_RATE` path. |
| Per-app syntax | `packageName idle active` (space-separated) | `packageName = idle active` (`=` separated) |
| Special tokens | `-` = offscreen rule, `*` = default rule, `-1` = system default | No `-`/`*` tokens; offscreen = global `offscreenRate`; `-1` = inherit fork default |
| Battery saver | not present | `batterySaver` / `lowBatteryThreshold` / `powerSaveMaxRate` added |
| Implementation | C++ (spdlog / scnlib) | C (POSIX, no external runtime deps) |
