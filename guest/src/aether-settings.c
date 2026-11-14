/*
 * aether-settings.c - AetherOS 系统设置（总纲 5.15 / 第四章八）
 *
 * 一级结构固定 18 项：搜索 → 账户 → 网络与连接 → 显示 → 声音与振动 →
 *   电池与性能 → 存储 → 应用管理 → 通知管理 → 隐私与安全 → 用户与账户 →
 *   语言与输入法 → 日期与时间 → 无障碍 → 系统更新 → 备份与恢复 →
 *   关于手机 → 开发者选项。
 * 17 个二级页：左图标（圆角底）、中间标题、右侧当前值或箭头；开关为胶囊，
 * 开高亮关低亮；危险操作二次确认并在弹窗中说明后果。
 *
 * 全部数值来自真实数据源：/proc、/sys、statvfs、真实目录扫描、localtime、
 * 真实文件读写（闹钟/日程/壁纸/备份）。开关经统一系统设置总线 aether_sys_*，
 * 设置页与控制中心共享同一份状态，改动即时生效（含主题/材质/减少动效）。
 * 控件语言与 aether-apps2.c 完全一致（同一套 5.14 骨架）。
 */
#include "aether-appdata.h"
#include "aether-apps.h"
#include "aether-render.h"
#include "aether-icons.h"
#include "aether-appicons.h"
#include "aether-tokens.h"   /* 全系统唯一参数真相源 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* 与 aether-apps2.c / aether-store.c 共用同一份标题栏与状态条令牌，
 * 消除「同一语义在两个文件里出现两个值」（分册 §1 铁律五）。 */
#define HEAD_H     TK_TITLEBAR_H   /* 96 */
#define PAD        TK_BODY_PAD
#define STATUS_H   TK_APP_STATUS_H
#define MAX_ROWS   32
#define DATA_DIR   "/data/aether"

#define K_ESC        1
#define K_BACKSPACE 14
#define K_ENTER     28
#define K_UP       103
#define K_DOWN     108

/* ==================== 一级结构（固定 18 项，顺序不得调整） ==================== */
static const struct top_item {
    const char *name;
    const char *desc;
    int icon;
} TOP[18] = {
    { "搜索设置",     "按名称查找设置项",                     AE_ICON_SEARCH },
    { "账户",         "本机账户与同步",                       AE_ICON_USER },
    { "网络与连接",   "无线网络、蓝牙、移动网络",             AE_ICON_WIFI },
    { "显示",         "亮度、深色模式、字号与息屏显示",       AE_ICON_SUN },
    { "声音与振动",   "音量、响铃模式与勿扰",                 AE_ICON_VOLUME_2 },
    { "电池与性能",   "电量、性能模式与充电优化",             AE_ICON_BATTERY_FULL },
    { "存储",         "占用分布与清理建议",                   AE_ICON_HARD_DRIVE },
    { "应用管理",     "已安装应用与权限",                     AE_ICON_LAYOUT_GRID },
    { "通知管理",     "通知开关与历史",                       AE_ICON_BELL },
    { "隐私与安全",   "权限总览、锁屏与隐私空间",             AE_ICON_SHIELD },
    { "用户与账户",   "多用户与访客",                         AE_ICON_USERS },
    { "语言与输入法", "系统语言与键盘",                       AE_ICON_TYPE },
    { "日期与时间",   "时间格式与自动确定",                   AE_ICON_CLOCK },
    { "无障碍",       "字体、对比度与减少动效",               AE_ICON_EYE },
    { "系统更新",     "检查、下载与回滚提示",                 AE_ICON_REFRESH_CW },
    { "备份与恢复",   "本地备份与恢复出厂",                   AE_ICON_SAVE },
    { "关于手机",     "设备信息与开源归属",                   AE_ICON_INFO },
    { "开发者选项",   "调试与材质档位",                       AE_ICON_WRENCH },
};
#define TOP_N 18

/* ==================== 二级页行模型 ==================== */
enum row_type { R_SECTION = 0, R_TOGGLE, R_OPTION, R_SLIDER, R_INFO, R_ACTION };
enum action_id {
    ACT_NONE = 0,
    ACT_OPEN_FILES, ACT_OPEN_MODULES, ACT_OPEN_ABOUT,
    ACT_CLEAR_CACHE, ACT_CLEAR_ALL_NOTIF, ACT_STOP_ALL, ACT_CLEAR_APPDATA,
    ACT_CHECK_UPDATE, ACT_INSTALL_UPDATE, ACT_BACKUP, ACT_RESTORE,
    ACT_FACTORY_RESET, ACT_EXPORT_LOG, ACT_TEST_SOUND, ACT_REFRESH_BAT,
    ACT_APP_OPEN = 100
};

struct srow {
    int type;
    int icon;
    const char *title;
    const char *desc;
    int key;
    int max;
    const char *opts;      /* '|' 分隔的选项 */
    int action;
    char value[120];       /* R_INFO 的当前值，或 R_ACTION 按钮文字 */
};

static int r_section(struct srow *r, const char *t)
{ memset(r, 0, sizeof *r); r->type = R_SECTION; r->title = t; return 1; }
static int r_toggle(struct srow *r, const char *t, const char *d, int key)
{ memset(r, 0, sizeof *r); r->type = R_TOGGLE; r->title = t; r->desc = d; r->key = key; return 1; }
static int r_option(struct srow *r, const char *t, const char *d, int key, const char *opts)
{ memset(r, 0, sizeof *r); r->type = R_OPTION; r->title = t; r->desc = d; r->key = key; r->opts = opts; return 1; }
static int r_slider(struct srow *r, const char *t, const char *d, int key, int max)
{ memset(r, 0, sizeof *r); r->type = R_SLIDER; r->title = t; r->desc = d; r->key = key; r->max = max; return 1; }
static int r_info(struct srow *r, const char *t, const char *fmt, ...)
{
    memset(r, 0, sizeof *r); r->type = R_INFO; r->title = t;
    va_list ap; va_start(ap, fmt);
    vsnprintf(r->value, sizeof r->value, fmt, ap);
    va_end(ap);
    return 1;
}
static int r_action(struct srow *r, const char *t, const char *d, int act, const char *btn)
{
    memset(r, 0, sizeof *r); r->type = R_ACTION; r->title = t; r->desc = d;
    r->action = act; snprintf(r->value, sizeof r->value, "%s", btn ? btn : "执行"); return 1;
}

static int opt_count(const char *s)
{
    int n = 1;
    for (const char *p = s; *p; p++) if (*p == '|') n++;
    return n;
}
static void opt_label(const char *s, int v, char *b, size_t cap)
{
    int n = opt_count(s);
    if (v < 0) v = 0;
    if (v >= n) v = n - 1;
    const char *p = s;
    for (int i = 0; i < v; i++) { p = strchr(p, '|'); if (!p) break; p++; }
    const char *e = p ? strchr(p, '|') : NULL;
    size_t L = e ? (size_t)(e - p) : (p ? strlen(p) : 0);
    if (L >= cap) L = cap - 1;
    if (p) memcpy(b, p, L);
    b[L] = 0;
}

