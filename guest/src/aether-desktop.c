/*
 * aether-desktop.c - AetherOS 桌面外壳
 *
 * 窗口管理（z-order / 焦点 / 拖拽 / 最小化 / 最大化 / 关闭）、
 * 任务栏、开始菜单、桌面图标、主题、通知、开机启动画面、输入分发。
 * 真机输入与 IPC 自动化注入共用同一套 handler。
 */
#include "aether-desktop.h"
#include "aether-apps.h"
#include "aether-render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/reboot.h>

#define TOPBAR   32
#define TASKBAR  48
#define TITLEBAR 30
#define MENU_W   300
#define ICON_W   92
#define ICON_H   88

static struct aether_win g_wins[MAX_WINDOWS];
static int g_focus = 0;
static int g_menu = 0;
static int g_theme = 0;
static int g_w, g_h;
static int g_btn_down = 0;
static int g_drag_id = 0, g_drag_dx = 0, g_drag_dy = 0;
static int g_next_id = 1;
static int g_topz = 1;
static int g_invalidate = 1;
static time_t g_boot_t0 = 0;
static int g_splash = 1;
static int g_shutdown_armed = 0;
static char g_notify[128];
static time_t g_notify_t = 0;
static int g_mx = 0, g_my = 0;

/* ---- 键位映射（Linux KEY_* -> ASCII） ---- */
static char km[128][2];
static int km_ready = 0;

static void km_init(void)
{
    if (km_ready) return;
    km_ready = 1;
    const char *a = "1234567890-=",  *A = "!@#$%^&*()_+";
    for (int i = 0; i < 12; i++) { km[2 + i][0] = a[i]; km[2 + i][1] = A[i]; }
    a = "qwertyuiop[]"; A = "QWERTYUIOP{}";
    for (int i = 0; i < 12; i++) { km[16 + i][0] = a[i]; km[16 + i][1] = A[i]; }
    a = "asdfghjkl;'"; A = "ASDFGHJKL:\"";
    for (int i = 0; i < 11; i++) { km[30 + i][0] = a[i]; km[30 + i][1] = A[i]; }
    km[41][0] = '`'; km[41][1] = '~';
    km[43][0] = '\\'; km[43][1] = '|';
    a = "zxcvbnm,./"; A = "ZXCVBNM<>?";
    for (int i = 0; i < 10; i++) { km[44 + i][0] = a[i]; km[44 + i][1] = A[i]; }
    km[57][0] = ' '; km[57][1] = ' ';
    /* 数字小键盘 */
    for (int i = 0; i < 10; i++) { km[79 + i][0] = '0' + i; km[79 + i][1] = '0' + i; }
    km[71][0] = '7'; km[72][0] = '8'; km[73][0] = '9';
    km[75][0] = '4'; km[76][0] = '5'; km[77][0] = '6';
    km[79][0] = '1'; km[80][0] = '2'; km[81][0] = '3';
    km[82][0] = '0'; km[83][0] = '.'; km[96][0] = '\n';
    km[55][0] = '*'; km[74][0] = '-'; km[78][0] = '+'; km[98][0] = '/';
}

char aether_key_to_char(unsigned code, int shift)
{
    km_init();
    if (code >= 128) return 0;
    return shift ? km[code][1] : km[code][0];
}

unsigned aether_char_to_key(char c, int *shift_out)
{
    km_init();
    if (shift_out) *shift_out = 0;
    for (unsigned i = 0; i < 128; i++) {
        if (km[i][0] && km[i][0] == c) return i;
    }
    for (unsigned i = 0; i < 128; i++) {
        if (km[i][1] && km[i][1] == c) { if (shift_out) *shift_out = 1; return i; }
    }
    return 0;
}

/* ---- 主题色 ---- */
static unsigned D_BG1, D_BG2, D_BAR, D_PANEL, D_TEXT, D_DIM, D_ACCENT, D_BORDER, D_BTN;

