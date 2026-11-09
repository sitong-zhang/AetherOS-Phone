/*
 * aether-island.c - 灵动岛与实时活动 / 隐私指示（阶段 4）
 *
 * 形态规则、场景分层、主副位优先级与连续形变参数均照搬开源实现，
 * 详见 aether-island.h 与 OPENSOURCE-ATTRIBUTION.md。本文件只做纯 C
 * 软件光栅化等价实现，不含任何上游源码拷贝。
 */
#include "aether-island.h"
#include "aether-render.h"
#include "aether-glass.h"       /* 灵动岛浮起感：双层阴影 + 镜面高光（上游规范） */
#include "aether-glass-spec.h"
#include "aether-icons.h"
#include "aether-appdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

struct island_act {
    int  used;
    char title[64];
    char sub[96];
    int  progress;       /* 0..100，进度类场景 */
    long start_ms;       /* 起始单调时间：计时器/录音/通话时长 */
    long auto_hide_ms;   /* >0：到点自动收起（充电） */
    int  playing;        /* 音乐/计时器是否在跑 */
};

static struct island_act g_act[IS_COUNT];
static int g_primary = IS_NONE, g_secondary = IS_NONE;
static int g_popup = IS_NONE;             /* 短暂事件占主位（SmartIsland popsUp） */
static int g_form = ISLAND_COLLAPSED, g_prev_form = ISLAND_COLLAPSED;
static double g_morph = 1.0;             /* 形变进度 0..1 */
static double g_side_x = 0.0;            /* 副圆当前 x（连续位移，不瞬移） */
static int g_side_from_center = 0;
static int g_side_snap = 0;              /* 取证结算：副圆直接吸附到终态位置 */
static long g_last_tick = 0;
static int g_priv_on[4];                 /* 按 privacy_kind 索引 */
static char g_priv_app[4][48];
static int g_priv_panel = 0;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static double clamp01(double v)
{
    if (v < 0.0) { return 0.0; }
    if (v > 1.0) { return 1.0; }
    return v;
}

static double ease_out(double t)
{
    t = clamp01(t);
    return 1.0 - (1.0 - t) * (1.0 - t);
}

/* 《总纲》5.7 优先级：通话 > 录音 > 导航 > 计时器 > 音乐 > 下载 > 投屏 > 蓝牙 */
static int prio(int s)
{
    switch (s) {
    case IS_CALL:     return 100;
    case IS_RECORD:   return 90;
    case IS_NAV:      return 80;
    case IS_TIMER:    return 70;
    case IS_MUSIC:    return 60;
    case IS_DOWNLOAD: return 50;
    case IS_CAST:     return 40;
    case IS_BT:       return 30;
    case IS_CHARGE:   return 20;
    case IS_UPDATE:   return 10;
    default:          return 0;
    }
}

static const char *SCENE_NAME[IS_COUNT] = {
    "无", "通话", "录音", "导航", "计时器", "音乐",
    "下载", "投屏", "蓝牙", "充电", "系统更新"
};

const char *aether_island_scene_name(int scene)
{
    if (scene < 0 || scene >= IS_COUNT) { return "无"; }
    return SCENE_NAME[scene];
}

static int scene_icon(int s)
{
    switch (s) {
    case IS_CALL:     return AE_ICON_PHONE;
    case IS_RECORD:   return AE_ICON_MIC;
    case IS_NAV:      return AE_ICON_NAVIGATION;
    case IS_TIMER:    return AE_ICON_TIMER;
    case IS_MUSIC:    return AE_ICON_MUSIC;
    case IS_DOWNLOAD: return AE_ICON_DOWNLOAD;
    case IS_CAST:     return AE_ICON_MONITOR;
    case IS_BT:       return AE_ICON_BLUETOOTH;
    case IS_CHARGE:   return AE_ICON_BATTERY_CHARGING;
    case IS_UPDATE:   return AE_ICON_REFRESH_CW;
    default:          return AE_ICON_INFO;
    }
}

static int is_popup_scene(int s)
{
    return s == IS_CHARGE || s == IS_UPDATE || s == IS_BT || s == IS_DOWNLOAD;
}

