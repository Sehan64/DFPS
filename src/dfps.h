/*
 * dfps.h — Master header for Dynamic FPS Controller
 *
 * All shared types, macros, extern globals, and cross-module declarations.
 */

#ifndef DFPS_H
#define DFPS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <strings.h>
#include <limits.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

extern char** environ;

/* ------------------------------------------------------------------ */
/*  Android Binder opaque types and function pointer typedefs          */
/* ------------------------------------------------------------------ */

typedef struct AIBinder            AIBinder;
typedef struct AIBinder_Class      AIBinder_Class;
typedef struct AParcel             AParcel;
typedef struct AIBinder_DeathRecipient AIBinder_DeathRecipient;

typedef uint32_t  transaction_code_t;
typedef int32_t   binder_status_t;
typedef uint32_t  binder_flags_t;

#define STATUS_OK 0

typedef void*  (*AIBinder_Class_onCreate)(void*);
typedef void   (*AIBinder_Class_onDestroy)(void*);
typedef binder_status_t (*AIBinder_Class_onTransact)(AIBinder*, transaction_code_t,
                                                     const AParcel*, AParcel*);
typedef bool   (*AParcel_stringAllocator)(void* stringData, int32_t size, char** out);

typedef AIBinder*          (*getService_t)(const char*);
typedef AIBinder*          (*waitForService_t)(const char*);
typedef AIBinder_Class*    (*Class_define_t)(const char*,
                                             AIBinder_Class_onCreate,
                                             AIBinder_Class_onDestroy,
                                             AIBinder_Class_onTransact);
typedef AIBinder*          (*new_t)(const AIBinder_Class*, void*);
typedef bool               (*associateClass_t)(AIBinder*, const AIBinder_Class*);
typedef binder_status_t    (*prepareTransaction_t)(AIBinder*, AParcel**);
typedef binder_status_t    (*transact_t)(AIBinder*, transaction_code_t,
                                         AParcel**, AParcel**, binder_flags_t);
typedef binder_status_t    (*writeStrongBinder_t)(AParcel*, AIBinder*);
typedef binder_status_t    (*writeInt32_t)(AParcel*, int32_t);
typedef binder_status_t    (*writeInt64_t)(AParcel*, int64_t);
typedef binder_status_t    (*readInt32_t)(const AParcel*, int32_t*);
typedef binder_status_t    (*readFloat_t)(const AParcel*, float*);
typedef binder_status_t    (*readString_t)(const AParcel*, void*, AParcel_stringAllocator);
typedef void               (*deleteParcel_t)(AParcel*);
typedef void               (*setThreadPoolMaxThreadCount_t)(uint32_t);
typedef void               (*startThreadPool_t)(void);
typedef void               (*joinThreadPool_t)(void);

typedef void    (*AIBinder_DeathRecipient_onBinderDied)(void* cookie);
typedef AIBinder_DeathRecipient* (*AIBinder_DeathRecipient_new_t)(
                                    AIBinder_DeathRecipient_onBinderDied);
typedef void    (*AIBinder_linkToDeath_t)(AIBinder*, AIBinder_DeathRecipient*, void*);

typedef int    (*__android_log_print_t)(int, const char*, const char*, ...);

/* ------------------------------------------------------------------ */
/*  Enums                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    API_UNKNOWN       = 0,
    API_ROOT_TASK_INFO = 1,
    API_STACK_INFO    = 2
} FocusedTaskApi;

typedef enum {
    FD_TOUCH,
    FD_WAKEUP,
    FD_INOTIFY,
    FD_SERVER,
    FD_CLIENT,
    FD_UEVENT
} FdKind;

/* ------------------------------------------------------------------ */
/*  Data structures                                                    */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((aligned(64))) {
    AIBinder*  activityManager;
    AIBinder*  powerManager;
    AIBinder*  surfaceFlinger;
    AIBinder*  displayManager;
    AIBinder*  batteryPropertiesRegistrar;
} HotBinders;

typedef struct __attribute__((aligned(64))) {
    prepareTransaction_t  prepareTransaction;
    transact_t            transact;
    writeInt32_t          writeInt32;
    writeInt64_t          writeInt64;
    readInt32_t           readInt32;
    readFloat_t           readFloat;
    readString_t          readString;
    deleteParcel_t        deleteParcel;
    transaction_code_t    resolvedFocusedTaskCode;
    transaction_code_t    resolvedForegroundCode;
    FocusedTaskApi        resolvedApi;
    transaction_code_t    resolvedIsInteractiveCode;
    transaction_code_t    resolvedIsPowerSaveModeCode;
    transaction_code_t    resolvedGetBrightnessCode;
    transaction_code_t    resolvedOnDisplayEventCode;
    transaction_code_t    resolvedRegisterCallbackWithEventMaskCode;
    transaction_code_t    resolvedRegisterCallbackCode;
    transaction_code_t    resolvedRegisterBatteryListenerCode;
    transaction_code_t    resolvedBatteryChangedCode;
} HotOps;

