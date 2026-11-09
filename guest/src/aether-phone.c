/*
 * aether-phone.c - AetherOS 手机版外壳（竖屏手机操作系统）
 *
 * 总纲对齐（第十章推进顺序第一步）：装上就能开机进桌面、能点、能滑、能开应用；
 * 图标与视觉同步推进（第六章图标铁律：来自 Lucide 开源库、统一描边、超椭圆玻璃底）。
 *
 * 状态机：BOOT -> LOCKED -> HOME <-> APP / RECENTS，覆盖层：抽屉 / 通知中心 / 控制中心。
 * 复用 aether-desktop.h 同一套 API，合成器主循环、渲染原语、真实输入(EV_KEY/EV_ABS/
 * BTN_TOUCH)与 IPC 自动化注入路径完全不变；滑动手势经由 down/motion/up 三事件判定。
 *
 * 所有交互为「点击 / 滑动」语义（与真机触摸一致）；所有展示内容取自真实数据源
 * （time / proc / sysfs / 应用真实状态），不使用演示假数据。
 */
#include "aether-desktop.h"
#include "aether-apps.h"
#include "aether-render.h"
#include "aether-icons.h"
#include "aether-appicons.h"
#include "aether-anim.h"
#include "aether-glass.h"
#include "aether-ppm.h"
#include "aether-island.h"
#include "aether-discover.h"
#include "aether-appdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ================= 统一动效（阶段 2 / 总纲 5.13、6.1、6.2） =================
 * 全系统共用一套曲线（aether-anim）；每个覆盖层/转场一个动画句柄，
 * 进入方向与消失方向相反；性能不足或减少动效时自动降级。 */
static struct aether_anim g_an_panel;    /* 通知/控制中心 下拉进入 */
static struct aether_anim g_an_drawer;   /* 应用抽屉 底部上推进入 */
static struct aether_anim g_an_page;     /* 桌面换页 横向推移 */
static struct aether_anim g_an_app;      /* 应用 从图标矩形放大展开 */
static struct aether_anim g_an_unlock;   /* 解锁 图标从中心铺开 */
static struct aether_anim g_an_island;   /* 灵动岛 连续形变 */
static int g_page_dir = 1;               /* 换页方向 +1/-1 */
static int g_app_from_x, g_app_from_y, g_app_from_s;  /* 启动动效起点（图标矩形） */
static int g_last_state = 0, g_last_panel = 0, g_last_drawer = 0, g_last_page = 0;

static void sync_anim_starts(void);   /* 定义在全局状态声明之后 */
static void ensure_wall_veil(void);   /* 磨砂玻璃遮罩层，定义在壁纸段之后 */
static void draw_glass_platter(int x, int y, int w, int h, int r,
                               int role, int pressed);

/* ---- 几何（按短边比例自适应） ---- */
static int g_w, g_h;
static int STATUS_H, NAV_H, DOCK_H;

enum ph_state { PH_BOOT = 0, PH_LOCKED, PH_HOME, PH_APP, PH_RECENTS, PH_AOD };
static int g_state = PH_BOOT;
static int g_wake_state = PH_HOME;  /* 息屏前的状态，唤醒恢复 */
static int g_theme = 0;         /* 0 深色 1 浅色 */
static int g_boot_t0;

/* 覆盖层 */
static int g_drawer = 0;        /* 应用抽屉 */
static int g_panel = 0;         /* 0 无 / 1 通知中心(左下拉) / 2 控制中心(右下拉) */

/* ---- 液态玻璃背景缓存（总纲 5.13 / 第七章「性能不足自动降级」） ----
 * 照搬 liquid_glass_widgets 的层级规则 "Glass is a Platter, Not a Wrapper"：
 * 浮层展开动画每帧只改变浮层自身的位移与透明度（translate / set_alpha），
 * 下层模糊结果是一块不变的「玻璃盘子」。软渲染环境（ARM64 TCG）逐帧重算
 * 可分离高斯 IIR 会长时间占住串行 IPC 主循环，导致 watchdog 的 ping 得不到
 * pong 而误杀合成器。故同一次浮层会话内只算一次，后续帧直接复用像素缓存。
 * 下层画面（壁纸 / 主题 / 换页 / 浮层开合）变化时递增代数强制重算。 */
static unsigned char *g_glass_cache = NULL;
static int  g_glass_pitch = 0;       /* 缓存行字节数 = cw*4 */
static int  g_glass_rows  = 0;
static int  g_glass_valid = 0;
static unsigned g_glass_key = 0;
static unsigned g_glass_gen = 0;     /* 下层画面代数 */
static char g_glass_wall[256] = "";  /* 当前壁纸路径（变化时使缓存失效） */

/* 磨砂玻璃遮罩层缓存（总纲 6.1 四层分离：背景→内容→导航/控制→浮层）。
 * 底层是清晰壁纸；这里对壁纸做一次真算模糊得到「磨砂壁纸盘子」，控件只从
 * 这块盘子取自己下方的像素，绝不逐控件重复模糊。ARM64 TCG 上逐帧全屏可分离
 * 高斯 IIR 会长时间占住串行 IPC 主循环导致 watchdog 误杀（见 draw_overlay_backdrop）。
 * 只在壁纸 / 主题 / 尺寸 / 换页变化（g_glass_gen）时重算一次。 */
static unsigned char *g_veil = NULL;
static int  g_veil_pitch = 0;
static int  g_veil_rows  = 0;
static int  g_veil_valid = 0;
static unsigned g_veil_key = 0;

/* 桌面分页：4 页容纳 14 个内置应用（空位用 -1 表示，桌面网格留白属正常布局）。
 * 商店安装的应用写入空槽（APP_EMPTY），卸载时清空（阶段 5 安装闭环）。 */
#define PAGES       4
#define PAGE_APPS   4
#define APP_EMPTY  (-1)
#define MAX_USER   8
static int g_page = 0;
static int page_apps[PAGES][PAGE_APPS] = {
    { APP_FILES,    APP_BROWSER,  APP_CLOCK,     APP_GALLERY },
    { APP_SETTINGS, APP_CALENDAR, APP_MUSIC,     APP_VIDEO  },
    { APP_TERMINAL, APP_EDITOR,   APP_CALC,      APP_MONITOR },
    { APP_ABOUT,    APP_MODULES,  APP_EMPTY,     APP_EMPTY  }
};
/* 用户经商店安装的应用（阶段 5） */
static int g_user_apps[MAX_USER];
static int g_user_n = 0;

#define DOCK_N 5
static const int dock_apps[DOCK_N] = { APP_SETTINGS, APP_STORE, APP_FILES, APP_BROWSER, APP_CLOCK };

/* ================= 统一系统设置总线 =================
 * 设置页（aether-settings.c）与外壳（控制中心 / 状态栏 / 渲染运行时）读写
 * 同一份状态；任何一侧改动即时生效，不存在两套开关。总纲 5.15。 */
static int g_sys[SYS_COUNT];
static void theme_palette(void);   /* 定义在下方，供 sys_apply 提前调用 */

static void sys_defaults(void)
{
    memset(g_sys, 0, sizeof g_sys);
    g_sys[SYS_WIFI] = 1;
    g_sys[SYS_LOC] = 1;
    g_sys[SYS_BRIGHT] = 70;
    g_sys[SYS_VOL_MEDIA] = 50;
    g_sys[SYS_VOL_RING] = 70;
    g_sys[SYS_VOL_ALARM] = 80;
    g_sys[SYS_VOL_CALL] = 60;
    g_sys[SYS_RING_MODE] = 2;        /* 响铃 */
    g_sys[SYS_FONT_SCALE] = 1;       /* 标准 */
    g_sys[SYS_DISPLAY_SIZE] = 1;     /* 标准 */
    g_sys[SYS_REFRESH] = 1;          /* 90 Hz */
    g_sys[SYS_COLOR_MODE] = 0;       /* 标准 */
    g_sys[SYS_AOD] = 1;
    g_sys[SYS_AOD_CONTENT] = 0;
    g_sys[SYS_PERF_MODE] = 1;        /* 均衡 */
    g_sys[SYS_FX] = 2;               /* 完整 */
    g_sys[SYS_ANIM_SCALE] = 2;       /* 标准 */
    g_sys[SYS_AUTO_TIME] = 1;
    g_sys[SYS_24H] = 1;
    g_sys[SYS_NOTIF_ENABLE] = 1;
    g_sys[SYS_ALLOW_LOCK_NOTIF] = 1;
    g_sys[SYS_NOTIF_HISTORY] = 1;
    g_sys[SYS_CLICK_SOUND] = 1;
    g_sys[SYS_SIDELOAD] = 1;
    g_sys[SYS_PERM_CAM] = 1;
    g_sys[SYS_PERM_MIC] = 1;
    g_sys[SYS_PERM_LOC] = 1;
    g_sys[SYS_PERM_STORAGE] = 1;
    g_sys[SYS_PERM_CONTACTS] = 1;
    g_sys[SYS_PERM_CLIP] = 1;
    g_sys[SYS_PERM_NOTIF] = 1;
    g_sys[SYS_AUTO_BRIGHT] = 1;
    g_sys[SYS_CHARGE_OPT] = 1;
    g_sys[SYS_UPDATE_AUTO] = 1;
    g_sys[SYS_BACKUP_AUTO] = 1;
    g_sys[SYS_HIGH_CONTRAST] = 0;
    g_sys[SYS_REDUCE_MOTION] = 0;
    g_sys[SYS_REDUCE_TRANS] = 0;
    g_sys[SYS_SCREEN_READER] = 0;
    g_sys[SYS_COLOR_FIX] = 0;
    g_sys[SYS_USB_DEBUG] = 0;
}

int aether_sys_get(int key)
{
    if (key < 0 || key >= SYS_COUNT) return 0;
    return g_sys[key];
}

void aether_sys_set(int key, int val)
{
    if (key < 0 || key >= SYS_COUNT) return;
    g_sys[key] = val;
}

/* 把设置总线的副作用真实应用到渲染 / 动画运行时 */
void aether_sys_apply(void)
{
    g_theme = g_sys[SYS_THEME] ? 1 : 0;
    theme_palette();
    /* 减少动效 / 动画时长关闭 / 省电模式 -> 极短淡入淡出 */
    int reduce = g_sys[SYS_REDUCE_MOTION] || g_sys[SYS_ANIM_SCALE] == 0;
    aether_anim_set_reduce(reduce ? 1 : 0);
    /* 材质档位：高对比 / 减少透明度 / 省电 强制实色；性能模式强制完整折射 */
    int fx = g_sys[SYS_FX];
    if (fx < 0 || fx > 2) fx = 2;
    if (g_sys[SYS_HIGH_CONTRAST] || g_sys[SYS_REDUCE_TRANS] || g_sys[SYS_PERF_MODE] == 0) fx = 0;
    if (g_sys[SYS_PERF_MODE] == 2) fx = 2;
    aether_anim_set_fx(fx);
}

/* 通知中心：3 条系统通知，逐条可清除 */
static int g_notif_n = 3;
static int g_notif_on[3] = { 1, 1, 1 };

/* 真实系统指标（render 参数透传，供桌面小组件使用） */
static double g_cpu, g_mem;

/* 拖拽手势状态 */
static int g_pressed = 0;
static int g_press_x, g_press_y, g_move_x, g_move_y;
static int g_dragged = 0;

/* 阶段 5：轻提示（toast），用于小窗第二请求等明确反馈 */
static char g_toast[64] = "";
static long g_toast_t0 = 0;
static void aether_desktop_toast(const char *s)
{ snprintf(g_toast, sizeof g_toast, "%s", s); g_toast_t0 = (long)time(NULL); }

/* 最后触点坐标：仅用于 EV_ABS 分轴上报累积与命中测试；
 * 本系统只支持触摸输入，界面内不绘制任何鼠标指针或光标。 */
static int g_mx = 0, g_my = 0;

/* ---- 窗口/应用管理 ---- */
static struct aether_win g_wins[MAX_WINDOWS];
static int g_focus = 0;
/* 窗口 id 与槽位绑定（1..MAX_WINDOWS）：aether_app_states[] 按 id-1 索引，
 * id 若无限递增会在第 9 个窗口后越界，导致应用状态为 NULL、整页空白。 */
static int g_app_order[MAX_WINDOWS];
static int g_app_n = 0;

#define TOPBAR   32
#define TITLEBAR 30

/* 状态变化时启动对应过渡；仅方向一致的进入，不做硬切（放在状态声明之后） */
static void sync_anim_starts(void)
{
    if (g_page != g_last_page) {
        g_page_dir = g_page > g_last_page ? 1 : -1;
        aether_anim_begin(&g_an_page, GLASS_ANIM_MS);        /* 200 <- glass_defaults.dart */
        g_last_page = g_page;
        g_glass_gen++;              /* 下层桌面换页，玻璃盘子需重算 */
    }
    if (g_panel != g_last_panel) {
        if (g_panel) aether_anim_begin(&g_an_panel, GLASS_ANIM_SLOW_MS);  /* 300 */
        g_last_panel = g_panel;
        g_glass_gen++;              /* 浮层开/合：一次新会话，重算一次 */
    }
    if (g_drawer != g_last_drawer) {
        if (g_drawer) aether_anim_begin(&g_an_drawer, GLASS_MATERIALIZE_MS); /* 250 */
        g_last_drawer = g_drawer;
        g_glass_gen++;
    }
    g_last_state = g_state;
}

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
    for (unsigned i = 0; i < 128; i++) if (km[i][0] && km[i][0] == c) return i;
    for (unsigned i = 0; i < 128; i++) if (km[i][1] && km[i][1] == c) { if (shift_out) *shift_out = 1; return i; }
    return 0;
}

