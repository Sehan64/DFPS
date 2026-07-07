# Installation

`dfps.zip` is a single archive for Magisk, KernelSU, and Axeron.

## Requirements

- Android 8.0 or newer
- root through Magisk, KernelSU, or Axeron
- a display with more than one SurfaceFlinger refresh rate

## Install

1. Open your root manager.
2. Install `dfps.zip` from storage.
3. Reboot or hot-reload if the manager supports it.
4. Verify that `/data/local/tmp/dfps/dfps` is running.

## What the installer sets up

- installs `system/bin/dfps`
- writes `dfps.conf` and `modes.map`
- creates `/data/local/tmp/dfps/dfps`, `/data/local/tmp/dfps/dfps.conf`, and
  `/data/local/tmp/dfps/modes.map` symlinks
- starts the daemon on boot through `service.sh`

`customize.sh` auto-detects display modes from `dumpsys SurfaceFlinger` and
writes a default config with sensible idle and active rates.

## Verify

```bash
su -c pgrep -af dfps
logcat -d -s DFPS_Daemon
```

If the process is running, the socket and rate control should also be active.

## Uninstall

Remove the module from your root manager and reboot, or run the manager's
uninstall flow for the module.
