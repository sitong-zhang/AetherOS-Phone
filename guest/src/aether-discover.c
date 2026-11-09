/*
 * aether-discover.c - 全局搜索 / 负一屏（阶段 4）
 *
 * 所有结果来自真实数据源：本机应用表、真实目录扫描、设置一级结构、
 * 本地日程/联系人/书签文件、/proc/uptime。不使用演示假数据。
 * 负一屏加深模糊背景只算一次后缓存复用，避免软渲染环境逐帧重算
 * 可分离高斯 IIR 阻塞串行 IPC（与 aether-phone.c 的玻璃缓存同理）。
 */
#include "aether-discover.h"
#include "aether-render.h"
#include "aether-icons.h"
#include "aether-desktop.h"
#include "aether-apps.h"
#include "aether-appdata.h"
#include "aether-glass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* ==================== 全局搜索 ==================== */
enum res_group { G_APP = 0, G_FILE, G_SETTING, G_CONTACT, G_WEB, G_COUNT };

static const char *GROUP_NAME[G_COUNT] = {
    "应用", "文件", "设置项", "联系人", "网页建议"
};
static const int GROUP_ICON[G_COUNT] = {
    AE_ICON_LAYOUT_GRID, AE_ICON_FOLDER, AE_ICON_SETTINGS,
    AE_ICON_USERS, AE_ICON_GLOBE
};

struct res {
    char text[96];
    char sub[96];
    int  app_kind;      /* G_APP 有效 */
    int  set_page;      /* G_SETTING 有效 */
};

static struct res g_res[G_COUNT][8];
static int g_res_n[G_COUNT];
static int g_show_all[G_COUNT];
static char g_query[128];
static int g_search_open = 0;
static int g_total = 0;

/* 可见行布局：render 与 hit 共用，避免两处几何不一致 */
struct row {
    int type;           /* 1 搜索框 2 组标题 3 结果 4 查看全部 5 空状态 */
    int group, idx;
    int x, y, w, h;
};
static struct row g_rows[96];
static int g_row_n = 0;

static void push_res(int g, const char *text, const char *sub,
                     int app_kind, int set_page)
{
    if (g < 0 || g >= G_COUNT || g_res_n[g] >= 8) { return; }
    struct res *r = &g_res[g][g_res_n[g]++];
    snprintf(r->text, sizeof r->text, "%s", text);
    snprintf(r->sub, sizeof r->sub, "%s", sub ? sub : "");
    r->app_kind = app_kind;
    r->set_page = set_page;
    g_total++;
}

static int match(const char *s, const char *q)
{
    if (!q || !q[0]) { return 1; }
    return strstr(s, q) != NULL;
}

static int read_lines(const char *path, int group, const char *label)
{
    FILE *f = fopen(path, "r");
    if (!f) { return 0; }
    char line[160];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < 8) {
        size_t L = strlen(line);
        while (L && line[L - 1] < 32) { line[--L] = 0; }
        if (!line[0] || !match(line, g_query)) { continue; }
        push_res(group, line, label, -1, -1);
        n++;
    }
    fclose(f);
    return n;
}

static int scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { return 0; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 8) {
        if (e->d_name[0] == '.') { continue; }
        if (!match(e->d_name, g_query)) { continue; }
        push_res(G_FILE, e->d_name, dir, -1, -1);
        n++;
    }
    closedir(d);
    return n;
}

static void rebuild(void)
{
    for (int g = 0; g < G_COUNT; g++) { g_res_n[g] = 0; }
    g_total = 0;

    /* 1) 应用：本机真实应用表 */
    for (int k = 0; k < APP_COUNT; k++) {
        const char *nm = aether_app_name((enum app_kind)k);
        if (match(nm, g_query)) {
            push_res(G_APP, nm, "本机应用 · 点击打开", k, -1);
        }
    }
    /* 2) 文件：真实目录扫描 */
    scan_dir("/usr/share/aether/wallpapers");
    scan_dir("/usr/share/aether/audio");
    scan_dir("/data/aether");
    /* 3) 设置项：一级结构同一份定义，不重复 */
    int tn = aether_settings_top_count();
    for (int i = 0; i < tn; i++) {
        const char *nm = aether_settings_top_name(i);
        if (match(nm, g_query)) {
            push_res(G_SETTING, nm, "系统设置 · 点击直达", -1, i);
        }
    }
    /* 4) 联系人：本地文件（本机无云同步，无文件即空组） */
    read_lines("/data/aether/contacts.conf", G_CONTACT, "本机联系人");
    /* 5) 网页建议：本地书签/历史（无网络时不臆造结果） */
    read_lines("/data/aether/bookmarks.conf", G_WEB, "本机书签");
}

