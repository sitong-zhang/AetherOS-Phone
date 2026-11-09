/*
 * aether-drm.c - AetherOS 的 KMS/DRM 显示后端实现
 *
 * 采用 DRM 原子接口完成模式设置（modeset）与翻页（page flip）。
 * 双缓冲：两个 dumb 帧缓冲轮流作为 front/back，present 触发原子翻页，
 * 翻页完成事件通过 handle_events() 回收并交换 back 指针。
 *
 * 显示后端实现，供 host 取证构建与客机共用；已修正前次移植中
 * poll 数组越界、跳过 IPC 轮询等结构性缺陷。
 */
#include "aether-drm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_FORMAT_XRGB8888_fourcc 0x34325258u /* 'X' 'R' '2' '4' */

static uint32_t get_prop_id(int fd, uint32_t obj, uint32_t obj_type, const char *name)
{
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, obj_type);
    if (!props)
        return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, name) == 0)
            id = p->prop_id;
        drmModeFreeProperty(p);
        if (id)
            break;
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static int make_dumb(struct aether_drm *d, int idx, uint32_t w, uint32_t h)
{
    uint32_t handle = 0, pitch = 0;
    uint64_t size = 0;
    if (drmModeCreateDumbBuffer(d->fd, w, h, 32, 0, &handle, &pitch, &size) != 0)
        return -1;
    d->size[idx] = (size_t)size;
    d->pitch = pitch;

    uint32_t handles[4] = {handle, 0, 0, 0};
    uint32_t pitches[4] = {pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(d->fd, w, h, DRM_FORMAT_XRGB8888_fourcc,
                      handles, pitches, offsets, &d->fb[idx], 0) != 0)
        return -1;

    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(d->fd, handle, &offset) != 0)
        return -1;
    d->map[idx] = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)offset);
    if (d->map[idx] == MAP_FAILED)
        return -1;
    return 0;
}

static void on_page_flip(int fd, unsigned int seq, unsigned int tv_sec,
                         unsigned int tv_usec, void *user)
{
    (void)fd;
    (void)seq;
    (void)tv_sec;
    (void)tv_usec;
    struct aether_drm *d = (struct aether_drm *)user;
    d->flip_pending = 0;
    d->back ^= 1; /* 翻页完成，交换 front/back */
}

