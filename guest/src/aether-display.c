/*
 * aether-display.c - 统一显示后端实现（DRM 优先 + /dev/fb0 回退）
 *
 * Limbo / 标准 VGA 环境只有 fbdev，且帧缓冲可能是 8/16/24/32 bpp。
 * 本实现按 var 中真实的 RGB 位域做像素打包，绝不假设 32bpp，
 * 避免 memcpy 写越界、花屏或黑屏。
 */
#include "aether-display.h"
#ifndef AETHER_NO_DRM
#include "aether-drm.h"
#endif

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------- fbdev 后端私有数据 ---------- */
struct fb_priv {
    int fd;
    unsigned char *mmap;
    size_t mmap_sz;
    unsigned char *buf;   /* CPU 渲染缓冲（XRGB8888，width*4 每行） */
    uint32_t w, h;
    uint32_t stride;      /* fb 实际行字节数（可能含填充） */
    uint32_t bpp;         /* fb 每像素位数 */
    uint32_t cpp;         /* fb 每像素字节数 */
    int direct_xrgb;      /* bpp==32 且位域正好是 XRGB8888，可走 memcpy 快路径 */
    struct fb_bitfield r, g, b;
};

/* 把 8bit 通道值按 fb 位域（长度/偏移）放入原生像素值 */
static uint32_t chan(uint32_t v8, const struct fb_bitfield *f)
{
    uint32_t len = f->length ? f->length : 8;
    uint32_t sh = len >= 8 ? 0 : 8 - len;
    uint32_t m = len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1u);
    return ((v8 >> sh) & m) << f->offset;
}

/* 把渲染缓冲里的 0x00RRGGBB 打包成 fb 的原生像素值 */
static uint32_t pack_pixel(uint32_t px, const struct fb_priv *p)
{
    return chan((px >> 16) & 0xFF, &p->r) |
           chan((px >> 8) & 0xFF, &p->g) |
           chan(px & 0xFF, &p->b);
}

static void store_pixel(unsigned char *dst, uint32_t v, uint32_t cpp)
{
    for (uint32_t i = 0; i < cpp; i++)
        dst[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static int open_fbdev(struct aether_disp *d, const char *res)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return -1;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        close(fd);
        return -1;
    }

    /* 1) 先尝试把 fb 切到 32bpp：省掉逐像素转换（TCG 软模拟下很关键） */
    if (var.bits_per_pixel != 32) {
        struct fb_var_screeninfo want = var;
        want.bits_per_pixel = 32;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &want) == 0) {
            ioctl(fd, FBIOGET_VSCREENINFO, &var);
            ioctl(fd, FBIOGET_FSCREENINFO, &fix);
        }
    }

    /* 2) 可选：按 AETHER_RES 设定分辨率（失败则保留驱动默认值） */
    uint32_t w = var.xres, h = var.yres;
    if (res) {
        uint32_t rw = 0, rh = 0;
        if (sscanf(res, "%ux%u", &rw, &rh) == 2 && rw && rh) {
            struct fb_var_screeninfo want = var;
            want.xres = rw;
            want.yres = rh;
            want.xres_virtual = rw;
            want.yres_virtual = rh;
            if (ioctl(fd, FBIOPUT_VSCREENINFO, &want) == 0) {
                ioctl(fd, FBIOGET_VSCREENINFO, &var);
                ioctl(fd, FBIOGET_FSCREENINFO, &fix);
                w = var.xres;
                h = var.yres;
            }
        }
    }

    uint32_t bpp = var.bits_per_pixel ? var.bits_per_pixel : 8;
    uint32_t cpp = (bpp + 7) / 8;
    uint32_t stride = fix.line_length ? fix.line_length : w * cpp;
    if (!w || !h || stride < w * cpp) {
        close(fd);
        return -1;
    }
    size_t sz = (size_t)stride * h;
    unsigned char *mm = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mm == MAP_FAILED) {
        close(fd);
        return -1;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)w * 4 * h);
    if (!buf) {
        munmap(mm, sz);
        close(fd);
        return -1;
    }
    struct fb_priv *p = (struct fb_priv *)malloc(sizeof *p);
    if (!p) {
        free(buf);
        munmap(mm, sz);
        close(fd);
        return -1;
    }
    p->fd = fd;
    p->mmap = mm;
    p->mmap_sz = sz;
    p->buf = buf;
    p->w = w;
    p->h = h;
    p->stride = stride;
    p->bpp = bpp;
    p->cpp = cpp;
    p->r = var.red;
    p->g = var.green;
    p->b = var.blue;
    /* 32bpp 且 RGB 位域为 8:8:8、偏移 16/8/0 时，渲染缓冲字节序与 fb 完全一致 */
    p->direct_xrgb = (bpp == 32 &&
                      p->r.offset == 16 && p->r.length == 8 &&
                      p->g.offset == 8 && p->g.length == 8 &&
                      p->b.offset == 0 && p->b.length == 8);

    d->backend = 1;
    d->fd = -1;
    d->width = w;
    d->height = h;
    d->pitch = w * 4;
    d->flip_pending = 0;
    d->back = buf;
    d->front = buf;
    d->priv = p;
    return 0;
}

