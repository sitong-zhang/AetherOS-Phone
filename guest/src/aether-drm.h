/*
 * aether-drm.h - AetherOS 的 KMS/DRM 显示后端（原子提交 + 双缓冲）
 *
 * 抽象出一张可翻页的显示平面：调用方渲染到 back_buffer，
 * 然后 present() 把该缓冲翻到屏幕；翻页完成事件通过 handle_events() 回收。
 * 采用 DRM 原子接口（virtio-gpu 等现代驱动支持），与手机基线无共享代码。
 */
#ifndef AETHER_DRM_H
#define AETHER_DRM_H

#include <stddef.h>
#include <stdint.h>

struct aether_drm {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; /* 每行字节数 */
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t fb[2];
    void *map[2];
    size_t size[2];
    int back; /* 当前可渲染缓冲下标（0 或 1） */
    int flip_pending;
};

/* 打开并初始化显示。dev 通常为 "/dev/dri/card0"；
 * res 形如 "1280x720" 可强制分辨率，NULL 则取连接器首模式。 */
int aether_drm_open(struct aether_drm *d, const char *dev, const char *res);

/* 返回当前 back（可渲染）缓冲的映射地址（XRGB8888，小端 RGBA 内存序） */
void *aether_drm_back_buffer(struct aether_drm *d);

/* 把 back 缓冲提交为下一次翻页；翻页完成前不可再次渲染到 back */
int aether_drm_present(struct aether_drm *d);

/* 处理翻页完成事件（需在 poll 到 drm.fd 可读时调用），完成后 back 自动交换 */
int aether_drm_handle_events(struct aether_drm *d);

void aether_drm_close(struct aether_drm *d);

#endif /* AETHER_DRM_H */
