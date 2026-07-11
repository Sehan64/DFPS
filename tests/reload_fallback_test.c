#undef main

#include "../src/dfps.h"

#ifndef DFPS_CONFIG_PATH
#define DFPS_CONFIG_PATH "/data/local/tmp/dfps/dfps.conf"
#endif
#ifndef DFPS_MODES_MAP_PATH
#define DFPS_MODES_MAP_PATH "/data/local/tmp/dfps/modes.map"
#endif

static int failures = 0;

static void expect_true(const char* label, bool value) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void expect_int(const char* label, int64_t actual, int64_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s (got %lld, expected %lld)\n",
                label, (long long)actual, (long long)expected);
        failures++;
    }
}

static bool write_text_file(const char* path, const char* contents) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    if (fputs(contents, f) < 0) {
        fclose(f);
        return false;
    }
    if (fclose(f) != 0) return false;
    return true;
}

static void reset_state(void) {
    pthread_rwlock_wrlock(&g_config_lock);
    g_rule_count = 2;
    strlcpy(g_rules[0].pkg, "com.example.game", sizeof(g_rules[0].pkg));
    g_rules[0].idle = 30;
    g_rules[0].active = 120;
    strlcpy(g_rules[1].pkg, "com.example.app", sizeof(g_rules[1].pkg));
    g_rules[1].idle = 60;
    g_rules[1].active = 90;
    rebuildRuleHash();

    g_mode_count = 2;
    g_modes[0].rate = 30;
    g_modes[0].id = 1;
    g_modes[1].rate = 120;
    g_modes[1].id = 2;
    atomic_store_explicit(&g_max_physical_rate, 120, memory_order_relaxed);
    atomic_store_explicit(&g_min_physical_rate, 30, memory_order_relaxed);
    pthread_rwlock_unlock(&g_config_lock);

    atomic_store_explicit(&g_last_set_rate, 90, memory_order_relaxed);
    atomic_store_explicit(&g_enable_frame_rate_flex, true, memory_order_relaxed);
    atomic_store_explicit(&g_enable_min_brightness, true, memory_order_relaxed);
    atomic_store_explicit(&g_min_brightness_threshold, 25, memory_order_relaxed);
    atomic_store_explicit(&g_debug, true, memory_order_relaxed);
    atomic_store_explicit(&g_battery_saver, true, memory_order_relaxed);
    atomic_store_explicit(&g_low_battery_threshold, 18, memory_order_relaxed);
    atomic_store_explicit(&g_power_save_max_rate, 48, memory_order_relaxed);
    atomic_store_explicit(&g_power_save_mode, true, memory_order_relaxed);
    atomic_store_explicit(&g_low_battery_mode, true, memory_order_relaxed);
    atomic_store_explicit(&g_min_brightness_clamp, true, memory_order_relaxed);
    atomic_store_explicit(&g_offscreen_rate, 45, memory_order_relaxed);
    atomic_store_explicit(&g_default_idle_rate, 30, memory_order_relaxed);
    atomic_store_explicit(&g_default_active_rate, 120, memory_order_relaxed);
}

