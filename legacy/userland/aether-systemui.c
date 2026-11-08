/*
 * aether-systemui.c - AetherOS SystemUI：锁屏 + 启动器（P4-1 手机语义重写）。
 *
 * 这不是壳、不是模拟、不是贴图——与 demo/settings/clock 一样是 compositor
 * 的独立客户端进程，经 UDS 注册窗口，用 SCM_RIGHTS 共享 memfd 承载像素，
 * 自己把内容画进去再 commit 上屏。
 *
 * P4-1 的手机语义（本版重写的核心）：
 *   1) SystemUI 是**全屏系统层窗口**（create SystemUI W H fs sys）：
 *      无 chrome、整窗即屏幕；sys 层渲染在普通窗口之下（与手机"锁屏/
 *      启动器是壁纸之上一层"一致），只有在没有普通窗口覆盖某点时，该点
 *      的触摸才路由到 SystemUI；
 *   2) 开机 = **锁屏**：全屏液态玻璃（合成器 BottomSheet 配方，0.62 不
 *      透明度）+ 大时钟（4x 字模）+ 中文日期 + 电量 + 锁图标 + 点按解锁；
 *      不再播种任何调试窗口——开机只见锁屏，应用只由用户从启动器点开；
 *   3) 解锁 = 启动器：全屏、手指尺寸行高（ROW_H=72）、2x 应用名；
 *   4) 玻璃配方不动：合成器负责 GlassMaterials.BottomSheet 的完整实现
 *      （vibrancy + blur 4dp + lens 24/48dp + depthEffect + 按压色差），
 *      SystemUI 只画内容像素，背景留 AETHER_TKEY 让玻璃透上来。
 *
 * 数据源全部真实（读不到显示 n/a，绝不编造）：
 *   时间   : time() + localtime（真墙钟；客机无 RTC 时区配置，按 UTC 显示）
 *   内存   : /proc/meminfo  MemTotal / MemAvailable
 *   负载   : /proc/loadavg
 *   uptime : /proc/uptime
 *   服务   : /run/aether-services.state（aether-init 的状态快照，P2-13c）
 *   电量   : /run/aether-hal-android.state（Android 宿主 HAL，P3-16）
 *   应用   : /data/aether/apps/index（aether-pkg 的真实索引，P2-11）
 *   屏幕   : compositor status 的 "mode":"WxH"（KMS 真实模式）
 *
 * 启动链路（P3-14b，锁屏解锁后生效）：数字键 1-9（evdev 真键盘，非独占读）
 * 或 sysui IPC `launch <id>` 或点按启动器行 -> fork+exec
 * /sbin/aether-sandbox-run（降权沙箱后 exec 目标应用，与 init 服务沙箱
 * 同一个 aether_apply_sandbox()）。
 *
 * 诚实边界（写在文件头，也写进 README）：
 *   1) 屏幕几何在启动时向 compositor 采样一次（KMS 模式运行期不变；
 *      合成器重启后沿用同值重注册——模式不会随进程重启改变）；
 *   2) Launcher 是文本网格，无图标渲染（无 PNG/矢量解码器）；
 *   3) 锁屏/解锁是 SystemUI 自己的两态状态机，不是合成器的窗口协议——
 *      合成器只提供 fs/sys 两个窗口标志与 tap 路由；
 *   4) 时钟只到分钟（秒级刷新会让合成器帧哈希持续变化——那是 P0-6 的
 *      「空闲帧哈希稳定」不变量；需要确定化时走 sysui `refresh off`）。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>

#include "aether-render.h"
#include "aether-log.h"

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

#define COMP_SOCK   "/run/aetheros-comp.sock"
#define SYSUI_SOCK  "/run/aetheros-sysui.sock"
#define APPS_INDEX  "/data/aether/apps/index"
#define APPS_DIR    "/data/aether/apps"
#define SVC_STATE   "/run/aether-services.state"
#define HAL_STATE   "/run/aether-hal-android.state"
#define SANDBOX_RUN "/sbin/aether-sandbox-run"

/* ---------------------------------------------------------------- 布局
 * fs 窗口无 chrome：客户端坐标就是屏幕坐标。手机产品形态优先——屏幕是
 * 竖屏（720x1600，VNC 正好铺满手机屏）时用手机密度：状态条 96、行高
 * 148、锁屏时钟 6x；横屏（CI/桌面的 1280x800）保持既有像素基线（J16/
 * T2/U7 的坐标以它为参）。全部数值运行期由 KMS 模式推导，不写死。 */
static int g_bar_h      = 48;    /* 状态条高度 */
static int g_row_top    = 96;    /* 首行顶（状态条 + 头部行之后） */
static int g_row_h      = 72;    /* 行高：手指尺寸 */
static int g_rows_max_y = 736;   /* 行区下界 */
static int g_portrait   = 0;     /* 1 = 竖屏手机形态 */
static int g_clock_scale = 4;    /* 锁屏大时钟字号 */
static int g_ui_scale    = 2;    /* 应用名/日期/提示字号 */

#define TAGLINE_DY   48      /* 中文标识行距屏幕底部的距离（800-48=752） */

#define MAX_APPS     16

#define REFRESH_MS   2000    /* 重绘间隔（分钟级变化才 commit） */

/* 调色板（与合成器 chrome 同一族，保证全系统一致） */
#define FG_MAIN   AETHER_RGB(0xE6, 0xED, 0xF3)   /* 近白：主文字/时钟 */
#define FG_DIM    AETHER_RGB(0x8B, 0x98, 0xA5)   /* 暗灰蓝：辅助文字 */
#define FG_ACCENT AETHER_RGB(0x4F, 0xC3, 0xF7)   /* 青：强调/角标/分隔线 */
#define LOCK_BODY AETHER_RGB(0x1B, 0x24, 0x30)   /* 锁体深色 */

