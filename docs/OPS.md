# Operations

Day-to-day runbook. Protocol: [`CLIENT_PROTOCOL.md`](./CLIENT_PROTOCOL.md).
Boot / hardening: [`INIT.md`](./INIT.md). Failures: [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md).

## Process lifecycle

- One instance only: binding abstract `@dfps` fails if already taken.
- Boot: module `service.sh` (or equivalent) starts
  `/data/local/tmp/dfps/dfps` as root.
- Stop: `SIGTERM` / `SIGINT` → event loop unwinds → `main()` returns so the
  supervisor can respawn. A second signal during the handler calls `_exit`.
- Binder death of a critical service requests the same graceful shutdown.

```bash
su -c 'pgrep -af /data/local/tmp/dfps/dfps'
su -c 'pkill -TERM -f /data/local/tmp/dfps/dfps'
su -c '/data/local/tmp/dfps/dfps --version'
# dfpsd 1.0.0 (build v1.0.0-3-gabcdef)
```

## Health: `STATUS`

Connect as root, the daemon UID, or `adb shell` (UID 2000), then send
`STATUS\n`. Reply (one line):

```text
idle=60 active=120 last=120 interactive=1 powersave=0 lowbatt=0 minbright=0 callback=1 uptime_ms=123456
```

| Field | Meaning |
|---|---|
| `idle` / `active` | Current app rates (Hz) after rules |
| `last` | Last rate written to SurfaceFlinger (`-1` if never) |
| `interactive` | Screen on / interactive |
| `powersave` | System power-save mode |
| `lowbatt` | Low-battery mode (hysteresis around threshold) |
| `minbright` | Brightness clamp engaged |
| `callback` | IDisplayManager callback registered |
| `uptime_ms` | Monotonic ms since process start |

Any other command disconnects the client.

On connect, the daemon also pushes the current foreground package list
(space-separated, newline-terminated) before any request.

## Logging

Tag: `DFPS_Daemon`. Errors always log; info/warn need `DEBUG = true` in
`dfps.conf` (or a debug build’s logging path).

```bash
logcat -s DFPS_Daemon
logcat -d -s DFPS_Daemon | tail -100
```

stderr also receives lines when the process has a console (useful under
`su -c` foreground runs).

## Live reload

Edit `/data/local/tmp/dfps/dfps.conf` or `modes.map` and save. inotify
(`IN_CLOSE_WRITE` / `IN_MOVED_TO`) reloads without restart.

| File | Missing / empty | Bad lines |
|---|---|---|
| `dfps.conf` | Revert to built-in defaults; clear rules | Drop bad lines; clamp absurd scalars |
| `modes.map` | **Keep** previous map | Ignore bad lines; keep prior map if zero valid rows |

After conf reload, the current foreground package’s rates are reapplied
immediately.

## Common checks

| Symptom | Check |
|---|---|
| No rate change | `modes.map` has ≥2 usable rates; `defaultIdle`/`defaultActive` map; `last` in STATUS |
| Touch ignored | Running as root; `/dev/input` open; screen interactive; wait past 50 ms debounce |
| Wrong app rates | Package string on `@dfps` push; exact match in conf |
| Power-save inert | `batterySaver=true`; wait up to 30 s for timer poll; STATUS `powersave`/`lowbatt` |
| Brightness clamp inert | `enableMinBrightness=true`; change brightness to fire event 4 |
| Immediate exit | logcat for binder death / tx resolve failure / socket in use |

## Safe restart

```bash
su -c 'pkill -TERM -f /data/local/tmp/dfps/dfps'
# wait for service.sh / init to respawn, or:
su -c '/data/local/tmp/dfps/dfps &'
```
