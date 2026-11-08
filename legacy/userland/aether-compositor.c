/*
 * aether-compositor.c - AetherOS window manager and compositor.
 *
 * Owns the display (aether-drm), the window table, and the IPC socket
 * (/run/aetheros-comp.sock). Modeled on aetherosd's service contract:
 * one line of JSON per request, socket mode 0660.
 *
 * Usage:
 *   aether-compositor            run as daemon (aether-init service)
 *   aether-compositor -c CMD     act as IPC client (aethercompctl link)
 *
 * IPC commands (schema aethercomp.state/1):
 *   status | list | hash | screenshot <path>
 *   open <title>                 open a server-rendered window (default size)
 *   create <title> <w> <h> [fs] [sys] [sock=<uds>]
 *                                 open a client window of content size w x h.
 *                                 fs  = fullscreen surface (no chrome; the
 *                                       client buffer covers the window)
 *                                 sys = system layer (renders BELOW normal
 *                                       windows — the home/lock surface —
 *                                       and gets taps only when no normal
 *                                       window covers the point)
 *                                 sock = optional UDS path that receives
 *                                       forwarded taps ("tap <x> <y>\n"
 *                                       lines, window-local coords) — P5:
 *                                       lets ordinary app windows have
 *                                       clickable buttons (System Update)
 *   close <id> | focus <id>
 *   buf <id>  + SCM_RIGHTS fd    attach a memfd pixel buffer (XRGB8888) to id
 *   commit <id>                  client finished drawing, request repaint
 *
 * Keyboard (optional, /dev/input/event*): Tab cycles focus, Esc closes
 * the focused window. A tablet / absolute pointer (optional) is routed as
 * taps to the topmost window under the point — a tap on the SystemUI
 * surface is forwarded as a `tap` IPC so SystemUI can hit-test (lock
 * screen -> unlock, launcher rows -> launch app). Every tap also raises a
 * short-lived touch dot + interactive glass highlight (P4-1). Everything
 * is also reachable through IPC, so the smoke tests do not depend on evdev.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/input.h>

#include "aether-drm.h"
#include "aether-render.h"

#define SOCK_PATH "/run/aetheros-comp.sock"
#define AETHER_WALLPAPER "/usr/share/aether/wallpaper.ppm"
#define SYSUI_SOCK "/run/aetheros-sysui.sock"
#define SCHEMA "aethercomp.state/1"
#define MAX_WINDOWS 16
#define IPC_LINE_MAX 512

static aether_drm_t drm;
static aether_fb_t fb;
static aether_win_t wins[MAX_WINDOWS];
static int win_count;
static int focused_idx;
static int dirty = 1;
static volatile sig_atomic_t stop_flag;

/* P4-1: 交互态（上游 InteractiveHighlight / chromaticAberration-on-press）。
 * 每次触摸置位，~400ms 内衰减；衰减期间主循环以 50ms 超时 poll 并持续
 * 重绘，实现"手指按下去玻璃起液相反应、松开慢慢平复"。 */
static int  g_press_x, g_press_y;
static long g_press_until_ms;         /* monotonic ms; 0 = 未激活 */
#define PRESS_MS 400
static int  g_touch_x, g_touch_y;
static long g_touch_until_ms;         /* 触摸反馈圆点 ~600ms */
#define TOUCH_MS 600

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* ------------------------------------------------------------------ */
/* Frame hashing: FNV-1a-64 over the back buffer.                     */
/* ------------------------------------------------------------------ */

