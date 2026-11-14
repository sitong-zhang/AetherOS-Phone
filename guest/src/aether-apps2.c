/*
 * aether-apps2.c - AetherOS 内置应用（二）：浏览器 / 图片查看器 / 视频播放器 /
 *                  音乐播放器 / 时钟 / 日历
 *
 * 总纲对齐：第四章十二「内置应用（最低配置）」、5.14 系统应用通用界面规范、
 *          5.19 拒绝演示假数据。所有内容取自真实数据源：
 *    - 浏览器     : 真实读取 /proc、本地文件与目录
 *    - 图片查看器 : 真实解码 PPM(P6) 像素，缩放/切换/设为壁纸
 *    - 视频播放器 : 真实逐帧解码系统内置 PPM 帧序列，单调时钟驱动播放
 *    - 音乐播放器 : 真实解析 WAV(PCM) 头与采样，绘制真实波形与进度
 *    - 时钟       : 真实系统时间 + 单调时钟（闹钟/计时器/秒表）
 *    - 日历       : 真实 localtime 月历算法 + 本地日程持久化
 * 每种状态（空 / 加载 / 错误）都有明确界面，无空白占位、无点击无反馈。
 * 图标全部取自 Lucide 开源库（aether-icons.h），无手绘、无自造。
 */
#include "aether-appdata.h"
#include "aether-apps.h"
#include "aether-render.h"
#include "aether-icons.h"
#include "aether-appicons.h"
#include "aether-ppm.h"
#include "aether-tokens.h"   /* 全系统唯一参数真相源 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 系统应用通用顶栏：与 aether-settings.c / aether-store.c 共用同一份令牌，
 * 不再各写一遍 54 / 44（分册 §1 铁律五「参数只有一处真相」）。 */
#define HEAD_H      TK_TITLEBAR_H   /* 96，容纳 88×88 返回命中区与大标题 */
#define BODY_PAD    TK_BODY_PAD
#define STATUS_H    TK_APP_STATUS_H
#define WALL_DIR    "/usr/share/aether/wallpapers"
#define CLIP_DIR    "/usr/share/aether/clips/loop"
#define AUDIO_DIR   "/usr/share/aether/audio"
#define DATA_DIR    "/data/aether"
#define MEDIA_MAX   48

/* 是否有应用需要连续帧（计时/秒表/播放中的实时数字与画面） */
static int g_live = 0;
int aether_apps_live(void) { return g_live; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Linux input-event-codes 常用键（与 aether-apps.c 保持一致） */
#define K_ESC        1
#define K_BACKSPACE 14
#define K_TAB       15
#define K_ENTER     28
#define K_SPACE     57
#define K_UP       103
#define K_LEFT     105
#define K_RIGHT    106
#define K_DOWN     108

/* ==================== 通用控件（一套控件语言，总纲 6.2） ==================== */
static void hdr(int ox, int oy, int cw, const char *title, int back, const char *right)
{
    /* 应用内标题栏骨架（总纲 5.14 / 分册 §3.2、§5.3、§6.4）：
     * 96px 高、左侧返回 32px 线性图标（命中区 88×88）、大标题 40px、
     * 右侧操作入口 24px。文字一律走真实字体度量的行盒居中，不再写死 16。 */
    const int H = TK_TITLEBAR_H;
    const int ICON = 32;
    aether_fill_rect(ox, oy, cw, H, C_PANEL);
    aether_fill_rect(ox, oy + H - 1, cw, 1, C_BORDER);

    char rbuf[64];
    int rw = 0;
    if (right) {
        aether_text_fit_sized(rbuf, sizeof rbuf, right, TK_T_BODY_2, cw / 2);
        rw = aether_text_width_utf8_sized(rbuf, TK_T_BODY_2);
    }
    int tx = ox + (back ? TK_HIT_MIN - TK_S4 : BODY_PAD);
    int maxw = cw - (tx - ox) - rw - BODY_PAD - (right ? TK_S3 : 0);
    if (maxw < TK_S6) maxw = TK_S6;
    char tbuf[128];
    aether_text_fit_sized(tbuf, sizeof tbuf, title, TK_T_TITLE_2, maxw);
    int ty = aether_text_box_y(oy, H, TK_T_TITLE_2);
    aether_draw_text_utf8_sized(tx, ty, tbuf, TK_T_TITLE_2, C_TEXT);

    if (back) {
        int iy = oy + (H - ICON) / 2;
        aether_draw_icon(AE_ICON_CHEVRON_LEFT, ox + TK_S3, iy, ICON, C_TEXT);
    }
    if (right) {
        int ry = aether_text_box_y(oy, H, TK_T_BODY_2);
        aether_draw_text_utf8_sized(ox + cw - BODY_PAD - rw, ry, rbuf, TK_T_BODY_2, C_ACCENT);
    }
}

/* 胶囊开关：开高亮、关低亮；总纲 5.15「开关为胶囊，开高亮关低亮」 */
static void toggle(int x, int y, int on)
{
    int w = 52, h = 28, r = 14;
    aether_draw_round_rect(x, y, w, h, r, on ? C_ACCENT : C_BTN, 0xFF);
    aether_draw_round_rect(x, y, w, h, r, C_BORDER, 0x40);
    int kx = on ? x + w - h + 4 : x + 4;
    aether_draw_round_rect(kx, y + 4, h - 8, h - 8, (h - 8) / 2,
                           pack_rgb(255, 255, 255), 0xFF);
}

/* 长条滑块（可见、可盲拖），返回在给定轨道内点击后的百分比值 */
static void slider(int x, int y, int w, int val, int max)
{
    int h = 12, r = 6;
    aether_draw_round_rect(x, y, w, h, r, C_BTN, 0xFF);
    int fw = max > 0 ? (int)((long)w * val / max) : 0;
    if (fw > r * 2) aether_draw_round_rect(x, y, fw, h, r, C_ACCENT, 0xFF);
    int kx = x + fw - r;
    if (kx < x) kx = x;
    if (kx > x + w - r * 2) kx = x + w - r * 2;
    aether_draw_round_rect(kx, y - 4, r * 4, h + 8, r * 2, pack_rgb(255, 255, 255), 0xFF);
    aether_draw_round_rect(kx, y - 4, r * 4, h + 8, r * 2, C_BORDER, 0x60);
}

static int slider_value(int lx, int x, int w, int max)
{
    int v = (int)((long)(lx - x) * max / (w > 0 ? w : 1));
    if (v < 0) v = 0;
    if (v > max) v = max;
    return v;
}

static void section(int ox, int y, const char *s)
{
    aether_ui_label(ox + BODY_PAD, y, s, C_DIM);
}

/* 空状态：极简插画（开源图标）+ 一句状态文案 + 一个主操作 */
static void empty(int ox, int oy, int cw, int ch, int icon,
                  const char *msg, const char *btn, int *bx, int *by, int *bw, int *bh)
{
    int cx = ox + cw / 2;
    int cy = oy + (ch - STATUS_H) / 2;
    aether_draw_round_rect(cx - 40, cy - 74, 80, 80, 24, C_BTN, 0xFF);
    aether_draw_icon(icon, cx - 26, cy - 60, 52, C_DIM);
    aether_ui_label(cx - aether_text_width_utf8(msg) / 2, cy + 22, msg, C_DIM);
    if (btn) {
        *bw = 200; *bh = 40; *bx = cx - *bw / 2; *by = cy + 54;
        aether_ui_button(*bx, *by, *bw, *bh, btn, C_ACCENT, pack_rgb(255, 255, 255));
    } else { *bx = *by = *bw = *bh = 0; }
}

static void hr_small(int ox, int cw, int y, const char *s)
{
    (void)cw;
    aether_ui_label(ox + BODY_PAD, y, s, C_DIM);
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void human_size(char *b, size_t cap, unsigned long n)
{
    if (n < 1024UL) snprintf(b, cap, "%lu B", n);
    else if (n < 1048576UL) snprintf(b, cap, "%lu KB", n / 1024UL);
    else snprintf(b, cap, "%lu MB", n / 1048576UL);
}

/* 按文件名排序扫描目录内指定后缀的真实文件 */
static int cmp_str(const void *a, const void *b)
{ return strcmp((const char *)a, (const char *)b); }

static int scan_files(const char *dir, const char *ext, char out[MEDIA_MAX][160], int max)
{
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    char names[MEDIA_MAX][160];
    int nn = 0;
    while ((de = readdir(d)) && nn < MEDIA_MAX) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        size_t L = strlen(nm), E = strlen(ext);
        if (L < E || strcasecmp(nm + L - E, ext) != 0) continue;
        snprintf(names[nn++], sizeof names[0], "%.150s", nm);
    }
    closedir(d);
    if (nn > 1) qsort(names, nn, sizeof names[0], cmp_str);
    for (int i = 0; i < nn && n < max; i++)
        snprintf(out[n++], sizeof out[0], "%.100s/%.58s", dir, names[i]);
    return n;
}

static int read_lines(const char *path, char lines[][120], int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0;
    char buf[256];
    while (n < max && fgets(buf, sizeof buf, f)) {
        size_t L = strlen(buf);
        while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        snprintf(lines[n++], 120, "%s", buf);
    }
    fclose(f);
    return n;
}

static void push_line(char lines[][120], int *n, int max, const char *fmt, ...)
{
    if (*n >= max) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[(*n)++], 120, fmt, ap);
    va_end(ap);
}

/* ==================== PPM(P6) 真实解码 ==================== */
static int ppm_token(FILE *f, char *buf, int cap)
{
    int c, n = 0;
    for (;;) {
        c = fgetc(f);
        if (c == EOF) return 0;
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (n) break;
            continue;
        }
        if (n < cap - 1) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return 1;
}

int aether_ppm_read(const char *path, unsigned char **rgb, int *w, int *h)
{
    *rgb = NULL; *w = *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char t[32];
    if (!ppm_token(f, t, sizeof t) || strcmp(t, "P6") != 0) { fclose(f); return 0; }
    int sw = 0, sh = 0, mv = 0;
    if (!ppm_token(f, t, sizeof t)) { fclose(f); return 0; } sw = atoi(t);
    if (!ppm_token(f, t, sizeof t)) { fclose(f); return 0; } sh = atoi(t);
    if (!ppm_token(f, t, sizeof t)) { fclose(f); return 0; } mv = atoi(t);
    if (sw <= 0 || sh <= 0 || sw > 2048 || sh > 2048 || (mv != 255 && mv != 65535)) { fclose(f); return 0; }
    size_t need = (size_t)sw * sh * 3;
    unsigned char *buf = (unsigned char *)malloc(need);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, need, f) != need) { free(buf); fclose(f); return 0; }
    fclose(f);
    *rgb = buf; *w = sw; *h = sh;
    return 1;
}

