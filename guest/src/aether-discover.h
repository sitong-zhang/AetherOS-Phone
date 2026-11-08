/*
 * aether-discover.h - 全局搜索 / 负一屏（阶段 4）
 *
 * 对齐《总纲》5.5「全局搜索」：输入即出结果不按回车；结果分组为
 * 应用、文件、设置项、联系人、网页建议；每组有限条数并给「查看全部」；
 * 点击直达；无结果给空状态与建议，不许留白。
 * 对齐 5.6「负一屏」：背景为壁纸的加深模糊版；卡片纵向排列
 * 天气、日程待办、快捷功能、使用统计、推荐；可整体关闭。
 * 所有内容取自真实数据源（本机应用表、真实目录、设置一级结构、
 * 本地日程/联系人文件、/proc），不使用演示假数据。
 */
#ifndef AETHER_DISCOVER_H
#define AETHER_DISCOVER_H

#include <stddef.h>

/* ---- 全局搜索 ---- */
void aether_search_init(void);
void aether_search_open(int on);
int  aether_search_is_open(void);
void aether_search_input(const char *text);
void aether_search_key(unsigned code, int shift);   /* 真机键盘：输入即出结果 */
void aether_search_state(char *buf, size_t sz);
/* 返回 1=打开应用(app_kind) 2=打开设置页(set_page) 3=查看全部 0=无动作 */
int  aether_search_hit(int lx, int ly, int cw, int ch, int status_h,
                       int *app_kind, int *set_page);
void aether_search_render(int ox, int oy, int cw, int ch, int theme, int status_h);

/* ---- 负一屏 ---- */
void aether_neg1_open(int on);
int  aether_neg1_is_open(void);
void aether_neg1_toggle_enabled(int on);
int  aether_neg1_enabled(void);
void aether_neg1_state(char *buf, size_t sz);
/* 返回 1=切换开关(sys_key) 2=打开应用(app_kind) 0=无动作 */
int  aether_neg1_hit(int lx, int ly, int cw, int ch, int status_h,
                     int *sys_key, int *app_kind);
void aether_neg1_render(int ox, int oy, int cw, int ch, int theme, int status_h,
                        double cpu, double mem, int battery);
void aether_neg1_invalidate(void);     /* 壁纸/主题变化后重算加深模糊背景 */

#endif /* AETHER_DISCOVER_H */
