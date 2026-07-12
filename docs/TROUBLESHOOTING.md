# Troubleshooting

Start here:

```bash
logcat -s DFPS_Daemon
su -c 'pgrep -af /data/local/tmp/dfps/dfps'
su -c '/data/local/tmp/dfps/dfps --version'
```

Enable `DEBUG = true` in `dfps.conf` for info/warn detail (hot-reloaded).

## Does not start

| Log / symptom | Action |
|---|---|
| `@dfps already in use` | `pkill -TERM -f dfps` and retry; only one instance |
| `libbinder_ndk.so open error` | Device too old or broken system image |
| `Core … transaction code mapping failed` | Resolver JAR / cache problem; delete tx cache under `/data/local/tmp/dfps/` and restart so the helper re-runs; confirm `app_process` works |
| Binary missing | Reinstall module; check `/data/local/tmp/dfps/dfps` |

## Exits immediately after start

- Binder death of ActivityManager / SurfaceFlinger / DisplayManager / Power
- Incomplete service map after a ROM update
- SELinux denials (check `dmesg` / `logcat` for avc)

Reboot after clean reinstall if a stale observer registration confuses the
next start.

## Refresh rate never changes

1. `modes.map` has ≥1 valid row that matches real SF ids  
   (`dumpsys SurfaceFlinger` / display modes).
2. `STATUS` shows expected `idle`/`active` and a changing `last`.
3. Touch path: root, devices registered in log, screen interactive, hold
   longer than 80 ms debounce, wait out `touchSlackMs` for idle drop.
   If the rate thrashs 60↔90 while idle, check for phantom MT contacts in
   logcat (`DEBUG=true`) and that PowerManager/DisplayManager prepareTransaction
   no longer logs “Class is not set”.
4. Overrides: `minbright=1` or `powersave`/`lowbatt=1` with a low cap.
5. OEM may ignore SF tx `1035` — try `enableFrameRateFlex = true` for `1036`.

Missing/empty `modes.map` after a good load **keeps** the old map; first boot
with no map cannot switch until a valid file appears.

## Touch not detected

- Non-root skips `/dev/input` registration by design.
- SELinux or missing `CAP_SYS_ADMIN`-class access to input nodes.
- Screen off: kernel often suppresses events; state resets on off edge.
- Phantom contacts are debounced (50 ms); very quick taps may not boost.

## Wrong foreground package

- Inspect the `@dfps` push line (root/shell).
- Package is taken from focused root-task **childTaskNames** (`pkg/cls`),
  not from `IProcessObserver` uid/pid (those are triggers only).
- Some ROMs report process names; rules must match the string dfps sees.
- Exact case-sensitive match; max 256 rules.

## Battery / brightness wrong

| Feature | Requires | Notes |
|---|---|---|
| Low battery / power-save cap | `batterySaver=true` | Power-save polled every 30 s; level via Binder, uevent, or sysfs |
| Brightness clamp | `enableMinBrightness=true` | Needs display callback event 4 (mask includes bit 8) or legacy poll |

Binder battery parcels differ by OEM; absurd jumps (>30 points) are ignored
and sysfs/uevent stay authoritative. Dock/keyboard supplies are filtered when
the main `Battery` supply name is known.

## Config reload ignored

- Save must produce `IN_CLOSE_WRITE` or `IN_MOVED_TO` on the watch dir
  (`/data/local/tmp/dfps`). Atomic replace (`mv` into place) is fine.
- Conf missing → defaults; modes missing/empty → keep prior map (look for
  the keep-warning in logcat).
- Per-app change applies after reload for the **current** FG package without
  waiting for the next app switch.

## Module WebUI

- Manager must support the bundled WebUI scheme; `webroot/index.html` present.
- Reinstall if the zip was hand-edited. Unrelated to the daemon core if
  `dfps` itself is healthy in logcat.
