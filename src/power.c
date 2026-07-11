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
 * See the License for the Specific language governing permissions and
 * limitations under the License.
 */

/*
 * power.c — Battery, brightness, and power-save management
 *
 * Monitors battery level via Binder and netlink uevent, tracks screen
 * brightness, and manages power-save / low-battery mode transitions.
 */

#include "dfps.h"

/* ================================================================== */
/*  Battery level (sysfs fallback)                                     */
/* ================================================================== */

static char s_battery_supply_name[64] = {0};

int32_t readInitialBatteryLevel(void) {
    /* Fast path: if we already know the battery supply name, read it directly. */
    if (s_battery_supply_name[0] != '\0') {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity",
                 s_battery_supply_name);
        FILE* f = fopen(path, "r");
        if (f) {
            int level = 100;
            if (fscanf(f, "%d", &level) == 1 && level >= 0 && level <= 100) {
                fclose(f);
                return level;
            }
            fclose(f);
        }
        /* Fall through to re-scan if the cached path stopped working. */
        s_battery_supply_name[0] = '\0';
    }

    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return 100;

    struct dirent* entry;
    int level = 100;
    char name_buf[256] = {0};

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        snprintf(name_buf, sizeof(name_buf),
                 "/sys/class/power_supply/%s/type", entry->d_name);
        FILE* f = fopen(name_buf, "r");
        if (f) {
            char type[32] = {0};
            if (fgets(type, sizeof(type), f)) {
                size_t nl = strcspn(type, "\n");
                if (nl < sizeof(type)) type[nl] = '\0';
                if (strcmp(type, "Battery") == 0) {
                    fclose(f);
                    strlcpy(s_battery_supply_name, entry->d_name,
                            sizeof(s_battery_supply_name));
                    snprintf(name_buf, sizeof(name_buf),
                             "/sys/class/power_supply/%s/capacity",
                             entry->d_name);
                    FILE* cf = fopen(name_buf, "r");
                    if (cf) {
                        if (fscanf(cf, "%d", &level) != 1) level = 100;
                        fclose(cf);
                    }
                    closedir(dir);
                    return level;
                }
            }
            fclose(f);
        }
    }
    closedir(dir);
    return level;
}

/* ================================================================== */
/*  Interactive / power-save state queries (Binder)                    */
/* ================================================================== */

void checkInteractiveAndPowerSave(bool probe_interactive) {
    if (!g_hot_binders.powerManager) return;

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
                int32_t exception = -1;
                int32_t result = -1;
                if (g_hot_ops.readInt32(reply, &exception) == STATUS_OK && exception == 0) {
                    if (g_hot_ops.readInt32(reply, &result) == STATUS_OK) {
                        bool interactive = (result != 0);
                        if (interactive != atomic_load_explicit(&g_screen_interactive,
                                                                 memory_order_relaxed)) {
                            LOGI("Interactive screen state changed: screen is now %s",
                                  interactive ? "ON" : "OFF");
                            atomic_store_explicit(&g_screen_interactive, interactive,
                                                   memory_order_release);
                            triggerPollerWakeup();
                        }
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
                int32_t exception = -1;
                int32_t result = -1;
                if (g_hot_ops.readInt32(reply2, &exception) == STATUS_OK && exception == 0) {
                    if (g_hot_ops.readInt32(reply2, &result) == STATUS_OK) {
                        bool power_save = (result != 0);
                        if (power_save != atomic_load_explicit(&g_power_save_mode,
                                                                memory_order_relaxed)) {
                            LOGI("System Power save mode changed: %s",
                                 power_save ? "ON" : "OFF");
                            atomic_store_explicit(&g_power_save_mode, power_save,
                                                   memory_order_release);
                            triggerPollerWakeup();
                        }
                    }
                }
                g_hot_ops.deleteParcel(reply2);
            }
        }
    }
}

/* ================================================================== */
/*  Brightness clamping                                                */
/* ================================================================== */

void checkMinBrightness(void) {
    if (!atomic_load_explicit(&g_enable_min_brightness, memory_order_relaxed) ||
        !g_hot_binders.displayManager ||
        g_hot_ops.resolvedGetBrightnessCode == 0) {
        return;
    }

    AParcel* in_b = NULL;
    if (g_hot_ops.prepareTransaction(g_hot_binders.displayManager, &in_b) != STATUS_OK || !in_b)
        return;

    g_hot_ops.writeInt32(in_b, 0); /* displayId = 0 */
    AParcel* reply_b = NULL;
    binder_status_t status_b = g_hot_ops.transact(
        g_hot_binders.displayManager,
        g_hot_ops.resolvedGetBrightnessCode, &in_b, &reply_b, 0);
    if (status_b != STATUS_OK || !reply_b) {
        if (reply_b) g_hot_ops.deleteParcel(reply_b);
        return;
    }

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
                    triggerPollerWakeup();
                }
            }
        }
    }
    g_hot_ops.deleteParcel(reply_b);
}

/* ================================================================== */
/*  Battery state evaluation                                           */
/* ================================================================== */

void evaluateBatteryState(int32_t level) {
    bool batt_saver_on = atomic_load_explicit(&g_battery_saver, memory_order_relaxed);
    if (!batt_saver_on) return;

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
            triggerPollerWakeup();
        }
        /* If CAS failed, another thread already updated the value;
         * the winning thread's triggerPollerWakeup() handles the state change. */
    }
}

/* ================================================================== */
/*  Netlink uevent handler                                             */
/* ================================================================== */

bool handleUevent(void) {
    char buf[8192];
    ssize_t len = recv(g_uevent_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (len <= 0) return false;

    buf[len] = '\0';

    char* p = buf;
    char* end = buf + len;
    bool is_power_supply = false;
    int new_level = -1;

    while (p < end) {
        if (strcmp(p, "SUBSYSTEM=power_supply") == 0) {
            is_power_supply = true;
        } else if (strncmp(p, "POWER_SUPPLY_CAPACITY=", 22) == 0) {
            char* endptr = NULL;
            long val = strtol(p + 22, &endptr, 10);
            if (endptr != p + 22 && val >= 0 && val <= 100) {
                new_level = (int)val;
            }
        }
        p += strlen(p) + 1;
    }

    if (is_power_supply && new_level >= 0) {
        evaluateBatteryState(new_level);
    }
    return true;
}