static void theme_palette(void)
{
    if (g_theme == 1) {
        D_BG1 = pack_rgb(206, 222, 244); D_BG2 = pack_rgb(120, 158, 206);
        D_BAR = pack_rgb(246, 248, 252); D_PANEL = pack_rgb(255, 255, 255);
        D_TEXT = pack_rgb(26, 32, 44); D_DIM = pack_rgb(98, 110, 130);
        D_ACCENT = pack_rgb(0, 100, 200); D_BORDER = pack_rgb(198, 208, 222);
        D_BTN = pack_rgb(226, 234, 246);
    } else {
        D_BG1 = pack_rgb(14, 22, 42); D_BG2 = pack_rgb(9, 48, 68);
        D_BAR = pack_rgb(20, 26, 38); D_PANEL = pack_rgb(33, 41, 58);
        D_TEXT = pack_rgb(234, 240, 250); D_DIM = pack_rgb(142, 158, 182);
        D_ACCENT = pack_rgb(64, 156, 255); D_BORDER = pack_rgb(62, 76, 100);
        D_BTN = pack_rgb(48, 60, 84);
    }
}

/* ---- 窗口几何 ---- */
static void default_geometry(enum app_kind k, int *w, int *h)
{
    static const int W[APP_COUNT] = { 660, 720, 660, 340, 660, 580, 600, 820 };
    static const int H[APP_COUNT] = { 460, 440, 460, 420, 460, 420, 420, 560 };
    *w = W[(int)k]; *h = H[(int)k];
}

static int max_content_top(void) { return TOPBAR + 8; }
static int max_content_bottom(void) { return g_h - TASKBAR - 8; }

int aether_desktop_open(enum app_kind kind)
{
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) {
        /* 全满：关掉最底层一个 */
        int lowest = 0, lz = 1 << 30;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g_wins[i].used && g_wins[i].z < lz) { lz = g_wins[i].z; lowest = i; }
        slot = lowest;
    }
    struct aether_win *w = &g_wins[slot];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->id = g_next_id++;
    if (g_next_id > 100000) g_next_id = 1;
    w->kind = kind;
    snprintf(w->title, sizeof w->title, "%s", aether_app_name(kind));
    default_geometry(kind, &w->w, &w->h);
    if (w->w > g_w - 24) w->w = g_w - 24;
    if (w->h > max_content_bottom() - max_content_top() - 4)
        w->h = max_content_bottom() - max_content_top() - 4;
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used) n++;
    w->x = 90 + (n % 6) * 30;
    w->y = TOPBAR + 16 + (n % 5) * 26;
    if (w->x + w->w > g_w - 8) w->x = g_w - 8 - w->w;
    if (w->y + w->h > max_content_bottom()) w->y = max_content_bottom() - w->h;
    if (w->x < 8) w->x = 8;
    if (w->y < TOPBAR + 4) w->y = TOPBAR + 4;
    w->z = ++g_topz;
    w->minimized = 0; w->maximized = 0;
    g_focus = w->id;
    aether_app_open(w);
    g_invalidate = 1;
    return w->id;
}

void aether_desktop_close(int id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].id == id) {
            aether_app_close(&g_wins[i]);
            memset(&g_wins[i], 0, sizeof g_wins[i]);
        }
    }
    /* 焦点给剩下的最高层 */
    int best = 0, bz = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_wins[i].used && !g_wins[i].minimized && g_wins[i].z > bz) { bz = g_wins[i].z; best = g_wins[i].id; }
    g_focus = best;
    g_invalidate = 1;
}

struct aether_win *aether_desktop_window(int id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id == id) return &g_wins[i];
    return NULL;
}

struct aether_win *aether_desktop_focus(void) { return aether_desktop_window(g_focus); }

void aether_desktop_invalidate(void) { g_invalidate = 1; }
int  aether_desktop_theme(void) { return g_theme; }
void aether_desktop_set_theme(int t) { g_theme = t ? 1 : 0; g_invalidate = 1; }
int  aether_desktop_width(void) { return g_w; }
int  aether_desktop_height(void) { return g_h; }

void aether_desktop_notify(const char *text)
{
    snprintf(g_notify, sizeof g_notify, "%s", text);
    g_notify_t = time(NULL);
    g_invalidate = 1;
}

void aether_desktop_init(int w, int h)
{
    g_w = w; g_h = h;
    memset(g_wins, 0, sizeof g_wins);
    g_focus = 0; g_menu = 0; g_theme = 0; g_next_id = 1; g_topz = 1;
    g_boot_t0 = time(NULL);
    g_splash = 1;
    g_notify[0] = 0;
    g_mx = w / 2; g_my = h / 2;
    km_init();
    theme_palette();
    g_invalidate = 1;
}