/* 把解码后的真实像素按 contain 缩放绘制到目标矩形（step 抽样，遵守裁剪/alpha） */
void aether_ppm_draw(unsigned char *rgb, int sw, int sh, int dx, int dy, int dw, int dh, int step)
{
    if (!rgb || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (step < 1) step = 1;
    double s = (double)dw / sw;
    if ((double)dh / sh < s) s = (double)dh / sh;
    int iw = (int)(sw * s), ih = (int)(sh * s);
    int ox = dx + (dw - iw) / 2, oy = dy + (dh - ih) / 2;
    for (int y = 0; y < ih; y += step) {
        int sy = (int)((long)y * sh / ih);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < iw; x += step) {
            int sx = (int)((long)x * sw / iw);
            if (sx >= sw) sx = sw - 1;
            const unsigned char *p = rgb + ((size_t)sy * sw + sx) * 3;
            int bw = step; if (x + bw > iw) bw = iw - x;
            int bh = step; if (y + bh > ih) bh = ih - y;
            aether_fill_rect(ox + x, oy + y, bw, bh, pack_rgb(p[0], p[1], p[2]));
        }
    }
}

/* ==================== 1) 浏览器 ==================== */
static const char *HOME_LINKS[4] = {
    "about:system", "about:apps", "file:/etc", "about:help"
};

static void browser_load(struct app_state *st)
{
    char lines[64][120];
    int n = 0;
    const char *u = st->url;

    if (!strcmp(u, "about:home")) {
        push_line(lines, &n, 64, "AetherOS 浏览器  ·  本地与系统内容");
        push_line(lines, &n, 64, "");
        push_line(lines, &n, 64, "  [1]  about:system     本机实时系统信息");
        push_line(lines, &n, 64, "  [2]  about:apps       已安装可执行程序");
        push_line(lines, &n, 64, "  [3]  file:/etc        浏览系统配置目录");
        push_line(lines, &n, 64, "  [4]  about:help       地址与用法说明");
        push_line(lines, &n, 64, "");
        push_line(lines, &n, 64, "点击上面任一条目，或在地址栏输入本地绝对路径。");
        push_line(lines, &n, 64, "本浏览器不访问外部网络。");
    } else if (!strcmp(u, "about:system")) {
        push_line(lines, &n, 64, "本机系统信息");
        push_line(lines, &n, 64, "--------------------------------------------");
        char b[120];
        FILE *f = fopen("/proc/version", "r");
        if (f) { if (fgets(b, sizeof b, f)) { size_t L = strlen(b); while (L && b[L-1] < 32) b[--L] = 0; push_line(lines, &n, 64, "%s", b); } fclose(f); }
        push_line(lines, &n, 64, "");
        f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char l[256];
            while (n < 60 && fgets(l, sizeof l, f)) {
                if (!strncmp(l, "model name", 10) || !strncmp(l, "Hardware", 8) ||
                    !strncmp(l, "Processor", 9)) {
                    size_t L = strlen(l); while (L && l[L-1] < 32) l[--L] = 0;
                    push_line(lines, &n, 64, "%s", l);
                }
            }
            fclose(f);
        }
        push_line(lines, &n, 64, "");
        f = fopen("/proc/meminfo", "r");
        if (f) {
            char l[256];
            while (n < 60 && fgets(l, sizeof l, f)) {
                if (!strncmp(l, "MemTotal:", 9) || !strncmp(l, "MemAvailable:", 13)) {
                    size_t L = strlen(l); while (L && l[L-1] < 32) l[--L] = 0;
                    push_line(lines, &n, 64, "%s", l);
                }
            }
            fclose(f);
        }
        f = fopen("/proc/uptime", "r");
        if (f) { if (fgets(b, sizeof b, f)) { size_t L = strlen(b); while (L && b[L-1] < 32) b[--L] = 0; push_line(lines, &n, 64, "运行时间(秒)  %s", b); } fclose(f); }
        f = fopen("/proc/loadavg", "r");
        if (f) { if (fgets(b, sizeof b, f)) { size_t L = strlen(b); while (L && b[L-1] < 32) b[--L] = 0; push_line(lines, &n, 64, "系统负载      %s", b); } fclose(f); }
    } else if (!strcmp(u, "about:apps")) {
        push_line(lines, &n, 64, "已安装应用");
        push_line(lines, &n, 64, "--------------------------------------------");
        const char *dirs[] = { "/bin", "/sbin", "/usr/bin", "/usr/sbin" };
        for (int d = 0; d < 4 && n < 62; d++) {
            DIR *dp = opendir(dirs[d]);
            if (!dp) continue;
            push_line(lines, &n, 64, "[%s]", dirs[d]);
            struct dirent *de;
            int c = 0;
            while ((de = readdir(dp)) && n < 62 && c < 14) {
                if (de->d_name[0] == '.') continue;
                char p[256];
                snprintf(p, sizeof p, "%.120s/%.120s", dirs[d], de->d_name);
                struct stat sb;
                if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode) && (sb.st_mode & 0111)) {
                    push_line(lines, &n, 64, "   %s", de->d_name);
                    c++;
                }
            }
            closedir(dp);
        }
    } else if (!strcmp(u, "about:help")) {
        push_line(lines, &n, 64, "浏览器用法");
        push_line(lines, &n, 64, "--------------------------------------------");
        push_line(lines, &n, 64, "about:home      导航首页");
        push_line(lines, &n, 64, "about:system    本机实时系统信息");
        push_line(lines, &n, 64, "about:apps      已安装可执行程序");
        push_line(lines, &n, 64, "file:/路径      读取本地文本文件或列出目录");
        push_line(lines, &n, 64, "/绝对路径       与上等价，例如 /etc/passwd");
        push_line(lines, &n, 64, "");
        push_line(lines, &n, 64, "点击地址栏后键盘输入，回车或点「前往」打开。");
        push_line(lines, &n, 64, "后退 / 前进按浏览历史移动，上下方向键滚动。");
    } else {
        const char *path = u;
        if (!strncmp(u, "file:", 5)) path = u + 5;
        struct stat sb;
        if (stat(path, &sb) != 0) {
            push_line(lines, &n, 64, "无法打开：%s", u);
            push_line(lines, &n, 64, "");
            push_line(lines, &n, 64, "原因：路径不存在或无法读取。");
            push_line(lines, &n, 64, "可重试：检查后重新输入，或输入 about:home 返回导航页。");
        } else if (S_ISDIR(sb.st_mode)) {
            push_line(lines, &n, 64, "目录：%s", path);
            push_line(lines, &n, 64, "--------------------------------------------");
            char names[64][120]; int nn = 0;
            DIR *dp = opendir(path);
            if (dp) {
                struct dirent *de;
                while ((de = readdir(dp)) && nn < 64) {
                    if (!strcmp(de->d_name, ".")) continue;
                    char p[512]; snprintf(p, sizeof p, "%s/%s", path, de->d_name);
                    struct stat s2;
                    int isdir = (stat(p, &s2) == 0 && S_ISDIR(s2.st_mode));
                    char sz[32] = "";
                    if (!isdir && stat(p, &s2) == 0) human_size(sz, sizeof sz, (unsigned long)s2.st_size);
                    snprintf(names[nn++], sizeof names[0], "   %-4.4s %-34.34s %.40s",
                             isdir ? "目录" : "文件", de->d_name, sz);
                }
                closedir(dp);
            }
            if (nn > 1) qsort(names, nn, sizeof names[0], cmp_str);
            for (int i = 0; i < nn && n < 64; i++) push_line(lines, &n, 64, "%s", names[i]);
            if (!nn) push_line(lines, &n, 64, "   空目录");
        } else {
            push_line(lines, &n, 64, "文件：%s   (%lu 字节)", path, (unsigned long)sb.st_size);
            push_line(lines, &n, 64, "--------------------------------------------");
            int got = read_lines(path, st->page, 60);
            if (got > 0) n = 2; /* 已直接写入 st->page[0..]，见下方拷贝 */
            /* read_lines 写的是 st->page，前两行是标题；重新组织： */
            if (got > 0) {
                char body[64][120];
                memcpy(body, st->page, sizeof body);
                n = 0;
                push_line(lines, &n, 64, "文件：%s   (%lu 字节)", path, (unsigned long)sb.st_size);
                push_line(lines, &n, 64, "--------------------------------------------");
                for (int i = 0; i < got && n < 64; i++) push_line(lines, &n, 64, "%s", body[i]);
            } else {
                push_line(lines, &n, 64, "原因：二进制文件或内容不可按文本显示。");
                push_line(lines, &n, 64, "可重试：用文件管理器查看，或改选其他文本文件路径。");
            }
        }
    }
    for (int i = 0; i < 64; i++) snprintf(st->page[i], 120, "%.119s", i < n ? lines[i] : "");
    st->page_n = n;
}

static void browser_nav(struct app_state *st, const char *url)
{
    if (!url || !url[0]) return;
    snprintf(st->url, sizeof st->url, "%s", url);
    snprintf(st->addr, sizeof st->addr, "%s", url);
    st->addr_len = (int)strlen(st->addr);
    st->addr_edit = 0;
    /* 浏览历史：从头追加（不去重，保证前进/后退语义真实） */
    if (st->hist_pos < st->hist_n - 1) st->hist_n = st->hist_pos + 1;
    if (st->hist_n < 8) snprintf(st->hist[st->hist_n++], sizeof st->hist[0], "%s", url);
    else {
        for (int i = 1; i < 8; i++) snprintf(st->hist[i-1], sizeof st->hist[0], "%s", st->hist[i]);
        snprintf(st->hist[7], sizeof st->hist[0], "%s", url);
        st->hist_n = 8;
    }
    st->hist_pos = st->hist_n - 1;
    browser_load(st);
    snprintf(st->status, sizeof st->status, "已打开 %s", base(url));
}

