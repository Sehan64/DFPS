/*
 * Copyright 2023 yc9559
 * Copyright 2026 Sehan64
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * power.c — Battery, brightness, and power-save management
 *
 * Monitors battery level via Binder and netlink uevent, tracks screen
 * brightness, and manages power-save / low-battery mode transitions.
 *
 * check/evaluate helpers return true when a rate-relevant atomic changed.
 * Callers on the epoll thread use the return value to set needs_rate_update
 * and avoid a nested eventfd write (which would force a second epoll_wait
 * round-trip). Binder-pool callers still wake the loop when the return is
 * true.
 */

#include "dfps.h"

/* ================================================================== */
/*  Battery level (sysfs fallback)                                     */
/* ================================================================== */

/* Cached supply name + full capacity path so the hot path is one open/read
 * instead of opendir + type probe + fopen(stdio). */
static char s_battery_supply_name[64] = {0};
static char s_battery_capacity_path[128] = {0};

static int32_t readCapacityAt(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char* end = NULL;
    long val = strtol(buf, &end, 10);
    if (end == buf || val < 0 || val > 100) return -1;
    return (int32_t)val;
}

int32_t readInitialBatteryLevel(void) {
    /* Fast path: known capacity path — single open/read. */
    if (s_battery_capacity_path[0] != '\0') {
        int32_t level = readCapacityAt(s_battery_capacity_path);
        if (level >= 0) return level;
        /* Cached path went stale — rescan. */
        s_battery_capacity_path[0] = '\0';
        s_battery_supply_name[0] = '\0';
    }

    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return 100;

    struct dirent* entry;
    int level = 100;
    char name_buf[256];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        snprintf(name_buf, sizeof(name_buf),
                 "/sys/class/power_supply/%s/type", entry->d_name);
        int tfd = open(name_buf, O_RDONLY | O_CLOEXEC);
        if (tfd < 0) continue;
        char type[32] = {0};
        ssize_t tn = read(tfd, type, sizeof(type) - 1);
        close(tfd);
        if (tn <= 0) continue;
        type[tn] = '\0';
        size_t nl = strcspn(type, "\n\r");
        type[nl] = '\0';
        if (strcmp(type, "Battery") != 0) continue;

        strlcpy(s_battery_supply_name, entry->d_name,
                sizeof(s_battery_supply_name));
        snprintf(s_battery_capacity_path, sizeof(s_battery_capacity_path),
                 "/sys/class/power_supply/%s/capacity", entry->d_name);
        int32_t got = readCapacityAt(s_battery_capacity_path);
        if (got >= 0) level = got;
        closedir(dir);
        return level;
    }
    closedir(dir);
    return level;
}

/* ================================================================== */
/*  Interactive / power-save state queries (Binder)                    */
/* ================================================================== */

/* Read exception code + bool result from a completed PowerManager reply.
 * Returns true when both fields were read and exception == 0. */
static bool read_pm_bool_reply(AParcel* reply, bool* out) {
    int32_t exception = -1;
    int32_t result = -1;
    if (g_hot_ops.readInt32(reply, &exception) != STATUS_OK || exception != 0)
        return false;
    if (g_hot_ops.readInt32(reply, &result) != STATUS_OK)
        return false;
    *out = (result != 0);
    return true;
}

