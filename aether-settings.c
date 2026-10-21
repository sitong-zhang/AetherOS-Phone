/*
 * aether-settings.c - AetherOS「设置」应用：真实系统读数面板。
 *
 * 与 demo / clock 一样是 compositor 的独立客户端进程：经 UDS 注册窗口，
 * SCM_RIGHTS 共享 memfd 承载像素，自绘后 commit 上屏。带 chrome 的普通
 * 窗口（非 fs）——应用窗口保持桌面形态，SystemUI 才走全屏系统层。
 *
 * 铁律：所有读数来自真实内核/系统文件，读不到显 "n/a"，绝不编造。
 *   内核版本 : /proc/sys/kernel/osrelease
 *   主机名   : gethostname(2)
 *   uptime   : /proc/uptime
 *   内存     : /proc/meminfo（Total/Available/Free）
 *   负载     : /proc/loadavg（1/5/15 分钟）
 *   CPU      : /proc/cpuinfo processor 行计数
 *   服务     : /run/aether-services.state（init 快照，running/total）
 *   电量     : /run/aether-hal-android.state（Android 宿主 HAL；未接入显 NONE）
 *   根分区   : statvfs("/")（真实块设备用量）
 *
 * 刷新：2s 一轮，只在内容变化时 commit（uptime 秒位每秒变，因此实际每
 * 秒左右一帧；无测试依赖本应用的帧哈希——J9 冻结的是 SystemUI）。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "aether-render.h"

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

#define SOCK_PATH  "/run/aetheros-comp.sock"
#define SVC_STATE  "/run/aether-services.state"
#define HAL_STATE  "/run/aether-hal-android.state"

#define CW 480
#define CH 320
#define ROW_H 22
#define TOP 14

#define FG_MAIN  0xE6EDF3u
#define FG_DIM   0x8B98A5u
#define FG_ACCENT 0x4FC3F7u

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

/* ---------------------------------------------------------------- IPC 客户端 */

static int connect_comp(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_comp_retry(void)
{
    for (int i = 0; i < 40 && !stop_flag; i++) {
        int s = connect_comp();
        if (s >= 0) return s;
        usleep(250000);
    }
    return -1;
}

static int recv_line(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    while (off < cap - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) break;
        if (c == '\n' || c == '\r') break;
        buf[off++] = c;
    }
    buf[off] = '\0';
    return off > 0 ? 0 : -1;
}

static int cmd_create(int *id_out)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char cmd[128];
    snprintf(cmd, sizeof cmd, "create aether-settings %d %d", CW, CH);
    if (write(s, cmd, strlen(cmd)) < 0) { close(s); return -1; }
    char resp[256];
    if (recv_line(s, resp, sizeof resp) < 0) { close(s); return -1; }
    const char *p = strstr(resp, "\"id\":");
    int id = p ? atoi(p + 5) : -1;
    close(s);
    *id_out = id;
    return id >= 0 ? 0 : -1;
}

static int cmd_buf(int id, int memfd)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char line[64];
    int n = snprintf(line, sizeof line, "buf %d\n", id);
    struct iovec iov = { .iov_base = line, .iov_len = (size_t)n };
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &memfd, sizeof(int));
    int rc = sendmsg(s, &msg, 0);
    char resp[256];
    recv_line(s, resp, sizeof resp);
    close(s);
    if (rc <= 0) return -1;
    return strstr(resp, "\"ok\":true") ? 0 : -1;
}

static int cmd_commit(int id)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char cmd[64];
    snprintf(cmd, sizeof cmd, "commit %d", id);
    if (write(s, cmd, strlen(cmd)) < 0) { close(s); return -1; }
    char resp[256];
    recv_line(s, resp, sizeof resp);
    close(s);
    return 0;
}

/* ---------------------------------------------------------------- 真实读数 */

