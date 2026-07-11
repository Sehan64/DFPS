/*
 * main.c — Entry point and global state definitions
 *
 * Contains main(), CPU affinity setup, and all global variable definitions.
 */

#include "dfps.h"

/* ================================================================== */
/*  Global variable definitions                                        */
/* ================================================================== */

/* Binder contexts */
HotBinders       g_hot_binders;
HotOps           g_hot_ops;
ColdBinderContext g_cold;

/* Configuration */
PerAppRule        g_rules[MAX_RULES];
int               g_rule_count = 0;
RuleHashSlot      g_rule_hash[RULE_HASH_SLOTS];
pthread_rwlock_t  g_config_lock;

ModeMapEntry      g_modes[MAX_MODES];
int               g_mode_count = 0;
_Atomic int32_t   g_max_physical_rate = 0;
_Atomic int32_t   g_min_physical_rate = 0;

/* Runtime tuning */
_Atomic int32_t   g_touch_slack_ms = 4000;
_Atomic bool      g_enable_frame_rate_flex = false;
_Atomic bool      g_enable_min_brightness = false;
_Atomic int32_t   g_min_brightness_threshold = 0;
_Atomic bool      g_debug = false;

_Atomic int32_t   g_offscreen_rate = -1;
_Atomic int32_t    g_default_idle_rate = 60;
_Atomic int32_t    g_default_active_rate = 120;

/* Current rate state */
_Atomic int32_t   g_curr_idle_rate = 60;
_Atomic int32_t   g_curr_active_rate = 120;
_Atomic int32_t   g_last_set_rate = -1;

/* Touch state */
_Atomic uint64_t  g_last_touch_time = 0;
_Atomic bool      g_screen_interactive = true;
_Atomic bool      g_touching = false;
bool              g_root_mode = false;
_Atomic bool      g_shutdown_requested = false;
static volatile sig_atomic_t s_shutdown_signal_seen = 0;


/* Battery / power */
_Atomic bool      g_battery_saver = false;
_Atomic int32_t   g_low_battery_threshold = 10;
_Atomic int32_t   g_power_save_max_rate = 60;
_Atomic bool      g_power_save_mode = false;
_Atomic bool      g_low_battery_mode = false;
_Atomic int32_t   g_battery_level = 100;

_Atomic bool      g_min_brightness_clamp = false;
_Atomic bool      g_display_callback_active = false;

uint64_t         g_start_time = 0;

/* Touch device fds */
int  g_touch_fds[MAX_TOUCH_DEVICES];
int  g_touch_fd_count = 0;

/* Client management */
pthread_spinlock_t g_client_lock;

/* File descriptors */
int  g_wakeup_fd = -1;
int  g_inotify_fd = -1;
int  g_inotify_wd = -1;
char g_watch_dir[PATH_MAX] = {0};

int  g_server_fd = -1;
int  g_client_fds[MAX_CLIENTS];
int  g_client_count = 0;

int  g_uevent_fd = -1;

/* Cross-thread signaling */
atomic_flag g_wakeup_pending = ATOMIC_FLAG_INIT;
_Atomic bool g_query_task_pending = false;

/* Task / package tracking */
TaskName       g_child_task_names[MAX_TASKS];
int            g_child_task_count = 0;

PackagePrefix  g_last_package_prefixes[MAX_TASKS];
int            g_last_package_count = 0;

/* Logging */
__android_log_print_t g_log_print = NULL;

/* ================================================================== */
/*  CPU affinity — pin to efficiency cores                              */
/* ================================================================== */

__attribute__((cold))
static unsigned long readCpuMaxFreq(int cpu_id) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu_id);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    unsigned long freq = 0;
    if (fscanf(f, "%lu", &freq) != 1) freq = 0;
    fclose(f);
    return freq;
}