typedef struct {
    AIBinder*              observer;
    transaction_code_t     resolvedProcessObserverCode;
    void*                  lib;
    getService_t           getService;
    waitForService_t       waitForService;
    Class_define_t         Class_define;
    new_t                  AIBinder_new;
    associateClass_t       associateClass;
    setThreadPoolMaxThreadCount_t setThreadPoolMaxThreadCount;
    startThreadPool_t      startThreadPool;
    joinThreadPool_t       joinThreadPool;
    writeStrongBinder_t    writeString;
    AIBinder_DeathRecipient_new_t DeathRecipient_new;
    AIBinder_linkToDeath_t linkToDeath;
    AIBinder_DeathRecipient* deathRecipient;
} ColdBinderContext;

typedef struct {
    char    pkg[128];
    int32_t idle;
    int32_t active;
} PerAppRule;

/* Simple open-addressed hash table for O(1) per-app rule lookups.
 * MAX_RULES is bounded (256), so this is small and cache-friendly. */
#define RULE_HASH_SLOTS 512
#define RULE_HASH_MASK  (RULE_HASH_SLOTS - 1)

typedef struct {
    int32_t index;  /* -1 means empty, otherwise index into g_rules */
} RuleHashSlot;

typedef struct {
    int32_t rate;
    int32_t id;
} ModeMapEntry;

typedef struct {
    char    buf[252];
    int32_t len;
} TaskName;

typedef struct {
    char    buf[124];
    int32_t len;
} PackagePrefix;

/* ------------------------------------------------------------------ */
/*  Constants / tunables                                               */
/* ------------------------------------------------------------------ */

#define MAX_RULES               256
#define MAX_MODES               16
#define MAX_TOUCH_DEVICES       4
#define MAX_CLIENTS             8
#define MAX_TASKS               8

#define ANDROID_LOG_INFO    4
#define ANDROID_LOG_WARN    5
#define ANDROID_LOG_ERROR   6

/* ------------------------------------------------------------------ */
/*  Logging                                                            */
/* ------------------------------------------------------------------ */

extern __android_log_print_t g_log_print;

void writeLog(int level, const char* fmt, ...);

#define LOGI(...) do { \
    if (atomic_load_explicit(&g_debug, memory_order_relaxed)) \
        writeLog(ANDROID_LOG_INFO, __VA_ARGS__); \
} while(0)

#define LOGW(...) do { \
    if (atomic_load_explicit(&g_debug, memory_order_relaxed)) \
        writeLog(ANDROID_LOG_WARN, __VA_ARGS__); \
} while(0)

#define LOGE(...) writeLog(ANDROID_LOG_ERROR, __VA_ARGS__)

#define LOG_HOT(...) ((void)0)

/* ------------------------------------------------------------------ */
/*  Global state (defined in main.c)                                   */
/* ------------------------------------------------------------------ */

extern HotBinders      g_hot_binders;
extern HotOps          g_hot_ops;
extern ColdBinderContext g_cold;

/* Config */
extern PerAppRule      g_rules[MAX_RULES];
extern int             g_rule_count;
extern RuleHashSlot    g_rule_hash[RULE_HASH_SLOTS];
extern pthread_rwlock_t g_config_lock;

extern ModeMapEntry    g_modes[MAX_MODES];
extern int             g_mode_count;
extern _Atomic int32_t  g_max_physical_rate;
extern _Atomic int32_t  g_min_physical_rate;

/* Runtime tuning */
extern _Atomic int32_t g_touch_slack_ms;
extern _Atomic bool    g_enable_frame_rate_flex;
extern _Atomic bool    g_enable_min_brightness;
extern _Atomic int32_t g_min_brightness_threshold;
extern _Atomic bool    g_debug;

extern _Atomic int32_t g_offscreen_rate;
extern _Atomic int32_t  g_default_idle_rate;
extern _Atomic int32_t  g_default_active_rate;

/* Current rate state */
extern _Atomic int32_t g_curr_idle_rate;
extern _Atomic int32_t g_curr_active_rate;
extern _Atomic int32_t g_last_set_rate;

/* Touch state */
extern _Atomic uint64_t g_last_touch_time;
extern _Atomic bool     g_screen_interactive;
extern _Atomic bool     g_touching;
extern bool             g_root_mode;
extern _Atomic bool     g_shutdown_requested;


/* Battery / power */
extern _Atomic bool     g_battery_saver;
extern _Atomic int32_t  g_low_battery_threshold;
extern _Atomic int32_t  g_power_save_max_rate;
extern _Atomic bool     g_power_save_mode;
extern _Atomic bool     g_low_battery_mode;
extern _Atomic int32_t  g_battery_level;

extern _Atomic bool     g_min_brightness_clamp;

/* Set once the IDisplayManager callback is successfully registered. While
 * true, interactive/brightness changes arrive via the callback, so the
 * periodic idle-timeout re-queries are skipped to save binder traffic. */
extern _Atomic bool     g_display_callback_active;

/* Touch device fds */
extern int  g_touch_fds[MAX_TOUCH_DEVICES];
extern int  g_touch_fd_count;

/* Client management */
extern pthread_spinlock_t g_client_lock;