void aether_search_init(void)
{
    memset(g_query, 0, sizeof g_query);
    memset(g_show_all, 0, sizeof g_show_all);
    g_search_open = 0;
    rebuild();
}

void aether_search_open(int on)
{
    g_search_open = on ? 1 : 0;
    if (g_search_open) { rebuild(); }
}

int aether_search_is_open(void) { return g_search_open; }

void aether_search_input(const char *text)
{
    snprintf(g_query, sizeof g_query, "%s", text ? text : "");
    g_search_open = 1;
    for (int g = 0; g < G_COUNT; g++) { g_show_all[g] = 0; }
    rebuild();
}

void aether_search_key(unsigned code, int shift)
{
    if (code == 14 /* BACKSPACE */) {
        size_t L = strlen(g_query);
        if (L) { g_query[L - 1] = 0; }
    } else {
        char c = aether_key_to_char(code, shift);
        if (c >= 32) {
            size_t L = strlen(g_query);
            if (L + 1 < sizeof g_query) {
                g_query[L] = c; g_query[L + 1] = 0;
            }
        }
    }
    for (int g = 0; g < G_COUNT; g++) { g_show_all[g] = 0; }
    rebuild();
}

static void layout(int cw, int ch, int status_h)
{
    g_row_n = 0;
    int y = status_h + 12;
    int x = 16, w = cw - 32;
    /* 搜索框 */
    g_rows[g_row_n].type = 1; g_rows[g_row_n].group = -1; g_rows[g_row_n].idx = -1;
    g_rows[g_row_n].x = x; g_rows[g_row_n].y = y; g_rows[g_row_n].w = w; g_rows[g_row_n].h = 46;
    g_row_n++;
    y += 54;
    int bottom = ch - 8;

    if (g_total == 0) {
        g_rows[g_row_n].type = 5; g_rows[g_row_n].group = -1; g_rows[g_row_n].idx = -1;
        g_rows[g_row_n].x = x; g_rows[g_row_n].y = y; g_rows[g_row_n].w = w; g_rows[g_row_n].h = 90;
        g_row_n++;
        return;
    }

    for (int g = 0; g < G_COUNT; g++) {
        if (g_res_n[g] == 0 || y + 26 > bottom) { continue; }
        g_rows[g_row_n].type = 2; g_rows[g_row_n].group = g; g_rows[g_row_n].idx = -1;
        g_rows[g_row_n].x = x; g_rows[g_row_n].y = y; g_rows[g_row_n].w = w; g_rows[g_row_n].h = 26;
        g_row_n++; y += 28;
        int lim = g_show_all[g] ? g_res_n[g] : (g_res_n[g] > 3 ? 3 : g_res_n[g]);
        for (int i = 0; i < lim && y + 42 <= bottom; i++) {
            g_rows[g_row_n].type = 3; g_rows[g_row_n].group = g; g_rows[g_row_n].idx = i;
            g_rows[g_row_n].x = x; g_rows[g_row_n].y = y;
            g_rows[g_row_n].w = w; g_rows[g_row_n].h = 42;
            g_row_n++; y += 44;
        }
        if (g_res_n[g] > 3 && y + 28 <= bottom) {
            g_rows[g_row_n].type = 4; g_rows[g_row_n].group = g; g_rows[g_row_n].idx = -1;
            g_rows[g_row_n].x = x; g_rows[g_row_n].y = y;
            g_rows[g_row_n].w = w; g_rows[g_row_n].h = 28;
            g_row_n++; y += 30;
        }
        y += 8;
    }
}