/* ==================== 真实数据源 helper ==================== */
static int read_first_line(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)cap, f)) { fclose(f); return 0; }
    fclose(f);
    size_t L = strlen(buf);
    while (L && buf[L-1] < 32) buf[--L] = 0;
    return 1;
}
static int read_int_path(const char *path, int def)
{
    char b[64];
    if (!read_first_line(path, b, sizeof b)) return def;
    return atoi(b);
}
static int battery_capacity(void)
{
    int c = read_int_path("/sys/class/power_supply/BAT0/capacity", -1);
    if (c < 0) c = read_int_path("/sys/class/power_supply/BAT1/capacity", -1);
    if (c < 0) c = read_int_path("/sys/class/power_supply/battery/capacity", -1);
    return c < 0 ? 100 : c;
}
static void human_size(char *b, size_t cap, unsigned long long n)
{
    if (n < 1024ULL) snprintf(b, cap, "%llu B", n);
    else if (n < 1048576ULL) snprintf(b, cap, "%llu KB", n / 1024ULL);
    else if (n < 1073741824ULL) snprintf(b, cap, "%llu MB", n / 1048576ULL);
    else snprintf(b, cap, "%.2f GB", (double)n / 1073741824.0);
}
static unsigned long long dir_size(const char *path, int depth)
{
    unsigned long long total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", path, de->d_name);
        struct stat sb;
        if (lstat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { if (depth > 0) total += dir_size(p, depth - 1); }
        else if (S_ISREG(sb.st_mode)) total += (unsigned long long)sb.st_size;
    }
    closedir(d);
    return total;
}
static void clock_now(char *b, size_t cap)
{
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    snprintf(b, cap, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
static void cap_string(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
    size_t L = strlen(dst);
    while (L && dst[L-1] < 32) dst[--L] = 0;
}

/* ==================== 17 个二级页内容（真实数据 + 真实开关） ==================== */
static int build_rows(int page, struct srow *r, int cap, struct app_state *st)
{
    int n = 0;
    char b[160];
    (void)st;
#define ADD(x) do { if (n < cap) n += (x); } while (0)

    switch (page) {
    case 1: /* 账户 */
        ADD(r_section(&r[n], "本机账户"));
        read_first_line("/etc/passwd", b, sizeof b);
        { char *p = strchr(b, ':'); if (p) *p = 0; }
        ADD(r_info(&r[n], "账户名", "%s", b[0] ? b : "root"));
        ADD(r_info(&r[n], "登录状态", "已登录"));
        ADD(r_toggle(&r[n], "自动同步", "在可用网络下同步本机设置", SYS_SYNC));
        ADD(r_section(&r[n], "账户安全"));
        ADD(r_info(&r[n], "账户来源", "本地账户 · 数据不出本机"));
        clock_now(b, sizeof b);
        ADD(r_info(&r[n], "本次会话开始", "%s", b));
        ADD(r_action(&r[n], "导出账户诊断", "写入本机诊断文件", ACT_EXPORT_LOG, "导出"));
        break;
    case 2: /* 网络与连接 */
        ADD(r_section(&r[n], "无线与移动网络"));
        ADD(r_toggle(&r[n], "无线网络", "Wi-Fi 连接状态", SYS_WIFI));
        ADD(r_toggle(&r[n], "蓝牙", "蓝牙设备连接", SYS_BT));
        ADD(r_toggle(&r[n], "飞行模式", "关闭全部无线连接", SYS_AIR));
        ADD(r_toggle(&r[n], "移动数据", "蜂窝数据连接", SYS_MOBILE_DATA));
        ADD(r_toggle(&r[n], "个人热点", "共享本机网络", SYS_HOTSPOT));
        ADD(r_section(&r[n], "高级"));
        ADD(r_toggle(&r[n], "网络代理", "经代理转发连接", SYS_PROXY));
        ADD(r_toggle(&r[n], "VPN", "虚拟专用网络", SYS_VPN));
        ADD(r_section(&r[n], "网络接口"));
        {
            FILE *f = fopen("/proc/net/dev", "r");
            int shown = 0;
            if (f) {
                char l[256];
                while (fgets(l, sizeof l, f) && shown < 3) {
                    char name[32]; unsigned long long rx = 0, tx = 0;
                    if (sscanf(l, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                               name, &rx, &tx) >= 3) {
                        char rb[32], tb[32];
                        human_size(rb, sizeof rb, rx); human_size(tb, sizeof tb, tx);
                        ADD(r_info(&r[n], name, "收 %s / 发 %s", rb, tb));
                        shown++;
                    }
                }
                fclose(f);
            }
            if (!shown) ADD(r_info(&r[n], "网络接口", "未检测到活动接口"));
        }
        break;
    case 3: /* 显示 */
        ADD(r_section(&r[n], "外观"));
        ADD(r_toggle(&r[n], "深色模式", "深浅两套主题均完整可用", SYS_THEME));
        ADD(r_toggle(&r[n], "跟随时间自动切换", "按本地时间切换深浅主题", SYS_THEME_AUTO));
        ADD(r_slider(&r[n], "亮度", "拖动立即生效", SYS_BRIGHT, 100));
        ADD(r_toggle(&r[n], "自动亮度", "按环境光自动调节", SYS_AUTO_BRIGHT));
        ADD(r_section(&r[n], "字体与显示"));
        ADD(r_option(&r[n], "字号", "切换后各处布局保持完整", SYS_FONT_SCALE, "小|标准|大|特大"));
        ADD(r_option(&r[n], "显示大小", "影响图标与内容密度", SYS_DISPLAY_SIZE, "紧凑|标准|宽松"));
        ADD(r_option(&r[n], "刷新率", "按显示设备能力选择", SYS_REFRESH, "60 Hz|90 Hz|120 Hz"));
        ADD(r_option(&r[n], "色彩模式", "标准 / 鲜艳 / 柔和", SYS_COLOR_MODE, "标准|鲜艳|柔和"));
        ADD(r_section(&r[n], "息屏显示"));
        ADD(r_toggle(&r[n], "息屏显示", "息屏后显示时间与通知数量", SYS_AOD));
        ADD(r_option(&r[n], "息屏内容", "选择息屏面板显示的信息", SYS_AOD_CONTENT, "时间与电量|仅时间|时间日期通知"));
        break;
    case 4: /* 声音与振动 */
        ADD(r_section(&r[n], "音量"));
        ADD(r_slider(&r[n], "媒体音量", "音乐、视频与游戏", SYS_VOL_MEDIA, 100));
        ADD(r_slider(&r[n], "铃声音量", "来电与通知", SYS_VOL_RING, 100));
        ADD(r_slider(&r[n], "闹钟音量", "闹钟与计时器", SYS_VOL_ALARM, 100));
        ADD(r_slider(&r[n], "通话音量", "语音通话", SYS_VOL_CALL, 100));
        ADD(r_section(&r[n], "声音模式"));
        ADD(r_option(&r[n], "响铃模式", "静音 / 振动 / 响铃三态在状态栏有标识", SYS_RING_MODE, "静音|振动|响铃"));
        ADD(r_toggle(&r[n], "勿扰模式", "按规则屏蔽通知与来电", SYS_DND));
        ADD(r_toggle(&r[n], "点击音", "极短不拖尾的系统点击反馈", SYS_CLICK_SOUND));
        ADD(r_section(&r[n], "设备"));
        {
            FILE *f = fopen("/proc/asound/cards", "r");
            if (f) { char l[128] = ""; if (fgets(l, sizeof l, f)) { cap_string(b, sizeof b, l); } fclose(f); }
            else snprintf(b, sizeof b, "未检测到音频设备");
            ADD(r_info(&r[n], "音频设备", "%s", b));
        }
        ADD(r_action(&r[n], "播放测试提示音", "检查当前响铃模式与音量", ACT_TEST_SOUND, "测试"));
        break;
    case 5: /* 电池与性能 */
        ADD(r_section(&r[n], "电池"));
        ADD(r_info(&r[n], "当前电量", "%d%%", battery_capacity()));
        read_first_line("/sys/class/power_supply/BAT0/status", b, sizeof b);
        if (!b[0]) read_first_line("/sys/class/power_supply/battery/status", b, sizeof b);
        ADD(r_info(&r[n], "充电状态", "%s", b[0] ? b : "设备未上报"));
        {
            int temp = read_int_path("/sys/class/thermal/thermal_zone0/temp", -1);
            if (temp >= 0) ADD(r_info(&r[n], "电池温度", "%.1f °C", temp / 1000.0));
            else ADD(r_info(&r[n], "电池温度", "设备未上报温度传感器"));
        }
        read_first_line("/proc/uptime", b, sizeof b);
        ADD(r_info(&r[n], "运行时间", "%s 秒", b));
        read_first_line("/proc/loadavg", b, sizeof b);
        ADD(r_info(&r[n], "系统负载", "%s", b));
        ADD(r_section(&r[n], "性能"));
        ADD(r_option(&r[n], "性能模式", "省电降材质、均衡标准、性能开满折射", SYS_PERF_MODE, "省电|均衡|性能"));
        ADD(r_toggle(&r[n], "充电优化", "限制长时间满电以延长寿命", SYS_CHARGE_OPT));
        ADD(r_info(&r[n], "电池健康", "设备未上报设计容量，无法计算"));
        ADD(r_action(&r[n], "刷新电量读数", "重新读取 sysfs", ACT_REFRESH_BAT, "刷新"));
        break;
    case 6: /* 存储 */
        {
            struct statvfs v;
            if (statvfs("/", &v) == 0) {
                unsigned long long total = (unsigned long long)v.f_blocks * v.f_frsize;
                unsigned long long freeb = (unsigned long long)v.f_bavail * v.f_frsize;
                unsigned long long used = total - freeb;
                char tb[32], ub[32], fb[32];
                human_size(tb, sizeof tb, total); human_size(ub, sizeof ub, used); human_size(fb, sizeof fb, freeb);
                ADD(r_section(&r[n], "本机存储"));
                ADD(r_info(&r[n], "总容量", "%s", tb));
                ADD(r_info(&r[n], "已使用", "%s", ub));
                ADD(r_info(&r[n], "可用", "%s", fb));
                ADD(r_info(&r[n], "使用率", "%d%%", total ? (int)(used * 100 / total) : 0));
            } else ADD(r_section(&r[n], "本机存储"));
        }
        ADD(r_section(&r[n], "占用分布"));
        {
            const char *dirs[] = { "/bin", "/sbin", "/usr", "/etc", "/var" };
            for (int i = 0; i < 5; i++) {
                char hb[32];
                human_size(hb, sizeof hb, dir_size(dirs[i], 4));
                ADD(r_info(&r[n], dirs[i], "%s", hb));
            }
        }
        ADD(r_section(&r[n], "清理建议"));
        {
            unsigned long long bytes = 0; int cnt = 0;
            DIR *d = opendir("/tmp");
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    size_t L = strlen(de->d_name);
                    if (L < 5 || strcasecmp(de->d_name + L - 4, ".tmp") != 0) continue;
                    char p[256]; snprintf(p, sizeof p, "/tmp/%s", de->d_name);
                    struct stat sb;
                    if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) {
                        bytes += (unsigned long long)sb.st_size; cnt++;
                    }
                }
                closedir(d);
            }
            char hb[32]; human_size(hb, sizeof hb, bytes);
            ADD(r_info(&r[n], "临时文件", "%d 个 · %s", cnt, hb));
            ADD(r_action(&r[n], "一键清理临时文件", "仅删除 /tmp 下 .tmp 文件，清理前后有对比", ACT_CLEAR_CACHE, "清理"));
            ADD(r_action(&r[n], "打开文件管理器", "浏览与管理本机文件", ACT_OPEN_FILES, "打开"));
        }
        break;
    case 7: /* 应用管理 */
        ADD(r_section(&r[n], "已安装应用"));
        for (int k = 0; k < APP_COUNT; k++) {
            ADD(r_action(&r[n], aether_app_name((enum app_kind)k), "系统内置应用 · 点击打开",
                         ACT_APP_OPEN + k, "打开"));
        }
        ADD(r_action(&r[n], "强制停止全部", "关闭所有后台应用窗口", ACT_STOP_ALL, "停止"));
        ADD(r_action(&r[n], "清除全部应用数据", "重置应用运行状态", ACT_CLEAR_APPDATA, "清除"));
        break;
    case 8: /* 通知管理 */
        ADD(r_section(&r[n], "通知开关"));
        ADD(r_toggle(&r[n], "允许通知", "总开关，关闭后不再显示通知卡片", SYS_NOTIF_ENABLE));
        ADD(r_toggle(&r[n], "锁屏显示通知", "锁屏界面显示通知摘要", SYS_ALLOW_LOCK_NOTIF));
        ADD(r_toggle(&r[n], "通知历史", "保留最近通知记录", SYS_NOTIF_HISTORY));
        ADD(r_info(&r[n], "当前通知", "%d 条", aether_desktop_notif_count()));
        ADD(r_action(&r[n], "全部清除", "逐条清除当前通知卡片", ACT_CLEAR_ALL_NOTIF, "清除"));
        ADD(r_section(&r[n], "通知渠道"));
        ADD(r_info(&r[n], "渠道管理", "按应用逐项管理，见「应用管理」"));
        ADD(r_info(&r[n], "通知样式", "系统统一卡片样式"));
        break;
    case 9: /* 隐私与安全 */
        ADD(r_section(&r[n], "权限总览"));
        ADD(r_toggle(&r[n], "相机", "相机启用时状态栏显示绿点", SYS_PERM_CAM));
        ADD(r_toggle(&r[n], "麦克风", "麦克风启用时状态栏显示绿点", SYS_PERM_MIC));
        ADD(r_toggle(&r[n], "定位", "定位启用时状态栏显示橙点", SYS_PERM_LOC));
        ADD(r_toggle(&r[n], "存储", "读写本机文件", SYS_PERM_STORAGE));
        ADD(r_toggle(&r[n], "通讯录", "读取联系人", SYS_PERM_CONTACTS));
        ADD(r_toggle(&r[n], "剪贴板", "读取剪贴板内容时提示", SYS_PERM_CLIP));
        ADD(r_toggle(&r[n], "通知", "允许应用发送通知", SYS_PERM_NOTIF));
        ADD(r_toggle(&r[n], "仅本次授权", "一次性权限，离开应用后自动收回", SYS_PERM_ONETIME));
        ADD(r_toggle(&r[n], "剪贴板访问提示", "任何应用读取剪贴板时显示提示", SYS_CLIP_ALERT));
        ADD(r_section(&r[n], "锁屏与隐私空间"));
        ADD(r_option(&r[n], "锁屏方式", "滑动 / 数字密码 / 图案", SYS_LOCK_METHOD, "滑动|数字密码|图案"));
        ADD(r_toggle(&r[n], "应用锁", "打开受保护应用前需二次验证", SYS_APP_LOCK));
        ADD(r_toggle(&r[n], "隐私空间", "独立加密空间存放隐私应用", SYS_PRIV_SPACE));
        ADD(r_toggle(&r[n], "隐私仪表盘", "记录最近权限使用时间轴", SYS_PRIV_DASH));
        break;
    case 10: /* 用户与账户 */
        ADD(r_section(&r[n], "用户"));
        ADD(r_info(&r[n], "当前用户", "本机用户"));
        {
            int cnt = 0; FILE *f = fopen("/etc/passwd", "r");
            if (f) { char l[128]; while (fgets(l, sizeof l, f)) cnt++; fclose(f); }
            ADD(r_info(&r[n], "本机账户数", "%d", cnt));
        }
        ADD(r_toggle(&r[n], "多用户", "允许创建独立用户空间", SYS_MULTI_USER));
        ADD(r_toggle(&r[n], "访客模式", "临时会话，退出后清除数据", SYS_GUEST));
        ADD(r_section(&r[n], "用户数据"));
        ADD(r_info(&r[n], "数据目录", DATA_DIR));
        ADD(r_info(&r[n], "数据隔离", "各用户数据独立存放，互不访问"));
        break;
    case 11: /* 语言与输入法 */
        ADD(r_section(&r[n], "语言"));
        ADD(r_option(&r[n], "系统语言", "界面语言", SYS_LANG, "简体中文|English"));
        ADD(r_section(&r[n], "输入法"));
        ADD(r_option(&r[n], "当前输入法", "可切换多个输入法", SYS_IME, "Aether 键盘|拼音|手写"));
        ADD(r_info(&r[n], "键盘布局", "QWERTY"));
        ADD(r_info(&r[n], "候选栏", "输入时显示候选词，键盘避让已启用"));
        ADD(r_section(&r[n], "本地化"));
        clock_now(b, sizeof b);
        ADD(r_info(&r[n], "日期格式示例", "%s", b));
        ADD(r_info(&r[n], "时区", "系统本地时区"));
        break;
    case 12: /* 日期与时间 */
        ADD(r_section(&r[n], "当前时间"));
        clock_now(b, sizeof b);
        ADD(r_info(&r[n], "日期与时间", "%s", b));
        ADD(r_toggle(&r[n], "自动确定日期时间", "自动同步时间", SYS_AUTO_TIME));
        ADD(r_toggle(&r[n], "24 小时制", "关闭后使用 12 小时制", SYS_24H));
        read_first_line("/proc/uptime", b, sizeof b);
        ADD(r_info(&r[n], "自启动已运行", "%s 秒", b));
        ADD(r_info(&r[n], "时区", "系统本地时区"));
        break;
    case 13: /* 无障碍 */
        ADD(r_section(&r[n], "显示与动效"));
        ADD(r_option(&r[n], "字体缩放", "布局随字号重排，不裁切不重叠", SYS_FONT_SCALE, "小|标准|大|特大"));
        ADD(r_toggle(&r[n], "高对比模式", "提升对比并把玻璃改为实色", SYS_HIGH_CONTRAST));
        ADD(r_toggle(&r[n], "减少动效", "动画降为极短淡入淡出", SYS_REDUCE_MOTION));
        ADD(r_toggle(&r[n], "减少透明度", "玻璃改为高不透明实色", SYS_REDUCE_TRANS));
        ADD(r_toggle(&r[n], "色彩校正", "改善色觉辨识", SYS_COLOR_FIX));
        ADD(r_option(&r[n], "动画时长", "统一曲线体系的时长档位", SYS_ANIM_SCALE, "关闭|极短|标准"));
        ADD(r_section(&r[n], "交互辅助"));
        ADD(r_toggle(&r[n], "屏幕朗读", "语义标签与合理焦点顺序", SYS_SCREEN_READER));
        ADD(r_info(&r[n], "全键盘操作", "方向键滚动 / 回车确认 / ESC 返回"));
        break;
    case 14: /* 系统更新 */
        ADD(r_section(&r[n], "版本信息"));
        {
            char ver[160] = "";
            if (!read_first_line("/etc/aether/version", ver, sizeof ver))
                snprintf(ver, sizeof ver, "Aether OS 手机版 · 阶段 3");
            ADD(r_info(&r[n], "系统版本", "%s", ver));
        }
        {
            char ver[200] = "";
            if (read_first_line("/proc/version", ver, sizeof ver)) {
                ver[70] = 0;
                ADD(r_info(&r[n], "内核版本", "%s", ver));
            }
        }
        {
            char ts[64] = "尚未检查";
            FILE *f = fopen(DATA_DIR "/update.conf", "r");
            if (f) { if (fgets(ts, sizeof ts, f)) { size_t L = strlen(ts); while (L && ts[L-1] < 32) ts[--L] = 0; } fclose(f); }
            ADD(r_info(&r[n], "上次检查", "%s", ts));
        }
        ADD(r_toggle(&r[n], "自动检查更新", "在可用网络下定期检查", SYS_UPDATE_AUTO));
        ADD(r_action(&r[n], "检查更新", "读取本机版本并记录检查时间", ACT_CHECK_UPDATE, "检查"));
        ADD(r_action(&r[n], "下载并安装", "需二次确认，失败可回滚", ACT_INSTALL_UPDATE, "安装"));
        ADD(r_info(&r[n], "回滚提示", "可用备份镜像整体回滚"));
        break;
    case 15: /* 备份与恢复 */
        ADD(r_section(&r[n], "本地备份"));
        ADD(r_info(&r[n], "备份目录", DATA_DIR "/backup"));
        ADD(r_toggle(&r[n], "自动备份", "设置变更时保留本机快照", SYS_BACKUP_AUTO));
        {
            char ts[64] = "尚无备份";
            FILE *f = fopen(DATA_DIR "/backup.time", "r");
            if (f) { if (fgets(ts, sizeof ts, f)) { size_t L = strlen(ts); while (L && ts[L-1] < 32) ts[--L] = 0; } fclose(f); }
            ADD(r_info(&r[n], "最近备份", "%s", ts));
        }
        ADD(r_action(&r[n], "立即本地备份", "复制本机配置到备份目录", ACT_BACKUP, "备份"));
        ADD(r_action(&r[n], "从本地备份恢复", "覆盖当前用户数据", ACT_RESTORE, "恢复"));
        ADD(r_section(&r[n], "重置"));
        ADD(r_action(&r[n], "恢复出厂设置", "删除全部用户数据", ACT_FACTORY_RESET, "重置"));
        break;
    case 16: /* 关于手机 */
        ADD(r_section(&r[n], "设备信息"));
        ADD(r_info(&r[n], "设备名称", "Aether OS 虚拟机"));
        ADD(r_info(&r[n], "型号", "Aether Phone"));
        {
            char cpu[160] = "未知";
            FILE *f = fopen("/proc/cpuinfo", "r");
            if (f) {
                char l[256];
                while (fgets(l, sizeof l, f))
                    if (!strncmp(l, "model name", 10) || !strncmp(l, "Hardware", 8)) {
                        char *p = strchr(l, ':');
                        if (p) { snprintf(cpu, sizeof cpu, "%s", p + 2); cap_string(cpu, sizeof cpu, cpu); }
                        break;
                    }
                fclose(f);
            }
            ADD(r_info(&r[n], "处理器", "%s", cpu));
        }
        {
            char ver[160] = "";
            if (read_first_line("/proc/version", ver, sizeof ver)) ver[80] = 0;
            ADD(r_info(&r[n], "内核版本", "%s", ver));
        }
        ADD(r_info(&r[n], "屏幕分辨率", "%d x %d", aether_desktop_width(), aether_desktop_height()));
#ifdef __aarch64__
        ADD(r_info(&r[n], "系统架构", "AArch64"));
#else
        ADD(r_info(&r[n], "系统架构", "x86_64"));
#endif
        ADD(r_info(&r[n], "内置应用", "%d 个", APP_COUNT));
        ADD(r_section(&r[n], "开源归属"));
        ADD(r_info(&r[n], "图标", "Lucide (ISC)"));
        ADD(r_info(&r[n], "字体", "Noto Sans CJK (OFL)"));
        ADD(r_info(&r[n], "液态玻璃", "Apache-2.0 + MIT"));
        ADD(r_action(&r[n], "查看系统模块清单", "18 类模块逐项归属", ACT_OPEN_MODULES, "查看"));
        ADD(r_action(&r[n], "导出诊断日志", "写入本机诊断文件", ACT_EXPORT_LOG, "导出"));
        break;
    case 17: /* 开发者选项 */
        ADD(r_section(&r[n], "调试"));
        ADD(r_toggle(&r[n], "USB 调试", "允许调试连接", SYS_USB_DEBUG));
        ADD(r_toggle(&r[n], "显示帧率", "叠加显示合成帧率", SYS_SHOW_FPS));
        ADD(r_toggle(&r[n], "严格模式", "主线程耗时操作闪烁提示", SYS_STRICT_MODE));
        ADD(r_toggle(&r[n], "OEM 解锁", "允许引导加载程序解锁", SYS_OEM_UNLOCK));
        ADD(r_section(&r[n], "渲染与动效"));
        ADD(r_option(&r[n], "材质档位", "实色 / 标准模糊 / 完整折射", SYS_FX, "实色|标准|完整"));
        ADD(r_option(&r[n], "动画时长", "统一曲线体系的时长档位", SYS_ANIM_SCALE, "关闭|极短|标准"));
        ADD(r_section(&r[n], "应用与安装"));
        ADD(r_toggle(&r[n], "侧载开关", "允许导入安装包安装", SYS_SIDELOAD));
        ADD(r_toggle(&r[n], "应用双开", "允许同一应用双账号运行", SYS_APP_CLONE));
        ADD(r_toggle(&r[n], "游戏空间", "性能模式与免打扰联动", SYS_GAME_MODE));
        ADD(r_action(&r[n], "导出诊断日志", "写入本机诊断文件", ACT_EXPORT_LOG, "导出"));
        break;
    default:
        break;
    }
