# Client Protocol

DFPS exposes a simple abstract Unix socket named `@dfps`.

## Authentication

Every connection is authenticated with `SO_PEERCRED` immediately on accept.
Only peers running as root (UID 0), the daemon's own UID, or the Android
shell (AID_SHELL = 2000, i.e. `adb shell`) are accepted. Any other UID is
disconnected before any data is exchanged. Unprivileged local apps therefore
cannot learn the foreground package list.

## What clients get

Each client receives one newline-delimited line containing the current
foreground-package list, sent immediately after a successful accept. When the
foreground changes, the daemon sends a new line.

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

## Commands

After connecting, an authenticated client may send a single request line
terminated by a newline (`\n` or `\r\n`):

- `STATUS` — the daemon replies with a one-line health snapshot of the form:

  ```text
  idle=60 active=120 last=120 interactive=1 powersave=0 lowbatt=0 minbright=0 callback=1 uptime_ms=123456
  ```

  All fields except `uptime_ms` are integers (booleans shown as `0`/`1`).
  Any command other than `STATUS` is treated as a protocol violation and the
  client is disconnected.

The initial foreground-package snapshot is always pushed; `STATUS` is an
explicit request/response on top of that.

## Parsing

- split the snapshot on ASCII spaces, stop at newline
- treat the line as best-effort state, not a guaranteed delivery queue
- tolerate truncation
- for `STATUS`, read one line and parse `key=value` pairs

## Limits

- request/response exists only for the `STATUS` command
- only UIDs root / daemon / shell are allowed to connect
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