struct app {
    char id[64];
    char name[64];
    char exec[64];
};

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_refresh = 1;   /* sysui `refresh off` 冻结刷新 */
static struct app g_apps[MAX_APPS];
static int g_napps = 0;
static int g_win_id = -1;
static int g_memfd = -1;
static uint32_t *g_px = MAP_FAILED;

/* 屏幕几何（启动时向 compositor 采样；运行期 KMS 模式不变） */
static int g_sw = 1280, g_sh = 800;

static void grid_init(void);   /* 应用网格几何（定义见下方 draw_launcher 前） */

/* 布局派生：竖屏=手机形态（720x1600），横屏=CI/桌面基线（1280x800）。
 * 必须在 query_screen() 之后调用。 */
static void layout_init(void)
{
    g_portrait = (g_sh > g_sw);
    if (g_portrait) {
        g_bar_h      = 96;
        g_row_top    = 200;
        g_row_h      = 148;
        g_rows_max_y = g_sh - 120;
        g_clock_scale = 6;
        g_ui_scale    = 3;
    } else {
        g_bar_h      = 48;
        g_row_top    = 96;
        g_row_h      = 72;
        g_rows_max_y = g_sh - 64;
        g_clock_scale = 4;
        g_ui_scale    = 2;
    }
    grid_init();
}

/* SystemUI 两态状态机：1=锁屏（开机态），0=启动器 */
static int g_locked = 1;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---------------------------------------------------------------- 数据源 */

/* /proc/meminfo: 返回可用百分比（0-100），失败返回 -1。 */
static int mem_avail_pct(long *total_kb, long *avail_kb)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long total = -1, avail = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MemTotal:", 9))     sscanf(line + 9, "%ld", &total);
        else if (!strncmp(line, "MemAvailable:", 13)) sscanf(line + 13, "%ld", &avail);
    }
    fclose(f);
    if (total <= 0 || avail < 0) return -1;
    if (total_kb) *total_kb = total;
    if (avail_kb) *avail_kb = avail;
    return (int)((avail * 100) / total);
}

/* /run/aether-hal-android.state: 读 Android 宿主 HAL 快照里的电量。
 * 返回 0-100；-1 表示未接入（诚实标记，绝不伪造数值）。 */
static int hal_battery_pct(int *charging)
{
    FILE *f = fopen(HAL_STATE, "r");
    if (charging) *charging = 0;
    if (!f) return -1;
    char line[128];
    int pct = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "battery_pct=", 12))
            pct = atoi(line + 12);
        else if (!strncmp(line, "charging=", 9) && charging)
            *charging = atoi(line + 9);
    }
    fclose(f);
    return pct;
}

static double load_avg(void)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1.0;
    double l = -1.0;
    if (fscanf(f, "%lf", &l) != 1) l = -1.0;
    fclose(f);
    return l;
}

static double sys_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1.0;
    double u = -1.0;
    if (fscanf(f, "%lf", &u) != 1) u = -1.0;
    fclose(f);
    return u;
}

/* /run/aether-services.state: 统计 running 行数与总行数。 */
static void svc_counts(int *running, int *total)
{
    int r = 0, t = 0;
    FILE *f = fopen(SVC_STATE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char n[64]; int p; char st[32];
            if (sscanf(line, "%63s %d %31s", n, &p, st) == 3) {
                t++;
                if (!strcmp(st, "running")) r++;
            }
        }
        fclose(f);
    }
    if (running) *running = r;
    if (total) *total = t;
}

/* /data/aether/apps/index: 每行 id|name|version|exec|sha256 */
static int apps_load(void)
{
    FILE *f = fopen(APPS_INDEX, "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < MAX_APPS) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *f1 = strchr(line, '|');
        if (!f1) continue;
        char *f2 = strchr(f1 + 1, '|');
        if (!f2) continue;
        char *f3 = strchr(f2 + 1, '|');
        if (!f3) continue;
        char *f4 = strchr(f3 + 1, '|');
        *f1 = '\0'; *f2 = '\0'; *f3 = '\0';
        if (f4) *f4 = '\0';
        /* 精度限定符：源行最长 512B，字段缓冲区 64B——显式截断，不依赖
         * snprintf 的静默截断（-Wformat-truncation 零警告是构建纪律）。 */
        snprintf(g_apps[n].id,   sizeof g_apps[n].id,   "%.63s", line);
        snprintf(g_apps[n].name, sizeof g_apps[n].name, "%.63s", f1 + 1);
        snprintf(g_apps[n].exec, sizeof g_apps[n].exec, "%.63s", f3 + 1);
        n++;
    }
    fclose(f);
    return n;
}

static const struct app *app_by_index(int i)
{
    return (i >= 0 && i < g_napps) ? &g_apps[i] : NULL;
}

/* ---------------------------------------------------------------- 启动 */

static pid_t launch_app(const struct app *a)
{
    if (!a) return -1;
    char path[320];
    snprintf(path, sizeof path, "%s/%s/%s", APPS_DIR, a->id, a->exec);
    if (access(path, X_OK) != 0) {
        aether_log(AETHER_LOG_WARN, "systemui: launch %s: %s not executable",
                   a->id, path);
        return -1;
    }
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        /* 系统服务直接 exec 沙箱启动器：由它降权（uid/gid=1000 + 能力全清
         * + mnt ns + seccomp）后再 exec 目标应用。 */
        execl(SANDBOX_RUN, "aether-sandbox-run", path, (char *)NULL);
        _exit(127);
    }
    aether_log(AETHER_LOG_INFO, "systemui: launched %s (%s) pid=%d",
               a->id, a->name, (int)p);
    return p;
}

