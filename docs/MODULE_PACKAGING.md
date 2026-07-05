# Module Packaging

DFPS is delivered as a **tri-compatible root module ZIP** that
installs on **Magisk**, **KernelSU**, and **Axeron** without any
modification. This document explains:

1. The structure of `dfps.zip`.
2. The purpose and behaviour of each file inside it.
3. The lifecycle of the module from install to uninstall.
4. How the three supervisors are detected and dispatched to.
5. How to repackage the module for custom distributions.

> **Audience.** Module maintainers, custom-ROM integrators, and
> anyone who needs to ship DFPS as part of a larger system.

---

## Table of Contents

1. [What "Tri-Compatible" Means](#1-what-tri-compatible-means)
2. [Archive Layout](#2-archive-layout)
3. [The `module.prop` File](#3-the-moduleprop-file)
4. [`customize.sh` — Install-Time Setup](#4-customizesh--install-time-setup)
5. [`service.sh` — Boot-Time Spawn](#5-servicesh--boot-time-spawn)
6. [`action.sh` — User-Invoked Toggle](#6-actionsh--user-invoked-toggle)
7. [`uninstall.sh` — Removal](#7-uninstallsh--removal)
8. [The WebUI](#8-the-webui)
9. [Supervisor Detection](#9-supervisor-detection)
10. [Filesystem Layout After Install](#10-filesystem-layout-after-install)
11. [Repackaging for Distribution](#11-repackaging-for-distribution)
12. [Testing a Module ZIP](#12-testing-a-module-zip)

---

## 1. What "Tri-Compatible" Means

A "module" in the Magisk sense is a ZIP archive with a known
internal layout. Magisk, KernelSU, and Axeron all read the same
basic files, but each has **at least one extension** for
supervisor-specific behaviour. A truly tri-compatible module:

- Has a `module.prop` that all three supervisors parse.
- Optionally has an `axeronPlugin=NNNNN` line that Axeron requires
  but Magisk and KernelSU ignore.
- Has a `service.sh` that all three supervisors run on boot.
- Has a `customize.sh` that runs in the install environment
  (recovery for Magisk, host-side for KernelSU/Axeron).
- Has an `uninstall.sh` that all three supervisors honour.

DFPS follows this convention **plus** Axeron's
`axeronPlugin=14000` extension, which is what unlocks the
"tri-compat" promise. There is no other supervisor-specific code
in the module — the daemon itself is a plain ELF binary that
runs identically under all three.

---

## 2. Archive Layout

```
dfps.zip
├── module.prop                 ← identity, version, author
├── customize.sh                ← install-time setup
├── service.sh                  ← boot-time spawn
├── action.sh                   ← user-invoked toggle
├── uninstall.sh                ← removal
├── system/
│   └── bin/
│       └── dfps                ← the ELF daemon (48 KB)
└── webroot/
    └── index.html              ← the WebUI (single-page app, 43 KB)
```

That's it. The total archive is ~95 KB. The daemon and the WebUI
are the only two substantial binaries; everything else is a
short shell script.

### 2.1 Why `system/bin/dfps`?

Magisk's module convention is that everything under `system/` is
mirrored onto `/system/` via an overlay mount. So `system/bin/dfps`
becomes accessible at `/system/bin/dfps` once the module is
loaded. However, the daemon does **not** run from `/system/bin/`
— it runs from `/data/local/tmp/dfps/dfps` via a symlink, which
allows the user (and the WebUI) to replace the binary in place
without rebooting.

The `system/bin/dfps` location is kept because:

1. The Magisk convention requires it for a module to be considered
   "well-formed".
2. Some supervisors (notably Axeron's early builds) refuse to
   enable a module that has no `system/` content.

---

## 3. The `module.prop` File

```prop
id=dfps
name=Dynamic FPS Controller
version=v1.0.0
versionCode=10000
author=Sehannnnn&yc9559
description=Dynamic screen refresh rate controller
axeronPlugin=14000
```

| Field           | Value                  | Notes                                                                  |
|-----------------|------------------------|------------------------------------------------------------------------|
| `id`            | `dfps`                 | Used as the directory name in `/data/adb/modules/dfps/`. **Lowercase, no spaces.** |
| `name`          | `Dynamic FPS Controller` | Human-readable name shown in the UI.                                  |
| `version`       | `v1.0.0`               | Display version. Free-form.                                            |
| `versionCode`   | `10000`                | Monotonically increasing integer. **Bump on every release.**          |
| `author`        | `Sehannnnn&yc9559`     | Free-form.                                                              |
| `description`   | `Dynamic screen refresh rate controller` | Free-form.                                              |
| `axeronPlugin`  | `14000`                | **Axeron-specific.** Magisk and KernelSU ignore this.                  |

### 3.1 The `axeronPlugin` Field

Axeron's module system uses a **two-versionCode** scheme:

- `versionCode` is the *upstream* version, used by Magisk and
  KernelSU.
- `axeronPlugin` is the Axeron-specific version, used by Axeron
  to detect out-of-tree module variants and to apply its own
  compatibility matrix.

A typical Axeron release uses `axeronPlugin = 10000 + versionCode`
so the two codes are related but independent. DFPS uses
`axeronPlugin = 14000` for v1.0.0. **Do not remove this line** —
Axeron will refuse to load the module.

### 3.2 Bumping Versions

When releasing a new version:

1. Update `version` (the display string).
2. Update `versionCode` (the monotonic integer).
3. **Do not** decrement any version field — supervisors may
   cache the old value and refuse to update.
4. Re-run the full install/uninstall test cycle (see §12).

---

## 4. `customize.sh` — Install-Time Setup

`customize.sh` runs **once per install**, in a sandboxed shell
provided by the supervisor. It is the right place to:

- Auto-detect hardware capabilities.
- Generate a default `dfps.conf`.
- Create symlinks or directories.
- Pre-compile or pre-process any data the daemon needs at runtime.

For DFPS, `customize.sh` does five things.

### 4.1 Query SurfaceFlinger for Display Modes

```sh
DUMPSYS_OUT=$(dumpsys SurfaceFlinger 2>/dev/null)
```

`dumpsys SurfaceFlinger` returns the full state of the display
server, including every display mode. The output format is
slightly different across Android versions, so the script
matches three common layouts:

```sh
case "$line" in
    # {id=0, hwcId=0, ... refreshRate=60.00 Hz}
    *id=[0-9]*refreshRate=[0-9]*) ... ;;

    # id=1, ... 120Hz
    *id=[0-9]*Hz*|*id=[0-9]*fps*) ... ;;

    # 0: 1080x2400, ... refresh=60.000000
    *[0-9]:*refresh=[0-9]*|*[0-9]:*fps=[0-9]*) ... ;;
esac
```

The extracted `(rate, id)` pairs are sorted and deduped:

```sh
... | sort -u -k1,1n > "$MODPATH/modes.map"
```

The result is a plain-text map like:

```
60 1
90 2
120 3
```

The daemon reads this at startup to map a desired Hz to the
SurfaceFlinger mode ID it must pass to transaction `1035`.

### 4.2 Extract Min/Max for the Default Config

```sh
MIN_RATE=$(awk '{print $1}' "$MODPATH/modes.map" | sort -n | head -n1)
MAX_RATE=$(awk '{print $1}' "$MODPATH/modes.map" | sort -n | tail -n1)
```

This produces sensible `defaultIdle` and `defaultActive` values
without requiring the user to know their panel's capabilities.

### 4.3 Write the Default `dfps.conf`

A heredoc writes a fully-commented default config. Key choices:

- `touchSlackMs = 2000` (the original "snappy but not jittery" default).
- `batterySaver = false` (the user opts in).
- `enableMinBrightness = false` (the user opts in).
- Pre-populated rules for the most common game apps (Genshin
  Impact, Arknights, Mobile Legends) and the launcher.

### 4.4 Apply Binary Permissions

```sh
chmod +x "$MODPATH/system/bin/dfps"
```

This is mandatory — without it the supervisor will refuse to
spawn the binary.

### 4.5 Create Runtime Symlinks

```sh
mkdir -p /data/local/tmp/dfps
ln -s "$MODPATH/system/bin/dfps" /data/local/tmp/dfps/dfps
ln -s "$MODPATH/modes.map"        /data/local/tmp/dfps/modes.map
ln -s "$MODPATH/dfps.conf"        /data/local/tmp/dfps/dfps.conf
```

The daemon is hard-coded to read `/data/local/tmp/dfps/`. The
symlinks point to the **real** files in the module directory,
which means:

- The user can edit `dfps.conf` and the change is reflected in
  the module's file (the WebUI relies on this).
- The module can be disabled and re-enabled without losing the
  user's configuration tweaks.
- The supervisor can remove the module cleanly (just `rm -rf`
  the module dir; the symlinks in `/data/local/tmp/dfps/` are
  dangling but harmless until `uninstall.sh` removes them).

### 4.6 What `customize.sh` Does **Not** Do

- It does **not** start the daemon. That's `service.sh`'s job.
- It does **not** validate `dfps.conf`. The daemon does that
  with sane fallbacks at startup.
- It does **not** write to `/data/local/tmp/tx_code.txt`. The
  daemon creates that on first run.

---

## 5. `service.sh` — Boot-Time Spawn

```sh
#!/system/bin/sh
pkill -9 -f "/data/local/tmp/dfps/dfps"
"/data/local/tmp/dfps/dfps" &
```

Three lines. Responsibilities:

1. **Kill any prior instance.** The `pkill` is a safety net for
   cases where a stale process is still around (e.g. after a
   crash that bypassed cleanup).
2. **Start the daemon detached.** The trailing `&` puts it in
   the background; the script exits and the supervisor moves on.
3. **No `wait`.** `service.sh` returns immediately. The daemon
   is now the supervisor's child of `init` (or `magiskd` /
   `ksud` / `axd`, depending on the supervisor).

The supervisor runs `service.sh` on every boot, on every module
enable, and after `customize.sh` finishes on install. This is
the **only** lifecycle hook besides `uninstall.sh`.

### 5.1 Why `pkill` Is Defensive, Not Required

A clean install has no prior process. But:

- If the user reboots without the module being disabled, the
  supervisor will re-run `service.sh`, which re-runs the daemon.
  The previous instance will already have been killed by the
  kernel during shutdown, so `pkill` finds nothing.
- If the user manually started the daemon via `action.sh`
  (e.g. for testing) before rebooting, that instance is killed
  cleanly.

The `pkill -9` is non-negotiable: a SIGTERM-based kill can
deadlock if the daemon is mid-syscall. `-9` is unconditional.

---

## 6. `action.sh` — User-Invoked Toggle

```sh
#!/system/bin/sh
if pgrep -f "/data/local/tmp/dfps/dfps" >/dev/null; then
    echo "Dynamic FPS is RUNNING."
    echo "--------------------------"
    echo "Current configuration:"
    cat "/data/local/tmp/dfps/dfps.conf"
else
    echo "Dynamic FPS is STOPPED. Initializing..."
    "/data/local/tmp/dfps/dfps" &
    echo "[+] Started."
fi
```

This script is invoked when the user taps the module in
**KernelSU**'s or **Axeron**'s UI under "Action" / "Run". It's a
**convenience**, not a core feature.

Behaviour:

- If the daemon is already running, print its config.
- If not, start it and confirm.

Magisk does not expose an "Action" button, so on Magisk the
script can be invoked manually:

```sh
adb shell sh /data/adb/modules/dfps/action.sh
```

---

## 7. `uninstall.sh` — Removal

```sh
#!/system/bin/sh
pkill -9 -f "/data/local/tmp/dfps/dfps"
rm -f /data/local/tmp/tx_code.txt
rm -rf /data/local/tmp/dfps
```

Three operations:

1. **Kill the daemon.** `-9` is mandatory (see §5.1).
2. **Remove the transaction-code cache.** This is a per-device
   fingerprint file that has no value once DFPS is gone.
3. **Remove the runtime symlinks directory.** This deletes
   `/data/local/tmp/dfps/` (a directory of symlinks) and its
   contents.

The module directory itself (`/data/adb/modules/dfps/`) is
removed by the supervisor after `uninstall.sh` returns.

### 7.1 What `uninstall.sh` Does **Not** Do

- It does **not** reset SurfaceFlinger to a default rate. If
  the user wants the system to revert to a specific rate, they
  must do it manually (`dumpsys SurfaceFlinger` shows the
  current mode).
- It does **not** undo any SELinux policy changes.
- It does **not** clear WebUI cached state in the supervisor's
  app.

---

## 8. The WebUI

The `webroot/` directory contains a single self-contained
`index.html` that the supervisor serves at module-install time
via its own internal HTTP server. The UI:

- Reads the current `dfps.conf` and `modes.map` via
  `ksu.exec()` (KernelSU), `axeron.exec()` (Axeron), or
  `magisk_exec()` (Magisk — not currently used; the WebUI
  auto-detects).
- Writes changes back via the same supervisor hook.
- Uses `mv` to overwrite files (rather than `cp`) so the
  daemon's inotify watch sees an `IN_MOVED_TO` event and
  hot-reloads the config.

### 8.1 File Layout

```
webroot/
└── index.html     ← 1053 lines, single-page app
```

The UI is intentionally a single file — no build step, no
external assets, no framework. Just CSS variables for the
"ColorOS Aquamorphic" palette, vanilla JS for the controller,
and a few `exec` calls for I/O.

### 8.2 Hot-Reload Trick

The daemon's inotify watch subscribes to `IN_CLOSE_WRITE` and
`IN_MOVED_TO`. Plain file overwrites via `echo > dfps.conf`
generate `IN_CLOSE_WRITE`; `mv new.conf dfps.conf` generates
`IN_MOVED_TO`. The WebUI uses `mv` because:

- It's atomic (no half-written file).
- It always triggers the watch (some editors emit
  `IN_CLOSE_NOWRITE` for the temp file they save to).
- It works even when the WebUI is running in a different
  mount namespace than the daemon.

---

## 9. Supervisor Detection

The module is **identical** for all three supervisors. The
detection happens in two layers:

### 9.1 At Install Time

| Supervisor   | Detection                                                                 |
|--------------|---------------------------------------------------------------------------|
| Magisk       | Parses `module.prop` for the basic fields. Ignores `axeronPlugin`.         |
| KernelSU     | Same as Magisk.                                                           |
| Axeron       | Parses `module.prop` **and** requires `axeronPlugin` to be a valid number. |

If a user accidentally flashes the module to Magisk with
`axeronPlugin=14000` in the file, nothing breaks — Magisk
ignores the unknown field. If a user flashes a module without
`axeronPlugin` to Axeron, Axeron rejects the install.

### 9.2 At Runtime

The daemon is **supervisor-agnostic**. It runs as a plain ELF
binary with no awareness of which supervisor launched it. The
WebUI uses the **window.ksu** global exposed by KernelSU and
Axeron's WebView; if `window.ksu.exec` is present, the WebUI
uses it; if `window.axy` (or similar Axeron global) is
present, the WebUI uses that; otherwise the WebUI shows a
"this supervisor is unsupported" message.

The same `index.html` works on KernelSU and Axeron because
both expose a compatible `ksu.exec` API.

---

## 10. Filesystem Layout After Install

After a successful install and reboot:

```
/data/adb/modules/dfps/                         ← module root (read-only)
├── module.prop
├── customize.sh
├── service.sh
├── action.sh
├── uninstall.sh
├── dfps.conf                                   ← user-editable, owned by root
├── modes.map                                   ← generated by customize.sh
├── system/
│   └── bin/
│       └── dfps                                ← the ELF daemon
└── webroot/
    └── index.html

/data/local/tmp/dfps/                           ← runtime symlink dir
├── dfps      -> /data/adb/modules/dfps/system/bin/dfps
├── dfps.conf -> /data/adb/modules/dfps/dfps.conf
└── modes.map -> /data/adb/modules/dfps/modes.map

/data/local/tmp/tx_code.txt                     ← transaction-code cache
```

The supervisor's view:

```
Magisk:   /data/adb/modules/dfps/
KernelSU: /data/adb/ksu/modules/dfps/  (symlinked to /data/adb/modules/dfps/)
Axeron:   /data/adb/axeron/modules/dfps/ (symlinked to /data/adb/modules/dfps/)
```

(The exact supervisor-specific path varies, but the module
contents are identical and the symlinks in
`/data/local/tmp/dfps/` are the same.)

---

## 11. Repackaging for Distribution

To produce a new release ZIP:

```sh
# 1. Build the daemon (see BUILDING.md)
cd dfpsd
make clean
make release ABI=arm64-v8a
ls -l src/dfps  # 48 KB binary, statically linked

# 2. Copy the binary into the module skeleton
cp src/dfps module_skeleton/system/bin/dfps

# 3. Optional: regenerate the embedded resolver JAR
#    (only needed if you change the Java code)
cd src/resolver
./build_resolver.py > ../resolver_bytes.h
cd ..
# rebuild the daemon so the new JAR is embedded.

# 4. Repackage
cd module_skeleton
zip -r ../dfps.zip . -x "*.DS_Store"
cd ..

# 5. Verify
unzip -l dfps.zip
```

### 11.1 Version Bump Checklist

When bumping the version:

- [ ] Edit `module_skeleton/module.prop`: `version` and `versionCode`.
- [ ] Edit `dfpsd/src/main.c` if you want the build string to match
      (it appears in the `LOGI` banner on startup).
- [ ] Re-run `make clean && make release ABI=arm64-v8a`.
- [ ] Re-zip.
- [ ] SHA-256 the new ZIP and post the hash alongside the binary.

### 11.2 Multi-ABI Distribution

The default build is `arm64-v8a` because that is what ~95% of
modern Android devices use. If you need to support 32-bit:

```sh
make clean && make release ABI=armeabi-v7a
mv src/dfps module_skeleton/system/bin/dfps.armv7
make clean && make release ABI=arm64-v8a
mv src/dfps module_skeleton/system/bin/dfps.arm64
```

Then in `customize.sh`:

```sh
if [ "$(getprop ro.product.cpu.abi)" = "armeabi-v7a" ]; then
    cp "$MODPATH/system/bin/dfps.armv7" "$MODPATH/system/bin/dfps"
else
    cp "$MODPATH/system/bin/dfps.arm64" "$MODPATH/system/bin/dfps"
fi
```

(For the default arm64-only build, this step is unnecessary.)

---

## 12. Testing a Module ZIP

Before publishing a ZIP:

### 12.1 Static Checks

```sh
# Confirm the archive is well-formed
unzip -t dfps.zip

# Confirm required files
unzip -l dfps.zip | grep -E "(module\.prop|customize\.sh|service\.sh|system/bin/dfps|webroot/index\.html)"

# Confirm the binary is the right ABI
unzip -p dfps.zip system/bin/dfps | file -
# → "ELF 64-bit LSB executable, ARM aarch64, …"

# Confirm module.prop syntax
unzip -p dfps.zip module.prop

# Confirm the binary runs
adb push dfps.zip /sdcard/
adb shell
cd /data/local/tmp
unzip /sdcard/dfps.zip -d test/
chmod +x test/system/bin/dfps
test/system/bin/dfps --help 2>&1 | head -3   # should fail gracefully
# Note: dfps has no --help; it will just start. Kill it after a moment.
```

### 12.2 Install/Uninstall Cycle

On a test device with all three supervisors (if possible):

```sh
# Magisk
adb push dfps.zip /sdcard/
adb shell magisk --install-module /sdcard/dfps.zip
adb reboot
adb shell ls -l /data/local/tmp/dfps/    # should show symlinks
adb shell pgrep -af /data/local/tmp/dfps/dfps
# Now disable and re-enable, confirm idempotency
# Now uninstall and confirm clean state
```

The KernelSU and Axeron cycles are identical except for the
`install` command (use the respective UI).

### 12.3 Smoke Test

After install, run:

```sh
adb shell logcat -c
adb shell am start -n com.android.chrome/com.google.android.apps.chrome.Main
sleep 3
adb shell logcat -d -s DFPS_Daemon:*
```

You should see:

- `Focused package matches rule: 'com.android.chrome' -> …`.
- A line per rate change.
- No `LOGE` lines.

If you see errors, see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).

---

## Appendix A — File-by-File Permissions

| File                          | Mode      | Owner  | Notes                       |
|-------------------------------|-----------|--------|-----------------------------|
| `module.prop`                 | `0644`    | root   | Required by spec.           |
| `customize.sh`                | `0755`    | root   | Executed by supervisor.     |
| `service.sh`                  | `0755`    | root   | Executed on boot.           |
| `action.sh`                   | `0755`    | root   | Invoked by user.            |
| `uninstall.sh`                | `0755`    | root   | Executed on uninstall.      |
| `dfps.conf`                   | `0644`    | root   | User-editable via WebUI.    |
| `modes.map`                   | `0644`    | root   | Generated.                  |
| `system/bin/dfps`             | `0755`    | root   | The daemon.                 |
| `webroot/index.html`          | `0644`    | root   | Static.                     |
| `/data/local/tmp/tx_code.txt` | `0600`    | root   | Fingerprint-keyed.          |
| `/data/local/tmp/resolver.jar`| `0600`    | root   | One-shot, deleted.          |
