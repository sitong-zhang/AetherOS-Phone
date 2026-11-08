/*
 * aether-desktop.h - AetherOS 桌面外壳（窗口管理 / 任务栏 / 开始菜单 / 输入分发）
 */
#ifndef AETHER_DESKTOP_H
#define AETHER_DESKTOP_H

#include <stddef.h>

enum app_kind {
    APP_FILES = 0,      /* 文件管理器 */
    APP_TERMINAL,       /* 终端 */
    APP_EDITOR,         /* 文本编辑器 */
    APP_CALC,           /* 计算器 */
    APP_MONITOR,        /* 系统监视器 */
    APP_SETTINGS,       /* 设置 */
    APP_ABOUT,          /* 关于本机 */
    APP_MODULES,        /* 系统模块清单 */
    /* 阶段 3：补齐总纲第四章十二「内置应用（最低配置）」12 项 */
    APP_BROWSER,        /* 浏览器 */
    APP_GALLERY,        /* 图片查看器 */
    APP_VIDEO,          /* 视频播放器 */
    APP_MUSIC,          /* 音乐播放器 */
    APP_CLOCK,          /* 时钟（闹钟·计时器·秒表） */
    APP_CALENDAR,       /* 日历 */
    /* 阶段 5：应用商店 + 安装闭环（总纲 5.16 / 模块十） */
    APP_STORE,          /* 应用商店 */
    APP_NOTES,          /* 笔记（商店可安装的新应用，文件真实存储） */
    APP_COUNT
};

/* ================= 统一系统设置总线 =================
 * 总纲 5.15「所有设置即时生效」：设置页与控制中心读写同一份状态，
 * 任何一侧改动另一侧立即反映；状态存于外壳进程内，跨窗口、跨页面保持。
 * 键为稳定的整数索引，新增键只许在末尾追加（既有基线只许做加法）。 */
enum aether_sys_key {
    SYS_WIFI = 0, SYS_BT, SYS_AIR, SYS_DND, SYS_TORCH, SYS_LOC, SYS_HOTSPOT, SYS_VPN,
    SYS_BRIGHT, SYS_VOL_MEDIA, SYS_VOL_RING, SYS_VOL_ALARM, SYS_VOL_CALL, SYS_RING_MODE,
    SYS_THEME, SYS_THEME_AUTO, SYS_FONT_SCALE, SYS_DISPLAY_SIZE, SYS_REFRESH, SYS_COLOR_MODE,
    SYS_AOD, SYS_AOD_CONTENT, SYS_PERF_MODE, SYS_CHARGE_OPT, SYS_NOTIF_ENABLE,
    SYS_AUTO_TIME, SYS_24H, SYS_LANG, SYS_IME, SYS_AUTO_BRIGHT,
    SYS_HIGH_CONTRAST, SYS_REDUCE_MOTION, SYS_REDUCE_TRANS, SYS_SCREEN_READER, SYS_COLOR_FIX,
    SYS_FX, SYS_USB_DEBUG, SYS_SHOW_FPS, SYS_ANIM_SCALE,
    SYS_MULTI_USER, SYS_GUEST, SYS_APP_LOCK, SYS_PRIV_SPACE, SYS_SIDELOAD, SYS_CLICK_SOUND,
    SYS_LOCK_METHOD,
    /* 阶段 3：17 个二级页所需的具名开关/选项槽（只做加法） */
    SYS_SYNC, SYS_MOBILE_DATA, SYS_PROXY, SYS_ALLOW_LOCK_NOTIF, SYS_NOTIF_HISTORY,
    SYS_PERM_CAM, SYS_PERM_MIC, SYS_PERM_LOC, SYS_PERM_STORAGE, SYS_PERM_CONTACTS,
    SYS_PERM_CLIP, SYS_PERM_NOTIF, SYS_PERM_ONETIME, SYS_CLIP_ALERT,
    SYS_PRIV_DASH, SYS_BACKUP_AUTO, SYS_UPDATE_AUTO, SYS_OEM_UNLOCK, SYS_STRICT_MODE,
    SYS_APP_CLONE, SYS_CHILD_MODE, SYS_FOCUS_MODE, SYS_GAME_MODE,
    SYS_COUNT
};