__attribute__((cold))
static void setupCpuAffinity(void) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    long n_online = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_online <= 0) n_online = 1;
    if (n_online > CPU_SETSIZE) n_online = CPU_SETSIZE;

    /* Gather max frequencies only for online CPUs. */
    unsigned long min_freq = 0;
    unsigned long max_freq = 0;
    for (int i = 0; i < n_online; i++) {
        unsigned long freq = readCpuMaxFreq(i);
        if (freq == 0) {
            /* Missing cpufreq is common on some kernels/VMs. Treat as
             * unknown and fall back to CPU 0 at the end if no valid freq. */
            continue;
        }
        if (min_freq == 0 || freq < min_freq) min_freq = freq;
        if (freq > max_freq) max_freq = freq;
    }

    if (min_freq > 0 && max_freq > 0) {
        if (min_freq == max_freq) {
            /* Homogeneous SoC: cpufreq cannot distinguish clusters.
             * Pin to CPU 0 to avoid bouncing, which is cheaper than
             * unrestricted migration. */
            CPU_SET(0, &cpuset);
        } else {
            /* Heterogeneous SoC: pin to the efficiency cluster (lowest
             * max frequency). This keeps the daemon off the performance
             * cores and reduces scheduling jitter. */
            for (int i = 0; i < n_online; i++) {
                if (readCpuMaxFreq(i) == min_freq) {
                    CPU_SET(i, &cpuset);
                }
            }
        }
    }

    if (CPU_COUNT(&cpuset) == 0) {
        CPU_SET(0, &cpuset);
    }

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        LOGW("sched_setaffinity failed: %s — continuing without CPU pinning",
             strerror(errno));
    } else {
        LOGI("CPU affinity set to %d core(s)", CPU_COUNT(&cpuset));
    }
}

/* ================================================================== */
/*  Graceful shutdown signal handler                                   */
/* ================================================================== */

__attribute__((cold))
static void shutdownHandler(int sig) {
    if (s_shutdown_signal_seen) {
        _exit(128 + sig);
    }
    s_shutdown_signal_seen = 1;

    if (g_wakeup_fd >= 0) {
        uint64_t val = 1;
        (void)write(g_wakeup_fd, &val, sizeof(val));
    }
}

bool consumeShutdownSignal(void) {
    if (!s_shutdown_signal_seen) return false;
    atomic_store_explicit(&g_shutdown_requested, true, memory_order_release);
    return true;
}

/* ================================================================== */
/*  Fatal-signal handler (crash diagnostics)                           */
/* ================================================================== */

/* Best-effort backtrace on crash. execinfo.h is not universal (older NDK
 * targets, some libcs), so guard on its presence; when unavailable we still
 * log the signal number. The handler restricts itself to async-signal-safe
 * primitives (write/raise/signal); backtrace* is best-effort and may be
 * unavailable or partly unsafe, but a partial trace beats a silent death.
 * Because PR_SET_DUMPABLE is 0 there is no core dump, so this stderr trace
 * (or addresses, if the binary was stripped) is the only post-mortem we get
 * before the supervisor respawns us. */
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define DFP_HAVE_BACKTRACE 1
#  endif
#endif

static volatile sig_atomic_t s_fatal_seen = 0;

__attribute__((cold))
static void fatalSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    (void)info; (void)ucontext;
    /* Re-entrancy guard: if we fault again while handling, just die. */
    if (s_fatal_seen) _exit(128 + sig);
    s_fatal_seen = 1;

    static const char hdr[] = "\n[dfps] FATAL signal received — backtrace:\n";
    (void)write(STDERR_FILENO, hdr, sizeof(hdr) - 1);

#ifdef DFP_HAVE_BACKTRACE
    void* frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
#else
    static const char nobt[] = "(backtrace unavailable on this platform)\n";
    (void)write(STDERR_FILENO, nobt, sizeof(nobt) - 1);
#endif

    /* Restore the default disposition and re-raise so the supervisor sees
     * the real signal (and respawns a fresh instance). */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ================================================================== */
/*  Entry point                                                        */
/* ================================================================== */

#ifndef DFP_VERSION
#define DFP_VERSION "1.0.0"
#endif
static const char* kDfpVersion = DFP_VERSION;

/* Build stamp is source-derived (git describe) so the same commit always
 * yields the same binary. Override with -DDFP_BUILD_STAMP=... when building
 * outside a git tree. Empty means "no stamp" (handled below). */
#ifndef DFP_BUILD_STAMP
#define DFP_BUILD_STAMP ""
#endif
static const char* kDfpBuild = DFP_BUILD_STAMP;

