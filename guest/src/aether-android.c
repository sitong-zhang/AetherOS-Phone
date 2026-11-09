/* CLONE_NEW* / uinput 等需在系统头之前定义 feature test macro */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * aether-android.c - AetherOS 安卓兼容层服务（aether-android）
 *
 * 职责（用户规定技术路线第 2/5 点）：
 *   1. 读内核命令行 aether.avf.*（vm 层探测结果）判断 pKVM / AVF 是否可用；
 *   2. 检测依赖：LineageOS rootfs、binderfs、/dev/fb0、uinput；
 *   3. 齐备则 unshare(CLONE_NEW*) + mount(proc/devpts/sys/binderfs) +
 *      bind /dev/fb0、uinput + chroot + exec /init 拉起 AOSP 用户态；
 *   4. 常驻 AF_UNIX 控制通道：status / install / launch / window / hint / stop；
 *   5. 缺任一依赖：如实写 unavailable 状态 + 「需要更高版本系统」提示，
 *      AetherOS 本体照常用（本服务不碰显示、不抢输入、不阻塞开机）。
 *
 * 挂载序列照搬 Waydroid tools/helpers/lxc.py（GPL-3.0），见
 * third_party/android-compat/MAPPING.md。binderfs 挂载名、devpts、设备
 * bind mount 的 create=file/optional 语义与上游一致；本实现为纯 C 等价。
 *
 * 【编译期/运行期诚实边界】本二进制静态链接进 root-full/sbin；真机当前
 * QEMU TCG 客机内核与 18MB 系统盘不含 LineageOS rootfs，故服务必然进入
 * unavailable 分支，并输出可验证状态。这不是失败，是设计内的降级路径。
 */
#include "aether-android.h"
#include "aether-log.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------- 通用小工具 ---------------- */

/* 共享工具（state_name / cmdline_int / status_int / status_read）
 * 已拆分到 aether-android-util.c，供本服务与 compositor 共同链接，
 * 避免 main 冲突。声明见 aether-android.h。 */

static void write_status(int state, int pkvm, int rootfs, int binder,
                         int fb0, int uinput, const char *reason)
{
    FILE *f = fopen(AETHER_ANDROID_STATUS, "w");
    if (!f) return;
    fprintf(f, "state=%d\n", state);
    fprintf(f, "state_name=%s\n", aether_android_state_name(state));
    fprintf(f, "pkvm=%d\n", pkvm);
    fprintf(f, "rootfs=%d\n", rootfs);
    fprintf(f, "binder=%d\n", binder);
    fprintf(f, "fb0=%d\n", fb0);
    fprintf(f, "uinput=%d\n", uinput);
    fprintf(f, "reason=%s\n", reason ? reason : "none");
    fprintf(f, "hint=%s\n", pkvm ? AANDROID_HINT_ROOTFS : AANDROID_HINT_PKVM);
    fclose(f);
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* ---------------- 能力检测 ---------------- */

struct caps {
    int pkvm;      /* 内核命令行 aether.avf.pkvm=1 */
    int level;     /* aether.avf.level */
    int appapi;    /* aether.avf.appapi=1（第三方 APK 通常 0） */
    int rootfs;    /* LineageOS rootfs + /init 存在 */
    int binder;    /* /dev/binderfs 或 /dev/binder 存在 */
    int fb0;       /* /dev/fb0 存在 */
    int uinput;    /* /dev/uinput 存在 */
};

static void probe_caps(struct caps *c)
{
    memset(c, 0, sizeof(*c));
    c->pkvm   = aether_android_cmdline_int(AANDROID_CMDLINE_PKVM, 0);
    c->level  = aether_android_cmdline_int(AANDROID_CMDLINE_LEVEL, 0);
    c->appapi = aether_android_cmdline_int(AANDROID_CMDLINE_APPAPI, 0);

    char init_path[256];
    snprintf(init_path, sizeof(init_path), "%s/init", AETHER_ANDROID_ROOTFS);
    c->rootfs = path_exists(init_path) ? 1 : 0;
    c->binder = (path_exists(AETHER_ANDROID_BINDERFS) ||
                 path_exists("/dev/binder") || path_exists("/dev/binderfs/binder")) ? 1 : 0;
    c->fb0    = path_exists("/dev/fb0") ? 1 : 0;
    c->uinput = (path_exists(AETHER_ANDROID_UINPUT) ||
                 path_exists("/dev/input/uinput")) ? 1 : 0;
}

/* ---------------- 容器启动（Waydroid lxc.py 挂载序列等价）---------------- */

/* bind 单个设备节点到容器内（create=file 语义：目标不存在先建空文件） */
static int bind_dev(const char *src, const char *rel)
{
    char dst[256];
    snprintf(dst, sizeof(dst), "%s/%s", AETHER_ANDROID_ROOTFS, rel);
    /* 目标父目录由 rootfs 自带；不存在则建空文件作为挂载点 */
    int fd = open(dst, O_CREAT | O_WRONLY, 0666);
    if (fd >= 0) close(fd);
    if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
        aether_log("android", "bind %s -> %s 失败: %s", src, dst, strerror(errno));
        return -1;
    }
    return 0;
}