/* ==================== 渲染 ==================== */
static void draw_icon_tile(int x, int y, int idx)
{
    unsigned col = aether_app_color((enum app_kind)idx);
    aether_draw_round_rect(x + 14, y, 64, 60, 12, col, 0xFF);
    aether_draw_round_rect(x + 17, y + 3, 58, 54, 10, pack_rgb(255, 255, 255), 40);
    const char *s = aether_app_short((enum app_kind)idx);
    int tw = aether_text_width_utf8(s);
    aether_draw_text_utf8(x + 14 + (64 - tw) / 2, y + 22, s, pack_rgb(255, 255, 255));
    const char *n = aether_app_name((enum app_kind)idx);
    char t[64];
    int avail = ICON_W - 4;
    /* 简单按像素截断名称 */
    int wsum = 0, cut = 0;
    for (int k = 0; n[k] && cut < 60; k++) {
        int len = (unsigned char)n[k] < 0x80 ? 1 : 3;
        char tmp[5]; memcpy(tmp, n + k, (size_t)len); tmp[len] = 0;
        int cw = aether_text_width_utf8(tmp);
        if (wsum + cw > avail) break;
        memcpy(t + cut, n + k, (size_t)len); cut += len; wsum += cw; k += len - 1;
    }
    t[cut] = 0;
    int nw = aether_text_width_utf8(t);
    aether_draw_text_utf8(x + (ICON_W - nw) / 2, y + 66, t, D_TEXT);
}

static void draw_window(struct aether_win *w)
{
    aether_fill_rect_alpha(w->x + 5, w->y + 5, w->w, w->h, 0x000000, 60);
    aether_draw_round_rect(w->x, w->y, w->w, w->h, 8, D_BORDER, 0xFF);
    aether_draw_round_rect(w->x + 1, w->y + 1, w->w - 2, w->h - 2, 7, D_PANEL, 0xFF);
    unsigned tb = (w->id == g_focus) ? D_ACCENT : D_BTN;
    aether_fill_rect(w->x + 1, w->y + 1, w->w - 2, TITLEBAR, tb);
    aether_draw_text_utf8(w->x + 12, w->y + 7, w->title,
                          (w->id == g_focus) ? pack_rgb(255, 255, 255) : D_TEXT);
    const char *btns[3] = { "-", "+", "X" };
    for (int i = 0; i < 3; i++) {
        int bx = w->x + w->w - 88 + i * 28;
        unsigned bg = (i == 2) ? pack_rgb(200, 60, 60) : pack_rgb(255, 255, 255);
        aether_draw_round_rect(bx, w->y + 4, 24, 22, 4, bg, i == 2 ? 0xFF : 60);
        const char *t = btns[i];
        int tw = aether_text_width_utf8(t);
        aether_draw_text_utf8(bx + (24 - tw) / 2, w->y + 7, t,
                              i == 2 ? pack_rgb(255, 255, 255) : D_TEXT);
    }
    aether_app_render(w, w->x + 1, w->y + TITLEBAR + 1, w->w - 2, w->h - TITLEBAR - 2);
}

static void draw_start_menu(void)
{
    int rows = 8, row_h = 36;
    int mh = rows * row_h + 8 + 2 * 32 + 16;
    int mx = 8, my = g_h - TASKBAR - mh - 8;
    if (my < TOPBAR + 4) my = TOPBAR + 4;
    aether_fill_rect_alpha(mx + 5, my + 5, MENU_W, mh, 0x000000, 60);
    aether_draw_round_rect(mx, my, MENU_W, mh, 10, D_BORDER, 0xFF);
    aether_draw_round_rect(mx + 1, my + 1, MENU_W - 2, mh - 2, 9, D_PANEL, 0xFF);
    aether_draw_text_utf8(mx + 14, my + 8, "AetherOS 应用", D_DIM);
    int y = my + 30;
    for (int i = 0; i < APP_COUNT; i++) {
        int yy = y + i * row_h;
        aether_draw_round_rect(mx + 10, yy + 2, 30, 28, 6, aether_app_color((enum app_kind)i), 0xFF);
        const char *s = aether_app_short((enum app_kind)i);
        int tw = aether_text_width_utf8(s);
        aether_draw_text_utf8(mx + 10 + (30 - tw) / 2, yy + 8, s, pack_rgb(255, 255, 255));
        aether_draw_text_utf8(mx + 50, yy + 8, aether_app_name((enum app_kind)i), D_TEXT);
    }
    y += APP_COUNT * row_h + 6;
    aether_fill_rect(mx + 10, y, MENU_W - 20, 1, D_BORDER);
    y += 6;
    aether_draw_text_utf8(mx + 20, y + 6, "重启桌面", D_TEXT);
    aether_draw_text_utf8(mx + 160, y + 6, "关机", D_TEXT);
}

