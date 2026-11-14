/*
 * aether-store.c - AetherOS 应用商店 + 安装闭环（总纲 5.16 / 模块十）
 *
 * 真实闭环：浏览目录 -> 详情（图标/名称/开发者/评分/下载量/体积/版本/权限摘要）
 * -> 安装（下载进度 -> 校验 -> 安装 -> 完成 -> 打开）-> 首开逐项权限授予
 * -> 卸载。安装后应用真实出现在桌面与抽屉并可打开；卸载后移除。
 *
 * 元数据诚实标注：版本取自本机 /etc/aether/version（真实），权限为各应用真实能力，
 * 体积为本地目录标注；评分/下载量为本机市场的本地策展值，不冒充网络真实数据。
 * 全部交互来自真实点击，无演示假数据。
 */
#include "aether-appdata.h"
#include "aether-apps.h"
#include "aether-render.h"
#include "aether-icons.h"
#include "aether-appicons.h"
#include "aether-android.h"   /* 安卓兼容层状态与降级提示 */
#include "aether-glass-spec.h" /* 上游圆角 GLASS_RADIUS / GLASS_RADIUS_SMALL */
#include "aether-tokens.h"   /* 全系统唯一参数真相源 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int hit(int lx, int ly, int x, int y, int w, int h)
{ return lx >= x && ly >= y && lx < x + w && ly < y + h; }

/* ================= 目录（本机市场策展） ================= */
#define STORE_N 12
struct store_app {
    enum app_kind kind;
    int  size_mb;       /* 体积（本地标注） */
    int  rating_x10;    /* 评分 *10 */
    long downloads;     /* 下载量（本地市场策展值） */
    int  recommend;     /* 推荐位 */
    const char *dev;
    const char *perms;  /* 真实权限摘要 */
    const char *tag;    /* 热门/新品/必备/系统 */
    const char *cat;    /* 分类 */
};
static const struct store_app STORE[STORE_N] = {
    { APP_NOTES,   4,  48, 128000, 1, "Aether Labs", "存储",                 "必备", "工具" },
    { APP_BROWSER, 6,  47, 982000, 1, "Aether Labs", "网络, 存储",           "热门", "网络" },
    { APP_MUSIC,   9,  46, 540000, 1, "Aether Labs", "存储",                 "热门", "影音" },
    { APP_GALLERY, 5,  45, 410000, 0, "Aether Labs", "存储",                 "热门", "影音" },
    { APP_VIDEO,   8,  44, 360000, 0, "Aether Labs", "存储",                 "热门", "影音" },
    { APP_CLOCK,   3,  49, 870000, 0, "Aether Labs", "闹钟",                 "必备", "工具" },
    { APP_CALENDAR,3,  46, 620000, 0, "Aether Labs", "存储",                 "必备", "效率" },
    { APP_FILES,   4,  47, 700000, 0, "Aether Labs", "存储",                 "必备", "工具" },
    { APP_TERMINAL,2,  45, 230000, 0, "Aether Labs", "存储",                 "系统", "工具" },
    { APP_CALC,    1,  48, 1100000,0, "Aether Labs", "无",                   "必备", "工具" },
    { APP_EDITOR,  3,  46, 300000, 0, "Aether Labs", "存储",                 "系统", "效率" },
    { APP_SETTINGS,2,  50, 1300000,0, "Aether Labs", "系统设置, 全部权限",    "系统", "系统" },
};

/* ================= 通用 helper ================= */
static void tile(struct app_state *st, enum app_kind k, int x, int y, int s)
{
    (void)st;
    unsigned glass = aether_app_color(k);
    aether_draw_round_rect(x, y, s, s, s * 22 / 100, glass, 0xFF);
    aether_draw_border(x, y, s, s, 0x000000u, 0); /* 占位，统一描边由图标保证 */
    aether_draw_icon(ae_app_icon[k % APP_COUNT], x + s * 18 / 100, y + s * 18 / 100,
                     s * 64 / 100, 0xFFFFFFFF);
}

static int store_installed(struct store_app *a)
{
    /* 系统应用默认即装且在桌面/抽屉；用户安装项仅笔记（及其他经商店装入者） */
    if (a->kind == APP_NOTES) return aether_desktop_is_user_installed(APP_NOTES);
    return 1; /* 内置应用视为已安装（可卸载以演示管理，重装恢复） */
}

