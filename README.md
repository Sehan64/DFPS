# dfpsd

Root-resident Android daemon that switches display refresh rate from touch,
foreground app, brightness, and battery state. Ships as a Magisk / KernelSU /
Axeron module and exposes a small `@dfps` control socket.

Fork of [yc9559/dfps](https://github.com/yc9559/dfps), rewritten in C for
low overhead. Configuration divergences from upstream are documented in
[`docs/HELP.md`](docs/HELP.md).

## Behavior

| Input | Effect |
|---|---|
| Touch down (after 50 ms debounce) | Active rate for the focused app |
| Touch up + `touchSlackMs` | Drop back to idle rate |
| Foreground app change | Apply per-app idle/active rules (or defaults) |
| Low brightness (optional) | Clamp to minimum physical rate |
| Power-save / low battery (optional) | Cap the active rate |
| Screen off | Optional `offscreenRate`, or leave the panel alone |

Config and mode map hot-reload from `/data/local/tmp/dfps/` via inotify.

## Quick start

```bash
# build on-device (Termux) or with NDK clang in PATH
make
make install   # → /data/local/tmp/dfps/bin/dfps

su -c '/data/local/tmp/dfps/dfps --version'
su -c 'pgrep -af dfps'
logcat -s DFPS_Daemon
```

Module install: flash `dfps.zip` in Magisk / KernelSU / Axeron, then reboot.
See [`docs/INSTALLATION.md`](docs/INSTALLATION.md).

## Runtime files

| Path | Role |
|---|---|
| `/data/local/tmp/dfps/dfps` | Daemon binary (module symlink) |
| `/data/local/tmp/dfps/dfps.conf` | Globals + per-app rules |
| `/data/local/tmp/dfps/modes.map` | Hz → SurfaceFlinger config ID |
| abstract `@dfps` | Foreground push + `STATUS` health |

Prefer a root-owned `0700` directory so unprivileged apps cannot rewrite rates.

## Documentation

| Doc | Contents |
|---|---|
| [`docs/HELP.md`](docs/HELP.md) | User-facing option guide |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | File format reference |
| [`docs/INSTALLATION.md`](docs/INSTALLATION.md) | Module install / verify / uninstall |
| [`docs/BUILDING.md`](docs/BUILDING.md) | Build, tests, `resolver_bytes.h` |
| [`docs/OPS.md`](docs/OPS.md) | Runbook: process, STATUS, logs, reload |
| [`docs/CLIENT_PROTOCOL.md`](docs/CLIENT_PROTOCOL.md) | `@dfps` wire protocol |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Threads, rate logic, hot path |
| [`docs/INIT.md`](docs/INIT.md) | Boot, hardening, capabilities |
| [`docs/SECURITY.md`](docs/SECURITY.md) | Threat model and auth |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | Common failures |

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).