static uint64_t fnv1a64(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t frame_hash(void)
{
    return fnv1a64(fb.px, (size_t)fb.h * fb.pitch);
}

/* Hash of the last rendered frame.  Computing it live over drm.back is
 * racy: aether_drm_wait_flip() swaps drm.back when a (possibly delayed)
 * FLIP_COMPLETE event arrives, so two reads with no repaint in between
 * can land on different buffers.  Caching the hash at render time makes
 * the value stable while idle and still change on every real repaint
 * (focus switch, commit, key) — which is exactly what the smoke test
 * J9/J10 contract requires. */
static uint64_t g_frame_hash;

/* ------------------------------------------------------------------ */
/* Window table.                                                      */
/* ------------------------------------------------------------------ */

static int win_find_by_id(int id)
{
    for (int i = 0; i < win_count; i++)
        if (wins[i].id == id)
            return i;
    return -1;
}

static void win_open(const char *title, int cw, int ch, int fs, int sys,
                     const char *sockpath)
{
    if (win_count >= MAX_WINDOWS)
        return;
    aether_win_t *w = &wins[win_count];
    memset(w, 0, sizeof(*w));
    w->shm_fd = -1;
    w->id = 100 + win_count;
    w->title = strdup(title);
    if (fs) {
        /* P4-1: fullscreen surface — the window IS the screen (lock screen,
         * launcher, phone-style app). No chrome, no cascade offset. */
        w->w = cw > 0 ? cw : 320;
        w->h = ch > 0 ? ch : 240;
        w->x = 0;
        w->y = 0;
        w->fs = 1;
    } else {
        int content_w = cw > 0 ? cw : 320 - 2 * AETHER_BORDER_W;
        int content_h = ch > 0 ? ch : 160 - AETHER_TITLEBAR_H - 2 * AETHER_BORDER_W;
        w->w = content_w + 2 * AETHER_BORDER_W;
        w->h = content_h + AETHER_TITLEBAR_H + 2 * AETHER_BORDER_W;
        w->x = 40 + (win_count % 4) * 40;
        w->y = 40 + (win_count % 4) * 30;
    }
    w->sys = sys ? 1 : 0;
    w->lines = NULL;
    w->n_lines = 0;
    if (sockpath && sockpath[0])
        snprintf(w->sockpath, sizeof w->sockpath, "%s", sockpath);
    win_count++;
    focused_idx = win_count - 1;
    dirty = 1;
}

static void win_close(int id)
{
    int i = win_find_by_id(id);
    if (i < 0)
        return;
    free((void *)wins[i].title);
    if (wins[i].shm_fd >= 0) {
        if (wins[i].shm)
            munmap(wins[i].shm,
                   (size_t)wins[i].shm_w * wins[i].shm_h * 4);
        close(wins[i].shm_fd);
    }
    for (int k = i; k < win_count - 1; k++)
        wins[k] = wins[k + 1];
    win_count--;
    if (focused_idx >= win_count)
        focused_idx = win_count - 1;
    dirty = 1;
}

static void win_focus(int id)
{
    int i = win_find_by_id(id);
    if (i < 0)
        return;
    focused_idx = i;
    dirty = 1;
}

/* P4-1: 开机窗口表为空——启动画面由 SystemUI 的锁屏（全屏系统层面板）
 * 承担，不再播种调试窗口。手机 OS 的开机语义：先锁屏，见不到任何
 * "桌面窗口"；应用只由用户从启动器点开。`open <title>` 仍可开服务端
 * 渲染的调试窗口（K/J 段测试用）。 */
static void win_seed(void)
{
    memset(wins, 0, sizeof(wins));
    for (int i = 0; i < MAX_WINDOWS; i++)
        wins[i].shm_fd = -1;
    win_count = 0;
    focused_idx = -1;
}

/* 渲染序 = 视觉 z 序（底 -> 顶）：系统层窗口先画（home/lock 在堆叠
 * 底部，与手机"启动器是壁纸之上一层"一致），普通窗口按创建序后画
 * （最新置顶）。tap_route 的两遍路由与此同序反向。 */
static void render_frame(void)
{
    /*
     * Re-bind the render target every frame: aether_drm_wait_flip()
     * swaps drm.back after each completed page flip, so the buffer we
     * must paint into moves. Binding once at startup would keep painting
     * the same buffer while present() flips the other (blank) one.
     */
    aether_render_init_fb(&fb, aether_drm_back_buffer(&drm),
                          drm.width, drm.height, drm.pitch);
    aether_render_desktop(&fb);

    /* 交互态：取"最后被绘制且包含触摸点"的窗口（即视觉最顶层的命中窗
     * 口）承载 press——上游 InteractiveHighlight 的局部高亮语义。 */
    float press_a = 0.0f;
    int press_i = -1;
    if (g_press_until_ms) {
        long now = now_ms();
        if (now < g_press_until_ms)
            press_a = (float)(g_press_until_ms - now) / PRESS_MS;
    }
    if (press_a > 0.0f) {
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < win_count; i++) {
                if ((pass == 0) != (wins[i].sys != 0))
                    continue;
                if (g_press_x < wins[i].x ||
                    g_press_x >= wins[i].x + wins[i].w ||
                    g_press_y < wins[i].y ||
                    g_press_y >= wins[i].y + wins[i].h)
                    continue;
                press_i = i;    /* 后画的覆盖先画的：最顶层命中窗 */
            }
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < win_count; i++) {
            if ((pass == 0) != (wins[i].sys != 0))
                continue;       /* pass 0: 系统层；pass 1: 普通窗口 */
            wins[i].focused = (i == focused_idx);
            wins[i].press = (i == press_i) ? press_a : 0.0f;
            aether_render_window(&fb, &wins[i]);
        }
    }

    /* 触摸反馈圆点：全部窗口之上（手指原生的可见确认） */
    if (g_touch_until_ms) {
        long now = now_ms();
        if (now < g_touch_until_ms) {
            float a = (float)(g_touch_until_ms - now) / TOUCH_MS;
            aether_render_touch_dot(&fb, g_touch_x, g_touch_y, a);
        }
    }
    g_frame_hash = frame_hash();   /* snapshot right after painting */
}