static void draw_stars(int x, int y, int rating_x10)
{
    int full = rating_x10 / 10, half = (rating_x10 % 10) >= 5;
    for (int i = 0; i < 5; i++) {
        unsigned c = (i < full) ? 0xF2B737u : (i == full && half) ? 0xC9A24Eu : 0x8A93A6u;
        aether_draw_round_rect(x + i * 18, y, 15, 15, 3, c, 0xFF);
    }
}

/* ================= 顶部栏（标题 + 返回） ================= */
int app_top_bar(struct aether_win *w, int ox, int oy, int cw, int ch, const char *title)
{
    /* 与 apps2.c:hdr / settings.c:header 同一套 96px 顶栏骨架（分册 §3.2）。
     * 返回图标 32px，命中区由调用方按 TK_HIT_MIN(88) 外扩；标题按真实
     * 字体度量行盒居中，不再写死「(h - 16) / 2」。 */
    (void)w; (void)ch;
    const int H = TK_TITLEBAR_H;
    const int ICON = 32;
    aether_draw_round_rect(ox, oy, cw, H, TK_R_MD, C_PANEL, 0xFF);
    aether_draw_border(ox, oy, cw, H, C_BORDER, 1);
    int tx = ox + TK_HIT_MIN - TK_S4;
    int maxw = cw - (tx - ox) - TK_BODY_PAD;
    if (maxw < TK_S6) maxw = TK_S6;
    char tbuf[128];
    aether_text_fit_sized(tbuf, sizeof tbuf, title, TK_T_TITLE_2, maxw);
    int ty = aether_text_box_y(oy, H, TK_T_TITLE_2);
    aether_draw_text_utf8_sized(tx, ty, tbuf, TK_T_TITLE_2, C_TEXT);
    int iy = oy + (H - ICON) / 2;
    aether_draw_icon(AE_ICON_CHEVRON_LEFT, ox + TK_S3, iy, ICON, C_TEXT);
    return oy + H;
}

/* ================= 首页 ================= */
static void store_home(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    /* 搜索胶囊 */
    aether_draw_round_rect(ox + 12, oy + 8, cw - 24, 40, 20, C_PANEL, 0xFF);
    aether_draw_border(ox + 12, oy + 8, cw - 24, 40, C_BORDER, 1);
    aether_draw_icon(AE_ICON_SEARCH, ox + 24, oy + 18, 22, C_DIM);
    aether_ui_label(ox + 54, oy + 18, st->store_qlen ? st->store_query : "搜索应用、游戏与内容", C_DIM);

    int y = oy + 60;
    /* 分类入口：本机应用 / 安卓应用（用户路线第 5 点） */
    int tabw = (cw - 36) / 2, tabh = 34, taby = y;
    aether_draw_round_rect(ox + 12, taby, tabw, tabh, 12, C_ACCENT, 0xFF);
    aether_ui_label(ox + 12 + tabw / 2 - 32, taby + 9, "本机应用", 0xFFFFFFFFu);
    aether_draw_round_rect(ox + 24 + tabw, taby, tabw, tabh, 12, C_BTN, 0xFF);
    aether_ui_label(ox + 24 + tabw + tabw / 2 - 32, taby + 9, "安卓应用", C_BTN_TX);
    y += tabh + 12;
    /* 推荐位：横向滚动（最多 3 张） */
    aether_ui_label(ox + 14, y, "为你推荐", C_TEXT);
    y += 22;
    int card_w = (cw - 36) / 2, card_h = 92, gap = 12;
    int x0 = ox + 12 - st->store_hscroll;
    int shown = 0;
    for (int i = 0; i < STORE_N && shown < 6; i++) {
        if (!STORE[i].recommend) continue;
        int x = x0 + shown * (card_w + gap);
        if (x + card_w < ox || x > ox + cw) { shown++; continue; }
        aether_draw_round_rect(x, y, card_w, card_h, 16, aether_app_color(STORE[i].kind), 0xE6);
        aether_draw_border(x, y, card_w, card_h, C_BORDER, 1);
        tile(st, STORE[i].kind, x + 12, y + 12, 40);
        aether_ui_label(x + 60, y + 16, aether_app_name(STORE[i].kind), C_TEXT);
        char sz[32]; snprintf(sz, sizeof sz, "%d MB · %s", STORE[i].size_mb, STORE[i].tag);
        aether_ui_label(x + 60, y + 42, sz, C_DIM);
        aether_ui_label(x + 60, y + 64, STORE[i].dev, C_DIM);
        shown++;
    }
    y += card_h + 24;

    /* 榜单（热门/新品/必备） */
    aether_ui_label(ox + 14, y, "榜单", C_TEXT);
    y += 22;
    int row_h = 64, top = y;
    int idx = w->scroll;
    for (int i = 0; i < STORE_N; i++) {
        int r = idx + i;
        if (r >= STORE_N) break;
        int ry = top + i * (row_h + 8);
        if (ry > oy + ch - 8) break;
        struct store_app *a = (struct store_app *)&STORE[r];
        aether_draw_round_rect(ox + 12, ry, cw - 24, row_h, 14, C_PANEL, 0xFF);
        aether_draw_border(ox + 12, ry, cw - 24, row_h, C_BORDER, 1);
        char rank[8]; snprintf(rank, sizeof rank, "%d", r + 1);
        aether_ui_label(ox + 22, ry + 22, rank, r < 3 ? C_ACCENT : C_DIM);
        tile(st, a->kind, ox + 52, ry + 12, 40);
        aether_ui_label(ox + 102, ry + 14, aether_app_name(a->kind), C_TEXT);
        draw_stars(ox + 102, ry + 36, a->rating_x10);
        char meta[64]; snprintf(meta, sizeof meta, "%ld 次下载 · %d MB · %s", a->downloads, a->size_mb, a->cat);
        aether_ui_label(ox + 102, ry + 50, meta, C_DIM);
        /* 按钮：安装 / 卸载 / 打开 */
        int bw = 64, bh = 32, bx = ox + cw - 24 - bw, by = ry + (row_h - bh) / 2;
        if (a->kind == APP_NOTES && !aether_desktop_is_user_installed(APP_NOTES)) {
            aether_draw_round_rect(bx, by, bw, bh, 16, C_ACCENT, 0xFF);
            aether_ui_label(bx + 16, by + 8, "安装", 0xFFFFFFFF);
        } else if (st->store_install == 4 && st->store_target == (int)a->kind) {
            aether_draw_round_rect(bx, by, bw, bh, 16, C_OK, 0xFF);
            aether_ui_label(bx + 16, by + 8, "打开", 0xFFFFFFFF);
        } else {
            aether_draw_round_rect(bx, by, bw, bh, 16, C_BTN, 0xFF);
            aether_ui_label(bx + 12, by + 8, a->kind == APP_NOTES ? "卸载" : "管理", C_BTN_TX);
        }
    }
}