bool checkInteractiveAndPowerSave(bool probe_interactive) {
    bool changed = false;
    if (!g_hot_binders.powerManager) return false;

    /* Query isInteractive.
     * When the IDisplayManager callback is registered, interactive-state
     * changes arrive via that callback (event 2), so callers in the polling
     * path pass probe_interactive=false to avoid a redundant binder query.
     * The power-save-mode probe below is ALWAYS performed: the display
     * callback does not deliver power-save-mode transitions, so skipping it
     * would leave g_power_save_mode stale and defeat the power-save cap. */
    if (probe_interactive) {
        AParcel* in = NULL;
        if (g_hot_ops.prepareTransaction(g_hot_binders.powerManager, &in) == STATUS_OK && in) {
            AParcel* reply = NULL;
            binder_status_t status = g_hot_ops.transact(
                g_hot_binders.powerManager,
                g_hot_ops.resolvedIsInteractiveCode, &in, &reply, 0);
            if (status == STATUS_OK && reply) {
                bool interactive = false;
                if (read_pm_bool_reply(reply, &interactive)) {
                    if (interactive != atomic_load_explicit(&g_screen_interactive,
                                                             memory_order_relaxed)) {
                        LOGI("Interactive screen state changed: screen is now %s",
                              interactive ? "ON" : "OFF");
                        atomic_store_explicit(&g_screen_interactive, interactive,
                                               memory_order_release);
                        changed = true;
                    }
                }
                g_hot_ops.deleteParcel(reply);
            }
        }
    }

    /* Query isPowerSaveMode — always probe (see note above). */
    if (atomic_load_explicit(&g_battery_saver, memory_order_relaxed) &&
        g_hot_ops.resolvedIsPowerSaveModeCode != 0) {
        AParcel* in2 = NULL;
        if (g_hot_ops.prepareTransaction(g_hot_binders.powerManager, &in2) == STATUS_OK && in2) {
            AParcel* reply2 = NULL;
            binder_status_t status = g_hot_ops.transact(
                g_hot_binders.powerManager,
                g_hot_ops.resolvedIsPowerSaveModeCode, &in2, &reply2, 0);
            if (status == STATUS_OK && reply2) {
                bool power_save = false;
                if (read_pm_bool_reply(reply2, &power_save)) {
                    if (power_save != atomic_load_explicit(&g_power_save_mode,
                                                            memory_order_relaxed)) {
                        LOGI("System Power save mode changed: %s",
                             power_save ? "ON" : "OFF");
                        atomic_store_explicit(&g_power_save_mode, power_save,
                                               memory_order_release);
                        changed = true;
                    }
                }
                g_hot_ops.deleteParcel(reply2);
            }
        }
    }
    return changed;
}

/* ================================================================== */
/*  Brightness clamping                                                */
/* ================================================================== */

bool checkMinBrightness(void) {
    if (!atomic_load_explicit(&g_enable_min_brightness, memory_order_relaxed) ||
        !g_hot_binders.displayManager ||
        g_hot_ops.resolvedGetBrightnessCode == 0) {
        return false;
    }

    AParcel* in_b = NULL;
    if (g_hot_ops.prepareTransaction(g_hot_binders.displayManager, &in_b) != STATUS_OK || !in_b)
        return false;

    g_hot_ops.writeInt32(in_b, 0); /* displayId = 0 */
    AParcel* reply_b = NULL;
    binder_status_t status_b = g_hot_ops.transact(
        g_hot_binders.displayManager,
        g_hot_ops.resolvedGetBrightnessCode, &in_b, &reply_b, 0);
    if (status_b != STATUS_OK || !reply_b) {
        if (reply_b) g_hot_ops.deleteParcel(reply_b);
        return false;
    }

    bool changed = false;
    int32_t exception_b = -1;
    if (g_hot_ops.readInt32(reply_b, &exception_b) == STATUS_OK && exception_b == 0) {
        float brightness = 1.0f;
        if (g_hot_ops.readFloat(reply_b, &brightness) == STATUS_OK) {
            if (brightness >= 0.0f) {
                int32_t threshold = atomic_load_explicit(&g_min_brightness_threshold,
                                                          memory_order_acquire);
                bool new_min_bright = (brightness * 100.0f) <= (float)threshold;
                bool current_min_bright = atomic_load_explicit(&g_min_brightness_clamp,
                                                                memory_order_acquire);
                if (new_min_bright != current_min_bright) {
                    LOGI("Min brightness clamp: %s (brightness: %.2f, threshold: %d%%)",
                         new_min_bright ? "ON" : "OFF", brightness, threshold);
                    atomic_store_explicit(&g_min_brightness_clamp, new_min_bright,
                                           memory_order_release);
                    changed = true;
                }
            }
        }
    }
    g_hot_ops.deleteParcel(reply_b);
    return changed;
}

