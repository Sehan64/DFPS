# Operations

Runtime runbook for the `dfps` daemon. For protocol details see
`docs/CLIENT_PROTOCOL.md`; for startup/hardening see `docs/INIT.md`.

## Process

- Runs as root. One instance only — the `@dfps` singleton socket blocks a
  second copy at startup.
- Started at boot by `service.sh` or the `dfps.rc` init service.
- Stops cleanly on `SIGTERM` / `SIGINT`: the event loop unwinds and `main()`
  returns, so the supervisor respawns a fresh instance if needed.

```bash
# Is it running?
su -c 'pgrep -af /data/local/tmp/dfps/dfps'

# Stop / restart (init will respawn via service.sh / dfps.rc)
su -c 'pkill -TERM -f /data/local/tmp/dfps/dfps'
```

## Version

```bash
su -c '/data/local/tmp/dfps/dfps --version'
# dfpsd 1.0.0 (build Jul 11 2026 19:04:12)
```

The build stamp is compiled in (`__DATE__` / `__TIME__`), useful for
confirming which binary is deployed on a device.

## Health check (STATUS)

Connect to the `@dfps` abstract socket as root or `adb shell`, then send
`STATUS\n`. The daemon replies with one line:

```text
idle=60 active=120 last=120 interactive=1 powersave=0 lowbatt=0 minbright=0 callback=1 uptime_ms=123456
```

| Field        | Meaning                                                 |
|--------------|---------------------------------------------------------|
| `idle`       | current idle (no-touch) refresh rate, Hz               |
| `active`     | current active (touching) refresh rate, Hz             |
| `last`       | last rate actually written to SurfaceFlinger, Hz      |
| `interactive`| 1 if the screen is interactive (on / unlocked)         |
| `powersave`  | 1 if battery-saver mode is active                      |
| `lowbatt`    | 1 if low-battery mode is active                        |
| `minbright`  | 1 if the brightness clamp is forcing the min rate      |
| `callback`   | 1 if the IDisplayManager callback is registered        |
| `uptime_ms`  | milliseconds since the daemon started                  |

Any command other than `STATUS` is treated as a protocol violation and the
client is disconnected.

## Logging

Logs go through `logcat` under the `DFPS_Daemon` tag:

```bash
logcat -d -s DFPS_Daemon
```

Build with `make debug` for verbose (`-DDEBUG`) output.

## Live configuration reload

`dfps.conf` and `modes.map` live under `/data/local/tmp/dfps/`. The daemon
watches them with `inotify` and reloads on write — no restart required.

- A malformed `dfps.conf` falls back to defaults (absurd `defaultIdle` /
  `defaultActive` are clamped to 60 / 120; bad per-app rules are dropped).
- A missing file also reverts to defaults rather than keeping stale state.

After editing, just save the file; the next foreground / touch / battery
event applies the new values.

## Common checks

- **No refresh-rate change** — confirm `modes.map` has more than one rate and
  `dfps.conf` has sensible `defaultIdle` / `defaultActive`.
- **Touch not detected** — root mode is required for `/dev/input/event*`;
  check SELinux / input-device access.
- **Foreground list looks wrong** — inspect it over the `@dfps` socket, or
  check `IProcessObserver` resolution; some ROMs report process names.
- **Daemon exits immediately** — look for Binder service-death messages;
  confirm `ActivityManager`, `PowerManager`, `SurfaceFlinger`,
  `DisplayManager`, and `batteryproperties` are available.

See `docs/TROUBLESHOOTING.md` for the full list.