/* ---------------------------------------------------------------- 绘制 */

/* aether_render_text_scaled 的推进宽度（ASCII 8px、CJK 16px，×scale）；
 * 非法 UTF-8 字节渲染器跳过（0 推进）——与此保持一致，供居中定位。 */
static int text_w_scaled(const char *s, int scale)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        if (*p < 0x80) { w += 8; p += 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { w += 16; p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { w += 16; p += 3; }
        else { p += 1; }
    }
    return w * scale;
}

/* 底部中文标识行（两态都画）：ASCII 前缀 9 字符=72px，汉字从 x=112 起。
 * CI 的 J16 扫描 x∈[112,312], y∈[752,768] 的近白像素——10 个汉字
 * （统一/图形 操作系统移动互联）全部在 16x16 CJK 子集内。 */
static void draw_tagline(aether_fb_t *fb, const char *cn)
{
    int ty = g_sh - TAGLINE_DY;
    if (ty < 0) ty = 0;
    char line[160];
    snprintf(line, sizeof line, "AetherOS %s", cn);
    aether_render_text_utf8(fb, 40, ty, line, FG_MAIN);
}

/* 状态条右侧读数（启动器用）：电量/内存/负载，全部真实读数。 */
static void draw_readings(aether_fb_t *fb, int y)
{
    int bat_chg = 0;
    int bat = hal_battery_pct(&bat_chg);
    char batstr[32];
    if (bat >= 0)
        snprintf(batstr, sizeof batstr, "bat %d%%%s", bat, bat_chg ? "+" : "");
    else
        snprintf(batstr, sizeof batstr, "bat --");

    long tk = 0, ak = 0;
    int pct = mem_avail_pct(&tk, &ak);
    double ld = load_avg();

    char right[200];
    if (pct >= 0 && ld >= 0)
        snprintf(right, sizeof right, "%s  mem %d%%  load %.2f",
                 batstr, pct, ld);
    else
        snprintf(right, sizeof right, "%s  mem n/a  load n/a", batstr);
    int rw = (int)strlen(right) * 8;      /* 8x16 ASCII 字模 */
    aether_render_text(fb, g_sw - rw - 24, y, right, FG_DIM);
}

/* ---------------------------------------------------------------- 应用网格
 * 主屏 = 圆角图标块网格（手机的样子），不是文件列表。几何由 grid_init()
 * 统一算：列数随宽度（720→3 列、1280→5 列）、块宽上限 160，
 * 点击命中与绘制共用同一份数值（sysui_tap 不再自己算行号）。 */
#define GRID_GAP   32
#define GRID_MAX_W 160

static int g_cols, g_tile, g_margin, g_cell_h;

static void grid_init(void)
{
    int label_h = g_portrait ? 40 : 24;
    int cols = g_sw / 240;
    if (cols < 3) cols = 3;
    int tile = (g_sw - 2 * 64 - (cols - 1) * GRID_GAP) / cols;
    if (tile > GRID_MAX_W) tile = GRID_MAX_W;
    if (tile < 96) tile = 96;
    g_cols = cols;
    g_tile = tile;
    g_margin = (g_sw - cols * tile - (cols - 1) * GRID_GAP) / 2;
    g_cell_h = tile + 8 + label_h + (g_portrait ? 48 : 32);
}

/* 圆角实心块（图标底座）：逐行按圆角内缩，sqrt 只算 2r 行，代价可忽略。 */
static void fill_round_rect(aether_fb_t *fb, int x, int y, int w, int h,
                            int r, uint32_t rgb)
{
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int d = 0;
        if (j < r) d = r - j;
        else if (j >= h - r) d = j - (h - r) + 1;
        if (d > 0 && d < r) {
            float t = (float)(r * r - d * d);
            float s = __builtin_sqrtf(t > 0.0f ? t : 0.0f);
            inset = r - (int)(s + 0.5f);
        }
        if (inset < 0) inset = 0;
        if (w - 2 * inset > 0)
            aether_render_rect(fb, x + inset, y + j, w - 2 * inset, 1, rgb);
    }
}

static uint32_t tile_color(int i)
{
    static const uint32_t pal[4] = {
        AETHER_RGB(0x4F, 0xC3, 0xF7),   /* 青 */
        AETHER_RGB(0x7C, 0x8C, 0xF0),   /* 靛 */
        AETHER_RGB(0xF0, 0x9F, 0x6B),   /* 琥珀 */
        AETHER_RGB(0x6B, 0xD9, 0xA8),   /* 薄荷 */
    };
    return pal[i % 4];
}

/* 图标标识：取 id 最后一段的首字母大写（aetheros.settings -> "S"）。 */
static void app_monogram(const struct app *a, char *out, size_t n)
{
    const char *p = strrchr(a->id, '.');
    p = p ? p + 1 : a->id;
    char c = (char)((p[0] >= 'a' && p[0] <= 'z') ? p[0] - 'a' + 'A' : p[0]);
    snprintf(out, n, "%c", c ? c : '?');
}