static void recompute(void)
{
    int best = IS_NONE, second = IS_NONE;
    for (int s = 1; s < IS_COUNT; s++) {
        if (!g_act[s].used || is_popup_scene(s)) { continue; }
        if (best == IS_NONE || prio(s) > prio(best)) {
            second = best; best = s;
        } else if (second == IS_NONE || prio(s) > prio(second)) {
            second = s;
        }
    }
    if (second != IS_NONE && g_secondary == IS_NONE) {
        g_side_from_center = 1;      /* 副位从岛中心连续滑出，不许瞬移 */
    }
    if (best == IS_NONE && g_popup == IS_NONE && g_form != ISLAND_COLLAPSED) {
        g_prev_form = g_form; g_form = ISLAND_COLLAPSED; g_morph = 0.0;
    }
    g_primary = best; g_secondary = second;
}

void aether_island_init(void)
{
    memset(g_act, 0, sizeof g_act);
    memset(g_priv_on, 0, sizeof g_priv_on);
    memset(g_priv_app, 0, sizeof g_priv_app);
    g_primary = g_secondary = g_popup = IS_NONE;
    g_form = g_prev_form = ISLAND_COLLAPSED;
    g_morph = 1.0; g_side_x = 0.0; g_side_from_center = 0; g_side_snap = 0;
    g_priv_panel = 0; g_last_tick = now_ms();
}

void aether_island_set(int scene, const char *title, const char *sub,
                       int progress, long auto_hide_ms)
{
    if (scene <= IS_NONE || scene >= IS_COUNT) { return; }
    struct island_act *a = &g_act[scene];
    if (!a->used) { a->start_ms = now_ms(); }
    a->used = 1;
    snprintf(a->title, sizeof a->title, "%s", title ? title : "");
    snprintf(a->sub, sizeof a->sub, "%s", sub ? sub : "");
    if (progress >= 0 && progress <= 100) { a->progress = progress; }
    a->auto_hide_ms = auto_hide_ms;
    a->playing = 1;

    /* SmartIsland IslandPriority.popsUp：短事件在无实时活动时占主位，
     * 有实时活动时退到副位等待。 */
    if (is_popup_scene(scene)) {
        if (g_primary == IS_NONE) {
            g_popup = scene;
            g_prev_form = g_form; g_form = ISLAND_EXPANDED; g_morph = 0.0;
        }
    } else if (g_primary == IS_NONE || prio(scene) > prio(g_primary)) {
        g_popup = IS_NONE;
        if (g_form == ISLAND_COLLAPSED) {
            g_prev_form = g_form; g_form = ISLAND_EXPANDED; g_morph = 0.0;
        }
    }
    recompute();
}

void aether_island_clear(int scene)
{
    if (scene <= IS_NONE || scene >= IS_COUNT) { return; }
    g_act[scene].used = 0;
    if (g_popup == scene) { g_popup = IS_NONE; }
    recompute();
}

void aether_island_clear_all(void)
{
    memset(g_act, 0, sizeof g_act);
    g_primary = g_secondary = g_popup = IS_NONE;
    g_prev_form = g_form; g_form = ISLAND_COLLAPSED; g_morph = 0.0;
    g_side_from_center = 0;
}

int aether_island_form(void) { return g_form; }

void aether_island_set_form(int form)
{
    if (form < ISLAND_COLLAPSED || form > ISLAND_CARD) { return; }
    if (form == g_form) { return; }
    g_prev_form = g_form;
    g_form = form;
    g_morph = 0.0;
}

void aether_island_swap(void)
{
    if (g_secondary == IS_NONE) { return; }
    int p = g_primary;
    g_primary = g_secondary;
    g_secondary = p;
    g_side_from_center = 1;       /* 互换时副圆从中心连续滑出 */
}

void aether_island_settle(void)
{
    g_morph = 1.0;
    g_prev_form = g_form;
    g_side_from_center = 0;
    g_side_snap = 1;      /* 截图取确定的终态，真机触摸路径仍走连续缓动 */
}

int aether_island_live(void)
{
    int main_s = g_popup != IS_NONE ? g_popup : g_primary;
    if (main_s == IS_TIMER || main_s == IS_RECORD || main_s == IS_CALL ||
        main_s == IS_MUSIC || main_s == IS_NAV || main_s == IS_DOWNLOAD) {
        return 1;
    }
    return g_morph < 1.0 ? 1 : 0;
}