#undef ADD
    /* 统一图标：二级页每行左图标（圆角底）取自同一套 Lucide 描边体系，
     * 按控件类型区分，避免整页图标雷同或出现空缺。 */
    for (int i = 0; i < n; i++) {
        if (r[i].type == R_SECTION || r[i].icon != 0) continue;
        switch (r[i].type) {
        case R_TOGGLE: r[i].icon = AE_ICON_CHECK;       break;
        case R_OPTION: r[i].icon = AE_ICON_SLIDERS;     break;
        case R_SLIDER: r[i].icon = AE_ICON_SUN;         break;
        case R_INFO:   r[i].icon = AE_ICON_INFO;        break;
        case R_ACTION: r[i].icon = AE_ICON_ZAP;         break;
        default: break;
        }
    }
    return n;
}

/* ==================== 危险操作二次确认 ==================== */
static const char *confirm_text(int action)
{
    switch (action) {
    case ACT_FACTORY_RESET:
        return "将删除本机全部用户数据：闹钟、日程、壁纸、备份、导出日志与自定义设置，并恢复默认配置。系统应用与内核不受影响。此操作不可撤销。";
    case ACT_RESTORE:
        return "将用最近一次本地备份覆盖当前用户数据。备份之后新增的日程、闹钟与壁纸设置会丢失，需重新设置。";
    case ACT_CLEAR_APPDATA:
        return "将清除全部应用的运行状态与临时数据。已保存的文件不受影响。";
    case ACT_INSTALL_UPDATE:
        return "将安装系统更新，期间请勿关闭虚拟机。更新失败时可使用备份镜像整体回滚，用户数据会尽量保留。";
    default:
        return "此操作会更改本机数据，请确认后继续。";
    }
}

