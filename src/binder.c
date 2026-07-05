/*
 * binder.c — Android Binder IPC, transaction code resolution, and callbacks
 *
 * Handles all communication with ActivityManager, PowerManager, DisplayManager,
 * and SurfaceFlinger.  Also manages foreground-app tracking and client notification.
 */

#include "dfps.h"

/* ================================================================== */
/*  Embedded resolver JAR (binary data)                                */
/* ================================================================== */

const unsigned char resolver_jar[] = {
#include "resolver_bytes.h"
};

/* ================================================================== */
/*  Binder callback stubs                                              */
/* ================================================================== */

binder_status_t displayCallbackOnTransact(AIBinder* binder, transaction_code_t code,
                                           const AParcel* in, AParcel* out) {
    (void)binder; (void)out;
    if (code == g_hot_ops.resolvedOnDisplayEventCode) {
        int32_t displayId = -1;
        int32_t event = -1;
        if (g_hot_ops.readInt32(in, &displayId) == STATUS_OK && displayId == 0) {
            if (g_hot_ops.readInt32(in, &event) == STATUS_OK) {
                /* event 2 -> EVENT_DISPLAY_CHANGED */
                /* event 4 -> EVENT_DISPLAY_BRIGHTNESS_CHANGED */
                if (event == 2) {
                    checkInteractiveAndPowerSave();
                } else if (event == 4) {
                    checkMinBrightness();
                } else {
                    checkInteractiveAndPowerSave();
                    checkMinBrightness();
                }
            } else {
                checkInteractiveAndPowerSave();
                checkMinBrightness();
            }
        }
    }
    return STATUS_OK;
}

__attribute__((hot))
binder_status_t observerOnTransact(AIBinder* binder, transaction_code_t code,
                                    const AParcel* in, AParcel* out) {
    (void)binder; (void)in; (void)out;
    if (code == g_hot_ops.resolvedForegroundCode) {
        if (!atomic_exchange_explicit(&g_query_task_pending, true,
                                       memory_order_acq_rel)) {
            triggerPollerWakeup();
        }
    }
    return STATUS_OK;
}

binder_status_t batteryListenerOnTransact(AIBinder* binder, transaction_code_t code,
                                           const AParcel* in, AParcel* out) {
    (void)binder; (void)out;
    if (code == g_hot_ops.resolvedBatteryChangedCode) {
        int32_t present = 0;
        if (g_hot_ops.readInt32(in, &present) != STATUS_OK) return STATUS_OK;
        if (present == 1) {
            int32_t scratch;
            for (int i = 0; i < 6; i++) {
                if (g_hot_ops.readInt32(in, &scratch) != STATUS_OK) return STATUS_OK;
            }
            int32_t level = -1;
            if (g_hot_ops.readInt32(in, &level) == STATUS_OK && level >= 0 && level <= 100) {
                evaluateBatteryState(level);
            }
        }
    }
    return STATUS_OK;
}

/* ================================================================== */
/*  Foreground task tracking                                           */
/* ================================================================== */

__attribute__((hot, always_inline))
static inline bool task_name_allocator(void* data, int32_t size, char** out) {
    TaskName* t = (TaskName*)data;
    if (__builtin_expect(size <= 0, 0)) {
        t->len = 0;
        return size == 0;
    }
    if (__builtin_expect(size > (int32_t)sizeof(t->buf), 0)) {
        t->len = 0;
        return false;
    }
    t->len = size - 1;
    *out = t->buf;
    return true;
}

__attribute__((hot, always_inline))
static inline void readTaskNames(readString_t readString, const AParcel* reply,
                                  int32_t count) {
    g_child_task_count = (count <= 0) ? 0 : (count > MAX_TASKS ? MAX_TASKS : count);
    for (int32_t i = 0; i < g_child_task_count; ++i) {
        if (__builtin_expect(readString(reply, &g_child_task_names[i],
                                         task_name_allocator) != STATUS_OK, 0)) {
            g_child_task_count = 0;
            return;
        }
    }
}

