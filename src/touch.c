/*
 * touch.c — Touch input detection, inotify config reload, and main event loop
 *
 * Manages the epoll-based event loop that drives the entire daemon:
 * touch events, config hot-reload, client socket I/O, battery uevent,
 * and periodic state evaluation.
 */

#include "dfps.h"

static bool s_touch_tracking_fallback[MAX_TOUCH_DEVICES];

/* ================================================================== */
/*  Touch device discovery                                             */
/* ================================================================== */

void findTouchscreens(void) {
    LOGI("Scanning /dev/input nodes for touchscreen inputs...");
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        LOGE("Could not read /dev/input directory!");
        return;
    }

    struct dirent* entry;
    g_touch_fd_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) == 0) {
            char path[128];
            snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                unsigned long ev_bits[EV_MAX / (sizeof(unsigned long) * 8) + 1] = {0};
                unsigned long abs_bits[ABS_MAX / (sizeof(unsigned long) * 8) + 1] = {0};
                unsigned long key_bits[KEY_MAX / (sizeof(unsigned long) * 8) + 1] = {0};

                ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
                bool has_abs = (ev_bits[EV_ABS / (sizeof(unsigned long) * 8)] &
                    (1UL << (EV_ABS % (sizeof(unsigned long) * 8))));
                bool has_key = (ev_bits[EV_KEY / (sizeof(unsigned long) * 8)] &
                    (1UL << (EV_KEY % (sizeof(unsigned long) * 8))));

                bool is_touch = false;
                bool has_btn_touch = false;
                if (has_abs) {
                    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
                    bool has_mt_x = (abs_bits[ABS_MT_POSITION_X / (sizeof(unsigned long) * 8)] &
                        (1UL << (ABS_MT_POSITION_X % (sizeof(unsigned long) * 8))));
                    if (has_mt_x) is_touch = true;
                }
                if (!is_touch && has_key) {
                    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
                    has_btn_touch = (key_bits[BTN_TOUCH / (sizeof(unsigned long) * 8)] &
                        (1UL << (BTN_TOUCH % (sizeof(unsigned long) * 8))));
                    if (has_btn_touch) is_touch = true;
                } else if (has_key) {
                    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
                    has_btn_touch = (key_bits[BTN_TOUCH / (sizeof(unsigned long) * 8)] &
                        (1UL << (BTN_TOUCH % (sizeof(unsigned long) * 8))));
                }

                if (is_touch && g_touch_fd_count < MAX_TOUCH_DEVICES) {
                    s_touch_tracking_fallback[g_touch_fd_count] = !has_btn_touch;
                    g_touch_fds[g_touch_fd_count++] = fd;
                    LOGI("  - Registered input device: %s", path);
                } else {
                    close(fd);
                }
            } else {
                LOGW("  - Node %s could not be opened (Permission Denied).", path);
            }
        }
    }
    closedir(dir);
}

/* ================================================================== */
/*  Inotify — live config reload                                       */
/* ================================================================== */

__attribute__((cold))
static void setupInotify(void) {
    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd < 0) {
        LOGE("Failed to initialize inotify.");
        return;
    }

    strcpy(g_watch_dir, "/data/local/tmp/dfps");
    g_inotify_wd = inotify_add_watch(g_inotify_fd, g_watch_dir,
                                      IN_CLOSE_WRITE | IN_MOVED_TO);
    if (g_inotify_wd >= 0) {
        LOGI("Monitoring configuration directory for live changes: %s", g_watch_dir);
    } else {
        LOGE("Failed adding watch path to inotify: %s", g_watch_dir);
    }
}

__attribute__((cold))
static void handleInotifyEvents(void) {
    char buffer[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(g_inotify_fd, buffer, sizeof(buffer));
    if (len <= 0) return;

    char* ptr = buffer;
    while (ptr < buffer + len) {
        struct inotify_event* event = (struct inotify_event*)ptr;
        if (event->len > 0 && strcmp(event->name, "dfps.conf") == 0) {
            LOGI("Configuration modification event triggered.");
            loadConfig();

            /* Re-evaluate rates for the current foreground package */
            char snap[128] = {0};
            int last_count;
            pthread_spin_lock(&g_client_lock);
            last_count = g_last_package_count;
            if (last_count > 0) {
                int l = g_last_package_prefixes[0].len;
                if (l > 127) l = 127;
                memcpy(snap, g_last_package_prefixes[0].buf, l);
            }
            pthread_spin_unlock(&g_client_lock);

            if (last_count > 0) {
                updateCurrentAppRates(snap);
            } else {
                /* No foreground app observed yet — still propagate new defaults */
                updateCurrentAppRates("");
            }
        }
        ptr += sizeof(struct inotify_event) + event->len;
    }
}

/* ================================================================== */
/*  Main event loop                                                    */
/* ================================================================== */

static inline void updateTouchStateFromEvent(const struct input_event* ev,
                                             bool use_tracking_fallback,
                                             bool* saw_state,
                                             bool* saw_active,
                                             bool* final_touching) {
    if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        if (ev->value == 0 || ev->value == 1) {
            *saw_state = true;
            *final_touching = (ev->value == 1);
            if (ev->value == 1) *saw_active = true;
        }
        return;
    }

    if (use_tracking_fallback &&
        ev->type == EV_ABS && ev->code == ABS_MT_TRACKING_ID) {
        *saw_state = true;
        *final_touching = (ev->value >= 0);
        if (ev->value >= 0) *saw_active = true;
    }
}

