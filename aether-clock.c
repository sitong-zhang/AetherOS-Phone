/*
 * aether-clock.c - AetherOS「时钟」应用：大字号时钟 + 中文日期。
 *
 * 与 demo / settings 一样是 compositor 的独立客户端进程：UDS 注册窗口，
 * SCM_RIGHTS 共享 memfd 承载像素，自绘后 commit 上屏。普通窗口（带
 * chrome）——应用窗口保持桌面形态；全屏系统层是 SystemUI 的专属。
 *
 * 铁律：时间取 time(2) + localtime_r 真墙钟；客机无 RTC 时区配置，按
 * UTC 显示（与 SystemUI 状态条同一约定，不伪造时区）。
 *
 * 刷新：秒位变化即重绘（约 1s 一帧）。无测试依赖本应用的帧哈希
 * （J9 冻结的是 SystemUI 的定时重绘）。
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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "aether-render.h"

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

#define SOCK_PATH "/run/aetheros-comp.sock"

#define CW 360
#define CH 180

#define FG_MAIN   AETHER_RGB(0xE6, 0xED, 0xF3)
#define FG_DIM    AETHER_RGB(0x8B, 0x98, 0xA5)
#define FG_ACCENT AETHER_RGB(0x4F, 0xC3, 0xF7)

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
    snprintf(cmd, sizeof cmd, "create aether-clock %d %d", CW, CH);
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

/* ---------------------------------------------------------------- 绘制 */

static void draw_ui(uint32_t *px, const struct tm *tmv)
{
    aether_fb_t fb;
    aether_render_init_fb(&fb, px, CW, CH, CW * 4);

    for (int i = 0; i < CW * CH; i++)
        px[i] = 0xFF141A22u;

    /* 大时钟 HH:MM（4x 字模，160x64）居中 */
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", tmv->tm_hour, tmv->tm_min);
    aether_render_text_scaled(&fb, (CW - 160) / 2, 22, clock, FG_MAIN, 4);

    /* 中文日期（YYYY年MM月DD日） */
    char date[48];
    snprintf(date, sizeof date, "%04d\u5e74%02d\u6708%02d\u65e5",
             tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday);
    aether_render_text_utf8(&fb, (CW - 224) / 2, 104, date, FG_DIM);

    /* 秒进度条（真实秒位，1/60 宽度） */
    int bar_w = (CW - 40) * tmv->tm_sec / 59;
    if (bar_w < 1) bar_w = 1;
    aether_render_rect(&fb, 20, 136, CW - 40, 4, AETHER_RGB(0x2A, 0x33, 0x40));
    aether_render_rect(&fb, 20, 136, bar_w, 4, FG_ACCENT);

    /* 角标 */
    aether_render_text(&fb, 12, 156, "AetherOS Clock", FG_DIM);
    char sec[16];
    snprintf(sec, sizeof sec, "%02ds", tmv->tm_sec);
    aether_render_text(&fb, CW - 12 - 5 * 8, 156, sec, FG_DIM);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int id;
    if (cmd_create(&id) < 0) {
        fprintf(stderr, "clock: create failed\n");
        return 1;
    }
    fprintf(stderr, "clock: window id=%d\n", id);

    int memfd = memfd_create("aether-clock", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (memfd < 0) {
        perror("clock: memfd_create");
        return 1;
    }
    if (ftruncate(memfd, (off_t)CW * CH * 4) < 0) {
        perror("clock: ftruncate");
        close(memfd);
        return 1;
    }
    uint32_t *px = mmap(NULL, (size_t)CW * CH * 4, PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (px == MAP_FAILED) {
        perror("clock: mmap");
        close(memfd);
        return 1;
    }

    /* 首帧上屏 */
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    draw_ui(px, &tmv);
    if (cmd_buf(id, memfd) < 0) {
        fprintf(stderr, "clock: buf failed\n");
        return 1;
    }
    cmd_commit(id);

    int last_sec = tmv.tm_sec;
    while (!stop_flag) {
        usleep(200000);   /* 200ms 轮询：秒位变化即重绘 */
        t = time(NULL);
        localtime_r(&t, &tmv);
        if (tmv.tm_sec == last_sec)
            continue;
        last_sec = tmv.tm_sec;
        draw_ui(px, &tmv);
        if (cmd_buf(id, memfd) < 0)
            break;          /* 窗口没了（合成器重启）：安静退出，由启动器重拉 */
        cmd_commit(id);
    }

    munmap(px, (size_t)CW * CH * 4);
    close(memfd);
    fprintf(stderr, "clock: exiting\n");
    return 0;
}