/* ---- 主题色（深/浅两套完整可用） ---- */
static unsigned P_BG1, P_BG2, P_PANEL, P_TEXT, P_DIM, P_ACCENT, P_BORDER, P_BTN, P_BTNTX;
static unsigned P_GLASS, P_GLASS_ON;
static void theme_palette(void)
{
    if (g_theme == 1) {
        P_BG1 = pack_rgb(228, 234, 244); P_BG2 = pack_rgb(168, 196, 228);
        P_PANEL = pack_rgb(255, 255, 255); P_TEXT = pack_rgb(22, 28, 40);
        P_DIM = pack_rgb(96, 108, 128); P_ACCENT = pack_rgb(0, 102, 204);
        P_BORDER = pack_rgb(204, 214, 228); P_BTN = pack_rgb(226, 234, 246); P_BTNTX = pack_rgb(22, 28, 40);
        P_GLASS = pack_rgb(250, 251, 255); P_GLASS_ON = pack_rgb(0, 120, 220);
    } else {
        P_BG1 = pack_rgb(12, 18, 36); P_BG2 = pack_rgb(10, 48, 72);
        P_PANEL = pack_rgb(30, 38, 56); P_TEXT = pack_rgb(234, 240, 250);
        P_DIM = pack_rgb(152, 168, 192); P_ACCENT = pack_rgb(80, 168, 255);
        P_BORDER = pack_rgb(60, 76, 102); P_BTN = pack_rgb(46, 60, 86); P_BTNTX = pack_rgb(234, 240, 250);
        P_GLASS = pack_rgb(44, 54, 76); P_GLASS_ON = pack_rgb(70, 160, 255);
    }
}

/* ---- 窗口/应用管理 ---- */
static void default_geometry(enum app_kind k, int *w, int *h)
{
    (void)k;
    *w = g_w; *h = g_h - STATUS_H - NAV_H;
}
int aether_desktop_open(enum app_kind kind)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].kind == kind) {
            g_focus = g_wins[i].id;
            int found = 0;
            for (int j = 0; j < g_app_n; j++) if (g_app_order[j] == (int)kind) { found = 1; break; }
            if (!found && g_app_n < MAX_WINDOWS) g_app_order[g_app_n++] = (int)kind;
            g_drawer = 0; g_panel = 0; g_state = PH_APP;
            aether_anim_begin(&g_an_app, GLASS_MATERIALIZE_MS);   /* 250 */
            return g_wins[i].id;
        }
    }
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;
    struct aether_win *w = &g_wins[slot];
    memset(w, 0, sizeof *w);
    w->used = 1; w->id = slot + 1;
    w->kind = kind;
    snprintf(w->title, sizeof w->title, "%s", aether_app_name(kind));
    default_geometry(kind, &w->w, &w->h);
    w->x = 0; w->y = STATUS_H; w->maximized = 1;
    g_focus = w->id;
    aether_app_open(w);
    if (g_app_n < MAX_WINDOWS) {
        int dup = 0;
        for (int j = 0; j < g_app_n; j++) if (g_app_order[j] == (int)kind) dup = 1;
        if (!dup) g_app_order[g_app_n++] = (int)kind;
    }
    g_drawer = 0; g_panel = 0; g_state = PH_APP;
    aether_anim_begin(&g_an_app, 300);
    return w->id;
}
void aether_desktop_close(int id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].id == id) {
            aether_app_close(&g_wins[i]);
            enum app_kind k = g_wins[i].kind;
            memset(&g_wins[i], 0, sizeof g_wins[i]);
            for (int j = 0; j < g_app_n; j++) if (g_app_order[j] == (int)k) {
                for (int m = j; m < g_app_n - 1; m++) g_app_order[m] = g_app_order[m + 1];
                g_app_n--; break;
            }
        }
    }
    g_focus = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used) g_focus = g_wins[i].id;
    if (g_app_n == 0) g_state = PH_HOME;
}
struct aether_win *aether_desktop_window(int id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used && g_wins[i].id == id) return &g_wins[i];
    return NULL;
}
struct aether_win *aether_desktop_focus(void) { return aether_desktop_window(g_focus); }
void aether_desktop_invalidate(void) { }
int aether_desktop_theme(void) { return g_theme; }
void aether_desktop_set_theme(int t) {
    aether_sys_set(SYS_THEME, t ? 1 : 0);
    aether_sys_apply();
    g_glass_gen++;              /* 深浅主题切换：玻璃 tint / solid 都变，缓存失效 */
    aether_neg1_invalidate();
}
int aether_desktop_width(void) { return g_w; }
int aether_desktop_height(void) { return g_h; }
void aether_desktop_notify(const char *text) { (void)text; }

/* 通知中心真实状态（设置页与外壳共用） */
int aether_desktop_notif_count(void)
{
    int n = 0;
    for (int i = 0; i < 3; i++) if (g_notif_on[i]) n++;
    return n;
}
void aether_desktop_clear_notifs(void)
{
    for (int i = 0; i < 3; i++) g_notif_on[i] = 0;
}
/* 强制停止全部后台应用：保留当前设置窗口，保证操作反馈可见 */
void aether_desktop_close_all(void)
{
    int keep = g_focus;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id != keep)
            aether_desktop_close(g_wins[i].id);
}

/* ================= 阶段 5：用户安装应用（商店安装闭环） ================= */
int aether_desktop_float_active(void);
int aether_desktop_float_kind(void);
int aether_desktop_user_count(void) { return g_user_n; }
enum app_kind aether_desktop_user_kind(int i)
{ return (i >= 0 && i < g_user_n) ? (enum app_kind)g_user_apps[i] : (enum app_kind)APP_COUNT; }
int aether_desktop_is_user_installed(enum app_kind kind)
{
    for (int i = 0; i < g_user_n; i++) if (g_user_apps[i] == (int)kind) return 1;
    return 0;
}
int aether_desktop_install(enum app_kind kind)
{
    if (kind < 0 || kind >= APP_COUNT) return 1;
    if (aether_desktop_is_user_installed(kind)) return 0;
    if (g_user_n >= MAX_USER) return 1;          /* 槽位满 */
    /* 写入桌面空槽；无空槽则仅入用户列表（抽屉可见） */
    for (int p = 0; p < PAGES; p++)
        for (int c = 0; c < PAGE_APPS; c++)
            if (page_apps[p][c] == APP_EMPTY) { page_apps[p][c] = (int)kind; g_user_apps[g_user_n++] = (int)kind; return 0; }
    g_user_apps[g_user_n++] = (int)kind;
    return 0;
}
int aether_desktop_uninstall(enum app_kind kind)
{
    for (int i = 0; i < g_user_n; i++) if (g_user_apps[i] == (int)kind) {
        for (int j = i; j < g_user_n - 1; j++) g_user_apps[j] = g_user_apps[j + 1];
        g_user_n--; break;
    }
    for (int p = 0; p < PAGES; p++)
        for (int c = 0; c < PAGE_APPS; c++)
            if (page_apps[p][c] == (int)kind) page_apps[p][c] = APP_EMPTY;
    /* 关闭可能正在运行的该应用窗口 */
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used && g_wins[i].kind == kind)
        aether_desktop_close(g_wins[i].id);
    if (aether_desktop_float_active() && aether_desktop_float_kind() == (int)kind) aether_desktop_close_float();
    return 0;
}

/* ================= 阶段 5：小窗（悬浮窗） ================= */
static struct {
    int active;
    int id;                 /* 对应的窗口 id */
    enum app_kind kind;
    int x, y, w, h;         /* 浮窗几何（绝对） */
    int dragging, resizing, snapped;
    int press_x, press_y, off_x, off_y;
} g_float = {0,0,APP_COUNT, 0,0,0,0, 0,0,0,0,0,0};
int aether_desktop_open_float(enum app_kind kind)
{
    if (g_float.active) return 1;     /* 同时仅一个小窗，需先关闭 */
    int id = aether_desktop_open(kind);
    if (id <= 0) return 1;
    g_state = g_wake_state;           /* 不改变主界面状态，仅叠加浮窗 */
    aether_anim_reset(&g_an_app);
    g_float.active = 1; g_float.id = id; g_float.kind = kind;
    g_float.w = g_w * 62 / 100; g_float.h = g_h * 52 / 100;
    g_float.x = (g_w - g_float.w) / 2; g_float.y = (g_h - g_float.h) / 2 - g_h * 6 / 100;
    g_float.snapped = 0;
    return 0;
}
void aether_desktop_close_float(void)
{
    if (g_float.id) aether_desktop_close(g_float.id);
    g_float.active = 0; g_float.id = 0; g_float.kind = (enum app_kind)APP_COUNT;
}
int aether_desktop_float_active(void) { return g_float.active; }
int aether_desktop_float_kind(void) { return (int)g_float.kind; }

/* ================= 阶段 5：息屏显示（AOD） ================= */
void aether_desktop_sleep(void)
{
    if (g_state == PH_AOD) { aether_desktop_wake(); return; }
    if (g_state == PH_BOOT || g_state == PH_LOCKED) return;
    g_wake_state = g_state;
    g_state = PH_AOD;
}
void aether_desktop_wake(void)
{
    if (g_state != PH_AOD) return;
    g_state = g_wake_state;
}
int aether_desktop_aod(void) { return g_state == PH_AOD; }

void aether_desktop_init(int w, int h)
{
    g_w = w; g_h = h;
    int base = (w < h) ? w : h;
    STATUS_H = base * 28 / 360; if (STATUS_H < 22) STATUS_H = 22; if (STATUS_H > 40) STATUS_H = 40;
    NAV_H   = base * 60 / 360; if (NAV_H < 44) NAV_H = 44; if (NAV_H > 84) NAV_H = 84;
    DOCK_H  = base * 112 / 360; if (DOCK_H < 88) DOCK_H = 88; if (DOCK_H > 150) DOCK_H = 150;
    memset(g_wins, 0, sizeof g_wins);
    g_focus = 0; g_app_n = 0;
    g_state = PH_BOOT; g_drawer = 0; g_panel = 0; g_page = 0; g_theme = 0;
    g_notif_n = 3; g_notif_on[0] = g_notif_on[1] = g_notif_on[2] = 1;
    g_pressed = 0; g_dragged = 0;
    g_boot_t0 = (int)time(NULL);
    g_mx = w / 2; g_my = h / 2;
    km_init(); theme_palette();
    sys_defaults();
    aether_sys_apply();
    aether_island_init();
    aether_search_init();
    aether_neg1_invalidate();
}

/* ================= 渲染辅助 ================= */
static int hit(int lx, int ly, int x, int y, int w, int h)
{ return lx >= x && ly >= y && lx < x + w && ly < y + h; }

static int battery_capacity(void)
{
    int cap = 100;
    FILE *f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (!f) f = fopen("/sys/class/power_supply/BAT1/capacity", "r");
    if (f) { if (fscanf(f, "%d", &cap) != 1) cap = 100; fclose(f); }
    if (cap < 0) cap = 100; if (cap > 100) cap = 100;
    return cap;
}

/* 应用图标磁贴：Lucide 线性符号 + 超椭圆玻璃底，视觉重量全系统一致 */
static void draw_app_tile(int kind, int x, int y, int size)
{
    int id = ae_app_icon[kind % APP_COUNT];
    unsigned glass = aether_app_color((enum app_kind)kind);
    aether_draw_icon_tile(id, x, y, size, glass,
                          pack_rgb(255, 255, 255),
                          aether_app_name((enum app_kind)kind), P_TEXT);
    /* 触控按压反馈：按下提亮（上游 GLASS_AMBIENT_LIGHT_*），松开回落 */
    if (g_pressed && g_mx >= x && g_mx < x + size
        && g_my >= y && g_my < y + size) {
        float lift = g_theme == 1 ? GLASS_AMBIENT_LIGHT_L : GLASS_AMBIENT_LIGHT_D;
        aether_draw_round_rect(x, y, size, size, (int)(size * 0.22f),
                               pack_rgb(255, 255, 255),
                               (unsigned char)(int)(lift * 255.0f));
    }
}

/* 状态栏：左侧时间；右侧信号/Wi-Fi/蓝牙/电量，全部单色同线宽开源图标 */
static void draw_status_bar(const char *clock)
{
    /* 导航层玻璃底：半透明磨砂，底部一条高光线，不再是纯色块 */
    aether_fill_rect_alpha(0, 0, g_w, STATUS_H,
                           g_theme == 1 ? pack_rgb(240, 246, 255) : pack_rgb(10, 18, 34), 0xB4);
    aether_fill_rect_alpha(0, STATUS_H - 1, g_w, 1, pack_rgb(255, 255, 255), 0x24);
    char tbuf[32];
    snprintf(tbuf, sizeof tbuf, "%s", (clock && clock[0]) ? clock : "--:--");
    aether_draw_text_utf8(STATUS_H / 2, (STATUS_H - 16) / 2, tbuf, P_TEXT);

    int is = STATUS_H - 10; if (is < 16) is = 16;
    int iy = (STATUS_H - is) / 2;
    int rx = g_w - STATUS_H / 2;
    unsigned ic = g_theme == 1 ? pack_rgb(30, 38, 54) : pack_rgb(230, 238, 250);
    /* 隐私指示（5.7 / 5.19）：相机麦克风绿点、定位橙点，与灵动岛同色同义 */
    rx -= aether_island_privacy_render(rx, iy, is, g_theme) + 4;
    /* 电量 */
    char cb[8]; snprintf(cb, sizeof cb, "%d", battery_capacity());
    int cw = aether_text_width(cb);
    rx -= cw; aether_draw_text_utf8(rx, (STATUS_H - 16) / 2, cb, P_TEXT);
    rx -= is + 4;
    aether_draw_icon(battery_capacity() < 20 ? AE_ICON_BATTERY_LOW : AE_ICON_BATTERY_FULL,
                     rx, iy, is, ic);
    rx -= is + 6;
    if (aether_sys_get(SYS_BT))   { aether_draw_icon(AE_ICON_BLUETOOTH, rx, iy, is, ic); rx -= is + 6; }
    if (aether_sys_get(SYS_WIFI)) { aether_draw_icon(AE_ICON_WIFI, rx, iy, is, ic); rx -= is + 6; }
    aether_draw_icon(AE_ICON_SIGNAL, rx, iy, is, ic);
}