static void fmt_uptime(double up, char *out, size_t n)
{
    if (up < 0) { snprintf(out, n, "n/a"); return; }
    long s = (long)up;
    long d = s / 86400; s %= 86400;
    long h = s / 3600;  s %= 3600;
    long m = s / 60;    s %= 60;
    if (d > 0) snprintf(out, n, "%ldd %ldh %ldm %lds", d, h, m, s);
    else if (h > 0) snprintf(out, n, "%ldh %ldm %lds", h, m, s);
    else snprintf(out, n, "%ldm %lds", m, s);
}

struct meminfo { long total_kb, avail_kb, free_kb; };

static int read_meminfo(struct meminfo *mi)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    mi->total_kb = mi->avail_kb = mi->free_kb = -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MemTotal:", 9))      sscanf(line + 9, "%ld", &mi->total_kb);
        else if (!strncmp(line, "MemAvailable:", 13)) sscanf(line + 13, "%ld", &mi->avail_kb);
        else if (!strncmp(line, "MemFree:", 9))  sscanf(line + 9, "%ld", &mi->free_kb);
    }
    fclose(f);
    return (mi->total_kb > 0) ? 0 : -1;
}

static int read_cpu_count(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return -1;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "processor", 9)) n++;
    fclose(f);
    return n > 0 ? n : -1;
}

static void read_loadavg(char *out, size_t n)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) { snprintf(out, n, "n/a"); return; }
    double l1 = 0, l5 = 0, l15 = 0;
    if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3)
        snprintf(out, n, "%.2f / %.2f / %.2f", l1, l5, l15);
    else
        snprintf(out, n, "n/a");
    fclose(f);
}

static void read_services(char *out, size_t n)
{
    int r = 0, t = 0;
    FILE *f = fopen(SVC_STATE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char name[64]; int pid; char st[32];
            if (sscanf(line, "%63s %d %31s", name, &pid, st) == 3) {
                t++;
                if (!strcmp(st, "running")) r++;
            }
        }
        fclose(f);
    }
    if (t > 0) snprintf(out, n, "%d running / %d total", r, t);
    else snprintf(out, n, "n/a");
}

static void read_battery(char *out, size_t n)
{
    FILE *f = fopen(HAL_STATE, "r");
    if (!f) { snprintf(out, n, "NONE (no host HAL)"); return; }
    char line[128];
    int pct = -1, chg = 0, backend_ok = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "battery_pct=", 12)) pct = atoi(line + 12);
        else if (!strncmp(line, "charging=", 9)) chg = atoi(line + 9);
        else if (!strncmp(line, "backend=", 8)) backend_ok = 1;
    }
    fclose(f);
    if (pct >= 0)
        snprintf(out, n, "%d%%%s (REAL)", pct, chg ? " charging" : "");
    else if (backend_ok)
        snprintf(out, n, "NONE (host HAL idle)");
    else
        snprintf(out, n, "NONE (no host HAL)");
}

static void read_rootfs(char *out, size_t n)
{
    struct statvfs vfs;
    if (statvfs("/", &vfs) != 0) { snprintf(out, n, "n/a"); return; }
    unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    snprintf(out, n, "%llu MB total / %llu MB free",
             total >> 20, avail >> 20);
}

/* ---------------------------------------------------------------- 绘制 */

