# Troubleshooting

Use `logcat -s DFPS_Daemon` first. Most failures are visible there.

## Daemon does not start

- Check that the module is installed and `/data/local/tmp/dfps/dfps` exists.
- Verify `libbinder_ndk.so` is present on the device.
- If the log says the singleton socket is in use, stop the other DFPS copy
  first.
- If transaction-code resolution fails, the device build likely changed and the
  cache needs regeneration.

## Daemon exits immediately

- Look for Binder service death messages.
- Confirm `ActivityManager`, `PowerManager`, `SurfaceFlinger`, and
  `DisplayManager` are available.
- Reboot after a clean reinstall if a stale process is still registered.

## Refresh rate does not change

- Confirm the device has more than one refresh rate in `modes.map`.
- Confirm `dfps.conf` has sensible `defaultIdle` and `defaultActive` values.
- Check that touch input is detected on rooted devices.
- If brightness clamp or battery saver is enabled, they may override the active
  rate.

## Touch input does not work

- Root mode is required for `/dev/input/event*`.
- Some devices need SELinux or input-device access tweaks.
- If the device screen is off, the kernel may suppress touch events.

## Foreground app tracking looks wrong

- Use the `@dfps` socket to inspect the current foreground list.
- Check that `IProcessObserver` is resolving correctly.
- On some ROMs, task names may be process names rather than package names.

## Battery or brightness behavior looks wrong

- Battery saver only matters if `batterySaver=true`.
- Brightness clamp only matters if `enableMinBrightness=true`.
- Both features are event-driven, so trigger a display or battery change to
  re-evaluate state.

## WebUI does not load

- Verify the manager actually supports the bundled WebUI scheme.
- Confirm the module installed successfully and `webroot/index.html` exists.
- Reinstall if the module zip was modified manually.