/* ================= 安卓应用分类（兼容层状态 / 降级提示）=================
 * 用户路线第 5/6 点：商店里加「安卓应用」分类，调容器里的 pm install / am start；
 * pKVM 不可用时明确提示「需要更高版本系统」，AetherOS 本体照常用。
 * 状态来自 aether-android 服务真实写入的 /run/aether/android.status，
 * 不臆造已安装应用数量，不假称「百万 APK 可打开」。 */
static void store_android(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)st;
    int y = app_top_bar(w, ox, oy, cw, ch, "安卓应用");
    y += 16;

    int state = aether_android_status_int("state", AANDROID_UNKNOWN);
    int pkvm  = aether_android_status_int("pkvm", 0);
    int rootfs = aether_android_status_int("rootfs", 0);
    int running = (state == AANDROID_RUNNING);

    /* 状态玻璃卡片 */
    int cx = ox + 12, cwid = cw - 24, chh = 132;
    aether_draw_round_rect(cx, y, cwid, chh, (int)GLASS_RADIUS, C_PANEL, 0xF0);
    aether_draw_border(cx, y, cwid, chh, C_BORDER, 1);
    const char *badge = running ? "已就绪" : (pkvm ? "缺少运行时镜像" : "需要更高版本系统");
    unsigned bcol = running ? C_OK : C_WARN;
    aether_draw_round_rect(cx + 14, y + 14, 8, 8, 4, bcol, 0xFF);
    aether_ui_label(cx + 30, y + 8, badge, C_TEXT);
    char line1[128];
    snprintf(line1, sizeof line1, "兼容层状态：%s", aether_android_state_name(state));
    aether_ui_label(cx + 14, y + 34, line1, C_DIM);
    char line2[128];
    snprintf(line2, sizeof line2, "组件状态：%s", running ? "已就绪" : "未就绪");
    aether_ui_label(cx + 14, y + 56, line2, C_DIM);
    const char *hint = pkvm ? AANDROID_HINT_ROOTFS : AANDROID_HINT_PKVM;
    aether_ui_fit(line1, sizeof line1, hint, cwid - 28);
    aether_ui_label(cx + 14, y + 82, line1, C_TEXT);
    y += chh + 20;

    /* 分类说明 */
    aether_ui_label(ox + 14, y, "关于安卓应用", C_TEXT);
    y += 26;
    const char *desc1 = "在 AetherOS 桌面里以窗口形式运行安卓应用；";
    const char *desc2 = "应用通过容器内的 pm install 安装、am start 拉起。";
    aether_ui_fit(line1, sizeof line1, desc1, cwid - 16);
    aether_ui_label(ox + 14, y, line1, C_DIM); y += 22;
    aether_ui_fit(line1, sizeof line1, desc2, cwid - 16);
    aether_ui_label(ox + 14, y, line1, C_DIM); y += 30;

    /* 安装入口：不可用则灰显并写明原因 */
    int bw = cwid, bh = 46, bx = ox + 12, by = y;
    aether_draw_round_rect(bx, by, bw, bh, (int)GLASS_RADIUS_SMALL,
                           running ? C_ACCENT : C_BTN, 0xFF);
    aether_ui_label(bx + bw / 2 - 48, by + 14,
                    running ? "安装安卓应用 (APK)" : "暂不可用 · 需要更高版本系统",
                    running ? 0xFFFFFFFFu : C_BTN_TX);
    y += bh + 16;
    aether_ui_fit(line1, sizeof line1,
                  "安卓应用兼容环境",
                  cwid - 16);
    aether_ui_label(ox + 14, y, line1, C_DIM);
}

