/*
 * aether-drm.c - atomic KMS via raw DRM ioctls. See aether-drm.h for the
 * contract. Every kernel interaction below is one explicit UAPI ioctl:
 * no libdrm, no hidden state machine.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/virtgpu_drm.h>   /* VIRTGPU_PARAM_3D_FEATURES / GETPARAM */

#include "aether-drm.h"

#define LOG_PREFIX "aether-drm: "

static void log_errno(const char *what)
{
    fprintf(stderr, LOG_PREFIX "%s: %s\n", what, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* Property lookup: turn "MODE_ID"/"ACTIVE"/... names into prop ids.   */
/* ------------------------------------------------------------------ */

/* Fill ids[] (parallel to names[]) for one object. Returns 0 on success. */
static int lookup_props(int fd, uint32_t obj_id, uint32_t obj_type,
                        const char **names, uint32_t *ids, uint32_t n_names)
{
    struct drm_mode_obj_get_properties gop;
    uint32_t count;

    /* First pass: learn how many props the object has. */
    memset(&gop, 0, sizeof(gop));
    gop.obj_id = obj_id;
    gop.obj_type = obj_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gop) < 0) {
        log_errno("OBJ_GETPROPERTIES(count)");
        return -1;
    }
    count = gop.count_props;
    if (count == 0 || count > AETHER_DRM_MAX_PROPS) {
        fprintf(stderr, LOG_PREFIX "object %u has %u props (limit %d)\n",
                obj_id, count, AETHER_DRM_MAX_PROPS);
        return -1;
    }

    uint32_t *ids_buf = calloc(count, sizeof(uint32_t));
    uint64_t *vals_buf = calloc(count, sizeof(uint64_t));
    struct drm_mode_get_property gp;
    uint32_t found = 0;
    int ret = -1;

    if (!ids_buf || !vals_buf)
        goto out;

    memset(&gop, 0, sizeof(gop));
    gop.obj_id = obj_id;
    gop.obj_type = obj_type;
    gop.count_props = count;
    gop.props_ptr = (uint64_t)ids_buf;
    gop.prop_values_ptr = (uint64_t)vals_buf;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gop) < 0) {
        log_errno("OBJ_GETPROPERTIES(fill)");
        goto out;
    }

    for (uint32_t i = 0; i < count; i++) {
        memset(&gp, 0, sizeof(gp));
        gp.prop_id = ids_buf[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0) {
            log_errno("GETPROPERTY");
            goto out;
        }
        for (uint32_t k = 0; k < n_names; k++) {
            if (strncmp(gp.name, names[k], DRM_PROP_NAME_LEN) == 0) {
                ids[k] = gp.prop_id;
                found++;
            }
        }
    }

    if (found != n_names) {
        fprintf(stderr, LOG_PREFIX "object %u: found %u/%u props\n",
                obj_id, found, n_names);
        goto out;
    }
    ret = 0;

