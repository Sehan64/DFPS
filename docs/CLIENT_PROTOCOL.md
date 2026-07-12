# Client protocol (`@dfps`)

Abstract Unix stream socket for foreground package push and a single health
command. Not a general control plane.

## Endpoint

| | |
|---|---|
| Family | `AF_UNIX` |
| Type | `SOCK_STREAM` |
| Namespace | abstract |
| Name | `dfps` (path byte 0 is `\0`, then `dfps`) |

## Authentication

On accept, the daemon reads `SO_PEERCRED` and keeps the connection only if:

- peer UID is `0` (root), or
- peer UID equals the daemon’s UID, or
- peer UID is `2000` (`AID_SHELL` / `adb shell`)

Everyone else is closed before any payload is sent. This protects the
foreground package list from unprivileged apps.

## Messages

All messages are newline-terminated text. No length prefix, no framing beyond
`\n` / `\r\n`. Delivery is best-effort (`MSG_DONTWAIT` / non-blocking).

### Server → client (push)

Immediately after accept, and whenever the focused package list changes:

```text
com.android.chrome com.android.systemui\n
```

- Space-separated package names (prefix before `/` in task names).
- Empty line (`\n` alone) means no foreground task was observed.
- Tolerate truncation; treat as a snapshot, not a reliable queue.

### Client → server

One request line. Only:

```text
STATUS\n
```

Response:

```text
idle=60 active=120 last=120 interactive=1 powersave=0 lowbatt=0 minbright=0 callback=1 uptime_ms=123456\n
```

Parse as `key=value` pairs. Fields are documented in [`OPS.md`](./OPS.md).
Any other command closes the client.

## Limits

- Max concurrent clients: 8.
- Max packages in a push line: 8 (daemon internal cap).
- Unknown command → disconnect (authenticated peers only reach this path).

## Minimal C client

```c
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(&addr.sun_path[1], "dfps", 4);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + 4;

    if (connect(fd, (struct sockaddr*)&addr, len) != 0) return 1;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1); /* initial package push */
    if (n > 0) { buf[n] = '\0'; fputs(buf, stdout); }

    const char* req = "STATUS\n";
    if (write(fd, req, 7) != 7) return 1;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; fputs(buf, stdout); }

    close(fd);
    return 0;
}
```

Run as root or from `adb shell`.
