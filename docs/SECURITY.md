# Security

Root-resident daemon: goal is predictable behavior, narrow local exposure,
and clean restart — not protection against a root attacker.

## Threat model

| Attacker | Goal | Mitigation |
|---|---|---|
| Unprivileged app | Read FG package list / drive rates | `SO_PEERCRED` on `@dfps`; conf under root-owned `0700` dir |
| Unprivileged app | Rewrite conf/map via world-writable dir | Warning + best-effort chmod; install as `0700` root:root |
| Compromised daemon | Escalate further | `NO_NEW_PRIVS`, undumpable, no setuid path |
| Root attacker | Full control | Out of scope |

## Hardening (process)

- `mlockall`, timerslack 0, IO_FLUSHER
- `SCHED_FIFO` (or nice `-20`)
- `PR_SET_NO_NEW_PRIVS`, `PR_SET_DUMPABLE(0)`, `PR_SET_KEEP_CAPS`
- Blocked `SIGPIPE` / `SIGWINCH`
- Capability reduction left to init/SELinux ([`INIT.md`](./INIT.md))

## Local interfaces

| Interface | Access |
|---|---|
| `@dfps` | UID 0 / self / shell (2000) only; package push + `STATUS` |
| `dfps.conf` / `modes.map` | Root-writable by design; world-writable dir is a footgun |
| Tx-code cache + resolver JAR | Written `0600`, `O_EXCL`, `O_NOFOLLOW`; fingerprint-scoped |

## Failure handling

- Critical Binder death → graceful shutdown (not `exit` from the callback)
- Duplicate instance → bind failure, process exits
- Missing `dfps.conf` → built-in defaults (no stale rules)
- Missing/empty `modes.map` mid-run → **keep** last good map (availability)

## Operational checklist

1. Runtime dir `chown root:root && chmod 700`.
2. Run under a domain that needs only Binder + input + uevent.
3. Do not expose `@dfps` via a setuid or world-reachable proxy.
4. Treat `STATUS` and package push as sensitive local telemetry.
