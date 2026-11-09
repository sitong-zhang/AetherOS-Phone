/*
 * aether-compositor.c - AetherOS 手机合成器 / 显示服务端
 *
 * 职责：
 *   - 通过 DRM 原子接口驱动 virtio-gpu 等显示硬件 / fbdev
 *   - 读取触摸输入（EV_ABS 绝对坐标 + BTN_TOUCH）与键盘，更新触点与窗口状态
 *     （PC 方案已废弃：不识别鼠标 EV_REL，不绘制任何指针）
 *   - 软件合成桌面（aether-render），双缓冲翻页上屏
 *   - 通过 Unix 域套接字 /run/aetheros-comp.sock 暴露 IPC：
 *       status              -> 帧数/触点/分辨率/窗口状态(JSON)
 *       screenshot <path>   -> 把当前上屏帧导出为 PPM
 *       ping                -> pong
 *
 * 本项目只存在手机形态：PC 桌面方案已彻底废弃，输入仅识别触摸（EV_ABS +
 * BTN_TOUCH）与键盘，不识别鼠标 EV_REL，界面内不绘制任何指针。
 * 已修正前次移植的结构性缺陷（poll 越界 / 跳过 IPC 轮询 / 逐像素写文件）。
 */
#include "aether-display.h"
#include "aether-render.h"
#include "aether-android.h"   /* 安卓兼容层：图形桥/输入桥 */
#include "aether-log.h"
#include "aether-desktop.h"
#include "aether-anim.h"
#include "aether-apps.h"
#include "aether-island.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define COMP_SOCK "/run/aetheros-comp.sock"
#define MAX_FDS 16

static volatile int g_stop = 0;
static int g_frame = 0;
static int g_shift = 0;               /* Shift 键按下状态 */
static struct aether_disp g_disp;     /* 显示后端（全局，供截图使用） */
static unsigned char *g_last_frame;   /* 最近一次完整渲染的缓冲（截图取它，避免翻页事件缺失导致空帧） */

/* 逻辑画布：手机外壳始终按 AETHER_RES（如 720x1280 竖屏）布局，
 * 再缩放到物理帧缓冲（可能是 1280x800 等横屏）。这样无论 VM 提供何种
 * 物理分辨率，手机 UI 始终是真正的竖屏，且对任意后端(DRM/fbdev)都成立。 */
static unsigned char *g_canvas = NULL;
static int g_cw = 0, g_ch = 0;
static int g_need_render = 0;      /* IPC 改变手机状态后，请求下一帧重绘 */
static int g_no_display = 0;       /* 1=无物理显示后端(headless)：仍渲染画布+IPC+screenshot */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 崩溃处理器：仅用裸系统调用，避免 stdio 缓冲在 _exit 前丢失、
 * 避免 backtrace 的堆分配在信号上下文里二次崩溃。信号号写到 /dev/console
 * （即串口，可在宿主机 serial.log 直接读到）。 */
static void on_crash(int sig)
{
    int con = open("/dev/console", O_WRONLY);
    int fd = (con >= 0) ? con : 2;
    char buf[80];
    int i = 0;
    const char *head = "[compositor][CRASH] sig=";
    while (head[i]) buf[i] = head[i], i++;
    /* 整数转十进制（倒序再反转） */
    char dig[12]; int d = 0, v = sig;
    if (v == 0) dig[d++] = '0';
    else while (v > 0) { dig[d++] = (char)('0' + (v % 10)); v /= 10; }
    while (d > 0) buf[i++] = dig[--d];
    buf[i++] = '\n';
    (void)!write(fd, buf, (size_t)i);
    if (con >= 0) close(con);
    _exit(1);
}

/* ---- /proc 系统指标（带 500ms 缓存） ---- */
static double g_cpu = 0, g_mem = 0;
static double g_last_sample = 0;
static unsigned long g_pu = 0, g_pn = 0, g_ps = 0, g_pid = 0;
static int g_have_prev = 0;

static void sample_sys(double now)
{
    if (now - g_last_sample < 0.5)
        return;
    g_last_sample = now;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof line, f)) {
            unsigned long u, n, s, id, io = 0;
            if (sscanf(line, "cpu %lu %lu %lu %lu %lu", &u, &n, &s, &id, &io) >= 4) {
                unsigned long idle = id + io, total = u + n + s + idle;
                if (g_have_prev) {
                    unsigned long dt = total - (g_pu + g_pn + g_ps + g_pid);
                    unsigned long di = idle - g_pid;
                    if (dt > 0)
                        g_cpu = (double)(dt - di) / (double)dt * 100.0;
                }
                g_pu = u;
                g_pn = n;
                g_ps = s;
                g_pid = idle;
                g_have_prev = 1;
            }
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        unsigned long tot = 0, av = 0;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %lu", &tot) == 1)
                continue;
            if (sscanf(line, "MemAvailable: %lu", &av) == 1)
                continue;
        }
        fclose(f);
        if (tot > 0)
            g_mem = (double)(tot - av) / (double)tot * 100.0;
    }
}