void* touchListenerThread(void* arg) {
    (void)arg;

    if (g_root_mode) {
        findTouchscreens();
    } else {
        LOGI("Non-root mode: skipping touch device registration.");
    }

    setupInotify();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        LOGE("Failed to create epoll instance.");
        return NULL;
    }

    struct epoll_event ev;
    struct epoll_event touch_ev = { .events = EPOLLIN };

    for (int i = 0; i < g_touch_fd_count; i++) {
        touch_ev.data.ptr = tag_fd(FD_TOUCH, g_touch_fds[i]);
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_touch_fds[i], &touch_ev) < 0) {
            LOGE("Failed to add touch fd %d to epoll", g_touch_fds[i]);
        }
    }

    if (g_wakeup_fd >= 0) {
        ev.data.ptr = tag_fd(FD_WAKEUP, g_wakeup_fd);
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_wakeup_fd, &ev) < 0) {
            LOGE("Failed to add wakeup fd to epoll");
        }
    }

    if (g_inotify_fd >= 0) {
        ev.data.ptr = tag_fd(FD_INOTIFY, g_inotify_fd);
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_inotify_fd, &ev) < 0) {
            LOGE("Failed to add inotify fd to epoll");
        }
    }

    if (g_server_fd >= 0) {
        ev.data.ptr = tag_fd(FD_SERVER, g_server_fd);
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_server_fd, &ev) < 0) {
            LOGE("Failed to add server fd to epoll");
        }
    }

    if (g_uevent_fd >= 0) {
        ev.data.ptr = tag_fd(FD_UEVENT, g_uevent_fd);
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_uevent_fd, &ev) < 0) {
            LOGE("Failed to add uevent fd to epoll: %s", strerror(errno));
            close(g_uevent_fd);
            g_uevent_fd = -1;
        }
    }

    struct epoll_event events[64];
    const int MAX_EVENTS = 64;

    #define TOUCH_EVENT_BATCH 64
    struct input_event evs[TOUCH_EVENT_BATCH];

    uint64_t now = getNowMs();

    while (!atomic_load_explicit(&g_shutdown_requested, memory_order_acquire)) {
        int timeout = getPollTimeout(now);
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOGE("epoll_wait error: %s", strerror(errno));
            break;
        }

        now = getNowMs();
        bool needs_rate_update = (nfds == 0);

        if (nfds == 0) {
            checkInteractiveAndPowerSave();
            checkMinBrightness();
            /* Rate-limit battery sysfs scan to every 30 seconds to avoid
             * repeated opendir/readdir/fopen overhead on every idle timeout. */
            if (atomic_load_explicit(&g_battery_saver, memory_order_relaxed)) {
                static uint64_t last_battery_read = 0;
                if (now - last_battery_read >= 30000) {
                    last_battery_read = now;
                    int32_t level = readInitialBatteryLevel();
                    evaluateBatteryState(level);
                }
            }
        }

        /* Handle screen off: reset touch state. Touch fds stay registered in
         * epoll; the kernel suppresses events while the screen is off, so
         * there is no need to ADD/DEL them repeatedly. */
        bool interactive = atomic_load_explicit(&g_screen_interactive, memory_order_acquire);
        if (!interactive) {
            if (atomic_load_explicit(&g_touching, memory_order_relaxed)) {
                atomic_store_explicit(&g_touching, false, memory_order_relaxed);
                atomic_store_explicit(&g_last_touch_time, now, memory_order_relaxed);
                needs_rate_update = true;
            }
        }

        /* Dispatch epoll events */
        for (int i = 0; i < nfds; i++) {
            void* ptr = events[i].data.ptr;
            FdKind kind = get_kind(ptr);
            int fd = get_fd(ptr);
            uint32_t revents = events[i].events;

            if (kind == FD_TOUCH) {
                if (revents & EPOLLIN) {
                    bool saw_touch_state = false;
                    bool saw_active_touch = false;
                    bool final_touching = atomic_load_explicit(&g_touching,
                                                               memory_order_relaxed);
                    bool use_tracking_fallback = false;
                    for (int t = 0; t < g_touch_fd_count; t++) {
                        if (g_touch_fds[t] == fd) {
                            use_tracking_fallback = s_touch_tracking_fallback[t];
                            break;
                        }
                    }

                    while (true) {
                        ssize_t n = read(fd, evs, sizeof(evs));
                        if (n <= 0) {
                            if (n < 0 && errno == EINTR) continue;
                            break;
                        }
                        size_t count = n / sizeof(struct input_event);
                        for (size_t k = 0; k < count; k++) {
                            updateTouchStateFromEvent(&evs[k], use_tracking_fallback,
                                                      &saw_touch_state,
                                                      &saw_active_touch,
                                                      &final_touching);
                        }
                    }

                    if (saw_touch_state) {
                        bool old_touching = atomic_load_explicit(&g_touching,
                                                                 memory_order_relaxed);
                        if (old_touching != final_touching) {
                            atomic_store_explicit(&g_touching, final_touching,
                                                  memory_order_relaxed);
                            atomic_store_explicit(&g_last_touch_time, now,
                                                  memory_order_relaxed);
                            needs_rate_update = true;
                        } else if (!final_touching && saw_active_touch) {
                            atomic_store_explicit(&g_last_touch_time, now,
                                                  memory_order_relaxed);
                            needs_rate_update = true;
                        }
                    }
                }
                continue;
            }

            if (kind == FD_WAKEUP) {
                if (revents & EPOLLIN) {
                    uint64_t val;
                    ssize_t n = read(fd, &val, sizeof(val));
                    if (n < 0 && errno != EINTR) {
                        LOGE("eventfd read failed: %s", strerror(errno));
                    }
                    atomic_flag_clear_explicit(&g_wakeup_pending, memory_order_release);

                    if (atomic_exchange_explicit(&g_query_task_pending, false,
                                                  memory_order_acq_rel)) {
                        queryFocusedTask();
                    }

                    needs_rate_update = true;
                }
                continue;
            }

            if (kind == FD_INOTIFY) {
                if (revents & EPOLLIN) {
                    handleInotifyEvents();
                    needs_rate_update = true;
                }
                continue;
            }

            if (kind == FD_UEVENT) {
                if (revents & EPOLLIN) {
                    while (handleUevent()) {
                    }
                    needs_rate_update = true;
                }
                continue;
            }

            if (kind == FD_SERVER) {
                if (revents & EPOLLIN) {
                    while (true) {
                        int client_fd = accept4(fd, NULL, NULL,
                                                SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (client_fd < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK &&
                                errno != EINTR) {
                                LOGE("accept4 failed on @dfps: %s", strerror(errno));
                            }
                            if (errno == EINTR) continue;
                            break;
                        }

                        pthread_spin_lock(&g_client_lock);
                        if (g_client_count < MAX_CLIENTS) {
                            struct epoll_event c_ev;
                            c_ev.data.ptr = tag_fd(FD_CLIENT, client_fd);
                            c_ev.events = EPOLLIN | EPOLLRDHUP;
                            if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &c_ev) < 0) {
                                LOGE("Failed to add client fd to epoll");
                                close(client_fd);
                            } else {
                                g_client_fds[g_client_count++] = client_fd;
                                LOGI("New socket client connected on @dfps (Total: %d)",
                                     g_client_count);

                                /* Send current foreground packages to new client */
                                if (g_last_package_count > 0) {
                                    char snap_buf[1024];
                                    int snap_offset = 0;
                                    for (int p = 0; p < g_last_package_count; p++) {
                                        if (p > 0) snap_buf[snap_offset++] = ' ';
                                        int len = g_last_package_prefixes[p].len;
                                        if (snap_offset + len < 1000) {
                                            memcpy(snap_buf + snap_offset,
                                                   g_last_package_prefixes[p].buf, len);
                                            snap_offset += len;
                                        }
                                    }
                                    snap_buf[snap_offset++] = '\n';
                                    send(client_fd, snap_buf, snap_offset,
                                         MSG_NOSIGNAL | MSG_DONTWAIT);
                                }
                            }
                        } else {
                            LOGW("Abstract socket clients limit reached. "
                                 "Refusing connection.");
                            close(client_fd);
                        }
                        pthread_spin_unlock(&g_client_lock);
                    }
                }
                continue;
            }

            if (kind == FD_CLIENT) {
                if (revents & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    char dummy[64];
                    ssize_t n = recv(fd, dummy, sizeof(dummy), MSG_DONTWAIT);

                    bool should_close = false;
                    if (n == 0) {
                        should_close = true;
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        should_close = true;
                    }

                    if (should_close) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        pthread_spin_lock(&g_client_lock);
                        for (int j = 0; j < g_client_count; j++) {
                            if (g_client_fds[j] == fd) {
                                g_client_fds[j] = g_client_fds[g_client_count - 1];
                                g_client_count--;
                                LOGI("Socket client disconnected from @dfps "
                                     "(Remaining: %d)", g_client_count);
                                break;
                            }
                        }
                        pthread_spin_unlock(&g_client_lock);
                    }
                }
                continue;
            }
        }

        /* Apply rate update if any event triggered a state change */
        if (needs_rate_update) {
            updateRateState();
        }
    }

    close(epfd);
    if (g_inotify_fd >= 0) close(g_inotify_fd);
    if (g_server_fd >= 0) close(g_server_fd);
    if (g_wakeup_fd >= 0) close(g_wakeup_fd);
    if (g_uevent_fd >= 0) close(g_uevent_fd);
    for (int i = 0; i < g_touch_fd_count; i++) {
        close(g_touch_fds[i]);
    }
    return NULL;
}
