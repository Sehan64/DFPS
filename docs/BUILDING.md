# Building

Plain C11 (`gnu11`) against Android system libraries. Top-level `Makefile`
targets Termux native builds and NDK-style cross builds.

## Requirements

- `clang`, `make`
- headers/libs for `libc`, `libdl` (and `liblog` at runtime on device)
- `src/resolver_bytes.h` — embedded Java helper used at first boot to resolve
  Binder transaction codes (see below)

Termux:

```bash
pkg install clang make
make
```

## Targets

| Target | Purpose |
|---|---|
| `make` / `make all` | Release binary → `bin/<abi>/dfps` |
| `make debug` | ASan + UBSan, `-O0 -g3`, no LTO |
| `make check` | Syntax-only (`-fsyntax-only`) |
| `make test` | Config / reload regression tests under `tests/` |
| `make test SAN=1` | Same tests with ASan/UBSan |
| `make install` | Copy release binary to `/data/local/tmp/dfps/bin/dfps` |
| `make clean` | Remove build artifacts |

ABI is detected from `uname -m` (`arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86`).

## Profiles

- **release** (default): `-O3`, LTO, `-march` baseline for the ABI (not
  `-march=native`), stack protector, FORTIFY, full RELRO/NOW; stripped.
- **debug**: sanitizers and symbols; not for production devices without care.

Overrides:

```bash
make CFLAGS_EXTRA='-DFOO' LDFLAGS_EXTRA=...
make DFP_BUILD_STAMP=local-dev
```

`DFP_BUILD_STAMP` defaults to `git describe --always --dirty --tags` and is
embedded for `--version`.

## `resolver_bytes.h`

The daemon embeds a small `app_process` helper JAR as a C byte array. On first
run (or cache miss) it resolves ROM-specific Binder transaction codes and
writes a fingerprint-scoped cache under `/data/local/tmp/dfps/`.

- Ship a real generated header from release packaging.
- A stub is enough for `make check` / host-side syntax only.
- Runtime without a real helper fails when core AM codes cannot be resolved.

## Tests

```bash
make test
# or
make test SAN=1
```

Tests compile against real `config.c` / `rate.c` / … with a writable temp dir
for `DFPS_CONFIG_PATH` / `DFPS_MODES_MAP_PATH`. They do not start the full
daemon or require root. Binder symbols are stubbed in `tests/test_stubs.c`.

## Cross builds

Put the NDK `clang` for the target ABI first in `PATH`, then `make` on the
host. Prefer building on-device (Termux) when possible to avoid ABI mismatch.
