# Installation

The shipping artifact is a single zip, **`dfps.zip`**, that is simultaneously
a Magisk module, a KernelSU module, and an Axeron plugin. One zip, three
managers, zero forks.

> If you want to build from source instead, see [BUILDING.md](BUILDING.md).

---

## Table of Contents

- [Supported managers](#supported-managers)
- [Before you install](#before-you-install)
- [Install — Magisk](#install--magisk)
- [Install — KernelSU](#install--kernelsu)
- [Install — Axeron](#install--axeron)
- [Verify the installation](#verify-the-installation)
- [What the installer actually does](#what-the-installer-actually-does)
- [First-boot behavior](#first-boot-behavior)
- [Uninstall](#uninstall)
- [Manual install (no root manager)](#manual-install-no-root-manager)

---

## Supported managers

| Manager | Min version | Notes |
|---|---|---|
| **Magisk** | 24.0 | Uses `service.sh` to start the daemon at boot. |
| **KernelSU** | Any recent (GKI 1.0+) | WebUI available at `ksu://webui/dfps`. |
| **Axeron** | 1.x | Auto-registered as a plugin via `axeronPlugin=14000` in `module.prop`. WebUI at `axeron://webui/dfps`. |

The module is identical across the three; `customize.sh` runs in the Magisk
install path and the KernelSU/Axeron installers do the same work.

---

## Before you install

Check the prerequisites:

- Your device exposes **more than one refresh rate** on its primary display.
  Run `dumpsys SurfaceFlinger | grep -i refresh` and look for at least two
  distinct `refreshRate=` entries. If you only see one, the daemon will
  install but do nothing.
- You have **root** through one of the three managers above.
- The `/data/local/tmp/` filesystem is writable (it is on every stock and
  custom ROM we are aware of).
- You have at least **2 MB** of free space in `/data/adb/modules/` (Magisk)
  or `/data/adb/ksu/modules/` (KernelSU) or the equivalent Axeron path.

---

## Install — Magisk

1. Open the **Magisk** app.
2. Tap **Modules** in the bottom bar.
3. Tap **Install from storage**.
4. Pick `dfps.zip`.
5. Wait for the success toast. Tap **Reboot** (or reboot manually later).
6. After reboot, the daemon is started by `service.sh` automatically.

To confirm it is running:

```bash
su -c pgrep -af dfps
# expected: /data/local/tmp/dfps/dfps
```

---

## Install — KernelSU

1. Open the **KernelSU** manager app.
2. Tap the **⋮** menu → **Modules** (or use the bottom bar in newer builds).
3. Tap **Install from storage** and select `dfps.zip`.
4. Reboot when prompted (or use the *hot-reload* button if your build
   supports it).
5. After boot, the daemon is running. Open the WebUI from
   *Modules → DFPS → Open WebUI*, or navigate to
   `ksu://webui/dfps` from any browser.

Confirm with:

```bash
su -c pgrep -af dfps
```

---

## Install — Axeron

1. Open the **Axeron** manager app.
2. Go to **Plugins** (Axeron calls root modules "plugins").
3. Install from storage → `dfps.zip`.
4. Reload / reboot when prompted.
5. The plugin entry appears as *Dynamic FPS Controller v1.0.0*. Tap
   *Open WebUI* (or navigate to `axeron://webui/dfps`).

Confirm with:

```bash
su -c pgrep -af dfps
```

> The `axeronPlugin=14000` line in `module.prop` is what tells Axeron this
> is also one of its plugins, with its own independent version code
> (14000 vs. the Magisk `versionCode=10000`). The dual-numbering lets the
> same zip version itself correctly in all three ecosystems. See
> [MODULE_PACKAGING.md](MODULE_PACKAGING.md) for the full breakdown.

---

## Verify the installation

Whichever manager you used, a healthy install produces these signals:

1. **Process is alive:**

   ```bash
   su -c pgrep -af dfps
   # /data/local/tmp/dfps/dfps
   ```

2. **Logcat is quiet unless `DEBUG=true`:**

   ```bash
   logcat -d -s DFPS_Daemon
   # should print "Initiated" and a UID / mode line on first start
   ```

3. **Refresh rate actually changes on touch:**

   ```bash
   # while touching the screen:
   su -c cat /sys/class/drm/card*/edid | head -c 0
   # OR (more portable):
   dumpsys SurfaceFlinger | grep -i 'refresh rate'
   # you should see the active rate while touching, idle rate otherwise
   ```

4. **WebUI loads** (KernelSU / Axeron only):

   Open the manager → module entry → *Open WebUI*. The app list should
   populate within a couple of seconds.

5. **Socket answers:**

   ```bash
   # Linux:
   socat - UNIX-CONNECT:"@dfps" <<< ""
   # BSD/macOS:
   nc -U "@dfps"
   # you should see the current foreground package name (or a blank line
   # if nothing is focused yet)
   ```

If any of these fail, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## What the installer actually does

The `customize.sh` script (runs once at install time, in the manager's
install environment) does the following:

1. **Auto-detects the display mode table** by running
   `dumpsys SurfaceFlinger` and parsing every line that contains both an
   `id=` and a `refreshRate=`, `Hz`, or `fps=` token. The result is sorted
   and deduped, then written to `$MODPATH/modes.map`. Each line has the
   format:

   ```
   <rate_hz> <sf_display_id>
   ```

   For example:

   ```
   60 0
   90 1
   120 2
   144 3
   ```

2. **Computes fallback rates** from the same table: the minimum rate becomes
   the default `defaultIdle` and `offscreenRate`, the maximum becomes
   `defaultActive`.

3. **Writes a default `dfps.conf`** to the module directory with
   game-specific example rules for popular titles (Genshin Impact,
   Arknights, Mobile Legends) pinned to the device's max rate.

4. **Sets the binary executable** (`chmod +x system/bin/dfps`).

5. **Creates symlinks** under `/data/local/tmp/dfps/`:

   ```text
   /data/local/tmp/dfps/dfps       -> $MODPATH/system/bin/dfps
   /data/local/tmp/dfps/modes.map  -> $MODPATH/modes.map
   /data/local/tmp/dfps/dfps.conf  -> $MODPATH/dfps.conf
   ```

   The daemon hardcodes these paths, so symlinking is how the module keeps
   everything inside its own directory and still appears to live under
   `/data/local/tmp/dfps/`.

`service.sh` then runs at every boot and simply starts the daemon:

```sh
#!/system/bin/sh
pkill -9 -f "/data/local/tmp/dfps/dfps"
"/data/local/tmp/dfps/dfps" &
```

`action.sh` is what your manager's *Run* / *Action* button executes: if the
daemon is not running it starts it, otherwise it prints the current
`dfps.conf` for inspection.

---

## First-boot behavior

The very first time the daemon starts on a fresh install:

1. The Binder service acquisition (`AServiceManager_getService` /
   `AServiceManager_waitForService`) acquires `activity`, `power`,
   `SurfaceFlinger`, `display`, and `batteryproperties` services.
2. The transaction code cache at `/data/local/tmp/tx_code.txt` is checked.
   If it is missing or the build fingerprint doesn't match, the daemon
   writes the embedded resolver JAR to `/data/local/tmp/resolver.jar` and
   spawns `app_process` to populate the cache. This step is skipped on
   subsequent boots if the ROM hasn't been updated.
3. The process observer is registered with `IActivityManager` and the
   display callback with `IDisplayManager`.
4. The foreground task is queried once for the initial state.
5. The epoll loop starts, `g_screen_interactive` is read from
   `IPowerManager`, and from this point the daemon is purely event-driven.

A full cold start takes ~150–400 ms on a modern device, dominated by the
first `waitForService` call and the Java VM spawn for the resolver (only on
the first boot, ever).

---

## Uninstall

Through your manager:

- **Magisk** → *Modules* → *DFPS* → *Uninstall* → reboot.
- **KernelSU** → *Modules* → *DFPS* → *Uninstall* → reboot.
- **Axeron** → *Plugins* → *DFPS* → *Uninstall* → reload.

`uninstall.sh` runs at removal time and:

1. Kills the daemon (`pkill -9 -f /data/local/tmp/dfps/dfps`).
2. Removes the transaction-code cache at `/data/local/tmp/tx_code.txt`.
3. Removes the runtime symlink directory at `/data/local/tmp/dfps/`.

If you ever want to force a clean re-install (e.g. to regenerate
`modes.map` after a display driver update), you can also run:

```bash
su -c /data/adb/modules/dfps/uninstall.sh
su -c /data/adb/modules/dfps/customize.sh   # not part of normal flow
```

---

## Manual install (no root manager)

The zip format is just a tarball with a known structure. If you cannot use
a root manager (or want to test without flashing), you can extract it
anywhere and run the binary directly:

```bash
unzip dfps.zip -d /tmp/dfps
chmod +x /tmp/dfps/system/bin/dfps
mkdir -p /data/local/tmp/dfps
ln -sf /tmp/dfps/system/bin/dfps /data/local/tmp/dfps/dfps
su -c /data/local/tmp/dfps/dfps
```

You will need to bring your own `modes.map` and `dfps.conf` if you skip
`customize.sh`. See [CONFIGURATION.md](CONFIGURATION.md) for the file
formats.