out:
    free(ids_buf);
    free(vals_buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Connector / encoder / crtc discovery.                              */
/* ------------------------------------------------------------------ */

static int pick_connector(aether_drm_t *d)
{
    struct drm_mode_card_res res;
    uint32_t *conn_ids = NULL;
    uint32_t *crtc_ids = NULL;
    uint32_t *enc_ids = NULL;
    int ret = -1;

    memset(&res, 0, sizeof(res));
    if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        log_errno("GETRESOURCES(count)");
        return -1;
    }
    if (res.count_connectors == 0 || res.count_crtcs == 0) {
        fprintf(stderr, LOG_PREFIX "no connectors or crtcs reported\n");
        return -1;
    }

    /* The kernel put_user()s ids for every category whose count is
     * non-zero on the second call — encoder_id_ptr included, even if we
     * never read the encoders. All four pointers must be valid. */
    conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    enc_ids = calloc(res.count_encoders, sizeof(uint32_t));
    if (!conn_ids || !crtc_ids || !enc_ids)
        goto out;
    res.connector_id_ptr = (uint64_t)conn_ids;
    res.crtc_id_ptr = (uint64_t)crtc_ids;
    res.encoder_id_ptr = (uint64_t)enc_ids;
    if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        log_errno("GETRESOURCES(fill)");
        goto out;
    }

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c;
        struct drm_mode_modeinfo *modes = NULL;
        uint32_t *encoders = NULL;
        uint32_t *props = NULL;
        uint64_t *prop_vals = NULL;

        memset(&c, 0, sizeof(c));
        c.connector_id = conn_ids[i];
        if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) {
            log_errno("GETCONNECTOR(count)");
            continue;
        }
        if (c.connection != DRM_MODE_CONNECTED)
            continue;
        if (c.count_modes == 0 || c.count_encoders == 0) {
            fprintf(stderr, LOG_PREFIX "connector %u connected but empty\n",
                    conn_ids[i]);
            continue;
        }

        modes = calloc(c.count_modes, sizeof(*modes));
        encoders = calloc(c.count_encoders, sizeof(uint32_t));
        /* props/prop_values: the kernel copies both arrays on the second
         * call whenever count_props is non-zero, so both must be valid
         * even though we only read modes + encoders here. */
        props = calloc(c.count_props ? c.count_props : 1, sizeof(uint32_t));
        prop_vals = calloc(c.count_props ? c.count_props : 1, sizeof(uint64_t));
        if (!modes || !encoders || !props || !prop_vals)
            goto next;

        c.modes_ptr = (uint64_t)modes;
        c.encoders_ptr = (uint64_t)encoders;
        c.props_ptr = (uint64_t)props;
        c.prop_values_ptr = (uint64_t)prop_vals;
        if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) {
            log_errno("GETCONNECTOR(fill)");
            goto next;
        }

        /* First mode is the preferred one on virtio-gpu. */
        d->connector_id = conn_ids[i];
        d->mode = modes[0];
        d->width = modes[0].hdisplay;
        d->height = modes[0].vdisplay;

        /* encoder -> crtc */
        struct drm_mode_get_encoder e;
        memset(&e, 0, sizeof(e));
        e.encoder_id = encoders[0];
        if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0) {
            log_errno("GETENCODER");
            goto next;
        }
        d->encoder_id = encoders[0];
        d->crtc_id = e.crtc_id;
        for (uint32_t ci = 0; ci < res.count_crtcs; ci++) {
            if (crtc_ids[ci] == d->crtc_id) {
                d->crtc_idx = ci;
                break;
            }
        }
        ret = 0;
        fprintf(stderr, LOG_PREFIX "connector %u: %ux%u, crtc %u, mode %s\n",
                d->connector_id, d->width, d->height, d->crtc_id,
                modes[0].name);
        goto next_done;

next:
        free(modes);
        free(encoders);
        free(props);
        free(prop_vals);
    }

next_done:
    if (ret != 0)
        fprintf(stderr, LOG_PREFIX "no connected connector with a mode\n");
out:
    free(conn_ids);
    free(crtc_ids);
    free(enc_ids);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Primary plane discovery.                                           */
/* ------------------------------------------------------------------ */