static void draw_app_tile(aether_fb_t *fb, int x, int y,
                          const struct app *a, int i)
{
    int r = g_tile / 5;
    uint32_t c = tile_color(i);

    fill_round_rect(fb, x, y, g_tile, g_tile, r, c);
    /* 底部 1/3 压暗一档：一块纯色太"平"，有一点体积感更像图标 */
    aether_render_rect(fb, x + 2, y + g_tile * 2 / 3, g_tile - 4,
                       g_tile / 3 - 2,
                       AETHER_RGB((int)(((c >> 16) & 0xFF) * 0.72f),
                                  (int)(((c >> 8) & 0xFF) * 0.72f),
                                  (int)((c & 0xFF) * 0.72f)));

    /* 字母标识（居中，scale 5；竖屏块大，字也大） */
    char mono[4];
    app_monogram(a, mono, sizeof mono);
    int ms = g_portrait ? 5 : 4;
    int mw = text_w_scaled(mono, ms);
    aether_render_text_scaled(fb, x + (g_tile - mw) / 2,
                              y + (g_tile - 16 * ms) / 2, mono,
                              AETHER_RGB(0x0E, 0x14, 0x1B), ms);

    /* 名称（块下方居中，1x 字模；超出块宽就截断加省略）。
     * 主屏标签去品牌前缀（"AetherOS Settings" -> "Settings"）——块宽
     * 160px 只放得下 19 个字符，带前缀必然被截成"Aetheros S..."。 */
    const char *disp = a->name;
    if (!strncmp(disp, "AetherOS ", 9)) disp += 9;
    char name[24];
    size_t maxc = (size_t)((g_tile - 8) / 8);
    if (maxc > sizeof name - 2) maxc = sizeof name - 2;
    snprintf(name, sizeof name, "%s", disp);
    if (strlen(name) > maxc && maxc > 1) {
        name[maxc - 1] = '.';
        name[maxc] = '\0';
    }
    int nw = (int)strlen(name) * 8;
    aether_render_text(fb, x + (g_tile - nw) / 2, y + g_tile + 10, name,
                       FG_MAIN);
}

/* 锁屏：全屏玻璃（合成器画）+ 大时钟 + 中文日期 + 电量 + 锁图标 + 提示。 */
static void draw_lock(uint32_t *px)
{
    aether_fb_t fb;
    aether_render_init_fb(&fb, px, (uint32_t)g_sw, (uint32_t)g_sh,
                          (uint32_t)g_sw * 4);

    /* 透明底：磨砂玻璃由合成器的系统层面板负责，这里只画内容像素。 */
    for (uint32_t i = 0; i < (uint32_t)g_sw * g_sh; i++)
        px[i] = AETHER_TKEY;

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);

    /* ---- 大时钟（横屏 4x / 竖屏 6x 字模） ---- */
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    int cw = text_w_scaled(clock, g_clock_scale);
    int cx = (g_sw - cw) / 2;
    int cy = g_sh * 28 / 100;
    aether_render_text_scaled(&fb, cx, cy, clock, FG_MAIN, g_clock_scale);

    /* ---- 中文日期（"YYYY年MM月DD日"） ---- */
    char date[48];
    snprintf(date, sizeof date, "%04d\u5e74%02d\u6708%02d\u65e5",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    int dw = text_w_scaled(date, g_ui_scale);
    int dy = cy + (g_portrait ? 128 : 104);
    aether_render_text_scaled(&fb, (g_sw - dw) / 2, dy, date,
                              FG_DIM, g_ui_scale);

    /* ---- 电量（真实读数；未接入显 --） ---- */
    int bat_chg = 0;
    int bat = hal_battery_pct(&bat_chg);
    char batstr[48];
    if (bat >= 0)
        snprintf(batstr, sizeof batstr, "bat %d%%%s", bat,
                 bat_chg ? " charging" : "");
    else
        snprintf(batstr, sizeof batstr, "bat --");
    int bscale = g_portrait ? 2 : 1;
    int bw = (int)strlen(batstr) * 8 * bscale;
    aether_render_text_scaled(&fb, (g_sw - bw) / 2,
                              dy + (g_portrait ? 72 : 64), batstr,
                              FG_DIM, bscale);

    /* ---- 锁图标（矩形拼意：锁梁 + 锁体 + 钥匙孔；几何按 k 等比放大） ---- */
    int k = g_portrait ? 2 : 1;
    int iw = 96 * k, ih = 72 * k;
    int ix = (g_sw - iw) / 2;
    int iy = cy + (g_portrait ? 248 : 216);
    /* 锁梁：两侧竖条 + 顶横条 */
    aether_render_rect(&fb, ix + 20 * k, iy, 8 * k, 40 * k, FG_ACCENT);
    aether_render_rect(&fb, ix + iw - 28 * k, iy, 8 * k, 40 * k, FG_ACCENT);
    aether_render_rect(&fb, ix + 20 * k, iy, iw - 40 * k, 8 * k, FG_ACCENT);
    /* 锁体：深底 + 青色描边 */
    aether_render_rect(&fb, ix, iy + 32 * k, iw, ih, LOCK_BODY);
    for (int i = 0; i < 2 * k; i++)
        aether_render_rect(&fb, ix + i, iy + 32 * k + i, iw - 2 * i, 1,
                           FG_ACCENT);
    aether_render_rect(&fb, ix + iw / 2 - 9 * k, iy + 32 * k + ih / 2 - 16 * k,
                       18 * k, 18 * k, FG_ACCENT);       /* 钥匙孔圆槽 */
    aether_render_rect(&fb, ix + iw / 2 - 4 * k, iy + 32 * k + ih / 2 + 2 * k,
                       8 * k, 16 * k, FG_ACCENT);        /* 钥匙孔柄 */

    /* ---- 解锁提示（居中） ---- */
    const char *hint = "tap to unlock";
    int hw = text_w_scaled(hint, g_ui_scale);
    aether_render_text_scaled(&fb, (g_sw - hw) / 2,
                              iy + ih + (g_portrait ? 88 : 56), hint,
                              FG_MAIN, g_ui_scale);

    draw_tagline(&fb, "\u7edf\u4e00\u64cd\u4f5c\u7cfb\u7edf\u79fb\u52a8\u4e92\u8054");
}

