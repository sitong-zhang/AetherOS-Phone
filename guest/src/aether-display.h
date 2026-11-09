/*
 * aether-display.h - AetherOS 统一显示后端
 *
 * 抽象出「一块可上屏的像素缓冲」：调用方渲染到 back 缓冲，
 * present() 把缓冲上屏。后端自动选择：
 *   - DRM（/dev/dri/card0，virtio-gpu / bochs 等现代驱动）
 *   - fbdev 回退（/dev/fb0，Limbo 等只提供标准 VGA 帧缓冲的环境）
 * 这样同一套合成器既能在桌面 QEMU（virtio-gpu）跑，也能在
 * Android Limbo 模拟器（标准 VGA /dev/fb0）跑，无需改渲染代码。
 */
#ifndef AETHER_DISPLAY_H
#define AETHER_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

struct aether_disp {
    int backend;             /* 0 = DRM, 1 = fbdev */
    int fd;                 /* poll 用 fd（DRM 为 drm.fd；fbdev 为 -1） */
    uint32_t width, height; /* 像素分辨率 */
    uint32_t pitch;         /* back 缓冲每行字节数（= width*4） */
    int flip_pending;       /* 是否有翻页在途（仅 DRM） */
    unsigned char *back;    /* 当前可渲染缓冲（XRGB8888，小端） */
    unsigned char *front;   /* 最近一次上屏的帧（供截图读取） */
    void *priv;             /* 后端私有对象 */
};

/* 打开显示。先试 DRM，失败回退 fbdev。
 * res 形如 "1280x720" 可强制分辨率（fbdev 会尝试设置），NULL 取默认。 */
int aether_disp_open(struct aether_disp *d, const char *res);

/* 当前可渲染缓冲地址 */
unsigned char *aether_disp_back(struct aether_disp *d);

/* 把 back 缓冲提交上屏（fbdev 立即生效；DRM 走翻页） */
int aether_disp_present(struct aether_disp *d);

/* 当 poll 到 fd 可读时调用（DRM 翻页完成事件）；fbdev 为 no-op */
int aether_disp_handle_events(struct aether_disp *d);

void aether_disp_close(struct aether_disp *d);

#endif /* AETHER_DISPLAY_H */