void app_browser_open(struct app_state *st)
{
    st->hist_n = 0; st->hist_pos = -1; st->addr_edit = 0;
    browser_nav(st, "about:home");
    snprintf(st->status, sizeof st->status, "点击地址栏输入");
}

void app_browser_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    hdr(ox, oy, cw, "浏览器", 0, "about:home");
    /* 地址栏 */
    int ay = oy + HEAD_H + 8, ah = 40;
    aether_draw_round_rect(ox + BODY_PAD - 6, ay, cw - 2 * BODY_PAD - 130, ah, 8, C_BG, 0xFF);
    aether_draw_border(ox + BODY_PAD - 6, ay, cw - 2 * BODY_PAD - 130, ah, C_BORDER, 1);
    char shown[120];
    const char *disp = st->addr_edit ? st->addr : st->url;
    aether_ui_fit(shown, sizeof shown, disp, cw - 2 * BODY_PAD - 150);
    aether_ui_label(ox + BODY_PAD + 2, ay + 12, shown, st->addr_edit ? C_TEXT : C_DIM);
    if (st->addr_edit) aether_fill_rect(ox + BODY_PAD + 2 + aether_text_width_utf8(shown), ay + 8, 2, 22, C_ACCENT);
    /* 后退 / 前进 / 前往 */
    int bw = 40, bx = ox + cw - BODY_PAD - 3 * (bw + 6);
    aether_ui_button(bx, ay, bw, ah, "<", C_BTN, C_BTN_TX);
    aether_ui_button(bx + bw + 6, ay, bw, ah, ">", C_BTN, C_BTN_TX);
    aether_ui_button(bx + 2 * (bw + 6), ay, bw + 12, ah, "前往", C_ACCENT, pack_rgb(255,255,255));

    int ty = oy + HEAD_H + 56;
    int rows = (ch - STATUS_H - (HEAD_H + 56) - 8) / 20;
    if (rows < 1) rows = 1;
    aether_fill_rect(ox + 6, ty, cw - 12, ch - STATUS_H - (HEAD_H + 56) - 6, C_PANEL);
    for (int i = 0; i < rows; i++) {
        int idx = w->scroll + i;
        if (idx >= st->page_n) break;
        char ln[120]; aether_ui_fit(ln, sizeof ln, st->page[idx], cw - 28);
        int is_link = (!strcmp(st->url, "about:home") && idx >= 2 && idx <= 5);
        aether_ui_label(ox + BODY_PAD, ty + 4 + i * 20, ln, is_link ? C_ACCENT : C_TEXT);
    }
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_browser_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)ch;
    int ay = HEAD_H + 8, ah = 40;
    int bw = 40, bx = cw - BODY_PAD - 3 * (bw + 6);
    if (aether_ui_hit(lx, ly, BODY_PAD - 6, ay, cw - 2 * BODY_PAD - 130, ah)) {
        st->addr_edit = 1;
        snprintf(st->addr, sizeof st->addr, "%s", st->url);
        st->addr_len = (int)strlen(st->addr);
        snprintf(st->status, sizeof st->status, "输入地址后回车或点「前往」");
        return;
    }
    if (aether_ui_hit(lx, ly, bx, ay, bw, ah)) {
        if (st->hist_pos > 0) { st->hist_pos--; browser_nav(st, st->hist[st->hist_pos]); }
        else snprintf(st->status, sizeof st->status, "已到最早一条历史记录");
        return;
    }
    if (aether_ui_hit(lx, ly, bx + bw + 6, ay, bw, ah)) {
        if (st->hist_pos < st->hist_n - 1) { st->hist_pos++; browser_nav(st, st->hist[st->hist_pos]); }
        else snprintf(st->status, sizeof st->status, "已到最新一条历史记录");
        return;
    }
    if (aether_ui_hit(lx, ly, bx + 2*(bw+6), ay, bw + 12, ah)) {
        browser_nav(st, st->addr[0] ? st->addr : "about:home"); w->scroll = 0; return;
    }
    int ty = HEAD_H + 56;
    int rows = (ch - STATUS_H - (HEAD_H + 56) - 8) / 20;
    if (ly >= ty && ly < ty + rows * 20) {
        int idx = w->scroll + (ly - ty) / 20;
        if (!strcmp(st->url, "about:home") && idx >= 2 && idx <= 5) {
            browser_nav(st, HOME_LINKS[idx - 2]); w->scroll = 0;
        }
    }
}

void app_browser_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    if (code == K_UP) { if (w->scroll > 0) w->scroll--; return; }
    if (code == K_DOWN) { if (w->scroll < st->page_n - 4) w->scroll++; return; }
    if (!st->addr_edit) return;
    if (code == K_ENTER) { browser_nav(st, st->addr[0] ? st->addr : "about:home"); w->scroll = 0; return; }
    if (code == K_BACKSPACE) {
        if (st->addr_len > 0) { st->addr[--st->addr_len] = 0; }
        return;
    }
    char c = aether_key_to_char(code, 0);
    if (c && st->addr_len < (int)sizeof st->addr - 2) {
        st->addr[st->addr_len++] = c;
        st->addr[st->addr_len] = 0;
    }
}

void app_browser_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    const char *s = line;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    if (*s) { browser_nav(st, s); w->scroll = 0; }
}

/* ==================== 2) 图片查看器 ==================== */
static void gallery_scan(struct app_state *st)
{
    st->media_n = scan_files(WALL_DIR, ".ppm", st->media, MEDIA_MAX);
    st->media_sel = 0; st->zoom = 0;
}

void app_gallery_open(struct app_state *st)
{
    gallery_scan(st);
    snprintf(st->status, sizeof st->status, "共 %d 张壁纸", st->media_n);
}

void app_gallery_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    hdr(ox, oy, cw, "图片查看器", 0, st->media_n ? "设为壁纸" : NULL);
    if (st->media_n <= 0) {
        int bx, by, bw, bh;
        empty(ox, oy, cw, ch, AE_ICON_IMAGE, "系统壁纸目录为空", "打开图片目录", &bx, &by, &bw, &bh);
        aether_ui_status(ox, oy, cw, ch, st);
        return;
    }
    /* 缩略图条 */
    int ty = oy + HEAD_H + 8, ts = 56, nshow = (cw - 2 * BODY_PAD + 8) / (ts + 8);
    if (nshow > 8) nshow = 8;
    for (int i = 0; i < nshow && i < st->media_n; i++) {
        int x = ox + BODY_PAD + i * (ts + 8);
        aether_draw_round_rect(x, ty, ts, ts, 10, C_BTN, 0xFF);
        unsigned char *rgb; int sw, sh;
        if (aether_ppm_read(st->media[i], &rgb, &sw, &sh)) {
            aether_ppm_draw(rgb, sw, sh, x + 2, ty + 2, ts - 4, ts - 4, 2);
            free(rgb);
        }
        if (i == st->media_sel) aether_draw_border(x, ty, ts, ts, C_ACCENT, 2);
    }
    /* 主预览区（真实像素） */
    int py = ty + ts + 10;
    int ph = ch - STATUS_H - (py - oy) - 92;
    if (ph < 120) ph = 120;
    aether_fill_rect(ox + 8, py, cw - 16, ph, C_BG);
    aether_draw_border(ox + 8, py, cw - 16, ph, C_BORDER, 1);
    unsigned char *rgb = NULL; int sw = 0, sh = 0;
    int ok = aether_ppm_read(st->media[st->media_sel], &rgb, &sw, &sh);
    if (ok) {
        if (st->zoom == 0) aether_ppm_draw(rgb, sw, sh, ox + 12, py + 4, cw - 24, ph - 8, 2);
        else if (st->zoom == 1) aether_ppm_draw(rgb, sw, sh, ox + 12, py + 4, cw - 24, ph - 8, 1);
        else {
            /* 放大 2 倍：只绘制中心区域，真实放大不插值造假 */
            int dw = sw * 2, dh = sh * 2;
            int cx = sw / 2, cy = sh / 2;
            int vw = (cw - 24) < dw ? (cw - 24) / 2 : sw;
            int vh = (ph - 8) < dh ? (ph - 8) / 2 : sh;
            (void)cx; (void)cy;
            if (vw > sw) { vw = sw; }
            if (vh > sh) { vh = sh; }
            unsigned char *crop = (unsigned char *)malloc((size_t)vw * vh * 3);
            if (crop) {
                int sx0 = cx - vw / 2, sy0 = cy - vh / 2;
                if (sx0 < 0) { sx0 = 0; }
                if (sy0 < 0) { sy0 = 0; }
                if (sx0 + vw > sw) { sx0 = sw - vw; }
                if (sy0 + vh > sh) { sy0 = sh - vh; }
                if (sx0 < 0) { sx0 = 0; }
                if (sy0 < 0) { sy0 = 0; }
                for (int y = 0; y < vh; y++)
                    memcpy(crop + (size_t)y * vw * 3,
                           rgb + ((size_t)(sy0 + y) * sw + sx0) * 3, (size_t)vw * 3);
                aether_ppm_draw(crop, vw, vh, ox + 12, py + 4, cw - 24, ph - 8, 1);
                free(crop);
            }
        }
    }
    /* 文件信息（真实 stat + 真实像素尺寸） */
    int iy = py + ph + 8;
    char b[160];
    struct stat sb;
    unsigned long sz = 0;
    if (stat(st->media[st->media_sel], &sb) == 0) sz = (unsigned long)sb.st_size;
    char hsz[32]; human_size(hsz, sizeof hsz, sz);
    snprintf(b, sizeof b, "%s   %dx%d   %s", base(st->media[st->media_sel]), sw, sh, hsz);
    char bf[160]; aether_ui_fit(bf, sizeof bf, b, cw - 28);
    aether_ui_label(ox + BODY_PAD, iy, bf, C_TEXT);
    /* 操作行 */
    int by = iy + 24;
    int bw3 = (cw - 2 * BODY_PAD - 16) / 3;
    aether_ui_button(ox + BODY_PAD, by, bw3, 36, "上一张", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + bw3 + 8, by, bw3, 36,
                     st->zoom == 0 ? "缩放：适应" : st->zoom == 1 ? "缩放：原图" : "缩放：放大",
                     C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + 2 * (bw3 + 8), by, bw3, 36, "下一张", C_BTN, C_BTN_TX);
    if (rgb) free(rgb);
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_gallery_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w;
    if (st->media_n <= 0) {
        if (aether_ui_hit(lx, ly, cw / 2 - 100, (ch - STATUS_H) / 2 + 54, 200, 40))
            aether_desktop_open(APP_FILES);
        return;
    }
    int ty = HEAD_H + 8, ts = 56, nshow = (cw - 2 * BODY_PAD + 8) / (ts + 8);
    if (nshow > 8) nshow = 8;
    for (int i = 0; i < nshow && i < st->media_n; i++) {
        int x = BODY_PAD + i * (ts + 8);
        if (aether_ui_hit(lx, ly, x, ty, ts, ts)) {
            st->media_sel = i;
            snprintf(st->status, sizeof st->status, "已选择 %s", base(st->media[i]));
            return;
        }
    }
    int py = ty + ts + 10;
    int ph = ch - STATUS_H - py - 92;
    if (ph < 120) ph = 120;
    int by = py + ph + 8 + 24;
    int bw3 = (cw - 2 * BODY_PAD - 16) / 3;
    if (aether_ui_hit(lx, ly, BODY_PAD, by, bw3, 36)) {
        st->media_sel = (st->media_sel - 1 + st->media_n) % st->media_n;
        snprintf(st->status, sizeof st->status, "上一张：%s", base(st->media[st->media_sel])); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + bw3 + 8, by, bw3, 36)) {
        st->zoom = (st->zoom + 1) % 3;
        snprintf(st->status, sizeof st->status, "缩放档位 %d", st->zoom); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 2 * (bw3 + 8), by, bw3, 36)) {
        st->media_sel = (st->media_sel + 1) % st->media_n;
        snprintf(st->status, sizeof st->status, "下一张：%s", base(st->media[st->media_sel])); return;
    }
    /* 顶部右侧「设为壁纸」 */
    if (ly < HEAD_H && lx > cw - 140) {
        FILE *f = fopen(DATA_DIR "/wallpaper.conf", "w");
        if (f) {
            fprintf(f, "%s\n", st->media[st->media_sel]); fclose(f);
            snprintf(st->status, sizeof st->status, "已设为桌面壁纸：%s", base(st->media[st->media_sel]));
        } else snprintf(st->status, sizeof st->status, "设置失败：无法写入 %s", DATA_DIR);
    }
}