/* 启动器：全屏、手指尺寸行高、2x 应用名。 */
static void draw_launcher(uint32_t *px)
{
    aether_fb_t fb;
    aether_render_init_fb(&fb, px, (uint32_t)g_sw, (uint32_t)g_sh,
                          (uint32_t)g_sw * 4);

    for (uint32_t i = 0; i < (uint32_t)g_sw * g_sh; i++)
        px[i] = AETHER_TKEY;

    /* ---- 状态条 ---- */
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    char left[64];
    snprintf(left, sizeof left, "AetherOS  %s", clock);
    aether_render_text_scaled(&fb, 24, g_portrait ? 24 : 10, left, FG_MAIN, 2);
    draw_readings(&fb, g_portrait ? (g_bar_h / 2 - 8) : 22);
    aether_render_rect(&fb, 0, g_bar_h - 2, g_sw, 2, FG_ACCENT);

    /* ---- 头部行：应用数/服务数/uptime（真实读数） ---- */
    int sr = 0, st = 0;
    svc_counts(&sr, &st);
    double up = sys_uptime();
    char head[160];
    if (up >= 0 && st > 0)
        snprintf(head, sizeof head,
                 "apps %d   services %d/%d   up %.0fs",
                 g_napps, sr, st, up);
    else
        snprintf(head, sizeof head, "apps %d   services n/a", g_napps);
    aether_render_text_scaled(&fb, 24, g_bar_h + (g_portrait ? 30 : 14), head,
                              FG_DIM, g_portrait ? 2 : 1);

    if (g_napps == 0) {
        aether_render_text_scaled(&fb, 24, g_row_top + 24,
                                  "(no installed apps - package index empty)",
                                  FG_DIM, g_portrait ? 2 : 1);
        draw_tagline(&fb,
                     "\u56fe\u5f62\u64cd\u4f5c\u7cfb\u7edf\u79fb\u52a8\u4e92\u8054");
        return;
    }

    /* ---- 应用网格：手机主屏的样子——圆角图标块 + 字母标识 + 名称 ----
     * 竖屏 3 列 × 5 行、横屏 5 列 × 2 行（grid_init 统一算，点击命中
     * 用同一份几何，draw/命中不会走偏）。 */
    int idx = 0;
    for (int row = 0; ; row++) {
        int cy = g_row_top + row * g_cell_h;
        if (cy + g_tile > g_rows_max_y)
            break;
        for (int col = 0; col < g_cols && idx < g_napps; col++, idx++) {
            int cx = g_margin + col * (g_tile + GRID_GAP);
            draw_app_tile(&fb, cx, cy, &g_apps[idx], idx);
        }
        if (idx >= g_napps)
            break;
    }

    draw_tagline(&fb,
                 "\u56fe\u5f62\u64cd\u4f5c\u7cfb\u7edf\u79fb\u52a8\u4e92\u8054");
}

static void draw_screen(uint32_t *px)
{
    if (g_locked)
        draw_lock(px);
    else
        draw_launcher(px);
}

static int ensure_window(void);

/* 触摸命中点（fs 窗口：局部坐标 = 屏幕坐标）映射到启动器行并启动应用。
 * 行布局与 draw_launcher 一致：首行 y = ROW_TOP，每行 ROW_H。 */
static void sysui_tap(int lx, int ly)
{
    aether_log(AETHER_LOG_INFO, "systemui: tap recv (%d,%d) locked=%d napps=%d",
               lx, ly, g_locked, g_napps);
    if (g_locked) {
        /* 锁屏：任意点按解锁（手机语义）。 */
        g_locked = 0;
        aether_log(AETHER_LOG_INFO, "systemui: tap -> unlock");
        draw_screen(g_px);
        if (ensure_window() < 0)
            aether_log(AETHER_LOG_WARN, "systemui: refresh after unlock failed");
        return;
    }
    if (g_napps <= 0)
        return;
    if (lx < 0 || lx >= g_sw)
        return;
    if (ly < g_row_top || ly >= g_rows_max_y)
        return;
    /* 网格命中：与 draw_launcher 同一份几何（grid_init 的 cols/tile/
     * margin/cell_h）。命中块本身或块下方的名称条都算点中。 */
    int col = (lx - g_margin) / (g_tile + GRID_GAP);
    int row = (ly - g_row_top) / g_cell_h;
    int in_x = lx >= g_margin + col * (g_tile + GRID_GAP) &&
               lx < g_margin + col * (g_tile + GRID_GAP) + g_tile;
    int in_y = ly >= g_row_top + row * g_cell_h &&
               ly < g_row_top + row * g_cell_h + g_tile + 36;
    if (col < 0 || col >= g_cols || row < 0 || !in_x || !in_y)
        return;
    int i = row * g_cols + col;
    const struct app *a = app_by_index(i);
    if (!a)
        return;
    aether_log(AETHER_LOG_INFO,
               "systemui: tap (%d,%d) -> cell r%d c%d (%s) -> launch %s",
               lx, ly, row, col, a->name, a->id);
    launch_app(a);
    g_napps = apps_load();   /* 应用可能新增 */
    draw_screen(g_px);
    if (ensure_window() < 0)
        aether_log(AETHER_LOG_WARN, "systemui: refresh after tap failed");
}

