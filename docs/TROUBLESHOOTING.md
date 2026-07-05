# Troubleshooting

This is a cookbook for the issues that come up in practice. Each
section is a **symptom → diagnosis → fix** triplet. Work top to
bottom; the first section covers the most common "it just doesn't
work" scenarios and the later sections cover the obscure ones.

> **Audience.** Anyone who has installed DFPS and is having trouble.
> No prior experience with Magisk / KernelSU / Axeron is assumed.

---

## Table of Contents

1. [Quick Diagnostic](#1-quick-diagnostic)
2. [Installation Issues](#2-installation-issues)
3. [The Daemon Won't Start](#3-the-daemon-wont-start)
4. [The Rate Doesn't Change](#4-the-rate-doesnt-change)
5. [Touch Detection Doesn't Work](#5-touch-detection-doesnt-work)
6. [Foreground App Tracking Is Wrong](#6-foreground-app-tracking-is-wrong)
7. [Battery/Power Issues](#7-batterypower-issues)
8. [The WebUI Is Broken](#8-the-webui-is-broken)
9. [Logcat Analysis](#9-logcat-analysis)
10. [Recovery and Reset](#10-recovery-and-reset)

---

## 1. Quick Diagnostic

Run this on the device (`adb shell`) **first**. It checks everything
in one pass:

```sh
#!/system/bin/sh
# dfps_diag.sh — quick health check

echo "=== Module / daemon ==="
ls -l /data/adb/modules/dfps 2>/dev/null || \
    ls -l /data/adb/ksu/modules/dfps 2>/dev/null || \
    echo "Module not installed."
pgrep -af /data/local/tmp/dfps/dfps || echo "Daemon NOT running."

echo
echo "=== Symlinks / files ==="
ls -la /data/local/tmp/dfps/

echo
echo "=== Logcat (last 50 DFPS lines) ==="
logcat -d -s DFPS_Daemon:* | tail -50

echo
echo "=== Abstract socket ==="
cat /proc/net/unix | grep '@dfps' || echo "Socket NOT bound."

echo
echo "=== Build fingerprint ==="
getprop ro.build.fingerprint

echo
echo "=== Configuration ==="
cat /data/local/tmp/dfps/dfps.conf 2>/dev/null

echo
echo "=== Modes map ==="
cat /data/local/tmp/dfps/modes.map 2>/dev/null
```

Read the output. Almost every issue is visible here.

---

## 2. Installation Issues

### 2.1 "Module not installed" in Magisk

**Symptom.** Magisk's Modules page doesn't list `dfps`.

**Diagnosis.** The ZIP was not flashed via Magisk, or the ZIP is
malformed.

**Fix.**

```sh
# Confirm the ZIP is well-formed
unzip -l dfps.zip | head -20

# Expected: META-INF/com/google/android/, module.prop, customize.sh, service.sh, ...
# If those are missing, re-download the ZIP.

# Reflash
adb push dfps.zip /sdcard/
# In the Magisk app → Modules → Install from storage → select dfps.zip
# Reboot.
```

### 2.2 "Module not installed" in KernelSU / Axeron

**Symptom.** Same as above but the module is missing from KernelSU's
or Axeron's module list.

**Fix.** KernelSU and Axeron also accept `dfps.zip` flashed through
their respective UIs. KernelSU: **Modules → Install from storage**.
Axeron: **Modules → Add**.

If the ZIP is rejected with "invalid module", check `module.prop`
inside it:

```sh
unzip -p dfps.zip module.prop
```

It must contain:

```prop
id=dfps
name=Dynamic FPS Controller
version=v1.0.0
versionCode=10000
author=...
axeronPlugin=14000    ← only present for Axeron
```

The `axeronPlugin=14000` line is **required** for Axeron and is
**ignored** by Magisk and KernelSU. Don't remove it.

### 2.3 Installation Succeeds but Module Disables on Reboot

**Symptom.** After reboot, Magisk/KernelSU shows the module as
**disabled** with a red warning.

**Diagnosis.** `customize.sh` failed. Check the Magisk install log:

```sh
adb shell cat /cache/magisk_install.log 2>/dev/null
# or for KernelSU:
adb shell cat /data/adb/ksu/log/install.log 2>/dev/null
```

**Common causes.**

| Error                                              | Cause                          |
|----------------------------------------------------|--------------------------------|
| `modes.map could not be generated`                 | `dumpsys SurfaceFlinger` failed. Touchscreen-only fix, ignore. |
| `dumpsys: not found`                               | Path issue on a non-standard ROM. Open `customize.sh` and replace `dumpsys` with the full path. |
| `mount: not found`                                 | `PATH` not set in the install environment. Same fix. |
| `cp: cannot stat 'system/bin/app_process'`         | Outdated module on a heavily-customized ROM. |

**Fix.** Read the install log, identify the failing step, and
either re-run with a more permissive `customize.sh` or open an
issue with the install log attached.

---

## 3. The Daemon Won't Start

### 3.1 "Daemon NOT running" Right After Install

**Diagnosis A — service.sh failed.**

```sh
adb shell logcat -d -s Magisk:* | tail -20
```

If you see `service.sh returned 1`, the start script errored out.

**Fix A.** Run it manually:

```sh
adb shell sh /data/adb/modules/dfps/service.sh
```

The output will show which line failed. Common causes:

- The `dfps` binary is not executable: `chmod +x /data/local/tmp/dfps/dfps`.
- A library is missing: `ldd /data/local/tmp/dfps/dfps` should show
  all libraries found. The most common missing library is
  `libbinder_ndk.so` on very old devices (API 26); see §3.3.

**Diagnosis B — daemon started but exited immediately.**

```sh
adb shell logcat -d -s DFPS_Daemon:* | tail -50
```

Look for `LOGE` lines. The most common are:

| Log                                                | Meaning                                          |
|----------------------------------------------------|--------------------------------------------------|
| `Failed to create eventfd synchronization descriptor` | Out of fd's or `seccomp` blocking `eventfd2`. Unusual. |
| `libbinder_ndk.so open error.`                     | `libbinder_ndk.so` is missing — see §3.3.       |
| `dlsym resolution failure.`                        | `libbinder_ndk.so` is too old.                  |
| `ActivityManager binder missing.`                  | `service call activity …` blocked by SELinux.  |
| `Core ActivityManager transaction code mapping failed. Halting.` | Resolver failed — see §3.4. |

### 3.2 "Daemon keeps dying every few seconds"

**Diagnosis.** A critical Binder service is dying. Check:

```sh
adb shell logcat -d -s DFPS_Daemon:* | grep -E "(died|exiting|restart)"
```

**Fix.** The daemon intentionally exits on critical Binder death
(ActivityManager, PowerManager, SurfaceFlinger, DisplayManager).
This is a feature, not a bug — it lets the supervisor respawn with
fresh handles. Look for upstream Binder instability in logcat
(`ServiceManager`/`system_server`).

### 3.3 "libbinder_ndk.so open error."

**Diagnosis.** The device's `/system/lib[64]/libbinder_ndk.so` is
missing or the loader path is wrong.

**Fix.**

```sh
adb shell ls /system/lib64/libbinder_ndk.so /system/lib/libbinder_ndk.so
```

If both are missing, your device is API 26 or older and **DFPS does
not support it**. Minimum supported API is 28 (Android 9 Pie) for
the AIDL-stable NDK surface.

If only one path is present, DFPS will find it (it tries both).
If neither is present, see above.

### 3.4 "Core ActivityManager transaction code mapping failed"

**Diagnosis.** The one-shot `app_process` resolver did not return
valid codes. Causes:

- `app_process` is broken on the device (rare).
- A custom ROM has stripped reflection metadata from a system
  class (very rare).
- A sandbox / `seccomp` filter is killing the child.

**Fix.** Run the resolver by hand:

```sh
adb shell
# /data/local/tmp/dfps/resolver.jar is the embedded JAR; copy it out
cp /data/local/tmp/dfps/resolver.jar /data/local/tmp/
# Reproduce the child invocation
CLASSPATH=/data/local/tmp/resolver.jar \
LD_LIBRARY_PATH= \
LD_PRELOAD= \
/system/bin/app_process /system/bin android.app.CodeResolver \
  android.app.IActivityManager\$Stub TRANSACTION_registerProcessObserver \
  android.app.IActivityManager\$Stub TRANSACTION_getFocusedRootTaskInfo \
  android.app.IActivityManager\$Stub TRANSACTION_getFocusedStackInfo \
  android.app.IProcessObserver\$Stub TRANSACTION_onForegroundActivitiesChanged \
  android.os.IPowerManager\$Stub TRANSACTION_isInteractive \
  android.os.IPowerManager\$Stub TRANSACTION_isPowerSaveMode \
  android.hardware.display.IDisplayManager\$Stub TRANSACTION_getBrightness \
  android.hardware.display.IDisplayManagerCallback\$Stub TRANSACTION_onDisplayEvent \
  android.hardware.display.IDisplayManager\$Stub TRANSACTION_registerCallbackWithEventMask \
  android.hardware.display.IDisplayManager\$Stub TRANSACTION_registerCallback \
  android.os.IBatteryPropertiesRegistrar\$Stub TRANSACTION_registerListener \
  android.os.IBatteryPropertiesListener\$Stub TRANSACTION_batteryPropertiesChanged \
  android.hardware.display.DisplayManager EVENT_FLAG_DISPLAY_CHANGED \
  android.hardware.display.DisplayManager EVENT_FLAG_DISPLAY_BRIGHTNESS
```

If you see numeric output, the resolver works — the issue is in
how DFPS parses it. Open an issue with the logcat.

If you see an `Exception` or a Java stack trace, the system class
DFPS is reflecting on is missing. **Open an issue.** Include:

- The exact output.
- `getprop ro.build.fingerprint`.
- `getprop ro.product.model`.

### 3.5 "Daemon won't start on a heavily-customized ROM"

**Diagnosis.** SELinux, `seccomp`, or a custom `init` is blocking
something.

**Fix.** Check:

```sh
adb shell dmesg | grep -iE "(dfps|avc|seccomp)" | tail -20
```

If you see `avc: denied`, you have an SELinux denial. Allow it
with `supolicy --live "allow untrusted_app ..."` or add a custom
rule to the module's `sepolicy.rule`.

---

## 4. The Rate Doesn't Change

### 4.1 Touching the screen does nothing

**Diagnosis.** Walk the chain.

```sh
adb shell
# 1. Is the daemon alive?
pgrep -af /data/local/tmp/dfps/dfps

# 2. Is the foreground being detected?
nc -U @dfps < /dev/null 2>/dev/null &
sleep 0.3
# (Navigate around; you should see lines like "com.android.chrome")
```

**Fix.** If the foreground is detected but the rate doesn't change:

```sh
adb shell logcat -d -s DFPS_Daemon:* | grep -E "Transitioning|HZ"
```

You should see `LOG_HOT` lines like `Transitioning device physical
refresh rate to: 120 Hz` (note: `LOG_HOT` is currently a no-op; the
log is via `LOGI` only with `DEBUG=true`).

If you see "Failed mapping N Hz to an ID in modes.map!", your
`modes.map` is missing that rate. See §4.3.

### 4.2 Only Idle Rate Works, Not Active

**Diagnosis.** Touch events are not reaching the daemon.

```sh
adb shell logcat -d -s DFPS_Daemon:* | grep -E "touch|Touchscreen|touchscreen"
```

If you see `Registered input device: /dev/input/event*`, touch is
detected at the device-discovery level. If not, see §5.

**Fix.** Set `DEBUG=true` in `dfps.conf` and watch for
`Focused package matches rule` and `Transitioning` lines. If the
former fires but the latter doesn't, the rate you're trying to
set isn't in `modes.map`. See §4.3.

### 4.3 "Failed mapping N Hz to an ID in modes.map!"

**Diagnosis.** Your `modes.map` doesn't contain a matching entry.

**Fix.**

```sh
adb shell cat /data/local/tmp/dfps/modes.map
```

The file is plain text, `<rate> <id>`. Confirm that the rate
DFPS is trying to set has a row. If not:

1. Run `dumpsys SurfaceFlinger | grep -A 20 'Display 0'`
2. Find the desired mode's ID.
3. Add the row to `modes.map`, or use the WebUI's "Add Mode"
   button to do it for you.

The WebUI also exposes a "Refresh modes" button that re-runs
`dumpsys SurfaceFlinger` and re-writes the map from scratch.

### 4.4 Rate Changes but the Display Doesn't Visibly Update

**Diagnosis.** Some panels (especially older LCDs) have a physical
PLL that takes 50–200 ms to lock onto a new clock. The transaction
succeeded but the panel is still settling.

**Fix.** This is not a DFPS issue. The next touch / foreground
change will reveal the new rate.

To confirm: `dumpsys SurfaceFlinger | grep -i 'active config'`
should show the new mode ID within a few hundred ms of the
transaction.

### 4.5 Rate Always Returns to One Specific Value

**Diagnosis.** A clamp is active:

- `powerSaveMode=true` (system level)
- `batterySaver=true` AND battery is low
- `enableMinBrightness=true` AND screen is dim
- Screen is off (`g_screen_interactive=false`)

**Fix.** Check `dfps.conf`:

```ini
batterySaver = false
powerSaveMaxRate = 60
enableMinBrightness = false
```

And check the system:

```sh
adb shell dumpsys battery | grep -E "level|saver|interactive"
adb shell settings get system screen_brightness
```

If the system is in battery saver mode, the daemon will cap rates
to `powerSaveMaxRate` regardless of your config. Disable battery
saver system-wide or set `powerSaveMaxRate` to the desired cap.

---

## 5. Touch Detection Doesn't Work

### 5.1 "Skipping touch device registration"

**Diagnosis.** The daemon is running in non-root mode and cannot
read `/dev/input/event*`.

**Fix.** Root mode is required for direct touch device access. If
you're using a non-root install, the daemon relies on
`IProcessObserver` to detect foreground changes (which work) but
**cannot** detect the start/end of touches. Per-app rules will
still work, but the rate will only change on app switches, not on
taps.

For touch detection, install via Magisk/KernelSU/Axeron.

### 5.2 "Permission Denied" on `/dev/input/event*`

**Diagnosis.** SELinux is blocking the access.

**Fix.**

```sh
adb shell dmesg | grep -i avc | grep input
```

Add an SELinux rule to the module:

```sh
# In the module dir on the host:
cat > sepolicy.rule <<'EOF'
allow untrusted_app kernel:file { read };
EOF
```

(Adjust to your actual denial; the message will name the exact
rule needed.)

### 5.3 No Touch Events, but `dumpsys input` Shows Touches

**Diagnosis.** The daemon is listening to the wrong device node
or the device is a USB / Bluetooth HID touchscreen.

**Fix.** DFPS only handles built-in touchscreens. USB and
Bluetooth devices are out of scope. Check:

```sh
adb shell getevent -l
```

While touching, you should see `EV_ABS ABS_MT_POSITION_X` events.
If the events come from a device named `usbhid` or `btinput`, DFPS
will ignore them.

---

## 6. Foreground App Tracking Is Wrong

### 6.1 "Always shows systemui" / "Always shows launcher"

**Diagnosis.** The list is correct but the **top** element is not
the app you expect. This happens when a transparent overlay (a
chathead, the volume dialog, the status bar) is on top of the
activity.

**Fix.** This is a limitation of `ActivityManager`'s
`getRootTaskInfo`. DFPS reports what the system reports. There is
no workaround.

### 6.2 "Doesn't update on split-screen changes"

**Diagnosis.** Split-screen has a separate root task. DFPS
handles this correctly — but only if the system has
`getFocusedRootTaskInfo`. On older systems, `getFocusedStackInfo`
is used and may return a less-precise answer.

**Fix.** Enable `DEBUG=true` and check the logs:

```sh
adb shell logcat -d -s DFPS_Daemon:* | grep -E "API_|api"
```

If you see `api=STACK_INFO`, the device is using the older API.
There's no way to upgrade this without a system change.

### 6.3 Per-App Rule Doesn't Match

**Diagnosis.** Whitespace, capitalization, or extra characters in
`dfps.conf`.

**Fix.** DFPS matches the rule's `pkg` field against the
**package prefix** (the substring before the first `/`). The
match is case-sensitive. The simplest rule:

```ini
com.android.chrome = 60 120
```

Make sure:

- No leading whitespace on the line.
- The `=` has a single space on each side (or none, both work).
- The package name exactly matches the one in `dumpsys activity
  activities` or in the `@dfps` socket output.

Test by running the daemon with `DEBUG=true` and watching for
`Focused package matches rule` lines.

---

## 7. Battery/Power Issues

### 7.1 "Daemon caps rate to 60 Hz even though config says 120"

**Diagnosis.** Power-save mode is active. Check:

```sh
adb shell dumpsys battery | grep saver
```

If `saver=_enabled=true` or `mSaverPolicyEnabled=true`, the
system has power-save on, and DFPS is correctly capping to
`powerSaveMaxRate`.

**Fix.**

- Disable system power-save (the user's choice).
- Or raise `powerSaveMaxRate` in `dfps.conf`.
- Or set `batterySaver=false` in `dfps.conf` to ignore the
  system state entirely (this is **not** recommended on phones
  — it disables low-battery throttling).

### 7.2 "Battery threshold doesn't trigger low-battery mode"

**Diagnosis.** The battery uevent path is not receiving updates.

**Fix.**

```sh
adb shell logcat -d -s DFPS_Daemon:* | grep -i battery
```

You should see lines like `evaluateBatteryState: level=15`. If
not, the netlink socket is not receiving uevents. Try:

```sh
adb shell su -c 'uevent --parse'
```

If this returns nothing, the kernel is not sending uevents. This
is a kernel issue, not a DFPS issue.

### 7.3 "Touch fds keep getting closed and reopened"

**Symptom.** Logs show `Battery low, closing touch devices` and
`Battery recovered, reopening touch devices` repeatedly around
the threshold.

**Diagnosis.** The battery is hovering at the threshold, causing
mode-flap. DFPS has a +2% hysteresis specifically to prevent this,
but if the kernel reports coarse levels (5%, 10%, 15%, ...), the
hysteresis may not be enough.

**Fix.** Set `batterySaver=false` or raise the threshold:

```ini
lowBatteryThreshold = 5
```

---

## 8. The WebUI Is Broken

### 8.1 WebUI Opens but the Page Is Empty

**Diagnosis.** The daemon's `@dfps` socket is not bound. The WebUI
fetches data via a script that uses `ksu.exec()` to read
`/proc/net/unix` and `dumpsys SurfaceFlinger`.

**Fix.**

```sh
adb shell logcat -d -s DFPS_Daemon:* | grep -E "socket|Abstract"
```

You should see `Abstract socket server successfully listening on:
@dfps`. If not, see §3.

### 8.2 WebUI "Add Mode" Doesn't Save

**Diagnosis.** The WebUI sends commands via `ksu.exec()`. The
helper script is broken or the daemon's PID has changed.

**Fix.**

```sh
adb shell logcat -d -s ksu:* | tail -20
```

The most common cause is a KernelSU / Axeron version mismatch.
The WebUI calls `ksu.exec("…")` (or `axeron.exec`) which must be
exposed by the active supervisor.

### 8.3 WebUI Says "Permission Denied" on Save

**Diagnosis.** The WebUI's `ksu.exec()` is being denied because
the supervisor's policy does not allow the WebUI's UID.

**Fix.** Open KernelSU / Axeron's app, find the WebUI in
**Superuser**, and grant it **root** access.

---

## 9. Logcat Analysis

### 9.1 Tag Reference

| Tag                  | Source               | What it logs                         |
|----------------------|----------------------|--------------------------------------|
| `DFPS_Daemon`        | The daemon           | All DFPS messages.                   |
| `Magisk`             | Magisk supervisor    | Service start/stop.                  |
| `ksu`                | KernelSU             | Service start/stop (newer versions). |
| `SurfaceFlinger`     | System               | `setActiveConfig` calls.             |
| `ActivityManager`    | System               | Task stack changes.                  |

### 9.2 Useful One-Liners

```sh
# Live tail with level filter
adb shell logcat -s DFPS_Daemon:*

# All DFPS messages since boot
adb shell logcat -d -s DFPS_Daemon:*

# Just errors
adb shell logcat -d -s DFPS_Daemon:E

# Filter for rate transitions
adb shell logcat -d -s DFPS_Daemon:* | grep -E "Hz|Transitioning"

# Filter for foreground changes
adb shell logcat -d -s DFPS_Daemon:* | grep -E "Focused package|rule"

# Filter for problems
adb shell logcat -d -s DFPS_Daemon:E DFPS_Daemon:W

# Capture to a file for sharing
adb shell logcat -d -s DFPS_Daemon:* > dfps.log
```

### 9.3 Expected Steady-State Output

When running cleanly with `DEBUG=true`, you should see something
like:

```
[DFPS] ==============================================
[DFPS]       Dynamic FPS Controller Initiated
[DFPS] ==============================================
[DFPS] UID: 0 | Execution Profile: ROOT
[DFPS]   - Registered input device: /dev/input/event3
[DFPS]   - Registered input device: /dev/input/event5
[DFPS] Monitoring configuration directory for live changes: /data/local/tmp/dfps
[DFPS] Abstract socket server successfully listening on: @dfps
[DFPS] Netlink uevent listener initialized for battery events
[DFPS] Battery listener registered successfully via Binder
[DFPS] Loaded 8 SurfaceFlinger mode mappings successfully.
[DFPS]   - Mapping: 60 Hz -> SF ID 1
[DFPS]   - Mapping: 90 Hz -> SF ID 2
[DFPS]   - Mapping: 120 Hz -> SF ID 3
[DFPS] Parsed dfps.conf successfully. Rules loaded: 12 rules
[DFPS]   - Mapping: 60 Hz -> SF ID 1
[DFPS]   - Mapping: 90 Hz -> SF ID 2
[DFPS]   - Mapping: 120 Hz -> SF ID 3
[DFPS] Focused package matches rule: 'com.android.chrome' -> idle: 60 Hz, active: 120 Hz
[DFPS] Focused package matches rule: 'com.google.android.youtube' -> idle: 60 Hz, active: 90 Hz
```

**No errors should appear in steady state.** If you see `LOGE`
lines, see the relevant section above.

### 9.4 Diagnosing a Stuck Rate

```sh
adb shell logcat -c
# Now do the action that should change the rate (e.g. open a specific app)
sleep 5
adb shell logcat -d -s DFPS_Daemon:*
```

You should see:

1. `Focused package matches rule: '…' -> idle: … Hz, active: … Hz`
2. Either `LOG_HOT("…")` (currently a no-op) or no `LOG_HOT` line
   if the rate is unchanged.

If you see (1) but no `setActiveConfig` in `SurfaceFlinger`
logcat, the SF transaction failed — see §3.4 / §4.3.

---

## 10. Recovery and Reset

### 10.1 Soft Reset (restart the daemon)

```sh
adb shell pkill -f /data/local/tmp/dfps/dfps
# The supervisor will respawn within 1–2 seconds.
```

### 10.2 Hard Reset (clear all state)

```sh
adb shell
pkill -9 -f /data/local/tmp/dfps/dfps
rm -rf /data/local/tmp/dfps
rm -f /data/local/tmp/tx_code.txt
rm -f /data/local/tmp/resolver.jar
# Now reboot or reflash the module.
```

### 10.3 Disable the Module Temporarily

- **Magisk:** Modules → tap **dfps** → **Disable** → reboot.
- **KernelSU:** Modules → tap **dfps** → toggle off → reboot.
- **Axeron:** Modules → tap **dfps** → **Disable** → reboot.

### 10.4 Full Uninstall

```sh
# Method 1: from the module's uninstall.sh
adb shell sh /data/adb/modules/dfps/uninstall.sh

# Method 2: from the module UI (Magisk / KernelSU / Axeron)
# Modules → dfps → Uninstall → reboot.
```

Both methods:

- Kill the daemon.
- Remove `/data/local/tmp/dfps/`.
- Remove `/data/local/tmp/tx_code.txt`.

The module directory itself is removed by the supervisor.

### 10.5 Capture Diagnostic Bundle

For bug reports, capture all of:

```sh
adb shell "\
  tar czf /sdcard/dfps_diag.tgz \
    /data/local/tmp/dfps \
    /data/local/tmp/tx_code.txt 2>/dev/null; \
  logcat -d -s DFPS_Daemon:* > /sdcard/dfps_logcat.txt; \
  dumpsys SurfaceFlinger | head -200 > /sdcard/dfps_sf.txt; \
  getprop ro.build.fingerprint > /sdcard/dfps_fingerprint.txt"

adb pull /sdcard/dfps_diag.tgz .
adb pull /sdcard/dfps_logcat.txt .
adb pull /sdcard/dfps_sf.txt .
adb pull /sdcard/dfps_fingerprint.txt .
```

Then attach all four to your bug report. **Do not include your
`dfps.conf` if it contains package names you consider private.**
