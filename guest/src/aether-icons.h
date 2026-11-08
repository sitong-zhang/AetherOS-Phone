/*
 * aether-icons.h - AetherOS 开源图标 API（自动生成，请勿手工编辑）
 *
 * 来源: Lucide (lucide-static v0.468.0), ISC License
 *       https://github.com/lucide-icons/lucide
 * 数据: aether-icons-data.h（仅由 aether-icons.c include 一次）
 * 图标数量: 76，全部同一开源库、同一描边体系。
 */
#ifndef AETHER_ICONS_H
#define AETHER_ICONS_H

#define AE_ICON_W 48
#define AE_ICON_H 48

enum aether_icon_id {
    AE_ICON_ACTIVITY = 0,
    AE_ICON_APP_WINDOW = 1,
    AE_ICON_BATTERY_CHARGING = 2,
    AE_ICON_BATTERY_FULL = 3,
    AE_ICON_BATTERY_LOW = 4,
    AE_ICON_BELL = 5,
    AE_ICON_BLUETOOTH = 6,
    AE_ICON_BOOKMARK = 7,
    AE_ICON_CALCULATOR = 8,
    AE_ICON_CALENDAR = 9,
    AE_ICON_CAMERA = 10,
    AE_ICON_CHECK = 11,
    AE_ICON_CHEVRON_LEFT = 12,
    AE_ICON_CHEVRON_RIGHT = 13,
    AE_ICON_CLIPBOARD = 14,
    AE_ICON_CLOCK = 15,
    AE_ICON_CLOUD = 16,
    AE_ICON_CPU = 17,
    AE_ICON_DOWNLOAD = 18,
    AE_ICON_DROPLET = 19,
    AE_ICON_EYE_OFF = 20,
    AE_ICON_EYE = 21,
    AE_ICON_FILE_TEXT = 22,
    AE_ICON_FLASHLIGHT = 23,
    AE_ICON_FOLDER_OPEN = 24,
    AE_ICON_FOLDER = 25,
    AE_ICON_GLOBE = 26,
    AE_ICON_GRID_3X3 = 27,
    AE_ICON_HARD_DRIVE = 28,
    AE_ICON_HOME = 29,
    AE_ICON_IMAGE = 30,
    AE_ICON_INFO = 31,
    AE_ICON_LAYOUT_GRID = 32,
    AE_ICON_LIST = 33,
    AE_ICON_LOCK = 34,
    AE_ICON_MAP_PIN = 35,
    AE_ICON_MAP = 36,
    AE_ICON_MESSAGE_CIRCLE = 37,
    AE_ICON_MIC = 38,
    AE_ICON_MINUS = 39,
    AE_ICON_MONITOR = 40,
    AE_ICON_MOON = 41,
    AE_ICON_MUSIC_2 = 42,
    AE_ICON_MUSIC = 43,
    AE_ICON_NAVIGATION = 44,
    AE_ICON_PALETTE = 45,
    AE_ICON_PHONE = 46,
    AE_ICON_PLAY = 47,
    AE_ICON_PLUS = 48,
    AE_ICON_POWER = 49,
    AE_ICON_REFRESH_CW = 50,
    AE_ICON_REPEAT = 51,
    AE_ICON_SAVE = 52,
    AE_ICON_SCAN_LINE = 53,
    AE_ICON_SCREENSHOT = 54,
    AE_ICON_SEARCH = 55,
    AE_ICON_SETTINGS = 56,
    AE_ICON_SHARE_2 = 57,
    AE_ICON_SHIELD = 58,
    AE_ICON_SIGNAL = 59,
    AE_ICON_SLIDERS = 60,
    AE_ICON_SUN = 61,
    AE_ICON_TERMINAL = 62,
    AE_ICON_TIMER = 63,
    AE_ICON_TRASH = 64,
    AE_ICON_TYPE = 65,
    AE_ICON_UNLOCK = 66,
    AE_ICON_UPLOAD = 67,
    AE_ICON_USER = 68,
    AE_ICON_USERS = 69,
    AE_ICON_VIDEO = 70,
    AE_ICON_VOLUME_2 = 71,
    AE_ICON_WIFI = 72,
    AE_ICON_WRENCH = 73,
    AE_ICON_X = 74,
    AE_ICON_ZAP = 75
};
#define AE_ICON_COUNT 76

/* 把 id 对应的线性符号以 fg 前景色绘制到 (x,y) 的 size x size 区域。
 * 采用双线性采样缩放，放大后描边平滑；alpha 掩膜来自开源矢量图标。 */
void aether_draw_icon(int id, int x, int y, int size, unsigned fg);

/* 应用图标磁贴：超椭圆玻璃底 + 上层单色线性符号 + 可选名称标签。
 * 底板/边框/高光由实现统一施加，保证同屏视觉重量一致。 */
void aether_draw_icon_tile(int id, int x, int y, int size,
                           unsigned glass, unsigned glyph,
                           const char *label, unsigned label_color);

/* 图标名（诊断/无障碍语义） */
const char *aether_icon_name(int id);

#endif /* AETHER_ICONS_H */