/* ------------------------------------------------------------------ */
/* Screenshot: PPM dump of the front buffer (host-inspectable).       */
/* ------------------------------------------------------------------ */

static int write_screenshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%u %u\n255\n", drm.width, drm.height);
    uint32_t stride = fb.pitch / 4;
    /* dump the buffer currently scanned out (1 - back) */
    uint32_t *src = (uint32_t *)drm.map[1 - drm.back];
    for (uint32_t y = 0; y < drm.height; y++) {
        for (uint32_t x = 0; x < drm.width; x++) {
            uint32_t px = src[(size_t)y * stride + x];
            unsigned char rgb[3];
            rgb[0] = (px >> 16) & 0xFF;
            rgb[1] = (px >> 8) & 0xFF;
            rgb[2] = px & 0xFF;
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* IPC server.                                                        */
/* ------------------------------------------------------------------ */

static int ipc_reply_line(int fd, const char *line)
{
    size_t len = strlen(line);
    char buf[IPC_LINE_MAX + 2];
    if (len > IPC_LINE_MAX)
        len = IPC_LINE_MAX;
    memcpy(buf, line, len);
    buf[len] = '\n';          /* line protocol: one JSON object per line */
    if (write(fd, buf, len + 1) < 0)
        return -1;
    return 0;
}

static void ipc_handle(int fd, char *req, int recv_fd)
{
    char resp[IPC_LINE_MAX];
    char *nl = strpbrk(req, "\r\n");
    if (nl)
        *nl = '\0';

    if (strcmp(req, "status") == 0) {
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":true,\"state\":\"running\","
                 "\"mode\":\"%ux%u\",\"frame\":%llu,\"windows\":%d,"
                 "\"focused\":%d,\"hash\":\"%016llx\","
                 "\"gpu\":\"virtio-gpu\",\"cap_3d\":%d,"
                 "\"planes\":{\"total\":%u,\"primary\":%u,\"overlay\":%u,"
                 "\"cursor\":%u}}",
                 SCHEMA, drm.width, drm.height,
                 (unsigned long long)drm.frame, win_count,
                 win_count ? wins[focused_idx].id : 0,
                 (unsigned long long)g_frame_hash,
                 drm.gpu_3d,
                 drm.n_planes, drm.n_primary, drm.n_overlay, drm.n_cursor);
    } else if (strcmp(req, "list") == 0) {
        int off = 0;
        off += snprintf(resp + off, sizeof(resp) - off,
                        "{\"schema\":\"%s\",\"windows\":[", SCHEMA);
        for (int i = 0; i < win_count && off < (int)sizeof(resp) - 192; i++) {
            char sock[116];
            snprintf(sock, sizeof sock, "%s",
                     wins[i].sockpath[0] ? wins[i].sockpath : "");
            off += snprintf(resp + off, sizeof(resp) - off,
                            "%s{\"id\":%d,\"title\":\"%s\","
                            "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                            "\"fs\":%s,\"sys\":%s,\"sock\":\"%s\"}",
                            i ? "," : "", wins[i].id, wins[i].title,
                            wins[i].x, wins[i].y, wins[i].w, wins[i].h,
                            wins[i].fs ? "true" : "false",
                            wins[i].sys ? "true" : "false", sock);
        }
        snprintf(resp + off, sizeof(resp) - off, "]}");
    } else if (strncmp(req, "open ", 5) == 0) {
        win_open(req + 5, 0, 0, 0, 0, NULL);
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":true,\"windows\":%d,\"focused\":%d}",
                 SCHEMA, win_count, wins[focused_idx].id);
    } else if (strncmp(req, "create ", 7) == 0) {
        /* create <title> <w> <h> [fs] [sys] [sock=<uds>] : open a client
         * window of content w x h; fs = fullscreen surface (no chrome),
         * sys = system layer (renders below normal windows, taps last),
         * sock = optional UDS path that receives forwarded taps
         * ("tap <x> <y>\n" lines, window-local coords). */
        char tbuf[512];
        char sockpath[108];
        sockpath[0] = '\0';
        int cw = 0, ch = 0, fs = 0, sys = 0;
        char *p = req + 7;
        char *sp = strchr(p, ' ');
        if (sp) {
            *sp = '\0';
            cw = atoi(sp + 1);
            char *sp2 = strchr(sp + 1, ' ');
            if (sp2) {
                ch = atoi(sp2 + 1);
                char *fl = sp2 + 1;
                while ((fl = strchr(fl, ' ')) != NULL) {
                    fl++;
                    if (!strncmp(fl, "fs", 2) &&
                        (fl[2] == ' ' || fl[2] == '\0'))
                        fs = 1;
                    else if (!strncmp(fl, "sys", 3) &&
                             (fl[3] == ' ' || fl[3] == '\0'))
                        sys = 1;
                    else if (!strncmp(fl, "sock=", 5)) {
                        char *e = fl + 5;
                        char *end = strchr(e, ' ');
                        size_t n = end ? (size_t)(end - e) : strlen(e);
                        if (n >= sizeof(sockpath))
                            n = sizeof(sockpath) - 1;
                        memcpy(sockpath, e, n);
                        sockpath[n] = '\0';
                        if (end)
                            fl = end - 1;
                    }
                }
            }
        }
        snprintf(tbuf, sizeof(tbuf), "%s", p);
        win_open(tbuf, cw, ch, fs, sys, sockpath[0] ? sockpath : NULL);
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":true,\"id\":%d,\"windows\":%d,"
                 "\"focused\":%d}",
                 SCHEMA, wins[win_count - 1].id, win_count,
                 wins[focused_idx].id);
    } else if (strncmp(req, "buf ", 4) == 0) {
        /* buf <id>  (must arrive with an SCM_RIGHTS memfd) */
        int id = atoi(req + 4);
        int i = win_find_by_id(id);
        if (i < 0 || recv_fd < 0) {
            if (recv_fd >= 0)
                close(recv_fd);
            snprintf(resp, sizeof(resp),
                     "{\"schema\":\"%s\",\"ok\":false,\"error\":\"bad buf\"}",
                     SCHEMA);
        } else {
            /* P4-1: fs 窗口的内容区就是整窗（无 chrome 预留）——否则
             * 两边尺寸语义不一致会 mmap 越界（曾致合成器 SIGBUS）。 */
            uint32_t cw, ch;
            if (wins[i].fs) {
                cw = wins[i].w;
                ch = wins[i].h;
            } else {
                cw = wins[i].w - 2 * AETHER_BORDER_W;
                ch = wins[i].h - AETHER_TITLEBAR_H - 2 * AETHER_BORDER_W;
            }
            void *m = mmap(NULL, (size_t)cw * ch * 4, PROT_READ,
                           MAP_SHARED, recv_fd, 0);
            if (m == MAP_FAILED) {
                close(recv_fd);
                snprintf(resp, sizeof(resp),
                         "{\"schema\":\"%s\",\"ok\":false,\"error\":\"mmap\"}",
                         SCHEMA);
            } else {
                if (wins[i].shm_fd >= 0) {
                    munmap(wins[i].shm,
                           (size_t)wins[i].shm_w * wins[i].shm_h * 4);
                    close(wins[i].shm_fd);
                }
                wins[i].shm = (uint32_t *)m;
                wins[i].shm_w = cw;
                wins[i].shm_h = ch;
                wins[i].shm_fd = recv_fd;
                wins[i].has_shm = 1;
                dirty = 1;
                snprintf(resp, sizeof(resp),
                         "{\"schema\":\"%s\",\"ok\":true,\"w\":%u,\"h\":%u}",
                         SCHEMA, cw, ch);
            }
        }
    } else if (strncmp(req, "commit ", 7) == 0) {
        int id = atoi(req + 7);
        int i = win_find_by_id(id);
        if (i >= 0)
            dirty = 1;
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":%s}", SCHEMA,
                 i >= 0 ? "true" : "false");
    } else if (strncmp(req, "close ", 6) == 0) {
        int id = atoi(req + 6);
        win_close(id);
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":true,\"windows\":%d}", SCHEMA,
                 win_count);
    } else if (strncmp(req, "focus ", 6) == 0) {
        int id = atoi(req + 6);
        win_focus(id);
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":true,\"focused\":%d}", SCHEMA,
                 win_count ? wins[focused_idx].id : 0);
    } else if (strcmp(req, "hash") == 0) {
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"hash\":\"%016llx\"}", SCHEMA,
                 (unsigned long long)g_frame_hash);
    } else if (strncmp(req, "screenshot ", 11) == 0) {
        char path[256];
        snprintf(path, sizeof(path), "%.255s", req + 11);
        int rc = write_screenshot(path);
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":%s,\"path\":\"%s\"}", SCHEMA,
                 rc == 0 ? "true" : "false", path);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"schema\":\"%s\",\"ok\":false,\"error\":\"unknown command\"}",
                 SCHEMA);
    }
    ipc_reply_line(fd, resp);
}