/* File descriptors */
extern int  g_wakeup_fd;
extern int  g_inotify_fd;
extern int  g_inotify_wd;
extern char g_watch_dir[];

extern int  g_server_fd;
extern int  g_client_fds[MAX_CLIENTS];
extern int  g_client_count;

extern int  g_uevent_fd;

/* Cross-thread signaling */
extern atomic_flag g_wakeup_pending;
extern _Atomic bool g_query_task_pending;

/* Task / package tracking */
extern TaskName       g_child_task_names[MAX_TASKS];
extern int            g_child_task_count;

extern PackagePrefix  g_last_package_prefixes[MAX_TASKS];
extern int            g_last_package_count;

/* Embedded resolver binary */
extern const unsigned char resolver_jar[];

/* ------------------------------------------------------------------ */
/*  Inline helpers                                                     */
/* ------------------------------------------------------------------ */

static inline uint64_t getNowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void triggerPollerWakeup(void) {
    if (g_wakeup_fd >= 0) {
        if (!atomic_flag_test_and_set_explicit(&g_wakeup_pending,
                                                memory_order_acq_rel)) {
            uint64_t val = 1;
            ssize_t n = write(g_wakeup_fd, &val, sizeof(val));
            if (n != sizeof(val) && errno != EAGAIN) {
                /* Retry once — transient failure (e.g. signal interrupt) */
                n = write(g_wakeup_fd, &val, sizeof(val));
            }
            if (n != sizeof(val)) {
                /* Write truly failed — clear flag so next caller can retry */
                atomic_flag_clear_explicit(&g_wakeup_pending,
                                           memory_order_release);
            }
        }
    }
}

/* Binder dummy callbacks — shared across modules */
static inline void* dummyOnCreate(void* args) { return args; }
static inline void  dummyOnDestroy(void* args) { (void)args; }
static inline binder_status_t dummyOnTransact(AIBinder* b, transaction_code_t c,
                                               const AParcel* in, AParcel* out) {
    (void)b; (void)c; (void)in; (void)out;
    return STATUS_OK;
}

/* Fast 32-bit FNV-1a hash for short package names. */
static inline uint32_t hash_string_fnv1a(const char* s) {
    uint32_t h = 2166136261U;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619U;
    }
    return h;
}

/* Epoll fd tagging helpers — safe on both 32-bit and 64-bit.
 * FdKind uses 8 bits; fd occupies the low 24 bits (max 16M fds). */
static inline void* tag_fd(FdKind kind, int fd) {
    return (void*)(((uintptr_t)(unsigned)kind << 24) | ((unsigned int)fd & 0x00FFFFFFU));
}
static inline FdKind get_kind(void* ptr) {
    return (FdKind)((uintptr_t)ptr >> 24);
}
static inline int get_fd(void* ptr) {
    return (int)((uintptr_t)ptr & 0x00FFFFFF);
}

/* ------------------------------------------------------------------ */
/*  Cross-module function declarations                                 */
/* ------------------------------------------------------------------ */

/* utils.c */
void initLogging(void);
bool setupAbstractSocket(void);
char** buildResolverEnv(const char* jar_path);

/* config.c */
void loadConfig(void);
void loadModesMap(void);
void rebuildRuleHash(void);

/* rate.c */
void setSurfaceFlingerFrameRateFlex(bool enable);
void invalidateRateModeCache(void);
void setRefreshRate(int32_t rate);
void updateRateState(void);
void updateCurrentAppRates(const char* pkg);

static inline int getPollTimeout(uint64_t now) {
    bool interactive = atomic_load_explicit(&g_screen_interactive, memory_order_acquire);
    if (!interactive) return -1;

    bool touching = atomic_load_explicit(&g_touching, memory_order_relaxed);
    if (touching) return -1;

    uint64_t last_touch = atomic_load_explicit(&g_last_touch_time, memory_order_relaxed);
    int32_t slack = atomic_load_explicit(&g_touch_slack_ms, memory_order_relaxed);
    uint64_t expire = last_touch + (uint64_t)slack;

    if (now < expire) {
        return (int)(expire - now);
    }

    return -1;
}

/* binder.c */
binder_status_t displayCallbackOnTransact(AIBinder* binder, transaction_code_t code,
                                           const AParcel* in, AParcel* out);
binder_status_t observerOnTransact(AIBinder* binder, transaction_code_t code,
                                    const AParcel* in, AParcel* out);
binder_status_t batteryListenerOnTransact(AIBinder* binder, transaction_code_t code,
                                           const AParcel* in, AParcel* out);
void queryFocusedTask(void);
void resolveTransactionCodes(void);
void onBinderDied(void* cookie);

/* power.c */
int32_t readInitialBatteryLevel(void);
void    checkInteractiveAndPowerSave(bool probe_interactive);
void    checkMinBrightness(void);
void    evaluateBatteryState(int32_t level);
bool    handleUevent(void);

/* touch.c */
bool  consumeShutdownSignal(void);
void  findTouchscreens(void);
void* touchListenerThread(void* arg);

#endif /* DFPS_H */