void aether_search_render(int ox, int oy, int cw, int ch, int theme, int status_h)
{
    /* 深 / 浅两套完整色板（总纲 6.4、禁止清单「深色模式只做一半、应用内不跟随」）。
     * 搜索是全屏浮层，必须用不透明底，否则下层桌面文字会透出；两套都保持不透明，
     * 只是底色、卡片、文字、描边、按钮按主题取不同值。 */
    int light = theme == 1;
    layout(cw, ch, status_h);
    unsigned bg     = light ? pack_rgb(238, 242, 249) : pack_rgb(12, 16, 28);
    unsigned panel  = light ? pack_rgb(255, 255, 255) : pack_rgb(24, 30, 46);
    unsigned tx     = light ? pack_rgb(24, 32, 46)   : pack_rgb(236, 242, 252);
    unsigned dim    = light ? pack_rgb(96, 108, 128)  : pack_rgb(146, 156, 176);
    unsigned accent = light ? pack_rgb(0, 102, 204)   : pack_rgb(96, 165, 250);
    unsigned border = light ? pack_rgb(204, 214, 228) : pack_rgb(58, 68, 88);
    unsigned btn    = light ? pack_rgb(226, 234, 246) : pack_rgb(40, 48, 66);
    unsigned grad_t = light ? pack_rgb(246, 249, 253) : pack_rgb(18, 26, 44);
    unsigned grad_b = light ? pack_rgb(220, 230, 243) : pack_rgb(9, 12, 22);

    /* 全屏搜索层：不透明底 + 顶部输入框，避免下层桌面文字透出 */
    aether_fill_rect(ox, oy, cw, ch, bg);
    aether_draw_vgradient(ox, oy, cw, ch, grad_t, grad_b);

    for (int i = 0; i < g_row_n; i++) {
        struct row *r = &g_rows[i];
        int X = ox + r->x, Y = oy + r->y;
        if (r->type == 1) {
            aether_glass_drop_shadow(X, Y, r->w, r->h, 12);
            aether_draw_round_rect(X, Y, r->w, r->h, 12, panel, 0xFF);
            aether_draw_border(X, Y, r->w, r->h, border, 1);
            aether_glass_specular_edge(X, Y, r->w, r->h, 12, theme);
            aether_draw_icon(AE_ICON_SEARCH, X + 12, Y + 11, 24, dim);
            char q[160];
            snprintf(q, sizeof q, "%s", g_query[0] ? g_query : "搜索应用、文件、设置与内容");
            unsigned qc = g_query[0] ? tx : dim;
            aether_draw_text_utf8(X + 46, Y + 14, q, qc);
            aether_ui_button(X + r->w - 64, Y + 8, 56, 30, "清除",
                             btn, tx);
        } else if (r->type == 2) {
            aether_draw_icon(GROUP_ICON[r->group], X, Y + 3, 20, accent);
            char t[64];
            snprintf(t, sizeof t, "%s · %d 项", GROUP_NAME[r->group], g_res_n[r->group]);
            aether_draw_text_utf8(X + 28, Y + 4, t, dim);
        } else if (r->type == 3) {
            struct res *s = &g_res[r->group][r->idx];
            aether_draw_round_rect(X, Y, r->w, r->h, 10, panel, 0xE8);
            aether_draw_icon(GROUP_ICON[r->group], X + 12, Y + 9, 24, accent);
            aether_draw_text_utf8(X + 46, Y + 5, s->text, tx);
            aether_draw_text_utf8(X + 46, Y + 23, s->sub, dim);
            aether_draw_icon(AE_ICON_CHEVRON_RIGHT, X + r->w - 32, Y + 9, 24, dim);
        } else if (r->type == 4) {
            char t[64];
            snprintf(t, sizeof t, "查看全部 %s（%d 项）",
                     GROUP_NAME[r->group], g_res_n[r->group]);
            aether_draw_text_utf8(X + 12, Y + 6, t, accent);
        } else if (r->type == 5) {
            aether_draw_round_rect(X, Y, r->w, r->h, 14, panel, 0xE0);
            aether_draw_icon(AE_ICON_SEARCH, X + 20, Y + 20, 32, dim);
            aether_draw_text_utf8(X + 66, Y + 18, "没有找到相关内容", tx);
            aether_draw_text_utf8(X + 66, Y + 42,
                "试试更短的关键词，或搜索应用、文件名、设置项名称", dim);
        }
    }
}