/* ------------------------------------------------------------------ */
/* Optional evdev keyboard.                                           */
/* ------------------------------------------------------------------ */

static char g_kbd_name[64];

static int open_evdev(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return -1;
    struct dirent *de;
    int fd = -1;
    g_kbd_name[0] = '\0';
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%.200s", de->d_name);
        int tfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (tfd < 0)
            continue;

        /* Distinguish a real keyboard from mice/tablets: it must support
         * the EV_KEY event type AND report the Enter key (mice report a
         * few button bits under EV_KEY but never KEY_ENTER). */
        unsigned long evbit[(EV_MAX / 8) + 1];
        unsigned long keybit[(KEY_MAX / 8) + 1];
        memset(evbit, 0, sizeof(evbit));
        memset(keybit, 0, sizeof(keybit));
        int is_kbd = 0;
        if (ioctl(tfd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0 &&
            (evbit[EV_KEY / (sizeof(long) * 8)] & (1UL << (EV_KEY % (sizeof(long) * 8)))) &&
            ioctl(tfd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
            int bit = KEY_ENTER / (sizeof(long) * 8);
            int off = KEY_ENTER % (sizeof(long) * 8);
            if (keybit[bit] & (1UL << off))
                is_kbd = 1;
        }
        if (!is_kbd) {
            close(tfd);
            continue;
        }
        /* record the device name for honest reporting */
        char nm[64];
        if (ioctl(tfd, EVIOCGNAME(sizeof(nm)), nm) >= 0)
            snprintf(g_kbd_name, sizeof(g_kbd_name), "%.63s", nm);
        fd = tfd;
        break;
    }
    closedir(dir);
    return fd;
}

static void evdev_handle(int fd)
{
    struct input_event evs[64];
    ssize_t n = read(fd, evs, sizeof(evs));
    if (n <= 0)
        return;
    for (ssize_t i = 0; i < n / (ssize_t)sizeof(struct input_event); i++) {
        if (evs[i].type != EV_KEY || evs[i].value != 1)
            continue;
        if (evs[i].code == KEY_TAB) {
            if (win_count) {
                focused_idx = (focused_idx + 1) % win_count;
                dirty = 1;
            }
        } else if (evs[i].code == KEY_ESC) {
            if (win_count) {
                win_close(wins[focused_idx].id);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Optional evdev tablet / absolute pointer (touch).                   */
/* ------------------------------------------------------------------ */

static int  g_tab_x    = 0, g_tab_y = 0;
static int  g_tab_xmin = 0, g_tab_xmax = 1;
static int  g_tab_ymin = 0, g_tab_ymax = 1;
static char g_tab_name[64];

static int open_tablet(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return -1;
    struct dirent *de;
    int fd = -1;
    g_tab_name[0] = '\0';
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%.200s", de->d_name);
        int tfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (tfd < 0)
            continue;

        /* A tablet reports EV_ABS with ABS_X/ABS_Y; a keyboard reports
         * neither, so this never collides with the keyboard scanner. */
        unsigned long evbit[(EV_MAX / 8) + 1];
        unsigned long absbit[(ABS_MAX / 8) + 1];
        memset(evbit, 0, sizeof(evbit));
        memset(absbit, 0, sizeof(absbit));
        int is_tab = 0;
        if (ioctl(tfd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0 &&
            (evbit[EV_ABS / (sizeof(long) * 8)] &
             (1UL << (EV_ABS % (sizeof(long) * 8)))) &&
            ioctl(tfd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0) {
            int bx = ABS_X / (sizeof(long) * 8), ox = ABS_X % (sizeof(long) * 8);
            int by = ABS_Y / (sizeof(long) * 8), oy = ABS_Y % (sizeof(long) * 8);
            if ((absbit[bx] & (1UL << ox)) && (absbit[by] & (1UL << oy)))
                is_tab = 1;
        }
        if (!is_tab) {
            close(tfd);
            continue;
        }
        /* read axis ranges so we can map device coords -> screen pixels */
        struct input_absinfo info;
        if (ioctl(tfd, EVIOCGABS(ABS_X), &info) >= 0) {
            g_tab_xmin = info.minimum;
            g_tab_xmax = info.maximum ? info.maximum : 1;
        }
        if (ioctl(tfd, EVIOCGABS(ABS_Y), &info) >= 0) {
            g_tab_ymin = info.minimum;
            g_tab_ymax = info.maximum ? info.maximum : 1;
        }
        if (ioctl(tfd, EVIOCGNAME(sizeof(g_tab_name)), g_tab_name) >= 0)
            g_tab_name[sizeof(g_tab_name) - 1] = '\0';
        fd = tfd;
        break;
    }
    closedir(dir);
    return fd;
}

/* Route a tap at screen (sx,sy) to the topmost window. If it lands on the
 * SystemUI surface, translate to surface-local coords and forward a `tap`
 * IPC so SystemUI can hit-test (lock -> unlock, launcher rows -> launch).
 * This is the compositor acting as the input hub / window manager — pointer
 * events are resolved against the window table, not blindly forwarded.
 *
 * 路由序与渲染序严格一致（视觉 z 序）：
 *   pass 0 —— 普通窗口，自顶向下（最新创建的最先匹配）
 *   pass 1 —— 系统层窗口（home/lock 在堆叠底部，普通窗口都不命中才轮到）
 * 手机语义：前台应用窗口消费点击；点到应用窗口之外，才落到启动器。 */
static void tap_route(int sx, int sy)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = win_count - 1; i >= 0; i--) {
            aether_win_t *w = &wins[i];
            if ((pass == 0) == (w->sys != 0))
                continue;       /* pass 0: 普通窗口；pass 1: 系统层 */
            if (sx < w->x || sx >= w->x + w->w)
                continue;
            if (sy < w->y || sy >= w->y + w->h)
                continue;

            /* 触摸反馈：圆点 + 交互式玻璃高亮（P4-1） */
            g_touch_x = sx;
            g_touch_y = sy;
            g_touch_until_ms = now_ms() + TOUCH_MS;
            g_press_x = sx;
            g_press_y = sy;
            g_press_until_ms = now_ms() + PRESS_MS;
            dirty = 1;

            /* P4-1: fs 表面无 chrome，局部坐标 = 屏幕坐标 - 窗口原点 */
            int lx, ly;
            if (w->fs) {
                lx = sx - w->x;
                ly = sy - w->y;
            } else {
                lx = sx - w->x - AETHER_BORDER_W;
                ly = sy - w->y - AETHER_TITLEBAR_H;
            }
            int cw = w->fs ? w->w : w->w - 2 * AETHER_BORDER_W;
            int ch = w->fs ? w->h
                           : w->h - AETHER_TITLEBAR_H - 2 * AETHER_BORDER_W;
            if (lx < 0 || ly < 0 || lx >= cw || ly >= ch)
                return;     /* 命中窗口边框/标题栏，不转发 */

            /* 转发目标（P5）：SystemUI 保持固定 sock；普通窗口在 create 时
             * 以 sock=<uds> 注册接收端——客机内「系统更新」等带按钮的应用
             * 依赖它获得可点击性。无接收端则点击仅作视觉反馈。 */
            const char *target = NULL;
            if (w->title && strcmp(w->title, "SystemUI") == 0)
                target = SYSUI_SOCK;
            else if (w->sockpath[0])
                target = w->sockpath;
            if (!target)
                return;
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            if (s >= 0) {
                struct sockaddr_un a;
                memset(&a, 0, sizeof a);
                a.sun_family = AF_UNIX;
                snprintf(a.sun_path, sizeof a.sun_path, "%s", target);
                if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
                    char buf[64];
                    int n = snprintf(buf, sizeof buf, "tap %d %d\n",
                                     lx, ly);
                    (void)!write(s, buf, (size_t)n);
                }
                close(s);
            }
            fprintf(stderr,
                    "aethercomp: tap -> %s (%d,%d) local (%d,%d)\n",
                    w->title ? w->title : "?", sx, sy, lx, ly);
            return; /* topmost window (of this pass) consumes the tap */
        }
    }
}

static void tablet_handle(int fd)
{
    struct input_event evs[64];
    ssize_t n = read(fd, evs, sizeof(evs));
    if (n <= 0)
        return;
    for (ssize_t i = 0; i < n / (ssize_t)sizeof(struct input_event); i++) {
        if (evs[i].type == EV_ABS) {
            if (evs[i].code == ABS_X)      g_tab_x = evs[i].value;
            else if (evs[i].code == ABS_Y) g_tab_y = evs[i].value;
        } else if (evs[i].type == EV_KEY &&
                   (evs[i].code == BTN_TOUCH || evs[i].code == BTN_LEFT ||
                    evs[i].code == BTN_RIGHT)) {
            fprintf(stderr,
                    "aethercomp: tablet btn code=%d value=%d (pos %d,%d)\n",
                    evs[i].code, evs[i].value, g_tab_x, g_tab_y);
            if (evs[i].value == 1) {
            /* map device coords -> screen pixels */
            int sx = (g_tab_xmax > g_tab_xmin)
                         ? (g_tab_x - g_tab_xmin) * (int)drm.width /
                               (g_tab_xmax - g_tab_xmin)
                         : g_tab_x;
            int sy = (g_tab_ymax > g_tab_ymin)
                         ? (g_tab_y - g_tab_ymin) * (int)drm.height /
                               (g_tab_ymax - g_tab_ymin)
                         : g_tab_y;
            if (sx < 0) sx = 0; else if (sx >= (int)drm.width)  sx = (int)drm.width  - 1;
            if (sy < 0) sy = 0; else if (sy >= (int)drm.height) sy = (int)drm.height - 1;
            tap_route(sx, sy);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main loop.                                                         */
/* ------------------------------------------------------------------ */

static int setup_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);
    unlink(SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind " SOCK_PATH);
        close(fd);
        return -1;
    }
    chmod(SOCK_PATH, 0660);
    /* P1-2b: 让非 root 应用（沙箱 uid/gid=1000）也能连合成器——
     * 显示服务器的 IPC 本就该对应用开放。root 仍可作属主访问。
     * 必须用 chown(路径) 而非 fchown(fd)：sockfs inode 无 setattr，
     * fchown() 对 unix socket fd 返回 0 但不改 owner（宿主最小实验实锤，
     * 曾导致沙箱 demo connect EACCES -> create failed 重启循环）。 */
    if (chown(SOCK_PATH, -1, 1000) < 0)   /* aether app group */
        perror("chown " SOCK_PATH);
    if (listen(fd, 8) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void serve_once(void)
{
    /* 先绑 IPC socket 再做 DRM 初始化：慢速软件模拟（QEMU TCG）下
     * aether_drm_open + modeset 可达数十秒，若等 DRM 就绪才 bind，
     * 期间「可连接性」无法反映进程存活——watchdog/客户端只能看到
     * ECONNREFUSED。提前 bind 后，连接进入 listen  backlog 排队，
     * 进入 serve 循环后即被 accept 并正常应答。 */
    int sfd = setup_socket();
    if (sfd < 0)
        exit(1);

    if (aether_drm_open(&drm, NULL) < 0) {
        fprintf(stderr, "aethercomp: DRM init failed\n");
        exit(1);
    }
    aether_render_init_fb(&fb, aether_drm_back_buffer(&drm),
                         drm.width, drm.height, drm.pitch);

    /* 壁纸：有则用真照片（玻璃的模糊/折射/色差才有内容可扭曲），
     * 无则桌面回退内置渐变——如实记录，不假装加载成功。 */
    if (aether_render_load_wallpaper(AETHER_WALLPAPER) == 0) {
        int ww = 0, wh = 0;
        aether_render_wallpaper_size(&ww, &wh);
        fprintf(stderr, "aethercomp: wallpaper loaded %s (%dx%d PPM)\n",
                AETHER_WALLPAPER, ww, wh);
    } else {
        fprintf(stderr,
                "aethercomp: wallpaper %s unavailable — gradient fallback\n",
                AETHER_WALLPAPER);
    }
    win_seed();

    int kfd = open_evdev();
    if (kfd >= 0)
        fprintf(stderr, "aethercomp: evdev keyboard attached: %s\n",
                g_kbd_name[0] ? g_kbd_name : "(unnamed)");

    int tfd = open_tablet();
    if (tfd >= 0)
        fprintf(stderr, "aethercomp: evdev tablet attached: %s\n",
                g_tab_name[0] ? g_tab_name : "(unnamed)");

    fprintf(stderr, "aethercomp: serving on %s (%ux%u, %d windows)\n",
            SOCK_PATH, drm.width, drm.height, win_count);

    struct pollfd pfds[4];
    int nfds = 0;

    /* first paint + flip so the display has content immediately */
    render_frame();
    dirty = 0;
    if (aether_drm_present(&drm) == 0)
        aether_drm_wait_flip(&drm, 2000);

    while (!stop_flag) {
        /* P4-1: 交互态衰减——press/touch 到期后清位并重绘一帧（玻璃高亮
         * 与触摸圆点随之消失）。活跃期间 poll 用 50ms 超时，让衰减动画
         * 持续推进；静止时回到无限阻塞（不烧 CPU）。 */
        long now = now_ms();
        if (g_press_until_ms && now >= g_press_until_ms) {
            g_press_until_ms = 0;
            dirty = 1;
        }
        if (g_touch_until_ms && now >= g_touch_until_ms) {
            g_touch_until_ms = 0;
            dirty = 1;
        }
        int poll_to = -1;
        if (g_press_until_ms || g_touch_until_ms) {
            long rem = g_press_until_ms ? g_press_until_ms - now : TOUCH_MS;
            long remt = g_touch_until_ms ? g_touch_until_ms - now : TOUCH_MS;
            if (remt < rem)
                rem = remt;
            poll_to = rem < 50 ? (int)rem : 50;
            if (poll_to < 1)
                poll_to = 1;
        }

        nfds = 0;
        pfds[nfds].fd = drm.fd;
        pfds[nfds].events = POLLIN;
        nfds++;
        pfds[nfds].fd = sfd;
        pfds[nfds].events = POLLIN;
        nfds++;
        if (kfd >= 0) {
            pfds[nfds].fd = kfd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (tfd >= 0) {
            pfds[nfds].fd = tfd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        if (dirty) {
            render_frame();
            dirty = 0;
            if (aether_drm_present(&drm) == 0)
                aether_drm_wait_flip(&drm, 2000);
            continue; /* re-poll with fresh state */
        }

        int r = poll(pfds, nfds, poll_to);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;
            if (pfds[i].fd == drm.fd) {
                aether_drm_wait_flip(&drm, 0);
            } else if (pfds[i].fd == kfd) {
                evdev_handle(kfd);
            } else if (pfds[i].fd == tfd) {
                tablet_handle(tfd);
            } else if (pfds[i].fd == sfd) {
                int c = accept(sfd, NULL, NULL);
                if (c < 0)
                    continue;
                char req[IPC_LINE_MAX];
                char cbuf[CMSG_SPACE(sizeof(int))];
                struct iovec iov;
                iov.iov_base = req;
                iov.iov_len = sizeof(req) - 1;
                struct msghdr msg;
                memset(&msg, 0, sizeof(msg));
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = cbuf;
                msg.msg_controllen = sizeof(cbuf);
                ssize_t n = recvmsg(c, &msg, 0);
                int recv_fd = -1;
                if (n > 0) {
                    req[n] = '\0';
                    struct cmsghdr *cmsg;
                    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
                         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                        if (cmsg->cmsg_level == SOL_SOCKET &&
                            cmsg->cmsg_type == SCM_RIGHTS)
                            memcpy(&recv_fd, CMSG_DATA(cmsg), sizeof(int));
                    }
                    ipc_handle(c, req, recv_fd);
                } else if (recv_fd >= 0) {
                    close(recv_fd);
                }
                close(c);
            }
        }
    }

    fprintf(stderr, "aethercomp: shutting down\n");
    if (kfd >= 0)
        close(kfd);
    close(sfd);
    unlink(SOCK_PATH);
    aether_drm_close(&drm);
}

/* ------------------------------------------------------------------ */
/* IPC client mode (aethercompctl).                                   */
/* ------------------------------------------------------------------ */

static int client_mode(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect " SOCK_PATH);
        close(fd);
        return 1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }
    char resp[IPC_LINE_MAX];
    ssize_t n = read(fd, resp, sizeof(resp) - 1);
    if (n > 0) {
        resp[n] = '\0';
        printf("%s", resp);   /* server already terminates the line */
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        /*
         * Join argv[2..] with spaces so that both shell forms work:
         *   aethercompctl -c "focus 1"   (quoted, single argv)
         *   aethercompctl -c focus 1     (unquoted, split argv)
         * The IPC line protocol is a single whitespace-joined line.
         */
        char cmd[IPC_LINE_MAX];
        size_t off = 0;
        for (int i = 2; i < argc; i++) {
            int n = snprintf(cmd + off, sizeof(cmd) - off, "%s%s",
                             i > 2 ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(cmd) - off)
                break; /* truncated: the server side would reject anyway */
            off += (size_t)n;
        }
        return client_mode(cmd);
    }

    serve_once();
    return 0;
}
