# Building

DFPS is plain C11 plus Android system libraries. The build is driven by the
top-level `Makefile`.

## Requirements

- `clang`
- `make`
- `libc`, `libdl`, and `liblog` headers/libs from Termux or the Android NDK
- `src/resolver_bytes.h`

## Common targets

```bash
make          # release build
make debug    # ASan + UBSan build
make check    # syntax-only check
make test     # regression test(s)
make install  # copy binary to /data/local/tmp/dfps/bin/dfps
```

## Build profiles

- `release` is the default. It uses `-O3`, LTO, and an arm64 baseline.
- `debug` enables sanitizers and keeps symbols.

## `resolver_bytes.h`

The daemon embeds a small Java helper JAR as `src/resolver_bytes.h`. It is used
to resolve Binder transaction codes that are not fixed across Android versions.

You can:

- copy a generated header from a release artifact
- generate one from the helper JAR
- stub it for syntax-only checks

Runtime startup still requires a real helper. A stub is only useful for
compilation checks.

## Test build

`make test` builds a small regression binary and runs it against local test
data. The tests live under `tests/` and use temporary files in `.testdata/`.

## Cross builds

The Makefile auto-selects the ABI from `uname -m`. On a desktop host, set the
NDK toolchain in `PATH` and build normally with `make`.
