/*
 * aether-android.h - AetherOS 安卓兼容层：共享协议 / 状态 / 控制通道
 *
 * 技术路线（用户规定，工程化落地）：
 *   外层 AVF / pKVM（vm 工程探测，AetherOS 本体仍走已验证的 QEMU TCG）；
 *   内层 LXC 式容器：unshare + chroot 跑 LineageOS arm64 rootfs 共享 AetherOS 内核。
 *   图形桥：容器内 SurfaceFlinger 输出写共享内存帧，compositor 当纹理画成桌面浮窗；
 *   输入桥：AetherOS 读 /dev/input，落在浮窗区域时用 uinput 虚拟设备喂给容器。
 *
 * 上游照搬（见 third_party/android-compat/）：
 *   - Waydroid tools/helpers/lxc.py / drivers.py（GPL-3.0）：容器命名空间、binderfs、
 *     devpts、设备 bind mount 的挂载序列与检测口径；
 *   - Linux UAPI linux/uinput.h、input-event-codes.h（GPL-2.0 WITH Linux-syscall-note）；
 *   - AOSP Virtualization README（Apache-2.0）：pKVM/AVF 能力检测。
 *
 * 【诚实边界】LineageOS rootfs 是 GB 级外部镜像，不在 APK / 18MB 系统盘内；
 * 客机内核未必编入 CONFIG_ANDROID_BINDERFS / CONFIG_INPUT_UINPUT；容器内
 * SurfaceFlinger 的 hwcomposer HAL 也需 AOSP 侧配套。因此本层在依赖缺失时
 * 明确进入 unavailable 并给出「需要更高版本系统」提示，AetherOS 本体照常用，
 * 绝不假称「百万 APK 已能打开」。所有降级均有真机可验证的状态输出。
 */
#ifndef AETHER_ANDROID_H
#define AETHER_ANDROID_H

#include <stdint.h>
#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 路径（客机 /run 为 tmpfs，服务与合成器共享）---- */
#define AETHER_ANDROID_STATUS   "/run/aether/android.status"
#define AETHER_ANDROID_SOCK     "/run/aether/android.sock"
#define AETHER_ANDROID_FB       "/run/aether/android.fb"   /* mmap 共享帧 */
#define AETHER_ANDROID_ROOTFS   "/var/lib/aether/android/rootfs"
#define AETHER_ANDROID_UINPUT   "/dev/uinput"
#define AETHER_ANDROID_BINDERFS "/dev/binderfs"

/* ---- 兼容层状态 ---- */
enum aether_android_state {
    AANDROID_UNKNOWN     = 0,
    AANDROID_UNAVAILABLE = 1,  /* pKVM 不可用或依赖缺失：给「需要更高版本系统」提示 */
    AANDROID_PROBING     = 2,  /* 正在挂载 / 拉起容器 */
    AANDROID_RUNNING     = 3,  /* 容器 /init 已拉起，桥可用 */
    AANDROID_FAILED      = 4   /* 启动失败（有明确 errno / 原因） */
};

const char *aether_android_state_name(int state);

/* ---- 共享帧缓冲协议（Waydroid hwcomposer 共享内存等价物）----
 * 生产者=容器内图形 HAL，消费者=AetherOS compositor。
 * 头部定长 64B，之后紧跟 stride*height 字节 XRGB8888 像素。
 * seq 由生产者递增，消费者只在 seq 变化时重绘，避免逐帧全屏拷贝。 */
#define AANDROID_FB_MAGIC   0x41414642u   /* "AAFB" */
#define AANDROID_FB_VERSION 1u
#define AANDROID_FB_FMT_XRGB8888 1u
#define AANDROID_FB_FLAG_VISIBLE 0x1u
#define AANDROID_FB_MAX_W   720
#define AANDROID_FB_MAX_H   1280
#define AANDROID_FB_HEAD_SZ 64u

struct aether_android_fb {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;        /* 生产者递增 */
    uint32_t width;
    uint32_t height;
    uint32_t stride;     /* 每行字节数 */
    uint32_t format;     /* AANDROID_FB_FMT_XRGB8888 */
    uint32_t flags;      /* bit0 = 浮窗可见 */
    int32_t  win_x, win_y, win_w, win_h;  /* 桌面浮窗几何（画布坐标） */
    uint32_t reserved[4];                 /* 补齐到 64B：32 + 16 + 16 */
};

/* 共享帧总字节数 */
#define AANDROID_FB_TOTAL \
    (AANDROID_FB_HEAD_SZ + (AANDROID_FB_MAX_W * 4) * AANDROID_FB_MAX_H)

/* ---- 内核命令行参数（由 vm 层 AvfCapability.kernelArgs 注入）---- */
#define AANDROID_CMDLINE_PKVM   "aether.avf.pkvm="
#define AANDROID_CMDLINE_LEVEL  "aether.avf.level="
#define AANDROID_CMDLINE_APPAPI "aether.avf.appapi="
#define AANDROID_CMDLINE_NAME   "aether.avf.name="

/* 从 /proc/cmdline 读取一个 aether.avf.* 整数参数；缺失返回 def */
int aether_android_cmdline_int(const char *key, int def);
/* 从 key=value 状态文件读取整数值；缺失或解析失败返回 def */
int aether_android_status_int(const char *key, int def);
/* 读取完整状态到缓冲（key=value 文本）；成功返回 0 */
int aether_android_status_read(char *buf, size_t sz);

/* ---- 控制通道命令（文本行协议，AF_UNIX 流式）----
 *  "status\n"              -> "state=<n> pkvm=<0|1> ...\n"
 *  "install <apk-path>\n"  -> 容器内 pm install；不可用返回明确原因
 *  "launch <package>\n"    -> 容器内 am start；不可用返回明确原因
 *  "hint\n"                -> 面向用户的降级提示文本（UTF-8）
 *  "window x y w h\n"      -> 设置浮窗几何
 *  "stop\n"                -> 关闭服务
 */
#define AANDROID_HINT_PKVM  "安卓兼容层需要更高版本系统（pKVM / AVF 不可用），AetherOS 本体可正常使用"
#define AANDROID_HINT_ROOTFS "未安装安卓运行时镜像（LineageOS arm64 rootfs），请先在系统设置中安装"

/* ---- compositor 侧桥接 API（链接进 aether-compositor）---- */
/* 每帧调用：若共享帧可见且 seq 变化，把 buffer 画成桌面浮窗；返回 1 表示画了。 */
int aether_android_bridge_render(void);
/* 输入命中：触点落在浮窗内返回 1，并通过 uinput 注入容器（若设备可用）。 */
int aether_android_bridge_input(int x, int y, int down);
/* 浮窗当前几何（供命中测试）；不可见时返回 0 */
int aether_android_bridge_window(int *x, int *y, int *w, int *h);
/* 一次性初始化桥（打开/创建共享帧与 uinput） */
void aether_android_bridge_init(void);
void aether_android_bridge_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* AETHER_ANDROID_H */
