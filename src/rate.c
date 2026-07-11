/*
 * rate.c — Refresh rate control logic
 *
 * Decides and applies the display refresh rate based on touch state,
 * per-app rules, battery constraints, and brightness clamping.
 */

#include "dfps.h"

/* ================================================================== */
/*  Internal helpers (file-local)                                      */
/* ================================================================== */

/* One-entry cache for the rate-to-SurfaceFlinger-ID mapping. */
static int32_t s_last_rate_in  = -1;
static int32_t s_last_id_out   = -1;

void invalidateRateModeCache(void) {
    s_last_rate_in = -1;
    s_last_id_out = -1;
    atomic_store_explicit(&g_last_set_rate, -1, memory_order_relaxed);
}

__attribute__((cold))
static int32_t resolveRootId(int32_t rate) {
    if (rate == s_last_rate_in) return s_last_id_out;
    s_last_rate_in = rate;

    if (g_mode_count == 0) {
        s_last_id_out = -1;
        return -1;
    }

    /* Exact match */
    for (int i = 0; i < g_mode_count; i++) {
        if (g_modes[i].rate == rate) {
            s_last_id_out = g_modes[i].id;
            return s_last_id_out;
        }
    }

    /* Closest match — cap at 30 Hz difference to avoid wildly wrong selections */
    int32_t closest_id = -1, closest_rate = 0, min_diff = 30;
    for (int i = 0; i < g_mode_count; i++) {
        int32_t diff = abs(g_modes[i].rate - rate);
        if (diff < min_diff) {
            min_diff = diff;
            closest_rate = g_modes[i].rate;
            closest_id = g_modes[i].id;
        }
    }

    if (closest_id != -1) {
        LOGW("No exact match for %d Hz in modes.map. Using closest: %d Hz (ID: %d)",
             rate, closest_rate, closest_id);
        s_last_id_out = closest_id;
        return s_last_id_out;
    }

    /* No reasonable mapping found — return error instead of conflating Hz with ID */
    s_last_id_out = -1;
    return s_last_id_out;
}

static bool setSfActiveConfigDirect(int32_t id) {
    if (!g_hot_binders.surfaceFlinger) {
        LOGE("SurfaceFlinger binder missing in hot context.");
        return false;
    }

    AParcel* in = NULL;
    if (g_hot_ops.prepareTransaction(g_hot_binders.surfaceFlinger, &in) == STATUS_OK && in) {
        g_hot_ops.writeInt32(in, id);
        AParcel* reply = NULL;
        binder_status_t status = g_hot_ops.transact(
            g_hot_binders.surfaceFlinger, 1035, &in, &reply, 0);
        if (status == STATUS_OK) {
            if (reply) g_hot_ops.deleteParcel(reply);
            return true;
        }
        if (reply) g_hot_ops.deleteParcel(reply);
        LOGE("Direct SurfaceFlinger binder transaction 1035 failed: status %d", status);
        return false;
    }
    return false;
}

/* ================================================================== */
/*  Public interface                                                   */
/* ================================================================== */

void setSurfaceFlingerFrameRateFlex(bool enable) {
    if (!g_hot_binders.surfaceFlinger) {
        LOGE("SurfaceFlinger binder missing in hot context.");
        return;
    }

    AParcel* in = NULL;
    if (g_hot_ops.prepareTransaction(g_hot_binders.surfaceFlinger, &in) != STATUS_OK || !in) {
        LOGE("Failed preparing SurfaceFlinger transaction 1036.");
        return;
    }

    g_hot_ops.writeInt32(in, enable ? 1 : 0);
    AParcel* reply = NULL;
    binder_status_t status = g_hot_ops.transact(
        g_hot_binders.surfaceFlinger, 1036, &in, &reply, 0);
    if (reply) g_hot_ops.deleteParcel(reply);

    if (status == STATUS_OK) {
        LOGI("SurfaceFlinger frame-rate flexibility %s via transaction 1036.",
             enable ? "enabled" : "disabled");
    } else {
        LOGW("SurfaceFlinger transaction 1036 failed: status %d. "
             "Continuing with direct mode changes only.", status);
    }
}

void setRefreshRate(int32_t rate) {
    if (rate <= 0) return;

    int32_t max_phys = atomic_load_explicit(&g_max_physical_rate, memory_order_relaxed);
    if (max_phys > 0 && rate > max_phys) {
        static _Atomic int32_t last_warned_rate = -1;
        if (rate != atomic_load_explicit(&last_warned_rate, memory_order_relaxed)) {
            LOGW("Requested rate %d Hz exceeds device maximum (%d Hz). "
                 "Clamping output to %d Hz.",
                  rate, max_phys, max_phys);
            atomic_store_explicit(&last_warned_rate, rate, memory_order_relaxed);
        }
        rate = max_phys;
    }

    if (atomic_load_explicit(&g_last_set_rate, memory_order_relaxed) == rate) return;

    LOG_HOT("Transitioning device physical refresh rate to: %d Hz", rate);

    int32_t id = resolveRootId(rate);
    if (id >= 0) {
        if (setSfActiveConfigDirect(id)) {
            atomic_store_explicit(&g_last_set_rate, rate, memory_order_relaxed);
        }
    } else {
        LOGE("Failed mapping %d Hz to an ID in modes.map!", rate);
    }
}