__attribute__((hot, always_inline))
static inline void parseRootTaskInfoReply(const AParcel* reply) {
    const readInt32_t  readInt32  = g_hot_ops.readInt32;
    const readString_t readString = g_hot_ops.readString;
    int32_t scratch = 0;

    if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
    if (scratch) {
        if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
        if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
        if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
        if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
    }

    int32_t child_task_count = 0;
    if (__builtin_expect(readInt32(reply, &child_task_count) != STATUS_OK, 0)) return;
    if (child_task_count > 0) {
        for (int32_t i = 0; i < child_task_count; ++i) {
            if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
        }
    }

    int32_t child_task_count_2 = 0;
    if (__builtin_expect(readInt32(reply, &child_task_count_2) != STATUS_OK, 0)) return;
    readTaskNames(readString, reply, child_task_count_2);
}

__attribute__((hot, always_inline))
static inline void parseStackInfoReply(const AParcel* reply) {
    const readInt32_t  readInt32  = g_hot_ops.readInt32;
    const readString_t readString = g_hot_ops.readString;
    int32_t scratch = 0;

    if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
    if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
    if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
    if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
    if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;

    int32_t task_count = 0;
    if (__builtin_expect(readInt32(reply, &task_count) != STATUS_OK, 0)) return;
    if (task_count > 0) {
        for (int32_t i = 0; i < task_count; ++i) {
            if (__builtin_expect(readInt32(reply, &scratch) != STATUS_OK, 0)) return;
        }
    }

    int32_t name_count = 0;
    if (__builtin_expect(readInt32(reply, &name_count) != STATUS_OK, 0)) return;
    readTaskNames(readString, reply, name_count);
}