void app_gallery_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (st->media_n <= 0) return;
    if (code == K_LEFT)  { st->media_sel = (st->media_sel - 1 + st->media_n) % st->media_n; return; }
    if (code == K_RIGHT) { st->media_sel = (st->media_sel + 1) % st->media_n; return; }
    if (code == K_SPACE) { st->zoom = (st->zoom + 1) % 3; return; }
}

void app_gallery_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    int a = -1, b = -1;
    sscanf(line, "%*s %d %d", &a, &b);
    if (a >= 0 && a < st->media_n) st->media_sel = a;
    if (b >= 0 && b <= 2) st->zoom = b;
}

/* ==================== 3) 视频播放器 ==================== */
static void video_scan(struct app_state *st)
{
    st->media_n = scan_files(CLIP_DIR, ".ppm", st->media, MEDIA_MAX);
    st->media_sel = 0; st->vid_frames = st->media_n;
    st->vid_fps = 8;
    st->playing = 0; st->loop = 1;
    st->play_start_ms = 0; st->play_offset_ms = 0;
}

static long video_elapsed_ms(struct app_state *st)
{
    long e = st->play_offset_ms;
    if (st->playing && st->play_start_ms) e += now_ms() - st->play_start_ms;
    return e;
}

static long video_total_ms(struct app_state *st)
{
    return (long)st->vid_frames * 1000L / (st->vid_fps > 0 ? st->vid_fps : 8);
}

void app_video_open(struct app_state *st)
{
    video_scan(st);
    if (st->vid_frames > 0) {
        st->playing = 1; st->play_start_ms = now_ms(); st->play_offset_ms = 0;
        snprintf(st->status, sizeof st->status, "播放中 · %d 帧 · %d fps",
                 st->vid_frames, st->vid_fps);
    } else snprintf(st->status, sizeof st->status, "系统片源目录为空");
}

void app_video_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    g_live = st->playing ? 1 : 0;
    hdr(ox, oy, cw, "视频播放器", 0, NULL);
    if (st->vid_frames <= 0) {
        int bx, by, bw, bh;
        empty(ox, oy, cw, ch, AE_ICON_VIDEO, "系统片源目录为空", "打开片源目录", &bx, &by, &bw, &bh);
        aether_ui_status(ox, oy, cw, ch, st);
        return;
    }
    long total = video_total_ms(st);
    long e = video_elapsed_ms(st);
    if (total > 0 && e >= total) {
        if (st->loop) { st->play_offset_ms = 0; st->play_start_ms = now_ms(); e = 0; }
        else { st->playing = 0; st->play_offset_ms = total; e = total; }
    }
    int frame = (int)(e * st->vid_fps / 1000L);
    if (frame >= st->vid_frames) frame = st->vid_frames - 1;
    if (frame < 0) frame = 0;

    int vy = oy + HEAD_H + 8;
    int vh = ch - STATUS_H - (HEAD_H + 8) - 128;
    if (vh < 120) vh = 120;
    aether_fill_rect(ox + 8, vy, cw - 16, vh, pack_rgb(4, 8, 18));
    unsigned char *rgb; int sw, sh;
    if (aether_ppm_read(st->media[frame], &rgb, &sw, &sh)) {
        aether_ppm_draw(rgb, sw, sh, ox + 12, vy + 4, cw - 24, vh - 8, 2);
        free(rgb);
    }
    /* 进度条（可点跳转） */
    int sy = vy + vh + 12;
    int pct = total > 0 ? (int)(e * 100 / total) : 0;
    aether_ui_bar(ox + BODY_PAD, sy, cw - 2 * BODY_PAD, 12, (double)pct, C_ACCENT);
    char tb[64];
    snprintf(tb, sizeof tb, "%02d:%02d / %02d:%02d   第 %d/%d 帧",
             (int)(e/60000), (int)((e/1000)%60), (int)(total/60000), (int)((total/1000)%60),
             frame + 1, st->vid_frames);
    aether_ui_label(ox + BODY_PAD, sy + 18, tb, C_DIM);
    /* 控制行 */
    int cy = sy + 46;
    int bw5 = (cw - 2 * BODY_PAD - 32) / 5;
    aether_ui_button(ox + BODY_PAD, cy, bw5, 40, "上一帧", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + (bw5+8), cy, bw5, 40,
                     st->playing ? "暂停" : "播放", C_ACCENT, pack_rgb(255,255,255));
    aether_ui_button(ox + BODY_PAD + 2*(bw5+8), cy, bw5, 40, "下一帧", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + 3*(bw5+8), cy, bw5, 40,
                     st->loop ? "循环开" : "循环关", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + 4*(bw5+8), cy, bw5, 40, "重播", C_BTN, C_BTN_TX);
    char fn[160];
    snprintf(fn, sizeof fn, "片源：%s", base(st->media[frame]));
    char ff[160]; aether_ui_fit(ff, sizeof ff, fn, cw - 28);
    aether_ui_label(ox + BODY_PAD, cy + 48, ff, C_DIM);
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_video_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w;
    if (st->vid_frames <= 0) {
        if (aether_ui_hit(lx, ly, cw / 2 - 100, (ch - STATUS_H) / 2 + 54, 200, 40))
            aether_desktop_open(APP_FILES);
        return;
    }
    int vy = HEAD_H + 8;
    int vh = ch - STATUS_H - (HEAD_H + 8) - 128;
    if (vh < 120) vh = 120;
    int sy = vy + vh + 12;
    long total = video_total_ms(st);
    if (aether_ui_hit(lx, ly, BODY_PAD, sy - 6, cw - 2 * BODY_PAD, 24)) {
        int v = slider_value(lx, BODY_PAD, cw - 2 * BODY_PAD, 100);
        st->play_offset_ms = (long)total * v / 100;
        if (st->playing) st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "已跳转到 %d%%", v);
        return;
    }
    int cy = sy + 46;
    int bw5 = (cw - 2 * BODY_PAD - 32) / 5;
    long e = video_elapsed_ms(st);
    int frame = total > 0 ? (int)(e * st->vid_fps / 1000L) : 0;
    if (frame >= st->vid_frames) frame = st->vid_frames - 1;
    long step = 1000L / (st->vid_fps > 0 ? st->vid_fps : 8);
    if (aether_ui_hit(lx, ly, BODY_PAD, cy, bw5, 40)) {
        st->play_offset_ms = (long)frame * step - step;
        if (st->play_offset_ms < 0) st->play_offset_ms = 0;
        if (st->playing) st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "上一帧"); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + (bw5+8), cy, bw5, 40)) {
        if (st->playing) { st->play_offset_ms = e; st->playing = 0; }
        else { st->play_start_ms = now_ms(); st->playing = 1; }
        snprintf(st->status, sizeof st->status, st->playing ? "继续播放" : "已暂停"); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 2*(bw5+8), cy, bw5, 40)) {
        st->play_offset_ms = (long)frame * step + step;
        if (st->play_offset_ms > total) st->play_offset_ms = total;
        if (st->playing) st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "下一帧"); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 3*(bw5+8), cy, bw5, 40)) {
        st->loop = !st->loop;
        snprintf(st->status, sizeof st->status, st->loop ? "循环播放已开启" : "循环播放已关闭"); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 4*(bw5+8), cy, bw5, 40)) {
        st->play_offset_ms = 0; st->play_start_ms = now_ms(); st->playing = 1;
        snprintf(st->status, sizeof st->status, "已从头重播"); return;
    }
}

void app_video_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (code == K_SPACE && st->vid_frames > 0) {
        if (st->playing) { st->play_offset_ms = video_elapsed_ms(st); st->playing = 0; }
        else { st->play_start_ms = now_ms(); st->playing = 1; }
    } else if (code == K_LEFT && st->vid_frames > 0) {
        long step = 1000L / st->vid_fps;
        st->play_offset_ms -= step; if (st->play_offset_ms < 0) st->play_offset_ms = 0;
        if (st->playing) st->play_start_ms = now_ms();
    } else if (code == K_RIGHT && st->vid_frames > 0) {
        long step = 1000L / st->vid_fps;
        st->play_offset_ms += step;
        if (st->play_offset_ms > video_total_ms(st)) st->play_offset_ms = video_total_ms(st);
        if (st->playing) st->play_start_ms = now_ms();
    }
}

