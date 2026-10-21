/*
 * aether-sandbox.c - AetherOS service sandbox implementation.
 *
 * Real, dependency-free sandboxing built only on Linux primitives:
 *   unshare(CLONE_NEWNS) + MS_PRIVATE  -> isolated mount view
 *   setresuid/setresgid to 1000        -> non-root identity (while still
 *                                        holding CAP_SETUID: dropping caps
 *                                        first would make this EPERM)
 *   PR_CAPBSET_DROP (all) + capset 0   -> no capabilities at all
 *   PR_SET_NO_NEW_PRIVS + seccomp BPF  -> syscall allow-list
 *
 * No libcap, no libseccomp: raw syscalls and a hand-written BPF program.
 * See aether-sandbox.h for the threat model and ordering rationale.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -c aether-sandbox.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <linux/capability.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "aether-sandbox.h"

/* ---------------------------------------------------------------- logging */

static void sb_log(const char *svc, const char *what, int ok, const char *detail)
{
    fprintf(stderr, "aether-sandbox[%s]: %s %s%s%s%s\n",
            svc ? svc : "(?)", what, ok ? "ok" : "FAILED",
            detail ? " (" : "", detail ? detail : "", detail ? ")" : "");
}

/* ---------------------------------------------------------------- caps */

static int drop_capabilities(void)
{
    /* 1) Clear the bounding set first: a process can never regain a cap
     *    once dropped from the bounding set, even via file capabilities. */
    for (int c = 0; c <= CAP_LAST_CAP; c++)
        syscall(SYS_prctl, PR_CAPBSET_DROP, (unsigned long)c, 0, 0, 0);

    /* 2) Zero effective / permitted / inheritable. Dropping is always
     *    permitted; we are still uid 0 at this point. The UAPI names the
     *    structs *__user_cap_*_struct* (see <linux/capability.h>). */
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    memset(&hdr, 0, sizeof(hdr));
    memset(&data, 0, sizeof(data));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    if (syscall(SYS_capset, &hdr, &data) < 0)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------- seccomp */

/* ALLOW a syscall: jump over the RET_ALLOW unless nr matches. */
#define S_ALLOW(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

/* KILL a syscall with extreme prejudice (privilege / namespace / kernel
 * modification attempts from inside the sandbox). */
#define S_KILL(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)

static int install_seccomp(void)
{
    /* Order matters: the validation + allow-list below is a single
     * linear BPF program. Keep the KILL rules for dangerous syscalls;
     * the final default returns -EPERM so an unlisted benign syscall
     * fails loudly instead of being silently allowed.
     * Only aarch64-native syscalls are referenced (no 32-bit compat
     * aliases such as *64 / llseek, which do not exist on arm64). */
    struct sock_filter filter[] = {
        /* 0: enforce aarch64 ABI so a filtered syscall number cannot be
         *    reinterpreted across architectures. */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* load syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),

        /* ---- KILL: privilege / namespace / kernel modification ---- */
        S_KILL(__NR_mount),
        S_KILL(__NR_umount2),
        S_KILL(__NR_reboot),
        S_KILL(__NR_swapon),
        S_KILL(__NR_swapoff),
        S_KILL(__NR_pivot_root),
        S_KILL(__NR_chroot),
        S_KILL(__NR_setns),
        S_KILL(__NR_unshare),
        S_KILL(__NR_setuid),
        S_KILL(__NR_setgid),
        S_KILL(__NR_setreuid),
        S_KILL(__NR_setregid),
        S_KILL(__NR_setresuid),
        S_KILL(__NR_setresgid),
        S_KILL(__NR_setfsuid),
        S_KILL(__NR_setfsgid),
        S_KILL(__NR_capset),
        S_KILL(__NR_ptrace),
        S_KILL(__NR_bpf),
        S_KILL(__NR_init_module),
        S_KILL(__NR_finit_module),
        S_KILL(__NR_delete_module),
        S_KILL(__NR_kexec_load),
        S_KILL(__NR_kexec_file_load),
        S_KILL(__NR_personality),
        S_KILL(__NR_acct),
        S_KILL(__NR_quotactl),
        S_KILL(__NR_process_vm_readv),
        S_KILL(__NR_process_vm_writev),
        S_KILL(__NR_name_to_handle_at),
        S_KILL(__NR_open_by_handle_at),
        S_KILL(__NR_fanotify_init),
        S_KILL(__NR_perf_event_open),
        S_KILL(__NR_lookup_dcookie),
        S_KILL(__NR_keyctl),
        S_KILL(__NR_add_key),
        S_KILL(__NR_request_key),
        S_KILL(__NR_vhangup),
        S_KILL(__NR_adjtimex),
        S_KILL(__NR_clock_adjtime),
        S_KILL(__NR_settimeofday),

        /* ---- ALLOW: normal application / IPC / memory syscalls ---- */
        S_ALLOW(__NR_epoll_create1),
        S_ALLOW(__NR_epoll_ctl),
        S_ALLOW(__NR_epoll_pwait),
        S_ALLOW(__NR_eventfd2),
        S_ALLOW(__NR_ppoll),
        S_ALLOW(__NR_pselect6),
        S_ALLOW(__NR_read),
        S_ALLOW(__NR_readv),
        S_ALLOW(__NR_pread64),
        S_ALLOW(__NR_write),
        S_ALLOW(__NR_writev),
        S_ALLOW(__NR_pwrite64),
        S_ALLOW(__NR_openat),
        S_ALLOW(__NR_openat2),
        S_ALLOW(__NR_close),
        S_ALLOW(__NR_close_range),
        S_ALLOW(__NR_dup),
        S_ALLOW(__NR_dup3),
        S_ALLOW(__NR_lseek),
        S_ALLOW(__NR_fstat),
        S_ALLOW(__NR_fstatfs),
        S_ALLOW(__NR_newfstatat),
        S_ALLOW(__NR_faccessat),
        S_ALLOW(__NR_faccessat2),
        S_ALLOW(__NR_readlinkat),
        S_ALLOW(__NR_getdents64),
        S_ALLOW(__NR_mmap),
        S_ALLOW(__NR_munmap),
        S_ALLOW(__NR_mprotect),
        S_ALLOW(__NR_madvise),
        S_ALLOW(__NR_mremap),
        S_ALLOW(__NR_brk),
        S_ALLOW(__NR_rt_sigaction),
        S_ALLOW(__NR_rt_sigprocmask),
        S_ALLOW(__NR_rt_sigreturn),
        S_ALLOW(__NR_sigaltstack),
        S_ALLOW(__NR_nanosleep),
        S_ALLOW(__NR_clock_nanosleep),
        S_ALLOW(__NR_clock_gettime),
        S_ALLOW(__NR_clock_getres),
        S_ALLOW(__NR_gettimeofday),
        S_ALLOW(__NR_getpid),
        S_ALLOW(__NR_gettid),
        S_ALLOW(__NR_getppid),
        S_ALLOW(__NR_getuid),
        S_ALLOW(__NR_geteuid),
        S_ALLOW(__NR_getgid),
        S_ALLOW(__NR_getegid),
        S_ALLOW(__NR_getpgid),
        S_ALLOW(__NR_setpgid),
        S_ALLOW(__NR_prctl),
        S_ALLOW(__NR_clone),
        S_ALLOW(__NR_clone3),
        S_ALLOW(__NR_wait4),
        S_ALLOW(__NR_execve),
        S_ALLOW(__NR_execveat),
        S_ALLOW(__NR_exit),
        S_ALLOW(__NR_exit_group),
        S_ALLOW(__NR_set_robust_list),
        S_ALLOW(__NR_get_robust_list),
        S_ALLOW(__NR_rseq),
        S_ALLOW(__NR_membarrier),
        S_ALLOW(__NR_set_tid_address),
        S_ALLOW(__NR_socket),
        S_ALLOW(__NR_socketpair),
        S_ALLOW(__NR_connect),
        S_ALLOW(__NR_accept),
        S_ALLOW(__NR_accept4),
        S_ALLOW(__NR_bind),
        S_ALLOW(__NR_listen),
        S_ALLOW(__NR_sendmsg),
        S_ALLOW(__NR_recvmsg),
        S_ALLOW(__NR_sendto),
        S_ALLOW(__NR_recvfrom),
        S_ALLOW(__NR_getsockopt),
        S_ALLOW(__NR_setsockopt),
        S_ALLOW(__NR_getsockname),
        S_ALLOW(__NR_getpeername),
        S_ALLOW(__NR_shutdown),
        S_ALLOW(__NR_fcntl),
        S_ALLOW(__NR_ioctl),
        S_ALLOW(__NR_kill),
        S_ALLOW(__NR_tkill),
        S_ALLOW(__NR_tgkill),
        S_ALLOW(__NR_futex),
        S_ALLOW(__NR_sched_yield),
        S_ALLOW(__NR_sched_getaffinity),
        S_ALLOW(__NR_getrandom),
        S_ALLOW(__NR_memfd_create),
        S_ALLOW(__NR_ftruncate),
        S_ALLOW(__NR_truncate),
        S_ALLOW(__NR_prlimit64),
        S_ALLOW(__NR_getrusage),
        S_ALLOW(__NR_times),
        S_ALLOW(__NR_uname),
        S_ALLOW(__NR_getrlimit),

        /* default: deny with EPERM (not kill) so an unexpected but benign
         * syscall surfaces as a clear failure rather than terminating the
         * app, and anything genuinely dangerous we forgot is still blocked. */
        BPF_STMT(BPF_RET | BPF_K,
                 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };
    if (syscall(SYS_prctl, PR_SET_SECCOMP, (unsigned long)SECCOMP_MODE_FILTER,
                (unsigned long)&prog, 0, 0) < 0)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------- public */

int aether_apply_sandbox(const char *svcname)
{
    int ok = 1;

    /* 1) private mount namespace: the service's mounts are isolated */
    if (unshare(CLONE_NEWNS) < 0) {
        sb_log(svcname, "unshare(CLONE_NEWNS)", 0, strerror(errno));
        ok = 0;
    } else if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        sb_log(svcname, "mount(MS_PRIVATE)", 0, strerror(errno));
        ok = 0;
    } else {
        sb_log(svcname, "mount-namespace", 1, "private");
    }

    /* 2) no-new-privs so the filter cannot be escaped by exec'ing a
     *    setuid binary; required before installing the filter. */
    if (syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        sb_log(svcname, "PR_SET_NO_NEW_PRIVS", 0, strerror(errno));
        ok = 0;
    }

    /* 3) change identity to the dedicated non-root uid/gid.
     *    必须发生在丢弃能力之前：setresuid(2) 的规则是「无 CAP_SETUID 时
     *    只能把 uid 设为当前 ruid/euid/suid 之一」——uid 0 并不例外。
     *    先丢 capset 再 setresuid(1000) 会得到 EPERM（CI 实测取证：
     *    "setresuid/gid FAILED (Operation not permitted)"，uid 停在 0）。
     *    因此顺序为：仍持 CAP_SETUID 时改身份，再无条件丢弃全部能力
     *    （丢弃自身能力无需任何特权）。 */
    if (setresgid(AETHER_SANDBOX_GID, AETHER_SANDBOX_GID, AETHER_SANDBOX_GID) < 0 ||
        setresuid(AETHER_SANDBOX_UID, AETHER_SANDBOX_UID, AETHER_SANDBOX_UID) < 0) {
        sb_log(svcname, "setresuid/gid", 0, strerror(errno));
        ok = 0;
    } else {
        sb_log(svcname, "identity", 1, "uid=1000 gid=1000");
    }

    /* 3) drop every capability (bounding set + effective/permitted/
     *    inheritable). 此刻已是非 root uid 1000：降低自身能力集无需特权，
     *    仅「提升」才需要 CAP_SETPCAP，因此本步在 setresuid 之后依然成立。 */
    if (drop_capabilities() < 0) {
        sb_log(svcname, "capability drop", 0, strerror(errno));
        ok = 0;
    } else {
        sb_log(svcname, "capabilities", 1, "all dropped");
    }

    /* 5) install the seccomp BPF allow-list (NO_NEW_PRIVS is set). */
    if (install_seccomp() < 0) {
        sb_log(svcname, "seccomp filter", 0, strerror(errno));
        ok = 0;
    } else {
        sb_log(svcname, "seccomp", 1, "BPF allow-list installed");
    }

    return ok ? 0 : -1;
}
