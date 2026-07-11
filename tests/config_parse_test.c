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

/* Exercises config.c parsing/validation edge cases and the per-app rule
 * hash lookup via updateCurrentAppRates(). */
int main(void) {
    const char* config_path = DFPS_CONFIG_PATH;

    char dir_buf[512];
    strlcpy(dir_buf, config_path, sizeof(dir_buf));
    char* slash = strrchr(dir_buf, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir_buf, 0700);
    }

    unlink(config_path);

    if (pthread_rwlock_init(&g_config_lock, NULL) != 0) {
        fprintf(stderr, "FAIL: pthread_rwlock_init\n");
        return 1;
    }

    /* 1. Missing file -> all defaults. */
    loadConfig();
    expect_int("default idle (missing file)",
               atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed), 60);
    expect_int("default active (missing file)",
               atomic_load_explicit(&g_default_active_rate, memory_order_relaxed), 120);
    expect_int("offscreen (missing file)",
               atomic_load_explicit(&g_offscreen_rate, memory_order_relaxed), -1);
    expect_int("touch slack (missing file)",
               atomic_load_explicit(&g_touch_slack_ms, memory_order_relaxed), 4000);
    expect_int("rule count (missing file)", g_rule_count, 0);

    /* 2. Comments, blanks, and a no-'=' line are ignored; a valid
     * rule still parses. */
    if (!write_text_file(config_path,
            "# this is a comment\n"
            "\n"
            "this line has no equals sign\n"
            "DEBUG = true\n"
            "com.example.app = 30 120\n")) {
        fprintf(stderr, "FAIL: could not write config (case 2)\n");
        return 1;
    }
    loadConfig();
    expect_true("debug parsed",
               atomic_load_explicit(&g_debug, memory_order_relaxed));
    expect_int("rule count (mixed)",
               g_rule_count, 1);
    expect_int("rule idle", g_rules[0].idle, 30);
    expect_int("rule active", g_rules[0].active, 120);
    expect_true("rule pkg matches",
               strcmp(g_rules[0].pkg, "com.example.app") == 0);

    /* 3. Absurd defaultIdle (>1000) is rejected and reset to 60. */
    if (!write_text_file(config_path, "defaultIdle = 5000\n")) {
        fprintf(stderr, "FAIL: could not write config (case 3)\n");
        return 1;
    }
    loadConfig();
    expect_int("defaultIdle absurd rejected",
               atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed), 60);

    /* 4. Negative defaultActive is rejected and reset to 120. */
    if (!write_text_file(config_path, "defaultActive = -5\n")) {
        fprintf(stderr, "FAIL: could not write config (case 4)\n");
        return 1;
    }
    loadConfig();
    expect_int("defaultActive negative rejected",
               atomic_load_explicit(&g_default_active_rate, memory_order_relaxed), 120);

    /* 5. A per-app rule with an absurd idle rate is dropped entirely. */
    if (!write_text_file(config_path, "com.example.bad = 5000 90\n")) {
        fprintf(stderr, "FAIL: could not write config (case 5)\n");
        return 1;
    }
    loadConfig();
    expect_int("absurd per-app rule dropped", g_rule_count, 0);

    /* 6. Whitespace tolerance around key/value. */
    if (!write_text_file(config_path,
            "   defaultIdle   =   45   \n"
            "defaultActive=90\n")) {
        fprintf(stderr, "FAIL: could not write config (case 6)\n");
        return 1;
    }
    loadConfig();
    expect_int("defaultIdle whitespace",
               atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed), 45);
    expect_int("defaultActive no-space",
               atomic_load_explicit(&g_default_active_rate, memory_order_relaxed), 90);

    /* 7. Idle -1 means "use default" for that rule. */
    if (!write_text_file(config_path, "com.example.def = -1 120\n")) {
        fprintf(stderr, "FAIL: could not write config (case 7)\n");
        return 1;
    }
    loadConfig();
    expect_int("wildcard-idle rule count", g_rule_count, 1);
    expect_int("wildcard idle stored as -1", g_rules[0].idle, -1);

    /* 8. batterySaver plumbing. */
    if (!write_text_file(config_path,
            "batterySaver = true\n"
            "lowBatteryThreshold = 21\n"
            "powerSaveMaxRate = 48\n")) {
        fprintf(stderr, "FAIL: could not write config (case 8)\n");
        return 1;
    }
    loadConfig();
    expect_true("batterySaver parsed",
               atomic_load_explicit(&g_battery_saver, memory_order_relaxed));
    expect_int("lowBatteryThreshold parsed",
               atomic_load_explicit(&g_low_battery_threshold, memory_order_relaxed), 21);
    expect_int("powerSaveMaxRate parsed",
               atomic_load_explicit(&g_power_save_max_rate, memory_order_relaxed), 48);

    /* 9. Rule hash lookup end-to-end via updateCurrentAppRates(). */
    if (!write_text_file(config_path,
            "com.example.match = 30 120\n")) {
        fprintf(stderr, "FAIL: could not write config (case 9)\n");
        return 1;
    }
    loadConfig();
    updateCurrentAppRates("com.example.match");
    expect_int("lookup idle",
               atomic_load_explicit(&g_curr_idle_rate, memory_order_relaxed), 30);
    expect_int("lookup active",
               atomic_load_explicit(&g_curr_active_rate, memory_order_relaxed), 120);
    updateCurrentAppRates("com.example.unknown");
    expect_int("unknown falls back to default idle",
               atomic_load_explicit(&g_curr_idle_rate, memory_order_relaxed),
               atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed));

    pthread_rwlock_destroy(&g_config_lock);

    if (failures != 0) {
        fprintf(stderr, "config_parse_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
