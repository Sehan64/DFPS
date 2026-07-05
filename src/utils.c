/*
 * utils.c — Logging, timing, wakeup, and socket utilities
 */

#include "dfps.h"

/* ================================================================== */
/*  Logging                                                            */
/* ================================================================== */

__attribute__((cold))
void initLogging(void) {
    void* liblog = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
    if (!liblog) liblog = dlopen("/system/lib64/liblog.so", RTLD_NOW | RTLD_LOCAL);
    if (!liblog) liblog = dlopen("/system/lib/liblog.so", RTLD_NOW | RTLD_LOCAL);
    if (liblog) {
        g_log_print = (__android_log_print_t)dlsym(liblog, "__android_log_print");
    }
}

__attribute__((cold))
void writeLog(int level, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    fprintf(stderr, "[DFPS] %s\n", buf);
    fflush(stderr);

    if (g_log_print) {
        g_log_print(level, "DFPS_Daemon", "%s", buf);
    }
}

/* ================================================================== */
/*  Abstract socket server                                             */
/* ================================================================== */

__attribute__((cold))
void setupAbstractSocket(void) {
    g_server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_server_fd < 0) {
        LOGE("Failed to create abstract socket descriptor.");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strcpy(&addr.sun_path[1], "dfps");

    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen("dfps");
    if (bind(g_server_fd, (struct sockaddr*)&addr, len) < 0) {
        if (errno == EADDRINUSE) {
            LOGW("Abstract socket @dfps already in use — another DFPS instance may be running. "
                 "Client connections disabled for this session.");
        } else {
            LOGE("Failed binding abstract socket @dfps. Error: %s", strerror(errno));
        }
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    if (listen(g_server_fd, 5) < 0) {
        LOGE("Failed to set abstract socket to listen. Error: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    LOGI("Abstract socket server successfully listening on: @dfps");
}

/* ================================================================== */
/*  Resolver environment builder                                       */
/* ================================================================== */

__attribute__((cold))
char** buildResolverEnv(const char* jar_path) {
    /* Count valid environment entries first */
    int count = 0;
    for (char** e = environ; e != NULL && *e != NULL; ++e) {
        count++;
    }

    char** envp = (char**)malloc((count + 2) * sizeof(char*));
    if (!envp) return NULL;

    static char classpath_buf[1024];
    snprintf(classpath_buf, sizeof(classpath_buf), "CLASSPATH=%s", jar_path);
    envp[0] = classpath_buf;

    int idx = 1;
    for (char** e = environ; e != NULL && *e != NULL; ++e) {
        if (strncmp(*e, "CLASSPATH=", 10) == 0 ||
            strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0 ||
            strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
        envp[idx++] = *e;
    }
    envp[idx] = NULL;
    return envp;
}
