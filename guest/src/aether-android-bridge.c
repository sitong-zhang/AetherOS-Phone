#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * aether-android-bridge.c - 图形桥 + 输入桥（编译进 aether-compositor）
 *
 * 用户技术路线第 3/4 点：
 *   图形桥：容器内 SurfaceFlinger 把一帧写进共享内存（AETHER_ANDROID_FB），
 *           AetherOS compositor 把它当「一张纹理」画进桌面浮窗，安卓 App
 *           以窗口形式浮在 AetherOS 桌面里，不抢占整个屏幕。
 *   输入桥：AetherOS 自己读 /dev/input；触点落在浮窗区域时，用 uinput
 *           虚拟设备把 ABS_X/ABS_Y/BTN_TOUCH 喂给容器。
 *
 * 上游依据：
 *   - Waydroid platform_hardware_wayland hwcomposer（共享内存 buffer 回退路径，
 *     memfd + wl_shm）；本实现是纯 C framebuffer 等价（mmap 文件 + blit）。
 *   - Linux UAPI linux/uinput.h：UI_SET_EVBIT / UI_SET_ABSBIT / UI_SET_KEYBIT /
 *     UI_DEV_CREATE / UI_DEV_DESTROY，struct uinput_setup / input_event。
 *
 * 【诚实边界】共享帧的生产者是容器内的 AOSP 图形 HAL；当前 rootfs 未随盘
 * 分发，故 seq 恒为 0、flags 不可见，浮窗不绘制，AetherOS 桌面完全不受影响。
 * uinput 设备只在 /dev/uinput 存在时创建；缺失则注入静默降级。桥的每一条
 * 路径都可在真机验证（无容器时零副作用），不假称已有真实安卓窗口。
 */
#include "aether-android.h"
#include "aether-render.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void          *g_fb = NULL;      /* mmap 的共享帧 */
static int            g_fb_fd = -1;
static struct aether_android_fb *g_hdr = NULL;
static uint32_t       g_last_seq = 0xFFFFFFFFu;
static int            g_uinput = -1;

/* 浮窗几何（画布坐标）：默认居中偏下，不覆盖状态栏与 Dock */
static int g_win_x = 60, g_win_y = 220, g_win_w = 600, g_win_h = 760;

static void ensure_fb(void)
{
    if (g_fb) return;
    g_fb_fd = open(AETHER_ANDROID_FB, O_RDWR | O_CREAT, 0666);
    if (g_fb_fd < 0) return;
    if (ftruncate(g_fb_fd, (off_t)AANDROID_FB_TOTAL) != 0) {
        close(g_fb_fd); g_fb_fd = -1; return;
    }
    g_fb = mmap(NULL, AANDROID_FB_TOTAL, PROT_READ | PROT_WRITE,
                MAP_SHARED, g_fb_fd, 0);
    if (g_fb == MAP_FAILED) {
        g_fb = NULL; close(g_fb_fd); g_fb_fd = -1; return;
    }
    g_hdr = (struct aether_android_fb *)g_fb;
    /* 只初始化一次；生产者（容器）负责之后写 magic/seq/像素 */
    if (g_hdr->magic != AANDROID_FB_MAGIC) {
        memset(g_hdr, 0, AANDROID_FB_HEAD_SZ);
        g_hdr->magic   = AANDROID_FB_MAGIC;
        g_hdr->version = AANDROID_FB_VERSION;
        g_hdr->width   = AANDROID_FB_MAX_W;
        g_hdr->height  = AANDROID_FB_MAX_H;
        g_hdr->stride  = AANDROID_FB_MAX_W * 4;
        g_hdr->format  = AANDROID_FB_FMT_XRGB8888;
        g_hdr->flags   = 0;   /* 默认不可见：容器未运行时零副作用 */
        g_hdr->seq     = 0;
        g_hdr->win_x = g_win_x; g_hdr->win_y = g_win_y;
        g_hdr->win_w = g_win_w; g_hdr->win_h = g_win_h;
    }
}

static void ensure_uinput(void)
{
    if (g_uinput >= 0) return;
    const char *paths[] = { AETHER_ANDROID_UINPUT, "/dev/input/uinput", NULL };
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_WRONLY | O_NONBLOCK);
        if (fd < 0) continue;
        /* Waydroid fake evdev：绝对坐标触摸屏（ABS_X/ABS_Y + BTN_TOUCH） */
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_EVBIT, EV_SYN);
        ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
        ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ioctl(fd, UI_SET_ABSBIT, ABS_Y);

        struct uinput_abs_setup abs;
        memset(&abs, 0, sizeof(abs));
        abs.code = ABS_X; abs.absinfo.minimum = 0;
        abs.absinfo.maximum = AANDROID_FB_MAX_W - 1;
        ioctl(fd, UI_ABS_SETUP, &abs);
        abs.code = ABS_Y; abs.absinfo.maximum = AANDROID_FB_MAX_H - 1;
        ioctl(fd, UI_ABS_SETUP, &abs);

        struct uinput_setup us;
        memset(&us, 0, sizeof(us));
        us.id.bustype = BUS_VIRTUAL;
        us.id.vendor  = 0x1ae7;   /* Aether */
        us.id.product = 0x0001;
        us.id.version = 1;
        strncpy(us.name, "aether-android-touch", sizeof(us.name) - 1);
        us.ff_effects_max = 0;
        if (ioctl(fd, UI_DEV_SETUP, &us) == 0 && ioctl(fd, UI_DEV_CREATE) == 0) {
            g_uinput = fd;
            return;
        }
        close(fd);
    }
    /* /dev/uinput 不存在是预期降级：AetherOS 本体照常用 */
}