/* ---------------------------------------------------------------- IPC 客户端 */

static int connect_comp(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", COMP_SOCK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_comp_retry(void)
{
    for (int i = 0; i < 60 && !g_stop; i++) {
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

/* 向 compositor 采样 KMS 模式（合成器在 DRM modeset 之后才 bind socket，
 * 因此这里的连通性本身就隐含"显示栈已就绪"）。 */
static int query_screen(int *w, int *h)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    if (write(s, "status", 6) < 0) { close(s); return -1; }
    char resp[1024];
    if (recv_line(s, resp, sizeof resp) < 0) { close(s); return -1; }
    close(s);
    const char *m = strstr(resp, "\"mode\":\"");
    if (!m) return -1;
    int mw = 0, mh = 0;
    if (sscanf(m + 8, "%dx%d", &mw, &mh) != 2 || mw <= 0 || mh <= 0)
        return -1;
    *w = mw;
    *h = mh;
    return 0;
}

static int cmd_create(int *id_out)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char cmd[160];
    snprintf(cmd, sizeof cmd, "create SystemUI %d %d fs sys", g_sw, g_sh);
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
    /* 诚实：sendmsg 成功不等于被接受（窗口 id 不存在时合成器回 ok:false）。
     * 合成器重启后旧 id 会失效——必须看响应，否则静默“假上屏”。 */
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

/*
 * 保证窗口有效并上屏一帧。
 *
 * 合成器可能重启（watchdog 处决、崩溃自恢复），重启后它的窗口表重建、
 * 旧 id 失效。SystemUI 必须自愈：commit 被拒 -> 重新 create + buf + commit。
 * 不做这件事就会静默“假上屏”：进程活着、日志说已提交，屏幕上什么也没有。
 */
static int ensure_window(void)
{
    if (g_win_id < 0) {
        if (cmd_create(&g_win_id) < 0) return -1;
        aether_log(AETHER_LOG_INFO, "systemui: (re)created window id=%d (%dx%d fs sys)",
                   g_win_id, g_sw, g_sh);
    }
    if (cmd_buf(g_win_id, g_memfd) < 0) {
        g_win_id = -1;                 /* id 已失效：下一轮重建 */
        return -1;
    }
    if (cmd_commit(g_win_id) < 0) {
        g_win_id = -1;
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- sysui IPC */

/*
 * /run/aetheros-sysui.sock —— 命令（每条一连接，行协议）：
 *   status           -> JSON 状态（真实读数，供 CI 交叉验证）
 *   launch <n|id>    -> 启动第 n 个应用（1-based）或按 id；返回 {"ok":..}
 *                       锁屏态拒绝（手机语义：先解锁）
 *   unlock | lock    -> 切换锁屏/启动器（点按解锁的 IPC 等价路径）
 *   refresh off|on   -> 冻结/恢复定时重绘（确定化测试用：冻结后合成器
 *                       帧哈希才能稳定——P0-6 的 J9 不变量）
 *   tap <lx> <ly>    -> compositor 转发的触摸点（fs 局部坐标=屏幕坐标）
 */
static void json_escape(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (const char *p = s; *p && o + 2 < n; p++) {
        if (*p == '"' || *p == '\\') { dst[o++] = '\\'; dst[o++] = *p; }
        else dst[o++] = *p;
    }
    dst[o] = '\0';
}

static void sysui_status(int fd)
{
    long tk = 0, ak = 0;
    int pct = mem_avail_pct(&tk, &ak);
    double ld = load_avg(), up = sys_uptime();
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    int sr = 0, st = 0;
    svc_counts(&sr, &st);

    int bat_chg = 0;
    int bat = hal_battery_pct(&bat_chg);
    char resp[4096];
    int off = 0;
    off += snprintf(resp + off, sizeof resp - off,
        "{\"schema\":\"aetheros.sysui/1\",\"ok\":true,\"window_id\":%d,"
        "\"refresh\":%s,\"locked\":%s,\"screen\":\"%dx%d\","
        "\"clock\":\"%s\",\"mem_avail_pct\":%d,"
        "\"mem_total_kb\":%ld,\"mem_avail_kb\":%ld,\"load\":%.2f,"
        "\"uptime_s\":%.1f,\"services_running\":%d,\"services_total\":%d,"
        "\"battery_pct\":%d,\"battery_charging\":%d,\"battery_src\":\"%s\","
        "\"apps\":[",
        g_win_id, g_refresh ? "true" : "false", g_locked ? "true" : "false",
        g_sw, g_sh, clock, pct, tk, ak, ld, up, sr, st,
        bat, bat_chg, bat >= 0 ? "REAL" : "NONE");
    for (int i = 0; i < g_napps; i++) {
        char eid[160], enm[160];
        json_escape(eid, sizeof eid, g_apps[i].id);
        json_escape(enm, sizeof enm, g_apps[i].name);
        off += snprintf(resp + off, sizeof resp - off,
                        "%s{\"id\":\"%s\",\"name\":\"%s\"}",
                        i ? "," : "", eid, enm);
    }
    off += snprintf(resp + off, sizeof resp - off, "]}");
    if (write(fd, resp, (size_t)off) < 0) { /* peer gone */ }
    if (write(fd, "\n", 1) < 0) { }
}

static void sysui_launch(int fd, const char *arg)
{
    if (g_locked) {
        /* 手机语义：锁屏态不响应启动请求（点按或 IPC 先解锁）。 */
        const char *resp =
            "{\"schema\":\"aetheros.sysui/1\",\"ok\":false,"
            "\"error\":\"locked\"}\n";
        if (write(fd, resp, strlen(resp)) < 0) { }
        return;
    }
    const struct app *a = NULL;
    if (arg && *arg >= '1' && *arg <= '9') {
        a = app_by_index(atoi(arg) - 1);
    } else if (arg && *arg) {
        for (int i = 0; i < g_napps; i++)
            if (!strcmp(g_apps[i].id, arg)) { a = &g_apps[i]; break; }
    }
    char resp[256];
    if (!a) {
        snprintf(resp, sizeof resp,
                 "{\"schema\":\"aetheros.sysui/1\",\"ok\":false,"
                 "\"error\":\"no such app\"}\n");
        if (write(fd, resp, strlen(resp)) < 0) { }
        return;
    }
    pid_t p = launch_app(a);
    snprintf(resp, sizeof resp,
             "{\"schema\":\"aetheros.sysui/1\",\"ok\":%s,\"id\":\"%s\","
             "\"pid\":%d}\n",
             p > 0 ? "true" : "false", a->id, (int)p);
    if (write(fd, resp, strlen(resp)) < 0) { }
}

/* 解锁/锁屏：切状态机 + 重绘 + 上屏。from 仅用于日志归因。 */
static void sysui_set_locked(int locked, const char *from)
{
    if (g_locked == locked)
        return;
    g_locked = locked;
    aether_log(AETHER_LOG_INFO, "systemui: %s via %s",
               locked ? "locked" : "unlocked", from);
    draw_screen(g_px);
    if (ensure_window() < 0)
        aether_log(AETHER_LOG_WARN, "systemui: refresh after %s failed", from);
}

/* ---------------------------------------------------------------- 键盘 */

static int open_kbd(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;
    struct dirent *de;
    int fd = -1;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[256];
        snprintf(path, sizeof path, "/dev/input/%.200s", de->d_name);
        int tfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (tfd < 0) continue;
        unsigned long evbit[(EV_MAX / 8) + 1];
        unsigned long keybit[(KEY_MAX / 8) + 1];
        memset(evbit, 0, sizeof evbit);
        memset(keybit, 0, sizeof keybit);
        int is_kbd = 0;
        if (ioctl(tfd, EVIOCGBIT(0, sizeof evbit), evbit) >= 0 &&
            (evbit[EV_KEY / (sizeof(long) * 8)] &
             (1UL << (EV_KEY % (sizeof(long) * 8)))) &&
            ioctl(tfd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit) >= 0) {
            int bit = KEY_ENTER / (sizeof(long) * 8);
            int o = KEY_ENTER % (sizeof(long) * 8);
            if (keybit[bit] & (1UL << o)) is_kbd = 1;
        }
        if (!is_kbd) { close(tfd); continue; }
        fd = tfd;   /* 非独占读：compositor 也未 EVIOCGRAB */
        break;
    }
    closedir(dir);
    return fd;
}

/* 返回被按下的数字键（1-9），无则返回 0。 */
static int poll_digit(int kfd)
{
    struct pollfd pfd = { .fd = kfd, .events = POLLIN };
    if (poll(&pfd, 1, 0) != 1) return 0;
    if (!(pfd.revents & POLLIN)) return 0;
    struct input_event ev;
    ssize_t n;
    int digit = 0;
    while ((n = read(kfd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.value == 1 &&
            ev.code >= KEY_1 && ev.code <= KEY_9)
            digit = (int)(ev.code - KEY_1) + 1;
    }
    return digit;
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    aether_log_init("aether-systemui");
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* 启动的应用退出后由内核回收，不造僵尸 */

    /* 屏幕几何：向 compositor 采样 KMS 模式（失败即退出，由 init 重启——
     * 宁可不上屏也不用一个猜错的尺寸注册全屏窗口）。 */
    if (query_screen(&g_sw, &g_sh) < 0) {
        aether_log(AETHER_LOG_ERROR,
                   "systemui: cannot sample screen mode (compositor down?)");
        return 1;
    }
    aether_log(AETHER_LOG_INFO, "systemui: screen mode %dx%d", g_sw, g_sh);
    layout_init();
    aether_log(AETHER_LOG_INFO,
               "systemui: layout %s bar=%d row_top=%d row_h=%d max_y=%d",
               g_portrait ? "portrait(phone)" : "landscape",
               g_bar_h, g_row_top, g_row_h, g_rows_max_y);

    g_napps = apps_load();
    aether_log(AETHER_LOG_INFO, "systemui: %d app(s) in package index", g_napps);

    g_memfd = memfd_create("aether-systemui", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (g_memfd < 0) {
        aether_log(AETHER_LOG_ERROR, "systemui: memfd_create failed (%s)", strerror(errno));
        return 1;
    }
    size_t sz = (size_t)g_sw * g_sh * 4;
    if (ftruncate(g_memfd, (off_t)sz) < 0) {
        aether_log(AETHER_LOG_ERROR, "systemui: ftruncate failed (%s)", strerror(errno));
        close(g_memfd);
        return 1;
    }
    g_px = mmap(NULL, sz, PROT_WRITE, MAP_SHARED, g_memfd, 0);
    if (g_px == MAP_FAILED) {
        aether_log(AETHER_LOG_ERROR, "systemui: mmap failed (%s)", strerror(errno));
        close(g_memfd);
        return 1;
    }

    /* 开机第一屏 = 锁屏（g_locked 初值 1） */
    draw_screen(g_px);
    if (ensure_window() < 0) {
        aether_log(AETHER_LOG_ERROR, "systemui: first commit failed");
    } else {
        aether_log(AETHER_LOG_INFO,
                   "systemui: window id=%d committed (%dx%d fs sys, locked)",
                   g_win_id, g_sw, g_sh);
    }

    /* sysui 服务 socket */
    unlink(SYSUI_SOCK);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SYSUI_SOCK);
    if (srv < 0 || bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(srv, 4) != 0) {
        aether_log(AETHER_LOG_WARN, "systemui: sysui socket unavailable (%s)",
                   strerror(errno));
        if (srv >= 0) close(srv);
        srv = -1;
    } else {
        chmod(SYSUI_SOCK, 0660);
    }

    int kfd = open_kbd();
    if (kfd < 0)
        aether_log(AETHER_LOG_WARN, "systemui: no evdev keyboard - digit launch disabled");

    /* 主循环：200ms 轮询（键盘 + sysui 命令），2s 重绘一次 */
    long last_refresh_ms = 0;
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    long t0 = ts0.tv_sec * 1000 + ts0.tv_nsec / 1000000;

    while (!g_stop) {
        struct pollfd pfds[2];
        int nf = 0;
        if (kfd >= 0) { pfds[nf].fd = kfd; pfds[nf].events = POLLIN; nf++; }
        if (srv >= 0) { pfds[nf].fd = srv; pfds[nf].events = POLLIN; nf++; }
        if (nf > 0) {
            int pr = poll(pfds, (nfds_t)nf, 200);
            if (pr > 0) {
                if (kfd >= 0 && (pfds[0].events & POLLIN) && pfds[0].revents & POLLIN) {
                    int d = poll_digit(kfd);
                    /* 手机语义：锁屏态数字键不启动应用（先解锁）。 */
                    if (d > 0 && !g_locked) {
                        const struct app *a = app_by_index(d - 1);
                        if (a) {
                            aether_log(AETHER_LOG_INFO,
                                       "systemui: digit key %d -> launch %s", d, a->id);
                            launch_app(a);
                            g_napps = apps_load();   /* 应用可能新增 */
                            draw_screen(g_px);
                            if (ensure_window() < 0)
                                aether_log(AETHER_LOG_WARN,
                                           "systemui: refresh after launch failed");
                        }
                    }
                }
                if (srv >= 0) {
                    int idx = (kfd >= 0) ? 1 : 0;
                    if (pfds[idx].revents & POLLIN) {
                        int c = accept(srv, NULL, NULL);
                        if (c >= 0) {
                            char req[256];
                            int n = (int)read(c, req, sizeof req - 1);
                            if (n > 0) {
                                req[n] = '\0';
                                char *nl = strpbrk(req, "\r\n");
                                if (nl) *nl = '\0';
                                if (!strcmp(req, "status"))               sysui_status(c);
                                else if (!strncmp(req, "launch ", 7))     sysui_launch(c, req + 7);
                                else if (!strcmp(req, "unlock"))
                                    sysui_set_locked(0, "ipc");
                                else if (!strcmp(req, "lock"))
                                    sysui_set_locked(1, "ipc");
                                else if (!strcmp(req, "refresh off")) {
                                    g_refresh = 0;
                                    const char *ok = "{\"schema\":\"aetheros.sysui/1\",\"ok\":true,\"refresh\":false}\n";
                                    if (write(c, ok, strlen(ok)) < 0) { }
                                } else if (!strcmp(req, "refresh on")) {
                                    g_refresh = 1;
                                    const char *ok = "{\"schema\":\"aetheros.sysui/1\",\"ok\":true,\"refresh\":true}\n";
                                    if (write(c, ok, strlen(ok)) < 0) { }
                                } else if (!strncmp(req, "tap ", 4)) {
                                    int lx = 0, ly = 0;
                                    if (sscanf(req + 4, "%d %d", &lx, &ly) == 2)
                                        sysui_tap(lx, ly);
                                    const char *ok = "{\"schema\":\"aetheros.sysui/1\",\"ok\":true}\n";
                                    if (write(c, ok, strlen(ok)) < 0) { }
                                } else {
                                    const char *bad = "{\"schema\":\"aetheros.sysui/1\",\"ok\":false,\"error\":\"unknown command\"}\n";
                                    if (write(c, bad, strlen(bad)) < 0) { }
                                }
                            }
                            close(c);
                        }
                    }
                }
            }
        } else {
            usleep(200000);
        }

        struct timespec tsn;
        clock_gettime(CLOCK_MONOTONIC, &tsn);
        long now = tsn.tv_sec * 1000 + tsn.tv_nsec / 1000000;
        if (g_refresh && now - t0 - last_refresh_ms >= REFRESH_MS) {
            last_refresh_ms = now - t0;
            g_napps = apps_load();          /* 索引可能变化（运行时安装） */
            draw_screen(g_px);
            if (ensure_window() < 0)
                aether_log(AETHER_LOG_WARN, "systemui: periodic refresh failed");
        }
    }

    munmap(g_px, sz);
    close(g_memfd);
    if (srv >= 0) { close(srv); unlink(SYSUI_SOCK); }
    if (kfd >= 0) close(kfd);
    aether_log(AETHER_LOG_INFO, "systemui: stopped");
    return 0;
}