static void clock_str(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, sz, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---- 输入设备扫描（仅触摸 + 键盘，无鼠标） ---- */
enum in_kind { IN_KEY = 0, IN_TOUCH = 1 };

struct input_dev {
    int fd;
    int kind;
    int abs_x_max, abs_y_max;
};
static struct input_dev g_in[8];
static int g_nin = 0;

static int has_type(int fd, unsigned long type)
{
    unsigned long bits[8];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(0, sizeof bits), bits) < 0)
        return 0;
    return !!(bits[type / 64] & (1UL << (type % 64)));
}

static void scan_inputs(void)
{
    for (int i = 0; i < 32; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            break;
        int is_abs = has_type(fd, EV_ABS);
        int is_key = has_type(fd, EV_KEY);
        int kind = -1;
        /* 触摸屏 / 触控板绝对坐标设备；鼠标 EV_REL 相对位移设备不纳入 */
        if (is_abs)
            kind = IN_TOUCH;
        else if (is_key)
            kind = IN_KEY;
        if (kind < 0) {
            close(fd);
            continue;
        }
        int dup = 0;
        for (int k = 0; k < g_nin; k++)
            if (g_in[k].kind == kind)
                dup = 1;
        if (dup) {
            close(fd);
            continue;
        }
        struct input_dev d;
        d.fd = fd;
        d.kind = kind;
        d.abs_x_max = 1;
        d.abs_y_max = 1;
        if (kind == IN_TOUCH) {
            struct input_absinfo info;
            if (ioctl(fd, EVIOCGABS(ABS_X), &info) == 0 && info.maximum > 0)
                d.abs_x_max = (int)info.maximum;
            if (ioctl(fd, EVIOCGABS(ABS_Y), &info) == 0 && info.maximum > 0)
                d.abs_y_max = (int)info.maximum;
        }
        g_in[g_nin++] = d;
        aether_log("comp", "输入设备 %s -> kind=%d", p, kind);
        if (g_nin >= 8)
            break;
    }
}

/* ---- 截图：把逻辑画布（竖屏手机 UI）导出为 PPM ---- */
static int do_screenshot(const char *path)
{
    /* 取逻辑画布（始终为 AETHER_RES，竖屏），而非物理帧缓冲，
       这样截出来的是干净的竖屏手机画面，不被物理横屏黑边污染。 */
    unsigned char *src = g_canvas ? g_canvas : (g_last_frame ? g_last_frame : NULL);
    if (!src || g_cw <= 0 || g_ch <= 0)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t w = (uint32_t)g_cw, h = (uint32_t)g_ch;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    size_t need = (size_t)w * h * 3;
    unsigned char *rgb = (unsigned char *)malloc(need);
    if (!rgb) {
        fclose(f);
        return -1;
    }
    size_t off = 0;
    for (uint32_t y = 0; y < h; y++) {
        unsigned *rowp = (unsigned *)(src + (size_t)y * (size_t)w * 4);
        for (uint32_t x = 0; x < w; x++) {
            unsigned px = rowp[x];   /* 渲染缓冲为 0x00RRGGBB */
            rgb[off++] = (unsigned char)((px >> 16) & 0xFF); /* R */
            rgb[off++] = (unsigned char)((px >> 8) & 0xFF);  /* G */
            rgb[off++] = (unsigned char)(px & 0xFF);         /* B */
        }
    }
    size_t wrote = fwrite(rgb, 1, need, f);
    free(rgb);
    fclose(f);
    return (wrote == need) ? 0 : -1;
}

/* ---- 逻辑画布 -> 物理帧缓冲：保持比例居中（letterbox），最近邻缩放 ---- */
static void present_canvas(void)
{
    unsigned char *phys = aether_disp_back(&g_disp);
    if (!phys || !g_canvas || g_cw <= 0 || g_ch <= 0)
        return;
    int pw = (int)g_disp.width, ph = (int)g_disp.height;
    int ppitch = (int)g_disp.pitch;
    /* 黑边填充 */
    for (int y = 0; y < ph; y++)
        memset(phys + (size_t)y * ppitch, 0, (size_t)pw * 4);
    double s = (double)pw / g_cw;
    if ((double)ph / g_ch < s) s = (double)ph / g_ch;
    int dw = (int)((double)g_cw * s), dh = (int)((double)g_ch * s);
    int ox = (pw - dw) / 2, oy = (ph - dh) / 2;
    unsigned *cd = (unsigned *)g_canvas;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * g_ch / dh);
        unsigned *dst = (unsigned *)(phys + (size_t)(oy + y) * ppitch + (size_t)ox * 4);
        unsigned *src = cd + (size_t)sy * g_cw;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * g_cw / dw);
            dst[x] = src[sx];
        }
    }
}