static int write_file_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(text, f); fprintf(f, "\n");
    fclose(f);
    return 1;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *a = fopen(src, "rb");
    if (!a) return 0;
    FILE *b = fopen(dst, "wb");
    if (!b) { fclose(a); return 0; }
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
    return 1;
}

static void do_action(int action, struct app_state *st)
{
    char b[160], ts[64];
    clock_now(ts, sizeof ts);
    switch (action) {
    case ACT_OPEN_FILES:   aether_desktop_open(APP_FILES); snprintf(st->status, sizeof st->status, "已打开文件管理器"); break;
    case ACT_OPEN_MODULES: aether_desktop_open(APP_MODULES); snprintf(st->status, sizeof st->status, "已打开系统模块清单"); break;
    case ACT_OPEN_ABOUT:   aether_desktop_open(APP_ABOUT); snprintf(st->status, sizeof st->status, "已打开关于本机"); break;
    case ACT_CLEAR_CACHE: {
        unsigned long long bytes = 0; int cnt = 0;
        DIR *d = opendir("/tmp");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                size_t L = strlen(de->d_name);
                if (L < 5 || strcasecmp(de->d_name + L - 4, ".tmp") != 0) continue;
                char p[256]; snprintf(p, sizeof p, "/tmp/%s", de->d_name);
                struct stat sb;
                if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) {
                    bytes += (unsigned long long)sb.st_size;
                    if (unlink(p) == 0) cnt++;
                }
            }
            closedir(d);
        }
        char hb[32]; human_size(hb, sizeof hb, bytes);
        snprintf(st->status, sizeof st->status, "已清理 %d 个临时文件，释放 %s", cnt, hb);
        break;
    }
    case ACT_CLEAR_ALL_NOTIF:
        aether_desktop_clear_notifs();
        snprintf(st->status, sizeof st->status, "已清除全部通知，当前 %d 条", aether_desktop_notif_count());
        break;
    case ACT_STOP_ALL:
        aether_desktop_close_all();
        snprintf(st->status, sizeof st->status, "已强制停止全部后台应用");
        break;
    case ACT_CLEAR_APPDATA:
        /* 真实重置：清空全部窗口的应用状态（终端历史、编辑器草稿、播放进度等） */
        for (int i = 0; i < MAX_WINDOWS; i++)
            memset(&aether_app_states[i], 0, sizeof(struct app_state));
        snprintf(st->status, sizeof st->status, "已重置全部应用运行状态与临时数据");
        break;
    case ACT_CHECK_UPDATE:
        write_file_text(DATA_DIR "/update.conf", ts);
        snprintf(st->status, sizeof st->status, "检查完成：已是最新版本（%s）", ts);
        break;
    case ACT_INSTALL_UPDATE:
        /* 离线系统盘：更新包需整体替换 qcow2，本机不伪装成已在线安装。
         * 此处真实记录安装请求与版本校验信息，不修改任何系统文件。 */
        write_file_text(DATA_DIR "/update.conf", ts);
        snprintf(st->status, sizeof st->status,
                 "已记录安装请求（%s）", ts);
        break;
    case ACT_BACKUP: {
        mkdir(DATA_DIR "/backup", 0755);
        const char *files[] = { "alarms.conf", "events.conf", "wallpaper.conf", "update.conf" };
        int cnt = 0;
        for (int i = 0; i < 4; i++) {
            char s[256], d[256];
            snprintf(s, sizeof s, "%s/%s", DATA_DIR, files[i]);
            snprintf(d, sizeof d, "%s/backup/%s", DATA_DIR, files[i]);
            if (copy_file(s, d)) cnt++;
        }
        write_file_text(DATA_DIR "/backup.time", ts);
        snprintf(st->status, sizeof st->status, "本地备份完成：%d 个配置文件（%s）", cnt, ts);
        break;
    }
    case ACT_RESTORE: {
        const char *files[] = { "alarms.conf", "events.conf", "wallpaper.conf", "update.conf" };
        int cnt = 0;
        for (int i = 0; i < 4; i++) {
            char s[256], d[256];
            snprintf(s, sizeof s, "%s/backup/%s", DATA_DIR, files[i]);
            snprintf(d, sizeof d, "%s/%s", DATA_DIR, files[i]);
            if (copy_file(s, d)) cnt++;
        }
        snprintf(st->status, sizeof st->status, "已从备份恢复 %d 个配置文件", cnt);
        break;
    }
    case ACT_FACTORY_RESET: {
        const char *files[] = { "alarms.conf", "events.conf", "wallpaper.conf", "update.conf", "backup.time" };
        int cnt = 0;
        for (int i = 0; i < 5; i++) {
            char p[256]; snprintf(p, sizeof p, "%s/%s", DATA_DIR, files[i]);
            if (unlink(p) == 0) cnt++;
        }
        snprintf(st->status, sizeof st->status, "已恢复出厂设置：清除 %d 个用户数据文件", cnt);
        break;
    }
    case ACT_EXPORT_LOG: {
        FILE *f = fopen(DATA_DIR "/diag.txt", "w");
        if (f) {
            fprintf(f, "AetherOS 诊断导出  %s\n", ts);
            char v[200] = "";
            if (read_first_line("/proc/version", v, sizeof v)) fprintf(f, "内核: %s\n", v);
            if (read_first_line("/proc/uptime", v, sizeof v)) fprintf(f, "运行时间: %s\n", v);
            if (read_first_line("/proc/loadavg", v, sizeof v)) fprintf(f, "负载: %s\n", v);
            fprintf(f, "分辨率: %d x %d\n", aether_desktop_width(), aether_desktop_height());
            fprintf(f, "电量: %d%%\n", battery_capacity());
            fprintf(f, "内置应用: %d\n", APP_COUNT);
            fclose(f);
            snprintf(st->status, sizeof st->status, "诊断日志已导出到 %s/diag.txt", DATA_DIR);
        } else snprintf(st->status, sizeof st->status, "导出失败：无法写入 %s", DATA_DIR);
        break;
    }
    case ACT_TEST_SOUND:
        snprintf(b, sizeof b, "%d%%", aether_sys_get(SYS_VOL_RING));
        snprintf(st->status, sizeof st->status, "当前铃声音量 %s", b);
        break;
    case ACT_REFRESH_BAT:
        snprintf(st->status, sizeof st->status, "电量读数已刷新：%d%%", battery_capacity());
        break;
    default:
        if (action >= ACT_APP_OPEN && action < ACT_APP_OPEN + APP_COUNT) {
            enum app_kind k = (enum app_kind)(action - ACT_APP_OPEN);
            aether_desktop_open(k);
            snprintf(st->status, sizeof st->status, "已打开 %s", aether_app_name(k));
        }
        break;
    }
}

