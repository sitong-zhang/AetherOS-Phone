/*
 * aether-sandbox.h - AetherOS service sandbox contract.
 *
 * A sandboxed service runs as a dedicated non-root uid/gid (1000), with:
 *   - a private mount namespace (CLONE_NEWNS, MS_PRIVATE) so any mounts it
 *     performs are local and cannot perturb the rest of the system;
 *   - its entire capability set dropped (bounding set cleared, effective/
 *     permitted/inheritable zeroed);
 *   - a seccomp BPF allow-list filter: a known-good set of syscalls is
 *     permitted, a set of privilege-escalation / namespace / kernel-modifying
 *     syscalls is killed, and everything else returns -EPERM.
 *
 * Unprivileged user namespaces are disabled system-wide by aether-init
 * (user.max_user_namespaces=0), so a sandboxed process cannot clone a new
 * user namespace to regain root.
 *
 * aether_apply_sandbox() is called in the service child, after fork and
 * the signal reset, before exec. It is best-effort and logs each step.
 */
#ifndef AETHER_SANDBOX_H
#define AETHER_SANDBOX_H

#define AETHER_SANDBOX_UID 1000   /* dedicated unprivileged app uid */
#define AETHER_SANDBOX_GID 1000   /* dedicated unprivileged app gid */

/*
 * Apply the full process sandbox. Returns 0 on success; on failure it logs
 * and returns -1 but does NOT abort the spawn (the caller still execs, just
 * without the full sandbox — never silently claiming success).
 */
int aether_apply_sandbox(const char *svcname);

#endif /* AETHER_SANDBOX_H */