__attribute__((hot, always_inline))
static inline void emitChangedForegroundPackages(void) {
    const int n = g_child_task_count;
    uint8_t slashes[MAX_TASKS];
    bool changed;

    pthread_spin_lock(&g_client_lock);

    changed = (n != g_last_package_count);

    for (int i = 0; i < n; ++i) {
        const TaskName* t = &g_child_task_names[i];
        const char* p = (const char*)memchr(t->buf, '/', t->len);
        uint8_t slash = p ? (uint8_t)(p - t->buf) : (uint8_t)t->len;
        if (slash > sizeof(g_last_package_prefixes[i].buf)) {
            slash = sizeof(g_last_package_prefixes[i].buf);
        }
        slashes[i] = slash;
    }

    if (!changed) {
        for (int i = 0; i < n; ++i) {
            const PackagePrefix* last = &g_last_package_prefixes[i];
            if (slashes[i] != (uint8_t)last->len ||
                (slashes[i] > 0 &&
                 memcmp(g_child_task_names[i].buf, last->buf, slashes[i]) != 0)) {
                changed = true;
                break;
            }
        }
    }

    if (__builtin_expect(!changed, 1)) {
        pthread_spin_unlock(&g_client_lock);
        return;
    }

    g_last_package_count = n;
    for (int i = 0; i < n; ++i) {
        uint8_t slash = slashes[i];
        memcpy(g_last_package_prefixes[i].buf, g_child_task_names[i].buf, slash);
        g_last_package_prefixes[i].len = (int32_t)slash;
    }

    int snap_fds[MAX_CLIENTS];
    int snap_count = g_client_count;
    memcpy(snap_fds, g_client_fds, sizeof(int) * snap_count);
    pthread_spin_unlock(&g_client_lock);

    if (n > 0) {
        char first_pkg[128];
        int slash_len = slashes[0];
        if (slash_len > 127) slash_len = 127;
        memcpy(first_pkg, g_child_task_names[0].buf, slash_len);
        first_pkg[slash_len] = '\0';

        updateCurrentAppRates(first_pkg);
    } else {
        updateCurrentAppRates("");
    }

    if (snap_count > 0) {
        char out_buf[1024];
        int offset = 0;

        for (int i = 0; i < n; ++i) {
            uint8_t slash = slashes[i];
            int space = (i > 0) ? 1 : 0;
            if (offset + space + slash + 1 > (int)sizeof(out_buf)) break;
            if (space) out_buf[offset++] = ' ';
            memcpy(out_buf + offset, g_child_task_names[i].buf, slash);
            offset += slash;
        }
        out_buf[offset++] = '\n';

        for (int i = 0; i < snap_count; i++) {
            send(snap_fds[i], out_buf, offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
    }
}

/* ================================================================== */
/*  Focused task query                                                */
/* ================================================================== */

__attribute__((hot, always_inline))
void queryFocusedTask(void) {
    AIBinder* am = g_hot_binders.activityManager;
    AParcel* in = NULL;
    if (__builtin_expect(g_hot_ops.prepareTransaction(am, &in) != STATUS_OK || !in, 0))
        return;

    AParcel* reply = NULL;
    binder_status_t status = g_hot_ops.transact(am, g_hot_ops.resolvedFocusedTaskCode,
                                                 &in, &reply, 0);
    if (__builtin_expect(status != STATUS_OK || !reply, 0)) {
        if (reply) g_hot_ops.deleteParcel(reply);
        return;
    }

    int32_t exception_code = 0;
    if (__builtin_expect(g_hot_ops.readInt32(reply, &exception_code) != STATUS_OK ||
                          exception_code != 0, 0)) {
        g_hot_ops.deleteParcel(reply);
        return;
    }

    int32_t present = 0;
    if (__builtin_expect(g_hot_ops.readInt32(reply, &present) != STATUS_OK || !present, 0)) {
        g_child_task_count = 0;
        g_hot_ops.deleteParcel(reply);
        emitChangedForegroundPackages();
        return;
    }

    g_child_task_count = 0;
    if (__builtin_expect(g_hot_ops.resolvedApi == API_ROOT_TASK_INFO, 1)) {
        parseRootTaskInfoReply(reply);
    } else {
        parseStackInfoReply(reply);
    }

    g_hot_ops.deleteParcel(reply);
    emitChangedForegroundPackages();
}

/* ================================================================== */
/*  Binder death handler                                               */
/* ================================================================== */

void onBinderDied(void* cookie) {
    (void)cookie;
    LOGE("Critical system binder service died! Exiting daemon for restart...");
    exit(0);
}

/* ================================================================== */
/*  Transaction code resolution                                        */
/* ================================================================== */

__attribute__((cold))
static int resolveAllFields(const char* jar_path,
                            transaction_code_t* observer,
                            transaction_code_t* rootTask,
                            transaction_code_t* stackTask,
                            transaction_code_t* fg,
                            transaction_code_t* is_interactive,
                            transaction_code_t* is_power_save,
                            transaction_code_t* get_brightness,
                            transaction_code_t* on_display_event,
                            transaction_code_t* register_cb_mask,
                            transaction_code_t* register_cb,
                            transaction_code_t* register_batt_listener,
                            transaction_code_t* batt_changed) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);

    char** envp = buildResolverEnv(jar_path);
    if (!envp) {
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    char* argv[] = {
        (char*)"app_process",
        (char*)"/system/bin",
        (char*)"android.app.CodeResolver",
        (char*)"android.app.IActivityManager$Stub",
            (char*)"TRANSACTION_registerProcessObserver",
        (char*)"android.app.IActivityManager$Stub",
            (char*)"TRANSACTION_getFocusedRootTaskInfo",
        (char*)"android.app.IActivityManager$Stub",
            (char*)"TRANSACTION_getFocusedStackInfo",
        (char*)"android.app.IProcessObserver$Stub",
            (char*)"TRANSACTION_onForegroundActivitiesChanged",
        (char*)"android.os.IPowerManager$Stub",
            (char*)"TRANSACTION_isInteractive",
        (char*)"android.os.IPowerManager$Stub",
            (char*)"TRANSACTION_isPowerSaveMode",
        (char*)"android.hardware.display.IDisplayManager$Stub",
            (char*)"TRANSACTION_getBrightness",
        (char*)"android.hardware.display.IDisplayManagerCallback$Stub",
            (char*)"TRANSACTION_onDisplayEvent",
        (char*)"android.hardware.display.IDisplayManager$Stub",
            (char*)"TRANSACTION_registerCallbackWithEventMask",
        (char*)"android.hardware.display.IDisplayManager$Stub",
            (char*)"TRANSACTION_registerCallback",
        (char*)"android.os.IBatteryPropertiesRegistrar$Stub",
            (char*)"TRANSACTION_registerListener",
        (char*)"android.os.IBatteryPropertiesListener$Stub",
            (char*)"TRANSACTION_batteryPropertiesChanged",
        NULL
    };

    pid_t pid = 0;
    int rc = posix_spawn(&pid, "/system/bin/app_process", &actions, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    free(envp);
    close(pipefd[1]);

    if (rc != 0) {
        close(pipefd[0]);
        LOGE("Failed to spawn app_process, posix_spawn returned: %d", rc);
        return -1;
    }

    char buffer[8192] = {0};
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(buffer) - 1) {
        n = read(pipefd[0], buffer + total, sizeof(buffer) - 1 - total);
        if (n > 0) {
            total += n;
        } else if (n < 0 && errno == EINTR) {
            continue;   /* signal interrupted — retry */
        } else {
            break;      /* EOF or real error */
        }
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (total == 0) {
        LOGE("Resolver returned empty output.");
        return -1;
    }

    char* saveptr = NULL;
    char* line = strtok_r(buffer, "\n", &saveptr);
    while (line != NULL) {
        char* space = strchr(line, ' ');
        if (space) {
            char* eq = strchr(space + 1, '=');
            if (eq) {
                *eq = '\0';
                char* field = space + 1;
                long val = strtol(eq + 1, NULL, 10);

                if (strcmp(field, "TRANSACTION_registerProcessObserver") == 0)
                    *observer = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_getFocusedRootTaskInfo") == 0)
                    *rootTask = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_getFocusedStackInfo") == 0)
                    *stackTask = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_onForegroundActivitiesChanged") == 0)
                    *fg = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_isInteractive") == 0)
                    *is_interactive = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_isPowerSaveMode") == 0)
                    *is_power_save = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_getBrightness") == 0)
                    *get_brightness = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_onDisplayEvent") == 0)
                    *on_display_event = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_registerCallbackWithEventMask") == 0)
                    *register_cb_mask = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_registerCallback") == 0)
                    *register_cb = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_registerListener") == 0)
                    *register_batt_listener = (transaction_code_t)val;
                else if (strcmp(field, "TRANSACTION_batteryPropertiesChanged") == 0)
                    *batt_changed = (transaction_code_t)val;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return 0;
}

__attribute__((cold))
void resolveTransactionCodes(void) {
    const char* cache_path = "/data/local/tmp/dfps/tx_code.txt";

    char current_fp[256] = {0};
    __system_property_get("ro.build.fingerprint", current_fp);
    if (current_fp[0] == '\0') {
        strcpy(current_fp, "unknown");
    }

    /* Try loading from cache first */
    FILE* cache_f = NULL;
    int cache_fd = open(cache_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (cache_fd >= 0) {
        cache_f = fdopen(cache_fd, "r");
        if (!cache_f) close(cache_fd);
    }
    if (cache_f) {
        char line[256];
        int version = 0;
        char cached_fp[256] = {0};
        transaction_code_t observer = 0, rootTask = 0, stackTask = 0, fg = 0;
        transaction_code_t is_interactive = 0, is_power_save = 0, get_brightness = 0;
        transaction_code_t on_display_event = 0, register_cb_mask = 0, register_cb = 0;
        transaction_code_t register_batt_listener = 0, batt_changed = 0;

        while (fgets(line, sizeof(line), cache_f)) {
            char key[128];
            char val_str[256];
            if (sscanf(line, "%127[^=]=%255s", key, val_str) == 2) {
                if (strcmp(key, "v") == 0)
                    version = (int)strtol(val_str, NULL, 10);
                else if (strcmp(key, "fingerprint") == 0)
                    strncpy(cached_fp, val_str, sizeof(cached_fp) - 1);
                else {
                    int val = (int)strtol(val_str, NULL, 10);
                    if (strcmp(key, "TRANSACTION_registerProcessObserver") == 0) observer = val;
                    else if (strcmp(key, "TRANSACTION_getFocusedRootTaskInfo") == 0) rootTask = val;
                    else if (strcmp(key, "TRANSACTION_getFocusedStackInfo") == 0) stackTask = val;
                    else if (strcmp(key, "TRANSACTION_onForegroundActivitiesChanged") == 0) fg = val;
                    else if (strcmp(key, "TRANSACTION_isInteractive") == 0) is_interactive = val;
                    else if (strcmp(key, "TRANSACTION_isPowerSaveMode") == 0) is_power_save = val;
                    else if (strcmp(key, "TRANSACTION_getBrightness") == 0) get_brightness = val;
                    else if (strcmp(key, "TRANSACTION_onDisplayEvent") == 0) on_display_event = val;
                    else if (strcmp(key, "TRANSACTION_registerCallbackWithEventMask") == 0) register_cb_mask = val;
                    else if (strcmp(key, "TRANSACTION_registerCallback") == 0) register_cb = val;
                    else if (strcmp(key, "TRANSACTION_registerListener") == 0) register_batt_listener = val;
                    else if (strcmp(key, "TRANSACTION_batteryPropertiesChanged") == 0) batt_changed = val;
                }
            }
        }
        fclose(cache_f);

        if (version == 1 && strcmp(cached_fp, current_fp) == 0 &&
            observer && (rootTask || stackTask) && fg && is_interactive &&
            (register_cb_mask || register_cb)) {
            g_cold.resolvedProcessObserverCode = observer;
            if (rootTask) {
                g_hot_ops.resolvedFocusedTaskCode = rootTask;
                g_hot_ops.resolvedApi = API_ROOT_TASK_INFO;
            } else {
                g_hot_ops.resolvedFocusedTaskCode = stackTask;
                g_hot_ops.resolvedApi = API_STACK_INFO;
            }
            g_hot_ops.resolvedForegroundCode = fg;
            g_hot_ops.resolvedIsInteractiveCode = is_interactive;
            g_hot_ops.resolvedIsPowerSaveModeCode = is_power_save;
            g_hot_ops.resolvedGetBrightnessCode = get_brightness;
            g_hot_ops.resolvedOnDisplayEventCode = on_display_event;
            g_hot_ops.resolvedRegisterCallbackWithEventMaskCode = register_cb_mask;
            g_hot_ops.resolvedRegisterCallbackCode = register_cb;
            g_hot_ops.resolvedRegisterBatteryListenerCode = register_batt_listener;
            g_hot_ops.resolvedBatteryChangedCode = batt_changed;
            return;
        }
        LOGW("Transaction code cache invalid or outdated. Regenerating...");
    }

    /* Regenerate via app_process resolver */
    LOGI("Generating system transaction resolver mappings...");
    /* Use compile-time constant from generated header for reliable size check */
    extern const unsigned char _resolver_jar_size;
    #ifdef RESOLVER_JAR_SIZE
        static const size_t RESOLVER_SIZE = RESOLVER_JAR_SIZE;
    #else
        static const size_t RESOLVER_SIZE = sizeof(resolver_jar);
    #endif

    if (RESOLVER_SIZE < 100) {
        LOGE("Error: resolver JAR size is too small (%zu bytes). Check resolver_bytes.h.", RESOLVER_SIZE);
        return;
    }

    char jar_path_buf[256];
    snprintf(jar_path_buf, sizeof(jar_path_buf),
             "/data/local/tmp/dfps/resolver.%ld.jar", (long)getpid());
    const char* jar_path = jar_path_buf;
    /* Use 0600: only this UID can read/execute the helper. The directory is
     * still world-writable on many devices, but this removes the world-read bit
     * from the file itself. */
    int fd = open(jar_path,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) { LOGE("Failed to write helper to %s", jar_path); return; }
    ssize_t written = write(fd, resolver_jar, RESOLVER_SIZE);
    close(fd);
    if (written != (ssize_t)RESOLVER_SIZE) {
        LOGE("Failed writing resolver JAR to %s (wrote %zd of %zu bytes)",
             jar_path, written, RESOLVER_SIZE);
        unlink(jar_path);
        return;
    }

    transaction_code_t observer = 0, rootTask = 0, stackTask = 0, fg = 0;
    transaction_code_t is_interactive = 0, is_power_save = 0, get_brightness = 0;
    transaction_code_t on_display_event = 0, register_cb_mask = 0, register_cb = 0;
    transaction_code_t register_batt_listener = 0, batt_changed = 0;

    if (resolveAllFields(jar_path, &observer, &rootTask, &stackTask, &fg,
                          &is_interactive, &is_power_save, &get_brightness,
                          &on_display_event, &register_cb_mask, &register_cb,
                          &register_batt_listener, &batt_changed) == 0) {
        g_cold.resolvedProcessObserverCode = observer;
        if (rootTask) {
            g_hot_ops.resolvedFocusedTaskCode = rootTask;
            g_hot_ops.resolvedApi = API_ROOT_TASK_INFO;
        } else if (stackTask) {
            g_hot_ops.resolvedFocusedTaskCode = stackTask;
            g_hot_ops.resolvedApi = API_STACK_INFO;
        }
        g_hot_ops.resolvedForegroundCode = fg;
        g_hot_ops.resolvedIsInteractiveCode = is_interactive;
        g_hot_ops.resolvedIsPowerSaveModeCode = is_power_save;
        g_hot_ops.resolvedGetBrightnessCode = get_brightness;
        g_hot_ops.resolvedOnDisplayEventCode = on_display_event;
        g_hot_ops.resolvedRegisterCallbackWithEventMaskCode = register_cb_mask;
        g_hot_ops.resolvedRegisterCallbackCode = register_cb;
        g_hot_ops.resolvedRegisterBatteryListenerCode = register_batt_listener;
        g_hot_ops.resolvedBatteryChangedCode = batt_changed;

        /* Write cache for next startup atomically (temp file + rename). */
        char tmp_cache_path[256];
        snprintf(tmp_cache_path, sizeof(tmp_cache_path),
                 "%s.%ld.tmp", cache_path, (long)getpid());
        int out_cache_fd = open(tmp_cache_path,
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                0600);
        FILE* out_cache_f = out_cache_fd >= 0 ? fdopen(out_cache_fd, "w") : NULL;
        if (out_cache_f) {
            int ok = 1;
            ok = ok && fprintf(out_cache_f, "v=1\n") > 0;
            ok = ok && fprintf(out_cache_f, "fingerprint=%s\n", current_fp) > 0;
            if (observer) ok = ok && fprintf(out_cache_f, "TRANSACTION_registerProcessObserver=%u\n", observer) > 0;
            if (rootTask) ok = ok && fprintf(out_cache_f, "TRANSACTION_getFocusedRootTaskInfo=%u\n", rootTask) > 0;
            if (stackTask) ok = ok && fprintf(out_cache_f, "TRANSACTION_getFocusedStackInfo=%u\n", stackTask) > 0;
            if (fg) ok = ok && fprintf(out_cache_f, "TRANSACTION_onForegroundActivitiesChanged=%u\n", fg) > 0;
            if (is_interactive) ok = ok && fprintf(out_cache_f, "TRANSACTION_isInteractive=%u\n", is_interactive) > 0;
            if (is_power_save) ok = ok && fprintf(out_cache_f, "TRANSACTION_isPowerSaveMode=%u\n", is_power_save) > 0;
            if (get_brightness) ok = ok && fprintf(out_cache_f, "TRANSACTION_getBrightness=%u\n", get_brightness) > 0;
            if (on_display_event) ok = ok && fprintf(out_cache_f, "TRANSACTION_onDisplayEvent=%u\n", on_display_event) > 0;
            if (register_cb_mask) ok = ok && fprintf(out_cache_f, "TRANSACTION_registerCallbackWithEventMask=%u\n", register_cb_mask) > 0;
            if (register_cb) ok = ok && fprintf(out_cache_f, "TRANSACTION_registerCallback=%u\n", register_cb) > 0;
            if (register_batt_listener) ok = ok && fprintf(out_cache_f, "TRANSACTION_registerListener=%u\n", register_batt_listener) > 0;
            if (batt_changed) ok = ok && fprintf(out_cache_f, "TRANSACTION_batteryPropertiesChanged=%u\n", batt_changed) > 0;
            if (fflush(out_cache_f) != 0) ok = 0;
            if (ok && fsync(fileno(out_cache_f)) != 0) ok = 0;
            if (fclose(out_cache_f) != 0) ok = 0;
            if (ok) {
                if (rename(tmp_cache_path, cache_path) != 0) {
                    LOGW("Transaction code cache rename failed: %s", strerror(errno));
                    unlink(tmp_cache_path);
                } else {
                    int dir_fd = open("/data/local/tmp/dfps",
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (dir_fd >= 0) {
                        (void)fsync(dir_fd);
                        close(dir_fd);
                    }
                }
            } else {
                LOGW("Transaction code cache write failed (disk full or I/O error). "
                     "Codes will be re-resolved on next startup.");
                unlink(tmp_cache_path);
            }
        } else if (out_cache_fd >= 0) {
            close(out_cache_fd);
        }
    } else {
        LOGE("Failed to resolve transaction codes via app_process.");
    }
    unlink(jar_path);
}