static void draw_ui(uint32_t *px)
{
    /* 与 SystemUI 相同的约定：软件光栅化器直接画进客户端缓冲
     * （CW x CH，pitch = CW*4）——合成器再把整块 blit 进窗口内容区。 */
    aether_fb_t fb;
    aether_render_init_fb(&fb, px, CW, CH, CW * 4);

    for (int i = 0; i < CW * CH; i++)
        px[i] = 0xFF141A22u;

    /* 标题行（青色下划线） */
    const char *title = "Settings";
    aether_render_rect(&fb, 10, TOP + 18, CW - 20, 1, FG_ACCENT);

    char kernel[128] = "n/a";
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        if (fgets(kernel, sizeof kernel, f)) {
            char *nl = strpbrk(kernel, "\r\n");
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
    char host[128] = "n/a";
    if (gethostname(host, sizeof host) != 0)
        snprintf(host, sizeof host, "n/a");

    double up = -1.0;
    f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = -1.0; fclose(f); }
    char upstr[64];
    fmt_uptime(up, upstr, sizeof upstr);

    struct meminfo mi;
    char memstr[96];
    if (read_meminfo(&mi) == 0)
        snprintf(memstr, sizeof memstr, "%ld MB total / %ld MB avail / %ld MB free",
                 mi.total_kb >> 10, mi.avail_kb >> 10, mi.free_kb >> 10);
    else
        snprintf(memstr, sizeof memstr, "n/a");

    char loadstr[64];
    read_loadavg(loadstr, sizeof loadstr);

    int cpus = read_cpu_count();
    char cpustr[64];
    if (cpus > 0) snprintf(cpustr, sizeof cpustr, "%d", cpus);
    else snprintf(cpustr, sizeof cpustr, "n/a");

    char svcstr[64];
    read_services(svcstr, sizeof svcstr);

    char batstr[80];
    read_battery(batstr, sizeof batstr);

    char fsstr[96];
    read_rootfs(fsstr, sizeof fsstr);

    char rows[10][200];
    snprintf(rows[0], sizeof rows[0], "kernel   %s", kernel);
    snprintf(rows[1], sizeof rows[1], "hostname %s", host);
    snprintf(rows[2], sizeof rows[2], "uptime   %s", upstr);
    snprintf(rows[3], sizeof rows[3], "memory   %s", memstr);
    snprintf(rows[4], sizeof rows[4], "loadavg  %s", loadstr);
    snprintf(rows[5], sizeof rows[5], "cpus     %s", cpustr);
    snprintf(rows[6], sizeof rows[6], "services %s", svcstr);
    snprintf(rows[7], sizeof rows[7], "battery  %s", batstr);
    snprintf(rows[8], sizeof rows[8], "rootfs   %s", fsstr);
    snprintf(rows[9], sizeof rows[9], "display  compositor client window (XRGB8888 shm)");

    /* 8x16 ASCII 字模（aether-render 的统一文本入口） */
    aether_render_text(&fb, 10, TOP, title, FG_MAIN);
    for (int i = 0; i < 10; i++)
        aether_render_text(&fb, 10, TOP + ROW_H * (i + 1),
                           rows[i], i == 9 ? FG_DIM : FG_MAIN);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int id;
    if (cmd_create(&id) < 0) {
        fprintf(stderr, "settings: create failed\n");
        return 1;
    }
    fprintf(stderr, "settings: window id=%d\n", id);

    int memfd = memfd_create("aether-settings", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (memfd < 0) {
        perror("settings: memfd_create");
        return 1;
    }
    if (ftruncate(memfd, (off_t)CW * CH * 4) < 0) {
        perror("settings: ftruncate");
        close(memfd);
        return 1;
    }
    uint32_t *px = mmap(NULL, (size_t)CW * CH * 4, PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (px == MAP_FAILED) {
        perror("settings: mmap");
        close(memfd);
        return 1;
    }

    /* 首帧上屏 */
    draw_ui(px);
    if (cmd_buf(id, memfd) < 0) {
        fprintf(stderr, "settings: buf failed\n");
        return 1;
    }
    cmd_commit(id);

    /* 2s 刷新一轮（uptime 秒位每约 1s 变一次，读取本身也推进） */
    while (!stop_flag) {
        for (int i = 0; i < 20 && !stop_flag; i++)
            usleep(100000);   /* 100ms x 20 = 2s，可被信号及时打断 */
        if (stop_flag)
            break;
        draw_ui(px);
        if (cmd_buf(id, memfd) < 0)
            break;              /* 窗口没了（合成器重启）：安静退出，由启动器重拉 */
        cmd_commit(id);
    }

    munmap(px, (size_t)CW * CH * 4);
    close(memfd);
    fprintf(stderr, "settings: exiting\n");
    return 0;
}