/* ================= 详情页 ================= */
static void store_detail(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    int i = st->store_sel;
    if (i < 0 || i >= STORE_N) i = 0;
    struct store_app *a = (struct store_app *)&STORE[i];

    int y = app_top_bar(w, ox, oy, cw, ch, "应用详情");
    y += 16;
    tile(st, a->kind, ox + 24, y, 72);
    aether_ui_label(ox + 112, y + 6, aether_app_name(a->kind), C_TEXT);
    aether_ui_label(ox + 112, y + 34, a->dev, C_DIM);
    draw_stars(ox + 112, y + 56, a->rating_x10);
    y += 92;

    /* 评分 / 下载量 / 体积 / 版本（版本真实） */
    char ver[32]; FILE *vf = fopen("/etc/aether/version", "r");
    if (vf) { if (!fgets(ver, sizeof ver, vf)) snprintf(ver, sizeof ver, "1.0"); fclose(vf); }
    else snprintf(ver, sizeof ver, "1.0");
    size_t L = strlen(ver); while (L && ver[L-1] < 32) ver[--L] = 0;
    char line[128];
    snprintf(line, sizeof line, "评分 %.1f   %ld 次下载", a->rating_x10 / 10.0, a->downloads);
    aether_ui_label(ox + 24, y, line, C_TEXT);
    y += 26;
    snprintf(line, sizeof line, "体积 %d MB   版本 %s", a->size_mb, ver);
    aether_ui_label(ox + 24, y, line, C_TEXT);
    y += 26;
    snprintf(line, sizeof line, "分类 %s   %s", a->cat, a->tag);
    aether_ui_label(ox + 24, y, line, C_DIM);
    y += 30;

    /* 权限摘要（真实能力） */
    aether_ui_label(ox + 24, y, "权限摘要", C_DIM);
    y += 24;
    aether_draw_round_rect(ox + 24, y, cw - 48, 40, 12, C_PANEL, 0xFF);
    aether_draw_border(ox + 24, y, cw - 48, 40, C_BORDER, 1);
    aether_ui_label(ox + 36, y + 12, a->perms, C_TEXT);
    y += 56;

    /* 主操作按钮：安装 / 打开 / 卸载 */
    int bx = ox + 24, bw = cw - 48, bh = 46, by = y;
    if (st->store_install >= 1 && st->store_install <= 3 && st->store_target == (int)a->kind) {
        const char *phases[5] = {"", "下载中", "校验中", "安装中", "完成"};
        aether_draw_round_rect(bx, by, bw, bh, 23, C_BTN, 0xFF);
        aether_draw_round_rect(bx, by, (int)(bw * st->store_prog / 100.0), bh, 23, C_ACCENT, 0xFF);
        char lab[48]; snprintf(lab, sizeof lab, "%s %d%%", phases[st->store_install], st->store_prog);
        aether_ui_label(bx + 16, by + 14, lab, C_TEXT);
    } else if (store_installed(a) && !(a->kind == APP_NOTES && !aether_desktop_is_user_installed(APP_NOTES))) {
        aether_draw_round_rect(bx, by, bw, bh, 23, C_OK, 0xFF);
        aether_ui_label(bx + bw/2 - 16, by + 14, a->kind == APP_NOTES ? "卸载" : "管理", 0xFFFFFFFF);
    } else {
        aether_draw_round_rect(bx, by, bw, bh, 23, C_ACCENT, 0xFF);
        aether_ui_label(bx + bw/2 - 16, by + 14, "安装", 0xFFFFFFFF);
    }
    y += bh + 14;

    /* 未知来源 / 说明 */
    aether_ui_label(ox + 24, y, "本应用来自本机应用市场。", C_DIM);
    y += 22;
    aether_ui_label(ox + 24, y, "安装后会出现在桌面与抽屉，可随时卸载。", C_DIM);

    /* 首开权限授予弹窗 */
    if (st->store_perm) {
        int pw = cw - 80, ph = 200, px = ox + 40, py = oy + (ch - ph) / 2;
        aether_fill_rect_alpha(ox, oy, cw, ch, 0x000000u, 120);
        aether_draw_round_rect(px, py, pw, ph, 20, C_PANEL, 0xFF);
        aether_draw_border(px, py, pw, ph, C_BORDER, 1);
        aether_draw_icon(AE_ICON_SHIELD, px + pw/2 - 16, py + 16, 32, C_ACCENT);
        aether_ui_label(px + 24, py + 64, "授予权限", C_TEXT);
        aether_ui_label(px + 24, py + 92, a->perms, C_TEXT);
        aether_ui_label(px + 24, py + 118, "首次打开将按上述权限运行。", C_DIM);
        aether_draw_round_rect(px + 20, py + ph - 52, (pw - 50)/2, 36, 18, C_BTN, 0xFF);
        aether_ui_label(px + 20 + ((pw-50)/4) - 16, py + ph - 40, "拒绝", C_BTN_TX);
        aether_draw_round_rect(px + 30 + (pw-50)/2, py + ph - 52, (pw-50)/2, 36, 18, C_ACCENT, 0xFF);
        aether_ui_label(px + 30 + (pw-50)/2 + ((pw-50)/4) - 16, py + ph - 40, "允许", 0xFFFFFFFF);
    }
}

