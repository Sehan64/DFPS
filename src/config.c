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
 * config.c — Configuration file parsing
 *
 * Loads dfps.conf and modes.map with validation and live-reload support.
 */

#include "dfps.h"

/* Allow tests to override the runtime file locations without changing the
 * production defaults. */
#ifndef DFPS_CONFIG_PATH
#define DFPS_CONFIG_PATH "/data/local/tmp/dfps/dfps.conf"
#endif

#ifndef DFPS_MODES_MAP_PATH
#define DFPS_MODES_MAP_PATH "/data/local/tmp/dfps/modes.map"
#endif

/* Skip leading ASCII whitespace and return the first non-space character. */
__attribute__((cold, always_inline))
static inline char* skip_leading_ws(char* p) {
    while (isspace((unsigned char)*p)) p++;
    return p;
}

/* ================================================================== */
/*  SurfaceFlinger mode map                                            */
/* ================================================================== */

__attribute__((cold))
void loadModesMap(void) {
    const char* map_path = DFPS_MODES_MAP_PATH;
    FILE* f = fopen(map_path, "r");
    if (!f) {
        LOGE("modes.map missing at %s; clearing cached mode map to defaults.",
             map_path);
    }

    ModeMapEntry temp_modes[MAX_MODES];
    int temp_mode_count = 0;
    int32_t temp_max_rate = 0, temp_min_rate = 0;
    char line[256];
    int line_num = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line_num++;
            char* hash = strchr(line, '#');
            if (hash) *hash = '\0';

            char* p = skip_leading_ws(line);
            if (*p == '\0') continue;

            char* end = NULL;
            long parsed_rate = strtol(p, &end, 10);
            if (end == p) {
                LOGW("modes.map line %d: ignored malformed line", line_num);
                continue;
            }

            p = end;
            while (isspace((unsigned char)*p)) p++;
            long parsed_id = strtol(p, &end, 10);
            if (end == p) {
                LOGW("modes.map line %d: ignored malformed line", line_num);
                continue;
            }

            int32_t rate = (int32_t)parsed_rate;
            int32_t id = (int32_t)parsed_id;
            if (rate <= 0 || rate > 1000) {
                LOGW("modes.map line %d: rejected absurd rate %d", line_num, rate);
                continue;
            }
            if (id < 0) {
                LOGW("modes.map line %d: rejected negative id %d", line_num, id);
                continue;
            }
            if (temp_mode_count < MAX_MODES) {
                temp_modes[temp_mode_count].rate = rate;
                temp_modes[temp_mode_count].id = id;
                if (rate > temp_max_rate) temp_max_rate = rate;
                if (temp_min_rate == 0 || rate < temp_min_rate) temp_min_rate = rate;
                temp_mode_count++;
            }
        }
        fclose(f);
    }

    /* Commit under write lock — consistent with loadConfig */
    pthread_rwlock_wrlock(&g_config_lock);
    g_mode_count = temp_mode_count;
    memset(g_modes, 0, sizeof(g_modes));
    if (temp_mode_count > 0) {
        memcpy(g_modes, temp_modes, sizeof(ModeMapEntry) * temp_mode_count);
    }
    atomic_store_explicit(&g_max_physical_rate, temp_max_rate, memory_order_release);
    atomic_store_explicit(&g_min_physical_rate, temp_min_rate, memory_order_release);
    invalidateRateModeCache();
    pthread_rwlock_unlock(&g_config_lock);

    LOGI("Loaded %d SurfaceFlinger mode mappings successfully.", g_mode_count);
    for (int i = 0; i < g_mode_count; i++) {
        LOGI("  - Mapping: %d Hz -> SF ID %d", g_modes[i].rate, g_modes[i].id);
    }
}

/* ================================================================== */
/*  Main configuration                                                 */
/* ================================================================== */

/* Rebuild the rule hash table. Caller must hold g_config_lock for writing,
 * because this mutates g_rule_hash while readers may be querying it. */
__attribute__((cold))
void rebuildRuleHash(void) {
    /* Clear table. */
    for (int i = 0; i < RULE_HASH_SLOTS; i++) {
        g_rule_hash[i].index = -1;
    }

    int n = g_rule_count;
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        const char* pkg = g_rules[i].pkg;
        uint32_t h = hash_string_fnv1a(pkg) & RULE_HASH_MASK;
        /* Open addressing with linear probing. MAX_RULES << RULE_HASH_SLOTS,
         * so collisions are rare and the loop always terminates. */
        for (int probe = 0; probe < RULE_HASH_SLOTS; probe++) {
            int slot = (int)((h + (uint32_t)probe) & RULE_HASH_MASK);
            if (g_rule_hash[slot].index < 0) {
                g_rule_hash[slot].index = i;
                break;
            }
        }
    }
}

__attribute__((cold))
/* Parse a base-10 int32 from a fully-trimmed string. Returns false unless the
 * ENTIRE string is a valid integer (no trailing garbage, no overflow). On
 * failure *out is left unchanged so the caller keeps its default value —
 * this prevents garbage like "abc" from silently becoming 0. */