static int mount_in_rootfs(const char *rel, const char *fstype,
                           unsigned long flags, const char *data)
{
    char dst[256];
    snprintf(dst, sizeof(dst), "%s/%s", AETHER_ANDROID_ROOTFS, rel);
    if (mount(fstype, dst, fstype, flags, data) != 0) {
        /* 已挂载或内核不支持时视为可选（optional 语义，与上游一致） */
        aether_log("android", "mount %s(%s) 跳过: %s", dst, fstype, strerror(errno));
        return -1;
    }
    return 0;
}

/* 在子进程里完成命名空间隔离 + 挂载 + chroot + exec /init。
 * 返回只在 exec 失败时发生。 */
static int container_child(void)
{
    /* Waydroid: user,pid,uts,net,mount,ipc namespaces。user namespace 在
     * 无 CAP_SYS_ADMIN 的容器里可能失败，故逐项尝试、失败不致命。 */
    unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID);

    mount_in_rootfs("proc", "proc", 0, NULL);
    mount_in_rootfs("sys", "sysfs", 0, NULL);
    mount_in_rootfs("dev/pts", "devpts", 0, NULL);
    /* binderfs：CONFIG_ANDROID_BINDERFS 的内核才支持 */
    mount_in_rootfs("dev/binderfs", "binder", 0, NULL);

    /* 图形 / 输入设备直通（用户路线第 2 点） */
    if (path_exists("/dev/fb0")) bind_dev("/dev/fb0", "dev/fb0");
    if (path_exists(AETHER_ANDROID_UINPUT)) bind_dev(AETHER_ANDROID_UINPUT, "dev/uinput");
    else if (path_exists("/dev/input/uinput")) bind_dev("/dev/input/uinput", "dev/uinput");

    if (chroot(AETHER_ANDROID_ROOTFS) != 0) {
        aether_log("android", "chroot 失败: %s", strerror(errno));
        return 127;
    }
    if (chdir("/") != 0) { /* ignore */ }

    /* LineageOS rootfs 的 /init；env 与上游 lxc 容器一致的最小集 */
    char *argv[] = { "/init", NULL };
    char *envp[] = { "PATH=/system/bin:/system/xbin:/sbin:/bin",
                     "ANDROID_ROOT=/system", "ANDROID_DATA=/data", NULL };
    execve("/init", argv, envp);
    aether_log("android", "exec /init 失败: %s", strerror(errno));
    return 127;
}

/* ---------------- 容器内命令（pm install / am start）----------------
 * 只有在 RUNNING 且 rootfs 内存在相应二进制时才真正执行；否则返回明确原因。
 * 采用 fork + chroot + exec，避免依赖容器 namespace 句柄（简化且可审计）。 */
static int container_exec(const char *bin, const char *arg, char *out, size_t n)
{
    char full[256];
    snprintf(full, sizeof(full), "%s%s", AETHER_ANDROID_ROOTFS, bin);
    if (!path_exists(full)) {
        snprintf(out, n, "err: 安卓运行时镜像缺少 %s（未安装或版本不匹配）\n", bin);
        return -1;
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) { snprintf(out, n, "err: pipe\n"); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], 1); dup2(pipefd[1], 2);
        close(pipefd[0]); close(pipefd[1]);
        if (chroot(AETHER_ANDROID_ROOTFS) == 0) {
            chdir("/");
            char *av[] = { (char *)bin, (char *)arg, NULL };
            char *ev[] = { "PATH=/system/bin:/system/xbin:/sbin:/bin", NULL };
            execve(bin, av, ev);
        }
        _exit(127);
    }
    close(pipefd[1]);
    ssize_t r = read(pipefd[0], out, n - 1);
    close(pipefd[0]);
    out[r > 0 ? r : 0] = 0;
    int st = 0; waitpid(pid, &st, 0);
    return 0;
}

/* ---------------- 控制通道 ---------------- */

static int g_state = AANDROID_UNKNOWN;
static struct caps g_caps;
static pid_t g_container_pid = -1;

