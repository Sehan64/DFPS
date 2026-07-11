# Builds dfps for Termux / Android NDK (native compilation)
#
# Usage in Termux:
#   pkg install clang make
#   make            # release build for current architecture
#   make debug      # debug build with sanitizers / no LTO
#   make check      # syntax check only
#   make clean
#
# Requires src/resolver_bytes.h to exist (see that file for details).

# Use '>' instead of TAB to avoid browser copy-paste issues
.RECIPEPREFIX := >

SRC      := src/main.c src/utils.c src/config.c src/rate.c src/binder.c src/power.c src/touch.c
PROFILE_STAMP = bin/$(ABI)/.profile-$(PROFILE)

# Auto-detect architecture (common on Termux)
ARCH := $(shell uname -m)
ifeq ($(ARCH),aarch64)
    ABI := arm64-v8a
    BASE_ARCH := armv8-a
else ifeq ($(ARCH),armv7l)
    ABI := armeabi-v7a
    BASE_ARCH := armv7-a
else ifeq ($(ARCH),x86_64)
    ABI := x86_64
    BASE_ARCH := x86-64
else ifeq ($(ARCH),i686)
    ABI := x86
    BASE_ARCH := i686
else
    ABI := $(ARCH)
    BASE_ARCH := $(ARCH)
endif

# Build profile: release (default) or debug
PROFILE ?= release

# Common flags
CFLAGS_COMMON := -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
                 -D_GNU_SOURCE -fvisibility=hidden

# Reproducible build stamp: derived from git so the same commit yields the
# same binary. Override with DFP_BUILD_STAMP=... when building outside a tree.
BUILD_STAMP ?= $(shell git describe --always --dirty --tags 2>/dev/null || echo unknown)

# Sanitizer support for `make test SAN=1` (ASan + UBSan). The regression
# binaries are built with -fsanitize=... so memory/UB bugs in the exercised
# code (config parsing, reload) surface at runtime. Leak detection at exit is
# disabled: the test binaries never run the daemon's full teardown, so
# intentionally-global allocations would otherwise be reported as leaks.
ifeq ($(SAN),1)
SAN_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g3
SAN_ENV    := ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
endif

ifeq ($(PROFILE),release)
    # Wide-compatibility baseline instead of -march=native. Native tuning makes
    # binaries that may crash on older CPUs (e.g. CRC32/SHA1/FP16 instructions).
    # Use -mtune=generic to still let the compiler pick good instruction
    # scheduling without introducing new ISA requirements.
    CFLAGS   := -O3 -flto -march=$(BASE_ARCH) -mtune=generic \
                -fno-plt -fno-exceptions -fomit-frame-pointer \
                -ffunction-sections -fdata-sections \
                -fmerge-all-constants -fno-semantic-interposition \
                -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                -DDFP_BUILD_STAMP="\"$(BUILD_STAMP)\"" \
                $(CFLAGS_COMMON)
    # Hardening in release: stack-protector-strong, FORTIFY_SOURCE=2, and
    # full RELRO/NOW (below). Disable only if you measured a real regression:
    #   make PROFILE=release CFLAGS_EXTRA=-fno-stack-protector
    LDFLAGS  := -flto -Wl,--gc-sections -Wl,-O1 -Wl,-z,noseparate-code \
                -Wl,-z,relro -Wl,-z,now -ldl
else ifeq ($(PROFILE),debug)
    CFLAGS   := -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer \
                -DDEBUG -DDFP_BUILD_STAMP="\"$(BUILD_STAMP)\"" $(CFLAGS_COMMON)
    LDFLAGS  := -fsanitize=address,undefined -ldl
else
    $(error Unknown PROFILE=$(PROFILE). Use "release" or "debug")
endif

CFLAGS += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

.PHONY: all clean check test debug arm64 arm x86_64 x86 install uninstall

all: bin/$(ABI)/dfps

$(PROFILE_STAMP):
>@mkdir -p bin/$(ABI)
>@rm -f bin/$(ABI)/.profile-*
>@touch $@

bin/$(ABI)/dfps: $(SRC) src/dfps.h src/resolver_bytes.h $(PROFILE_STAMP)
>@mkdir -p bin/$(ABI)
>@if [ ! -f src/resolver_bytes.h ] || [ "$$(wc -l < src/resolver_bytes.h)" -lt 2 ]; then \
>    echo "WARNING: src/resolver_bytes.h looks like the placeholder."; \
>    echo "See src/resolver_bytes.h for what needs to be filled in."; \
>fi
>@echo "Building dfps for $(ABI) ($(PROFILE), -march=$(BASE_ARCH))..."
>@clang $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)
>@if [ "$(PROFILE)" = "release" ]; then \
>    llvm-strip $@ 2>/dev/null || strip $@ 2>/dev/null || true; \
>fi
>@echo "-> $@"
>@ls -lh $@

debug:
>@$(MAKE) PROFILE=debug all

check:
>@clang -fsyntax-only $(CFLAGS) $(SRC) \
>    && echo "syntax OK (Termux clang)"

test:
>@mkdir -p bin
>@# Use a writable temp dir for the config/modes files. The test binaries
>@# honor DFPS_CONFIG_PATH / DFPS_MODES_MAP_PATH, so this lets the suite
>@# run unprivileged (Termux) and on plain-Linux CI runners, where the
>@# default /data/local/tmp/dfps/ is not writable.
>@DFPS_TEST_DIR=$$(mktemp -d) && echo "Test dir: $$DFPS_TEST_DIR" && \
> echo "Building regression test (reload fallback)..." && \
> clang $(CFLAGS_COMMON) $(SAN_CFLAGS) -Dmain=dfps_main \
>   -DDFPS_CONFIG_PATH="\"$$DFPS_TEST_DIR/dfps.conf\"" \
>   -DDFPS_MODES_MAP_PATH="\"$$DFPS_TEST_DIR/modes.map\"" \
>   tests/reload_fallback_test.c \
>   src/main.c src/utils.c src/config.c src/rate.c \
>   src/power.c src/touch.c tests/test_stubs.c -o bin/test_dfps -ldl -lpthread && \
> $(SAN_ENV) ./bin/test_dfps && \
> echo "Building regression test (config parser)..." && \
> clang $(CFLAGS_COMMON) $(SAN_CFLAGS) -Dmain=dfps_main \
>   -DDFPS_CONFIG_PATH="\"$$DFPS_TEST_DIR/dfps.conf\"" \
>   -DDFPS_MODES_MAP_PATH="\"$$DFPS_TEST_DIR/modes.map\"" \
>   tests/config_parse_test.c \
>   src/main.c src/utils.c src/config.c src/rate.c \
>   src/power.c src/touch.c tests/test_stubs.c -o bin/test_cfg -ldl -lpthread && \
> $(SAN_ENV) ./bin/test_cfg && \
> rm -rf "$$DFPS_TEST_DIR" && \
> echo "ALL REGRESSION TESTS PASSED"

# Convenience targets
arm64: all
arm: all
x86_64: all
x86: all

clean:
>@rm -rf bin

install: all
>@mkdir -p /data/local/tmp/dfps/bin
>@install -m 755 bin/$(ABI)/dfps /data/local/tmp/dfps/bin/
>@echo "Installed dfps to /data/local/tmp/dfps/bin/"

uninstall:
>@rm -f /data/local/tmp/dfps/bin/dfps
>@echo "Uninstalled dfps"