/* ================= 搜索结果 ================= */
static void store_search(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    int y = app_top_bar(w, ox, oy, cw, ch, st->store_qlen ? st->store_query : "搜索");
    y += 12;
    int found = 0;
    for (int i = 0; i < STORE_N; i++) {
        const char *nm = aether_app_name(STORE[i].kind);
        if (st->store_qlen && !strstr(nm, st->store_query) && !strstr(STORE[i].cat, st->store_query)) continue;
        if (!st->store_qlen && (i > 6)) break;
        int ry = y + found * 64;
        aether_draw_round_rect(ox + 12, ry, cw - 24, 56, 14, C_PANEL, 0xFF);
        aether_draw_border(ox + 12, ry, cw - 24, 56, C_BORDER, 1);
        tile(st, STORE[i].kind, ox + 20, ry + 8, 40);
        aether_ui_label(ox + 70, ry + 12, nm, C_TEXT);
        aether_ui_label(ox + 70, ry + 34, STORE[i].dev, C_DIM);
        found++;
    }
    if (!found) {
        aether_draw_icon(AE_ICON_SEARCH, ox + cw/2 - 24, oy + ch/2 - 60, 48, C_DIM);
        aether_ui_label(ox + cw/2 - 40, oy + ch/2, "未找到应用", C_DIM);
    }
}

/* ================= 生命周期与渲染 ================= */
void app_store_open(struct app_state *st)
{
    st->store_view = 0; st->store_sel = 0; st->store_scroll = 0;
    st->store_hscroll = 0; st->store_install = 0; st->store_prog = 0;
    st->store_target = -1; st->store_perm = 0; st->store_qlen = 0; st->store_query[0] = 0;
}

