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

ifeq ($(PROFILE),release)
    # Wide-compatibility baseline instead of -march=native. Native tuning makes
    # binaries that may crash on older CPUs (e.g. CRC32/SHA1/FP16 instructions).
    # Use -mtune=generic to still let the compiler pick good instruction
    # scheduling without introducing new ISA requirements.
    CFLAGS   := -O3 -flto -march=$(BASE_ARCH) -mtune=generic \
                -fno-plt -fno-exceptions -fomit-frame-pointer \
                -ffunction-sections -fdata-sections \
                -fmerge-all-constants -fno-semantic-interposition \
                $(CFLAGS_COMMON)
    # Stack protector is intentionally enabled in release for safety.
    # Disable only if you have measured it as a hot-path problem:
    #   make PROFILE=release CFLAGS_EXTRA=-fno-stack-protector
    LDFLAGS  := -flto -Wl,--gc-sections -Wl,-O1 -Wl,-z,noseparate-code -ldl
else ifeq ($(PROFILE),debug)
    CFLAGS   := -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer \
                -DDEBUG $(CFLAGS_COMMON)
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
>@echo "Building regression test..."
>@clang $(CFLAGS_COMMON) -Dmain=dfps_main \
>    tests/reload_fallback_test.c $(SRC) -o bin/test_dfps -ldl -lpthread
>@echo "Running regression test..."
>@./bin/test_dfps && echo "ALL REGRESSION TESTS PASSED"

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
