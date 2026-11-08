/*
 * aether-appicons.h - 内置应用 -> 开源图标 ID 映射
 *
 * 全部取自 Lucide 同一套描边体系（见 aether-icons.h），系统只做统一处理。
 * 与 aether-desktop.h 的 enum app_kind 顺序严格一一对应。
 */
#ifndef AETHER_APPICONS_H
#define AETHER_APPICONS_H

#include "aether-icons.h"
#include "aether-desktop.h"

/* 与 enum app_kind 顺序严格一致。全部取自 Lucide 同一套描边体系，
 * 系统只做统一处理（超椭圆玻璃底 + 单色线性符号），无手绘、无自造。 */
static const int ae_app_icon[APP_COUNT] = {
    AE_ICON_FOLDER,        /* APP_FILES     文件管理器 */
    AE_ICON_TERMINAL,      /* APP_TERMINAL  终端 */
    AE_ICON_FILE_TEXT,     /* APP_EDITOR    文本编辑器 */
    AE_ICON_CALCULATOR,    /* APP_CALC      计算器 */
    AE_ICON_ACTIVITY,      /* APP_MONITOR   系统监视器 */
    AE_ICON_SETTINGS,      /* APP_SETTINGS  系统设置 */
    AE_ICON_INFO,          /* APP_ABOUT     关于本机 */
    AE_ICON_LAYOUT_GRID,   /* APP_MODULES   系统模块清单 */
    AE_ICON_GLOBE,         /* APP_BROWSER   浏览器 */
    AE_ICON_IMAGE,         /* APP_GALLERY   图片查看器 */
    AE_ICON_VIDEO,         /* APP_VIDEO     视频播放器 */
    AE_ICON_MUSIC,         /* APP_MUSIC     音乐播放器 */
    AE_ICON_CLOCK,         /* APP_CLOCK     时钟 */
    AE_ICON_CALENDAR,      /* APP_CALENDAR  日历 */
    AE_ICON_DOWNLOAD,      /* APP_STORE     应用商店 */
    AE_ICON_TYPE           /* APP_NOTES     笔记 */
};

#endif /* AETHER_APPICONS_H */
