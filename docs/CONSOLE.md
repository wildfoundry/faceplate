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
in a different systemd unit.

## Split

1. **`faceplate.service`** — DRM compositor and PTY master. Keep the sandbox.
2. **`faceplate-host-login.socket` / `.service`** — unsandboxed helper.
   The PTY child (still sandboxed) sends the slave fd over
   `/run/faceplate/host-login.sock` with `SCM_RIGHTS`. The helper `exec`s
   `/bin/login` in the host mount, network, and cgroup world.

If the helper is missing, Faceplate still tries `setns` into pid 1's
namespaces before a local `exec` so `/` may become writable; TCP remains
blocked until the helper is running.

Override the socket path with `FACEPLATE_HOST_LOGIN_SOCKET`. Disable handoff
with `FACEPLATE_HOST_LOGIN=0` (tests).