static int pick_plane(aether_drm_t *d)
{
    struct drm_mode_get_plane_res pr;

    /* Hardware plane inventory: we enumerate EVERY plane the driver
     * exposes for our crtc and record how many of each type exist.
     * Composition itself still uses the single PRIMARY plane (the only
     * type virtio-gpu advertises); the counts are reported honestly so
     * the compositor never claims overlay/cursor blending it cannot do. */
    d->n_planes = d->n_primary = d->n_overlay = d->n_cursor = 0;

    memset(&pr, 0, sizeof(pr));
    if (ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
        log_errno("GETPLANERESOURCES(count)");
        return -1;
    }
    if (pr.count_planes == 0) {
        fprintf(stderr, LOG_PREFIX "no planes reported\n");
        return -1;
    }

    uint32_t *plane_ids = calloc(pr.count_planes, sizeof(uint32_t));
    if (!plane_ids)
        return -1;
    pr.plane_id_ptr = (uint64_t)plane_ids;
    if (ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
        log_errno("GETPLANERESOURCES(fill)");
        free(plane_ids);
        return -1;
    }

    int ret = -1;
    for (uint32_t i = 0; i < pr.count_planes; i++) {
        struct drm_mode_get_plane p;
        struct drm_mode_obj_get_properties gop;
        static const char *type_name = "type";
        uint64_t type_val = 0;
        int have_type = 0;

        memset(&p, 0, sizeof(p));
        p.plane_id = plane_ids[i];
        if (ioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &p) < 0) {
            log_errno("GETPLANE");
            continue;
        }

        /*
         * The kernel writes back the real prop count on the second
         * round-trip, so the buffers must cover the maximum, not a
         * guessed number (planes carry >10 props on virtio-gpu).
         */
        uint32_t *pids = calloc(AETHER_DRM_MAX_PROPS, sizeof(uint32_t));
        uint64_t *pvals = calloc(AETHER_DRM_MAX_PROPS, sizeof(uint64_t));
        if (!pids || !pvals) {
            free(pids);
            free(pvals);
            continue;
        }
        memset(&gop, 0, sizeof(gop));
        gop.obj_id = plane_ids[i];
        gop.obj_type = DRM_MODE_OBJECT_PLANE;
        gop.count_props = AETHER_DRM_MAX_PROPS;
        gop.props_ptr = (uint64_t)pids;
        gop.prop_values_ptr = (uint64_t)pvals;
        if (ioctl(d->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gop) == 0) {
            uint32_t n = gop.count_props;
            if (n > AETHER_DRM_MAX_PROPS)
                n = AETHER_DRM_MAX_PROPS;
            for (uint32_t k = 0; k < n; k++) {
                struct drm_mode_get_property gpn;
                memset(&gpn, 0, sizeof(gpn));
                gpn.prop_id = pids[k];
                if (ioctl(d->fd, DRM_IOCTL_MODE_GETPROPERTY, &gpn) < 0)
                    continue;
                if (strncmp(gpn.name, type_name, DRM_PROP_NAME_LEN) == 0) {
                    type_val = pvals[k];
                    have_type = 1;
                }
            }
        }
        free(pids);
        free(pvals);

        /* Inventory: tally every plane this crtc could drive, by type.
         * This is the honest "what hardware can we actually composite
         * with" report — independent of which plane we bind for blits. */
        if (p.possible_crtcs & (1u << d->crtc_idx)) {
            d->n_planes++;
            if (have_type) {
                if (type_val == DRM_PLANE_TYPE_PRIMARY)
                    d->n_primary++;
                else if (type_val == DRM_PLANE_TYPE_OVERLAY)
                    d->n_overlay++;
                else if (type_val == DRM_PLANE_TYPE_CURSOR)
                    d->n_cursor++;
            }
        }

        if (have_type && type_val == DRM_PLANE_TYPE_PRIMARY &&
            (p.possible_crtcs & (1u << d->crtc_idx))) {
            d->plane_id = plane_ids[i];
            ret = 0;
            fprintf(stderr, LOG_PREFIX "primary plane %u (type=%llu, crtc mask 0x%x)\n",
                    d->plane_id, (unsigned long long)type_val,
                    p.possible_crtcs);
            break;
        }
        fprintf(stderr, LOG_PREFIX "plane %u skipped: type=%llu mask=0x%x\n",
                plane_ids[i], (unsigned long long)type_val,
                p.possible_crtcs);
    }

    free(plane_ids);
    if (ret != 0)
        fprintf(stderr, LOG_PREFIX "no usable primary plane found\n");
    return ret;
}

/* ------------------------------------------------------------------ */
/* Dumb buffers + framebuffers.                                       */
/* ------------------------------------------------------------------ */