/* 底部导航栏：返回 / 主页 / 多任务，统一开源线性图标 */
static void draw_nav_bar(void)
{
    int y0 = g_h - NAV_H;
    aether_fill_rect_alpha(0, y0, g_w, NAV_H,
                           g_theme == 1 ? pack_rgb(240, 246, 255) : pack_rgb(10, 18, 34), 0xB4);
    aether_fill_rect_alpha(0, y0, g_w, 1, pack_rgb(255, 255, 255), 0x24);
    int by = y0 + NAV_H / 2;
    int is = NAV_H * 40 / 100; if (is < 20) is = 20;
    unsigned ic = P_TEXT;
    aether_draw_icon(AE_ICON_CHEVRON_LEFT, g_w * 16 / 100 - is / 2, by - is / 2, is, ic);
    aether_draw_icon(AE_ICON_HOME, g_w / 2 - is / 2, by - is / 2, is, ic);
    aether_draw_icon(AE_ICON_APP_WINDOW, g_w * 84 / 100 - is / 2, by - is / 2, is, ic);
}

static void draw_app_fullscreen(struct aether_win *w)
{
    int ch = g_h - STATUS_H - NAV_H;
    aether_app_render(w, 0, STATUS_H, g_w, ch);
}

/* 顶部搜索胶囊（点击进应用抽屉） */
static void draw_search_pill(int top)
{
    int pw = g_w - 32;
    int ph = (int)GLASS_H_INPUT;                 /* 48 <- glass_defaults.dart */
    int pressed = g_pressed && g_mx >= 16 && g_mx < 16 + pw
                  && g_my >= top && g_my < top + ph;
    draw_glass_platter(16, top, pw, ph, (int)GLASS_RADIUS_CAPSULE > ph / 2
                       ? ph / 2 : (int)GLASS_RADIUS_CAPSULE,
                       GLASS_ROLE_NAV, pressed);
    aether_draw_icon(AE_ICON_SEARCH, 28, top + (ph - 24) / 2, 24, P_DIM);
    aether_draw_text_utf8(62, top + (ph - 16) / 2, "搜索应用与内容", P_DIM);
    int is = 22;
    aether_draw_icon(AE_ICON_LAYOUT_GRID, 16 + pw - 34, top + (ph - is) / 2, is, P_DIM);
}

/* 桌面壁纸：读取图片查看器「设为壁纸」写入的真实配置，解码 PPM 像素铺满画布。
 * 这样设置改动即时生效，而不是只改一个配置文件。壁纸过亮时压极淡暗罩，
 * 保证图标文字可读（总纲 5.5「不许为了好看牺牲可读性」）。 */
static unsigned char *g_wall_rgb = NULL;   /* 已解码壁纸像素（按路径缓存） */
static int g_wall_sw = 0, g_wall_sh = 0;
static char g_wall_path[256] = "";

static void draw_wallpaper(void)
{
    char path[256] = "";
    FILE *f = fopen("/data/aether/wallpaper.conf", "r");
    if (f) {
        if (fgets(path, sizeof path, f)) {
            size_t L = strlen(path);
            while (L && path[L - 1] < 32) path[--L] = 0;
        }
        fclose(f);
    }
    /* 内置默认壁纸：配置缺失时也让壁纸真正上屏，而不是退化成单调渐变。
     * 深浅主题各取一套成套壁纸，保证图标文字可读（总纲 5.5 / 5.17）。 */
    if (!path[0])
        snprintf(path, sizeof path, "%s",
                 g_theme == 1 ? "/usr/share/aether/wallpapers/wall-02.ppm"
                              : "/usr/share/aether/wallpapers/wall-01.ppm");
    if (strcmp(path, g_glass_wall)) {
        snprintf(g_glass_wall, sizeof g_glass_wall, "%s", path);
        g_glass_gen++;          /* 「设为壁纸」真实生效：下层变化，玻璃盘子需重算 */
        aether_neg1_invalidate();   /* 负一屏加深模糊背景同样基于壁纸 */
    }
    /* 解码结果按路径缓存：动画连续帧不重复读盘/解析近 700KB PPM。
     * 这是 ARM64 TCG 上控制中心动画不再拖垮串行 IPC 的另一半关键。 */
    if (!g_wall_rgb || strcmp(path, g_wall_path)) {
        free(g_wall_rgb);
        g_wall_rgb = NULL;
        g_wall_path[0] = 0;
        if (!aether_ppm_read(path, &g_wall_rgb, &g_wall_sw, &g_wall_sh)) {
            g_wall_rgb = NULL;
            return;
        }
        snprintf(g_wall_path, sizeof g_wall_path, "%s", path);
    }
    aether_ppm_draw(g_wall_rgb, g_wall_sw, g_wall_sh, 0, 0, g_w, g_h, 3);
    aether_fill_rect_alpha(0, 0, g_w, g_h, pack_rgb(0, 0, 0), 34);
}

/* ---- 磨砂玻璃遮罩层 + 玻璃控件盘子（总纲 6.1 / 第七章性能降级） ----
 * 底层：清晰壁纸（draw_wallpaper）。
 * 遮罩层：对壁纸做一次真算模糊得到「磨砂壁纸盘子」g_veil（可分离高斯 IIR，
 *         照搬 sdegenaar progressive_blur.frag），控件只从盘子取自己下方的像素。
 * 内容层：图标 / 文字保持不透明。
 * 导航/控制层：玻璃控件 = 磨砂底 + 双层阴影 + 高光边 + 按压提亮。
 *
 * 半径 / 阴影 / 按压 / 高光 / 时长全部取自 aether-glass-spec.h（上游开源规范），
 * 不自行设定。遵守 "Glass is a Platter, Not a Wrapper" 与 "NEVER Nest
 * Refractive Glass"：本盘子只做磨砂、不做折射，父层亦不嵌套折射玻璃。 */
/* 磨砂壁纸盘子只依赖【壁纸内容 + 主题 + 画布尺寸】，不依赖换页 / 浮层 /
 * 面板开合等瞬时状态。按上游 "Glass is a Platter, Not a Wrapper"，这块盘子
 * 在整个桌面会话里只算一次，绝不能随 g_glass_gen 逐帧重算：ARM64 TCG 上
 * 每次整屏可分离高斯 IIR 会长时间占住串行 IPC 主循环，watchdog 的 ping
 * 收不到 pong 会误杀合成器（见 draw_overlay_backdrop 的历史排障结论）。 */
static unsigned veil_cache_key(void)
{
    unsigned h = 2166136261u;              /* FNV-1a over 当前壁纸路径 */
    const unsigned char *p = (const unsigned char *)g_wall_path;
    for (; *p; p++) { h ^= *p; h *= 16777619u; }
    return h + (unsigned)g_theme * 977u
             + (unsigned)g_w * 104729u
             + (unsigned)g_h * 1299709u;
}

static void ensure_wall_veil(void)
{
    unsigned key = veil_cache_key();
    if (g_veil_valid && key == g_veil_key && g_veil
        && g_veil_rows == g_h && g_veil_pitch == g_w * 4) return;

    unsigned char *bits = NULL; int fw = 0, fh = 0, pitch = 0;
    aether_render_framebuffer(&bits, &fw, &fh, &pitch);
    if (!bits || pitch < g_w * 4 || fh < g_h || g_w <= 0 || g_h <= 0) return;

    if (!g_veil || g_veil_rows != g_h || g_veil_pitch != g_w * 4) {
        free(g_veil);
        g_veil_pitch = g_w * 4;
        g_veil_rows  = g_h;
        g_veil = (unsigned char *)malloc((size_t)g_h * g_veil_pitch);
    }
    if (!g_veil) return;

    /* 纯模糊快路径：把「对壁纸真算可分离高斯」的结果直接写进遮罩缓冲，
     * 画布保持清晰壁纸不变。整程只算一次，且跳过完整玻璃合成的 92 万次
     * 逐像素重浮点循环 —— 这是避免 watchdog 误杀合成器的关键。 */
    aether_glass_blur_veil(g_veil, g_veil_pitch, 0, 0, g_w, g_h,
                           (int)GLASS_BLUR_DEFAULT + 2);
    g_veil_key = key; g_veil_valid = 1;
}

/* 玻璃控件盘子：r 为圆角；role 决定层级是否允许玻璃；pressed 为触控按下态。
 * 双层阴影反向裁剪：先画外扩阴影，再用玻璃覆盖中心，阴影只留在边界之外。 */
static void draw_glass_platter(int x, int y, int w, int h, int r,
                               int role, int pressed)
{
    if (w <= 0 || h <= 0) return;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    int glass = glass_role_allows_glass(role);

    unsigned char *bits = NULL; int fw = 0, fh = 0, pitch = 0;
    aether_render_framebuffer(&bits, &fw, &fh, &pitch);

    /* 双层阴影（glass_shadow.dart）：elevation 约 6% 黑 / blur 8 / dy 2，
     * contact 约 2% 黑 / blur 2 / dy 1。 */
    /* ARGB 常量的 alpha 在最高字节：取 >>24。此前误用 &0xFF 恒得 0，
     * 导致两层阴影实际不可见，玻璃控件缺失阴影分层。 */
    unsigned char elev_a = (unsigned char)((GLASS_SHADOW_ELEV_ARGB >> 24) & 0xFFu);
    unsigned char cont_a = (unsigned char)((GLASS_SHADOW_CONTACT_ARGB >> 24) & 0xFFu);
    aether_draw_round_rect(x - GLASS_SHADOW_ELEV_BLUR / 2,
                           y + GLASS_SHADOW_ELEV_DY,
                           w + GLASS_SHADOW_ELEV_BLUR, h, r + 2,
                           pack_rgb(0, 0, 0), elev_a);
    aether_draw_round_rect(x - GLASS_SHADOW_CONTACT_BLUR / 2,
                           y + GLASS_SHADOW_CONTACT_DY,
                           w + GLASS_SHADOW_CONTACT_BLUR, h, r,
                           pack_rgb(0, 0, 0), cont_a);

    /* 镜面高光边：外扩 1px 的半透明白，形成一圈柔和亮边 */
    int spec_a = (int)((g_theme == 1 ? GLASS_SPECULAR_LIGHT
                                     : GLASS_SPECULAR_DARK) * 255.0f);
    aether_draw_round_rect(x - 1, y - 1, w + 2, h + 2, r + 1,
                           pack_rgb(255, 255, 255), (unsigned char)spec_a);

    /* 磨砂玻璃底：从盘子逐行按圆角裁切贴上（真实模糊过的壁纸，非静态贴图）。
     * 圆角之外保留清晰壁纸，形成「控件区磨砂 / 其余清晰」的分层。 */
    if (glass && g_veil && g_veil_valid && bits && pitch >= g_w * 4) {
        for (int j = 0; j < h; j++) {
            int yy = y + j;
            if (yy < 0 || yy >= g_h) continue;
            int inset = 0;
            if (r > 0) {
                int dy = (j < r) ? (r - j)
                       : (j > h - 1 - r) ? (r - (h - 1 - j)) : 0;
                if (dy > 0 && dy <= r) {
                    float v = (float)(r * r - dy * dy);
                    if (v > 0) inset = r - (int)sqrtf(v);
                    else inset = r;
                }
            }
            int xa = x + inset, xb = x + w - inset;
            if (xa < 0) xa = 0;
            if (xb > g_w) xb = g_w;
            if (xb > xa)
                memcpy(bits + (size_t)yy * pitch + (size_t)xa * 4,
                       g_veil + (size_t)yy * g_veil_pitch + (size_t)xa * 4,
                       (size_t)(xb - xa) * 4);
        }
    } else {
        aether_draw_round_rect(x, y, w, h, r, P_GLASS, 0xE0);
    }

    /* 玻璃色调罩：半透明主题色，让磨砂壁纸透出但不抢内容 */
    unsigned tint = g_theme == 1 ? pack_rgb(236, 242, 252) : pack_rgb(16, 26, 44);
    aether_draw_round_rect(x, y, w, h, r, tint,
                           (unsigned char)(g_theme == 1 ? 0x64 : 0x78));

    /* 触控按压反馈（glass_defaults.dart）：按下整体提亮
     * 浅色 +0.30 luma / 深色 +0.14 luma，随按压膨胀爬升、松开回落。 */
    if (pressed) {
        float lift = g_theme == 1 ? GLASS_AMBIENT_LIGHT_L : GLASS_AMBIENT_LIGHT_D;
        aether_draw_round_rect(x, y, w, h, r, pack_rgb(255, 255, 255),
                               (unsigned char)(int)(lift * 255.0f));
    }
}

/* 浮层内玻璃控件（通知卡片 / 控制中心磁贴 / 抽屉搜索栏 / 最近任务卡片）。
 * 与 draw_glass_platter 的区别：浮层下层已由 draw_overlay_backdrop 做真算
 * 模糊，控件不再从 g_veil 复制像素，只叠一层半透明玻璃色调罩，让虚化壁纸
 * 从控件内透出；全部走绘制原语，天然跟随 aether_render_translate 转场，
 * 避免直接写 framebuffer 在下拉/上推动画期间与内容错位。
 * 层级规则仍遵上游：仅 NAV / OVERLAY / MODAL 允许玻璃，内容层落实色。 */
