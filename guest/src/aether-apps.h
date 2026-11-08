/*
 * aether-apps.h - AetherOS 内置应用：内容渲染与交互
 *
 * 所有应用直接读取真实数据源（/proc、opendir、popen 执行 BusyBox 命令），
 * 不使用任何硬编码假数据。
 */
#ifndef AETHER_APPS_H
#define AETHER_APPS_H

#include "aether-desktop.h"

const char *aether_app_name(enum app_kind kind);
const char *aether_app_short(enum app_kind kind);   /* 图标上的短标签 */
unsigned    aether_app_color(enum app_kind kind);  /* 磁贴颜色 */

void aether_app_open(struct aether_win *win);
void aether_app_close(struct aether_win *win);

/* ox/oy/cw/ch = 窗口内容区绝对原点与尺寸 */
void aether_app_render(struct aether_win *win, int ox, int oy, int cw, int ch);
/* lx/ly = 相对内容区左上的点击坐标 */
void aether_app_click(struct aether_win *win, int lx, int ly, int cw, int ch);
void aether_app_key(struct aether_win *win, unsigned code, int shift);
/* 自动化取证命令：与真机点击共用同一套 handler（切成指定视图 / 触发真实动作），
 * 仅由 IPC appcmd 转发，保证现场演示路径与自动化路径一致。 */
void aether_app_cmd(struct aether_win *win, const char *line);
/* 当前是否有应用需要连续帧（计时器/秒表/媒体播放中的数字与画面实时推进）。
 * 合成器据此把空闲 120ms 轮询降为 30ms，动画之外也能跟手。 */
int  aether_apps_live(void);

#endif /* AETHER_APPS_H */