void aether_island_tick(void)
{
    long now = now_ms();
    long dt = now - g_last_tick;
    g_last_tick = now;
    if (dt < 0) { dt = 0; }
    if (dt > 200) { dt = 200; }
    if (g_morph < 1.0) {
        g_morph += (double)dt / 300.0;
        if (g_morph > 1.0) { g_morph = 1.0; }
    }
    /* 充电/更新等短暂事件：到点自动收起，不打断用户 */
    for (int s = 1; s < IS_COUNT; s++) {
        struct island_act *a = &g_act[s];
        if (!a->used || a->auto_hide_ms <= 0) { continue; }
        if (now - a->start_ms >= a->auto_hide_ms) {
            a->used = 0;
            if (g_popup == s) { g_popup = IS_NONE; }
            recompute();
            if (g_primary == IS_NONE && g_popup == IS_NONE &&
                g_form != ISLAND_COLLAPSED) {
                g_prev_form = g_form; g_form = ISLAND_COLLAPSED; g_morph = 0.0;
            }
        }
    }
}

/* ---------- 几何：宽度先拉、高度后撑（照搬 ExpandMotion） ---------- */
static void form_geom(int form, int cw, int status_h, double *w, double *h)
{
    /* 宽度上限留出状态栏左侧时间与右侧信号/电量（总纲 5.8），
     * 不让灵动岛盖住系统常驻信息。 */
    if (form == ISLAND_EXPANDED) {
        *w = cw * 0.72; *h = 100;
    } else if (form == ISLAND_CARD) {
        *w = cw * 0.84; *h = 190;
    } else {
        *w = 118; *h = status_h - 8;
    }
}

static void draw_wave(int x, int y, int w, int h, long now, unsigned c)
{
    int n = 14;
    int bw = w / n;
    for (int i = 0; i < n; i++) {
        double ph = (double)now / 190.0 + i * 0.7;
        double v = 0.5 + 0.5 * sin(ph);
        int bh = (int)(h * (0.25 + 0.75 * v));
        if (bh < 2) { bh = 2; }
        aether_fill_rect(x + i * bw + 1, y + (h - bh) / 2, bw - 2, bh, c);
    }
}