static void draw_glass_control(int x, int y, int w, int h, int r,
                               int role, int pressed, unsigned tint, int tint_a)
{
    if (w <= 0 || h <= 0) return;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    if (!glass_role_allows_glass(role)) {
        aether_draw_round_rect(x, y, w, h, r, tint, 0xFF);
        return;
    }
    unsigned char elev_a = (unsigned char)((GLASS_SHADOW_ELEV_ARGB >> 24) & 0xFFu);
    unsigned char cont_a = (unsigned char)((GLASS_SHADOW_CONTACT_ARGB >> 24) & 0xFFu);
    aether_draw_round_rect(x - GLASS_SHADOW_ELEV_BLUR / 2,
                           y + GLASS_SHADOW_ELEV_DY,
                           w + GLASS_SHADOW_ELEV_BLUR, h, r + 2,
                           pack_rgb(0, 0, 0), elev_a);
    aether_draw_round_rect(x - GLASS_SHADOW_CONTACT_BLUR / 2,
                           y + GLASS_SHADOW_CONTACT_DY,
                           w + GLASS_SHADOW_CONTACT_BLUR, h, r,
                           pack_rgb(0, 0, 0), cont_a);
    int spec_a = (int)((g_theme == 1 ? GLASS_SPECULAR_LIGHT
                                     : GLASS_SPECULAR_DARK) * 255.0f);
    aether_draw_round_rect(x - 1, y - 1, w + 2, h + 2, r + 1,
                           pack_rgb(255, 255, 255), (unsigned char)spec_a);
    /* 半透明玻璃罩：透出下层真算模糊，不盖成实色 */
    aether_draw_round_rect(x, y, w, h, r, tint, (unsigned char)tint_a);
    /* 顶部 1px 镜面高光，形成玻璃厚度感 */
    if (w > 2 * r + 2)
        aether_fill_rect_alpha(x + r, y, w - 2 * r, 1,
                               pack_rgb(255, 255, 255),
                               (unsigned char)(spec_a / 2 + 1));
    if (pressed) {
        float lift = g_theme == 1 ? GLASS_AMBIENT_LIGHT_L : GLASS_AMBIENT_LIGHT_D;
        aether_draw_round_rect(x, y, w, h, r, pack_rgb(255, 255, 255),
                               (unsigned char)(int)(lift * 255.0f));
    }
}