int main(int argc, char** argv) {
    /* Version / help must work even when run unprivileged or before
     * any heavy setup, so handle it first. */
    if (argc > 1) {
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            if (kDfpBuild[0])
                printf("dfpsd %s (build %s)\n", kDfpVersion, kDfpBuild);
            else
                printf("dfpsd %s\n", kDfpVersion);
            return 0;
        }
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("dfpsd %s - Dynamic FPS Controller\n"
                   "usage: dfpsd [-v|--version] [-h|--help]\n"
                   "  -v, --version  print version and build stamp, then exit\n"
                   "  -h, --help     print this help, then exit\n",
                   kDfpVersion);
            return 0;
        }
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        /* Non-root and some kernels reject this; log but continue. */
        LOGW("mlockall(MCL_CURRENT | MCL_FUTURE) failed: %s — pages may be paged out under pressure.",
             strerror(errno));
    }
    prctl(PR_SET_TIMERSLACK, 0);
    #ifndef PR_SET_IO_FLUSHER
    #define PR_SET_IO_FLUSHER 33
    #endif
    prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);

    /* Hardening: the daemon runs as root (needed for /dev/input and the
     * netlink uevent socket). Prevent it from gaining further
     * privileges via a setuid helper, and stop it from leaking a
     * core image or being ptraced by an unprivileged process.
     * A full capability-set reduction is intentionally left to the init
     * / SELinux context (see docs/INIT.md), where the exact caps
     * required for input/uevent access are known. */
    #ifndef PR_SET_NO_NEW_PRIVS
    #define PR_SET_NO_NEW_PRIVS 38
    #endif
    #ifndef PR_SET_DUMPABLE
    #define PR_SET_DUMPABLE 4
    #endif
    #ifndef PR_SET_KEEP_CAPS
    #define PR_SET_KEEP_CAPS 7
    #endif
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_KEEP_CAPS, 1, 0, 0, 0);

    struct sched_param sp = { .sched_priority = 2 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
            LOGW("setpriority(PRIO_PROCESS, 0, -20) failed after SCHED_FIFO was denied: %s",
                 strerror(errno));
        }
    }

    setupCpuAffinity();

    if (pthread_spin_init(&g_client_lock, 0) != 0) {
        LOGE("Failed to initialize client spinlock: %s", strerror(errno));
        return 1;
    }
    if (pthread_rwlock_init(&g_config_lock, NULL) != 0) {
        LOGE("Failed to initialize config rwlock: %s", strerror(errno));
        pthread_spin_destroy(&g_client_lock);
        return 1;
    }

    memset(g_child_task_names, 0, sizeof(g_child_task_names));
    memset(g_last_package_prefixes, 0, sizeof(g_last_package_prefixes));
    for (int i = 0; i < RULE_HASH_SLOTS; i++) g_rule_hash[i].index = -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdownHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    /* Fatal-signal handlers: log a backtrace on a crash so the supervisor's
     * respawn is not blind. Installed after PR_SET_DUMPABLE(0) so no core is
     * produced; the stderr trace is the sole diagnostics. */
    struct sigaction fa;
    memset(&fa, 0, sizeof(fa));
    fa.sa_sigaction = fatalSignalHandler;
    fa.sa_flags = SA_SIGINFO;
    sigemptyset(&fa.sa_mask);
    sigaction(SIGSEGV, &fa, NULL);
    sigaction(SIGABRT, &fa, NULL);
    sigaction(SIGBUS,  &fa, NULL);
    sigaction(SIGILL,  &fa, NULL);
    sigaction(SIGFPE,  &fa, NULL);

    uid_t my_uid = getuid();
    g_root_mode = (my_uid == 0);

    initLogging();

    g_start_time = getNowMs();

    LOGI("==============================================");
    LOGI("      Dynamic FPS Controller Initiated        ");
    LOGI("==============================================");
    LOGI("UID: %d | Execution Profile: %s", my_uid,
         g_root_mode ? "ROOT" : "NON-ROOT");

    g_wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_wakeup_fd < 0) {
        LOGE("Failed to create eventfd synchronization descriptor.");
        return 1;
    }

    /* Load libbinder_ndk.so and resolve all function pointers */
    g_cold.lib = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_cold.lib)
        g_cold.lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_cold.lib) { LOGE("libbinder_ndk.so open error."); return 1; }

    g_cold.getService        = (getService_t)dlsym(g_cold.lib, "AServiceManager_getService");
    g_cold.waitForService    = (waitForService_t)dlsym(g_cold.lib, "AServiceManager_waitForService");
    g_cold.Class_define      = (Class_define_t)dlsym(g_cold.lib, "AIBinder_Class_define");
    g_cold.AIBinder_new      = (new_t)dlsym(g_cold.lib, "AIBinder_new");
    g_cold.associateClass    = (associateClass_t)dlsym(g_cold.lib, "AIBinder_associateClass");
    g_hot_ops.prepareTransaction = (prepareTransaction_t)dlsym(g_cold.lib, "AIBinder_prepareTransaction");
    g_hot_ops.transact       = (transact_t)dlsym(g_cold.lib, "AIBinder_transact");
    g_cold.writeString       = (writeStrongBinder_t)dlsym(g_cold.lib, "AParcel_writeStrongBinder");
    g_hot_ops.readInt32      = (readInt32_t)dlsym(g_cold.lib, "AParcel_readInt32");
    g_hot_ops.readFloat      = (readFloat_t)dlsym(g_cold.lib, "AParcel_readFloat");
    g_hot_ops.readString     = (readString_t)dlsym(g_cold.lib, "AParcel_readString");
    g_hot_ops.deleteParcel   = (deleteParcel_t)dlsym(g_cold.lib, "AParcel_delete");
    g_cold.setThreadPoolMaxThreadCount = (setThreadPoolMaxThreadCount_t)dlsym(g_cold.lib, "ABinderProcess_setThreadPoolMaxThreadCount");
    g_cold.startThreadPool   = (startThreadPool_t)dlsym(g_cold.lib, "ABinderProcess_startThreadPool");
    g_cold.joinThreadPool    = (joinThreadPool_t)dlsym(g_cold.lib, "ABinderProcess_joinThreadPool");
    g_hot_ops.writeInt32     = (writeInt32_t)dlsym(g_cold.lib, "AParcel_writeInt32");
    g_hot_ops.writeInt64     = (writeInt64_t)dlsym(g_cold.lib, "AParcel_writeInt64");
    g_cold.DeathRecipient_new = (AIBinder_DeathRecipient_new_t)dlsym(g_cold.lib, "AIBinder_DeathRecipient_new");
    g_cold.linkToDeath       = (AIBinder_linkToDeath_t)dlsym(g_cold.lib, "AIBinder_linkToDeath");

    if (!g_cold.getService || !g_cold.Class_define || !g_cold.AIBinder_new ||
        !g_cold.associateClass ||
        !g_hot_ops.prepareTransaction || !g_hot_ops.transact ||
        !g_hot_ops.readInt32 || !g_hot_ops.readFloat || !g_hot_ops.readString ||
        !g_hot_ops.deleteParcel || !g_cold.startThreadPool ||
        !g_cold.writeString || !g_hot_ops.writeInt32 || !g_hot_ops.writeInt64) {
        LOGE("dlsym resolution failure.");
        return 1;
    }

    if (g_cold.setThreadPoolMaxThreadCount) g_cold.setThreadPoolMaxThreadCount(0);
    g_cold.startThreadPool();
    if (g_cold.DeathRecipient_new)
        g_cold.deathRecipient = g_cold.DeathRecipient_new(onBinderDied);

    if (g_cold.lib) {
        /* Acquire system binder services */
        g_hot_binders.activityManager = g_cold.getService("activity");
        if (!g_hot_binders.activityManager && g_cold.waitForService)
            g_hot_binders.activityManager = g_cold.waitForService("activity");
        if (!g_hot_binders.activityManager) { LOGE("ActivityManager binder missing."); return 1; }
        if (g_cold.linkToDeath && g_cold.deathRecipient)
            g_cold.linkToDeath(g_hot_binders.activityManager, g_cold.deathRecipient, NULL);

        g_hot_binders.powerManager = g_cold.getService("power");
        if (!g_hot_binders.powerManager && g_cold.waitForService)
            g_hot_binders.powerManager = g_cold.waitForService("power");
        if (g_hot_binders.powerManager && g_cold.linkToDeath && g_cold.deathRecipient)
            g_cold.linkToDeath(g_hot_binders.powerManager, g_cold.deathRecipient, NULL);

        g_hot_binders.surfaceFlinger = g_cold.getService("SurfaceFlinger");
        if (!g_hot_binders.surfaceFlinger && g_cold.waitForService)
            g_hot_binders.surfaceFlinger = g_cold.waitForService("SurfaceFlinger");
        if (g_hot_binders.surfaceFlinger && g_cold.linkToDeath && g_cold.deathRecipient)
            g_cold.linkToDeath(g_hot_binders.surfaceFlinger, g_cold.deathRecipient, NULL);

        g_hot_binders.displayManager = g_cold.getService("display");
        if (!g_hot_binders.displayManager && g_cold.waitForService)
            g_hot_binders.displayManager = g_cold.waitForService("display");
        if (g_hot_binders.displayManager && g_cold.linkToDeath && g_cold.deathRecipient)
            g_cold.linkToDeath(g_hot_binders.displayManager, g_cold.deathRecipient, NULL);

        g_hot_binders.batteryPropertiesRegistrar = g_cold.getService("batteryproperties");
        if (!g_hot_binders.batteryPropertiesRegistrar && g_cold.waitForService)
            g_hot_binders.batteryPropertiesRegistrar = g_cold.waitForService("batteryproperties");
    }

    /* Associate dummy classes for client-side binder proxies */
    AIBinder_Class* amClazz = g_cold.Class_define("android.app.IActivityManager",
                                                    dummyOnCreate, dummyOnDestroy,
                                                    dummyOnTransact);
    if (amClazz) g_cold.associateClass(g_hot_binders.activityManager, amClazz);
    else { LOGE("Failed defining ActivityManager class."); return 1; }

    if (g_hot_binders.surfaceFlinger) {
        AIBinder_Class* sfClazz = g_cold.Class_define("android.ui.ISurfaceComposer",
                                                        dummyOnCreate, dummyOnDestroy,
                                                        dummyOnTransact);
        if (sfClazz) g_cold.associateClass(g_hot_binders.surfaceFlinger, sfClazz);
    }

    /* Resolve Binder transaction codes (cached or via app_process) */
    resolveTransactionCodes();

    if (g_cold.resolvedProcessObserverCode == 0 ||
        g_hot_ops.resolvedFocusedTaskCode == 0 ||
        g_hot_ops.resolvedForegroundCode == 0 ||
        g_hot_ops.resolvedIsInteractiveCode == 0 ||
        g_hot_ops.resolvedApi == API_UNKNOWN) {
        LOGE("Core ActivityManager transaction code mapping failed. Halting.");
        return 1;
    }

    loadConfig();
    loadModesMap();

    if (!setupAbstractSocket()) {
        return 1;
    }

    /* Netlink uevent listener for battery events (root only) */
    if (g_root_mode) {
        g_uevent_fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                             NETLINK_KOBJECT_UEVENT);
        if (g_uevent_fd >= 0) {
            struct sockaddr_nl addr;
            memset(&addr, 0, sizeof(addr));
            addr.nl_family = AF_NETLINK;
            addr.nl_pid    = getpid();
            addr.nl_groups = 1;
            if (bind(g_uevent_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                LOGE("Failed to bind netlink uevent socket");
                close(g_uevent_fd);
                g_uevent_fd = -1;
            } else {
                LOGI("Netlink uevent listener initialized for battery events");
            }
        }
    }

    /* Register battery listener via Binder (if available) */
    if (g_hot_binders.batteryPropertiesRegistrar &&
        g_hot_ops.resolvedRegisterBatteryListenerCode != 0 &&
        g_hot_ops.resolvedBatteryChangedCode != 0) {
        AIBinder_Class* battClazz = g_cold.Class_define(
            "android.os.IBatteryPropertiesRegistrar",
            dummyOnCreate, dummyOnDestroy, dummyOnTransact);
        if (battClazz) {
            g_cold.associateClass(g_hot_binders.batteryPropertiesRegistrar, battClazz);
            AIBinder_Class* battListenerClazz = g_cold.Class_define(
                "android.os.IBatteryPropertiesListener",
                dummyOnCreate, dummyOnDestroy, batteryListenerOnTransact);
            if (battListenerClazz) {
                AIBinder* battListener = g_cold.AIBinder_new(battListenerClazz, NULL);
                if (battListener) {
                    g_cold.associateClass(battListener, battListenerClazz);
                    AParcel* in = NULL;
                    if (g_hot_ops.prepareTransaction(
                            g_hot_binders.batteryPropertiesRegistrar, &in) == STATUS_OK && in) {
                        g_cold.writeString(in, battListener);
                        AParcel* reply = NULL;
                        binder_status_t s = g_hot_ops.transact(
                            g_hot_binders.batteryPropertiesRegistrar,
                            g_hot_ops.resolvedRegisterBatteryListenerCode,
                            &in, &reply, 0);
                        if (s != STATUS_OK) {
                            LOGE("Failed to register battery listener: %d", s);
                        } else {
                            LOGI("Battery listener registered successfully via Binder");
                        }
                        if (reply) g_hot_ops.deleteParcel(reply);
                    }
                }
            }
        }
    }

    /* Read initial battery state */
    if (g_uevent_fd >= 0 || g_hot_binders.batteryPropertiesRegistrar) {
        int32_t initial_level = readInitialBatteryLevel();
        evaluateBatteryState(initial_level);
        triggerPollerWakeup();
    }

    /* Spawn the touch / event-loop thread */
    pthread_t touch_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)256 * 1024);
    if (pthread_create(&touch_thread, &attr, touchListenerThread, NULL) != 0) {
        LOGE("Failed to create touch thread");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(touch_thread);

    /* Register display change callback */
    if (g_hot_binders.displayManager) {
        AIBinder_Class* displayClazz = g_cold.Class_define(
            "android.hardware.display.IDisplayManager",
            dummyOnCreate, dummyOnDestroy, dummyOnTransact);
        if (displayClazz) {
            g_cold.associateClass(g_hot_binders.displayManager, displayClazz);

            AIBinder_Class* displayCallbackClazz = g_cold.Class_define(
                "android.hardware.display.IDisplayManagerCallback",
                dummyOnCreate, dummyOnDestroy, displayCallbackOnTransact);
            if (displayCallbackClazz) {
                AIBinder* displayCallback = g_cold.AIBinder_new(displayCallbackClazz, NULL);
                if (displayCallback) {
                    g_cold.associateClass(displayCallback, displayCallbackClazz);

                    AParcel* in = NULL;
                    binder_status_t status = g_hot_ops.prepareTransaction(
                        g_hot_binders.displayManager, &in);
                    if (status == STATUS_OK && in) {
                        if (g_hot_ops.resolvedRegisterCallbackWithEventMaskCode != 0) {
                            g_cold.writeString(in, displayCallback);

                            int64_t event_mask = 2 | 4;
                            g_hot_ops.writeInt64(in, event_mask);
                        } else if (g_hot_ops.resolvedRegisterCallbackCode != 0) {
                            g_cold.writeString(in, displayCallback);
                        }

                        if (g_hot_ops.resolvedRegisterCallbackWithEventMaskCode != 0 ||
                            g_hot_ops.resolvedRegisterCallbackCode != 0) {
                            AParcel* reply = NULL;
                            transaction_code_t reg_code =
                                g_hot_ops.resolvedRegisterCallbackWithEventMaskCode != 0
                                ? g_hot_ops.resolvedRegisterCallbackWithEventMaskCode
                                : g_hot_ops.resolvedRegisterCallbackCode;
                            binder_status_t reg_status = g_hot_ops.transact(
                                g_hot_binders.displayManager, reg_code, &in, &reply, 0);
                            if (reg_status != STATUS_OK) {
                                LOGE("Failed to register display callback (code %u): status %d",
                                     reg_code, reg_status);
                            } else {
                                atomic_store_explicit(&g_display_callback_active, true,
                                                      memory_order_relaxed);
                            }
                            if (reply) g_hot_ops.deleteParcel(reply);
                        }
                    }
                }
            }
        }
    }

    checkInteractiveAndPowerSave(true);
    checkMinBrightness();

    /* Register process observer for foreground app tracking */
    AIBinder_Class* obsClazz = g_cold.Class_define(
        "android.app.IProcessObserver",
        dummyOnCreate, dummyOnDestroy, observerOnTransact);
    if (!obsClazz) { LOGE("Failed to define observer."); return 1; }
    g_cold.observer = g_cold.AIBinder_new(obsClazz, NULL);
    if (!g_cold.observer) { LOGE("Instantiation error."); return 1; }
    g_cold.associateClass(g_cold.observer, obsClazz);

    AParcel* in = NULL;
    binder_status_t status = g_hot_ops.prepareTransaction(g_hot_binders.activityManager, &in);
    if (status != STATUS_OK || !in) { LOGE("Observation preparation error."); return 1; }
    g_cold.writeString(in, g_cold.observer);

    AParcel* reply = NULL;
    status = g_hot_ops.transact(g_hot_binders.activityManager,
                                 g_cold.resolvedProcessObserverCode, &in, &reply, 0);
    if (status != STATUS_OK) { LOGE("Binder transaction failure."); return 1; }
    if (reply) g_hot_ops.deleteParcel(reply);

    queryFocusedTask();
    while (!atomic_load_explicit(&g_shutdown_requested, memory_order_acquire)) {
        usleep(250000);
    }
    return 0;
}