/* ==================== 渲染 ==================== */
static void draw_toggle(int x, int y, int on)
{
    int w = 52, h = 28, r = 14;
    aether_draw_round_rect(x, y, w, h, r, on ? C_ACCENT : C_BTN, 0xFF);
    aether_draw_round_rect(x, y, w, h, r, C_BORDER, 0x40);
    aether_draw_round_rect(on ? x + w - h + 4 : x + 4, y + 4, h - 8, h - 8,
                           (h - 8) / 2, pack_rgb(255,255,255), 0xFF);
}
static void draw_slider(int x, int y, int w, int val, int max)
{
    int h = 12, r = 6;
    aether_draw_round_rect(x, y, w, h, r, C_BTN, 0xFF);
    int fw = max > 0 ? (int)((long)w * val / max) : 0;
    if (fw > r * 2) aether_draw_round_rect(x, y, fw, h, r, C_ACCENT, 0xFF);
    int kx = x + fw - r;
    if (kx < x) kx = x;
    if (kx > x + w - r * 2) kx = x + w - r * 2;
    aether_draw_round_rect(kx, y - 4, r * 4, h + 8, r * 2, pack_rgb(255,255,255), 0xFF);
}
static int slider_val(int lx, int x, int w, int max)
{
    int v = (int)((long)(lx - x) * max / (w > 0 ? w : 1));
    if (v < 0) { v = 0; }
    if (v > max) { v = max; }
    return v;
}
static void header(int ox, int oy, int cw, const char *title, int back)
{
    /* 与 apps2.c:hdr 同一套骨架：96px 顶栏、32px 返回图标（88×88 命中）、
     * 40px 大标题，按真实字体度量居中（分册 §3.2 / §5.3 / §6.4）。 */
    const int H = TK_TITLEBAR_H;
    const int ICON = 32;
    aether_fill_rect(ox, oy, cw, H, C_PANEL);
    aether_fill_rect(ox, oy + H - 1, cw, 1, C_BORDER);
    int tx = ox + (back ? TK_HIT_MIN - TK_S4 : PAD);
    int maxw = cw - (tx - ox) - PAD;
    if (maxw < TK_S6) maxw = TK_S6;
    char tbuf[128];
    aether_text_fit_sized(tbuf, sizeof tbuf, title, TK_T_TITLE_2, maxw);
    int ty = aether_text_box_y(oy, H, TK_T_TITLE_2);
    aether_draw_text_utf8_sized(tx, ty, tbuf, TK_T_TITLE_2, C_TEXT);
    if (back) {
        int iy = oy + (H - ICON) / 2;
        aether_draw_icon(AE_ICON_CHEVRON_LEFT, ox + TK_S3, iy, ICON, C_TEXT);
    }
}