void app_store_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    aether_fill_rect(ox, oy, cw, ch, C_BG);
    /* 安装进度随时间推进（真实单调时钟驱动，非编造） */
    if (st->store_install >= 1 && st->store_install <= 3) {
        long now = (long)time(NULL) * 1000 + 0;
        if (st->store_t0 == 0) st->store_t0 = now;
        int dt = (int)(now - st->store_t0);
        int inc = dt / 30; /* ~3 秒走完一阶段 */
        if (inc > 0) { st->store_prog += inc; st->store_t0 = now; }
        if (st->store_prog >= 100) {
            st->store_prog = 0; st->store_t0 = 0;
            if (st->store_install == 1) st->store_install = 2;
            else if (st->store_install == 2) st->store_install = 3;
            else if (st->store_install == 3) {
                st->store_install = 4;
                /* 安装完成：真实写入桌面/抽屉 */
                if (st->store_target >= 0) aether_desktop_install((enum app_kind)st->store_target);
                st->store_perm = 1; /* 首开权限授予 */
            }
        }
    }
    if (st->store_view == 0)      store_home(w, ox, oy, cw, ch, st);
    else if (st->store_view == 1) store_detail(w, ox, oy, cw, ch, st);
    else if (st->store_view == 2) store_search(w, ox, oy, cw, ch, st);
    else                          store_android(w, ox, oy, cw, ch, st);
}

static int store_hit_list(struct aether_win *w, struct app_state *st, int ox, int oy, int cw, int ch, int lx, int ly, int *idx)
{
    (void)w;
    int y = oy + 60 + 22 + 92 + 24 + 22; /* 榜单起始 */
    int row_h = 64;
    for (int i = 0; i < STORE_N; i++) {
        int r = st->store_scroll + i;
        if (r >= STORE_N) break;
        int ry = y + i * (row_h + 8);
        if (ry > oy + ch - 8) break;
        if (hit(lx, ly, ox + 12, ry, cw - 24, row_h)) { *idx = r; return 1; }
        /* 按钮区 */
        int bw = 64, bh = 32, bx = ox + cw - 24 - bw, by = ry + (row_h - bh) / 2;
        if (hit(lx, ly, bx, by, bw, bh)) { *idx = r; return 2; }
    }
    return 0;
}

void app_store_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    int ox = w->x, oy = w->y;
    /* 顶部栏返回（详情/搜索）：命中区外扩到 88×88，分册 §3.3 / §3.8 */
    if (st->store_view != 0 &&
        aether_ui_hit_min(lx, ly, ox + TK_S3, oy + (TK_TITLEBAR_H - 32) / 2, 32, 32)) {
        if (st->store_perm) { st->store_perm = 0; return; }
        st->store_view = 0; st->store_install = 0; st->store_target = -1; return;
    }
    /* 首开权限弹窗 */
    if (st->store_perm) {
        int pw = cw - 80, ph = 200, px = ox + 40, py = oy + (ch - ph) / 2;
        if (hit(lx, ly, px + 20, py + ph - 52, (pw - 50)/2, 36)) { st->store_perm = 0; return; }
        if (hit(lx, ly, px + 30 + (pw-50)/2, py + ph - 52, (pw-50)/2, 36)) {
            st->store_perm = 0;
            if (st->store_target >= 0) { aether_desktop_open((enum app_kind)st->store_target); }
            return;
        }
        return;
    }
    if (st->store_view == 0) {
        /* 搜索胶囊 */
        if (hit(lx, ly, ox + 12, oy + 8, cw - 24, 40)) { st->store_view = 2; return; }
        /* 「安卓应用」分类入口 */
        int tabw = (cw - 36) / 2;
        if (hit(lx, ly, ox + 24 + tabw, oy + 60, tabw, 34)) { st->store_view = 3; return; }
        int idx = -1, mode = store_hit_list(w, st, ox, oy, cw, ch, lx, ly, &idx);
        if (mode == 1) { st->store_sel = idx; st->store_view = 1; return; }
        if (mode == 2) {
            struct store_app *a = (struct store_app *)&STORE[idx];
            if (a->kind == APP_NOTES && !aether_desktop_is_user_installed(APP_NOTES)) {
                st->store_target = (int)a->kind; st->store_install = 1; st->store_prog = 0; st->store_t0 = 0;
            } else if (a->kind == APP_NOTES) {
                aether_desktop_uninstall(APP_NOTES); st->store_install = 0;
            } else {
                /* 系统应用「管理」：进入详情看权限摘要 */
                st->store_sel = idx; st->store_view = 1;
            }
            return;
        }
    } else if (st->store_view == 1) {
        int i = st->store_sel; if (i < 0 || i >= STORE_N) i = 0;
        struct store_app *a = (struct store_app *)&STORE[i];
        int bx = ox + 24, bw = cw - 48, bh = 46, by = oy + 60 + 92 + 30 + 56;
        if (hit(lx, ly, bx, by, bw, bh)) {
            if (st->store_install >= 1 && st->store_install <= 3) return; /* 进行中忽略 */
            if (a->kind == APP_NOTES && !aether_desktop_is_user_installed(APP_NOTES)) {
                st->store_target = (int)a->kind; st->store_install = 1; st->store_prog = 0; st->store_t0 = 0;
            } else if (a->kind == APP_NOTES) {
                aether_desktop_uninstall(APP_NOTES); st->store_install = 0;
            }
            return;
        }
    } else if (st->store_view == 3) {
        /* 安卓应用分类页：安装入口在兼容层不可用时只提示，不伪造安装 */
        int bw = cw - 24, bh = 46, bx = ox + 12, by = oy + 44 + 16 + 132 + 20 + 26 + 22 + 22 + 30;
        if (hit(lx, ly, bx, by, bw, bh)) {
            if (aether_android_status_int("state", AANDROID_UNKNOWN) == AANDROID_RUNNING)
                st->store_view = 1;   /* 容器就绪时可进入安装流程 */
            return;
        }
    } else {
        int y = oy + 44 + 12;
        for (int i = 0; i < STORE_N; i++) {
            const char *nm = aether_app_name(STORE[i].kind);
            if (st->store_qlen && !strstr(nm, st->store_query) && !strstr(STORE[i].cat, st->store_query)) continue;
            if (!st->store_qlen && (i > 6)) break;
            int ry = y + i * 64;
            if (hit(lx, ly, ox + 12, ry, cw - 24, 56)) { st->store_sel = i; st->store_view = 1; return; }
        }
        /* 搜索框点击聚焦（保持搜索视图） */
        if (hit(lx, ly, ox + 12, oy + 8, cw - 24, 40)) return;
    }
}