static int menu_rect(int *x, int *y, int *w, int *h)
{
    int mh = 8 * 36 + 8 + 2 * 32 + 16;
    int my = g_h - TASKBAR - mh - 8;
    if (my < TOPBAR + 4) my = TOPBAR + 4;
    *x = 8; *y = my; *w = MENU_W; *h = mh;
    return mh;
}

static void draw_splash(void)
{
    aether_draw_vgradient(0, 0, g_w, g_h, pack_rgb(8, 14, 28), pack_rgb(6, 36, 54));
    aether_draw_logo(g_w / 2, g_h / 2 - 60, 150);
    const char *n = "AetherOS";
    int nw = aether_text_width(n);
    aether_draw_text(g_w / 2 - nw / 2, g_h / 2 + 30, n, pack_rgb(240, 246, 255));
    const char *s = "以太操作系统  ·  x86_64 桌面版";
    int sw = aether_text_width_utf8(s);
    aether_draw_text_utf8(g_w / 2 - sw / 2, g_h / 2 + 54, s, pack_rgb(150, 190, 220));
    int elapsed = (int)(time(NULL) - g_boot_t0);
    int pct = elapsed * 100 / 3;
    if (pct > 100) pct = 100;
    int bw = 360, bx = g_w / 2 - bw / 2, by = g_h / 2 + 92;
    aether_draw_round_rect(bx, by, bw, 12, 6, D_BORDER, 0xFF);
    if (pct > 2) aether_draw_round_rect(bx + 2, by + 2, (bw - 4) * pct / 100, 8, 4, D_ACCENT, 0xFF);
    const char *t = "正在启动系统服务…  点击任意位置进入桌面";
    int tw2 = aether_text_width_utf8(t);
    aether_draw_text_utf8(g_w / 2 - tw2 / 2, by + 24, t, pack_rgb(140, 170, 200));
}

static int window_at(int x, int y)
{
    int best = 0, bz = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct aether_win *w = &g_wins[i];
        if (!w->used || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (w->z > bz) { bz = w->z; best = w->id; }
        }
    }
    return best;
}