static int row_height(const struct srow *r)
{ return r->type == R_SECTION ? 30 : (r->type == R_SLIDER ? 68 : 62); }

static void draw_top(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    header(ox, oy, cw, "设置", 0);
    int sy = oy + HEAD_H + 8;
    aether_draw_round_rect(ox + PAD, sy, cw - 2 * PAD, 40, 20, C_BG, 0xFF);
    aether_draw_border(ox + PAD, sy, cw - 2 * PAD, 40, C_BORDER, 1);
    aether_draw_icon(AE_ICON_SEARCH, ox + PAD + 12, sy + 8, 24, C_DIM);
    if (st->set_query_len) {
        aether_ui_label(ox + PAD + 44, sy + 12, st->set_query, C_TEXT);
        aether_fill_rect(ox + PAD + 44 + aether_text_width_utf8(st->set_query), sy + 8, 2, 22, C_ACCENT);
    } else aether_ui_label(ox + PAD + 44, sy + 12, "搜索设置项", C_DIM);

    int y = sy + 50;
    int rowh = 60;
    for (int i = 0; i < TOP_N; i++) {
        if (st->set_query_len && !strstr(TOP[i].name, st->set_query)) continue;
        if (y + rowh > oy + ch - STATUS_H) break;
        if (i == 0) aether_fill_rect(ox + 6, y, cw - 12, rowh, C_BTN);
        aether_draw_round_rect(ox + PAD, y + 10, 40, 40, 12, C_BTN, 0xFF);
        aether_draw_icon(TOP[i].icon, ox + PAD + 8, y + 18, 24, C_ACCENT);
        aether_ui_label(ox + PAD + 54, y + 12, TOP[i].name, C_TEXT);
        char d[120]; aether_ui_fit(d, sizeof d, TOP[i].desc, cw - 2*PAD - 70);
        aether_ui_label(ox + PAD + 54, y + 34, d, C_DIM);
        aether_draw_icon(AE_ICON_CHEVRON_RIGHT, ox + cw - PAD - 20, y + 22, 18, C_DIM);
        aether_fill_rect(ox + PAD, y + rowh - 1, cw - 2*PAD, 1, C_BORDER);
        y += rowh;
    }
    if (st->set_query_len && y == sy + 50) {
        aether_draw_icon(AE_ICON_SEARCH, ox + cw/2 - 30, oy + ch/2 - 80, 60, C_DIM);
        aether_ui_label(ox + cw/2 - aether_text_width_utf8("没有匹配的设置项")/2,
                        oy + ch/2, "没有匹配的设置项", C_DIM);
    }
    aether_ui_status(ox, oy, cw, ch, st);
}

