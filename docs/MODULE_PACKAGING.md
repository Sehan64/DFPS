# Module Packaging

DFPS ships as a tri-compatible root module ZIP.

## Layout

```text
dfps.zip
├── module.prop
├── customize.sh
├── service.sh
├── action.sh
├── uninstall.sh
├── system/bin/dfps
└── webroot/index.html
```

## Files

- `module.prop` identifies the module and its version.
- `customize.sh` runs at install time and generates `dfps.conf` and
  `modes.map`.
- `service.sh` starts the daemon at boot.
- `action.sh` is the manager action button.
- `uninstall.sh` removes module-owned state.
- `system/bin/dfps` is the daemon binary.
- `webroot/index.html` is the bundled WebUI.

## Axeron support

`axeronPlugin=14000` is included in `module.prop`. Magisk and KernelSU ignore
it, but Axeron uses it to recognize the zip as a plugin.

## After install

The installer creates these runtime paths:

- `/data/local/tmp/dfps/dfps`
- `/data/local/tmp/dfps/dfps.conf`
- `/data/local/tmp/dfps/modes.map`

Those paths are where the daemon reads and reloads its runtime state.

## Repackaging

If you repack the module, keep the same file layout and preserve the symlinks
under `/data/local/tmp/dfps/`. If you change the runtime paths, update the
daemon and docs together.