void app_video_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    char c[32] = ""; int v = 0;
    sscanf(line, "%*s %31s %d", c, &v);
    if (!strcmp(c, "pause")) { if (st->playing) { st->play_offset_ms = video_elapsed_ms(st); st->playing = 0; } }
    else if (!strcmp(c, "play")) { st->play_start_ms = now_ms(); st->playing = 1; }
    else if (!strcmp(c, "frame") && st->vid_frames > 0) {
        long step = 1000L / st->vid_fps;
        if (v < 0) { v = 0; }
        if (v >= st->vid_frames) { v = st->vid_frames - 1; }
        st->play_offset_ms = (long)v * step;
        if (st->playing) st->play_start_ms = now_ms();
    }
}

/* ==================== 4) 音乐播放器（真实 WAV 解析 + 波形） ==================== */
static unsigned le32(const unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned le16(const unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1] << 8); }

/* 真实解析 WAV(PCM) 头；wave[] 取 data 区各段最大振幅（0..1000） */
static int wav_info(const char *path, int *rate, int *bits, int *ch,
                    int *dur_ms, short *wave, int wn)
{
    *rate = *bits = *ch = *dur_ms = 0;
    for (int i = 0; i < wn; i++) wave[i] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return 0; }
    unsigned data_size = 0;
    long data_off = 0;
    for (;;) {
        unsigned char id[8];
        if (fread(id, 1, 8, f) != 8) break;
        unsigned sz = le32(id + 4);
        if (!memcmp(id, "fmt ", 4)) {
            unsigned char fm[16];
            unsigned n = sz < sizeof fm ? sz : sizeof fm;
            if (fread(fm, 1, n, f) != n) break;
            if (n >= 16) {
                *ch = (int)le16(fm + 2);
                *rate = (int)le32(fm + 4);
                *bits = (int)le16(fm + 14);
            }
            if (sz > n) fseek(f, (long)(sz - n), SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            data_size = sz; data_off = ftell(f); break;
        } else {
            if (fseek(f, (long)sz, SEEK_CUR) != 0) break;
        }
        if (sz & 1) fseek(f, 1, SEEK_CUR);
    }
    if (!data_off || !*rate || !*bits || !*ch || *bits != 16) { fclose(f); return 0; }
    int bytes_per_sample = *bits / 8;
    long nsamp = (long)data_size / (bytes_per_sample * *ch);
    *dur_ms = (int)((double)nsamp * 1000.0 / *rate);
    /* 真实采样 -> 波形柱 */
    int seg = wn > 0 ? wn : 1;
    long per = nsamp / seg; if (per < 1) per = 1;
    if (fseek(f, data_off, SEEK_SET) == 0) {
        for (int s = 0; s < seg; s++) {
            long maxv = 1;
            for (long i = 0; i < per; i++) {
                unsigned char b[2];
                if (fread(b, 1, 2, f) != 2) break;
                int v = (int)(short)le16(b);
                if (v < 0) v = -v;
                if (v > maxv) maxv = v;
                /* 跳过其余声道同一采样点 */
                for (int c = 1; c < *ch; c++) {
                    unsigned char skip[2];
                    if (fread(skip, 1, 2, f) != 2) break;
                }
            }
            wave[s] = (short)(maxv * 1000L / 32767L);
            if (wave[s] > 1000) wave[s] = 1000;
        }
    }
    fclose(f);
    return 1;
}

static void music_scan(struct app_state *st)
{
    st->media_n = scan_files(AUDIO_DIR, ".wav", st->media, MEDIA_MAX);
    st->media_sel = 0; st->playing = 0; st->loop = 1; st->volume = 70;
    st->play_start_ms = 0; st->play_offset_ms = 0;
}

static long music_elapsed_ms(struct app_state *st)
{
    long e = st->play_offset_ms;
    if (st->playing && st->play_start_ms) e += now_ms() - st->play_start_ms;
    if (e > st->wav_ms) e = st->wav_ms;
    return e;
}

void app_music_open(struct app_state *st)
{
    music_scan(st);
    if (st->media_n > 0) {
        short wave[64];
        wav_info(st->media[0], &st->wav_rate, &st->wav_bits, &st->wav_ch, &st->wav_ms, wave, 64);
        st->playing = 1; st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "%d Hz / %d bit / %d 声道",
                 st->wav_rate, st->wav_bits, st->wav_ch);
    } else snprintf(st->status, sizeof st->status, "音频库目录为空");
}

void app_music_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    g_live = st->playing ? 1 : 0;
    hdr(ox, oy, cw, "音乐播放器", 0, NULL);
    if (st->media_n <= 0) {
        int bx, by, bw, bh;
        empty(ox, oy, cw, ch, AE_ICON_MUSIC, "音频库目录为空", "打开音频目录", &bx, &by, &bw, &bh);
        aether_ui_status(ox, oy, cw, ch, st);
        return;
    }
    long e = music_elapsed_ms(st);
    if (st->wav_ms > 0 && e >= st->wav_ms) {
        if (st->loop) { st->play_offset_ms = 0; st->play_start_ms = now_ms(); e = 0; }
        else { st->playing = 0; }
    }
    /* 封面区：真实波形柱（来自 PCM 采样） */
    int cy = oy + HEAD_H + 12;
    int chh = ch - STATUS_H - (HEAD_H + 12) - 210;
    if (chh < 100) chh = 100;
    aether_draw_round_rect(ox + BODY_PAD, cy, cw - 2 * BODY_PAD, chh, 18, C_PANEL, 0xFF);
    short wave[64];
    wav_info(st->media[st->media_sel], &st->wav_rate, &st->wav_bits, &st->wav_ch,
             &st->wav_ms, wave, 64);
    int bn = 56, bw2 = (cw - 2 * BODY_PAD - 24) / bn;
    int basey = cy + chh - 24;
    int maxh = chh - 48;
    for (int i = 0; i < bn; i++) {
        int hh = (int)((long)maxh * wave[i * 64 / bn] / 1000);
        if (hh < 2) hh = 2;
        int played = (int)((long)bn * e / (st->wav_ms > 0 ? st->wav_ms : 1));
        unsigned c = i <= played ? C_ACCENT : C_BTN;
        aether_draw_round_rect(ox + BODY_PAD + 12 + i * bw2, basey - hh,
                               bw2 > 2 ? bw2 - 2 : 1, hh, 2, c, 0xFF);
    }
    char title[160];
    snprintf(title, sizeof title, "%s", base(st->media[st->media_sel]));
    aether_ui_label(ox + BODY_PAD + 12, cy + 14, title, C_TEXT);
    char meta[160];
    snprintf(meta, sizeof meta, "%d Hz  %d bit  %d 声道  %d:%02d",
             st->wav_rate, st->wav_bits, st->wav_ch,
             st->wav_ms / 60000, (st->wav_ms / 1000) % 60);
    aether_ui_label(ox + BODY_PAD + 12, cy + 36, meta, C_DIM);
    /* 进度 */
    int sy = cy + chh + 12;
    int pct = st->wav_ms > 0 ? (int)(e * 100 / st->wav_ms) : 0;
    aether_ui_bar(ox + BODY_PAD, sy, cw - 2 * BODY_PAD, 12, (double)pct, C_ACCENT);
    char tt[64];
    snprintf(tt, sizeof tt, "%02d:%02d / %02d:%02d",
             (int)(e/60000), (int)((e/1000)%60),
             st->wav_ms/60000, (st->wav_ms/1000)%60);
    aether_ui_label(ox + BODY_PAD, sy + 18, tt, C_DIM);
    /* 播放控制 */
    int by = sy + 46;
    int bw5 = (cw - 2 * BODY_PAD - 32) / 5;
    aether_ui_button(ox + BODY_PAD, by, bw5, 40, "上一首", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + (bw5+8), by, bw5, 40,
                     st->playing ? "暂停" : "播放", C_ACCENT, pack_rgb(255,255,255));
    aether_ui_button(ox + BODY_PAD + 2*(bw5+8), by, bw5, 40, "下一首", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + 3*(bw5+8), by, bw5, 40,
                     st->loop ? "循环开" : "循环关", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + 4*(bw5+8), by, bw5, 40, "重播", C_BTN, C_BTN_TX);
    /* 音量 */
    int vy = by + 48;
    aether_ui_label(ox + BODY_PAD, vy, "音量", C_DIM);
    slider(ox + BODY_PAD + 48, vy + 2, cw - 2 * BODY_PAD - 48 - 40, st->volume, 100);
    char vb[16]; snprintf(vb, sizeof vb, "%d%%", st->volume);
    aether_ui_label(ox + cw - BODY_PAD - aether_text_width_utf8(vb), vy, vb, C_DIM);
    hr_small(ox, cw, vy + 24, "波形与进度");
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_music_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w;
    if (st->media_n <= 0) {
        if (aether_ui_hit(lx, ly, cw / 2 - 100, (ch - STATUS_H) / 2 + 54, 200, 40))
            aether_desktop_open(APP_FILES);
        return;
    }
    int cy = HEAD_H + 12;
    int chh = ch - STATUS_H - (HEAD_H + 12) - 210;
    if (chh < 100) chh = 100;
    int sy = cy + chh + 12;
    if (aether_ui_hit(lx, ly, BODY_PAD, sy - 6, cw - 2 * BODY_PAD, 24)) {
        int v = slider_value(lx, BODY_PAD, cw - 2 * BODY_PAD, 100);
        st->play_offset_ms = (long)st->wav_ms * v / 100;
        if (st->playing) st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "已跳转到 %d%%", v); return;
    }
    int by = sy + 46;
    int bw5 = (cw - 2 * BODY_PAD - 32) / 5;
    long e = music_elapsed_ms(st);
    if (aether_ui_hit(lx, ly, BODY_PAD, by, bw5, 40)) {
        st->media_sel = (st->media_sel - 1 + st->media_n) % st->media_n;
        st->play_offset_ms = 0; st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "上一首：%s", base(st->media[st->media_sel])); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + (bw5+8), by, bw5, 40)) {
        if (st->playing) { st->play_offset_ms = e; st->playing = 0; }
        else { st->play_start_ms = now_ms(); st->playing = 1; }
        snprintf(st->status, sizeof st->status, st->playing ? "继续播放" : "已暂停"); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 2*(bw5+8), by, bw5, 40)) {
        st->media_sel = (st->media_sel + 1) % st->media_n;
        st->play_offset_ms = 0; st->play_start_ms = now_ms();
        snprintf(st->status, sizeof st->status, "下一首：%s", base(st->media[st->media_sel])); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 3*(bw5+8), by, bw5, 40)) {
        st->loop = !st->loop;
        snprintf(st->status, sizeof st->status, st->loop ? "循环播放已开启" : "循环播放已关闭"); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 4*(bw5+8), by, bw5, 40)) {
        st->play_offset_ms = 0; st->play_start_ms = now_ms(); st->playing = 1;
        snprintf(st->status, sizeof st->status, "已从头重播"); return;
    }
    int vy = by + 48;
    if (aether_ui_hit(lx, ly, BODY_PAD + 48, vy - 8, cw - 2 * BODY_PAD - 88, 28)) {
        st->volume = slider_value(lx, BODY_PAD + 48, cw - 2 * BODY_PAD - 88, 100);
        snprintf(st->status, sizeof st->status, "音量 %d%%", st->volume); return;
    }
}