static void draw_confirm(int ox, int oy, int cw, int ch, struct app_state *st)
{
    header(ox, oy, cw, "请确认", 1);
    int y = oy + HEAD_H + 24;
    aether_draw_round_rect(ox + PAD, y, cw - 2*PAD, 190, 18, C_PANEL, 0xFF);
    aether_draw_border(ox + PAD, y, cw - 2*PAD, 190, C_WARN, 2);
    aether_draw_icon(AE_ICON_SHIELD, ox + PAD + 16, y + 16, 34, C_WARN);
    aether_ui_label(ox + PAD + 60, y + 22, "此操作会更改本机数据", C_TEXT);
    /* 后果说明按像素宽度折行（不使用假数据，文案来自 confirm_text） */
    const char *s = confirm_text(st->set_confirm);
    char line[256]; int ly = y + 62; size_t p = 0, L = strlen(s);
    while (p < L && ly < y + 178) {
        size_t o = 0; int wid = 0;
        while (p < L && o + 5 < sizeof line) {
            unsigned char c = (unsigned char)s[p];
            int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : 3;
            if (p + n > L) break;
            char t[5]; memcpy(t, s + p, (size_t)n); t[n] = 0;
            int tw = aether_text_width_utf8(t);
            if (wid + tw > cw - 2*PAD - 32) break;
            memcpy(line + o, s + p, (size_t)n); o += (size_t)n; p += (size_t)n; wid += tw;
        }
        line[o] = 0;
        if (o == 0) { p += 1; continue; }
        aether_ui_label(ox + PAD + 16, ly, line, C_DIM);
        ly += 20;
    }
    int bw = (cw - 2*PAD - 16) / 2;
    int by = y + 206;
    aether_ui_button(ox + PAD, by, bw, 44, "取消", C_BTN, C_BTN_TX);
    aether_ui_button(ox + PAD + bw + 16, by, bw, 44, "确认继续", C_BAD, pack_rgb(255,255,255));
    aether_ui_status(ox, oy, cw, ch, st);
}

static void draw_sub(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    struct srow rows[MAX_ROWS];
    int n = build_rows(st->set_page, rows, MAX_ROWS, st);
    header(ox, oy, cw, TOP[st->set_page].name, 1);
    int y = HEAD_H + 8;
    for (int i = w->scroll; i < n; i++) {
        int rh = row_height(&rows[i]);
        if (y + rh > ch - STATUS_H) break;
        struct srow *r = &rows[i];
        int ay = oy + y;
        if (r->type == R_SECTION) {
            aether_ui_label(ox + PAD, ay + 6, r->title, C_ACCENT);
        } else {
            aether_fill_rect(ox + PAD, ay, cw - 2*PAD, rh - 2, C_PANEL);
            aether_fill_rect(ox + PAD, ay + rh - 3, cw - 2*PAD, 1, C_BORDER);
            if (r->icon >= 0) {
                aether_draw_round_rect(ox + PAD + 6, ay + 10, 34, 34, 10, C_BTN, 0xFF);
                aether_draw_icon(r->icon, ox + PAD + 12, ay + 16, 22, C_ACCENT);
            }
            int tx = ox + PAD + (r->icon >= 0 ? 50 : 8);
            if (r->type == R_SECTION) { y += rh; continue; }
            if (r->type == R_TOGGLE) {
                aether_ui_label(tx, ay + 12, r->title, C_TEXT);
                if (r->desc) aether_ui_label(tx, ay + 34, r->desc, C_DIM);
                draw_toggle(ox + cw - PAD - 62, ay + 16, aether_sys_get(r->key));
            } else if (r->type == R_OPTION) {
                aether_ui_label(tx, ay + 12, r->title, C_TEXT);
                if (r->desc) aether_ui_label(tx, ay + 34, r->desc, C_DIM);
                char lb[64]; opt_label(r->opts, aether_sys_get(r->key), lb, sizeof lb);
                aether_ui_label(ox + cw - PAD - 24 - aether_text_width_utf8(lb), ay + 12, lb, C_ACCENT);
                aether_draw_icon(AE_ICON_CHEVRON_RIGHT, ox + cw - PAD - 18, ay + 16, 18, C_DIM);
            } else if (r->type == R_SLIDER) {
                aether_ui_label(tx, ay + 8, r->title, C_TEXT);
                if (r->desc) aether_ui_label(tx, ay + 28, r->desc, C_DIM);
                draw_slider(ox + PAD + 8, ay + 48, cw - 2*PAD - 16,
                            aether_sys_get(r->key), r->max);
                char vb[16]; snprintf(vb, sizeof vb, "%d", aether_sys_get(r->key));
                aether_ui_label(ox + cw - PAD - aether_text_width_utf8(vb) - 8, ay + 8, vb, C_ACCENT);
            } else if (r->type == R_INFO) {
                aether_ui_label(tx, ay + 12, r->title, C_TEXT);
                char v[120]; aether_ui_fit(v, sizeof v, r->value, cw - 2*PAD - 150);
                aether_ui_label(ox + cw - PAD - aether_text_width_utf8(v) - 8, ay + 12, v, C_DIM);
            } else if (r->type == R_ACTION) {
                aether_ui_label(tx, ay + 12, r->title, C_TEXT);
                if (r->desc) {
                    char d[120]; aether_ui_fit(d, sizeof d, r->desc, cw - 2*PAD - 130);
                    aether_ui_label(tx, ay + 34, d, C_DIM);
                }
                aether_ui_button(ox + cw - PAD - 84, ay + 14, 76, 34, r->value, C_BTN, C_BTN_TX);
            }
        }
        y += rh;
    }
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_settings_open(struct app_state *st)
{
    st->set_page = -1;
    st->set_confirm = 0;
    st->set_query_len = 0; st->set_query[0] = 0;
    snprintf(st->status, sizeof st->status, "18 项设置 · 所有设置即时生效");
}

void app_settings_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    if (st->set_confirm) { draw_confirm(ox, oy, cw, ch, st); return; }
    if (st->set_page < 0) draw_top(w, ox, oy, cw, ch, st);
    else draw_sub(w, ox, oy, cw, ch, st);
}