int aether_search_hit(int lx, int ly, int cw, int ch, int status_h,
                      int *app_kind, int *set_page)
{
    if (app_kind) { *app_kind = -1; }
    if (set_page) { *set_page = -1; }
    layout(cw, ch, status_h);
    for (int i = 0; i < g_row_n; i++) {
        struct row *r = &g_rows[i];
        if (lx < r->x || lx > r->x + r->w || ly < r->y || ly > r->y + r->h) {
            continue;
        }
        if (r->type == 1 && lx > r->x + r->w - 70) {
            aether_search_input("");
            return 0;
        }
        if (r->type == 4) {
            g_show_all[r->group] = !g_show_all[r->group];
            return 3;
        }
        if (r->type == 3) {
            struct res *s = &g_res[r->group][r->idx];
            if (r->group == G_APP && app_kind) { *app_kind = s->app_kind; return 1; }
            if (r->group == G_SETTING && set_page) { *set_page = s->set_page; return 2; }
            return 0;
        }
    }
    return 0;
}

void aether_search_state(char *buf, size_t sz)
{
    snprintf(buf, sz,
        "{\"open\":%d,\"query\":\"%s\",\"total\":%d,"
        "\"apps\":%d,\"files\":%d,\"settings\":%d,\"contacts\":%d,\"web\":%d}",
        g_search_open, g_query, g_total,
        g_res_n[G_APP], g_res_n[G_FILE], g_res_n[G_SETTING],
        g_res_n[G_CONTACT], g_res_n[G_WEB]);
}

/* ==================== 负一屏 ==================== */
static int g_neg1_open = 0;
static int g_neg1_enabled = 1;
static unsigned char *g_bg_cache = NULL;
static int g_bg_pitch = 0, g_bg_rows = 0, g_bg_valid = 0, g_bg_theme = -1;

/* 负一屏可点击区域 */
struct n1hit { int kind; int val; int x, y, w, h; };  /* kind 1=sys 2=app */
static struct n1hit g_n1hits[16];
static int g_n1hit_n = 0;

void aether_neg1_open(int on)
{
    if (!g_neg1_enabled && on) { return; }
    g_neg1_open = on ? 1 : 0;
}

int aether_neg1_is_open(void) { return g_neg1_open; }
void aether_neg1_toggle_enabled(int on) { g_neg1_enabled = on ? 1 : 0; }
int aether_neg1_enabled(void) { return g_neg1_enabled; }
void aether_neg1_invalidate(void) { g_bg_valid = 0; }

static void n1_add(int kind, int val, int x, int y, int w, int h)
{
    if (g_n1hit_n >= 16) { return; }
    struct n1hit *p = &g_n1hits[g_n1hit_n++];
    p->kind = kind; p->val = val;
    p->x = x; p->y = y; p->w = w; p->h = h;
}

static void n1_card(int x, int y, int w, int h, const char *title,
                    int icon, int theme)
{
    unsigned panel = theme == 1 ? pack_rgb(238, 242, 250) : pack_rgb(26, 33, 50);
    unsigned tx = theme == 1 ? pack_rgb(24, 32, 48) : pack_rgb(236, 242, 252);
    aether_draw_round_rect(x, y, w, h, 20, panel, 0xEC);
    aether_draw_border(x, y, w, h, pack_rgb(90, 102, 128), 1);
    aether_draw_icon(icon, x + 16, y + 12, 22, tx);
    aether_draw_text_utf8(x + 46, y + 14, title, tx);
}