void aether_desktop_render(const char *clock, double cpu, double mem)
{
    theme_palette();
    aether_render_clear_clip();
    aether_draw_vgradient(0, 0, g_w, g_h, D_BG1, D_BG2);
    /* 桌面品牌水印 */
    aether_draw_text_utf8(g_w - 260, g_h - TASKBAR - 66, "AetherOS 以太操作系统", D_DIM);
    aether_draw_text(g_w - 260, g_h - TASKBAR - 44, "x86_64 Desktop Distribution", D_DIM);

    /* 桌面图标：2 列 x 4 行 */
    for (int i = 0; i < APP_COUNT; i++) {
        int col = i / 4, row = i % 4;
        draw_icon_tile(18 + col * (ICON_W + 8), TOPBAR + 18 + row * (ICON_H + 8), i);
    }

    /* 窗口按 z 升序绘制（简单插入排序，保证焦点窗口在最上层） */
    int order[MAX_WINDOWS], n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_wins[i].used && !g_wins[i].minimized) order[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && g_wins[order[b - 1]].z > g_wins[order[b]].z; b--) {
            int t = order[b - 1]; order[b - 1] = order[b]; order[b] = t;
        }
    for (int i = 0; i < n; i++) draw_window(&g_wins[order[i]]);

    /* 顶栏 */
    aether_fill_rect_alpha(0, 0, g_w, TOPBAR, D_BAR, 235);
    aether_fill_rect(0, TOPBAR - 1, g_w, 1, D_BORDER);
    aether_draw_text(10, 8, "AetherOS", D_TEXT);
    struct aether_win *f = aether_desktop_focus();
    if (f) {
        char t[96];
        snprintf(t, sizeof t, "  |  %s", f->title);
        aether_draw_text_utf8(90, 8, t, D_DIM);
    }
    char right[96];
    snprintf(right, sizeof right, "CPU %4.1f%%   MEM %4.1f%%   %s", cpu, mem, clock ? clock : "");
    int rw = aether_text_width(right);
    aether_draw_text(g_w - rw - 12, 8, right, D_DIM);

    /* 任务栏 */
    int y0 = g_h - TASKBAR;
    aether_fill_rect(0, y0, g_w, TASKBAR, D_BAR);
    aether_fill_rect(0, y0, g_w, 1, D_BORDER);
    aether_draw_round_rect(8, y0 + 6, 110, 36, 7, g_menu ? D_ACCENT : D_BTN, 0xFF);
    aether_draw_text_utf8(34, y0 + 16, "开始",
                          g_menu ? pack_rgb(255, 255, 255) : D_TEXT);
    int tx = 128;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].used) continue;
        int bw = 150;
        if (tx + bw > g_w - 200) break;
        unsigned bg = (g_wins[i].id == g_focus && !g_wins[i].minimized) ? D_ACCENT : D_BTN;
        aether_draw_round_rect(tx, y0 + 7, bw, 34, 6, bg, 0xFF);
        char t[80];
        snprintf(t, sizeof t, "%s%s", g_wins[i].minimized ? "-" : "", g_wins[i].title);
        char tt[80];
        /* 名称截断 */
        int wsum = 0, cut = 0;
        for (int k = 0; t[k] && cut < 70; k++) {
            int len = (unsigned char)t[k] < 0x80 ? 1 : 3;
            char tmp[5]; memcpy(tmp, t + k, (size_t)len); tmp[len] = 0;
            int cw = aether_text_width_utf8(tmp);
            if (wsum + cw > bw - 14) break;
            memcpy(tt + cut, t + k, (size_t)len); cut += len; wsum += cw; k += len - 1;
        }
        tt[cut] = 0;
        aether_draw_text_utf8(tx + 8, y0 + 16, tt,
                              (g_wins[i].id == g_focus && !g_wins[i].minimized)
                              ? pack_rgb(255, 255, 255) : D_TEXT);
        tx += bw + 6;
    }
    char tray[64];
    snprintf(tray, sizeof tray, "%s", clock ? clock : "");
    int trw = aether_text_width(tray);
    aether_draw_text(g_w - trw - 12, y0 + 16, tray, D_TEXT);

    if (g_menu) draw_start_menu();

    /* 通知气泡 */
    if (g_notify[0] && time(NULL) - g_notify_t < 5) {
        int nw = 300, nx = g_w - nw - 12, ny = y0 - 54;
        aether_draw_round_rect(nx, ny, nw, 42, 8, D_BORDER, 0xFF);
        aether_draw_round_rect(nx + 1, ny + 1, nw - 2, 40, 7, D_PANEL, 0xFF);
        char t[128];
        int wsum = 0, cut = 0;
        for (int k = 0; g_notify[k] && cut < 120; k++) {
            int len = (unsigned char)g_notify[k] < 0x80 ? 1 : 3;
            char tmp[5]; memcpy(tmp, g_notify + k, (size_t)len); tmp[len] = 0;
            int cw = aether_text_width_utf8(tmp);
            if (wsum + cw > nw - 24) break;
            memcpy(t + cut, g_notify + k, (size_t)len); cut += len; wsum += cw; k += len - 1;
        }
        t[cut] = 0;
        aether_draw_text_utf8(nx + 12, ny + 13, t, D_TEXT);
    } else if (g_notify[0]) g_notify[0] = 0;

    if (g_splash) {
        if (time(NULL) - g_boot_t0 >= 3) g_splash = 0;
        else draw_splash();
    }

    aether_draw_cursor(aether_render_cursor_x(), aether_render_cursor_y());
    g_invalidate = 0;
}

/* ==================== 输入分发 ==================== */
static void raise_window(int id)
{
    struct aether_win *w = aether_desktop_window(id);
    if (!w) return;
    w->z = ++g_topz;
    g_focus = id;
    g_invalidate = 1;
}