static int create_fbs(aether_drm_t *d)
{
    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb cd;
        struct drm_mode_map_dumb md;
        struct drm_mode_fb_cmd2 fb;

        memset(&cd, 0, sizeof(cd));
        cd.width = d->width;
        cd.height = d->height;
        cd.bpp = 32;
        if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
            log_errno("CREATE_DUMB");
            return -1;
        }
        d->dumb_handle[i] = cd.handle;
        d->pitch = cd.pitch;
        d->size = cd.size;

        memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
            log_errno("MAP_DUMB");
            return -1;
        }
        d->map[i] = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         d->fd, md.offset);
        if (d->map[i] == MAP_FAILED) {
            log_errno("mmap(dumb)");
            d->map[i] = NULL;
            return -1;
        }

        memset(&fb, 0, sizeof(fb));
        fb.width = d->width;
        fb.height = d->height;
        fb.pixel_format = DRM_FORMAT_XRGB8888;
        fb.handles[0] = cd.handle;
        fb.pitches[0] = cd.pitch;
        if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
            log_errno("ADDFB2");
            return -1;
        }
        d->fb_id[i] = fb.fb_id;
    }
    fprintf(stderr, LOG_PREFIX "dumb fbs: %ux%u pitch %u size %u (x2)\n",
            d->width, d->height, d->pitch, d->size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Atomic commit helpers.                                             */
/* ------------------------------------------------------------------ */

struct prop_slot {
    uint32_t prop;
    uint64_t value;
};

static int atomic_commit(aether_drm_t *d,
                         uint32_t *objs, uint32_t *obj_counts,
                         struct prop_slot **slots, uint32_t n_objs,
                         uint64_t flags, uint64_t user_data)
{
    uint32_t flat_props[AETHER_DRM_MAX_OBJECTS * 16];
    uint64_t flat_vals[AETHER_DRM_MAX_OBJECTS * 16];
    uint32_t total = 0;

    for (uint32_t i = 0; i < n_objs; i++) {
        for (uint32_t k = 0; k < obj_counts[i]; k++) {
            flat_props[total] = slots[i][k].prop;
            flat_vals[total] = slots[i][k].value;
            total++;
        }
    }

    struct drm_mode_atomic a;
    memset(&a, 0, sizeof(a));
    a.flags = flags;
    a.count_objs = n_objs;
    a.objs_ptr = (uint64_t)objs;
    a.count_props_ptr = (uint64_t)obj_counts;
    a.props_ptr = (uint64_t)flat_props;
    a.prop_values_ptr = (uint64_t)flat_vals;
    a.user_data = user_data;

    if (ioctl(d->fd, DRM_IOCTL_MODE_ATOMIC, &a) < 0) {
        log_errno("MODE_ATOMIC");
        return -1;
    }
    return 0;
}

static int first_modeset(aether_drm_t *d)
{
    static const char *conn_names[] = { "CRTC_ID" };
    static const char *crtc_names[] = { "MODE_ID", "ACTIVE" };
    static const char *plane_names[] = {
        "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
        "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"
    };
    uint32_t conn_crtc[1];
    uint32_t crtc_ids[2];
    uint32_t plane_ids[10];
    uint32_t objs[3];
    uint32_t counts[3];
    struct prop_slot conn_slots[1];
    struct prop_slot crtc_slots[2];
    struct prop_slot plane_slots[10];
    struct prop_slot *slots[3];
    uint32_t mode_blob;

    if (lookup_props(d->fd, d->connector_id, DRM_MODE_OBJECT_CONNECTOR,
                     conn_names, conn_crtc, 1) < 0)
        return -1;
    if (lookup_props(d->fd, d->crtc_id, DRM_MODE_OBJECT_CRTC,
                     crtc_names, crtc_ids, 2) < 0)
        return -1;
    if (lookup_props(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE,
                     plane_names, plane_ids, 10) < 0)
        return -1;

    /* mode blob */
    struct drm_mode_create_blob blob;
    memset(&blob, 0, sizeof(blob));
    blob.length = sizeof(d->mode);
    blob.data = (uint64_t)&d->mode;
    if (ioctl(d->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0) {
        log_errno("CREATEPROPBLOB");
        return -1;
    }
    mode_blob = blob.blob_id;

    conn_slots[0].prop = conn_crtc[0];
    conn_slots[0].value = d->crtc_id;

    crtc_slots[0].prop = crtc_ids[0];   /* MODE_ID */
    crtc_slots[0].value = mode_blob;
    crtc_slots[1].prop = crtc_ids[1];   /* ACTIVE */
    crtc_slots[1].value = 1;

    plane_slots[0].prop = plane_ids[0]; /* FB_ID */
    plane_slots[0].value = d->fb_id[0];
    plane_slots[1].prop = plane_ids[1]; /* CRTC_ID */
    plane_slots[1].value = d->crtc_id;
    plane_slots[2].prop = plane_ids[2]; /* SRC_X */
    plane_slots[2].value = 0;
    plane_slots[3].prop = plane_ids[3]; /* SRC_Y */
    plane_slots[3].value = 0;
    plane_slots[4].prop = plane_ids[4]; /* SRC_W */
    plane_slots[4].value = (uint64_t)d->width << 16;
    plane_slots[5].prop = plane_ids[5]; /* SRC_H */
    plane_slots[5].value = (uint64_t)d->height << 16;
    plane_slots[6].prop = plane_ids[6]; /* CRTC_X */
    plane_slots[6].value = 0;
    plane_slots[7].prop = plane_ids[7]; /* CRTC_Y */
    plane_slots[7].value = 0;
    plane_slots[8].prop = plane_ids[8]; /* CRTC_W */
    plane_slots[8].value = d->width;
    plane_slots[9].prop = plane_ids[9]; /* CRTC_H */
    plane_slots[9].value = d->height;

    objs[0] = d->connector_id; counts[0] = 1; slots[0] = conn_slots;
    objs[1] = d->crtc_id;     counts[1] = 2; slots[1] = crtc_slots;
    objs[2] = d->plane_id;    counts[2] = 10; slots[2] = plane_slots;

    /* test first: no side effects, catches bad property wiring */
    if (atomic_commit(d, objs, counts, slots, 3,
                      DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
                      0) < 0)
        return -1;

    if (atomic_commit(d, objs, counts, slots, 3,
                      DRM_MODE_ATOMIC_ALLOW_MODESET, 0) < 0)
        return -1;

    fprintf(stderr, LOG_PREFIX "initial modeset committed (%ux%u)\n",
            d->width, d->height);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                        */
/* ------------------------------------------------------------------ */

/* Detect the GPU's 3D/virgl capability the way Mesa's virgl driver
 * does: query the virtio-gpu specific GETPARAM 3D_FEATURES. This is the
 * negotiated VIRTIO_GPU_F_VIRGL bit, surfaced by the kernel as
 * vgdev->has_virgl_3d. Returns 0 on the software-only dumb-buffer
 * path (no GL command stream). Only meaningful on virtio-gpu; on any
 * other driver the ioctl returns -EINVAL and we honestly report 0. */
static void detect_gpu_caps(aether_drm_t *d)
{
    d->gpu_3d = 0;
    struct drm_virtgpu_getparam gp;
    memset(&gp, 0, sizeof(gp));
    gp.param = VIRTGPU_PARAM_3D_FEATURES;
    if (ioctl(d->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) == 0)
        d->gpu_3d = gp.value ? 1 : 0;
    fprintf(stderr, LOG_PREFIX "gpu caps: VIRTGPU_PARAM_3D_FEATURES=%d%s\n",
            d->gpu_3d,
            d->gpu_3d ? " (virgl/GL command stream available)" : "");
}

int aether_drm_open(aether_drm_t *d, const char *device)
{
    const char *dev = device;
    const char *env;

    memset(d, 0, sizeof(*d));
    d->fd = -1;

    env = getenv("AETHER_DRM_NODE");
    if (env && *env)
        dev = env;
    if (!dev)
        dev = "/dev/dri/card0";

    snprintf(d->device, sizeof(d->device), "%s", dev);
    d->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        log_errno(dev);
        return -1;
    }

    if (ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
        log_errno("SET_MASTER");
        goto fail;
    }

    struct drm_set_client_cap cap;
    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value = 1;
    if (ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        log_errno("SET_CLIENT_CAP(UNIVERSAL_PLANES)");
        goto fail;
    }
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value = 1;
    if (ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        log_errno("SET_CLIENT_CAP(ATOMIC)");
        goto fail;
    }

    if (pick_connector(d) < 0)
        goto fail;
    if (pick_plane(d) < 0)
        goto fail;
    detect_gpu_caps(d);

    fprintf(stderr, LOG_PREFIX "plane inventory: total=%u primary=%u overlay=%u cursor=%u\n",
            d->n_planes, d->n_primary, d->n_overlay, d->n_cursor);
    if (create_fbs(d) < 0)
        goto fail;
    if (first_modeset(d) < 0)
        goto fail;

    d->back = 0;
    d->frame = 0;
    d->seq = 0;
    return 0;

fail:
    aether_drm_close(d);
    return -1;
}

void aether_drm_close(aether_drm_t *d)
{
    if (d->fd >= 0) {
        for (int i = 0; i < 2; i++) {
            if (d->map[i] && d->map[i] != MAP_FAILED)
                munmap(d->map[i], d->size);
            d->map[i] = NULL;
        }
        if (d->fb_id[0])
            ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &d->fb_id[0]);
        if (d->fb_id[1])
            ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &d->fb_id[1]);
        for (int i = 0; i < 2; i++) {
            if (d->dumb_handle[i]) {
                struct drm_mode_destroy_dumb dd;
                memset(&dd, 0, sizeof(dd));
                dd.handle = d->dumb_handle[i];
                ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            }
        }
        close(d->fd);
        d->fd = -1;
    }
}