void aether_android_bridge_init(void)
{
    ensure_fb();
    ensure_uinput();
}

void aether_android_bridge_shutdown(void)
{
    if (g_uinput >= 0) { ioctl(g_uinput, UI_DEV_DESTROY); close(g_uinput); g_uinput = -1; }
    if (g_fb) { munmap(g_fb, AANDROID_FB_TOTAL); g_fb = NULL; g_hdr = NULL; }
    if (g_fb_fd >= 0) { close(g_fb_fd); g_fb_fd = -1; }
}

int aether_android_bridge_window(int *x, int *y, int *w, int *h)
{
    ensure_fb();
    if (g_hdr && (g_hdr->flags & AANDROID_FB_FLAG_VISIBLE)) {
        g_win_x = g_hdr->win_x; g_win_y = g_hdr->win_y;
        g_win_w = g_hdr->win_w; g_win_h = g_hdr->win_h;
    } else if (g_hdr && g_hdr->win_w > 0) {
        g_win_x = g_hdr->win_x; g_win_y = g_hdr->win_y;
        g_win_w = g_hdr->win_w; g_win_h = g_hdr->win_h;
    }
    if (x) *x = g_win_x; if (y) *y = g_win_y;
    if (w) *w = g_win_w; if (h) *h = g_win_h;
    return (g_hdr && (g_hdr->flags & AANDROID_FB_FLAG_VISIBLE)) ? 1 : 0;
}

/* 把共享帧（stride*h XRGB8888）缩放画进浮窗矩形，并画玻璃边框/标题栏。
 * 用 aether_blit_rect 逐行缩放；容器未运行（flags 不可见）时直接返回 0。 */
int aether_android_bridge_render(void)
{
    ensure_fb();
    if (!g_hdr) return 0;
    if (g_hdr->magic != AANDROID_FB_MAGIC) return 0;
    if (!(g_hdr->flags & AANDROID_FB_FLAG_VISIBLE)) return 0;
    uint32_t seq = g_hdr->seq;
    if (seq == g_last_seq && seq != 0) return 1;   /* 内容未变，仍保持窗口可见 */
    g_last_seq = seq;

    int x, y, w, h;
    aether_android_bridge_window(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    uint32_t fw = g_hdr->width, fh = g_hdr->height, stride = g_hdr->stride;
    if (fw == 0 || fh == 0 || stride < fw * 4) return 0;
    const unsigned char *pix =
        (const unsigned char *)g_fb + AANDROID_FB_HEAD_SZ;

    /* 浮窗玻璃底板（导航/浮层允许玻璃；内容层是安卓自己的像素，不折射） */
    aether_fill_rect_alpha(x - 6, y - 34, w + 12, h + 40, 0x0A1428u, 0xD8);
    aether_draw_round_rect(x - 6, y - 34, w + 12, h + 40, 18, 0x1A2438u, 0xE6);
    aether_draw_border(x - 6, y - 34, w + 12, h + 40, 0x5A6E96u, 1);
    /* 标题栏：明确「安卓应用」窗口，不与 AetherOS 桌面混淆 */
    aether_fill_rect_alpha(x - 6, y - 34, w + 12, 30, 0x182238u, 0xE0);
    aether_draw_text_utf8(x + 8, y - 27, "安卓应用窗口", 0xE8EEFAu);

    /* 逐行把容器帧缩放贴进内容区（最近邻，TCG 上恒定开销） */
    for (int row = 0; row < h; row++) {
        int sy = (int)((uint64_t)row * fh / (uint32_t)h);
        const unsigned char *src = pix + (size_t)sy * stride;
        for (int col = 0; col < w; col++) {
            int sx = (int)((uint64_t)col * fw / (uint32_t)w);
            unsigned px = ((const unsigned *)src)[sx];
            aether_blend_pixel(x + col, y + row, px & 0xFFFFFFu, 0xFF);
        }
    }
    /* 亮边（镜面高光，与全系统控件语言一致） */
    aether_fill_rect_alpha(x, y, w, 1, 0xFFFFFFu, 0x30);
    return 1;
}

/* 触点落在浮窗内：把画布坐标映射回容器帧坐标，用 uinput 注入。
 * 返回 1 表示该事件被安卓窗口消费（AetherOS 桌面不再处理）。 */
int aether_android_bridge_input(int cx, int cy, int down)
{
    ensure_fb();
    ensure_uinput();
    int x, y, w, h;
    int visible = aether_android_bridge_window(&x, &y, &w, &h);
    if (!visible) return 0;                 /* 容器未运行：完全不拦截桌面输入 */
    if (cx < x || cx >= x + w || cy < y || cy >= y + h) return 0;
    if (g_uinput < 0) return 1;             /* 命中窗口但内核无 uinput：静默降级 */

    uint32_t fw = g_hdr->width ? g_hdr->width : AANDROID_FB_MAX_W;
    uint32_t fh = g_hdr->height ? g_hdr->height : AANDROID_FB_MAX_H;
    int tx = (int)((uint64_t)(cx - x) * fw / (uint32_t)w);
    int ty = (int)((uint64_t)(cy - y) * fh / (uint32_t)h);

    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_ABS; ev.code = ABS_X; ev.value = tx;
    (void)write(g_uinput, &ev, sizeof(ev));
    ev.code = ABS_Y; ev.value = ty;
    (void)write(g_uinput, &ev, sizeof(ev));
    ev.type = EV_KEY; ev.code = BTN_TOUCH; ev.value = down ? 1 : 0;
    (void)write(g_uinput, &ev, sizeof(ev));
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    (void)write(g_uinput, &ev, sizeof(ev));
    return 1;
}