int aether_drm_open(struct aether_drm *d, const char *dev, const char *res)
{
    memset(d, 0, sizeof *d);
    d->fd = open(dev, O_RDWR);
    if (d->fd < 0)
        return -1;
    if (drmSetClientCap(d->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        /* 没有原子能力也尝试继续（部分驱动仅 legacy） */
        fprintf(stderr, "aether-drm: 警告 无原子能力\n");
    }

    drmModeResPtr res0 = drmModeGetResources(d->fd);
    if (!res0)
        return -1;

    /* 找第一个已连接连接器 */
    drmModeConnectorPtr conn = NULL;
    for (int i = 0; i < res0->count_connectors; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(d->fd, res0->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        if (c)
            drmModeFreeConnector(c);
    }
    if (!conn)
        return -1;
    d->conn_id = conn->connector_id;

    /* 解析分辨率（可选） */
    uint32_t want_w = 0, want_h = 0;
    if (res) {
        unsigned w, h;
        if (sscanf(res, "%ux%u", &w, &h) == 2 || sscanf(res, "%ux%u", &w, &h) == 2) {
            want_w = w;
            want_h = h;
        }
    }

    /* 选择模式 */
    drmModeModeInfo *mode = NULL;
    for (int i = 0; i < conn->count_modes; i++) {
        if (want_w && (uint32_t)conn->modes[i].hdisplay == want_w &&
            (uint32_t)conn->modes[i].vdisplay == want_h) {
            mode = &conn->modes[i];
            break;
        }
    }
    if (!mode)
        mode = &conn->modes[0];
    d->width = mode->hdisplay;
    d->height = mode->vdisplay;

    /* 选 CRTC：优先连接器当前 encoder 指向的 crtc */
    uint32_t crtc_id = 0;
    int crtc_index = -1;
    if (conn->encoder_id) {
        drmModeEncoderPtr enc = drmModeGetEncoder(d->fd, conn->encoder_id);
        if (enc) {
            crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    if (!crtc_id && res0->count_crtcs > 0) {
        crtc_id = res0->crtcs[0];
    }
    /* 计算 crtc 在资源表中的索引（用于 possible_crtcs 位掩码） */
    for (int i = 0; i < res0->count_crtcs; i++) {
        if (res0->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }
    d->crtc_id = crtc_id;

    /* 选主平面（PRIMARY），且覆盖当前 crtc */
    drmModePlaneResPtr pres = drmModeGetPlaneResources(d->fd);
    uint32_t plane_id = 0;
    uint32_t fallback_plane = 0;
    if (pres) {
        for (uint32_t i = 0; i < pres->count_planes; i++) {
            drmModePlanePtr pl = drmModeGetPlane(d->fd, pres->planes[i]);
            if (!pl)
                continue;
            uint32_t type = get_prop_id(d->fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type");
            uint64_t tv = 0;
            if (type) {
                drmModeObjectPropertiesPtr pp =
                    drmModeObjectGetProperties(d->fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
                for (uint32_t k = 0; k < pp->count_props; k++) {
                    if (pp->props[k] == type)
                        tv = pp->prop_values[k];
                }
                drmModeFreeObjectProperties(pp);
            }
            int covers = (pl->possible_crtcs & (1u << (crtc_index < 0 ? 0 : crtc_index))) != 0;
            if (!fallback_plane && covers)
                fallback_plane = pl->plane_id;
            if (tv == DRM_PLANE_TYPE_PRIMARY && covers) {
                plane_id = pl->plane_id;
                drmModeFreePlane(pl);
                break;
            }
            drmModeFreePlane(pl);
        }
        drmModeFreePlaneResources(pres);
    }
    if (!plane_id)
        plane_id = fallback_plane; /* 兜底：覆盖本 CRTC 的第一个平面 */
    if (!plane_id) {
        fprintf(stderr, "aether-drm: 未找到可用平面\n");
        return -1;
    }
    d->plane_id = plane_id;

    /* 创建双缓冲 */
    if (make_dumb(d, 0, d->width, d->height) != 0)
        return -1;
    if (make_dumb(d, 1, d->width, d->height) != 0)
        return -1;

    /* 模式设置（阻塞原子提交） */
    uint32_t blob = 0;
    drmModeCreatePropertyBlob(d->fd, mode, sizeof(*mode), &blob);

    uint32_t p_active = get_prop_id(d->fd, d->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t p_mode = get_prop_id(d->fd, d->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    uint32_t p_conn_crtc = get_prop_id(d->fd, d->conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    uint32_t p_fb = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t p_plane_crtc = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t p_sx = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t p_sy = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t p_sw = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t p_sh = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    uint32_t p_cx = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t p_cy = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t p_cw = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t p_ch = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, d->conn_id, p_conn_crtc, d->crtc_id);
    drmModeAtomicAddProperty(req, d->crtc_id, p_active, 1);
    if (blob)
        drmModeAtomicAddProperty(req, d->crtc_id, p_mode, blob);
    drmModeAtomicAddProperty(req, d->plane_id, p_plane_crtc, d->crtc_id);
    drmModeAtomicAddProperty(req, d->plane_id, p_fb, d->fb[0]);
    drmModeAtomicAddProperty(req, d->plane_id, p_sx, 0);
    drmModeAtomicAddProperty(req, d->plane_id, p_sy, 0);
    drmModeAtomicAddProperty(req, d->plane_id, p_sw, (uint64_t)d->width << 16);
    drmModeAtomicAddProperty(req, d->plane_id, p_sh, (uint64_t)d->height << 16);
    drmModeAtomicAddProperty(req, d->plane_id, p_cx, 0);
    drmModeAtomicAddProperty(req, d->plane_id, p_cy, 0);
    drmModeAtomicAddProperty(req, d->plane_id, p_cw, d->width);
    drmModeAtomicAddProperty(req, d->plane_id, p_ch, d->height);
    int rc = drmModeAtomicCommit(d->fd, req, 0, NULL);
    drmModeAtomicFree(req);
    if (rc != 0) {
        fprintf(stderr, "aether-drm: modeset 失败 (%d) %s\n", rc, strerror(errno));
        return -1;
    }

    d->back = 1; /* fb[0] 已上屏，back 取另一块 */
    d->flip_pending = 0;
    if (conn)
        drmModeFreeConnector(conn);
    drmModeFreeResources(res0);
    return 0;
}

void *aether_drm_back_buffer(struct aether_drm *d)
{
    return d->map[d->back];
}

int aether_drm_present(struct aether_drm *d)
{
    if (d->flip_pending)
        return 0;
    uint32_t p_fb = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t p_plane_crtc = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t p_sw = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t p_sh = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    uint32_t p_cw = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t p_ch = get_prop_id(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, d->plane_id, p_plane_crtc, d->crtc_id);
    drmModeAtomicAddProperty(req, d->plane_id, p_fb, d->fb[d->back]);
    drmModeAtomicAddProperty(req, d->plane_id, p_sw, (uint64_t)d->width << 16);
    drmModeAtomicAddProperty(req, d->plane_id, p_sh, (uint64_t)d->height << 16);
    drmModeAtomicAddProperty(req, d->plane_id, p_cw, d->width);
    drmModeAtomicAddProperty(req, d->plane_id, p_ch, d->height);
    int rc = drmModeAtomicCommit(d->fd, req,
                                 DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, d);
    drmModeAtomicFree(req);
    if (rc != 0)
        return -1;
    d->flip_pending = 1;
    return 0;
}

int aether_drm_handle_events(struct aether_drm *d)
{
    drmEventContext ev;
    memset(&ev, 0, sizeof ev);
    ev.version = 2;
    ev.page_flip_handler = on_page_flip;
    /* 非阻塞：仅在有事件时处理 */
    drmHandleEvent(d->fd, &ev);
    return 0;
}

void aether_drm_close(struct aether_drm *d)
{
    if (d->fd >= 0)
        close(d->fd);
    d->fd = -1;
}
