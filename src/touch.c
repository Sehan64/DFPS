/*
 * Copyright 2023 yc9559
 * Copyright 2024 Sehan64
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
 * touch.c — Touch input detection, inotify config reload, and main event loop
 *
 * Manages the epoll-based event loop that drives the entire daemon:
 * touch events, config hot-reload, client socket I/O, battery uevent,
 * and periodic state evaluation.
 */

#include "dfps.h"

static bool s_touch_tracking_fallback[MAX_TOUCH_DEVICES];
static unsigned long s_touch_active_slots[MAX_TOUCH_DEVICES];
static int s_touch_current_slot[MAX_TOUCH_DEVICES];

/* ================================================================== */
/*  Touch device discovery                                             */
/* ================================================================== */

/* Return the peer's UID for an accepted AF_UNIX socket, or false if it
 * cannot be determined (treat as unauthorized). Used to gate the @dfps
 * control socket, which otherwise lets any local app learn the
 * foreground package name. */
static bool client_cred_uid(int fd, uid_t* out_uid) {
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0)
        return false;
    *out_uid = cred.uid;
    return true;
}

/* The @dfps socket broadcasts the foreground package, which is sensitive.
 * Only root, the daemon's own UID, and the Android shell (adb,
 * AID_SHELL=2000) may connect. Anything else is refused. */
static bool client_authorized(int fd) {
    uid_t peer = (uid_t)-1;
    if (!client_cred_uid(fd, &peer))
        return false;
    uid_t me = getuid();
    return (peer == 0 || peer == me || peer == 2000);
}

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
                    s_touch_active_slots[g_touch_fd_count] = 0;
                    s_touch_current_slot[g_touch_fd_count] = 0;
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

    strlcpy(g_watch_dir, "/data/local/tmp/dfps", PATH_MAX);
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
        } else if (event->len > 0 && strcmp(event->name, "modes.map") == 0) {
            LOGI("Mode map modification event triggered.");
            loadModesMap();
        }
        ptr += sizeof(struct inotify_event) + event->len;
    }
}

/* ================================================================== */
/*  Main event loop                                                    */
/* ================================================================== */

static inline void updateTouchStateFromEvent(const struct input_event* ev,
                                             int device_index,
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

    if (!use_tracking_fallback || device_index < 0 ||
        device_index >= MAX_TOUCH_DEVICES || ev->type != EV_ABS) {
        return;
    }

    if (ev->code == ABS_MT_SLOT) {
        if (ev->value >= 0 &&
            ev->value < (int)(sizeof(s_touch_active_slots[device_index]) * CHAR_BIT)) {
            s_touch_current_slot[device_index] = ev->value;
        }
        return;
    }

    if (ev->code == ABS_MT_TRACKING_ID) {
        int slot = s_touch_current_slot[device_index];
        unsigned long bit = 1UL << slot;
        *saw_state = true;
        if (ev->value >= 0) {
            s_touch_active_slots[device_index] |= bit;
            *saw_active = true;
        } else {
            s_touch_active_slots[device_index] &= ~bit;
        }
        *final_touching = (s_touch_active_slots[device_index] != 0);
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

    /* Overhead note: this loop is the daemon's only hot path. It does one
     * coarse CLOCK_MONOTONIC read per iteration (getNowMs, used both for the
     * epoll timeout and below), no allocations, and no Binder traffic while
     * the IDisplayManager callback is active (foreground/power/brightness
     * arrive as events, not polls). The idle branch below is the only periodic
     * work and is already minimal + battery-scan-gated. Further reductions
     * (e.g. dropping the per-idle power-save probe) require on-device
     * profiling (perfetto/systrace) to confirm they are safe — do not
     * micro-optimize blind. */
    uint64_t now = getNowMs();

    while (!atomic_load_explicit(&g_shutdown_requested, memory_order_acquire)) {
        if (consumeShutdownSignal()) break;
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
            bool cb_active = atomic_load_explicit(&g_display_callback_active,
                                                memory_order_relaxed);
            if (!cb_active) {
                /* No display callback: poll interactive, power-save, and
                 * brightness state the old-fashioned way. */
                checkInteractiveAndPowerSave(true);
                checkMinBrightness();
            } else {
                /* The IDisplayManager callback delivers interactive (event 2)
                 * and brightness (event 4) changes, so skip those two queries
                 * here. It does NOT deliver power-save-mode transitions, so we
                 * still probe power-save to keep g_power_save_mode current. */
                checkInteractiveAndPowerSave(false);
            }
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
            for (int t = 0; t < g_touch_fd_count; t++) {
                s_touch_active_slots[t] = 0;
                s_touch_current_slot[t] = 0;
            }
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
                    int device_index = -1;
                    for (int t = 0; t < g_touch_fd_count; t++) {
                        if (g_touch_fds[t] == fd) {
                            device_index = t;
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
                            updateTouchStateFromEvent(&evs[k], device_index,
                                                      use_tracking_fallback,
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
                    if (consumeShutdownSignal()) {
                        needs_rate_update = false;
                        continue;
                    }

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

                        /* Authenticate the peer before any I/O. */
                        if (!client_authorized(client_fd)) {
                            LOGW("Rejected unauthorized @dfps client");
                            close(client_fd);
                            continue;
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
                    char buf[128];
                    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
                    bool should_close = false;

                    if (n > 0) {
                        buf[n] = '\0';
                        size_t l = (size_t)n;
                        while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                            buf[--l] = '\0';

                        if (strcmp(buf, "STATUS") == 0) {
                            /* Post-auth diagnostics snapshot. */
                            uint64_t up = (g_start_time ? (getNowMs() - g_start_time) : 0);
                            char health[256];
                            int hl = snprintf(health, sizeof(health),
                                "idle=%d active=%d last=%d interactive=%d powersave=%d "
                                "lowbatt=%d minbright=%d callback=%d uptime_ms=%llu\n",
                                (int)atomic_load_explicit(&g_curr_idle_rate, memory_order_relaxed),
                                (int)atomic_load_explicit(&g_curr_active_rate, memory_order_relaxed),
                                (int)atomic_load_explicit(&g_last_set_rate, memory_order_relaxed),
                                atomic_load_explicit(&g_screen_interactive, memory_order_relaxed) ? 1 : 0,
                                atomic_load_explicit(&g_power_save_mode, memory_order_relaxed) ? 1 : 0,
                                atomic_load_explicit(&g_low_battery_mode, memory_order_relaxed) ? 1 : 0,
                                atomic_load_explicit(&g_min_brightness_clamp, memory_order_relaxed) ? 1 : 0,
                                atomic_load_explicit(&g_display_callback_active, memory_order_relaxed) ? 1 : 0,
                                (unsigned long long)(up));
                            if (send(fd, health, (size_t)hl, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
                                errno != EAGAIN) {
                                /* Client vanished mid-send; drop below. */
                                should_close = true;
                            }
                        } else {
                            /* Unknown command from an authed client -> drop. */
                            should_close = true;
                        }
                    } else {
                        /* Clean EOF (n == 0) or a hard read error drops the
                         * client; EAGAIN/EWOULDBLOCK/EINTR are transient. */
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                     errno == EINTR))
                            should_close = false;
                        else
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