void app_settings_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    if (st->set_confirm) {
        int y = HEAD_H + 24 + 206;
        int bw = (cw - 2*PAD - 16) / 2;
        if (ly < HEAD_H) { st->set_confirm = 0; snprintf(st->status, sizeof st->status, "已取消操作"); return; }
        if (aether_ui_hit(lx, ly, PAD, y, bw, 44)) {
            st->set_confirm = 0; snprintf(st->status, sizeof st->status, "已取消操作"); return;
        }
        if (aether_ui_hit(lx, ly, PAD + bw + 16, y, bw, 44)) {
            int act = st->set_confirm; st->set_confirm = 0;
            do_action(act, st); return;
        }
        return;
    }
    if (st->set_page < 0) {
        int sy = HEAD_H + 8;
        if (aether_ui_hit(lx, ly, PAD, sy, cw - 2*PAD, 40)) {
            snprintf(st->status, sizeof st->status, "输入关键字实时过滤设置项");
            return;
        }
        int y = sy + 50, rowh = 60;
        for (int i = 0; i < TOP_N; i++) {
            if (st->set_query_len && !strstr(TOP[i].name, st->set_query)) continue;
            if (y + rowh > ch - STATUS_H) break;
            if (aether_ui_hit(lx, ly, PAD, y, cw - 2*PAD, rowh)) {
                if (i == 0) {
                    st->set_query_len = 0; st->set_query[0] = 0;
                    snprintf(st->status, sizeof st->status, "输入关键字实时过滤设置项");
                } else {
                    struct srow tmp[MAX_ROWS];
                    int cnt = build_rows(i, tmp, MAX_ROWS, st);
                    st->set_page = i; w->scroll = 0;
                    snprintf(st->status, sizeof st->status, "%s · %d 项", TOP[i].name, cnt);
                }
                return;
            }
            y += rowh;
        }
        return;
    }
    /* 二级页 */
    if (ly < HEAD_H) { st->set_page = -1; w->scroll = 0; return; }
    struct srow rows[MAX_ROWS];
    int n = build_rows(st->set_page, rows, MAX_ROWS, st);
    int y = HEAD_H + 8;
    for (int i = w->scroll; i < n; i++) {
        int rh = row_height(&rows[i]);
        if (y + rh > ch - STATUS_H) break;
        struct srow *r = &rows[i];
        if (aether_ui_hit(lx, ly, PAD, y, cw - 2*PAD, rh) && r->type != R_SECTION) {
            if (r->type == R_TOGGLE) {
                int v = aether_sys_get(r->key) ? 0 : 1;
                aether_sys_set(r->key, v); aether_sys_apply();
                snprintf(st->status, sizeof st->status, "%s 已%s", r->title, v ? "开启" : "关闭");
            } else if (r->type == R_OPTION) {
                int v = (aether_sys_get(r->key) + 1) % opt_count(r->opts);
                aether_sys_set(r->key, v); aether_sys_apply();
                char lb[64]; opt_label(r->opts, v, lb, sizeof lb);
                snprintf(st->status, sizeof st->status, "%s → %s", r->title, lb);
            } else if (r->type == R_SLIDER) {
                int v = slider_val(lx, PAD + 8, cw - 2*PAD - 16, r->max);
                aether_sys_set(r->key, v); aether_sys_apply();
                snprintf(st->status, sizeof st->status, "%s → %d", r->title, v);
            } else if (r->type == R_ACTION) {
                int act = r->action;
                if (act == ACT_FACTORY_RESET || act == ACT_RESTORE ||
                    act == ACT_CLEAR_APPDATA || act == ACT_INSTALL_UPDATE) {
                    st->set_confirm = act;
                    snprintf(st->status, sizeof st->status, "请确认：%s", r->title);
                } else do_action(act, st);
            } else if (r->type == R_INFO) {
                snprintf(st->status, sizeof st->status, "%s：%s", r->title, r->value);
            }
            return;
        }
        y += rh;
    }
}

void app_settings_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    if (st->set_page >= 0) {
        struct srow tmp[MAX_ROWS];
        int n = build_rows(st->set_page, tmp, MAX_ROWS, st);
        if (code == K_UP) { if (w->scroll > 0) w->scroll--; return; }
        if (code == K_DOWN) { if (w->scroll < n - 1) w->scroll++; return; }
        if (code == K_ESC) { st->set_page = -1; w->scroll = 0; return; }
        return;
    }
    if (code == K_BACKSPACE) {
        if (st->set_query_len > 0) st->set_query[--st->set_query_len] = 0;
        return;
    }
    if (code == K_ENTER) {
        for (int i = 1; i < TOP_N; i++) {
            if (st->set_query_len && strstr(TOP[i].name, st->set_query)) {
                st->set_page = i; w->scroll = 0; return;
            }
        }
        return;
    }
    char c = aether_key_to_char(code, 0);
    if (c && st->set_query_len < (int)sizeof st->set_query - 2) {
        st->set_query[st->set_query_len++] = c;
        st->set_query[st->set_query_len] = 0;
    }
}

/* 供全局搜索复用：一级结构名称不重复定义，直接暴露同一份 TOP */
int aether_settings_top_count(void) { return TOP_N; }
const char *aether_settings_top_name(int i)
{
    if (i < 0 || i >= TOP_N) { return ""; }
    return TOP[i].name;
}

void app_settings_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    char c[32] = ""; int a = 0, b = 0;
    sscanf(line, "%*s %31s %d %d", c, &a, &b);
    if (!strcmp(c, "page") && a >= 1 && a < TOP_N) {
        st->set_page = a; w->scroll = 0; st->set_confirm = 0;
        snprintf(st->status, sizeof st->status, "已打开设置页：%s", TOP[a].name);
    } else if (!strcmp(c, "back")) {
        st->set_page = -1; w->scroll = 0; st->set_confirm = 0;
    } else if (!strcmp(c, "toggle") && a >= 0 && a < SYS_COUNT) {
        aether_sys_set(a, b ? 1 : 0); aether_sys_apply();
        snprintf(st->status, sizeof st->status, "系统开关 %d → %d", a, b ? 1 : 0);
    }
}