/* 物理坐标 -> 逻辑画布坐标（含 letterbox 偏移与缩放），越界则夹到边界 */
static void phys_to_canvas(int *px, int *py)
{
    if (!g_canvas || g_cw <= 0 || g_ch <= 0) return;
    int pw = (int)g_disp.width, ph = (int)g_disp.height;
    double s = (double)pw / g_cw;
    if ((double)ph / g_ch < s) s = (double)ph / g_ch;
    int dw = (int)((double)g_cw * s), dh = (int)((double)g_ch * s);
    int ox = (pw - dw) / 2, oy = (ph - dh) / 2;
    int cx = (int)(((*px) - ox) / s);
    int cy = (int)(((*py) - oy) / s);
    if (cx < 0) cx = 0;
    if (cx >= g_cw) cx = g_cw - 1;
    if (cy < 0) cy = 0;
    if (cy >= g_ch) cy = g_ch - 1;
    *px = cx; *py = cy;
}

/* ---- IPC ---- */
static void handle_client(int cfd)
{
    char buf[4096];
    ssize_t got = 0;
    while (got < (ssize_t)sizeof buf - 1) {
        ssize_t n = recv(cfd, buf + got, 1, 0);
        if (n <= 0)
            break;
        got += n;
        if (buf[got - 1] == '\n')
            break;
    }
    if (got <= 0)
        return;
    buf[got] = 0;
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
    char cmd[64];
    sscanf(buf, "%63s", cmd);

    char reply[4096];

    /* 桌面自动化命令（state / click / move / key / text / open / skipboot ...）
       与真机输入共用同一套 handler，保证测试路径 = 用户点击路径。 */
    aether_desktop_ipc(buf, reply, sizeof reply);
    /* 除纯查询外，任何命令都可能改变手机状态，请求重绘（否则截图/显示停留在旧帧） */
    if (strcmp(cmd, "ping") && strcmp(cmd, "status") && strcmp(cmd, "screenshot"))
        g_need_render = 1;
    if (strcmp(reply, "unknown\n") == 0) {
        if (strcmp(cmd, "ping") == 0) {
            snprintf(reply, sizeof reply, "pong\n");
        } else if (strcmp(cmd, "status") == 0) {
            char js[2048];
            aether_desktop_state(js, sizeof js);
            int a_state = aether_android_status_int("state", AANDROID_UNKNOWN);
            int a_pkvm  = aether_android_status_int("pkvm", 0);
            uint32_t pw = g_no_display ? (uint32_t)g_cw : g_disp.width;
            uint32_t ph = g_no_display ? (uint32_t)g_ch : g_disp.height;
            snprintf(reply, sizeof reply,
                     "{\"frame\":%d,\"res\":\"%dx%d\",\"phys\":\"%ux%u\",\"backend\":%d,\"headless\":%d,\"touch\":[%d,%d],\"android\":{\"state\":%d,\"pkvm\":%d},\"desktop\":%s}\n",
                     g_frame, g_cw, g_ch, pw, ph, g_disp.backend, g_no_display,
                     aether_render_touch_x(), aether_render_touch_y(),
                     a_state, a_pkvm, js);
        } else if (strcmp(cmd, "screenshot") == 0) {
            char path[256];
            if (sscanf(buf, "screenshot %255s", path) == 1) {
                /* 取证：把动效推到完成态，再用最新手机状态重绘一帧抓屏，
                 * 避免截到过渡中间帧或 IPC 改变状态前的陈旧帧。 */
                aether_desktop_settle();
                char clk[16]; clock_str(clk, sizeof clk);
                aether_render_set_target(g_canvas);
                aether_desktop_render(clk, g_cpu, g_mem);
                int rc = do_screenshot(path);
                snprintf(reply, sizeof reply, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
            } else {
                snprintf(reply, sizeof reply, "{\"ok\":false,\"error\":\"no path\"}\n");
            }
        } else {
            snprintf(reply, sizeof reply, "unknown\n");
        }
    }
    (void)send(cfd, reply, strlen(reply), 0);
}

int main(void)
{
    aether_log_ident("compositor");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGABRT, on_crash);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGFPE, on_crash);

    const char *res = getenv("AETHER_RES");
    if (aether_disp_open(&g_disp, res) != 0) {
        /* 无物理显示后端（如 ARM64/Limbo 内核未提供 /dev/fb0 或 DRM）：
           仍渲染到逻辑画布并响应 IPC/screenshot，便于无头取证与服务模式；
           真实设备有 /dev/fb0 时走 present 上屏，二者共用同一渲染管线。 */
        aether_log("comp", "无物理显示后端：进入 headless 渲染模式（仍渲染画布+IPC+screenshot）");
        g_no_display = 1;
        g_disp.width = 0; g_disp.height = 0; g_disp.pitch = 0; g_disp.fd = -1;
    } else {
        aether_log("comp", "显示打开 %ux%u backend=%d (pitch=%u)", g_disp.width, g_disp.height,
                   g_disp.backend, g_disp.pitch);
    }

    /* 逻辑画布：仅手机形态(AETHER_PHONE=1)按 AETHER_RES 布局（默认竖屏 720x1280）；
       否则画布=物理分辨率（桌面形态不受影响）。 */
    g_cw = (int)g_disp.width; g_ch = (int)g_disp.height;
    const char *phone = getenv("AETHER_PHONE");
    if (phone && !strcmp(phone, "1") && res) {
        int rw = 0, rh = 0;
        if (sscanf(res, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            g_cw = rw; g_ch = rh;
        } else if (sscanf(res, "%ux%u", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            g_cw = rw; g_ch = rh;
        }
    }
    if (g_cw <= 0 || g_ch <= 0) { g_cw = 1280; g_ch = 720; }   /* headless 且无 AETHER_RES 时的兜底 */
    g_canvas = (unsigned char *)malloc((size_t)g_cw * g_ch * 4);
    if (!g_canvas) {
        aether_log_fatal("comp", "画布内存分配失败 %dx%d", g_cw, g_ch);
        return 1;
    }
    memset(g_canvas, 0, (size_t)g_cw * g_ch * 4);
    aether_log("comp", "逻辑画布 %dx%d (物理 %ux%u)", g_cw, g_ch, g_disp.width, g_disp.height);

    aether_render_init(g_canvas, g_cw, g_ch, g_cw * 4);
    aether_desktop_init(g_cw, g_ch);
    /* 安卓兼容层桥：无容器时零副作用（共享帧不可见、uinput 缺失静默降级） */
    aether_android_bridge_init();

    scan_inputs();

    /* IPC 监听套接字 */
    unlink(COMP_SOCK);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        aether_log_fatal("comp", "socket 失败: %s", strerror(errno));
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, COMP_SOCK, sizeof addr.sun_path - 1);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        aether_log_fatal("comp", "bind %s 失败: %s", COMP_SOCK, strerror(errno));
        return 1;
    }
    listen(sfd, 8);
    chmod(COMP_SOCK, 0666);

    aether_log("comp", "serving on %s  (%ux%u)", COMP_SOCK, g_disp.width, g_disp.height);

    int dirty = 1;
    time_t last_draw = 0;
    time_t flip_since = 0;
    struct pollfd pfds[MAX_FDS];
    while (!g_stop) {
        int n = 0;
        if (g_disp.fd >= 0) {
            pfds[n].fd = g_disp.fd;
            pfds[n].events = POLLIN;
            n++;
        }
        pfds[n].fd = sfd;
        pfds[n].events = POLLIN;
        n++;
        for (int i = 0; i < g_nin; i++) {
            if (g_in[i].fd >= 0 && n < MAX_FDS) {
                pfds[n].fd = g_in[i].fd;
                pfds[n].events = POLLIN;
                n++;
            }
        }
        /* 动画期间用短超时做约 30fps 连续帧，空闲时回到 120ms 省电
         * （总纲 7：跟手顺滑、性能不足自动降级；动画时钟驱动连续重绘）。 */
        /* 过渡动画或「计时器/秒表/媒体播放」需要连续帧（30ms），
         * 空闲时回到 120ms 省电。总纲 5.7 数字实时跳、第七章跟手顺滑。 */
        int anim_live = aether_anim_active() || aether_apps_live()
                        || aether_island_live();
        int pr = poll(pfds, n, anim_live ? 30 : 120);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* IPC 改变了手机状态，请求本帧重绘 */
        if (g_need_render) {
            dirty = 1;
            g_need_render = 0;
        }
        /* 有过渡在跑：每帧都重绘，保证跟手与转场连续 */
        if (anim_live) dirty = 1;

        /* 翻页完成事件：交换 back 指针（仅 DRM 后端有 fd） */
        for (int i = 0; i < n; i++) {
            if (g_disp.fd >= 0 && pfds[i].fd == g_disp.fd && (pfds[i].revents & POLLIN)) {
                aether_disp_handle_events(&g_disp);
            }
        }

        /* IPC 新连接 */
        for (int i = 0; i < n; i++) {
            if (pfds[i].fd == sfd && (pfds[i].revents & POLLIN)) {
                int cfd = accept(sfd, NULL, NULL);
                if (cfd >= 0) {
                    handle_client(cfd);
                    close(cfd);
                }
            }
        }

        /* 输入事件（仅当该 fd 有 POLLIN） */
        for (int i = 0; i < g_nin; i++) {
            int fd = g_in[i].fd;
            if (fd < 0)
                continue;
            int readable = 0;
            for (int k = 0; k < n; k++)
                if (pfds[k].fd == fd && (pfds[k].revents & POLLIN))
                    readable = 1;
            if (!readable)
                continue;
            struct input_event ev;
            while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                int cx = aether_render_touch_x();
                int cy = aether_render_touch_y();
                if (ev.type == EV_ABS && g_in[i].kind == IN_TOUCH) {
                    /* 触摸绝对坐标：EV_ABS 分轴上报，累积成完整触点 */
                    if (ev.code == ABS_X) cx = ev.value * (int)g_disp.width / g_in[i].abs_x_max;
                    else if (ev.code == ABS_Y) cy = ev.value * (int)g_disp.height / g_in[i].abs_y_max;
                    else continue;
                    phys_to_canvas(&cx, &cy);
                    aether_render_set_touch(cx, cy);
                    /* 输入桥：触点落在安卓窗口内则喂给容器，桌面不重复处理 */
                    if (!aether_android_bridge_input(cx, cy, 0))
                        aether_desktop_motion(cx, cy);
                    dirty = 1;
                } else if (ev.type == EV_KEY) {
                    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                        g_shift = (ev.value != 0);
                    } else if (ev.code == BTN_TOUCH) {
                        /* 触摸语义：按下(down=1) -> 期间 EV_ABS 位移转发给
                         * aether_desktop_motion 形成拖拽 -> 抬起(down=0) 由外壳
                         * 判定 click 还是 swipe。手机形态不使用幽灵点击。 */
                        if (ev.value == 1) {
                            phys_to_canvas(&cx, &cy);
                            if (!aether_android_bridge_input(cx, cy, 1))
                                aether_desktop_button(cx, cy, 1);
                        } else if (ev.value == 0) {
                            phys_to_canvas(&cx, &cy);
                            if (!aether_android_bridge_input(cx, cy, 0))
                                aether_desktop_button(cx, cy, 0);
                        }
                        dirty = 1;
                    } else if (ev.value == 1 || ev.value == 2) {
                        /* 真机键盘事件与 IPC text 注入都进 aether_desktop_key */
                        aether_desktop_key(ev.code, g_shift);
                        dirty = 1;
                    }
                }
            }
        }

        /* 每秒重绘：顶栏时钟走动；fbdev 没有翻页事件，靠它持续上屏 */
        time_t now_s = time(NULL);
        if (now_s != last_draw) {
            dirty = 1;
            last_draw = now_s;
        }
        /* 某些驱动不送 page-flip 事件，超时强制解锁，避免画面冻结 */
        if (g_disp.flip_pending && flip_since && now_s - flip_since >= 2)
            g_disp.flip_pending = 0;

        /* 渲染并提交（翻页未挂起时） */
        if (dirty && !g_disp.flip_pending) {
            /* 渲染到逻辑画布（竖屏手机 UI），再缩放到物理帧缓冲上屏。 */
            aether_render_set_target(g_canvas);
            double now = (double)time(NULL);
            sample_sys(now);
            char clk[16];
            clock_str(clk, sizeof clk);
            aether_desktop_render(clk, g_cpu, g_mem);
            /* 安卓 App 以窗口形式浮在桌面，不抢全屏；容器未运行时不绘制 */
            aether_android_bridge_render();
            g_last_frame = g_canvas;
            if (!g_no_display) {
                present_canvas();
                aether_disp_present(&g_disp);
                if (g_disp.flip_pending)
                    flip_since = now_s;
            }
            g_frame++;
            dirty = 0;
        }
    }

    aether_disp_close(&g_disp);
    aether_log("comp", "退出");
    return 0;
}