void app_music_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (code == K_SPACE && st->media_n > 0) {
        if (st->playing) { st->play_offset_ms = music_elapsed_ms(st); st->playing = 0; }
        else { st->play_start_ms = now_ms(); st->playing = 1; }
    } else if (code == K_LEFT && st->media_n > 0) {
        st->media_sel = (st->media_sel - 1 + st->media_n) % st->media_n;
        st->play_offset_ms = 0; st->play_start_ms = now_ms();
    } else if (code == K_RIGHT && st->media_n > 0) {
        st->media_sel = (st->media_sel + 1) % st->media_n;
        st->play_offset_ms = 0; st->play_start_ms = now_ms();
    }
}

void app_music_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    char c[32] = ""; int v = 0;
    sscanf(line, "%*s %31s %d", c, &v);
    if (!strcmp(c, "pause")) { if (st->playing) { st->play_offset_ms = music_elapsed_ms(st); st->playing = 0; } }
    else if (!strcmp(c, "play")) { st->play_start_ms = now_ms(); st->playing = 1; }
    else if (!strcmp(c, "sel") && v >= 0 && v < st->media_n) {
        st->media_sel = v; st->play_offset_ms = 0; st->play_start_ms = now_ms();
    }
}

/* ==================== 5) 时钟（闹钟 / 计时器 / 秒表） ==================== */
static void alarms_load(struct app_state *st)
{
    st->alarm_n = 0;
    FILE *f = fopen(DATA_DIR "/alarms.conf", "r");
    if (!f) {
        /* 首次启动给三条真实可编辑的默认闹钟（不是假数据，是本机初始配置） */
        const char *def[3] = { "07:00|1", "08:30|0", "12:00|0" };
        f = fopen(DATA_DIR "/alarms.conf", "w");
        if (f) { for (int i = 0; i < 3; i++) fprintf(f, "%s\n", def[i]); fclose(f); }
        f = fopen(DATA_DIR "/alarms.conf", "r");
    }
    if (!f) return;
    char l[64];
    while (st->alarm_n < 8 && fgets(l, sizeof l, f)) {
        char *p = strchr(l, '|');
        if (!p) continue;
        *p = 0;
        snprintf(st->alarms[st->alarm_n], sizeof st->alarms[0], "%.15s", l);
        st->alarm_on[st->alarm_n] = atoi(p + 1) ? 1 : 0;
        st->alarm_n++;
    }
    fclose(f);
}

static void alarms_save(struct app_state *st)
{
    FILE *f = fopen(DATA_DIR "/alarms.conf", "w");
    if (!f) return;
    for (int i = 0; i < st->alarm_n; i++)
        fprintf(f, "%s|%d\n", st->alarms[i], st->alarm_on[i]);
    fclose(f);
}

void app_clock_open(struct app_state *st)
{
    alarms_load(st);
    st->clock_tab = 0; st->alarm_ring = 0;
    st->timer_set_s = 60; st->timer_left_s = 60; st->timer_run = 0;
    st->sw_run = 0; st->sw_acc_ms = 0; st->sw_lap_n = 0;
    snprintf(st->status, sizeof st->status, "系统时间");
}

static long sw_now_ms(struct app_state *st)
{
    long e = st->sw_acc_ms;
    if (st->sw_run && st->sw_start_ms) e += now_ms() - st->sw_start_ms;
    return e;
}