static bool parse_int32(const char* s, int32_t* out) {
    if (s == NULL || *s == '\0') return false;
    char* endptr = NULL;
    errno = 0;
    long v = strtol(s, &endptr, 10);
    if (endptr == s || *endptr != '\0' || errno == ERANGE) return false;
    *out = (int32_t)v;
    return true;
}

void loadConfig(void) {
    const char* config_path = DFPS_CONFIG_PATH;
    FILE* f = fopen(config_path, "r");
    if (!f) {
        LOGE("Configuration file missing at %s; reverting to defaults.",
             config_path);
    }

    PerAppRule temp_rules[MAX_RULES];
    int temp_rule_count = 0;
    int32_t temp_offscreen_rate = -1;
    int32_t temp_default_idle = 60;
    int32_t temp_default_active = 120;
    int32_t temp_slack = 4000;
    bool temp_frame_rate_flex = false;
    bool temp_min_bright = false;
    int32_t temp_min_bright_threshold = 0;
    bool temp_debug = false;

    bool temp_battery_saver = false;
    int32_t temp_low_battery_threshold = 10;
    int32_t temp_power_save_max_rate = 60;

    char line[256];
    int line_num = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line_num++;
            char* hash = strchr(line, '#');
            if (hash) *hash = '\0';

            int len = (int)strlen(line);
            while (len > 0 && isspace((unsigned char)line[len-1])) {
                line[--len] = '\0';
            }
            if (len == 0) continue;

            char* eq = strchr(line, '=');
            if (!eq) continue;

            *eq = '\0';
            char* key = line;
            char* val = eq + 1;

            key = skip_leading_ws(key);
            if (*key == '\0') continue;
            char* key_end = key + strlen(key) - 1;
            while (key_end > key && isspace((unsigned char)*key_end)) *key_end-- = '\0';
            while (isspace((unsigned char)*val)) val++;
            /* Trim trailing whitespace so a value like "key = 45  " (with a
             * trailing space, as written by some editors) is accepted by the
             * strict parser below instead of being flagged as garbage. */
            {
                size_t vlen = strlen(val);
                while (vlen > 0 && isspace((unsigned char)val[vlen - 1]))
                    val[--vlen] = '\0';
            }

            if (strcasecmp(key, "DEBUG") == 0) {
                if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0)
                    temp_debug = true;
            } else if (strcasecmp(key, "touchSlackMs") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_slack = v;
                else LOGE("dfps.conf: invalid touchSlackMs '%s', using default %d", val, temp_slack);
            } else if (strcasecmp(key, "enableFrameRateFlex") == 0) {
                if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0)
                    temp_frame_rate_flex = true;
            } else if (strcasecmp(key, "enableMinBrightness") == 0) {
                if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0)
                    temp_min_bright = true;
            } else if (strcasecmp(key, "minBrightnessThreshold") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_min_bright_threshold = v;
                else LOGE("dfps.conf: invalid minBrightnessThreshold '%s', using default %d", val, temp_min_bright_threshold);
            } else if (strcasecmp(key, "defaultIdle") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_default_idle = v;
                else LOGE("dfps.conf: invalid defaultIdle '%s', using default %d", val, temp_default_idle);
            } else if (strcasecmp(key, "defaultActive") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_default_active = v;
                else LOGE("dfps.conf: invalid defaultActive '%s', using default %d", val, temp_default_active);
            } else if (strcasecmp(key, "offscreenRate") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_offscreen_rate = v;
                else LOGE("dfps.conf: invalid offscreenRate '%s', using default %d", val, temp_offscreen_rate);
            } else if (strcasecmp(key, "batterySaver") == 0) {
                if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0)
                    temp_battery_saver = true;
            } else if (strcasecmp(key, "lowBatteryThreshold") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_low_battery_threshold = v;
                else LOGE("dfps.conf: invalid lowBatteryThreshold '%s', using default %d", val, temp_low_battery_threshold);
            } else if (strcasecmp(key, "powerSaveMaxRate") == 0) {
                int32_t v; if (parse_int32(val, &v)) temp_power_save_max_rate = v;
                else LOGE("dfps.conf: invalid powerSaveMaxRate '%s', using default %d", val, temp_power_save_max_rate);
            } else {
                /* Per-app rule: "PackageName = idle active" */
                int idle = -1, active = -1;
                if (sscanf(val, "%d %d", &idle, &active) == 2) {
                    bool valid = true;
                    if (idle != -1 && (idle <= 0 || idle > 1000)) {
                        LOGW("dfps.conf line %d: rejected absurd idle rate %d for '%s'",
                             line_num, idle, key);
                        valid = false;
                    }
                    if (active != -1 && (active <= 0 || active > 1000)) {
                        LOGW("dfps.conf line %d: rejected absurd active rate %d for '%s'",
                             line_num, active, key);
                        valid = false;
                    }
                    if (valid && temp_rule_count < MAX_RULES) {
                        strlcpy(temp_rules[temp_rule_count].pkg, key,
                                sizeof(temp_rules[0].pkg));
                        temp_rules[temp_rule_count].idle = idle;
                        temp_rules[temp_rule_count].active = active;
                        temp_rule_count++;
                    }
                }
            }
        }
        fclose(f);
    }

    /* Validate accumulated values */
    if (temp_slack < 0 || temp_slack > 60000) {
        LOGW("dfps.conf: rejected absurd touchSlackMs %d, using 4000", temp_slack);
        temp_slack = 4000;
    }
    if (temp_default_idle <= 0 || temp_default_idle > 1000) {
        LOGW("dfps.conf: rejected absurd defaultIdle %d, using 60", temp_default_idle);
        temp_default_idle = 60;
    }
    if (temp_default_active <= 0 || temp_default_active > 1000) {
        LOGW("dfps.conf: rejected absurd defaultActive %d, using 120", temp_default_active);
        temp_default_active = 120;
    }
    if (temp_offscreen_rate != -1 &&
        (temp_offscreen_rate <= 0 || temp_offscreen_rate > 1000)) {
        LOGW("dfps.conf: rejected absurd offscreenRate %d, using -1", temp_offscreen_rate);
        temp_offscreen_rate = -1;
    }
    if (temp_power_save_max_rate <= 0 || temp_power_save_max_rate > 1000) {
        LOGW("dfps.conf: rejected absurd powerSaveMaxRate %d, using 60",
             temp_power_save_max_rate);
        temp_power_save_max_rate = 60;
    }
    if (temp_low_battery_threshold < 0 || temp_low_battery_threshold > 100) {
        LOGW("dfps.conf: rejected absurd lowBatteryThreshold %d, using 10",
             temp_low_battery_threshold);
        temp_low_battery_threshold = 10;
    }
    if (temp_min_bright_threshold < 0 || temp_min_bright_threshold > 100) {
        LOGW("dfps.conf: rejected absurd minBrightnessThreshold %d, using 0",
             temp_min_bright_threshold);
        temp_min_bright_threshold = 0;
    }

    if (!temp_battery_saver &&
        (temp_low_battery_threshold != 10 || temp_power_save_max_rate != 60)) {
        LOGW("dfps.conf: batterySaver=false, so lowBatteryThreshold and "
             "powerSaveMaxRate have no effect until batterySaver=true.");
    }

    atomic_store(&g_debug, temp_debug);
    bool old_frame_rate_flex = atomic_exchange_explicit(
        &g_enable_frame_rate_flex, temp_frame_rate_flex, memory_order_acq_rel);
    if (old_frame_rate_flex != temp_frame_rate_flex && g_hot_binders.surfaceFlinger) {
        setSurfaceFlingerFrameRateFlex(temp_frame_rate_flex);
    }

    /* Commit parsed config under write lock */
    pthread_rwlock_wrlock(&g_config_lock);
    g_rule_count = temp_rule_count;
    if (temp_rule_count > 0) {
        memcpy(g_rules, temp_rules, sizeof(PerAppRule) * temp_rule_count);
    }
    atomic_store_explicit(&g_offscreen_rate, temp_offscreen_rate, memory_order_release);
    atomic_store_explicit(&g_default_idle_rate, temp_default_idle, memory_order_release);
    atomic_store_explicit(&g_default_active_rate, temp_default_active, memory_order_release);
    rebuildRuleHash();
    pthread_rwlock_unlock(&g_config_lock);

    atomic_store_explicit(&g_touch_slack_ms, temp_slack, memory_order_release);
    bool old_min_bright = atomic_exchange_explicit(
        &g_enable_min_brightness, temp_min_bright, memory_order_acq_rel);
    if (old_min_bright && !temp_min_bright) {
        atomic_store_explicit(&g_min_brightness_clamp, false, memory_order_release);
    }
    atomic_store_explicit(&g_min_brightness_threshold, temp_min_bright_threshold, memory_order_release);
    bool old_battery_saver = atomic_exchange_explicit(
        &g_battery_saver, temp_battery_saver, memory_order_acq_rel);
    if (old_battery_saver && !temp_battery_saver) {
        atomic_store_explicit(&g_power_save_mode, false, memory_order_release);
        atomic_store_explicit(&g_low_battery_mode, false, memory_order_release);
    }
    atomic_store_explicit(&g_low_battery_threshold, temp_low_battery_threshold, memory_order_release);
    atomic_store_explicit(&g_power_save_max_rate, temp_power_save_max_rate, memory_order_release);

    LOGI("Parsed dfps.conf successfully. Rules loaded: %d rules", g_rule_count);
    LOGI("Params: slack=%d ms, frame_rate_flex=%d, min_bright=%d, "
         "min_bright_thresh=%d%%, debug=%d",
         temp_slack, temp_frame_rate_flex, temp_min_bright,
         temp_min_bright_threshold, temp_debug);
    LOGI("Fallback behavior: Idle: %d Hz | Active: %d Hz",
         temp_default_idle, temp_default_active);
    LOGI("Offscreen behavior: %d Hz", temp_offscreen_rate);
    LOGI("Battery Saver: %s, Threshold: %d%%, MaxRate: %d Hz",
         temp_battery_saver ? "ON" : "OFF",
         temp_low_battery_threshold, temp_power_save_max_rate);
}
