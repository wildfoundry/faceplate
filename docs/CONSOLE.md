# Console sandbox vs login

Faceplate replaces `getty` on an appliance VT. The compositor process is
sandboxed:

- `ProtectSystem=strict` remounts the file hierarchy read-only in that mount
  namespace
- `RestrictAddressFamilies=AF_NETLINK AF_UNIX` is a seccomp filter, inherited
  by every child

A login shell that `fork`+`exec`s from that process therefore sees `/` and
`/run` as read-only (so `ufw` cannot create `/run/ufw.lock`) and cannot open
`AF_INET` sockets (`ssh: Address family not supported by protocol`).

`setns(2)` into pid 1's namespaces does **not** drop seccomp. Login must run
in a different systemd unit. Faceplate also must **not** `setns` then exec
locally: that would make the host disk writable from the compositor cgroup.

## Split

1. **`faceplate.service`** — DRM compositor and PTY master. Keep the sandbox.
2. **`faceplate-host-login.socket` / `.service`** — getty-equivalent helper.

The PTY child sends only a slave fd over `/run/faceplate/host-login.sock`
(`0600`, root). The helper:

- requires `SO_PEERCRED` uid equal to the helper euid (root in production)
- requires the peer cgroup name to contain `faceplate.service`
- requires the fd to be a Unix98 PTY slave (`isatty`, char device, major 136)
- **ignores** client `argv` and environ (no `LD_PRELOAD`, no chosen binary)
- execs only `/bin/login -p` with a fixed `PATH` and a sanitized `TERM`
- fails closed if the helper is missing (no host-namespace fallback)

The helper unit uses `ProtectSystem=strict` and related hardening. It must
**not** set `RestrictAddressFamilies` or `PrivateNetwork`: login shells need
TCP. A Faceplate compromise is still console-equivalent (it can request a
login PTY), not a general root command spawner.

Override the socket path with `FACEPLATE_HOST_LOGIN_SOCKET`. Disable handoff
with `FACEPLATE_HOST_LOGIN=0` (tests). Tests may set
`FACEPLATE_HOST_LOGIN_SKIP_CGROUP` and `FACEPLATE_HOST_LOGIN_BIN`; production
units must not.