static void scene_content(int s, struct island_act *a, int x, int y, int w, int h,
                          int form, long now, unsigned tx, unsigned dim,
                          unsigned accent)
{
    int icon_s = form == ISLAND_COLLAPSED ? 20 : 28;
    int ix = x + 14;
    /* 展开/卡片态：图标与标题同一行，形成「图标 + 标题 + 副标题」的稳定层级 */
    int iy = form == ISLAND_COLLAPSED ? y + (h - icon_s) / 2 : y + 14;
    unsigned ic = s == IS_RECORD ? pack_rgb(255, 92, 92) : accent;
    if (s == IS_CHARGE) { ic = pack_rgb(120, 220, 160); }
    aether_draw_icon(scene_icon(s), ix, iy, icon_s, ic);

    if (form == ISLAND_COLLAPSED) {
        char t[24];
        if (s == IS_TIMER || s == IS_RECORD || s == IS_CALL) {
            long el = (now - a->start_ms) / 1000L;
            snprintf(t, sizeof t, "%02ld:%02ld", el / 60, el % 60);
        } else if (s == IS_CHARGE) {
            snprintf(t, sizeof t, "%.23s", a->title[0] ? a->title : "充电中");
        } else {
            snprintf(t, sizeof t, "%.23s", a->title[0] ? a->title : SCENE_NAME[s]);
        }
        int tw = aether_text_width_utf8(t);
        int maxw = w - icon_s - 24;
        if (tw > maxw) {
            t[0] = 0;
            snprintf(t, sizeof t, "%.23s", SCENE_NAME[s]);
        }
        aether_draw_text_utf8(x + w - 12 - aether_text_width_utf8(t),
                              y + (h - 16) / 2, t, tx);
        return;
    }

    int tx0 = ix + icon_s + 12;
    int avail = x + w - tx0 - 16;
    /* 标题：字号分层靠位置与颜色，不用描边花字（总纲 5.7 观感底线） */
    aether_draw_text_utf8(tx0, y + 14, a->title, tx);
    aether_draw_text_utf8(tx0, y + 36, a->sub, dim);

    /* 右侧状态 / 进度数字 */
    char rt[32];
    if (s == IS_TIMER || s == IS_RECORD || s == IS_CALL) {
        long el = (now - a->start_ms) / 1000L;
        snprintf(rt, sizeof rt, "%02ld:%02ld", el / 60, el % 60);
    } else if (s == IS_CHARGE) {
        snprintf(rt, sizeof rt, "%d%%", a->progress);
    } else if (s == IS_DOWNLOAD || s == IS_UPDATE) {
        snprintf(rt, sizeof rt, "%d%%", a->progress);
    } else {
        snprintf(rt, sizeof rt, "%s", SCENE_NAME[s]);
    }
    aether_draw_text_utf8(x + w - 14 - aether_text_width_utf8(rt), y + 14, rt, accent);

    int wy = form == ISLAND_CARD ? y + 82 : y + 64;
    if (s == IS_MUSIC) {
        draw_wave(tx0, wy, avail, 24, now, accent);
    } else if (s == IS_RECORD) {
        draw_wave(tx0, wy, avail, 24, now, pack_rgb(255, 92, 92));
    } else if (s == IS_DOWNLOAD || s == IS_UPDATE || s == IS_CHARGE || s == IS_NAV) {
        aether_ui_bar(tx0, wy + 6, avail, 8, (double)a->progress, accent);
    } else {
        aether_ui_bar(tx0, wy + 6, avail, 8, 100.0, dim);
    }

    if (form == ISLAND_CARD) {
        /* 卡片态：可点控制按钮，统一控件语言 */
        const char *b1 = "暂停", *b2 = "停止";
        if (s == IS_CALL)      { b1 = "保持"; b2 = "挂断"; }
        if (s == IS_MUSIC)     { b1 = "上一曲"; b2 = "下一曲"; }
        if (s == IS_NAV)       { b1 = "概览"; b2 = "结束"; }
        if (s == IS_CHARGE)    { b1 = "电池"; b2 = "关闭"; }
        if (s == IS_DOWNLOAD)  { b1 = "暂停"; b2 = "取消"; }
        int by = y + h - 46;
        int bw2 = (avail - 12) / 2;
        aether_ui_button(tx0, by, bw2, 34, b1, pack_rgb(38, 46, 62), tx);
        aether_ui_button(tx0 + bw2 + 12, by, bw2, 34, b2, pack_rgb(58, 40, 46), tx);
    }
}

