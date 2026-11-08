/*
 * aether-island.h - 灵动岛与实时活动 / 隐私指示（阶段 4）
 *
 * 开源对齐：形态与场景分层照搬 istornz/flutter_live_activities（MIT）的
 *   compactLeading / compactTrailing / minimal / ExpandedRegion(leading|center|trailing)；
 * 主副位、优先级排序、popsUp、自动收起照搬 agupta07505/SmartIsland（GPL-3.0）
 *   的 IslandEvent / IslandPriority / IslandMode；
 * 连续形变分段参数照搬 SmartIsland ExpandMotion：
 *   PILL_OUT_END_PROGRESS=0.25、CARD_IN 0.1->0.9、MIN_FADE_MS=50。
 * 优先级顺序以《AetherOS 总规划书》5.7 为准（通话>录音>导航>计时器>音乐>下载>投屏>蓝牙）。
 * 客机为纯 C 软件光栅化，不具备 Compose/SwiftUI/ActivityKit 运行时，故照搬的是
 * 状态机、层级规则与动效参数，而非整段编译上游源码。详见 OPENSOURCE-ATTRIBUTION.md。
 */
#ifndef AETHER_ISLAND_H
#define AETHER_ISLAND_H

#include <stddef.h>

/* 场景：顺序即《总纲》5.7 优先级从高到低 */
enum island_scene {
    IS_NONE = 0,
    IS_CALL,        /* 通话：联系人 + 时长 + 挂断 */
    IS_RECORD,      /* 录音：红点 + 时长 + 实时波形 */
    IS_NAV,         /* 导航：下一步转向 + 距离 + 街道名 */
    IS_TIMER,       /* 计时器 / 秒表：数字实时跳 */
    IS_MUSIC,       /* 音乐：波形 + 歌名 + 播放控制 */
    IS_DOWNLOAD,    /* 下载与安装进度 */
    IS_CAST,        /* 投屏或热点：图标 + 连接数 */
    IS_BT,          /* 蓝牙连接 */
    IS_CHARGE,      /* 充电：电量与闪电，出现后自动收起 */
    IS_UPDATE,      /* 系统更新进度 */
    IS_COUNT
};

/* 三种形态：收起 / 展开 / 卡片 */
enum island_form {
    ISLAND_COLLAPSED = 0,
    ISLAND_EXPANDED  = 1,
    ISLAND_CARD      = 2
};

/* 隐私指示：相机/麦克风=绿，定位=橙（与状态栏同色同义） */
enum privacy_kind {
    PRIV_NONE     = 0,
    PRIV_CAMERA   = 1,
    PRIV_MIC      = 2,
    PRIV_LOCATION = 3
};

void        aether_island_init(void);
void        aether_island_tick(void);
void        aether_island_set(int scene, const char *title, const char *sub,
                              int progress, long auto_hide_ms);
void        aether_island_clear(int scene);
void        aether_island_clear_all(void);
void        aether_island_set_form(int form);
void        aether_island_swap(void);
int         aether_island_form(void);
void        aether_island_settle(void);   /* 取证：把形变直接推到完成态 */
int         aether_island_live(void);
const char *aether_island_scene_name(int scene);

/* 绘制：ox/oy 为画布偏移，cw 为画布宽，status_h 为状态栏高 */
void        aether_island_render(int ox, int oy, int cw, int ch,
                                 int theme, int status_h);
/* 命中共 6 种动作：1 展开/收起 2 进应用 3 卡片 4 互换主副 5 下滑收起 6 卡片内按钮 */
int         aether_island_hit(int lx, int ly, int cw, int status_h);

/* 隐私指示：返回占用宽度；点击后弹面板显示「哪个应用在用」并可直接关闭 */
int         aether_island_privacy_render(int x, int y, int h, int theme);
int         aether_island_privacy_hit(int lx, int ly, int cw, int status_h);
void        aether_island_privacy_panel(int ox, int oy, int cw, int ch, int theme);
void        aether_island_privacy_set(int kind, const char *app);
void        aether_island_privacy_clear(int kind);
int         aether_island_privacy_open(void);
void        aether_island_privacy_close(void);

void        aether_island_state(char *buf, size_t sz);

#endif /* AETHER_ISLAND_H */
