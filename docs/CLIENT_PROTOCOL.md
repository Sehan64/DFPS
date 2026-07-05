# Client Protocol — The `@dfps` Abstract Socket

DFPS exposes a small fire-and-forget notification channel to user-space
clients over an **abstract Unix domain socket** named `@dfps`. It is
intentionally one-directional and intentionally minimal: clients
**read** the current foreground-app list, and the daemon **pushes** a
new list every time it changes.

This document specifies the wire protocol, lifecycle, error handling,
and provides a reference implementation you can copy.

> **Audience.** Anyone integrating DFPS into a Tasker / shell /
> third-party app. You only need `connect()`, `read()`, and the ability
> to parse a line of space-separated strings.

---

## Table of Contents

1. [What `@dfps` Is — and Isn't](#1-what-dfps-is--and-isnt)
2. [Quick Start](#2-quick-start)
3. [Wire Format](#3-wire-format)
4. [Lifecycle](#4-lifecycle)
5. [Connection-Local Semantics](#5-connection-local-semantics)
6. [Parsing Rules](#6-parsing-rules)
7. [Error Handling](#7-error-handling)
8. [Limits and Backpressure](#8-limits-and-backpressure)
9. [Reference Implementations](#9-reference-implementations)
10. [Security Considerations](#10-security-considerations)

---

## 1. What `@dfps` Is — and Isn't

| ✅ It is                                       | ❌ It isn't                                  |
|------------------------------------------------|----------------------------------------------|
| A one-way push channel of foreground packages. | A request/response RPC.                      |
| Best-effort, no delivery guarantees.           | A guaranteed-delivery message bus.           |
| Stateless from the client's perspective.       | Stateful: each client gets an initial snapshot. |
| Bound to an abstract namespace (`@dfps`).     | A regular filesystem socket.                 |
| Read-only by the client.                       | Read-write.                                  |

If you need to **change** DFPS's behaviour, edit `dfps.conf`. If you
need to **trigger** a rate change, you cannot — DFPS is the single
arbiter of the rate.

---

## 2. Quick Start

```sh
# 1. Confirm the daemon is running and the socket is up
$ cat /proc/net/unix | grep '@dfps'
0000000000000000 0 1000 0 1 @dfps

# 2. Connect and read
$ exec 3<>/dev/tcp/unix/@dfps   # or: nc -U @dfps
$ cat <&3
com.android.chrome
```

That's the entire API. Each newline-terminated line is a snapshot of
the foreground-app list at a point in time.

---

## 3. Wire Format

The protocol is **line-based UTF-8 text** over an abstract `AF_UNIX`
`SOCK_STREAM` socket. Each line is a single snapshot of the
foreground-app list.

### 3.1 Single-Line Snapshot

```
pkgA pkgB pkgC\n
```

- **Fields:** the package-prefix of each foreground task, **separated
  by a single ASCII space (0x20)**. There is never a leading space.
- **Order:** the first field is the topmost focused package; the
  remainder are the tasks under it in the same root-task window.
- **Length:** up to 1024 bytes total (the daemon's internal buffer).
  The daemon **truncates** without erroring if the limit is reached —
  the client's job is to be tolerant of a too-short list.
- **Terminator:** a single ASCII newline (0x0A). No carriage return.

### 3.2 Package Prefix

A "package prefix" is the substring of a `taskName` returned by
`ActivityManager` up to (but not including) the first `/` character,
or the entire `taskName` if it has no `/`. This corresponds to the
**component name's class name** in tasks whose root component is
`<pkg>/<class>`, e.g.:

| `taskName` (raw)                       | Package prefix sent to clients |
|----------------------------------------|--------------------------------|
| `com.android.chrome/com.google...`     | `com.android.chrome`           |
| `com.google.android.apps.nexuslauncher` | `com.google.android.apps.nexuslauncher` |
| `com.android.systemui/StatusBar`      | `com.android.systemui`         |

The slash-split is intentional: many task names contain the class as
the leaf, but the package is the only piece clients care about.

### 3.3 Single-Element Snapshots

A single package is sent as just the prefix, terminated by `\n`:

```
com.android.chrome\n
```

There is no leading "0x01 marker" or any other framing — the
distinction between "empty" and "one" is the length of the line.

### 3.4 Empty Snapshots

When no foreground task is present (e.g. the user is in Recents with no
app above), the daemon sends an **empty line**:

```

```

That is, a single `0x0A` byte. Clients should treat a zero-length
content followed by `\n` as "no foreground app".

### 3.5 No Length Prefix

There is no length prefix, no version byte, and no magic. The protocol
is **deliberately trivial** so it can be read with `cat`, `socat`,
`nc`, or 5 lines of C. If you need a versioned protocol, layer it on
top.

---

## 4. Lifecycle

### 4.1 Connection Establishment

A client opens a `SOCK_STREAM` connection to abstract namespace `dfps`.
On success the kernel returns immediately; the daemon is in
`accept4()` and accepts the connection on its next epoll cycle.

```c
int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
struct sockaddr_un addr = { .sun_family = AF_UNIX };
addr.sun_path[0] = '\0';
strcpy(&addr.sun_path[1], "dfps");
connect(fd, (struct sockaddr*)&addr,
        offsetof(struct sockaddr_un, sun_path) + 1 + strlen("dfps"));
```

### 4.2 Initial Snapshot

**Immediately** on accept, the daemon **sends the current
foreground-app list** to the new client. This means a freshly connected
client does not need to wait for the next foreground change to learn
what's focused.

If the daemon has not yet observed any foreground task (e.g. it
started 200 ms ago), the initial snapshot is the empty line.

The send uses `MSG_NOSIGNAL | MSG_DONTWAIT`. If the client's receive
buffer is full, the initial snapshot is **dropped** — the client will
still get the next foreground transition.

### 4.3 Steady State

The daemon pushes a new line on every **foreground transition**:

1. The user navigates to a new activity.
2. `IProcessObserver.onForegroundActivitiesChanged` fires.
3. The daemon wakes the epoll thread, calls `queryFocusedTask()`,
   diffs the result, and **if it differs from the previous snapshot**,
   sends the new list to all clients.

If a diff is empty (e.g. the foreground app changed but the package
prefixes are identical, or the foreground task was already tracked),
no message is sent. Clients should therefore treat "no message for a
while" as "nothing changed".

### 4.4 Disconnection

The client closes its end with `close()` / `shutdown()`. The daemon
detects the EOF (or `EPOLLRDHUP`) and removes the client from its
table. **The daemon never sends a goodbye frame.** A client can
disconnect at any time with no protocol error.

If the daemon is killed (e.g. via `killall dfps`), all client
connections are closed by the kernel. The next `read()` on the client
side returns `0` (EOF).

---

## 5. Connection-Local Semantics

The protocol is **stateless except for the initial snapshot**. Two
clients connected at the same time each see:

- Their own initial snapshot (which is the same as everyone else's
  *if* the foreground has not changed since they connected).
- The same subsequent transitions in the same order — but **with no
  ordering guarantee relative to other clients**. A transition that
  happened during a brief TCP buffer-full window may reach client B
  before client A.

In practice, transitions are rare and the buffer-empty window is
~zero, so this is not observable. But you should not build a system
that requires "client A received message N before client B did".

---

## 6. Parsing Rules

A robust parser must tolerate the following:

1. **Empty line.** No fields. Treat as "no foreground app".
2. **One line at a time.** The protocol is line-delimited, but
   `read()` can return multiple lines in one syscall or one line
   across two reads. **Buffer until `\n`.**
3. **Embedded NULs.** Should not occur in practice (package names
   don't contain them), but be defensive.
4. **Truncation.** A line may be shorter than the daemon intended
   because the daemon truncates at 1024 bytes. A client should not
   assume a "complete" list, only that the bytes up to `\n` are valid.
5. **Non-UTF-8.** A valid task name is always ASCII / printable
   UTF-8. If you receive anything else, treat as a protocol error
   and close the connection.
6. **Trailing whitespace.** No, the daemon does not emit trailing
   spaces. But your parser should be tolerant.

A naïve but correct parser:

```c
char buf[2048];
int  n = 0;
while (1) {
    ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
    if (r <= 0) break;          // EOF or error
    n += r;
    buf[n] = '\0';

    char* nl;
    while ((nl = strchr(buf, '\n')) != NULL) {
        *nl = '\0';
        process_line(buf);     // buf is now a single line
        int consumed = (nl - buf) + 1;
        memmove(buf, buf + consumed, n - consumed);
        n -= consumed;
    }
}
```

---

## 7. Error Handling

### 7.1 Client-Side Errors

| Condition                       | Recommended action                                  |
|---------------------------------|-----------------------------------------------------|
| `ECONNREFUSED` on connect       | Daemon is not running. Wait and retry, or fall back to a no-op. |
| `read()` returns `0`            | Daemon has exited. Close and reconnect.            |
| `read()` returns `-1`, `EINTR`  | Retry.                                              |
| `read()` returns `-1`, other    | Treat as fatal, close, reconnect with backoff.     |
| Malformed line                  | Log and skip.                                       |
| Connection stalls > 30 s        | Heartbeat missing — daemon may be wedged. Reconnect.|

### 7.2 Daemon-Side Errors

The daemon does not return errors to the client. If a write to a
client fails (`EPIPE` on `MSG_NOSIGNAL`, `EAGAIN` on a full
non-blocking buffer), the daemon **silently drops the message and
keeps the client registered** for the next event. If the failure
persists (i.e. the client never reads), the daemon will continue to
queue up to its internal send-buffer depth per event and then drop.
The daemon never proactively closes a client on a write error.

If the client **never reads at all**, the kernel will eventually
backpressure and the daemon's `send()` will return `EAGAIN` for that
client; the message is dropped. The client connection remains open
from the daemon's perspective until the client actually closes it.

### 7.3 Recovering from a Stale Connection

A client that reconnects (e.g. after a daemon restart) will receive
the **current** snapshot on the new connection. There is no replay
of missed messages. If your application needs every transition
logged, persist the latest snapshot in your own storage and treat
each new line as "this is now the foreground" without assuming the
gap between messages is empty.

---

## 8. Limits and Backpressure

| Limit                    | Value     | Origin                                     |
|--------------------------|-----------|--------------------------------------------|
| Max concurrent clients   | 8         | `MAX_CLIENTS` in `dfps.h`                  |
| Max bytes per snapshot   | 1024      | Internal send buffer                       |
| Max tracked tasks        | 8         | `MAX_TASKS`                                |
| Max bytes per package    | 124       | `PackagePrefix.buf` size                   |
| Send flags               | `MSG_NOSIGNAL \| MSG_DONTWAIT` | Never SIGPIPE the daemon; never block. |

The 8-client cap is generous for the intended use case (Tasker, a
shell script, and a couple of debugging tools). If you need more
concurrent consumers, run your own fan-out from a single client
process — that is the canonical pattern.

The 8-task cap corresponds to the deepest legitimate task stack on
stock Android. Real stacks are almost always 1–3 deep.

---

## 9. Reference Implementations

### 9.1 Shell (one-shot)

```sh
#!/system/bin/sh
# print every foreground change until ^C
exec 3<>/dev/tcp/unix/@dfps || { echo "DFPS not running"; exit 1; }
cat <&3
```

(The exact `connect-to-abstract-socket` incantation varies by shell.
On Android, `toybox nc -U @dfps` also works.)

### 9.2 Shell (subscribe once, react)

```sh
#!/system/bin/sh
exec 3<>/dev/tcp/unix/@dfps || exit 1
while IFS= read -r line <&3; do
    echo "[$(date +%T)] foreground: ${line:-<none>}"
    # … your reaction here …
done
echo "DFPS daemon disconnected."
```

### 9.3 C (Android NDK)

```c
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strcpy(&addr.sun_path[1], "dfps");
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + 4;
    if (connect(fd, (struct sockaddr*)&addr, len) != 0) return 1;

    char buf[2048];
    int n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) break;
        n += r;
        buf[n] = '\0';
        char* nl;
        while ((nl = strchr(buf, '\n')) != NULL) {
            *nl = '\0';
            printf("fg: [%s]\n", buf);
            int consumed = nl - buf + 1;
            memmove(buf, buf + consumed, n - consumed);
            n -= consumed;
        }
    }
    return 0;
}
```

### 9.4 Python (host machine, e.g. for monitoring over adb)

```python
import socket, sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("\0dfps")          # leading NUL = abstract namespace
try:
    buf = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            sys.stdout.write("fg: %s\n" % line.decode("utf-8", "replace"))
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
```

### 9.5 Java (Android app)

```java
import android.net.LocalSocket;
import android.net.LocalSocketAddress;

LocalSocket sock = new LocalSocket();
sock.connect(new LocalSocketAddress("dfps",
        LocalSocketAddress.Namespace.ABSTRACT));
InputStream in = sock.getInputStream();
byte[] buf = new byte[2048];
int n = 0;
for (;;) {
    int r = in.read(buf, n, buf.length - n);
    if (r <= 0) break;
    n += r;
    int start = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            String line = new String(buf, start, i - start, "UTF-8");
            onForegroundChanged(line.isEmpty() ? null : line);
            start = i + 1;
        }
    }
    System.arraycopy(buf, start, buf, 0, n - start);
    n -= start;
}
```

---

## 10. Security Considerations

The socket is created in the **abstract namespace** and bound to a
well-known name. It is **not** placed in the filesystem, so standard
filesystem permissions do not apply. In practice:

- The abstract namespace is per-network-namespace, which on Android is
  per-mount-namespace-per-app. **Any process running as root in the
  same network namespace can connect.** That includes the system
  server, adb shell, and any root-capable app.
- **No authentication is performed.** A malicious local app with
  root could connect and observe foreground transitions. The leak is
  the same as what `dumpsys activity activities` already exposes, so
  DFPS does not worsen the situation.
- **No client identity is tracked.** The daemon does not log which
  UID connected.

If your threat model includes untrusted local apps, treat the socket
as world-readable-and-noisy. Do not rely on it for anything that
requires confidentiality.

For a deeper security analysis, see [`SECURITY.md`](SECURITY.md).

---

## Appendix A — Grammar (ABNF)

```abnf
snapshot      = *field LF
field         = 1*VCHAR                       ; trimmed at first '/'
LF            = %x0A
VCHAR         = %x21-7E                        ; printable ASCII (excl. space)
                                                ; extended to UTF-8 in practice
```

That is the entire grammar. There is no version byte, no length
prefix, no checksum.
