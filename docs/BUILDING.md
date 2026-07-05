# Building from source

The daemon is plain C11 with no external dependencies beyond the NDK /
Android system libraries that ship on every device. The build is driven by
a hand-written `Makefile`; no CMake, no Ninja, no autoconf.

---

## Table of Contents

- [Requirements](#requirements)
- [Build profiles](#build-profiles)
- [Quick start (Termux)](#quick-start-termux)
- [Quick start (Linux + Android NDK)](#quick-start-linux--android-ndk)
- [The `resolver_bytes.h` requirement](#the-resolver_bytesh-requirement)
  - [Where to get it](#where-to-get-it)
  - [Generating it yourself](#generating-it-yourself)
  - [Building without it](#building-without-it)
- [Build outputs and ABI selection](#build-outputs-and-abi-selection)
- [Creating the installable module zip](#creating-the-installable-module-zip)
- [Cross-compiling from x86_64](#cross-compiling-from-x86_64)
- [Sanitizers and static analysis](#sanitizers-and-static-analysis)
- [Environment variables](#environment-variables)
- [Makefile targets](#makefile-targets)
- [Troubleshooting](#troubleshooting)

---

## Requirements

You need **one of**:

| Environment | Pros | Cons |
|---|---|---|
| **Termux on the target device** | Nothing to set up, you build straight on the phone. The resulting binary runs on the same device with no surprises. | Slow; uses the device's storage and battery. |
| **Linux x86_64 + Android NDK r25+** | Fast, reproducible, CI-friendly. | You need to install the NDK and the right clang. |
| **macOS + Android NDK** | Works; same approach as Linux. | Slightly more involved cross-toolchain setup. |

Toolchain components in all cases:

- **`clang`** (C11 capable). Any recent release (14+) is fine.
- **`make`**.
- The C runtime headers and stub libraries for `libc`, `liblog`, `libdl`.
  On Termux these come from the base system; with the NDK they come from
  `$NDK/toolchains/llvm/prebuilt/.../sysroot`.
- A `resolver_bytes.h` file containing a tiny Java helper JAR. See below.

Optional:

- **`llvm-strip` or GNU `strip`** for stripping the release binary
  (auto-detected by the Makefile; falls back to `true` if neither is
  available).

---

## Build profiles

The Makefile supports two profiles, selectable via `PROFILE=`:

| Profile | Flags | Use it for |
|---|---|---|
| `release` *(default)* | `-O3 -flto -march=armv8-a -mtune=generic -fno-plt -fomit-frame-pointer -fvisibility=hidden -ffunction-sections -fdata-sections -fmerge-all-constants -fno-semantic-interposition`, with the stack protector **enabled** by default. Links with `-flto -Wl,--gc-sections -Wl,-O1 -Wl,-z,noseparate-code`. Binary is stripped. | Shipping. Produces a ~48 KB static binary. |
| `debug` | `-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer -DDEBUG`, links with ASan + UBSan. Not stripped. | Local development, crash hunting, leak detection. |

Select with `make PROFILE=debug`, or run the convenience target `make
debug` which does the same thing.

> **Why `-march=armv8-a` and not `-march=native`?** Building on a phone
> with, say, ARMv8.4-A features (like `FRINT`, `SHA3`, `DOTPROD`) would
> produce a binary that SIGILLs on older arm64 devices. The default
> baseline ensures the binary runs on every arm64-v8a device.

---

## Quick start (Termux)

```bash
pkg install clang make
git clone https://github.com/Sehan64/DFPS.git
cd DFPS
# obtain or generate src/resolver_bytes.h (see below)
make
# -> bin/arm64-v8a/dfps

# install into the runtime path used by the module / daemon:
make install
# -> /data/local/tmp/dfps/bin/dfps   (binary)
# symlinks into /data/local/tmp/dfps/ for dfps, modes.map, dfps.conf
#    are created by customize.sh — see INSTALLATION.md
```

Run manually to test:

```bash
su -c /data/local/tmp/dfps/bin/dfps
logcat -s DFPS_Daemon
```

---

## Quick start (Linux + Android NDK)

```bash
# 1. Install the NDK
#    e.g. via sdkmanager:
sdkmanager --install "ndk;25.2.9519653"
export NDK=$ANDROID_HOME/ndk/25.2.9519653
#    or download from https://developer.android.com/ndk/downloads

# 2. Point clang at the NDK
export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
export CC=aarch64-linux-android24-clang

# 3. Clone and build
git clone https://github.com/Sehan64/DFPS.git
cd DFPS
make
```

The Makefile uses `clang` directly, so `$CC` is honored if set. The
`BASE_ARCH` is auto-selected from `uname -m` — on an x86_64 host it falls
back to `armv8-a` (the cross target) for a portable arm64 build.

If you want to *also* test-execute the binary, use `qemu-aarch64` with
the NDK's sysroot, but most of the daemon's functionality is Android-only
so on-device testing is the practical option.

---

## The `resolver_bytes.h` requirement

`src/resolver_bytes.h` is a generated header containing the bytes of a
tiny Java helper JAR. The daemon uses it to discover Binder transaction
codes that are not exposed in the NDK headers (they are Android-version
specific and would otherwise require a Java resolver at runtime).

### Where to get it

The precompiled `resolver_bytes.h` ships in the **module zip** (under
`system/bin/dfps`, the binary embeds the same bytes). For source builds:

- **Option A — copy from the module zip.** Extract `dfps.zip` and use the
  binary's `.rodata` as the source of truth. The Makefile does **not**
  currently extract this; you can dump it with:
  ```bash
  # on a built release binary, look for the embedded JAR signature:
  xxd bin/arm64-v8a/dfps | grep -m1 '504b0304'
  ```
  but the more reliable path is to regenerate it.

- **Option B — pull from the GitHub release artifact.** Each release of
  DFPS includes a `resolver_bytes.h` next to the source. Drop it in
  `src/` and `make` will just work.

### Generating it yourself

The helper is a single-class Java program (`android.app.CodeResolver`)
that uses reflection to read the transaction code constants out of the
system's `IActivityManager$Stub`, `IProcessObserver$Stub`, etc. classes
and prints them to stdout.

```bash
# 1. compile the helper to a JAR
mkdir -p src/android/app
cat > src/android/app/CodeResolver.java <<'EOF'
package android.app;
public class CodeResolver {
    public static void main(String[] args) throws Exception {
        for (int i = 0; i < args.length; i += 2) {
            String iface = args[i];
            String field = args[i+1];
            Class<?> c = Class.forName(iface);
            System.out.println(iface + " " + field + "=" + c.getField(field).getInt(null));
        }
    }
}
EOF
javac -source 1.8 -target 1.8 -d build src/android/app/CodeResolver.java
( cd build && jar cfe ../resolver.jar android.app.CodeResolver android/app/CodeResolver.class )

# 2. convert the JAR into a C header
python3 -c "
import sys
data = open('resolver.jar', 'rb').read()
sys.stdout.write('// Auto-generated from resolver.jar — do not edit.\n\n')
sys.stdout.write('const unsigned char resolver_jar[] = {\n')
for i, b in enumerate(data):
    if i % 12 == 0: sys.stdout.write('    ')
    sys.stdout.write('0x%02x, ' % b)
    if i % 12 == 11: sys.stdout.write('\n')
if len(data) % 12 != 0: sys.stdout.write('\n')
sys.stdout.write('};\n')
" > src/resolver_bytes.h
```

The Makefile refuses to build if `resolver_bytes.h` is empty or missing
(it logs a clear warning) but does **not** fail the build. The daemon
itself will refuse to start without a valid file because
`RESOLVER_SIZE < 100` after the `#include` produces a near-empty array.

### Building without it

If you only want to validate that the C code compiles (e.g. in CI without
the Java toolchain), you can stub it:

```bash
echo '// stub for CI' > src/resolver_bytes.h
echo 'const unsigned char resolver_jar[] = {0};' >> src/resolver_bytes.h
make check
```

The daemon will exit at startup with *"resolver JAR size is too small"*.
This is only meant for static analysis and CI smoke tests.

---

## Build outputs and ABI selection

The Makefile auto-detects the host architecture via `uname -m` and maps it
to the Android ABI and the right `-march` baseline:

| Host | ABI (`bin/`) | `-march=` |
|---|---|---|
| `aarch64` | `arm64-v8a` | `armv8-a` |
| `armv7l` | `armeabi-v7a` | `armv7-a` |
| `x86_64`  | `x86_64`    | `x86-64` |
| `i686`    | `x86`       | `i686` |
| anything else | as-is | as-is |

The shipping module is arm64-only. To build the other ABIs for testing:

```bash
make x86_64      # -> bin/x86_64/dfps
make arm         # -> bin/armeabi-v7a/dfps
```

> **Note on `armeabi-v7a`.** The daemon uses 64-bit atomics and relies
> on a 32-bit FNV-1a hash; both work on 32-bit ARMv7 too, but the
> transaction-code resolver helper JAR is pre-compiled for `classes.dex`
> which is bytecode, not native — same artifact works on all ABIs.

---

## Creating the installable module zip

The module zip layout is:

```text
dfps.zip/
├── META-INF/
│   └── com/google/android/
│       ├── update-binary        # your manager's flash script
│       └── updater-script
├── module.prop
├── customize.sh
├── service.sh
├── action.sh
├── uninstall.sh
├── webroot/
│   └── index.html
└── system/
    └── bin/
        └── dfps                 # the built binary
```

A minimal `update-binary` (used by Magisk, KernelSU, and Axeron):

```bash
#!/sbin/sh
. "$MODPATH"/../functions.sh  # or your manager's equivalent
install_module
```

To build a release zip from the source tree (assuming the
`module/META-INF`, `module/customize.sh`, etc. skeleton lives in
`module/`):

```bash
make
cd module
cp ../bin/arm64-v8a/dfps system/bin/dfps
zip -r ../dfps.zip . -x '*.DS_Store'
```

> The repository does not currently ship a `module/` skeleton — it's
> maintained alongside the `dfps.zip` artifact in the release tarball.
> See [MODULE_PACKAGING.md](MODULE_PACKAGING.md) for the full breakdown
> of every file in the zip.

---

## Cross-compiling from x86_64

Two options:

**A. Use the NDK's prebuilt clang (recommended):**

```bash
NDK=$ANDROID_HOME/ndk/25.2.9519653
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
    $(make echo-flags) src/main.c ... -o dfps
```

**B. Use plain clang with the NDK sysroot:**

```bash
SYSROOT=$ANDROID_HOME/ndk/25.2.9519653/toolchains/llvm/prebuilt/linux-x86_64/sysroot
clang -O3 -flto -march=armv8-a -mtune=generic --target=aarch64-linux-android24 \
      --sysroot=$SYSROOT -fuse-ld=lld -static \
      -I$SYSROOT/usr/include -L$SYSROOT/usr/lib/aarch64-linux-android/24 \
      src/*.c -o dfps
```

The Makefile's defaults are tuned for both paths.

---

## Sanitizers and static analysis

The `debug` profile enables AddressSanitizer + UndefinedBehaviorSanitizer.
You can opt-in to other checks via `CFLAGS_EXTRA`:

```bash
# Extra UBSan checks
make PROFILE=debug CFLAGS_EXTRA='-fno-sanitize-recover=all'

# Static analysis via scan-build (use the system clang)
scan-build make PROFILE=debug

# clang-tidy
clang-tidy --checks=bugprone-*,cert-*,performance-* \
           src/*.c -- $(make echo-flags) -I src
```

`make` does not currently integrate with these; invoke them externally.

---

## Environment variables

| Variable | Effect |
|---|---|
| `PROFILE` | `release` (default) or `debug`. |
| `CFLAGS_EXTRA` | Extra flags appended to `CFLAGS`. |
| `LDFLAGS_EXTRA` | Extra flags appended to `LDFLAGS`. |
| `CC` | Override `clang` (e.g. for cross-compilation). |
| `ANDROID_HOME` / `ANDROID_NDK_HOME` | Standard NDK locations if you wrap `make` in your own CI script. |

---

## Makefile targets

```text
all           Build the default profile into bin/<ABI>/dfps.
debug         Build PROFILE=debug into bin/<ABI>/dfps.
check         Syntax check (clang -fsyntax-only) on src/main.c only.
clean         Remove bin/.
install       Install bin/<ABI>/dfps to /data/local/tmp/dfps/bin/dfps.
uninstall     Remove the installed binary.
arm64 / arm / Convenience targets — same as `all` but disambiguated
x86_64 / x86   in case the auto-detect is wrong.
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `WARNING: src/resolver_bytes.h looks like the placeholder.` | The file is missing or has fewer than 2 lines. | See the "Generating it yourself" section above. |
| `error: 'CLOCK_MONOTONIC_COARSE' undeclared` | The host's libc doesn't expose the coarse clock. | Update to a newer Termux (`pkg update && pkg upgrade`) or switch to the NDK clang. |
| Linking fails with `cannot find -lbinder_ndk` | The NDK sysroot doesn't have it (older NDKs). | Update to NDK r23 or newer, or use Termux. |
| Binary runs but immediately exits with "Failed writing resolver JAR" | `/data/local/tmp/` is not writable, or the partition is read-only. | Check `mount | grep /data`; you may be in a strange boot mode. |
| `error: linker not found` on a clean Termux install | `lld` is missing. | `pkg install lld` or let clang fall back to its built-in linker. |