int aether_drm_present(aether_drm_t *d)
{
    static const char *plane_names[] = { "FB_ID" };
    uint32_t fb_prop;
    uint32_t obj = d->plane_id;
    uint32_t count = 1;
    struct prop_slot slot;
    struct prop_slot *slots[1];

    if (d->pending_fb) {
        fprintf(stderr, LOG_PREFIX "present called with flip pending\n");
        return -1;
    }
    if (lookup_props(d->fd, d->plane_id, DRM_MODE_OBJECT_PLANE,
                     plane_names, &fb_prop, 1) < 0)
        return -1;

    slot.prop = fb_prop;
    slot.value = d->fb_id[d->back];
    slots[0] = &slot;

    d->seq++;
    if (atomic_commit(d, &obj, &count, slots, 1,
                      DRM_MODE_PAGE_FLIP_EVENT, d->seq) < 0) {
        d->seq--;
        return -1;
    }
    d->pending_fb = d->fb_id[d->back];
    return 0;
}

int aether_drm_wait_flip(aether_drm_t *d, int timeout_ms)
{
    struct pollfd pfd;
    uint64_t want = d->seq;
    char buf[256];

    if (!d->pending_fb)
        return 0;

    pfd.fd = d->fd;
    pfd.events = POLLIN;
    while (1) {
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            log_errno("poll(drm fd)");
            return -1;
        }
        if (r == 0)
            return -1; /* timeout */

        ssize_t n = read(d->fd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(struct drm_event)) {
            if (n < 0 && errno == EINTR)
                continue;
            log_errno("read(drm event)");
            return -1;
        }
        struct drm_event *ev = (struct drm_event *)buf;
        if (ev->type == DRM_EVENT_FLIP_COMPLETE &&
            n >= (ssize_t)sizeof(struct drm_event_vblank)) {
            struct drm_event_vblank *vb = (struct drm_event_vblank *)buf;
            if (vb->user_data == want) {
                d->pending_fb = 0;
                d->frame++;
                d->back = 1 - d->back;
                return 0;
            }
        }
        /* other events (hotplug etc.) are drained and ignored */
    }
}