static int present_fbdev(struct aether_disp *d)
{
    struct fb_priv *p = (struct fb_priv *)d->priv;
    const unsigned char *src = d->back;
    unsigned char *dst = p->mmap;
    uint32_t w = p->w, h = p->h;

    if (p->direct_xrgb) {
        for (uint32_t y = 0; y < h; y++)
            memcpy(dst + (size_t)y * p->stride, src + (size_t)y * (w * 4), w * 4);
    } else {
        for (uint32_t y = 0; y < h; y++) {
            const unsigned *row = (const unsigned *)(src + (size_t)y * (w * 4));
            unsigned char *out = dst + (size_t)y * p->stride;
            for (uint32_t x = 0; x < w; x++)
                store_pixel(out + (size_t)x * p->cpp, pack_pixel(row[x], p), p->cpp);
        }
    }
    d->front = d->back;
    return 0;
}

/* ---------- 统一接口 ---------- */

int aether_disp_open(struct aether_disp *d, const char *res)
{
    memset(d, 0, sizeof *d);
    d->fd = -1;

#ifdef AETHER_NO_DRM
    /* 仅 fbdev：ARM64 / Limbo 目标无 guest 内 KMS，走帧缓冲(/dev/fb0)。
       编译期不链接 libdrm，规避 aarch64 缺静态 libdrm 的问题。 */
    if (open_fbdev(d, res) == 0)
        return 0;
    return -1;
#else

    /* 后端选择：AETHER_BACKEND=fbdev 优先走帧缓冲（尊重 AETHER_RES，
     * 适配 Limbo/标准 VGA 的竖屏分辨率）；=drm 仅走 KMS；默认 auto 先 DRM 后 fbdev。
     * fbdev 在失败（如内核无 fb0）时回退到 DRM，保证任意环境都能起显示。 */
    const char *be = getenv("AETHER_BACKEND");
    int prefer_fb = (be && !strcmp(be, "fbdev"));
    int force_drm = (be && !strcmp(be, "drm"));

    struct aether_drm *dr = (struct aether_drm *)malloc(sizeof *dr);

    if (!prefer_fb && !force_drm && dr) {
        if (aether_drm_open(dr, "/dev/dri/card0", res) == 0) {
            d->backend = 0;
            d->fd = dr->fd;
            d->width = dr->width;
            d->height = dr->height;
            d->pitch = dr->pitch;
            d->flip_pending = dr->flip_pending;
            d->back = (unsigned char *)aether_drm_back_buffer(dr);
            d->front = (unsigned char *)dr->map[1 - dr->back];
            d->priv = dr;
            return 0;
        }
        aether_drm_close(dr);
        free(dr);
        dr = NULL;
    } else if (force_drm && dr) {
        if (aether_drm_open(dr, "/dev/dri/card0", res) == 0) {
            d->backend = 0;
            d->fd = dr->fd;
            d->width = dr->width;
            d->height = dr->height;
            d->pitch = dr->pitch;
            d->flip_pending = dr->flip_pending;
            d->back = (unsigned char *)aether_drm_back_buffer(dr);
            d->front = (unsigned char *)dr->map[1 - dr->back];
            d->priv = dr;
            return 0;
        }
        aether_drm_close(dr);
        free(dr);
        dr = NULL;
    }

    /* 帧缓冲（/dev/fb0）：bochs/cirrus/vesa 等，支持 AETHER_RES 设定竖屏分辨率。
     * 若优先 fbdev但失败，则回退到 DRM（避免无显示）。 */
    if (open_fbdev(d, res) == 0)
        return 0;

    if (prefer_fb && dr == NULL) {
        /* fbdev 不可用，回退 DRM */
        dr = (struct aether_drm *)malloc(sizeof *dr);
        if (dr && aether_drm_open(dr, "/dev/dri/card0", res) == 0) {
            d->backend = 0;
            d->fd = dr->fd;
            d->width = dr->width;
            d->height = dr->height;
            d->pitch = dr->pitch;
            d->flip_pending = dr->flip_pending;
            d->back = (unsigned char *)aether_drm_back_buffer(dr);
            d->front = (unsigned char *)dr->map[1 - dr->back];
            d->priv = dr;
            return 0;
        }
        if (dr) { aether_drm_close(dr); free(dr); }
    }

#endif /* !AETHER_NO_DRM */
    return -1;
}