int main(void) {
    const char* config_path = DFPS_CONFIG_PATH;
    const char* modes_path = DFPS_MODES_MAP_PATH;

    char dir_buf[512];
    strlcpy(dir_buf, config_path, sizeof(dir_buf));
    char* slash = strrchr(dir_buf, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir_buf, 0700);
    }

    unlink(config_path);
    unlink(modes_path);

    if (pthread_rwlock_init(&g_config_lock, NULL) != 0) {
        fprintf(stderr, "FAIL: pthread_rwlock_init\n");
        return 1;
    }

    reset_state();

    /* No files present -> loadConfig/loadModesMap must fall back to defaults. */
    loadConfig();
    expect_int("rule count reset", g_rule_count, 0);
    expect_int("offscreen rate reset",
               atomic_load_explicit(&g_offscreen_rate, memory_order_relaxed), -1);
    expect_int("default idle reset",
               atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed), 60);
    expect_int("default active reset",
               atomic_load_explicit(&g_default_active_rate, memory_order_relaxed), 120);
    expect_int("touch slack reset",
               atomic_load_explicit(&g_touch_slack_ms, memory_order_relaxed), 4000);
    expect_true("frame-rate-flex disabled",
               !atomic_load_explicit(&g_enable_frame_rate_flex, memory_order_relaxed));
    expect_true("min-brightness disabled",
               !atomic_load_explicit(&g_enable_min_brightness, memory_order_relaxed));
    expect_int("min brightness threshold reset",
               atomic_load_explicit(&g_min_brightness_threshold, memory_order_relaxed), 0);
    expect_true("debug disabled",
               !atomic_load_explicit(&g_debug, memory_order_relaxed));
    expect_true("battery saver disabled",
               !atomic_load_explicit(&g_battery_saver, memory_order_relaxed));
    expect_true("power save mode cleared",
               !atomic_load_explicit(&g_power_save_mode, memory_order_relaxed));
    expect_true("low battery mode cleared",
               !atomic_load_explicit(&g_low_battery_mode, memory_order_relaxed));
    expect_int("low battery threshold reset",
               atomic_load_explicit(&g_low_battery_threshold, memory_order_relaxed), 10);
    expect_int("power save max rate reset",
               atomic_load_explicit(&g_power_save_max_rate, memory_order_relaxed), 60);

    reset_state();
    loadModesMap();
    expect_int("mode count reset", g_mode_count, 0);
    expect_int("max physical rate reset",
               atomic_load_explicit(&g_max_physical_rate, memory_order_relaxed), 0);
    expect_int("min physical rate reset",
               atomic_load_explicit(&g_min_physical_rate, memory_order_relaxed), 0);
    expect_int("rate cache reset",
               atomic_load_explicit(&g_last_set_rate, memory_order_relaxed), -1);

    const char* config_contents =
        "DEBUG = true\n"
        "touchSlackMs = 1500\n"
        "enableMinBrightness = true\n"
        "minBrightnessThreshold = 12\n"
        "batterySaver = true\n"
        "lowBatteryThreshold = 21\n"
        "powerSaveMaxRate = 48\n"
        "defaultIdle = 45\n"
        "defaultActive = 90\n"
        "offscreenRate = 30\n"
        "com.example.game = 60 120\n";

    if (!write_text_file(config_path, config_contents)) {
        fprintf(stderr, "FAIL: could not write test config\n");
        return 1;
    }
    unlink(modes_path);

    reset_state();
    loadConfig();
    loadModesMap();

    expect_true("debug enabled from config",
               atomic_load_explicit(&g_debug, memory_order_relaxed));
    expect_int("touch slack loaded",
               atomic_load_explicit(&g_touch_slack_ms, memory_order_relaxed), 1500);
    expect_true("min-brightness enabled from config",
               atomic_load_explicit(&g_enable_min_brightness, memory_order_relaxed));
    expect_int("min brightness threshold loaded",
               atomic_load_explicit(&g_min_brightness_threshold, memory_order_relaxed), 12);
    expect_true("battery saver enabled from config",
               atomic_load_explicit(&g_battery_saver, memory_order_relaxed));
    expect_int("low battery threshold loaded",
               atomic_load_explicit(&g_low_battery_threshold, memory_order_relaxed), 21);
    expect_int("power save max rate loaded",
               atomic_load_explicit(&g_power_save_max_rate, memory_order_relaxed), 48);
    expect_int("default idle loaded",
               atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed), 45);
    expect_int("default active loaded",
               atomic_load_explicit(&g_default_active_rate, memory_order_relaxed), 90);
    expect_int("offscreen rate loaded",
               atomic_load_explicit(&g_offscreen_rate, memory_order_relaxed), 30);
    expect_int("rule count loaded", g_rule_count, 1);
    expect_int("mode count still reset when modes.map missing", g_mode_count, 0);
    expect_int("max physical rate still reset when modes.map missing",
               atomic_load_explicit(&g_max_physical_rate, memory_order_relaxed), 0);
    expect_int("min physical rate still reset when modes.map missing",
               atomic_load_explicit(&g_min_physical_rate, memory_order_relaxed), 0);

    pthread_rwlock_destroy(&g_config_lock);

    if (failures != 0) {
        fprintf(stderr, "reload_fallback_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