static void handle_menu_click(int x, int y)
{
    int mx, my, mw, mh;
    menu_rect(&mx, &my, &mw, &mh);
    if (x < mx || x >= mx + mw || y < my || y >= my + mh) { g_menu = 0; g_invalidate = 1; return; }
    int row_h = 36, y0 = my + 30;
    if (y >= y0 && y < y0 + APP_COUNT * row_h) {
        int i = (y - y0) / row_h;
        g_menu = 0;
        aether_desktop_open((enum app_kind)i);
        return;
    }
    int tail = y0 + APP_COUNT * row_h + 12;
    if (y >= tail && y < tail + 32) {
        if (x >= mx + 150) {
            if (!g_shutdown_armed) {
                g_shutdown_armed = 1;
                g_menu = 0;
                aether_desktop_notify("再点一次「关机」确认关闭系统");
            } else {
                sync();
                reboot(RB_POWER_OFF);
            }
        } else {
            for (int i = 0; i < MAX_WINDOWS; i++) g_wins[i].used = 0;
            g_focus = 0; g_menu = 0; g_shutdown_armed = 0;
            aether_desktop_notify("桌面已重置，所有窗口已关闭");
        }
    }
}

static void handle_taskbar_click(int x, int y)
{
    (void)y;
    if (x >= 8 && x < 118) { g_menu = !g_menu; g_invalidate = 1; return; }
    int tx = 128;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].used) continue;
        int bw = 150;
        if (tx + bw > g_w - 200) break;
        if (x >= tx && x < tx + bw) {
            if (g_wins[i].minimized) { g_wins[i].minimized = 0; raise_window(g_wins[i].id); }
            else if (g_focus == g_wins[i].id) g_wins[i].minimized = 1;
            else raise_window(g_wins[i].id);
            g_invalidate = 1;
            return;
        }
        tx += bw + 6;
    }
}

void aether_desktop_button(int x, int y, int down)
{
    g_mx = x; g_my = y;
    if (g_splash) {
        if (!down) { g_splash = 0; g_invalidate = 1; }
        return;
    }
    if (!down) { g_btn_down = 0; g_drag_id = 0; return; }
    g_btn_down = 1;
    g_invalidate = 1;

    if (g_menu) { handle_menu_click(x, y); return; }
    if (y >= g_h - TASKBAR) { handle_taskbar_click(x, y); return; }
    if (y < TOPBAR) return;

    int id = window_at(x, y);
    if (id) {
        struct aether_win *w = aether_desktop_window(id);
        raise_window(id);
        if (y < w->y + TITLEBAR) {
            int bx0 = w->x + w->w - 88;
            if (x >= bx0 && x < bx0 + 28) { w->minimized = 1; return; }
            if (x >= bx0 + 28 && x < bx0 + 56) {
                if (!w->maximized) {
                    w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
                    w->x = 8; w->y = TOPBAR + 4;
                    w->w = g_w - 16; w->h = g_h - TASKBAR - TOPBAR - 12;
                    w->maximized = 1;
                } else {
                    w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
                    w->maximized = 0;
                }
                return;
            }
            if (x >= bx0 + 56 && x < bx0 + 84) { aether_desktop_close(id); return; }
            g_drag_id = id;
            g_drag_dx = x - w->x;
            g_drag_dy = y - w->y;
            return;
        }
        aether_app_click(w, x - w->x - 1, y - w->y - TITLEBAR - 1,
                         w->w - 2, w->h - TITLEBAR - 2);
        return;
    }
    /* 桌面图标：点击打开 */
    for (int i = 0; i < APP_COUNT; i++) {
        int col = i / 4, row = i % 4;
        int ix = 18 + col * (ICON_W + 8), iy = TOPBAR + 18 + row * (ICON_H + 8);
        if (x >= ix && x < ix + ICON_W && y >= iy && y < iy + ICON_H) {
            aether_desktop_open((enum app_kind)i);
            return;
        }
    }
}

void aether_desktop_motion(int x, int y)
{
    g_mx = x; g_my = y;
    if (!g_btn_down || !g_drag_id) return;
    struct aether_win *w = aether_desktop_window(g_drag_id);
    if (!w || w->maximized) return;
    int nx = x - g_drag_dx, ny = y - g_drag_dy;
    if (nx < 0) nx = 0;
    if (nx + w->w > g_w) nx = g_w - w->w;
    if (ny < TOPBAR) ny = TOPBAR;
    if (ny + TITLEBAR > g_h - TASKBAR) ny = g_h - TASKBAR - TITLEBAR;
    w->x = nx; w->y = ny;
    g_invalidate = 1;
}