void aether_island_render(int ox, int oy, int cw, int ch, int theme, int status_h)
{
    (void)ch;
    int main_s = g_popup != IS_NONE ? g_popup : g_primary;
    if (main_s == IS_NONE) {
        g_side_x = 0.0;
        /* 隐私面板独立于实时活动：即使岛已收起也必须能显示「哪个应用在用」 */
        if (g_priv_panel) {
            aether_island_privacy_panel(ox, oy, cw, ch, theme);
        }
        return;
    }
    struct island_act *a = &g_act[main_s];
    long now = now_ms();

    double w0, h0, w1, h1;
    form_geom(g_prev_form, cw, status_h, &w0, &h0);
    form_geom(g_form, cw, status_h, &w1, &h1);
    /* PILL_OUT_END_PROGRESS=0.25：宽度先拉满；CARD_IN 0.1->0.9：高度后撑 */
    double pw = ease_out(clamp01(g_morph / 0.25));
    double ph = ease_out(clamp01((g_morph - 0.10) / 0.80));
    double dw = w0 + (w1 - w0) * pw;
    double dh = h0 + (h1 - h0) * ph;
    int iw = (int)dw, ih = (int)dh;
    if (iw < 60) { iw = 60; }
    if (ih < 18) { ih = 18; }

    int cx = ox + cw / 2;
    int x = cx - iw / 2;
    int y = oy + 4;
    unsigned bg = pack_rgb(0, 0, 0);        /* 真黑，与屏幕边框融为一体 */
    unsigned tx = pack_rgb(244, 247, 252);
    unsigned dim = pack_rgb(150, 158, 174);
    unsigned accent = theme == 1 ? pack_rgb(70, 120, 220) : pack_rgb(96, 165, 250);

    /* 副活动：侧边小圆形图标，连续位移，不瞬移 */
    int sub_s = g_popup != IS_NONE ? g_primary : g_secondary;
    double target_side = (double)(cx + iw / 2 + 22);
    if (sub_s != IS_NONE) {
        if (g_side_from_center || g_side_x == 0.0) {
            g_side_x = (double)cx;
            g_side_from_center = 0;
        }
        if (g_side_snap) {
            g_side_x = target_side;
            g_side_snap = 0;
        } else {
            g_side_x += (target_side - g_side_x) * 0.16;
        }
        int sx = (int)g_side_x;
        aether_draw_round_rect(sx - 15, y + (ih - 30) / 2, 30, 30, 15,
                               pack_rgb(22, 26, 36), 0xF0);
        aether_draw_icon(scene_icon(sub_s), sx - 10,
                         y + (ih - 30) / 2 + 5, 20, accent);
    } else {
        g_side_x = 0.0;
    }

    /* 岛主体：真黑胶囊（iOS 灵动岛规范），圆角随高度收敛。
     * 双层阴影让岛从壁纸上方浮起；顶边镜面高光与全系统控件语言一致。 */
    int radius = ih < 40 ? ih / 2 : 24;
    aether_glass_drop_shadow(x, y, iw, ih, radius);
    aether_draw_round_rect(x, y, iw, ih, radius, bg, 0xFF);
    aether_draw_border(x, y, iw, ih, pack_rgb(28, 32, 44), 1);
    aether_glass_specular_edge(x, y, iw, ih, radius, theme);

    /* 内容淡入：形变后段才显现，避免中途文字挤压 */
    double fade = clamp01((g_morph - 0.10) / 0.80);
    if (fade > 0.02) {
        aether_render_set_alpha((unsigned char)(fade * 255));
        scene_content(main_s, a, x, y, iw, ih,
                      g_morph >= 1.0 ? g_form : (dh > 150 ? ISLAND_CARD :
                                                dh > 60 ? ISLAND_EXPANDED :
                                                ISLAND_COLLAPSED),
                      now, tx, dim, accent);
        aether_render_set_alpha(0xFF);
    }
    if (g_priv_panel) {
        aether_island_privacy_panel(ox, oy, cw, ch, theme);
    }
}

/* 单击展开 / 再单击进应用 / 长按进卡片 / 左右滑切换 / 下滑收起 */
int aether_island_hit(int lx, int ly, int cw, int status_h)
{
    (void)status_h;
    int main_s = g_popup != IS_NONE ? g_popup : g_primary;
    if (main_s == IS_NONE) { return 0; }
    double w1, h1;
    form_geom(g_form, cw, status_h, &w1, &h1);
    int iw = (int)w1, ih = (int)h1;
    int x = cw / 2 - iw / 2, y = 4;
    if (lx < x || lx > x + iw || ly < y || ly > y + ih) { return 0; }
    /* 副圆命中：互换主副 */
    if (g_secondary != IS_NONE || g_popup != IS_NONE) {
        int sx = (int)g_side_x;
        if (lx > sx - 18 && lx < sx + 18) { return 4; }
    }
    if (g_form == ISLAND_COLLAPSED) { return 1; }
    if (g_form == ISLAND_EXPANDED) { return 2; }
    /* 卡片态：下半部按钮区 */
    if (ly > y + ih - 52) { return 6; }
    return 3;
}

/* ---------------- 隐私指示（5.7 / 5.19：绿点=相机麦克风，橙点=定位） ---------------- */
static unsigned priv_color(int kind)
{
    if (kind == PRIV_LOCATION) { return pack_rgb(255, 168, 64); }
    return pack_rgb(64, 208, 120);
}

int aether_island_privacy_render(int right_x, int y, int h, int theme)
{
    (void)theme;
    int n = 0;
    for (int k = 1; k <= 3; k++) {
        if (g_priv_on[k]) { n++; }
    }
    if (!n) { return 0; }
    int d = 10, gap = 4;
    int x = right_x - n * (d + gap) + gap;
    for (int k = 1; k <= 3; k++) {
        if (!g_priv_on[k]) { continue; }
        aether_draw_round_rect(x, y + (h - d) / 2, d, d, d / 2, priv_color(k), 0xFF);
        x += d + gap;
    }
    return n * (d + gap);
}