/* ================================================================== */
/*  Battery state evaluation                                           */
/* ================================================================== */

bool evaluateBatteryState(int32_t level) {
    bool batt_saver_on = atomic_load_explicit(&g_battery_saver, memory_order_relaxed);
    if (!batt_saver_on) return false;

    int32_t old_level = atomic_exchange_explicit(&g_battery_level, level,
                                                  memory_order_relaxed);
    if (level != old_level) {
        LOGI("Battery level: %d%%", level);
    }

    int32_t threshold = atomic_load_explicit(&g_low_battery_threshold,
                                              memory_order_relaxed);
    /* Hysteresis: enter low-battery at threshold, exit at threshold+2
     * to prevent oscillation when battery level hovers at the boundary. */
    bool new_low_batt;
    bool current_low_batt = atomic_load_explicit(&g_low_battery_mode,
                                                  memory_order_acquire);
    if (current_low_batt) {
        new_low_batt = (level <= threshold + 2);
    } else {
        new_low_batt = (level <= threshold);
    }

    /* Atomic compare-exchange loop: safely update g_low_battery_mode even
     * when called concurrently from binder and touch threads. */
    if (new_low_batt != current_low_batt) {
        if (atomic_compare_exchange_weak_explicit(
                &g_low_battery_mode, &current_low_batt, new_low_batt,
                memory_order_release, memory_order_acquire)) {
            LOGI("Low battery mode: %s", new_low_batt ? "ON" : "OFF");
            return true;
        }
        /* If CAS failed, another thread already updated the value;
         * the winning thread reports the change. */
    }
    return false;
}

/* ================================================================== */
/*  Netlink uevent handler                                             */
/* ================================================================== */

bool handleUevent(bool* state_changed) {
    if (state_changed) *state_changed = false;

    char buf[8192];
    ssize_t len = recv(g_uevent_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (len <= 0) return false;

    buf[len] = '\0';

    char* p = buf;
    char* end = buf + len;
    bool is_power_supply = false;
    int new_level = -1;

    /* The name of the supply this event refers to, from POWER_SUPPLY_NAME=.
     * power_supply uevents carry this so we can tell a main-battery event
     * from an unrelated supply (dock/keyboard/USB). When s_battery_supply_name
     * is known (we found "Battery"-typed supply at startup), only act on
     * events whose name matches it, so a dock battery doesn't drive the rate
     * cap. When the cached name is unknown (startup scan failed), fall back to
     * the old "any power_supply with a capacity" behavior. */
    const char* supply_name = NULL;

    while (p < end) {
        if (strcmp(p, "SUBSYSTEM=power_supply") == 0) {
            is_power_supply = true;
        } else if (strncmp(p, "POWER_SUPPLY_NAME=", 18) == 0) {
            supply_name = p + 18;
        } else if (strncmp(p, "POWER_SUPPLY_CAPACITY=", 22) == 0) {
            char* endptr = NULL;
            long val = strtol(p + 22, &endptr, 10);
            if (endptr != p + 22 && val >= 0 && val <= 100) {
                new_level = (int)val;
            }
        }
        p += strlen(p) + 1;
    }

    /* batterySaver off: capacity uevents cannot affect rate — drain only. */
    if (is_power_supply && new_level >= 0 &&
        atomic_load_explicit(&g_battery_saver, memory_order_relaxed)) {
        if (s_battery_supply_name[0] != '\0') {
            if (supply_name == NULL ||
                strcmp(supply_name, s_battery_supply_name) != 0) {
                return true; /* Different supply — ignore. */
            }
        }
        bool changed = evaluateBatteryState(new_level);
        if (state_changed) *state_changed = changed;
    }
    return true;
}
