# DFPS

DFPS is a small Android daemon that switches display refresh rate from touch,
foreground app, brightness, and battery state. It ships as a root module for
Magisk, KernelSU, and Axeron, and exposes a simple `@dfps` socket for
foreground-package snapshots.

## What it does

- Touch moves the display to the active rate.
- Idle timeout returns the display to the idle rate.
- Per-app rules override the default rates.
- Brightness clamp can force the minimum rate when dim.
- Battery saver and low-battery mode cap the active rate.
- `dfps.conf` and `modes.map` reload live from `/data/local/tmp/dfps/`.

## Runtime model

- Binder handles ActivityManager, PowerManager, SurfaceFlinger, DisplayManager,
  and battery state.
- One epoll thread handles touch input, inotify reloads, sockets, and uevents.
- The main process starts the Binder thread pool and exits cleanly on
  `SIGTERM` / `SIGINT`.
- `@dfps` is a singleton abstract socket. A second instance fails startup.

## Build and install

- [BUILDING.md](docs/BUILDING.md)
- [INSTALLATION.md](docs/INSTALLATION.md)

Quick start:

```bash
make
make install
su # or adb shell
/data/local/tmp/dfps/dfps
```

## Configuration

- [CONFIGURATION.md](docs/CONFIGURATION.md)
- [CLIENT_PROTOCOL.md](docs/CLIENT_PROTOCOL.md)

Runtime files:

- `/data/local/tmp/dfps/dfps.conf`
- `/data/local/tmp/dfps/modes.map`

## More docs

- [ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [SECURITY.md](docs/SECURITY.md)
- [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)

## License

DFPS is licensed under the Apache License, Version 2.0. See
[LICENSE](LICENSE) for the full text.