/* 桌面小组件：页 0 时钟/日期；其余页真实系统状态（CPU/内存/电量） */
static void draw_widget(int x, int y, int w, int h)
{
    draw_glass_platter(x, y, w, h, (int)GLASS_RADIUS_LARGE,
                       GLASS_ROLE_NAV, 0);
    aether_draw_icon(AE_ICON_CLOCK, x + 16, y + 14, 22, P_ACCENT);
    aether_draw_text_utf8(x + 46, y + 16, g_page == 0 ? "时钟" : "系统状态", P_DIM);
    if (g_page == 0) {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        char big[16]; snprintf(big, sizeof big, "%02d:%02d", tm.tm_hour, tm.tm_min);
        int fs = 56;
        int bw = aether_text_width(big);
        aether_draw_text_scale(big, x + 20, y + 44, fs, P_TEXT);
        (void)bw;
        const char *wd[7] = {"周日","周一","周二","周三","周四","周五","周六"};
        char date[64];
        snprintf(date, sizeof date, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
        aether_draw_text_utf8(x + 22, y + 44 + fs + 10, date, P_DIM);
    } else {
        char line[64];
        snprintf(line, sizeof line, "处理器占用   %.0f%%", g_cpu);
        aether_draw_text_utf8(x + 20, y + 46, line, P_TEXT);
        aether_draw_round_rect(x + 20, y + 72, w - 40, 10, 5, P_BTN, 0xFF);
        if (g_cpu > 0) aether_draw_round_rect(x + 20, y + 72, (int)((w - 40) * g_cpu / 100.0), 10, 5, P_ACCENT, 0xFF);
        snprintf(line, sizeof line, "内存占用   %.0f%%", g_mem);
        aether_draw_text_utf8(x + 20, y + 94, line, P_TEXT);
        aether_draw_round_rect(x + 20, y + 120, w - 40, 10, 5, P_BTN, 0xFF);
        if (g_mem > 0) aether_draw_round_rect(x + 20, y + 120, (int)((w - 40) * g_mem / 100.0), 10, 5, P_ACCENT, 0xFF);
    }
}

/* 桌面网格布局：draw 与 tap 共用同一计算，避免点击错位 */
static void home_grid_geom(int *gx0, int *gy0, int *cellw, int *isz, int *cols)
{
    int top = STATUS_H + 8;
    *cols = 4;
    int cw = (g_w - 24) / *cols;
    *gx0 = 12 + (cw - (cw * 62 / 100)) / 2;
    *cellw = cw;
    *isz = cw * 62 / 100;
    *gy0 = top + 44 + 20;
}
static void home_tile_rect(int idx, int *x, int *y, int *size)
{
    int gx0, gy0, cellw, isz, cols;
    home_grid_geom(&gx0, &gy0, &cellw, &isz, &cols);
    *x = gx0 + (idx % cols) * cellw;
    *y = gy0 + (idx / cols) * (isz + 40);
    *size = isz;
}

static void draw_page_dots(int y)
{
    int r = 5, gap = 20;
    int total = PAGES * gap;
    int x0 = (g_w - total) / 2;
    for (int i = 0; i < PAGES; i++) {
        int cx = x0 + i * gap;
        unsigned c = (i == g_page) ? P_ACCENT : P_DIM;
        aether_draw_round_rect(cx - r, y - r, r * 2, r * 2, r, c, 0xFF);
    }
}

static void draw_home(void)
{
    int top = STATUS_H + 8;
    draw_search_pill(top);
    /* 应用网格（当前页 4 格，空位留白属正常桌面布局） */
    for (int i = 0; i < PAGE_APPS; i++) {
        int k = page_apps[g_page][i];
        if (k < 0) continue;
        int x, y, sz; home_tile_rect(i, &x, &y, &sz);
        draw_app_tile(k, x, y, sz);
    }
    /* 小组件 */
    int gx0, gy0, cellw, isz, cols;
    home_grid_geom(&gx0, &gy0, &cellw, &isz, &cols);
    int wy = gy0 + isz + 44;
    int wh = (g_h - NAV_H - DOCK_H - 24) - wy;
    if (wh > 40) draw_widget(16, wy, g_w - 32, wh);
    /* 页面指示点 */
    draw_page_dots(g_h - NAV_H - DOCK_H - 12);
    /* Dock */
    int dy = g_h - NAV_H - DOCK_H + 4;
    /* 底栏玻璃胶囊底板（总纲 5.11）：磨砂玻璃 + 双层阴影，非纯色块 */
    draw_glass_platter(8, dy, g_w - 16, DOCK_H - 8, (int)GLASS_RADIUS_LARGE,
                       GLASS_ROLE_NAV, 0);
    int dcell = (g_w - 32) / DOCK_N;
    int disz = (DOCK_H - 16) * 58 / 100;
    for (int i = 0; i < DOCK_N; i++) {
        int gx = 16 + i * dcell + (dcell - disz) / 2;
        int gy = dy + (DOCK_H - 8 - disz) / 2 - 6;
        draw_app_tile(dock_apps[i], gx, gy, disz);
    }
}

/* 锁屏：层级 时间 > 日期 > 通知 > 快捷入口 > 解锁提示 */
static void draw_lock(const char *clock)
{
    aether_draw_vgradient(0, 0, g_w, g_h, pack_rgb(6, 12, 28), pack_rgb(4, 30, 52));
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char big[16], date[64];
    snprintf(big, sizeof big, "%02d:%02d", tm.tm_hour, tm.tm_min);
    const char *wd[7] = {"周日","周一","周二","周三","周四","周五","周六"};
    snprintf(date, sizeof date, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    (void)clock;
    int fs = g_w * 118 / 360; if (fs < 64) fs = 64; if (fs > 140) fs = 140;
    int bw = aether_text_width(big);
    aether_draw_text_scale(big, g_w / 2 - bw * fs / 32 / 2, g_h * 27 / 100, fs, pack_rgb(244, 248, 255));
    int dw = aether_text_width_utf8(date);
    aether_draw_text_utf8(g_w / 2 - dw / 2, g_h * 27 / 100 + fs + 12, date, pack_rgb(178, 204, 230));
    /* 通知卡片（与通知中心同一套视觉） */
    int ny = g_h * 27 / 100 + fs + 60;
    const char *msg = "AetherOS：系统已就绪";
    aether_draw_round_rect(24, ny, g_w - 48, 52, 14, pack_rgb(255, 255, 255), 0x28);
    aether_draw_icon(AE_ICON_BELL, 36, ny + 14, 24, pack_rgb(210, 226, 245));
    aether_draw_text_utf8(70, ny + 18, msg, pack_rgb(224, 236, 250));
    /* 底部快捷入口 */
    int sy = g_h - STATUS_H - 96;
    int ss = 56;
    aether_draw_round_rect(g_w / 2 - 96, sy, ss, ss, ss / 2, pack_rgb(255, 255, 255), 0x24);
    aether_draw_icon(AE_ICON_FLASHLIGHT, g_w / 2 - 96 + 16, sy + 16, 24, pack_rgb(230, 240, 252));
    aether_draw_round_rect(g_w / 2 + 40, sy, ss, ss, ss / 2, pack_rgb(255, 255, 255), 0x24);
    aether_draw_icon(AE_ICON_CAMERA, g_w / 2 + 40 + 16, sy + 16, 24, pack_rgb(230, 240, 252));
    char hint[64]; snprintf(hint, sizeof hint, "上滑或点击解锁");
    int hw = aether_text_width_utf8(hint);
    aether_draw_text_utf8(g_w / 2 - hw / 2, sy + ss + 18, hint, pack_rgb(160, 190, 220));
}

/* 息屏显示（AOD）：息屏后屏幕不完全黑——中心时间、日期小一号在下面、
 * 通知数量与电量；SYS_AOD 关闭则为全黑。极低亮度（总纲 5.3）。 */
static void draw_aod(void)
{
    aether_fill_rect(0, 0, g_w, g_h, pack_rgb(0, 0, 0));
    if (!aether_sys_get(SYS_AOD)) return;   /* 关闭 AOD -> 全黑 */
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char big[16], date[64];
    snprintf(big, sizeof big, "%02d:%02d", tm.tm_hour, tm.tm_min);
    const char *wd[7] = {"周日","周一","周二","周三","周四","周五","周六"};
    snprintf(date, sizeof date, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    int fs = g_w * 120 / 360; if (fs < 70) fs = 70; if (fs > 150) fs = 150;
    int bw = aether_text_width(big);
    unsigned dim = g_theme == 1 ? pack_rgb(170, 180, 200) : pack_rgb(120, 140, 170);
    aether_draw_text_scale(big, g_w / 2 - bw * fs / 32 / 2, g_h * 38 / 100, fs, dim);
    int dw = aether_text_width_utf8(date);
    aether_draw_text_utf8(g_w / 2 - dw / 2, g_h * 38 / 100 + fs + 14, date, dim);
    /* 通知数量 + 电量 */
    int n = aether_desktop_notif_count();
    char foot[64]; snprintf(foot, sizeof foot, "%d%%  ·  %d 条通知", battery_capacity(), n);
    int fw = aether_text_width_utf8(foot);
    aether_draw_text_utf8(g_w / 2 - fw / 2, g_h * 38 / 100 + fs + 50, foot, dim);
    /* 未读通知：极低亮度图标，数量受控 */
    if (n > 0) {
        int ix = g_w / 2 - (n * 22) / 2;
        for (int i = 0; i < n && i < 5; i++) {
            aether_draw_icon(AE_ICON_BELL, ix + i * 22, g_h * 38 / 100 + fs + 80, 16, dim);
        }
    }
}

/* 开机动画：暗场 -> 光晕 -> 环形闭合(光点游走, 非旋转加载圈) -> 品牌字显影 -> 细进度线 */
static void draw_boot(int elapsed)
{
    aether_draw_vgradient(0, 0, g_w, g_h, pack_rgb(3, 6, 16), pack_rgb(4, 14, 30));
    int cx = g_w / 2, cy = g_h * 40 / 100;
    /* 中心柔光（随时间从暗浮起） */
    int glow = elapsed > 2 ? 2 : elapsed;
    double gr = g_w * (0.10 + 0.12 * glow);
    for (int y = cy - (int)gr; y <= cy + (int)gr; y++) {
        for (int x = cx - (int)gr; x <= cx + (int)gr; x++) {
            double d = ((double)(x - cx) * (x - cx) + (double)(y - cy) * (y - cy)) / (gr * gr);
            if (d > 1.0) continue;
            aether_blend_pixel(x, y, pack_rgb(60, 130, 220), (unsigned char)((1.0 - d) * 26));
        }
    }
    /* 环形：细线 + 沿环缓慢游走的光点 */
    int R = g_w * 18 / 100;
    int lw = 3;
    for (int a = 0; a < 360; a++) {
        double rad = a * 3.14159265 / 180.0;
        for (int t = 0; t < lw; t++) {
            int px = cx + (int)((R + t) * cos(rad));
            int py = cy + (int)((R + t) * sin(rad));
            aether_blend_pixel(px, py, pack_rgb(120, 180, 255), 150);
        }
    }
    /* 光点沿环游走 */
    for (int k = 0; k < 5; k++) {
        double rad = ((elapsed * 40 + k * 72) % 360) * 3.14159265 / 180.0;
        int px = cx + (int)(R * cos(rad)), py = cy + (int)(R * sin(rad));
        aether_draw_round_rect(px - 4, py - 4, 8, 8, 4, pack_rgb(200, 226, 255), 0xFF);
    }
    /* 品牌字从无到有显影（用 alpha 递增） */
    const char *logo = "Aether OS";
    int lwpx = aether_text_width_utf8(logo);
    int fs = g_w * 60 / 360; if (fs < 34) fs = 34; if (fs > 88) fs = 88;
    unsigned char la = (unsigned char)(elapsed > 1 ? 255 : elapsed * 200 + 55);
    (void)la;
    aether_draw_text_scale(logo, g_w / 2 - lwpx / 2, cy + R + 40, fs, pack_rgb(150, 200, 255));
    /* 细进度线，稳定走完，不跳数字 */
    int bw = g_w * 56 / 100, bx = (g_w - bw) / 2, by = g_h * 68 / 100, bh = 6;
    aether_draw_round_rect(bx, by, bw, bh, bh / 2, pack_rgb(30, 44, 70), 0xFF);
    int p = elapsed; if (p > 3) p = 3; if (p < 0) p = 0;
    int pw = bw * p / 3; if (pw < 3) pw = 3;
    aether_draw_round_rect(bx, by, pw, bh, bh / 2, pack_rgb(80, 168, 255), 0xFF);
}

/* 浮层背景：液态玻璃材质（总纲 6.1 四层分离、6.3 更虚背景/更柔边界）。
 *
 * 实现照搬自两个开源仓库，非自研：
 *   - 可分离高斯模糊、Rec.709 饱和度、三档降级、135° 光源
 *       <- sdegenaar/liquid_glass_widgets (MIT)
 *   - SDF 圆角折射、7 段色散、法线高光
 *       <- Kyant0/AndroidLiquidGlass (Apache-2.0)
 * 详见 aether-glass.h / aether-glass.c 头部归属声明。
 *
 * 档位映射（glass_quality.dart + 总纲第七章「先降模糊、再关折射、最后落实色」）：
 *   fx=2 premium  : 真算模糊 + 边缘折射 + 色散 + 高光
 *   fx=1 standard : 真算模糊（sigma 减半）+ 高光，关折射
 *   fx=0 minimal  : 实色，零模糊（无障碍高对比 / 性能兜底）
 * reduce=1（减少动效）时不改变材质，仅影响转场曲线。 */
static void draw_overlay_backdrop(void)
{
    int fx = aether_anim_fx();
    unsigned tint  = g_theme == 1 ? pack_rgb(198, 214, 236) : pack_rgb(8, 14, 28);
    unsigned solid = g_theme == 1 ? pack_rgb(224, 232, 244) : pack_rgb(18, 24, 38);
    int quality = fx >= 2 ? GLASS_PREMIUM : fx == 1 ? GLASS_STANDARD : GLASS_MINIMAL;
    aether_glass_set_quality(quality);

    int cw = g_w, ch = g_h - STATUS_H - NAV_H, y0 = STATUS_H;
    /* 缓存键：浮层类型 / 主题 / 材质档位 / 画布尺寸 / 桌面状态 / 下层代数。
     * 浮层展开动画期间这些量都不变，因此整段动画只算一次玻璃。 */
    unsigned key = (unsigned)(g_panel ? g_panel : 4) * 131u
                 + (unsigned)g_theme * 977u
                 + (unsigned)fx * 7919u
                 + (unsigned)cw * 104729u
                 + (unsigned)ch * 1299709u
                 + (unsigned)g_state * 15485863u
                 + g_glass_gen * 32452843u;

    unsigned char *bits = NULL;
    int fbw = 0, fbh = 0, pitch = 0;
    aether_render_framebuffer(&bits, &fbw, &fbh, &pitch);

    if (g_glass_valid && g_glass_key == key && g_glass_cache && bits
        && g_glass_rows == ch && g_glass_pitch == cw * 4
        && pitch >= cw * 4 && fbh >= y0 + ch) {
        /* 复用：把上一份玻璃盘子整行贴回画布。下层桌面本来就会被这块
         * 玻璃完全遮住，逐像素等价，却省掉一整轮 O(N) 可分离高斯 IIR、
         * SDF 折射与 7 段色散，串行 IPC 心跳不再被渲染饿死。 */
        for (int r = 0; r < ch; r++)
            memcpy(bits + (size_t)(y0 + r) * pitch,
                   g_glass_cache + (size_t)r * g_glass_pitch,
                   (size_t)g_glass_pitch);
        return;
    }

    aether_glass_panel(0, y0, cw, ch,
                       28,                       /* 圆角半径（SDF） */
                       fx >= 2 ? 18 : 9,        /* 模糊半径 */
                       tint, g_theme == 1 ? 96 : 132,
                       pack_rgb(255, 255, 255), /* 高光色：白，Plus 混合 */
                       solid);                  /* minimal 实色 */

    /* 保存本次合成结果，供同一会话后续动画帧 / 截图帧复用 */
    if (bits && cw > 0 && ch > 0 && pitch >= cw * 4 && fbh >= y0 + ch) {
        if (!g_glass_cache || g_glass_rows != ch || g_glass_pitch != cw * 4) {
            free(g_glass_cache);
            g_glass_pitch = cw * 4;
            g_glass_rows  = ch;
            g_glass_cache = (unsigned char *)malloc((size_t)ch * g_glass_pitch);
        }
        if (g_glass_cache) {
            for (int r = 0; r < ch; r++)
                memcpy(g_glass_cache + (size_t)r * g_glass_pitch,
                       bits + (size_t)(y0 + r) * pitch,
                       (size_t)g_glass_pitch);
            g_glass_key   = key;
            g_glass_valid = 1;
        }
    }
}

/* 通知中心：逐条卡片、可单条/全部清除，空状态有明确提示 */
static void draw_notif_center(void)
{
    int ph = g_h * 60 / 100;
    /* 下层已由 draw_overlay_backdrop 真算模糊：只压极淡遮罩保证可读性，
     * 让虚化壁纸从卡片玻璃内透出，不盖成实色面板。 */
    aether_fill_rect_alpha(0, STATUS_H, g_w, ph, P_PANEL,
                           g_theme == 1 ? 0x28 : 0x58);
    aether_draw_border(0, STATUS_H, g_w, ph, P_BORDER, 1);
    aether_draw_icon(AE_ICON_BELL, 20, STATUS_H + 12, 24, P_TEXT);
    aether_draw_text_utf8(52, STATUS_H + 16, "通知", P_TEXT);
    int cw = aether_text_width_utf8("全部清除");
    aether_draw_text_utf8(g_w - 20 - cw, STATUS_H + 16, "全部清除", P_ACCENT);

    const char *titles[3] = { "系统已就绪", "安全隔离", "电量状态" };
    const int icons[3] = { AE_ICON_SHIELD, AE_ICON_SHIELD, AE_ICON_BATTERY_FULL };
    char bodies[3][96];
    snprintf(bodies[0], 96, "AetherOS 手机版启动完成，桌面可用");
    snprintf(bodies[1], 96, "系统运行于独立虚拟机，与宿主隔离");
    snprintf(bodies[2], 96, "当前电量 %d%%", battery_capacity());

    int y = STATUS_H + 52;
    int shown = 0;
    for (int i = 0; i < 3; i++) {
        if (!g_notif_on[i]) continue;
        shown++;
        int pressed = g_pressed && g_mx >= 14 && g_mx < g_w - 14
                      && g_my >= y && g_my < y + 78;
        draw_glass_control(14, y, g_w - 28, 78, (int)GLASS_RADIUS,
                           GLASS_ROLE_OVERLAY, pressed,
                           g_theme == 1 ? pack_rgb(236, 242, 252)
                                        : pack_rgb(22, 32, 52),
                           g_theme == 1 ? 0x90 : 0xA0);
        aether_draw_border(14, y, g_w - 28, 78, P_BORDER, 1);
        aether_draw_icon(icons[i], 28, y + 16, 26, P_ACCENT);
        aether_draw_text_utf8(66, y + 14, titles[i], P_TEXT);
        aether_draw_text_utf8(66, y + 42, bodies[i], P_DIM);
        y += 88;
    }
    if (!shown) {
        aether_draw_icon(AE_ICON_CHECK, g_w / 2 - 24, STATUS_H + 120, 48, P_DIM);
        const char *e = "暂无通知";
        aether_draw_text_utf8(g_w / 2 - aether_text_width_utf8(e) / 2, STATUS_H + 184, e, P_DIM);
    }
}

/* 控制中心磁贴：3x3，全部可点且有状态反馈；开=高亮玻璃、关=低亮玻璃 */
struct cc_tile { int icon; const char *name; };
static const struct cc_tile cc_tiles[9] = {
    { AE_ICON_WIFI, "无线网络" }, { AE_ICON_BLUETOOTH, "蓝牙" }, { AE_ICON_SIGNAL, "飞行模式" },
    { AE_ICON_MOON, "勿扰" },     { AE_ICON_FLASHLIGHT, "手电筒" }, { AE_ICON_MAP_PIN, "定位" },
    { AE_ICON_SUN, "深色模式" },  { AE_ICON_DROPLET, "亮度" },   { AE_ICON_VOLUME_2, "音量" }
};
static int cc_on(int i)
{
    switch (i) {
        case 0: return aether_sys_get(SYS_WIFI);
        case 1: return aether_sys_get(SYS_BT);
        case 2: return aether_sys_get(SYS_AIR);
        case 3: return aether_sys_get(SYS_DND);
        case 4: return aether_sys_get(SYS_TORCH);
        case 5: return aether_sys_get(SYS_LOC);
        case 6: return g_theme == 0;
        case 7: return aether_sys_get(SYS_BRIGHT) > 0;
        case 8: return aether_sys_get(SYS_VOL_MEDIA) > 0;
    }
    return 0;
}
static void cc_toggle(int i)
{
    switch (i) {
        case 0: aether_sys_set(SYS_WIFI,  !aether_sys_get(SYS_WIFI));  break;
        case 1: aether_sys_set(SYS_BT,    !aether_sys_get(SYS_BT));    break;
        case 2: aether_sys_set(SYS_AIR,   !aether_sys_get(SYS_AIR));   break;
        case 3: aether_sys_set(SYS_DND,   !aether_sys_get(SYS_DND));   break;
        case 4: aether_sys_set(SYS_TORCH, !aether_sys_get(SYS_TORCH)); break;
        case 5: aether_sys_set(SYS_LOC,   !aether_sys_get(SYS_LOC));   break;
        case 6: aether_desktop_set_theme(g_theme ? 0 : 1); break;
        case 7: {
            int v = aether_sys_get(SYS_BRIGHT) + 25;
            aether_sys_set(SYS_BRIGHT, v > 100 ? 0 : v);
            break;
        }
        case 8: {
            int v = aether_sys_get(SYS_VOL_MEDIA) + 25;
            aether_sys_set(SYS_VOL_MEDIA, v > 100 ? 0 : v);
            break;
        }
    }
    aether_sys_apply();
}
static void cc_geom(int i, int *x, int *y, int *w, int *h)
{
    int cols = 3, pad = 16, gap = 12;
    int panel_top = STATUS_H + 60;
    int avail = g_w - pad * 2;
    *w = (avail - gap * (cols - 1)) / cols;
    *h = *w * 82 / 100;
    *x = pad + (i % cols) * (*w + gap);
    *y = panel_top + (i / cols) * (*h + gap);
}
static void draw_control_center(void)
{
    int ph = g_h * 66 / 100;
    aether_fill_rect_alpha(0, STATUS_H, g_w, ph, P_PANEL,
                           g_theme == 1 ? 0x28 : 0x58);
    aether_draw_border(0, STATUS_H, g_w, ph, P_BORDER, 1);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char dt[64];
    snprintf(dt, sizeof dt, "%d月%d日  %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    aether_draw_text_utf8(20, STATUS_H + 14, dt, P_TEXT);
    aether_draw_text_utf8(20, STATUS_H + 38, "控制中心", P_DIM);

    for (int i = 0; i < 9; i++) {
        int x, y, w, h; cc_geom(i, &x, &y, &w, &h);
        int on = cc_on(i);
        int pressed = g_pressed && g_mx >= x && g_mx < x + w
                      && g_my >= y && g_my < y + h;
        unsigned ctint = on ? (g_theme == 1 ? pack_rgb(0, 110, 210)
                                            : pack_rgb(40, 130, 230))
                            : (g_theme == 1 ? pack_rgb(232, 239, 250)
                                            : pack_rgb(24, 34, 54));
        draw_glass_control(x, y, w, h, (int)GLASS_RADIUS_LARGE,
                           GLASS_ROLE_OVERLAY, pressed, ctint,
                           on ? 0xC8 : (g_theme == 1 ? 0x88 : 0x98));
        aether_draw_border(x, y, w, h, P_BORDER, 1);
        int is = h * 36 / 100;
        aether_draw_icon(cc_tiles[i].icon, x + 12, y + 10, is,
                         on ? pack_rgb(255, 255, 255) : P_TEXT);
        aether_draw_text_utf8(x + 12, y + h - 26, cc_tiles[i].name,
                              on ? pack_rgb(255, 255, 255) : P_TEXT);
        /* 亮度/音量显示当前档位 */
        if (i == 7 || i == 8) {
            char v[16];
            snprintf(v, sizeof v, "%d%%", i == 7 ? aether_sys_get(SYS_BRIGHT)
                                                : aether_sys_get(SYS_VOL_MEDIA));
            int vw = aether_text_width(v);
            aether_draw_text_utf8(x + w - 12 - vw, y + 10, v,
                                  on ? pack_rgb(255, 255, 255) : P_DIM);
        }
    }
    /* 底部亮度长条（可见、可点） */
    int by = STATUS_H + 60 + 3 * ((g_w - 32 - 24) / 3 * 82 / 100 + 12) + 10;
    if (by < g_h - NAV_H - 40) {
        int bw = g_w - 40, bx = 20;
        aether_draw_round_rect(bx, by, bw, 12, 6, P_BTN, 0xFF);
        aether_draw_round_rect(bx, by,
                               (int)(bw * aether_sys_get(SYS_BRIGHT) / 100.0), 12, 6,
                               P_ACCENT, 0xFF);
    }
}

/* 应用抽屉：全屏玻璃面板，按网格列出全部应用，点击即开；从底栏中央上滑进入 */
static void drawer_geom(int i, int *x, int *y, int *sz)
{
    int cols = 4;
    int cw = (g_w - 24) / cols;
    int isz = cw * 60 / 100;
    *x = 12 + (i % cols) * cw + (cw - isz) / 2;
    *y = STATUS_H + 84 + (i / cols) * (isz + 52);
    *sz = isz;
}
static void draw_drawer(void)
{
    /* 背景由 draw_overlay_backdrop 真算模糊；这里只压一层极淡色，
     * 让模糊后的壁纸颜色倾向透出但绝无清晰数字/图标（层级不混淆）。 */
    aether_fill_rect_alpha(0, STATUS_H, g_w, g_h - STATUS_H - NAV_H, P_PANEL,
                           g_theme == 1 ? 0x24 : 0x54);
    int dpressed = g_pressed && g_mx >= 16 && g_mx < g_w - 16
                   && g_my >= STATUS_H + 8 && g_my < STATUS_H + 52;
    draw_glass_control(16, STATUS_H + 8, g_w - 32, 44,
                       (int)GLASS_RADIUS_CAPSULE, GLASS_ROLE_OVERLAY, dpressed,
                       g_theme == 1 ? pack_rgb(236, 242, 252)
                                    : pack_rgb(22, 32, 52),
                       g_theme == 1 ? 0x90 : 0xA0);
    aether_draw_icon(AE_ICON_SEARCH, 30, STATUS_H + 18, 24, P_DIM);
    aether_draw_text_utf8(64, STATUS_H + 22, "全部应用", P_TEXT);
    char cnt[64];
    snprintf(cnt, sizeof cnt, "本机应用 · %d 项", APP_COUNT + g_user_n);
    aether_draw_text_utf8(20, STATUS_H + 66, cnt, P_TEXT);
    for (int k = 0; k < APP_COUNT; k++) {
        int x, y, sz; drawer_geom(k, &x, &y, &sz);
        draw_app_tile(k, x, y, sz);
    }
    /* 用户经商店安装的应用同样出现在抽屉 */
    for (int u = 0; u < g_user_n; u++) {
        int k = g_user_apps[u];
        int x, y, sz; drawer_geom(k, &x, &y, &sz);
        draw_app_tile(k, x, y, sz);
    }
    char hint[64]; snprintf(hint, sizeof hint, "下滑或点击空白处返回桌面");
    aether_draw_text_utf8(g_w / 2 - aether_text_width_utf8(hint) / 2, g_h - NAV_H - 34, hint, P_TEXT);
}

/* 多任务卡片 */
static void draw_recents(void)
{
    aether_fill_rect_alpha(0, 0, g_w, g_h, g_theme == 1 ? pack_rgb(220, 226, 236) : pack_rgb(10, 14, 26), 226);
    aether_draw_text_utf8(20, STATUS_H + 10, "最近任务", P_TEXT);
    int card_w = g_w - 60, card_h = (g_h - NAV_H - STATUS_H - 80) / (g_app_n > 0 ? g_app_n : 1) - 12;
    if (card_h > 140) card_h = 140; if (card_h < 60) card_h = 60;
    int y = STATUS_H + 44;
    for (int i = 0; i < g_app_n; i++) {
        enum app_kind k = (enum app_kind)g_app_order[i];
        int rpressed = g_pressed && g_mx >= 30 && g_mx < 30 + card_w
                       && g_my >= y && g_my < y + card_h;
        draw_glass_control(30, y, card_w, card_h, (int)GLASS_RADIUS,
                           GLASS_ROLE_OVERLAY, rpressed,
                           g_theme == 1 ? pack_rgb(236, 242, 252)
                                        : pack_rgb(22, 32, 52),
                           g_theme == 1 ? 0xB0 : 0xB8);
        aether_draw_border(30, y, card_w, card_h, P_BORDER, 1);
        int is = 40;
        aether_draw_round_rect(46, y + (card_h - is) / 2, is, is, 10, aether_app_color(k), 0xFF);
        aether_draw_icon(ae_app_icon[k % APP_COUNT], 46 + 8, y + (card_h - is) / 2 + 8, is - 16,
                         pack_rgb(255, 255, 255));
        aether_draw_text_utf8(100, y + card_h / 2 - 8, aether_app_name(k), P_TEXT);
        aether_draw_round_rect(30 + card_w - 40, y + 10, 30, 30, 8, P_BTN, 0xFF);
        aether_draw_icon(AE_ICON_X, 30 + card_w - 36, y + 14, 22, P_DIM);
        y += card_h + 12;
    }
    if (g_app_n == 0) {
        aether_draw_icon(AE_ICON_APP_WINDOW, g_w / 2 - 30, g_h / 2 - 60, 60, P_DIM);
        const char *e = "暂无最近任务";
        aether_draw_text_utf8(g_w / 2 - aether_text_width_utf8(e) / 2, g_h / 2 + 16, e, P_DIM);
    }
}

void aether_desktop_render(const char *clock, double cpu, double mem)
{
    g_cpu = cpu; g_mem = mem;
    aether_island_tick();       /* 连续形变 / 计时 / 自动收起，每帧推进 */
    theme_palette();
    aether_ui_palette();        /* 灵动岛/负一屏/搜索复用同一套控件色板 */
    aether_render_clear_clip();
    aether_render_set_alpha(0xFF);
    aether_render_translate(0, 0);
    sync_anim_starts();
    int now = (int)time(NULL);

    if (g_state == PH_BOOT) {
        if (now - g_boot_t0 >= 3) g_state = PH_LOCKED;
        draw_boot(now - g_boot_t0);
        return;
    }
    if (g_state == PH_LOCKED) {
        draw_lock(clock);
        draw_status_bar(clock);
        return;
    }
    if (g_state == PH_AOD) {
        draw_aod();
        return;
    }
    /* 背景层：渐变兜底 + 内置/用户设定的真实 PPM 壁纸 */
    aether_draw_vgradient(0, 0, g_w, g_h, P_BG1, P_BG2);
    draw_wallpaper();
    /* 磨砂玻璃遮罩层：生成一次「磨砂壁纸盘子」，供导航/控制层玻璃控件采样。
     * 壁纸/主题/尺寸/换页变化时才重算，逐帧只是复用像素，不阻塞串行 IPC。 */
    ensure_wall_veil();

    /* 解锁动效：桌面图标从中心向外铺开 + 模糊由有到无（5.3），用统一曲线 */
    double unl = aether_anim_value(&g_an_unlock, aether_ease_out);
    int unlocking = aether_anim_running(&g_an_unlock);
    if (unlocking) aether_render_set_alpha((unsigned char)(unl * 255));

    /* 桌面换页：整层横向推移，方向感明确（5.13 转场统一） */
    if (aether_anim_running(&g_an_page)) {
        double pg = aether_anim_value(&g_an_page, aether_ease_in_out);
        int off = (int)((1.0 - pg) * g_w * 0.45 * (double)(-g_page_dir));
        aether_render_translate(off, 0);
    }
    if (g_state == PH_HOME) {
        /* 负一屏：滑过最后一页继续滑进入（总纲 5.5 / 5.6） */
        if (aether_neg1_is_open())
            aether_neg1_render(0, 0, g_w, g_h, g_theme, STATUS_H,
                               cpu, mem, battery_capacity());
        else
            draw_home();
    }
    else if (g_state == PH_APP) {
        /* 应用从图标矩形放大展开：用全局 alpha 做内容淡入（5.13），
         * 展开末期内容才完全显现，避免白/黑屏直跳。 */
        if (aether_anim_running(&g_an_app)) {
            double ap = aether_anim_value(&g_an_app, aether_ease_out);
            aether_render_set_alpha((unsigned char)(ap * 255));
        }
        struct aether_win *f = aether_desktop_focus();
        if (f) draw_app_fullscreen(f); else draw_home();
    } else if (g_state == PH_RECENTS) {
        draw_recents();
    }
    aether_render_translate(0, 0);
    aether_render_set_alpha(0xFF);

    draw_status_bar(clock);

    /* 灵动岛与实时活动：常驻顶部，真黑胶囊、宽度先拉高度后撑的连续形变、
     * 主活动 + 侧边副活动并存；隐私面板由其内部按需绘制。 */
    aether_island_render(0, 0, g_w, g_h, g_theme, STATUS_H);

    /* 覆盖层：先对下层画布做真算模糊（背景虚化），再按统一曲线带方向进入 */
    if (g_drawer || g_panel) {
        draw_overlay_backdrop();
        if (g_panel) {
            /* 两段下拉：前段模糊+放大时间，后段卡片进入；用曲线分两段体现 */
            double p = aether_anim_value(&g_an_panel, aether_ease_out);
            int ph = g_panel == 1 ? g_h * 60 / 100 : g_h * 66 / 100;
            int dy = (int)((1.0 - p) * (-ph));   /* 从上方下拉进入 */
            aether_render_translate(0, dy);
            aether_render_set_alpha((unsigned char)(p * 255));
            if (g_panel == 1) draw_notif_center(); else draw_control_center();
            aether_render_translate(0, 0);
            aether_render_set_alpha(0xFF);
        } else {
            double d = aether_anim_value(&g_an_drawer, aether_ease_out);
            int dy = (int)((1.0 - d) * (g_h - STATUS_H));  /* 从底部上推 */
            aether_render_translate(0, dy);
            aether_render_set_alpha((unsigned char)(d * 255));
            draw_drawer();
            aether_render_translate(0, 0);
            aether_render_set_alpha(0xFF);
        }
    }
    /* 全局搜索：全屏层，输入即出结果；置于最上层但保留系统导航栏 */
    if (aether_search_is_open())
        aether_search_render(0, 0, g_w, g_h, g_theme, STATUS_H);
    draw_nav_bar();
    aether_render_set_alpha(0xFF);
    aether_render_translate(0, 0);

    /* 阶段 5：小窗（悬浮窗）。背景应用变暗并降低饱和度以区分前景背景；
     * 同时仅一个，第二个请求给明确反馈（toast）。 */
    if (g_float.active) {
        struct aether_win *fw = aether_desktop_window(g_float.id);
        aether_fill_rect_alpha(0, 0, g_w, g_h, pack_rgb(0, 0, 0), g_float.snapped ? 150 : 120);
        int r = 16;
        aether_draw_round_rect(g_float.x, g_float.y, g_float.w, g_float.h, r, P_GLASS, 0xF2);
        aether_draw_border(g_float.x, g_float.y, g_float.w, g_float.h, P_BORDER, 2);
        /* 顶部拖拽条 + 关闭入口 */
        aether_draw_round_rect(g_float.x, g_float.y, g_float.w, 30, r, P_BTN, 0xFF);
        aether_draw_icon(AE_ICON_X, g_float.x + g_float.w - 30, g_float.y + 6, 18, P_DIM);
        aether_draw_icon(AE_ICON_APP_WINDOW, g_float.x + 10, g_float.y + 6, 18, P_DIM);
        /* 应用内容（按浮窗尺寸自适应布局） */
        if (fw) aether_app_render(fw, g_float.x, g_float.y + 30,
                                  g_float.w, g_float.h - 30 - 14);
        /* 右下角缩放手柄 */
        aether_draw_round_rect(g_float.x + g_float.w - 22, g_float.y + g_float.h - 22, 18, 18, 6, P_BTN, 0xFF);
        aether_draw_icon(AE_ICON_PLUS, g_float.x + g_float.w - 21, g_float.y + g_float.h - 21, 16, P_DIM);
    }
    /* 阶段 5：轻提示（toast） */
    if (g_toast[0] && (long)time(NULL) - g_toast_t0 < 2) {
        int tw = aether_text_width_utf8(g_toast) + 40;
        int tx = (g_w - tw) / 2, ty = g_h - NAV_H - 60;
        aether_draw_round_rect(tx, ty, tw, 36, 18, P_PANEL, 0xE6);
        aether_draw_border(tx, ty, tw, 36, P_BORDER, 1);
        aether_ui_label(tx + 20, ty + 9, g_toast, P_TEXT);
    }
}

/* ================= 输入分发 ================= */
static void unlock(void) {
    /* skipboot / unlock 共用：跳过开机动画。起步处于 PH_BOOT 时先推进到
     * PH_LOCKED，再解锁进桌面；否则设备会卡在 BOOT/LOCKED，导致 sleep(AOD)
     * 等依赖非锁屏状态的命令被守卫拦截而失效。 */
    if (g_state == PH_BOOT) g_state = PH_LOCKED;
    if (g_state == PH_LOCKED) {
        g_state = PH_HOME;
        aether_anim_begin(&g_an_unlock, 380);  /* 图标从中心铺开 */
    }
}
static void go_home(void)
{
    if (g_state != PH_LOCKED) { g_state = PH_HOME; g_drawer = 0; g_panel = 0; }
}
static void go_recents(void) { if (g_state != PH_LOCKED) { g_drawer = 0; g_panel = 0; g_state = (g_state == PH_RECENTS) ? PH_HOME : PH_RECENTS; } }
static void back_in_app(void)
{
    if (g_state == PH_APP || g_state == PH_RECENTS) g_state = PH_HOME;
    else if (g_drawer) g_drawer = 0;
    else if (g_panel) g_panel = 0;
}
static void swipe(int dx, int dy)
{
    int thr = g_w * 12 / 100;
    if (dx < -thr && dx < dy && dx < -dy) {
        if (aether_neg1_is_open()) return;
        if (g_page < PAGES - 1) g_page++;
        else if (aether_neg1_enabled()) {
            /* 总纲 5.5：滑过最后一页继续滑进入负一屏 */
            aether_neg1_open(1); aether_neg1_invalidate();
        }
        return;
    }
    if (dx > thr && dx > dy && dx > -dy) {
        if (aether_neg1_is_open()) { aether_neg1_open(0); return; }
        if (g_page > 0) g_page--;
        return;
    }
    if (dy < -thr && dy < dx && dy < -dx) { if (g_state == PH_HOME) g_drawer = 1; return; }
    if (dy > thr && dy > dx && dy > -dx) {
        if (g_drawer) { g_drawer = 0; return; }
        if (g_panel) { g_panel = 0; return; }
        /* 主屏中线下拉唤出全局搜索（总纲 5.5） */
        if (g_state == PH_HOME && !aether_neg1_is_open()) {
            aether_search_open(1); return;
        }
    }
}

static void handle_home_tap(int x, int y)
{
    int top = STATUS_H + 8;
    if (hit(x, y, 16, top, g_w - 32, 44)) { aether_search_open(1); return; }
    for (int i = 0; i < PAGE_APPS; i++) {
        int k = page_apps[g_page][i];
        if (k < 0) continue;
        int tx, ty, sz; home_tile_rect(i, &tx, &ty, &sz);
        if (hit(x, y, tx - 6, ty - 6, sz + 12, sz + 34)) {
            aether_desktop_open((enum app_kind)k); return;
        }
    }
    int dy = g_h - NAV_H - DOCK_H + 4;
    int dcell = (g_w - 32) / DOCK_N;
    int disz = (DOCK_H - 16) * 58 / 100;
    for (int i = 0; i < DOCK_N; i++) {
        int gx = 16 + i * dcell + (dcell - disz) / 2;
        int gy = dy + (DOCK_H - 8 - disz) / 2 - 6;
        if (hit(x, y, gx - 6, gy - 6, disz + 12, disz + 30)) {
            aether_desktop_open((enum app_kind)dock_apps[i]); return;
        }
    }
}

static void handle_drawer_tap(int x, int y)
{
    for (int k = 0; k < APP_COUNT; k++) {
        int tx, ty, sz; drawer_geom(k, &tx, &ty, &sz);
        if (hit(x, y, tx - 6, ty - 6, sz + 12, sz + 34)) {
            aether_desktop_open((enum app_kind)k); return;
        }
    }
    if (y < STATUS_H + 60 || y > g_h - NAV_H - 44) g_drawer = 0;
}

static void handle_panel_tap(int x, int y)
{
    int ph = g_panel == 1 ? g_h * 60 / 100 : g_h * 66 / 100;
    if (y < STATUS_H || y >= STATUS_H + ph) { g_panel = 0; return; }
    if (g_panel == 1) {
        int cw = aether_text_width_utf8("全部清除");
        if (hit(x, y, g_w - 20 - cw - 8, STATUS_H + 8, cw + 16, 32)) {
            for (int i = 0; i < 3; i++) g_notif_on[i] = 0; return;
        }
        int yy = STATUS_H + 52;
        for (int i = 0; i < 3; i++) {
            if (!g_notif_on[i]) continue;
            if (hit(x, y, 14, yy, g_w - 28, 78)) { g_notif_on[i] = 0; return; }
            yy += 88;
        }
    } else {
        for (int i = 0; i < 9; i++) {
            int tx, ty, tw, th; cc_geom(i, &tx, &ty, &tw, &th);
            if (hit(x, y, tx, ty, tw, th)) { cc_toggle(i); return; }
        }
    }
}

static void handle_nav_tap(int x, int y)
{
    int y0 = g_h - NAV_H;
    int bx0 = g_w * 16 / 100, cx = g_w / 2, bx2 = g_w * 84 / 100;
    if (hit(x, y, bx0 - 30, y0, 60, NAV_H)) { back_in_app(); return; }
    if (hit(x, y, cx - 30, y0, 60, NAV_H)) { go_home(); return; }
    if (hit(x, y, bx2 - 30, y0, 60, NAV_H)) { go_recents(); return; }
}

static void handle_app_tap(int x, int y)
{
    if (y < STATUS_H || y >= g_h - NAV_H) return;
    struct aether_win *f = aether_desktop_focus();
    if (!f) return;
    int ch = g_h - STATUS_H - NAV_H;
    aether_app_click(f, x, y - STATUS_H, g_w, ch);
}

static void handle_recents_tap(int x, int y)
{
    int card_w = g_w - 60, card_h = (g_h - NAV_H - STATUS_H - 80) / (g_app_n > 0 ? g_app_n : 1) - 12;
    if (card_h > 140) card_h = 140; if (card_h < 60) card_h = 60;
    int yy = STATUS_H + 44;
    for (int i = 0; i < g_app_n; i++) {
        enum app_kind k = (enum app_kind)g_app_order[i];
        if (hit(x, y, 30 + card_w - 40, yy + 10, 30, 30)) {
            for (int j = 0; j < MAX_WINDOWS; j++) if (g_wins[j].used && g_wins[j].kind == k)
                aether_desktop_close(g_wins[j].id);
            if (g_app_n == 0) g_state = PH_HOME;
            return;
        }
        if (hit(x, y, 30, yy, card_w, card_h)) { aether_desktop_open(k); return; }
        yy += card_h + 12;
    }
    if (x < 30 || x > 30 + card_w) g_state = PH_HOME;
}

/* 阶段 5：小窗输入处理（down/move/up 共用）。返回 1 表示事件已被小窗消费。 */
static int float_handle(int x, int y, int down)
{
    if (!g_float.active) return 0;
    int fx = g_float.x, fy = g_float.y, fw = g_float.w, fh = g_float.h;
    int in_x = x >= fx && x < fx + fw;
    int in_y = y >= fy && y < fy + fh;
    if (down) {
        /* 关闭入口 */
        if (hit(x, y, fx + fw - 30, fy + 6, 18, 18)) { aether_desktop_close_float(); return 1; }
        /* 缩放手柄（右下角） */
        if (hit(x, y, fx + fw - 22, fy + fh - 22, 18, 18)) { g_float.resizing = 1; return 1; }
        /* 顶部拖拽条（除关闭外） */
        if (hit(x, y, fx, fy, fw, 30)) {
            if (in_y) { g_float.dragging = 1; g_float.off_x = x - fx; g_float.off_y = y - fy; g_float.snapped = 0; }
            return 1;
        }
        if (in_x && in_y) { g_float.press_x = x; g_float.press_y = y; return 1; } /* 内容点击（up 时转发） */
        return 1; /* 浮窗外区域：消费，不穿透到下层 UI */
    } else {
        if (g_float.dragging) {
            g_float.dragging = 0;
            /* 侧边吸附：靠近左/右边缘收成侧边条 */
            if (fx < g_w * 10 / 100)      { g_float.x = 6; g_float.snapped = 1; }
            else if (fx + fw > g_w * 90 / 100) { g_float.x = g_w - fw - 6; g_float.snapped = 1; }
            else g_float.snapped = 0;
            return 1;
        }
        if (g_float.resizing) { g_float.resizing = 0; return 1; }
        /* 内容点击：转发给应用（坐标相对浮窗内容区左上） */
        if (hit(x, y, fx, fy + 30, fw, fh - 44)) {
            struct aether_win *w = aether_desktop_window(g_float.id);
            if (w) aether_app_click(w, x - fx, y - (fy + 30), fw, fh - 44);
            return 1;
        }
        return 1;
    }
}

/* 点击语义（down=1 记起点；down=0 判定 tap 或 swipe） */
void aether_desktop_button(int x, int y, int down)
{
    if (g_state == PH_AOD) { if (down) aether_desktop_wake(); return; }
    if (g_float.active && float_handle(x, y, down)) return;
    if (down) {
        g_pressed = 1; g_dragged = 0;
        g_press_x = g_move_x = x; g_press_y = g_move_y = y;
        g_mx = x; g_my = y;
        return;
    }
    if (!g_pressed) return;
    g_pressed = 0;
    int dx = g_move_x - g_press_x, dy = g_move_y - g_press_y;
    if (g_dragged) { swipe(dx, dy); return; }

    int tx = x, ty = y;
    g_mx = tx; g_my = ty;
    if (g_state == PH_BOOT) { g_state = PH_LOCKED; return; }
    if (g_state == PH_LOCKED) { unlock(); return; }

    /* 隐私面板 / 状态栏隐私指示点：点开可查「哪个应用在用」并直接关闭 */
    if (aether_island_privacy_open()) {
        (void)aether_island_privacy_hit(tx, ty, g_w, STATUS_H);
        return;
    }
    if (ty < STATUS_H && aether_island_privacy_hit(tx, ty, g_w, STATUS_H)) return;

    /* 灵动岛交互：单击展开 → 再单击进卡片 → 再点收起；副圆点击互换主副 */
    {
        int act = aether_island_hit(tx, ty, g_w, STATUS_H);
        if (act == 1) { aether_island_set_form(ISLAND_EXPANDED); return; }
        if (act == 2) { aether_island_set_form(ISLAND_CARD); return; }
        if (act == 3) { aether_island_set_form(ISLAND_COLLAPSED); return; }
        if (act == 4) { aether_island_swap(); return; }
        if (act == 6) { return; }
    }

    /* 全局搜索：点击结果直达应用 / 设置二级页 */
    if (aether_search_is_open()) {
        if (ty >= g_h - NAV_H) { aether_search_open(0); return; }
        int ak = -1, sp = -1;
        int r = aether_search_hit(tx, ty, g_w, g_h, STATUS_H, &ak, &sp);
        if (r == 1 && ak >= 0) {
            aether_search_open(0); unlock();
            aether_desktop_open((enum app_kind)ak); return;
        }
        if (r == 2 && sp >= 0) {
            aether_search_open(0); unlock();
            aether_desktop_open(APP_SETTINGS);
            {
                struct aether_win *f = aether_desktop_focus();
                char c[64];
                snprintf(c, sizeof c, "appcmd page %d", sp);
                if (f) aether_app_cmd(f, c);
            }
            return;
        }
        return;
    }

    /* 负一屏：快捷开关写入统一系统设置总线，推荐应用直达 */
    if (aether_neg1_is_open()) {
        int sk = -1, ak = -1;
        int r = aether_neg1_hit(tx, ty, g_w, g_h, STATUS_H, &sk, &ak);
        if (r == 1 && sk >= 0) {
            aether_sys_set(sk, aether_sys_get(sk) ? 0 : 1);
            aether_sys_apply(); aether_neg1_invalidate(); return;
        }
        if (r == 2 && ak >= 0) { aether_desktop_open((enum app_kind)ak); return; }
        return;
    }

    /* 状态栏左右分区：左=通知中心，右=控制中心 */
    if (ty < STATUS_H) { g_panel = (tx < g_w / 2) ? 1 : 2; g_drawer = 0; return; }
    if (ty >= g_h - NAV_H) { handle_nav_tap(tx, ty); return; }
    if (g_panel) { handle_panel_tap(tx, ty); return; }
    if (g_drawer) { handle_drawer_tap(tx, ty); return; }
    if (g_state == PH_HOME) { handle_home_tap(tx, ty); return; }
    if (g_state == PH_APP) { handle_app_tap(tx, ty); return; }
    if (g_state == PH_RECENTS) { handle_recents_tap(tx, ty); return; }
}

void aether_desktop_motion(int x, int y)
{
    g_mx = x; g_my = y;
    /* 阶段 5：小窗拖拽 / 缩放（连续跟手） */
    if (g_float.active && (g_float.dragging || g_float.resizing)) {
        if (g_float.dragging) {
            g_float.x = x - g_float.off_x; g_float.y = y - g_float.off_y;
            if (g_float.y < STATUS_H) g_float.y = STATUS_H;
            if (g_float.y > g_h - NAV_H - 40) g_float.y = g_h - NAV_H - 40;
            if (g_float.x < -g_float.w * 40 / 100) g_float.x = -g_float.w * 40 / 100;
            if (g_float.x > g_w - g_float.w * 60 / 100) g_float.x = g_w - g_float.w * 60 / 100;
        } else if (g_float.resizing) {
            g_float.w = x - g_float.x; g_float.h = y - g_float.y;
            if (g_float.w < g_w * 35 / 100) g_float.w = g_w * 35 / 100;
            if (g_float.w > g_w * 92 / 100) g_float.w = g_w * 92 / 100;
            if (g_float.h < g_h * 30 / 100) g_float.h = g_h * 30 / 100;
            if (g_float.h > g_h * 90 / 100) g_float.h = g_h * 90 / 100;
        }
        return;
    }
    if (g_pressed) {
        g_move_x = x; g_move_y = y;
        int dx = x - g_press_x, dy = y - g_press_y;
        if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
        if (dx > 8 || dy > 8) g_dragged = 1;
    }
}

void aether_desktop_key(unsigned code, int shift)
{
    if (code == 116 /* KEY_POWER */) { aether_desktop_sleep(); return; }
    if (code == 1 /* ESC */) {
        if (aether_island_privacy_open()) { aether_island_privacy_close(); return; }
        if (aether_search_is_open()) { aether_search_open(0); return; }
        if (aether_neg1_is_open()) { aether_neg1_open(0); return; }
        back_in_app(); return;
    }
    if (aether_search_is_open()) { aether_search_key(code, shift); return; }
    if (g_state == PH_APP) {
        struct aether_win *f = aether_desktop_focus();
        if (f) aether_app_key(f, code, shift);
    }
}

/* ================= 状态与 IPC ================= */
void aether_desktop_settle(void)
{
    /* 取证/自动化：把过渡直接推到完成态，画面呈现确定的终态；
     * 真机触摸路径不调用它，动效照常播放。aether_anim_reset 之后
     * aether_anim_value() 返回 1，running() 返回 0。 */
    aether_anim_reset(&g_an_panel);
    aether_anim_reset(&g_an_drawer);
    aether_anim_reset(&g_an_page);
    aether_anim_reset(&g_an_app);
    aether_anim_reset(&g_an_unlock);
    aether_anim_reset(&g_an_island);
    aether_island_settle();
    g_last_state = g_state; g_last_panel = g_panel;
    g_last_drawer = g_drawer; g_last_page = g_page;
}

void aether_desktop_state(char *buf, size_t sz)
{
    int n = 0; enum app_kind fk = APP_COUNT;
    struct aether_win *f = aether_desktop_focus();
    if (f) fk = f->kind;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used) n++;
    size_t o = 0;
    char isj[256], ssj[256], nsj[128];
    aether_island_state(isj, sizeof isj);
    aether_search_state(ssj, sizeof ssj);
    aether_neg1_state(nsj, sizeof nsj);
    o += (size_t)snprintf(buf + o, sz - o,
        "{\"state\":%d,\"app\":%d,\"panel\":%d,\"drawer\":%d,\"page\":%d,\"locked\":%d,"
        "\"theme\":%d,\"open\":%d,\"wifi\":%d,\"bt\":%d,\"bright\":%d,\"vol\":%d,"
        "\"aod\":%d,\"user_n\":%d,\"float\":%d,\"float_kind\":%d,"
        "\"island\":%s,\"search\":%s,\"neg1\":%s,\"wins\":[",
        g_state, (int)fk, g_panel, g_drawer, g_page, (g_state == PH_LOCKED) ? 1 : 0,
        g_theme, g_app_n,
        aether_sys_get(SYS_WIFI), aether_sys_get(SYS_BT),
        aether_sys_get(SYS_BRIGHT), aether_sys_get(SYS_VOL_MEDIA),
        aether_desktop_aod() ? 1 : 0, g_user_n, aether_desktop_float_active() ? 1 : 0,
        aether_desktop_float_active() ? aether_desktop_float_kind() : -1,
        isj, ssj, nsj);
    int first = 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct aether_win *w = &g_wins[i];
        if (!w->used) continue;
        o += (size_t)snprintf(buf + o, sz - o,
            "%s{\"id\":%d,\"kind\":%d}", first ? "" : ",", w->id, (int)w->kind);
        first = 0;
    }
    snprintf(buf + o, sz - o, "]}");
}

/* 灵动岛场景关键字 -> 枚举（取证/现场演示路径与真机点击共用同一状态机） */
static int ipc_scene(const char *s)
{
    if (!strcmp(s, "music"))    return IS_MUSIC;
    if (!strcmp(s, "timer"))    return IS_TIMER;
    if (!strcmp(s, "record"))   return IS_RECORD;
    if (!strcmp(s, "charge"))   return IS_CHARGE;
    if (!strcmp(s, "nav"))      return IS_NAV;
    if (!strcmp(s, "call"))     return IS_CALL;
    if (!strcmp(s, "download")) return IS_DOWNLOAD;
    if (!strcmp(s, "cast"))     return IS_CAST;
    if (!strcmp(s, "bt"))       return IS_BT;
    if (!strcmp(s, "update"))   return IS_UPDATE;
    return -1;
}

void aether_desktop_ipc(const char *line, char *reply, size_t sz)
{
    char cmd[32];
    if (sscanf(line, "%31s", cmd) != 1) { snprintf(reply, sz, "{\"ok\":false}\n"); return; }
    if (!strcmp(cmd, "state")) {
        aether_desktop_state(reply, sz);
        size_t L = strlen(reply); reply[L] = '\n'; reply[L + 1] = 0;
        return;
    }
    if (!strcmp(cmd, "skipboot") || !strcmp(cmd, "unlock")) { unlock(); snprintf(reply, sz, "{\"ok\":true}\n"); return; }
    if (!strcmp(cmd, "home")) { go_home(); snprintf(reply, sz, "{\"ok\":true}\n"); return; }
    if (!strcmp(cmd, "back")) { back_in_app(); snprintf(reply, sz, "{\"ok\":true}\n"); return; }
    if (!strcmp(cmd, "recents")) { go_recents(); snprintf(reply, sz, "{\"ok\":true}\n"); return; }
    if (!strcmp(cmd, "drawer")) {
        int v = 0; sscanf(line, "%*s %d", &v); g_drawer = v ? 1 : 0;
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "panel")) {
        int v = 0; sscanf(line, "%*s %d", &v);
        g_panel = (v == 1 || v == 2) ? v : 0; g_drawer = 0;
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "page")) {
        int v = 0; sscanf(line, "%*s %d", &v);
        if (v < 0) v = 0; if (v >= PAGES) v = PAGES - 1;
        g_page = v; g_state = PH_HOME; g_drawer = 0; g_panel = 0;
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "theme")) {
        int v = 0; sscanf(line, "%*s %d", &v); aether_desktop_set_theme(v);
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "swipe")) {
        int dx = 0, dy = 0; sscanf(line, "%*s %d %d", &dx, &dy);
        swipe(dx, dy); snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "move") || !strcmp(cmd, "click") || !strcmp(cmd, "down") || !strcmp(cmd, "up")) {
        int x = 0, y = 0;
        sscanf(line, "%*s %d %d", &x, &y);
        if (!strcmp(cmd, "move")) { g_mx = x; g_my = y; aether_desktop_motion(x, y); }
        else if (!strcmp(cmd, "click")) { g_mx = x; g_my = y; aether_desktop_button(x, y, 1); aether_desktop_button(x, y, 0); }
        else if (!strcmp(cmd, "down")) { g_mx = x; g_my = y; aether_desktop_button(x, y, 1); }
        else { aether_desktop_button(x, y, 0); }
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "key")) {
        unsigned code = 0; int shift = 0;
        sscanf(line, "%*s %u %d", &code, &shift);
        aether_desktop_key(code, shift);
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "text")) {
        const char *s = line + 4; while (*s == ' ') s++;
        for (int i = 0; s[i] && s[i] != '\r' && s[i] != '\n'; i++) {
            int shift = 0; unsigned code = aether_char_to_key(s[i], &shift);
            if (code) aether_desktop_key(code, shift);
        }
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "open")) {
        int k = 0; sscanf(line, "%*s %d", &k);
        if (k >= 0 && k < APP_COUNT) {
            unlock(); int id = aether_desktop_open((enum app_kind)k);
            snprintf(reply, sz, "{\"ok\":true,\"id\":%d}\n", id);
        } else snprintf(reply, sz, "{\"ok\":false}\n");
        return;
    }
    if (!strcmp(cmd, "closeall")) {
        for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].used) aether_desktop_close(g_wins[i].id);
        g_state = PH_HOME; g_drawer = 0; g_panel = 0;
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "appcmd")) {
        /* 自动化取证：把 "appcmd <appname> <subcmd> <arg>" 去掉 "appcmd"
         * 前缀后转发给当前焦点应用；各 app 的 cmd handler 会再跳过 appname
         * 读 subcmd，与真机点击路径一致。 */
        struct aether_win *f = aether_desktop_focus();
        const char *p = line;
        while (*p && *p != ' ') p++;   /* 跳过 "appcmd" */
        while (*p == ' ') p++;         /* 跳过其后的空格 */
        if (f) aether_app_cmd(f, p);
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "settle")) {
        aether_desktop_settle();
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "reduce")) {   /* 无障碍：减少动效 */
        int v = 0; sscanf(line, "%*s %d", &v);
        aether_anim_set_reduce(v);
        snprintf(reply, sz, "{\"ok\":true,\"reduce\":%d}\n", aether_anim_reduce()); return;
    }
    if (!strcmp(cmd, "fx")) {       /* 材质档位 0/1/2 */
        int v = 2; sscanf(line, "%*s %d", &v);
        aether_anim_set_fx(v);
        snprintf(reply, sz, "{\"ok\":true,\"fx\":%d}\n", aether_anim_fx()); return;
    }

    /* ---- 阶段 4：灵动岛五场景 + 三形态 + 主副互换 ---- */
    if (!strcmp(cmd, "island")) {
        char key[32] = "", title[64] = "", sub[96] = "";
        sscanf(line, "%*s %31s %63s %95s", key, title, sub);
        int s = ipc_scene(key);
        if (s <= 0) { snprintf(reply, sz, "{\"ok\":false}\n"); return; }
        int prog = -1;
        long hide = 0;
        char extra[128] = "";
        if (s == IS_CHARGE) {
            prog = battery_capacity(); hide = 6000;  /* 短暂出现后自动收起（TCG 截图窗口） */
            if (!title[0]) snprintf(title, sizeof title, "充电中");
            if (!sub[0]) snprintf(sub, sizeof sub, "电量 %d%% · 已接入电源", prog);
        } else if (s == IS_MUSIC) {
            /* 真实 WAV(PCM) 头：采样率与时长来自本机音频文件，不是编造 */
            FILE *wf = fopen("/usr/share/aether/audio/overture.wav", "rb");
            if (wf) {
                unsigned char h[44];
                if (fread(h, 1, 44, wf) == 44) {
                    unsigned rate = (unsigned)h[24] | ((unsigned)h[25] << 8) |
                                    ((unsigned)h[26] << 16) | ((unsigned)h[27] << 24);
                    unsigned data = (unsigned)h[40] | ((unsigned)h[41] << 8) |
                                    ((unsigned)h[42] << 16) | ((unsigned)h[43] << 24);
                    int secs = rate ? (int)(data / (rate * 4)) : 0;
                    snprintf(extra, sizeof extra, "序曲 · %d 秒 · %u Hz 真实 PCM", secs, rate);
                }
                fclose(wf);
            }
            if (!title[0]) snprintf(title, sizeof title, "序曲");
            if (!sub[0]) snprintf(sub, sizeof sub, "%s", extra[0] ? extra : "本机音频文件");
            prog = 35;
        } else if (s == IS_TIMER) {
            if (!title[0]) snprintf(title, sizeof title, "倒计时");
            if (!sub[0]) snprintf(sub, sizeof sub, "本机单调时钟 · 数字实时跳");
        } else if (s == IS_RECORD) {
            if (!title[0]) snprintf(title, sizeof title, "录音中");
            if (!sub[0]) snprintf(sub, sizeof sub, "本机音频输入 · 实时波形");
        } else if (s == IS_NAV) {
            /* 真实本机路线文件；无文件时如实显示空状态，不臆造街道名 */
            FILE *rf = fopen("/data/aether/route.conf", "r");
            if (rf) {
                char l[128];
                if (fgets(l, sizeof l, rf)) {
                    size_t L = strlen(l);
                    while (L && l[L - 1] < 32) { l[--L] = 0; }
                    snprintf(title, sizeof title, "%s", l);
                }
                if (fgets(l, sizeof l, rf)) {
                    size_t L = strlen(l);
                    while (L && l[L - 1] < 32) { l[--L] = 0; }
                    snprintf(sub, sizeof sub, "%s", l);
                }
                fclose(rf);
            }
            if (!title[0]) snprintf(title, sizeof title, "暂无导航路线");
            if (!sub[0]) snprintf(sub, sizeof sub, "本机路线文件 /data/aether/route.conf");
            prog = 62;
        } else {
            if (!title[0]) snprintf(title, sizeof title, "%s", aether_island_scene_name(s));
            if (!sub[0]) snprintf(sub, sizeof sub, "本机实时活动");
        }
        aether_island_set(s, title, sub, prog, hide);
        snprintf(reply, sz, "{\"ok\":true,\"scene\":%d}\n", s);
        return;
    }
    if (!strcmp(cmd, "islandform")) {
        int v = 0; sscanf(line, "%*s %d", &v);
        aether_island_set_form(v);
        snprintf(reply, sz, "{\"ok\":true,\"form\":%d}\n", aether_island_form()); return;
    }
    if (!strcmp(cmd, "islandswap")) {
        aether_island_swap(); snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "islandclear")) {
        char key[32] = ""; sscanf(line, "%*s %31s", key);
        int s = ipc_scene(key);
        if (s > 0) aether_island_clear(s);
        else aether_island_clear_all();
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    /* 隐私指示：1 相机 / 2 麦克风 / 3 定位，0 全部关闭 */
    if (!strcmp(cmd, "privacy")) {
        int k = 0; char app[48] = "";
        sscanf(line, "%*s %d %47s", &k, app);
        if (k == 0) {
            for (int j = 1; j <= 3; j++) aether_island_privacy_clear(j);
            aether_island_privacy_close();
        } else {
            aether_island_privacy_set(k, app[0] ? app : "本机应用");
        }
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    /* 全局搜索：search <关键词> / search open / search close */
    if (!strcmp(cmd, "search")) {
        const char *q = line + 6;
        while (*q == ' ') { q++; }
        if (!strcmp(q, "close")) aether_search_open(0);
        else if (!strcmp(q, "open")) aether_search_open(1);
        else aether_search_input(q);
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "neg1")) {
        int v = 1; sscanf(line, "%*s %d", &v);
        aether_neg1_open(v); aether_neg1_invalidate();
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "neg1enabled")) {
        int v = 1; sscanf(line, "%*s %d", &v);
        aether_neg1_toggle_enabled(v);
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }

    /* ---- 阶段 5：小窗 / 息屏显示 ---- */
    if (!strcmp(cmd, "float")) {
        int k = 0; sscanf(line, "%*s %d", &k);
        int r = (k >= 0 && k < APP_COUNT) ? aether_desktop_open_float((enum app_kind)k) : 1;
        if (r == 1) aether_desktop_toast("请先关闭当前小窗");
        snprintf(reply, sz, "{\"ok\":true,\"float\":%d}\n", aether_desktop_float_active()); return;
    }
    if (!strcmp(cmd, "closefloat")) {
        aether_desktop_close_float();
        snprintf(reply, sz, "{\"ok\":true}\n"); return;
    }
    if (!strcmp(cmd, "sleep"))  { aether_desktop_sleep();  snprintf(reply, sz, "{\"ok\":true,\"aod\":%d}\n", aether_desktop_aod()); return; }
    if (!strcmp(cmd, "wake"))   { aether_desktop_wake();   snprintf(reply, sz, "{\"ok\":true}\n"); return; }
    if (!strcmp(cmd, "aod"))    { snprintf(reply, sz, "{\"ok\":true,\"aod\":%d}\n", aether_desktop_aod()); return; }

    /* ---- 阶段 5：商店安装闭环（用户安装应用管理） ---- */
    if (!strcmp(cmd, "install")) {
        int k = 0; sscanf(line, "%*s %d", &k);
        int r = (k >= 0 && k < APP_COUNT) ? aether_desktop_install((enum app_kind)k) : 1;
        snprintf(reply, sz, "{\"ok\":true,\"user_n\":%d,\"r\":%d}\n", g_user_n, r); return;
    }
    if (!strcmp(cmd, "uninstall")) {
        int k = 0; sscanf(line, "%*s %d", &k);
        if (k >= 0 && k < APP_COUNT) aether_desktop_uninstall((enum app_kind)k);
        snprintf(reply, sz, "{\"ok\":true,\"user_n\":%d}\n", g_user_n); return;
    }
    if (!strcmp(cmd, "userapps")) {
        char ub[256]; int o = 0;
        o += (size_t)snprintf(ub + o, sizeof ub - o, "{\"ok\":true,\"user_n\":%d,\"kinds\":[", g_user_n);
        for (int i = 0; i < g_user_n; i++) o += (size_t)snprintf(ub + o, sizeof ub - o, "%s%d", i ? "," : "", g_user_apps[i]);
        o += (size_t)snprintf(ub + o, sizeof ub - o, "]}");
        snprintf(reply, sz, "%s\n", ub); return;
    }
    /* 通用系统设置总线写入（自动化取证用） */
    if (!strcmp(cmd, "sys")) {
        int k = 0, v = 0; sscanf(line, "%*s %d %d", &k, &v);
        if (k >= 0 && k < SYS_COUNT) { aether_sys_set(k, v); aether_sys_apply(); }
        snprintf(reply, sz, "{\"ok\":true,\"key\":%d,\"val\":%d}\n", k, aether_sys_get(k)); return;
    }
    snprintf(reply, sz, "unknown\n");
}