void updateRateState(void) {
    bool interactive = atomic_load_explicit(&g_screen_interactive, memory_order_acquire);
    int32_t target_rate = -1;

    /* Config scalars are atomics: read lock-free to avoid a futex syscall on
     * every rate evaluation in the hot path. */
    int32_t offscreen = atomic_load_explicit(&g_offscreen_rate, memory_order_relaxed);
    int32_t min_phys  = atomic_load_explicit(&g_min_physical_rate, memory_order_relaxed);
    int32_t def_idle  = atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed);

    if (!interactive) {
        if (offscreen > 0) {
            LOG_HOT("Device offscreen state evaluated.");
            target_rate = offscreen;
        }
    } else if (atomic_load_explicit(&g_min_brightness_clamp, memory_order_acquire)) {
        target_rate = (min_phys > 0) ? min_phys : def_idle;
    } else {
        uint64_t now = getNowMs();
        uint64_t last_touch = atomic_load_explicit(&g_last_touch_time, memory_order_relaxed);
        bool touching = atomic_load_explicit(&g_touching, memory_order_relaxed);
        int32_t slack = atomic_load_explicit(&g_touch_slack_ms, memory_order_relaxed);

        if (touching || (now - last_touch < (uint64_t)slack)) {
            int32_t active_rate = atomic_load_explicit(&g_curr_active_rate, memory_order_relaxed);
            if (atomic_load_explicit(&g_power_save_mode, memory_order_acquire) ||
                atomic_load_explicit(&g_low_battery_mode, memory_order_acquire)) {
                int32_t max_rate = atomic_load_explicit(&g_power_save_max_rate,
                                                         memory_order_relaxed);
                if (active_rate > max_rate) {
                    active_rate = max_rate;
                }
            }
            target_rate = active_rate;
        } else {
            target_rate = atomic_load_explicit(&g_curr_idle_rate, memory_order_relaxed);
        }
    }

    if (target_rate <= 0) return;
    if (atomic_load_explicit(&g_last_set_rate, memory_order_relaxed) == target_rate) return;

    setRefreshRate(target_rate);
}

void updateCurrentAppRates(const char* pkg) {
    int32_t new_idle, new_active;
    bool found = false;

    pthread_rwlock_rdlock(&g_config_lock);
    uint32_t h = hash_string_fnv1a(pkg) & RULE_HASH_MASK;
    for (int probe = 0; probe < RULE_HASH_SLOTS; probe++) {
        int slot = (h + (uint32_t)probe) & RULE_HASH_MASK;
        int idx = g_rule_hash[slot].index;
        if (idx < 0) break; /* Empty slot — package has no rule. */
        if (strcmp(g_rules[idx].pkg, pkg) == 0) {
            new_idle   = (g_rules[idx].idle == -1)   ? atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed)   : g_rules[idx].idle;
            new_active = (g_rules[idx].active == -1) ? atomic_load_explicit(&g_default_active_rate, memory_order_relaxed) : g_rules[idx].active;
            found = true;
            break;
        }
    }
    if (!found) {
        new_idle   = atomic_load_explicit(&g_default_idle_rate, memory_order_relaxed);
        new_active = atomic_load_explicit(&g_default_active_rate, memory_order_relaxed);
    }
    pthread_rwlock_unlock(&g_config_lock);

    int32_t cur_idle   = atomic_load_explicit(&g_curr_idle_rate, memory_order_relaxed);
    int32_t cur_active = atomic_load_explicit(&g_curr_active_rate, memory_order_relaxed);

    if (cur_idle == new_idle && cur_active == new_active) return;

    atomic_store_explicit(&g_curr_idle_rate, new_idle, memory_order_relaxed);
    atomic_store_explicit(&g_curr_active_rate, new_active, memory_order_relaxed);

    if (found) {
        LOGI("Focused package matches rule: '%s' -> idle: %d Hz, active: %d Hz",
             pkg, new_idle, new_active);
    } else {
        LOGI("Focused package defaults (*): '%s' -> idle: %d Hz, active: %d Hz",
             pkg, new_idle, new_active);
    }
    if (new_idle == new_active) {
        LOGW("Note: Idle and active rates are equal (%d Hz). "
             "Transitions will not occur within '%s'.", new_idle, pkg);
    }

    /* No wakeup here: both callers (emitChangedForegroundPackages via the
     * wakeup path, and handleInotifyEvents via the FD_INOTIFY branch) already
     * set needs_rate_update = true, so updateRateState() runs in the same
     * epoll iteration. An extra eventfd write would just cause a spurious
     * second wakeup. */
}