unsigned char *aether_disp_back(struct aether_disp *d)
{
#ifndef AETHER_NO_DRM
    if (d->backend == 0) {
        struct aether_drm *dr = (struct aether_drm *)d->priv;
        return (unsigned char *)aether_drm_back_buffer(dr);
    }
#endif
    return d->back;
}

int aether_disp_present(struct aether_disp *d)
{
#ifndef AETHER_NO_DRM
    if (d->backend == 0) {
        struct aether_drm *dr = (struct aether_drm *)d->priv;
        int rc = aether_drm_present(dr);
        d->flip_pending = dr->flip_pending;
        d->front = (unsigned char *)dr->map[1 - dr->back];
        return rc;
    }
#endif
    return present_fbdev(d);
}

int aether_disp_handle_events(struct aether_disp *d)
{
#ifndef AETHER_NO_DRM
    if (d->backend == 0) {
        struct aether_drm *dr = (struct aether_drm *)d->priv;
        int rc = aether_drm_handle_events(dr);
        d->flip_pending = dr->flip_pending;
        d->back = (unsigned char *)aether_drm_back_buffer(dr);
        d->front = (unsigned char *)dr->map[1 - dr->back];
        return rc;
    }
#endif
    return 0;
}

void aether_disp_close(struct aether_disp *d)
{
    /* headless / 无显示后端：aether_disp_open 失败，priv 为 NULL、backend
     * 保持默认 0，绝不能按 DRM 后端去解引用/释放 NULL，否则关闭时 SIGSEGV。 */
    if (d->priv == NULL) {
        memset(d, 0, sizeof *d);
        d->fd = -1;
        return;
    }
#ifndef AETHER_NO_DRM
    if (d->backend == 0) {
        struct aether_drm *dr = (struct aether_drm *)d->priv;
        aether_drm_close(dr);
        free(dr);
        memset(d, 0, sizeof *d);
        d->fd = -1;
        return;
    }
#endif
    if (d->backend == 1) {
        struct fb_priv *p = (struct fb_priv *)d->priv;
        if (p) {
            munmap(p->mmap, p->mmap_sz);
            close(p->fd);
            free(p->buf);
            free(p);
        }
    }
    memset(d, 0, sizeof *d);
    d->fd = -1;
}
