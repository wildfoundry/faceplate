/* SPDX-License-Identifier: MIT */
#ifndef FACEPLATE_HOST_SESSION_H
#define FACEPLATE_HOST_SESSION_H

/*
 * Host-namespace login handoff
 *
 * Faceplate's compositor unit is sandboxed (ProtectSystem=strict,
 * RestrictAddressFamilies). Login shells must not inherit that. The PTY child
 * keeps stdin/stdout/stderr on the slave, then hands the fd to an unsandboxed
 * helper over AF_UNIX + SCM_RIGHTS. systemd seccomp is per-process-tree and
 * Helper policy (production):
 * - SO_PEERCRED uid must match the helper euid (root)
 * - peer cgroup must contain faceplate.service
 * - fd must be a Unix98 PTY slave
 * - exec is always /bin/login -p; payload argv/environ are not executed
 */

#define FACEPLATE_HOST_LOGIN_SOCKET_DEFAULT "/run/faceplate/host-login.sock"
#define FACEPLATE_HOST_LOGIN_MAGIC 0x314c5046u /* 'FPL1' little-endian */

/* Join pid 1 mount/net/uts/ipc namespaces and move to the root cgroup.
 * Best-effort; returns 0 on full success, negative errno otherwise.
 */
int faceplate_host_session_enter_init_namespaces(void);

/* Hand the current stdin fd and argv/environ to the helper, then _exit with
 * the helper-reported status. Returns -1 if the helper is unavailable.
 */
int faceplate_host_session_exec(char **argv);

/* Serve one accepted client connection (helper). Returns 0 after the login
 * process exits and the status has been written back.
 */
int faceplate_host_session_serve_one(int conn);

const char *faceplate_host_session_socket_path(void);

#endif
