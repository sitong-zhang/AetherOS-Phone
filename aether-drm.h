/*
 * aether-drm.h - AetherOS graphics stack, layer 1: atomic KMS via raw ioctls.
 *
 * Deliberately dependency-free: no libdrm, no gbm, no third-party graphics
 * library. Talks to the kernel DRM subsystem through its UAPI directly so
 * that every mode-setting step is visible and auditable in this file.
 *
 * Contract:
 *   - open /dev/dri/card0 (or $AETHER_DRM_NODE)
 *   - SET_MASTER
 *   - client caps: ATOMIC + UNIVERSAL_PLANES
 *   - pick first connected connector, first mode in its mode list
 *   - create two dumb buffers (double buffering), wrap as framebuffers
 *   - one atomic modeset commit, then per-frame page-flip commits
 *   - page-flip completion is drained from the drm fd via read()
 *
 * The framebuffers are XRGB8888. The renderer (aether-render.h) paints into
 * the back buffer; aether_drm_present() flips it to the front.
 */
#ifndef AETHER_DRM_H
#define AETHER_DRM_H

#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#define AETHER_DRM_MAX_OBJECTS 32
#define AETHER_DRM_MAX_PROPS 64

/*
 * UAPI enum drm_connector_status: connected = 1. The enum itself lives in
 * drm_connector.h, which is exported by neither the Debian aarch64 sysroot
 * nor this kernel tree's uapi headers; the values are stable kernel ABI.
 */
#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

/*
 * enum drm_plane_type ABI values. The enum lives in the kernel-private
 * <drm/drm_plane.h>; no uapi header defines these (libdrm carries its own
 * copy), so define them here from the kernel source of truth
 * (include/drm/drm_plane.h, enum drm_plane_type):
 *
 *     DRM_PLANE_TYPE_OVERLAY = 0
 *     DRM_PLANE_TYPE_PRIMARY = 1
 *     DRM_PLANE_TYPE_CURSOR  = 2
 *
 * The kernel writes plane->type straight into the immutable "type" property
 * (drm_object_attach_property in __drm_universal_plane_init), so these are
 * exactly the values OBJ_GETPROPERTIES reports back.
 */
#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2
#endif

typedef struct {
    int fd;

    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    uint32_t crtc_idx;      /* index of crtc_id in the resource list */
    uint32_t plane_id;      /* primary plane of the crtc */
    uint16_t plane_idx;     /* index within the object array for atomic commits */

    struct drm_mode_modeinfo mode;
    uint32_t width;
    uint32_t height;

    uint32_t dumb_handle[2];
    uint32_t fb_id[2];
    uint32_t pitch;         /* bytes per scanline */
    uint32_t size;          /* total mapped size of one buffer */
    void *map[2];           /* mmap of the dumb buffers */

    int back;               /* index of the buffer being painted */
    uint32_t pending_fb;    /* fb awaiting flip completion, 0 if idle */
    uint64_t frame;         /* monotonic frame counter (flips completed) */
    uint64_t seq;           /* user_data cookie for page-flip events */

    /* Hardware plane inventory (enumerated from the real DRM resources;
     * not assumed). Counts are over planes whose possible_crtcs covers
     * our crtc. Only PRImaxRY is actually used for composition today. */
    uint32_t n_planes;      /* total planes visible to this crtc */
    uint32_t n_primary;
    uint32_t n_overlay;
    uint32_t n_cursor;

    /* GPU 3D / virgl capability, read from DRM_CAP_3D (negotiated
     * VIRTIO_GPU_F_VIRGL). 0 = software-only dumb-buffer path. */
    int gpu_3d;

    char device[64];
} aether_drm_t;

/*
 * Open and fully initialize the display. Returns 0 on success, -1 on
 * failure (message already printed to stderr with errno context).
 */
int aether_drm_open(aether_drm_t *d, const char *device);

/* Release all resources. Safe on a partially-initialized handle. */
void aether_drm_close(aether_drm_t *d);

/* Pointer to the back buffer that the renderer should paint into. */
static inline void *aether_drm_back_buffer(aether_drm_t *d)
{
    return d->map[d->back];
}

/*
 * Queue an atomic page-flip of the back buffer. Does not wait for the
 * flip to complete; call aether_drm_wait_flip() before painting again.
 * Returns 0 on success.
 */
int aether_drm_present(aether_drm_t *d);

/*
 * Block until the pending page-flip completes (or timeout). On success
 * the buffer that was just presented becomes back for the next frame.
 * Returns 0 on success, -1 on timeout/error.
 */
int aether_drm_wait_flip(aether_drm_t *d, int timeout_ms);

/* Total bytes of one framebuffer. */
static inline uint32_t aether_drm_buffer_size(const aether_drm_t *d)
{
    return d->size;
}

#endif /* AETHER_DRM_H */