/* 外壳侧通知的真实状态（设置页与通知中心共用，避免两套状态） */
int  aether_desktop_notif_count(void);
void aether_desktop_clear_notifs(void);
/* 应用管理：强制停止全部后台应用（设置页与多任务「全部清理」共用） */
void aether_desktop_close_all(void);

int  aether_sys_get(int key);
void aether_sys_set(int key, int val);
/* 外壳把设置总线的副作用应用到渲染/动画运行时（主题、材质、减少动效） */
void aether_sys_apply(void);

#define MAX_WINDOWS 8

struct aether_win {
    int used;
    int id;
    enum app_kind kind;
    char title[64];
    int x, y, w, h;
    int z;              /* 层级，越大越靠上 */
    int minimized;
    int maximized;
    int rx, ry, rw, rh; /* 最大化前的几何 */
    int scroll;         /* 应用通用滚动位置 */
};

/* ---- 生命周期 ---- */
void aether_desktop_init(int w, int h);
void aether_desktop_render(const char *clock, double cpu, double mem);

/* 阶段 5：用户安装的应用（商店安装闭环）。内置 14 应用之外的可装卸条目。
 * 安装时写入 page_apps 的空槽（APP_EMPTY）；卸载时清空。 */
int  aether_desktop_user_count(void);
enum app_kind aether_desktop_user_kind(int i);
int  aether_desktop_install(enum app_kind kind);     /* 0=已存在/成功 1=槽位满 */
int  aether_desktop_uninstall(enum app_kind kind);   /* 0=已移除 */
int  aether_desktop_is_user_installed(enum app_kind kind);

/* 阶段 5：小窗（悬浮窗）。同时仅一个；第二请求返回 1（需先关闭）。 */
int  aether_desktop_open_float(enum app_kind kind);  /* 1=已有小窗，需先关闭 */
void aether_desktop_close_float(void);
int  aether_desktop_float_active(void);
int  aether_desktop_float_kind(void);

/* 阶段 5：息屏显示（AOD）。SYS_AOD 关闭时息屏为全黑。 */
void aether_desktop_sleep(void);
void aether_desktop_wake(void);
int  aether_desktop_aod(void);

/* ---- 输入：真机事件与 IPC 注入共用同一套 handler ---- */
void aether_desktop_motion(int x, int y);
void aether_desktop_button(int x, int y, int down);   /* down=1 按下, 0 释放 */
void aether_desktop_key(unsigned code, int shift);    /* code = Linux KEY_* 原始码 */

/* ---- 状态与自动化 ---- */
void aether_desktop_state(char *buf, size_t sz);
void aether_desktop_ipc(const char *line, char *reply, size_t sz);
/* 把所有进行中的过渡推进到完成态（截图/自动化取证用，保证确定性终态） */
void aether_desktop_settle(void);

/* ---- 供 apps 模块使用 ---- */
struct aether_win *aether_desktop_window(int id);
struct aether_win *aether_desktop_focus(void);
int  aether_desktop_open(enum app_kind kind);
void aether_desktop_close(int id);
void aether_desktop_invalidate(void);
int  aether_desktop_theme(void);
void aether_desktop_set_theme(int t);
int  aether_desktop_width(void);
int  aether_desktop_height(void);

/* 设置一级结构（总纲 5.15 固定 18 项）：供全局搜索等模块复用，避免重复定义 */
int         aether_settings_top_count(void);
const char *aether_settings_top_name(int i);

/* Linux KEY_* 原始码 -> 可打印 ASCII（返回 0 表示非可打印键）。
 * 真机键盘事件与 IPC 注入都经由它，保证两条路径完全一致。 */
char aether_key_to_char(unsigned code, int shift);
/* 反向：ASCII -> KEY_* 码，供 IPC text 命令逐字符注入 */
unsigned aether_char_to_key(char c, int *shift_out);

/* 窗口内通知气泡 */
void aether_desktop_notify(const char *text);

#endif /* AETHER_DESKTOP_H */