void aether_neg1_render(int ox, int oy, int cw, int ch, int theme, int status_h,
                        double cpu, double mem, int battery)
{
    /* 背景：壁纸的加深模糊版。只算一次后缓存，避免逐帧 O(N) 高斯 IIR。 */
    unsigned char *bits = NULL;
    int fbw = 0, fbh = 0, pitch = 0;
    aether_render_framebuffer(&bits, &fbw, &fbh, &pitch);
    int rowbytes = cw * 4;
    if (g_bg_valid && g_bg_cache && g_bg_theme == theme &&
        g_bg_rows == ch && g_bg_pitch == rowbytes && bits && pitch >= rowbytes) {
        for (int r = 0; r < ch; r++) {
            memcpy(bits + (size_t)(oy + r) * pitch + (size_t)ox * 4,
                   g_bg_cache + (size_t)r * g_bg_pitch, (size_t)rowbytes);
        }
    } else {
        aether_glass_set_quality(GLASS_STANDARD);
        aether_glass_blur_tint(ox, oy, cw, ch, 12,
            theme == 1 ? pack_rgb(210, 220, 238) : pack_rgb(6, 10, 20), 168);
        if (bits && cw > 0 && ch > 0 && pitch >= rowbytes) {
            if (!g_bg_cache || g_bg_rows != ch || g_bg_pitch != rowbytes) {
                free(g_bg_cache);
                g_bg_pitch = rowbytes; g_bg_rows = ch;
                g_bg_cache = (unsigned char *)malloc((size_t)ch * rowbytes);
            }
            if (g_bg_cache) {
                for (int r = 0; r < ch; r++) {
                    memcpy(g_bg_cache + (size_t)r * rowbytes,
                           bits + (size_t)(oy + r) * pitch + (size_t)ox * 4,
                           (size_t)rowbytes);
                }
                g_bg_theme = theme; g_bg_valid = 1;
            }
        }
    }

    unsigned tx = theme == 1 ? pack_rgb(24, 32, 48) : pack_rgb(238, 244, 252);
    unsigned dim = theme == 1 ? pack_rgb(92, 104, 126) : pack_rgb(154, 166, 188);
    unsigned accent = theme == 1 ? pack_rgb(52, 104, 200) : pack_rgb(110, 178, 255);

    g_n1hit_n = 0;
    int x = ox + 18, w = cw - 36;
    int y = oy + status_h + 20;

    aether_draw_text_utf8(x, y, "负一屏", tx);
    aether_draw_text_utf8(x + 90, y + 2, "本机实时信息", dim);
    y += 34;

    /* 1) 天气 → 本机环境（无网络天气源时如实标注，数据来自 proc/sys） */
    n1_card(x, y, w, 92, "天气 · 本机环境", AE_ICON_CLOUD, theme);
    char l1[96];
    snprintf(l1, sizeof l1, "处理器 %.0f%%   内存 %.0f%%   电量 %d%%",
             cpu, mem, battery);
    aether_draw_text_utf8(x + 18, y + 44, l1, tx);
    aether_draw_text_utf8(x + 18, y + 66, "无网络天气源 · 以上为 /proc 与电池真实读数", dim);
    y += 100;

    /* 2) 日程待办：读日历应用写入的真实日程文件 */
    n1_card(x, y, w, 92, "日程待办", AE_ICON_CALENDAR, theme);
    FILE *f = fopen("/data/aether/events.conf", "r");
    int ev = 0;
    char line[160];
    if (f) {
        while (fgets(line, sizeof line, f) && ev < 2) {
            size_t L = strlen(line);
            while (L && line[L - 1] < 32) { line[--L] = 0; }
            if (!line[0]) { continue; }
            aether_draw_text_utf8(x + 18, y + 44 + ev * 22, line, tx);
            ev++;
        }
        fclose(f);
    }
    if (!ev) { aether_draw_text_utf8(x + 18, y + 46, "今天暂无日程 · 可在日历中添加", dim); }
    y += 100;

    /* 3) 快捷功能：4 个磁贴，真实写入统一系统设置总线 */
    n1_card(x, y, w, 108, "快捷功能", AE_ICON_SLIDERS, theme);
    const int keys[4] = { SYS_WIFI, SYS_BT, SYS_TORCH, SYS_DND };
    const char *kn[4] = { "无线网络", "蓝牙", "手电筒", "勿扰" };
    const int ki[4] = { AE_ICON_WIFI, AE_ICON_BLUETOOTH, AE_ICON_FLASHLIGHT, AE_ICON_MOON };
    int bw = (w - 36 - 3 * 8) / 4;
    /* 关闭态磁贴：浅色用低亮玻璃灰，深色用深玻璃灰，两套都完整（总纲 6.4） */
    unsigned off = theme == 1 ? pack_rgb(214, 223, 236) : pack_rgb(48, 56, 74);
    unsigned off_tx = theme == 1 ? pack_rgb(96, 108, 128) : dim;
    for (int i = 0; i < 4; i++) {
        int bx = x + 18 + i * (bw + 8), by = y + 44;
        int v = aether_sys_get(keys[i]);
        aether_glass_drop_shadow(bx, by, bw, 52, 12);
        aether_draw_round_rect(bx, by, bw, 52, 12,
            v ? accent : off, v ? 0xF0 : 0xD0);
        aether_glass_specular_edge(bx, by, bw, 52, 12, theme);
        aether_draw_icon(ki[i], bx + bw / 2 - 12, by + 6, 24,
                         v ? pack_rgb(255, 255, 255) : off_tx);
        aether_draw_text_utf8(bx + 6, by + 32, kn[i],
                              v ? pack_rgb(255, 255, 255) : off_tx);
        n1_add(1, keys[i], bx - ox, by - oy, bw, 52);
    }
    y += 116;

    /* 4) 使用统计：真实 uptime 与已打开应用数 */
    n1_card(x, y, w, 92, "使用统计", AE_ICON_ACTIVITY, theme);
    double up = 0.0;
    FILE *uf = fopen("/proc/uptime", "r");
    if (uf) {
        if (fscanf(uf, "%lf", &up) != 1) { up = 0.0; }
        fclose(uf);
    }
    int wins = 0, recent[8];
    for (int i = 1; i <= MAX_WINDOWS; i++) {
        struct aether_win *wv = aether_desktop_window(i);
        if (wv && wv->used) { recent[wins++] = (int)wv->kind; }
    }
    char l4[128];
    snprintf(l4, sizeof l4, "已运行 %.0f 分钟   打开应用 %d 个", up / 60.0, wins);
    aether_draw_text_utf8(x + 18, y + 44, l4, tx);
    aether_draw_text_utf8(x + 18, y + 66, "数据来自 /proc/uptime 与本机窗口表", dim);
    y += 100;

    /* 5) 推荐：最近打开的真实应用，点击直达 */
    n1_card(x, y, w, 92, "推荐 · 最近使用", AE_ICON_LAYOUT_GRID, theme);
    if (wins > 0) {
        for (int i = 0; i < wins && i < 3; i++) {
            int k = recent[wins - 1 - i];
            int bx = x + 18 + i * ((w - 36) / 3);
            aether_draw_icon(AE_ICON_APP_WINDOW, bx, y + 44, 22, accent);
            char nm[64];
            snprintf(nm, sizeof nm, "%s", aether_app_name((enum app_kind)k));
            aether_draw_text_utf8(bx + 28, y + 46, nm, tx);
            n1_add(2, k, bx - ox, y + 40 - oy, (w - 36) / 3 - 6, 40);
        }
    } else {
        aether_draw_text_utf8(x + 18, y + 46, "还没有打开过应用 · 去桌面点一个", dim);
    }
}

int aether_neg1_hit(int lx, int ly, int cw, int ch, int status_h,
                    int *sys_key, int *app_kind)
{
    (void)cw; (void)ch; (void)status_h;
    if (sys_key) { *sys_key = -1; }
    if (app_kind) { *app_kind = -1; }
    for (int i = 0; i < g_n1hit_n; i++) {
        struct n1hit *p = &g_n1hits[i];
        if (lx < p->x || lx > p->x + p->w || ly < p->y || ly > p->y + p->h) {
            continue;
        }
        if (p->kind == 1 && sys_key) { *sys_key = p->val; return 1; }
        if (p->kind == 2 && app_kind) { *app_kind = p->val; return 2; }
    }
    return 0;
}

void aether_neg1_state(char *buf, size_t sz)
{
    snprintf(buf, sz, "{\"open\":%d,\"enabled\":%d}", g_neg1_open, g_neg1_enabled);
}