int aether_island_privacy_hit(int lx, int ly, int cw, int status_h)
{
    /* 面板已打开：命中「关闭」按钮即直接停止该应用对权限的使用 */
    if (g_priv_panel) {
        int w = cw - 80, x = 40, y = 64, h = 178;
        int row = y + 44;
        for (int k = 1; k <= 3; k++) {
            if (!g_priv_on[k]) { continue; }
            if (lx >= x + w - 76 && lx <= x + w - 16 &&
                ly >= row - 4 && ly <= row + 24) {
                aether_island_privacy_clear(k);
                int left = 0;
                for (int j = 1; j <= 3; j++) {
                    if (g_priv_on[j]) { left++; }
                }
                if (!left) { g_priv_panel = 0; }
                return 10 + k;
            }
            row += 38;
        }
        if (lx < x || lx > x + w || ly < y || ly > y + h) {
            g_priv_panel = 0;
        }
        return 1;
    }
    int n = 0;
    for (int k = 1; k <= 3; k++) {
        if (g_priv_on[k]) { n++; }
    }
    if (!n) { return 0; }
    int d = 10, gap = 4;
    int right = cw - 16;
    int x0 = right - n * (d + gap) + gap;
    if (ly > status_h || lx < x0 - 6 || lx > right + 6) { return 0; }
    g_priv_panel = 1;
    return 1;
}

void aether_island_privacy_panel(int ox, int oy, int cw, int ch, int theme)
{
    (void)ch;
    int w = cw - 80, h = 178;
    int x = ox + 40, y = oy + 64;
    unsigned bg = theme == 1 ? pack_rgb(238, 242, 250) : pack_rgb(20, 26, 40);
    unsigned tx = theme == 1 ? pack_rgb(24, 32, 48) : pack_rgb(238, 244, 252);
    aether_draw_round_rect(x, y, w, h, 20, bg, 0xFF);  /* 不透明：不许下层文字透出 */
    aether_draw_border(x, y, w, h, pack_rgb(90, 100, 120), 1);
    aether_draw_text_utf8(x + 18, y + 14, "隐私使用情况", tx);
    int row = y + 44;
    int any = 0;
    for (int k = 1; k <= 3; k++) {
        if (!g_priv_on[k]) { continue; }
        any = 1;
        const char *kn = k == PRIV_CAMERA ? "相机" :
                         k == PRIV_MIC ? "麦克风" : "定位";
        aether_draw_round_rect(x + 18, row + 2, 10, 10, 5, priv_color(k), 0xFF);
        char line[96];
        snprintf(line, sizeof line, "%s · %s 正在使用", kn,
                 g_priv_app[k][0] ? g_priv_app[k] : "未知应用");
        aether_draw_text_utf8(x + 36, row, line, tx);
        aether_ui_button(x + w - 76, row - 4, 60, 28, "关闭",
                         pack_rgb(58, 40, 46), tx);
        row += 38;
    }
    if (!any) {
        aether_draw_text_utf8(x + 18, row, "当前没有应用使用相机、麦克风或定位", tx);
    }
}

void aether_island_privacy_set(int kind, const char *app)
{
    if (kind < 1 || kind > 3) { return; }
    g_priv_on[kind] = 1;
    snprintf(g_priv_app[kind], sizeof g_priv_app[kind], "%s", app ? app : "");
}

void aether_island_privacy_clear(int kind)
{
    if (kind < 1 || kind > 3) { return; }
    g_priv_on[kind] = 0;
    g_priv_app[kind][0] = 0;
}

int aether_island_privacy_open(void) { return g_priv_panel; }
void aether_island_privacy_close(void) { g_priv_panel = 0; }

void aether_island_state(char *buf, size_t sz)
{
    int main_s = g_popup != IS_NONE ? g_popup : g_primary;
    int sub_s = g_popup != IS_NONE ? g_primary : g_secondary;
    int pc = 0;
    for (int k = 1; k <= 3; k++) {
        if (g_priv_on[k]) { pc++; }
    }
    snprintf(buf, sz,
        "{\"main\":%d,\"main_name\":\"%s\",\"sub\":%d,\"form\":%d,"
        "\"privacy_count\":%d,\"privacy_panel\":%d,\"morph\":%.2f}",
        main_s, aether_island_scene_name(main_s), sub_s, g_form,
        pc, g_priv_panel, g_morph);
}