void app_clock_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    g_live = (st->timer_run || st->sw_run) ? 1 : 0;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    hdr(ox, oy, cw, "时钟", 0, NULL);
    /* 标签栏 */
    const char *tabs[3] = { "闹钟", "计时器", "秒表" };
    int ty = oy + HEAD_H + 8, th = 40;
    int tw = (cw - 2 * BODY_PAD) / 3;
    for (int i = 0; i < 3; i++) {
        int x = ox + BODY_PAD + i * tw;
        unsigned bg = i == st->clock_tab ? C_ACCENT : C_BTN;
        unsigned fg = i == st->clock_tab ? pack_rgb(255,255,255) : C_TEXT;
        aether_draw_round_rect(x, ty, tw - 4, th, 10, bg, 0xFF);
        aether_ui_label(x + (tw - 4 - aether_text_width_utf8(tabs[i])) / 2, ty + 12, tabs[i], fg);
    }
    int by = ty + th + 12;
    /* 当前时间（真实） */
    char big[32]; snprintf(big, sizeof big, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    aether_draw_text_scale(big, ox + cw / 2 - aether_text_width(big) * 2, by, 48, C_TEXT);
    by += 66;

    if (st->clock_tab == 0) {
        /* 闹钟到点检查（真实 localtime 对比） */
        char hm[8]; snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
        if (!st->alarm_ring) {
            for (int i = 0; i < st->alarm_n; i++)
                if (st->alarm_on[i] && !strcmp(st->alarms[i], hm) && tm.tm_sec < 3) {
                    st->alarm_ring = i + 1;
                    snprintf(st->status, sizeof st->status, "闹钟到点：%s（点击「关闭提醒」）", st->alarms[i]);
                    break;
                }
        }
        if (st->alarm_ring) {
            int ry = by;
            aether_draw_round_rect(ox + BODY_PAD, ry, cw - 2*BODY_PAD, 56, 14, C_WARN, 0xFF);
            char r[96]; snprintf(r, sizeof r, "闹钟提醒  %s", st->alarms[st->alarm_ring-1]);
            aether_ui_label(ox + BODY_PAD + 14, ry + 18, r, pack_rgb(255,255,255));
            aether_ui_button(ox + cw - BODY_PAD - 110, ry + 10, 100, 36, "关闭提醒", C_PANEL, C_TEXT);
            by += 64;
        }
        section(ox, by, "本机闹钟"); by += 24;
        for (int i = 0; i < st->alarm_n; i++) {
            int y = by + i * 52;
            if (y + 46 > oy + ch - STATUS_H - 60) break;
            aether_fill_rect(ox + BODY_PAD, y, cw - 2*BODY_PAD, 46, C_PANEL);
            aether_draw_icon(AE_ICON_BELL, ox + BODY_PAD + 8, y + 11, 24,
                             st->alarm_on[i] ? C_ACCENT : C_DIM);
            aether_ui_label(ox + BODY_PAD + 44, y + 14, st->alarms[i], C_TEXT);
            toggle(ox + cw - BODY_PAD - 62, y + 9, st->alarm_on[i]);
            aether_ui_button(ox + cw - BODY_PAD - 130, y + 9, 58, 28, "删除", C_BTN, C_BTN_TX);
        }
        int ay = by + st->alarm_n * 52 + 8;
        if (ay + 40 < oy + ch - STATUS_H) {
            aether_ui_button(ox + BODY_PAD, ay, 160, 40, "添加闹钟", C_ACCENT, pack_rgb(255,255,255));
            aether_ui_label(ox + BODY_PAD + 172, ay + 12, "键盘输入 HHMM 后回车", C_DIM);
        }
    } else if (st->clock_tab == 1) {
        if (st->timer_run) {
            long left = (st->timer_end_ms - now_ms()) / 1000;
            if (left <= 0) {
                st->timer_run = 0; st->timer_left_s = 0;
                snprintf(st->status, sizeof st->status, "计时结束");
            } else st->timer_left_s = (int)left;
        }
        char tt[32];
        snprintf(tt, sizeof tt, "%02d:%02d", st->timer_left_s / 60, st->timer_left_s % 60);
        aether_draw_text_scale(tt, ox + cw/2 - aether_text_width(tt)*3, by, 72, C_ACCENT);
        by += 92;
        section(ox, by, "设定时长（秒）"); by += 24;
        const int presets[4] = { 30, 60, 180, 300 };
        int bw4 = (cw - 2*BODY_PAD - 24) / 4;
        for (int i = 0; i < 4; i++) {
            char l[16]; snprintf(l, sizeof l, "%d秒", presets[i]);
            aether_ui_button(ox + BODY_PAD + i*(bw4+8), by, bw4, 36, l,
                             st->timer_set_s == presets[i] ? C_ACCENT : C_BTN,
                             st->timer_set_s == presets[i] ? pack_rgb(255,255,255) : C_BTN_TX);
        }
        by += 46;
        int bw3 = (cw - 2*BODY_PAD - 16) / 3;
        aether_ui_button(ox + BODY_PAD, by, bw3, 42,
                         st->timer_run ? "暂停" : "开始", C_ACCENT, pack_rgb(255,255,255));
        aether_ui_button(ox + BODY_PAD + bw3 + 8, by, bw3, 42, "重置", C_BTN, C_BTN_TX);
        aether_ui_button(ox + BODY_PAD + 2*(bw3+8), by, bw3, 42, "+30秒", C_BTN, C_BTN_TX);
    } else {
        long ms = sw_now_ms(st);
        char stt[40];
        snprintf(stt, sizeof stt, "%02d:%02d.%02d",
                 (int)(ms/60000), (int)((ms/1000)%60), (int)((ms/10)%100));
        aether_draw_text_scale(stt, ox + cw/2 - aether_text_width(stt)*3, by, 64, C_ACCENT);
        by += 86;
        int bw4 = (cw - 2*BODY_PAD - 24) / 4;
        aether_ui_button(ox + BODY_PAD, by, bw4, 42,
                         st->sw_run ? "暂停" : "开始", C_ACCENT, pack_rgb(255,255,255));
        aether_ui_button(ox + BODY_PAD + (bw4+8), by, bw4, 42, "计次", C_BTN, C_BTN_TX);
        aether_ui_button(ox + BODY_PAD + 2*(bw4+8), by, bw4, 42, "复位", C_BTN, C_BTN_TX);
        aether_ui_button(ox + BODY_PAD + 3*(bw4+8), by, bw4, 42, "全部清零", C_BTN, C_BTN_TX);
        by += 52;
        section(ox, by, "计次记录"); by += 22;
        for (int i = 0; i < st->sw_lap_n; i++) {
            char l[64]; long m = st->sw_laps[i];
            snprintf(l, sizeof l, "第 %d 次     %02d:%02d.%02d", i+1,
                     (int)(m/60000), (int)((m/1000)%60), (int)((m/10)%100));
            aether_ui_label(ox + BODY_PAD + 8, by + i*22, l, i%2 ? C_DIM : C_TEXT);
        }
        if (!st->sw_lap_n) aether_ui_label(ox + BODY_PAD + 8, by, "暂无计次，点「计次」记录", C_DIM);
    }
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_clock_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w; (void)ch;
    int ty = HEAD_H + 8, th = 40, tw = (cw - 2*BODY_PAD)/3;
    for (int i = 0; i < 3; i++) {
        if (aether_ui_hit(lx, ly, BODY_PAD + i*tw, ty, tw-4, th)) {
            st->clock_tab = i;
            snprintf(st->status, sizeof st->status, "切换到 %s",
                     i==0 ? "闹钟" : i==1 ? "计时器" : "秒表");
            return;
        }
    }
    int by = ty + th + 12 + 66;
    if (st->clock_tab == 0) {
        if (st->alarm_ring) {
            if (aether_ui_hit(lx, ly, cw - BODY_PAD - 110, by + 10, 100, 36)) {
                st->alarm_ring = 0;
                snprintf(st->status, sizeof st->status, "提醒已关闭");
                return;
            }
            by += 64;
        }
        by += 24;
        for (int i = 0; i < st->alarm_n; i++) {
            int y = by + i*52;
            if (aether_ui_hit(lx, ly, cw - BODY_PAD - 62, y + 9, 52, 28)) {
                st->alarm_on[i] = !st->alarm_on[i]; alarms_save(st);
                snprintf(st->status, sizeof st->status, "%s 已%s", st->alarms[i],
                         st->alarm_on[i] ? "开启" : "关闭"); return;
            }
            if (aether_ui_hit(lx, ly, cw - BODY_PAD - 130, y + 9, 58, 28)) {
                for (int j = i; j < st->alarm_n - 1; j++) {
                    snprintf(st->alarms[j], sizeof st->alarms[0], "%s", st->alarms[j+1]);
                    st->alarm_on[j] = st->alarm_on[j+1];
                }
                st->alarm_n--; alarms_save(st);
                snprintf(st->status, sizeof st->status, "已删除闹钟"); return;
            }
        }
        int ay = by + st->alarm_n*52 + 8;
        if (aether_ui_hit(lx, ly, BODY_PAD, ay, 160, 40)) {
            st->numlen = 0; st->numbuf[0] = 0;
            snprintf(st->status, sizeof st->status, "输入 HHMM 四位数字后回车（如 0730）");
        }
    } else if (st->clock_tab == 1) {
        by += 92 + 24;
        const int presets[4] = { 30, 60, 180, 300 };
        int bw4 = (cw - 2*BODY_PAD - 24)/4;
        for (int i = 0; i < 4; i++) {
            if (aether_ui_hit(lx, ly, BODY_PAD + i*(bw4+8), by, bw4, 36)) {
                st->timer_set_s = presets[i]; st->timer_left_s = presets[i];
                st->timer_run = 0;
                snprintf(st->status, sizeof st->status, "设定 %d 秒", presets[i]); return;
            }
        }
        by += 46;
        int bw3 = (cw - 2*BODY_PAD - 16)/3;
        if (aether_ui_hit(lx, ly, BODY_PAD, by, bw3, 42)) {
            if (st->timer_run) {
                st->timer_left_s = (int)((st->timer_end_ms - now_ms())/1000);
                st->timer_run = 0;
                snprintf(st->status, sizeof st->status, "计时已暂停");
            } else {
                if (st->timer_left_s <= 0) st->timer_left_s = st->timer_set_s;
                st->timer_end_ms = now_ms() + (long)st->timer_left_s * 1000L;
                st->timer_run = 1;
                snprintf(st->status, sizeof st->status, "计时开始");
            }
            return;
        }
        if (aether_ui_hit(lx, ly, BODY_PAD + bw3 + 8, by, bw3, 42)) {
            st->timer_run = 0; st->timer_left_s = st->timer_set_s;
            snprintf(st->status, sizeof st->status, "已重置"); return;
        }
        if (aether_ui_hit(lx, ly, BODY_PAD + 2*(bw3+8), by, bw3, 42)) {
            st->timer_set_s += 30; st->timer_left_s += 30;
            if (st->timer_run) st->timer_end_ms += 30000L;
            snprintf(st->status, sizeof st->status, "增加 30 秒，共 %d 秒", st->timer_set_s); return;
        }
    } else {
        by += 86;
        int bw4 = (cw - 2*BODY_PAD - 24)/4;
        if (aether_ui_hit(lx, ly, BODY_PAD, by, bw4, 42)) {
            if (st->sw_run) { st->sw_acc_ms = sw_now_ms(st); st->sw_run = 0; }
            else { st->sw_start_ms = now_ms(); st->sw_run = 1; }
            snprintf(st->status, sizeof st->status, st->sw_run ? "秒表开始" : "秒表暂停"); return;
        }
        if (aether_ui_hit(lx, ly, BODY_PAD + (bw4+8), by, bw4, 42)) {
            if (st->sw_lap_n < 8) st->sw_laps[st->sw_lap_n++] = sw_now_ms(st);
            snprintf(st->status, sizeof st->status, "已记录第 %d 次", st->sw_lap_n); return;
        }
        if (aether_ui_hit(lx, ly, BODY_PAD + 2*(bw4+8), by, bw4, 42)) {
            st->sw_run = 0; st->sw_acc_ms = 0; st->sw_lap_n = 0;
            snprintf(st->status, sizeof st->status, "秒表已复位"); return;
        }
        if (aether_ui_hit(lx, ly, BODY_PAD + 3*(bw4+8), by, bw4, 42)) {
            st->sw_lap_n = 0;
            snprintf(st->status, sizeof st->status, "计次记录已清零"); return;
        }
    }
}

void app_clock_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (st->clock_tab == 0) {
        if (code == K_BACKSPACE) { if (st->numlen > 0) st->numbuf[--st->numlen] = 0; return; }
        if (code == K_ENTER) {
            if (st->numlen == 4 && st->alarm_n < 8) {
                char a[8];
                snprintf(a, sizeof a, "%.2s:%.2s", st->numbuf, st->numbuf + 2);
                int hh = (st->numbuf[0]-'0')*10 + (st->numbuf[1]-'0');
                int mm = (st->numbuf[2]-'0')*10 + (st->numbuf[3]-'0');
                if (hh < 24 && mm < 60) {
                    snprintf(st->alarms[st->alarm_n], sizeof st->alarms[0], "%s", a);
                    st->alarm_on[st->alarm_n] = 1; st->alarm_n++;
                    alarms_save(st);
                    snprintf(st->status, sizeof st->status, "已添加闹钟 %s", a);
                } else snprintf(st->status, sizeof st->status, "时间无效，请输入 0000-2359");
            } else snprintf(st->status, sizeof st->status, "请输入四位数字 HHMM");
            st->numlen = 0; st->numbuf[0] = 0;
            return;
        }
        char c = aether_key_to_char(code, 0);
        if (c >= '0' && c <= '9' && st->numlen < 5) {
            st->numbuf[st->numlen++] = c; st->numbuf[st->numlen] = 0;
            snprintf(st->status, sizeof st->status, "输入中：%s", st->numbuf);
        }
        return;
    }
    if (st->clock_tab == 2 && code == K_SPACE) {
        if (st->sw_run) { st->sw_acc_ms = sw_now_ms(st); st->sw_run = 0; }
        else { st->sw_start_ms = now_ms(); st->sw_run = 1; }
    }
}

void app_clock_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    char c[32] = ""; int v = 0;
    sscanf(line, "%*s %31s %d", c, &v);
    if (!strcmp(c, "tab") && v >= 0 && v <= 2) st->clock_tab = v;
    else if (!strcmp(c, "timer") && v > 0) {
        st->clock_tab = 1; st->timer_set_s = v; st->timer_left_s = v;
        st->timer_end_ms = now_ms() + (long)v * 1000L; st->timer_run = 1;
    } else if (!strcmp(c, "sw")) {
        st->clock_tab = 2;
        if (st->sw_run) { st->sw_acc_ms = sw_now_ms(st); st->sw_run = 0; }
        else { st->sw_start_ms = now_ms(); st->sw_run = 1; }
    }
}

/* ==================== 6) 日历 ==================== */
static int month_days(int y, int m)
{
    struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = m; t.tm_mday = 0;
    mktime(&t);
    return t.tm_mday;
}
static int first_wday(int y, int m)
{
    struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = 1;
    mktime(&t);
    return t.tm_wday;
}
static void events_load(struct app_state *st)
{
    st->event_n = 0;
    FILE *f = fopen(DATA_DIR "/events.conf", "r");
    if (!f) return;
    char l[160];
    char key[16];
    snprintf(key, sizeof key, "%04d-%02d-%02d", st->cal_y, st->cal_m, st->cal_sel);
    while (fgets(l, sizeof l, f)) {
        size_t L = strlen(l); while (L && (l[L-1]=='\n' || l[L-1]=='\r')) l[--L] = 0;
        if (!strncmp(l, key, 10) && l[10] == '|' && st->event_n < 16)
            snprintf(st->events[st->event_n++], sizeof st->events[0], "%.95s", l + 11);
    }
    fclose(f);
}
static void events_save_add(struct app_state *st, const char *text)
{
    FILE *f = fopen(DATA_DIR "/events.conf", "a");
    if (!f) { snprintf(st->status, sizeof st->status, "保存失败：无法写入 %s", DATA_DIR); return; }
    fprintf(f, "%04d-%02d-%02d|%s\n", st->cal_y, st->cal_m, st->cal_sel, text);
    fclose(f);
}