void aether_desktop_key(unsigned code, int shift)
{
    if (g_splash) { g_splash = 0; g_invalidate = 1; return; }
    if (code == 1 /* KEY_ESC */) { g_menu = 0; g_invalidate = 1; return; }
    struct aether_win *f = aether_desktop_focus();
    if (f) { aether_app_key(f, code, shift); g_invalidate = 1; }
}

/* ==================== 状态与 IPC 自动化 ==================== */
void aether_desktop_state(char *buf, size_t sz)
{
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used) n++;
    size_t o = 0;
    o += (size_t)snprintf(buf + o, sz - o,
        "{\"menu\":%d,\"focus\":%d,\"windows\":%d,\"theme\":%d,\"splash\":%d,\"wins\":[",
        g_menu, g_focus, n, g_theme, g_splash);
    int first = 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct aether_win *w = &g_wins[i];
        if (!w->used) continue;
        o += (size_t)snprintf(buf + o, sz - o,
            "%s{\"id\":%d,\"kind\":%d,\"min\":%d,\"max\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
            first ? "" : ",", w->id, (int)w->kind, w->minimized, w->maximized,
            w->x, w->y, w->w, w->h);
        first = 0;
    }
    snprintf(buf + o, sz - o, "]}");
}

void aether_desktop_ipc(const char *line, char *reply, size_t sz)
{
    char cmd[32];
    if (sscanf(line, "%31s", cmd) != 1) { snprintf(reply, sz, "{\"ok\":false}\n"); return; }

    if (!strcmp(cmd, "state")) {
        char js[2048];
        aether_desktop_state(js, sizeof js);
        snprintf(reply, sz, "%s\n", js);
        return;
    }
    if (!strcmp(cmd, "skipboot")) { g_splash = 0; g_invalidate = 1; snprintf(reply, sz, "{\"ok\":true}\n"); return; }
    if (!strcmp(cmd, "move") || !strcmp(cmd, "click") || !strcmp(cmd, "down") || !strcmp(cmd, "up")) {
        int x = 0, y = 0;
        sscanf(line, "%*s %d %d", &x, &y);
        if (!strcmp(cmd, "move")) { aether_render_set_cursor(x, y); aether_desktop_motion(x, y); }
        else if (!strcmp(cmd, "click")) {
            aether_render_set_cursor(x, y);
            aether_desktop_button(x, y, 1);
            aether_desktop_button(x, y, 0);
        } else if (!strcmp(cmd, "down")) { aether_render_set_cursor(x, y); aether_desktop_button(x, y, 1); }
        else { aether_render_set_cursor(x, y); aether_desktop_button(x, y, 0); }
        snprintf(reply, sz, "{\"ok\":true}\n");
        return;
    }
    if (!strcmp(cmd, "key")) {
        unsigned code = 0; int shift = 0;
        sscanf(line, "%*s %u %d", &code, &shift);
        aether_desktop_key(code, shift);
        snprintf(reply, sz, "{\"ok\":true}\n");
        return;
    }
    if (!strcmp(cmd, "text")) {
        const char *s = line + 4;
        while (*s == ' ') s++;
        for (int i = 0; s[i] && s[i] != '\r' && s[i] != '\n'; i++) {
            int shift = 0;
            unsigned code = aether_char_to_key(s[i], &shift);
            if (code) aether_desktop_key(code, shift);
        }
        snprintf(reply, sz, "{\"ok\":true}\n");
        return;
    }
    if (!strcmp(cmd, "open")) {
        int k = 0;
        sscanf(line, "%*s %d", &k);
        if (k >= 0 && k < APP_COUNT) {
            int id = aether_desktop_open((enum app_kind)k);
            snprintf(reply, sz, "{\"ok\":true,\"id\":%d}\n", id);
        } else snprintf(reply, sz, "{\"ok\":false}\n");
        return;
    }
    if (!strcmp(cmd, "closeall")) {
        for (int i = 0; i < MAX_WINDOWS; i++) g_wins[i].used = 0;
        g_focus = 0; g_menu = 0; g_invalidate = 1;
        snprintf(reply, sz, "{\"ok\":true}\n");
        return;
    }
    snprintf(reply, sz, "unknown\n");
}
