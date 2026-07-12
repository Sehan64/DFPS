# Installation

## Requirements

- Android 8+ (needs `libbinder_ndk.so`)
- Root via Magisk, KernelSU, or Axeron
- Panel with more than one SurfaceFlinger refresh-rate config
- Vendor SF transactions `1035` (set active config) present; `1036` optional

## Module install

1. Flash `dfps.zip` in your root manager.
2. Reboot (or hot-reload if the manager supports it).
3. Confirm the process:

```bash
su -c 'pgrep -af /data/local/tmp/dfps/dfps'
logcat -d -s DFPS_Daemon
```

## What the installer sets up

Typical module layout (names may vary slightly by packaging):

- installs the daemon binary (often as `system/bin/dfps`)
- writes initial `dfps.conf` and `modes.map`
- places runtime copies / symlinks under `/data/local/tmp/dfps/`
- starts the daemon at boot via `service.sh`

`customize.sh` usually derives `modes.map` from `dumpsys SurfaceFlinger` and
picks sensible default idle/active rates. Always verify the map against your
panel after install.

## Directory permissions

Prefer:

```bash
su -c 'mkdir -p /data/local/tmp/dfps && chown root:root /data/local/tmp/dfps && chmod 700 /data/local/tmp/dfps'
```

A world-writable runtime dir lets any local app rewrite rates or the mode map.
The daemon logs a warning and best-effort `chmod 0700` when it owns the dir.

## Manual / development install

```bash
make
make install   # copies to /data/local/tmp/dfps/bin/dfps
su -c 'cp /data/local/tmp/dfps/bin/dfps /data/local/tmp/dfps/dfps'
# write dfps.conf + modes.map, then:
su -c '/data/local/tmp/dfps/dfps'
```

Only one instance can bind `@dfps`. Stop an existing copy first.

## Verify

```bash
su -c '/data/local/tmp/dfps/dfps --version'
su -c 'pgrep -af dfps'
# health (root or adb shell):
printf 'STATUS\n' | su -c 'nc -U @dfps'   # if nc supports abstract; else use a small SO_PEERCRED client
logcat -s DFPS_Daemon
```

See [`OPS.md`](./OPS.md) for STATUS field meanings and [`CLIENT_PROTOCOL.md`](./CLIENT_PROTOCOL.md)
for the socket protocol.

## Uninstall

Remove the module in the root manager and reboot, or run the manager’s
uninstall path. Manually stop any leftover process:

```bash
su -c 'pkill -TERM -f /data/local/tmp/dfps/dfps'
```