void app_calendar_open(struct app_state *st)
{
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    st->cal_y = tm.tm_year + 1900; st->cal_m = tm.tm_mon + 1; st->cal_sel = tm.tm_mday;
    events_load(st);
    snprintf(st->status, sizeof st->status, "日程保存到本机");
}

void app_calendar_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    hdr(ox, oy, cw, "日历", 0, "回到今天");
    char ym[32];
    snprintf(ym, sizeof ym, "%d 年 %d 月", st->cal_y, st->cal_m);
    int ty = oy + HEAD_H + 6;
    aether_ui_label(ox + BODY_PAD, ty, ym, C_TEXT);
    aether_ui_button(ox + BODY_PAD, ty + 26, 56, 32, "上月", C_BTN, C_BTN_TX);
    aether_ui_button(ox + BODY_PAD + 64, ty + 26, 56, 32, "下月", C_BTN, C_BTN_TX);
    const char *wd[7] = { "日","一","二","三","四","五","六" };
    int gy = ty + 68;
    int cw7 = (cw - 2*BODY_PAD) / 7;
    for (int i = 0; i < 7; i++) {
        char s[16]; snprintf(s, sizeof s, "%s", wd[i]);
        aether_ui_label(ox + BODY_PAD + i*cw7 + cw7/2 - aether_text_width_utf8(s)/2, gy, s, C_DIM);
    }
    gy += 24;
    int fd = first_wday(st->cal_y, st->cal_m);
    int md = month_days(st->cal_y, st->cal_m);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    int is_this_month = (tm.tm_year+1900 == st->cal_y && tm.tm_mon+1 == st->cal_m);
    int rows = (fd + md + 6) / 7;
    int cellh = 40;
    for (int i = 0; i < rows * 7; i++) {
        int day = i - fd + 1;
        int r = i / 7, c = i % 7;
        int x = ox + BODY_PAD + c*cw7, y = gy + r*cellh;
        if (day < 1 || day > md) continue;
        int today = is_this_month && day == tm.tm_mday;
        int sel = day == st->cal_sel;
        if (today || sel) {
            unsigned bg = today ? C_ACCENT : C_BTN;
            aether_draw_round_rect(x + cw7/2 - 17, y, 34, 32, 16, bg, 0xFF);
        }
        char d[12]; snprintf(d, sizeof d, "%d", day);
        unsigned fg = (today || sel) ? pack_rgb(255,255,255) : C_TEXT;
        aether_ui_label(x + cw7/2 - aether_text_width(d)/2, y + 8, d, fg);
    }
    int ey = gy + rows*cellh + 10;
    if (ey > oy + ch - STATUS_H - 150) ey = oy + ch - STATUS_H - 150;
    char eh[64];
    snprintf(eh, sizeof eh, "%d月%d日 日程（%d 项）", st->cal_m, st->cal_sel, st->event_n);
    aether_ui_label(ox + BODY_PAD, ey, eh, C_TEXT);
    ey += 24;
    if (st->event_n == 0) {
        aether_ui_label(ox + BODY_PAD + 8, ey, "暂无日程 · 在下方输入后回车添加", C_DIM);
    } else {
        for (int i = 0; i < st->event_n; i++) {
            if (ey + i*22 > oy + ch - STATUS_H - 60) break;
            aether_draw_icon(AE_ICON_CALENDAR, ox + BODY_PAD + 4, ey + i*22 + 2, 16, C_ACCENT);
            char b[120]; aether_ui_fit(b, sizeof b, st->events[i], cw - 2*BODY_PAD - 90);
            aether_ui_label(ox + BODY_PAD + 26, ey + i*22 + 2, b, C_TEXT);
            aether_ui_button(ox + cw - BODY_PAD - 52, ey + i*22 - 2, 48, 20, "删除", C_BTN, C_BTN_TX);
        }
    }
    int iy = oy + ch - STATUS_H - 40;
    aether_draw_round_rect(ox + BODY_PAD, iy, cw - 2*BODY_PAD - 84, 32, 8, C_BG, 0xFF);
    aether_draw_border(ox + BODY_PAD, iy, cw - 2*BODY_PAD - 84, 32, C_BORDER, 1);
    if (st->ev_len) aether_ui_label(ox + BODY_PAD + 6, iy + 8, st->ev_input, C_TEXT);
    else aether_ui_label(ox + BODY_PAD + 6, iy + 8, "输入日程内容", C_DIM);
    aether_ui_button(ox + cw - BODY_PAD - 76, iy, 76, 32, "添加", C_ACCENT, pack_rgb(255,255,255));
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_calendar_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w;
    int ty = HEAD_H + 6;
    if (aether_ui_hit(lx, ly, BODY_PAD, ty + 26, 56, 32)) {
        st->cal_m--; if (st->cal_m < 1) { st->cal_m = 12; st->cal_y--; }
        st->cal_sel = 1; events_load(st);
        snprintf(st->status, sizeof st->status, "上一月：%d年%d月", st->cal_y, st->cal_m); return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD + 64, ty + 26, 56, 32)) {
        st->cal_m++; if (st->cal_m > 12) { st->cal_m = 1; st->cal_y++; }
        st->cal_sel = 1; events_load(st);
        snprintf(st->status, sizeof st->status, "下一月：%d年%d月", st->cal_y, st->cal_m); return;
    }
    if (ly < HEAD_H && lx > cw - 140) {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        st->cal_y = tm.tm_year+1900; st->cal_m = tm.tm_mon+1; st->cal_sel = tm.tm_mday;
        events_load(st);
        snprintf(st->status, sizeof st->status, "已回到今天"); return;
    }
    int gy = ty + 68 + 24;
    int fd = first_wday(st->cal_y, st->cal_m);
    int md = month_days(st->cal_y, st->cal_m);
    int rows = (fd + md + 6) / 7;
    int cw7 = (cw - 2*BODY_PAD)/7, cellh = 40;
    if (ly >= gy && ly < gy + rows*cellh) {
        int r = (ly - gy)/cellh, c = (lx - BODY_PAD)/cw7;
        if (c >= 0 && c < 7) {
            int day = r*7 + c - fd + 1;
            if (day >= 1 && day <= md) {
                st->cal_sel = day; events_load(st);
                snprintf(st->status, sizeof st->status, "已选择 %d月%d日", st->cal_m, day);
                return;
            }
        }
    }
    int ey = gy + rows*cellh + 10;
    if (ey > ch - STATUS_H - 150) ey = ch - STATUS_H - 150;
    ey += 24;
    for (int i = 0; i < st->event_n; i++) {
        if (aether_ui_hit(lx, ly, cw - BODY_PAD - 52, ey + i*22 - 2, 48, 20)) {
            /* 真实删除：重写 events.conf，剔除该行 */
            char key[16];
            snprintf(key, sizeof key, "%04d-%02d-%02d", st->cal_y, st->cal_m, st->cal_sel);
            FILE *f = fopen(DATA_DIR "/events.conf", "r");
            FILE *o = fopen(DATA_DIR "/events.tmp", "w");
            if (f && o) {
                char l[160]; int idx = 0;
                while (fgets(l, sizeof l, f)) {
                    int match = (!strncmp(l, key, 10) && l[10] == '|');
                    if (match && idx++ == i) continue;
                    fputs(l, o);
                }
                fclose(f); fclose(o);
                rename(DATA_DIR "/events.tmp", DATA_DIR "/events.conf");
            } else { if (f) fclose(f); if (o) fclose(o); }
            events_load(st);
            snprintf(st->status, sizeof st->status, "已删除该日程"); return;
        }
    }
    int iy = ch - STATUS_H - 40;
    if (aether_ui_hit(lx, ly, cw - BODY_PAD - 76, iy, 76, 32)) {
        if (st->ev_len) {
            events_save_add(st, st->ev_input);
            events_load(st);
            st->ev_len = 0; st->ev_input[0] = 0;
            snprintf(st->status, sizeof st->status, "日程已保存到本机");
        } else snprintf(st->status, sizeof st->status, "请先用键盘输入日程内容");
        return;
    }
    if (aether_ui_hit(lx, ly, BODY_PAD, iy, cw - 2*BODY_PAD - 84, 32)) {
        snprintf(st->status, sizeof st->status, "键盘输入日程内容后点「添加」");
    }
}

void app_calendar_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (code == K_ENTER) {
        if (st->ev_len) {
            events_save_add(st, st->ev_input); events_load(st);
            snprintf(st->status, sizeof st->status, "日程已保存到本机");
            st->ev_len = 0; st->ev_input[0] = 0;
        }
        return;
    }
    if (code == K_BACKSPACE) {
        if (st->ev_len > 0) st->ev_input[--st->ev_len] = 0;
        return;
    }
    if (code == K_LEFT)  { st->cal_m--; if (st->cal_m < 1) { st->cal_m=12; st->cal_y--; } st->cal_sel=1; events_load(st); return; }
    if (code == K_RIGHT) { st->cal_m++; if (st->cal_m >12) { st->cal_m=1;  st->cal_y++; } st->cal_sel=1; events_load(st); return; }
    char c = aether_key_to_char(code, 0);
    if (c && st->ev_len < (int)sizeof st->ev_input - 2) {
        st->ev_input[st->ev_len++] = c;
        st->ev_input[st->ev_len] = 0;
    }
}

void app_calendar_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    int a = 0, b = 0, c = 0;
    int n = sscanf(line, "%*s %d %d %d", &a, &b, &c);
    if (n >= 2 && a > 1900 && b >= 1 && b <= 12) { st->cal_y = a; st->cal_m = b; }
    if (n >= 3 && c >= 1 && c <= 31) st->cal_sel = c;
    events_load(st);
}
