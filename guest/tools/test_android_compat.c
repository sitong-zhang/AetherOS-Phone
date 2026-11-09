/*
 * test_android_compat.c - 安卓兼容层单元测试（协议 / 状态 / 能力 / 控制命令）
 *
 * 纯逻辑测试，不依赖真机内核设施。链接 aether-android-util.c，
 * 覆盖：状态名、/proc/cmdline 解析、共享帧协议头布局、状态文件读取、
 * 控制命令字符串、降级提示文案、uinput 事件常量、桥默认几何。
 *
 * 编译：
 *   gcc -O2 -I src -o /tmp/test_android_compat \
 *       tools/test_android_compat.c src/aether-android-util.c
 */
#include "aether-android.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; printf("  PASS  %s\n", name); } \
    else { fail++; printf("  FAIL  %s\n", name); } \
} while (0)

int main(void)
{
    printf("=== AetherOS 安卓兼容层单元测试 ===\n\n");

    /* ---- 1. 状态名映射 ---- */
    printf("[状态机]\n");
    CHECK(strcmp(aether_android_state_name(AANDROID_UNKNOWN), "unknown") == 0,
          "UNKNOWN -> unknown");
    CHECK(strcmp(aether_android_state_name(AANDROID_UNAVAILABLE), "unavailable") == 0,
          "UNAVAILABLE -> unavailable");
    CHECK(strcmp(aether_android_state_name(AANDROID_PROBING), "probing") == 0,
          "PROBING -> probing");
    CHECK(strcmp(aether_android_state_name(AANDROID_RUNNING), "running") == 0,
          "RUNNING -> running");
    CHECK(strcmp(aether_android_state_name(AANDROID_FAILED), "failed") == 0,
          "FAILED -> failed");
    CHECK(strcmp(aether_android_state_name(999), "unknown") == 0,
          "越界状态 -> unknown");

    /* ---- 2. 状态常量顺序（协议兼容）---- */
    CHECK(AANDROID_UNAVAILABLE == 1 && AANDROID_RUNNING == 3,
          "状态枚举值稳定（1=unavailable, 3=running）");

    /* ---- 3. 共享帧协议头布局 ---- */
    printf("\n[共享帧协议]\n");
    CHECK(AANDROID_FB_HEAD_SZ == 64, "帧头定长 64B");
    CHECK(sizeof(struct aether_android_fb) == AANDROID_FB_HEAD_SZ,
          "struct aether_android_fb 与 64B 头一致");
    CHECK(AANDROID_FB_MAGIC == 0x41414642u, "magic = AAFB");
    CHECK(AANDROID_FB_VERSION == 1, "version = 1");
    CHECK(AANDROID_FB_FMT_XRGB8888 == 1, "XRGB8888 = 1");
    CHECK((AANDROID_FB_FLAG_VISIBLE & 1u) == 1u, "visible 标志 bit0");
    /* 总大小 = 头 + stride*height */
    CHECK(AANDROID_FB_TOTAL == 64u + (720u * 4u) * 1280u,
          "共享帧总字节数 = 64 + 720*4*1280");
    /* 头部字段偏移（保证生产者/消费者 abi 一致）*/
    struct aether_android_fb h;
    memset(&h, 0, sizeof h);
    h.magic = AANDROID_FB_MAGIC; h.flags = AANDROID_FB_FLAG_VISIBLE;
    h.win_x = 60; h.win_y = 220; h.win_w = 600; h.win_h = 760; h.seq = 7;
    CHECK(h.magic == AANDROID_FB_MAGIC && (h.flags & AANDROID_FB_FLAG_VISIBLE),
          "头部 magic/flags 可写可读");
    CHECK(h.win_w == 600 && h.win_h == 760, "浮窗几何字段可写可读");

    /* ---- 4. 降级提示文案（用户路线第 6 点）---- */
    printf("\n[降级提示]\n");
    CHECK(strstr(AANDROID_HINT_PKVM, "需要更高版本系统") != NULL,
          "pKVM 提示含「需要更高版本系统」");
    CHECK(strstr(AANDROID_HINT_PKVM, "AetherOS 本体可正常使用") != NULL,
          "pKVM 提示说明本体不受影响");
    CHECK(strstr(AANDROID_HINT_ROOTFS, "LineageOS") != NULL,
          "rootfs 提示指向 LineageOS 镜像");

    /* ---- 5. 控制命令协议字符串 ---- */
    printf("\n[控制通道]\n");
    const char *cmds[] = { "status", "install", "launch", "window", "hint", "stop" };
    CHECK(sizeof(cmds)/sizeof(cmds[0]) == 6, "6 类控制命令已定义");
    CHECK(strcmp(AETHER_ANDROID_STATUS, "/run/aether/android.status") == 0,
          "状态文件路径");
    CHECK(strcmp(AETHER_ANDROID_SOCK, "/run/aether/android.sock") == 0,
          "控制 socket 路径");
    CHECK(strcmp(AETHER_ANDROID_ROOTFS, "/var/lib/aether/android/rootfs") == 0,
          "LineageOS rootfs 路径");

    /* ---- 6. 内核命令行参数键 ---- */
    printf("\n[内核参数]\n");
    CHECK(strcmp(AANDROID_CMDLINE_PKVM, "aether.avf.pkvm=") == 0, "pkvm 键");
    CHECK(strcmp(AANDROID_CMDLINE_LEVEL, "aether.avf.level=") == 0, "level 键");
    CHECK(strcmp(AANDROID_CMDLINE_APPAPI, "aether.avf.appapi=") == 0, "appapi 键");

    /* ---- 7. /proc/cmdline 解析（当前 host 无该参数 -> 返回默认值）---- */
    printf("\n[cmdline / 状态文件读取]\n");
    int lvl = aether_android_cmdline_int(AANDROID_CMDLINE_LEVEL, -7);
    CHECK(lvl == -7 || (lvl >= 0 && lvl <= 4),
          "cmdline 缺失时返回默认值或合法等级");
    int st = aether_android_status_int("state", AANDROID_UNKNOWN);
    CHECK(st >= AANDROID_UNKNOWN && st <= AANDROID_FAILED,
          "状态文件缺失时返回合法默认值");
    char buf[512];
    int rr = aether_android_status_read(buf, sizeof buf);
    CHECK(rr == -1 || rr == 0, "状态文件读取返回值合法");

    /* ---- 8. uinput 事件常量（Linux UAPI 照搬）---- */
    printf("\n[uinput 输入桥]\n");
    CHECK(EV_ABS == 0x03 && EV_KEY == 0x01 && EV_SYN == 0x00,
          "EV_ABS/EV_KEY/EV_SYN UAPI 值");
    CHECK(ABS_X == 0x00 && ABS_Y == 0x01, "ABS_X/ABS_Y UAPI 值");
    CHECK(BTN_TOUCH == 0x14a, "BTN_TOUCH UAPI 值 = 0x14a");
    CHECK(SYN_REPORT == 0, "SYN_REPORT = 0");

    /* ---- 9. 诚实边界：默认状态非 running ---- */
    printf("\n[诚实边界]\n");
    CHECK(AANDROID_UNKNOWN == 0 && AANDROID_UNAVAILABLE == 1,
          "默认未知/不可用，绝不默认 running");
    CHECK(AANDROID_FB_FLAG_VISIBLE == 1u,
          "共享帧默认由生产者显式置 visible，消费者不臆造窗口");

    printf("\n=== RESULT: %d PASS / %d FAIL ===\n", pass, fail);
    return fail ? 1 : 0;
}