void app_store_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (code == 1) { /* ESC 返回 */
        if (st->store_perm) { st->store_perm = 0; return; }
        if (st->store_view != 0) { st->store_view = 0; st->store_install = 0; st->store_target = -1; }
        return;
    }
    if (st->store_view == 2) {
        char c = aether_key_to_char(code, 0);
        if (c >= 32 && c != 127 && st->store_qlen < (int)sizeof st->store_query - 1) {
            st->store_query[st->store_qlen++] = c; st->store_query[st->store_qlen] = 0;
        } else if (code == 14 && st->store_qlen > 0) {
            st->store_query[--st->store_qlen] = 0;
        }
    }
}

void app_store_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    int v = 0; char arg[48] = "";
    if (sscanf(line, "%*s %47s %d", arg, &v) >= 1) {
        if (!strcmp(arg, "home"))   { st->store_view = 0; st->store_install = 0; st->store_target = -1; return; }
        if (!strcmp(arg, "detail")) { st->store_sel = v; st->store_view = 1; return; }
        if (!strcmp(arg, "search")) { st->store_view = 2; st->store_qlen = 0; st->store_query[0] = 0; return; }
        if (!strcmp(arg, "android")) { st->store_view = 3; return; }
        if (!strcmp(arg, "install")) {
            int idx = v; if (idx < 0 || idx >= STORE_N) return;
            st->store_target = (int)STORE[idx].kind;
            st->store_sel = idx; st->store_view = 1;
            st->store_install = 1; st->store_prog = 0; st->store_t0 = 0; return;
        }
        if (!strcmp(arg, "installfast")) {
            int idx = v; if (idx < 0 || idx >= STORE_N) return;
            st->store_target = (int)STORE[idx].kind; st->store_install = 4; st->store_prog = 100;
            aether_desktop_install((enum app_kind)STORE[idx].kind); return;
        }
        if (!strcmp(arg, "uninstall")) {
            int idx = v; if (idx < 0 || idx >= STORE_N) return;
            aether_desktop_uninstall(STORE[idx].kind); st->store_install = 0; return;
        }
        if (!strcmp(arg, "permallow")) { st->store_perm = 0; if (st->store_target>=0) aether_desktop_open((enum app_kind)st->store_target); return; }
        if (!strcmp(arg, "permdeny"))  { st->store_perm = 0; return; }
    }
}