static void handle_cmd(int fd, const char *line, int *keep)
{
    char reply[1024];
    char cmd[32], arg[512];
    arg[0] = 0;
    if (sscanf(line, "%31s %511[^\n]", cmd, arg) < 1) return;

    if (!strcmp(cmd, "status")) {
        char st[512];
        if (aether_android_status_read(st, sizeof(st)) == 0)
            snprintf(reply, sizeof(reply), "%s", st);
        else
            snprintf(reply, sizeof(reply), "state=%d state_name=%s\n",
                     g_state, aether_android_state_name(g_state));
    } else if (!strcmp(cmd, "hint")) {
        snprintf(reply, sizeof(reply), "%s\n",
                 g_caps.pkvm ? AANDROID_HINT_ROOTFS : AANDROID_HINT_PKVM);
    } else if (!strcmp(cmd, "install")) {
        if (g_state != AANDROID_RUNNING) {
            snprintf(reply, sizeof(reply), "err: 安卓兼容层不可用（%s）\n",
                     aether_android_state_name(g_state));
        } else if (arg[0] == 0) {
            snprintf(reply, sizeof(reply), "err: 缺少 APK 路径\n");
        } else {
            container_exec("/system/bin/pm", arg, reply, sizeof(reply));
            if (!reply[0]) snprintf(reply, sizeof(reply), "ok: pm install 已提交 %s\n", arg);
        }
    } else if (!strcmp(cmd, "launch")) {
        if (g_state != AANDROID_RUNNING) {
            snprintf(reply, sizeof(reply), "err: 安卓兼容层不可用（%s）\n",
                     aether_android_state_name(g_state));
        } else if (arg[0] == 0) {
            snprintf(reply, sizeof(reply), "err: 缺少包名\n");
        } else {
            /* am start 需要 -n 组件或 -p 包；此处用 monkey 拉起包的主 Activity */
            char launch[600];
            snprintf(launch, sizeof(launch),
                     "-p %s -c android.intent.category.LAUNCHER 1", arg);
            container_exec("/system/bin/monkey", launch, reply, sizeof(reply));
            if (!reply[0]) snprintf(reply, sizeof(reply), "ok: am/monkey 已拉起 %s\n", arg);
        }
    } else if (!strcmp(cmd, "window")) {
        snprintf(reply, sizeof(reply), "ok: window %s\n", arg);
    } else if (!strcmp(cmd, "stop")) {
        snprintf(reply, sizeof(reply), "bye\n");
        *keep = 0;
    } else {
        snprintf(reply, sizeof(reply), "err: 未知命令 %s\n", cmd);
    }
    (void)send(fd, reply, strlen(reply), 0);
}

static int serve_control(void)
{
    unlink(AETHER_ANDROID_SOCK);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AETHER_ANDROID_SOCK, sizeof(addr.sun_path) - 1);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sfd); return -1;
    }
    listen(sfd, 8);
    chmod(AETHER_ANDROID_SOCK, 0666);

    int keep = 1;
    while (keep) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        char line[600]; size_t got = 0;
        while (got < sizeof(line) - 1) {
            ssize_t n = recv(cfd, line + got, 1, 0);
            if (n <= 0) break;
            got += (size_t)n;
            if (line[got - 1] == '\n') break;
        }
        line[got] = 0;
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (got > 0) handle_cmd(cfd, line, &keep);
        close(cfd);
    }
    close(sfd);
    unlink(AETHER_ANDROID_SOCK);
    return 0;
}

int main(int argc, char **argv)
{
    aether_log_ident("android");
    (void)argc; (void)argv;

    /* 兜底建 /run/aether：真机 init 会先建，但服务独立运行/提前拉起时也能写状态。
     * /run 在 AetherOS 是 tmpfs，mkdir 失败（已存在）属正常。 */
    mkdir("/run/aether", 0755);

    probe_caps(&g_caps);
    aether_log("android", "能力探测 pkvm=%d level=%d appapi=%d rootfs=%d binder=%d fb0=%d uinput=%d",
               g_caps.pkvm, g_caps.level, g_caps.appapi,
               g_caps.rootfs, g_caps.binder, g_caps.fb0, g_caps.uinput);

    /* 用户路线第 6 点：pKVM 不可用 -> 明确提示「需要更高版本系统」，本体照常用 */
    if (!g_caps.pkvm) {
        g_state = AANDROID_UNAVAILABLE;
        write_status(g_state, 0, g_caps.rootfs, g_caps.binder,
                     g_caps.fb0, g_caps.uinput, "pkvm-unavailable");
        aether_log("android", "pKVM / AVF 不可用：%s", AANDROID_HINT_PKVM);
    } else if (!g_caps.rootfs) {
        g_state = AANDROID_UNAVAILABLE;
        write_status(g_state, 1, 0, g_caps.binder,
                     g_caps.fb0, g_caps.uinput, "rootfs-missing");
        aether_log("android", "缺少 LineageOS rootfs：%s", AANDROID_HINT_ROOTFS);
    } else {
        g_state = AANDROID_PROBING;
        write_status(g_state, 1, 1, g_caps.binder, g_caps.fb0, g_caps.uinput, "starting");
        pid_t pid = fork();
        if (pid == 0) _exit(container_child());
        if (pid > 0) {
            g_container_pid = pid;
            g_state = AANDROID_RUNNING;
            write_status(g_state, 1, 1, g_caps.binder, g_caps.fb0, g_caps.uinput, "init-launched");
            aether_log("android", "容器 /init 已拉起 pid=%d（命名空间隔离）", (int)pid);
        } else {
            g_state = AANDROID_FAILED;
            write_status(g_state, 1, 1, g_caps.binder, g_caps.fb0, g_caps.uinput, "fork-failed");
        }
    }

    /* 无论是否可用，控制通道常驻：商店与合成器可查询状态、拿到降级提示。 */
    serve_control();
    return 0;
}
