# Client Protocol

DFPS exposes a simple read-only abstract Unix socket named `@dfps`.

## What clients get

Each client receives one newline-delimited line containing the current
foreground-package list. When the foreground changes, the daemon sends a new
line.

Example:

```text
com.android.chrome com.android.systemui
```

An empty line means no foreground app was observed.

## Connection

- socket type: `AF_UNIX`
- mode: `SOCK_STREAM`
- namespace: abstract
- name: `dfps`

The daemon also sends an initial snapshot immediately after accept.

## Parsing

- split on ASCII spaces
- stop on newline
- treat the line as best-effort state, not a guaranteed delivery queue
- tolerate truncation

## Limits

- no request/response behavior
- no authentication
- no length prefix
- no framing beyond newline termination

## Example

```c
int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
struct sockaddr_un addr = { .sun_family = AF_UNIX };
addr.sun_path[0] = '\0';
strcpy(&addr.sun_path[1], "dfps");
connect(fd, (struct sockaddr*)&addr,
        offsetof(struct sockaddr_un, sun_path) + 1 + strlen("dfps"));
```
