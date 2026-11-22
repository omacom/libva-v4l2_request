/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deterministic userspace failures against a model V4L2 device. No hardware. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <linux/media.h>
#include <va/va_drmcommon.h>
#include "v4l2_request.h"

#define LIMIT 1024
#define FD_BASE 1000
enum kind { VIDEO = 1, MEDIA, REQUEST, DMABUF, CONVERTER };
static struct {
    enum kind kind;
    bool live;
    unsigned int generation;
    unsigned int outputs, captures;
    uint32_t output_queued, output_ready;
    uint64_t capture_queued, capture_ready;
    int request_output, request_video;
    bool request_done;
    struct v4l2_format formats[2];
} fds[LIMIT];
static struct { void *ptr; size_t size; } maps[LIMIT], heaps[LIMIT];
static unsigned int fd_count, op, fail_at, injected, queues;
static bool armed, packed, single_plane, hold, sliced, dequeue_blocked, fast_clock;
static bool pitch_retry, fail_rollback, optional_failure;
static uint64_t model_ns;
static unsigned long fail_ioctl;
static int fail_type = -1, fail_errno = EIO;
static unsigned int fail_ioctl_nth, ioctl_matches;
static int capture_flags, capture_bad_index = -1, output_bad_index = -1;
static int poll_error, poll_error_count, poll_timeouts;
static short poll_events;
static const char *operations[LIMIT];
static struct VADriverContext va;
static struct VADriverVTable table;
static struct v4l2r_driver *drv;
static VAConfigID cfg;
/* Rebind cases: completions held back as if the hardware were still decoding.
 * With finish_on_poll, a driver wait for a CAPTURE buffer releases them and is
 * counted, so a case can tell a rebind (no wait) from a wait. */
static bool hw_busy, finish_on_poll;
static uint64_t hw_held;
static unsigned int hw_slot, hw_waits;
static void hw_finish(void)
{
    fds[hw_slot].capture_ready |= hw_held; hw_held = 0;
}

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int __real_munmap(void *addr, size_t len);
int __real_clock_gettime(clockid_t clock, struct timespec *ts);
int __wrap_clock_gettime(clockid_t clock, struct timespec *ts)
{
    if (!fast_clock || clock != CLOCK_MONOTONIC) return __real_clock_gettime(clock, ts);
    model_ns += 10000000;
    ts->tv_sec = (time_t)(model_ns / 1000000000);
    ts->tv_nsec = (long)(model_ns % 1000000000);
    return 0;
}

static bool fault(const char *name)
{
    if (!armed) return false;
    assert(op + 1 < LIMIT);
    operations[++op] = name;
    if (fail_at && op == fail_at) {
        injected++;
        errno = !strcmp(name, "calloc") || !strcmp(name, "malloc") ||
                !strcmp(name, "realloc") || !strcmp(name, "mmap") ? ENOMEM : EIO;
        return true;
    }
    return false;
}
static unsigned int heap_slot(void *ptr)
{
    for (unsigned int i = 0; i < LIMIT; i++) if (heaps[i].ptr == ptr) return i;
    assert(!"untracked heap pointer");
    return 0;
}
static void track_heap(void *ptr, size_t size)
{
    if (!ptr) return;
    unsigned int i = heap_slot(NULL);
    heaps[i].ptr = ptr; heaps[i].size = size;
}
void *__wrap_malloc(size_t size)
{
    if (fault("malloc")) return NULL;
    void *p = __real_malloc(size); track_heap(p, size); return p;
}
void *__wrap_calloc(size_t n, size_t size)
{
    if (fault("calloc")) return NULL;
    void *p = __real_calloc(n, size); track_heap(p, n * size); return p;
}
void *__wrap_realloc(void *ptr, size_t size)
{
    if (fault("realloc")) return NULL;
    unsigned int i = heap_slot(ptr);
    void *p = __real_realloc(ptr, size);
    if (p) { heaps[i].ptr = p; heaps[i].size = size; }
    return p;
}
void __wrap_free(void *ptr)
{
    if (!ptr) return;
    heaps[heap_slot(ptr)].ptr = NULL;
    __real_free(ptr);
}
static int new_fd(enum kind kind)
{
    unsigned int n;
    for (n = 0; n < fd_count && fds[n].live; n++);
    if (n == fd_count) { assert(fd_count < LIMIT); fd_count++; }
    unsigned int generation = fds[n].generation + 1;
    memset(&fds[n], 0, sizeof(fds[n]));
    fds[n].kind = kind; fds[n].live = true; fds[n].generation = generation;
    fds[n].request_output = fds[n].request_video = -1;
    return FD_BASE + (int)(generation * LIMIT + n);
}
static unsigned int fd_slot(int fd)
{
    assert(fd >= FD_BASE);
    unsigned int value = (unsigned int)(fd - FD_BASE);
    unsigned int n = value % LIMIT;
    assert(n < fd_count && fds[n].live);
    assert(fds[n].generation == value / LIMIT);
    return n;
}
int __wrap_open(const char *path, int flags, ...)
{
    (void)flags;
    if (fault("open")) return -1;
    if (!strcmp(path, "model-video")) return new_fd(VIDEO);
    if (!strcmp(path, "model-media")) return new_fd(MEDIA);
    assert(!strcmp(path, "model-converter"));
    return new_fd(CONVERTER);
}
int __wrap_close(int fd)
{
    fds[fd_slot(fd)].live = false;
    return 0;
}
int __wrap_dup(int fd)
{
    unsigned int i = fd_slot(fd);
    if (fault("dup")) return -1;
    return new_fd(fds[i].kind);
}
int __wrap_fcntl(int fd, int cmd, ...)
{
    unsigned int i = fd_slot(fd);
    assert(cmd == F_DUPFD_CLOEXEC);
    if (fault("fcntl-dup")) return -1;
    return new_fd(fds[i].kind);
}
int __wrap_open64(const char *path, int flags, ...) { return __wrap_open(path, flags); }
int __wrap___open_2(const char *path, int flags) { return __wrap_open(path, flags); }
int __wrap___open64_2(const char *path, int flags) { return __wrap_open(path, flags); }
int __wrap_fcntl64(int fd, int cmd, ...) { return __wrap_fcntl(fd, cmd); }
void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    (void)addr; (void)flags; (void)offset;
    fd_slot(fd);
    if (fault("mmap")) return MAP_FAILED;
    void *p = __real_mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    for (unsigned int i = 0; i < LIMIT; i++) if (!maps[i].ptr) {
        maps[i].ptr = p; maps[i].size = len; return p;
    }
    abort();
}
void *__wrap_mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    return __wrap_mmap(addr, len, prot, flags, fd, offset);
}
int __wrap_munmap(void *addr, size_t len)
{
    for (unsigned int i = 0; i < LIMIT; i++) if (maps[i].ptr == addr) {
        assert(maps[i].size == len); maps[i].ptr = NULL;
        return __real_munmap(addr, len);
    }
    assert(!"untracked mapping"); return -1;
}
static void format_fill(struct v4l2_format *f)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
        if (!f->fmt.pix_mp.width) f->fmt.pix_mp.width = 64;
        if (!f->fmt.pix_mp.height) f->fmt.pix_mp.height = 64;
        if (!f->fmt.pix_mp.pixelformat)
            f->fmt.pix_mp.pixelformat = packed ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_NV12;
        f->fmt.pix_mp.num_planes = 1;
        f->fmt.pix_mp.plane_fmt[0].bytesperline = 64;
        if (!f->fmt.pix_mp.plane_fmt[0].sizeimage)
            f->fmt.pix_mp.plane_fmt[0].sizeimage = 64 * 96;
    } else {
        if (!f->fmt.pix.width) f->fmt.pix.width = 64;
        if (!f->fmt.pix.height) f->fmt.pix.height = 64;
        if (!f->fmt.pix.pixelformat) f->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        f->fmt.pix.bytesperline = 64;
        if (!f->fmt.pix.sizeimage) f->fmt.pix.sizeimage = 64 * 96;
    }
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    unsigned int i = fd_slot(fd);
    void *arg = NULL;
    if (request != MEDIA_REQUEST_IOC_REINIT && request != MEDIA_REQUEST_IOC_QUEUE) {
        va_list ap; va_start(ap, request); arg = va_arg(ap, void *); va_end(ap);
    }
    int type = -1;
    if (request == VIDIOC_QBUF || request == VIDIOC_DQBUF)
        type = V4L2_TYPE_IS_OUTPUT(((struct v4l2_buffer *)arg)->type);
    const char *name = "ioctl";
#define OP(x) if (request == x) name = #x
    OP(VIDIOC_QUERYCAP); OP(VIDIOC_ENUM_FMT); OP(VIDIOC_ENUM_FRAMESIZES);
    OP(VIDIOC_S_FMT); OP(VIDIOC_G_FMT); OP(VIDIOC_CREATE_BUFS);
    OP(VIDIOC_QUERYBUF); OP(MEDIA_IOC_REQUEST_ALLOC); OP(VIDIOC_REQBUFS);
    OP(VIDIOC_STREAMON); OP(VIDIOC_STREAMOFF); OP(VIDIOC_S_EXT_CTRLS);
    OP(VIDIOC_QUERY_EXT_CTRL); OP(VIDIOC_QBUF); OP(VIDIOC_DQBUF);
    OP(VIDIOC_EXPBUF); OP(MEDIA_REQUEST_IOC_REINIT); OP(MEDIA_REQUEST_IOC_QUEUE);
    OP(VIDIOC_S_CTRL); OP(VIDIOC_S_SELECTION);
#undef OP
    if (fault(name)) {
        /* Only capability alternatives may complete despite a failed probe.
         * Required format setup and actual submitted work must report error. */
        optional_failure = request == VIDIOC_ENUM_FMT &&
            !V4L2_TYPE_IS_OUTPUT(((struct v4l2_fmtdesc *)arg)->type);
        if (request == VIDIOC_G_FMT) {
            struct v4l2_format *f = arg;
            optional_failure = !V4L2_TYPE_IS_OUTPUT(f->type) &&
                !fds[i].formats[0].type;
        }
        if (request == VIDIOC_S_FMT) {
            struct v4l2_format *f = arg;
            optional_failure = V4L2_TYPE_IS_OUTPUT(f->type) ?
                v4l2r_format_pixelformat(f) == V4L2_PIX_FMT_VP8 :
                v4l2r_format_pixelformat(&fds[i].formats[1]) == V4L2_PIX_FMT_VP8;
        }
        return -1;
    }
    if (armed && fail_rollback && injected && request == MEDIA_REQUEST_IOC_REINIT) {
        injected++; errno = EBUSY; return -1;
    }
    if (armed && request == fail_ioctl && (fail_type < 0 || type == fail_type) &&
        ++ioctl_matches == fail_ioctl_nth) {
        injected++; errno = fail_errno; return -1;
    }
    if (request == VIDIOC_QUERYCAP) {
        struct v4l2_capability *c = arg;
        c->capabilities = single_plane ? V4L2_CAP_VIDEO_M2M : V4L2_CAP_VIDEO_M2M_MPLANE;
        return 0;
    }
    if (request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg;
        if (f->index) { errno = EINVAL; return -1; }
        f->pixelformat = V4L2_TYPE_IS_OUTPUT(f->type) ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_NV12;
        if (packed && !V4L2_TYPE_IS_OUTPUT(f->type)) f->pixelformat = V4L2_PIX_FMT_NV16;
        return 0;
    }
    if (request == VIDIOC_ENUM_FRAMESIZES) {
        struct v4l2_frmsizeenum *f = arg;
        f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        f->discrete.width = f->discrete.height = 64; return 0;
    }
    if (request == VIDIOC_S_FMT || request == VIDIOC_G_FMT) {
        struct v4l2_format *f = arg;
        unsigned int side = V4L2_TYPE_IS_OUTPUT(f->type);
        if (request == VIDIOC_G_FMT && fds[i].formats[side].type)
            *f = fds[i].formats[side];
        format_fill(f);
        if (pitch_retry && fds[i].kind == CONVERTER &&
            f->fmt.pix_mp.width == 64)
            f->fmt.pix_mp.plane_fmt[0].bytesperline = 32;
        fds[i].formats[side] = *f; return 0;
    }
    if (request == VIDIOC_CREATE_BUFS) {
        struct v4l2_create_buffers *b = arg;
        b->capabilities = V4L2_BUF_CAP_SUPPORTS_REQUESTS;
#ifdef V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF
        if (hold) b->capabilities |= V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
#endif
        unsigned int *n = V4L2_TYPE_IS_OUTPUT(b->format.type) ? &fds[i].outputs : &fds[i].captures;
        b->index = *n; *n += b->count;
        if (b->count) fds[i].formats[V4L2_TYPE_IS_OUTPUT(b->format.type)] = b->format;
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        struct v4l2_format *f = &fds[i].formats[V4L2_TYPE_IS_OUTPUT(b->type)];
        if (V4L2_TYPE_IS_MULTIPLANAR(b->type)) {
            b->length = f->fmt.pix_mp.num_planes;
            for (unsigned int p = 0; p < b->length; p++)
                b->m.planes[p].length = f->fmt.pix_mp.plane_fmt[p].sizeimage;
        } else b->length = f->fmt.pix.sizeimage;
        return 0;
    }
    if (request == MEDIA_IOC_REQUEST_ALLOC) { *(int *)arg = new_fd(REQUEST); return 0; }
    if (request == VIDIOC_EXPBUF) { ((struct v4l2_exportbuffer *)arg)->fd = new_fd(DMABUF); return 0; }
    if (request == VIDIOC_QUERY_EXT_CTRL) { ((struct v4l2_query_ext_ctrl *)arg)->default_value = 1; return 0; }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        assert(b->index < 32); queues++;
        if (type) {
            assert(!(fds[i].output_queued & (1u << b->index)));
            fds[i].output_queued |= 1u << b->index;
            if (fds[i].kind == VIDEO) {
                unsigned int req = fd_slot(b->request_fd);
                fds[req].request_video = fd; fds[req].request_output = (int)b->index;
                /* Keep the target and final-slice bit with the request. */
                fds[req].captures = (unsigned int)b->timestamp.tv_usec - 1;
#ifdef V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF
                fds[req].outputs = !(b->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF);
#else
                fds[req].outputs = 1;
#endif
            } else fds[i].output_ready |= 1u << b->index;
        } else {
            assert(!(fds[i].capture_queued & (UINT64_C(1) << b->index)));
            fds[i].capture_queued |= UINT64_C(1) << b->index;
            if (fds[i].kind == CONVERTER) fds[i].capture_ready |= UINT64_C(1) << b->index;
        }
        return 0;
    }
    if (request == MEDIA_REQUEST_IOC_QUEUE) {
        assert(fds[i].request_video >= 0 && fds[i].request_output >= 0);
        unsigned int v = fd_slot(fds[i].request_video);
        fds[v].output_ready |= 1u << fds[i].request_output;
        if (fds[i].outputs) {
            uint64_t done = UINT64_C(1) << fds[i].captures;
            if (hw_busy) { hw_held |= done; hw_slot = v; }
            else fds[v].capture_ready |= done;
        }
        fds[i].request_done = true; return 0;
    }
    if (request == MEDIA_REQUEST_IOC_REINIT) {
        if (fds[i].request_video >= 0 && fds[i].request_output >= 0) {
            unsigned int v = fd_slot(fds[i].request_video);
            fds[v].output_queued &= ~(1u << fds[i].request_output);
            fds[v].output_ready &= ~(1u << fds[i].request_output);
        }
        fds[i].request_done = false; return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        uint64_t ready = type ? fds[i].output_ready : fds[i].capture_ready;
        if (!ready || dequeue_blocked) { errno = EAGAIN; return -1; }
        unsigned int n = (unsigned int)__builtin_ctzll(ready);
        if (type) {
            fds[i].output_ready &= ~(1u << n); fds[i].output_queued &= ~(1u << n);
            b->index = output_bad_index < 0 ? n : (unsigned int)output_bad_index;
        } else {
            fds[i].capture_ready &= ~(UINT64_C(1) << n); fds[i].capture_queued &= ~(UINT64_C(1) << n);
            b->index = capture_bad_index < 0 ? n : (unsigned int)capture_bad_index;
            b->flags = (unsigned int)capture_flags;
        }
        return 0;
    }
    if (request == VIDIOC_STREAMOFF) {
        if (V4L2_TYPE_IS_OUTPUT(*(enum v4l2_buf_type *)arg))
            fds[i].output_queued = fds[i].output_ready = 0;
        else fds[i].capture_queued = fds[i].capture_ready = 0;
        return 0;
    }
    if (request == VIDIOC_REQBUFS || request == VIDIOC_STREAMON ||
        request == VIDIOC_S_EXT_CTRLS || request == VIDIOC_S_CTRL || request == VIDIOC_S_SELECTION)
        return 0;
    fprintf(stderr, "unmodelled ioctl %s %#lx\n", name, request); abort();
}
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    (void)timeout;
    assert(n == 1); fd_slot(p[0].fd);
    if (fault("poll")) return -1;
    if (poll_error) {
        errno = poll_error;
        if (!poll_error_count || !--poll_error_count) poll_error = 0;
        return -1;
    }
    if (poll_timeouts) { poll_timeouts--; return 0; }
    unsigned int i = fd_slot(p[0].fd);
    if (finish_on_poll && hw_held && fds[i].kind == VIDEO && (p[0].events & POLLIN)) {
        hw_finish(); hw_waits++;
    }
    if (fds[i].kind == VIDEO && !fds[i].output_ready && !fds[i].capture_ready) return 0;
    p[0].revents = poll_events ? poll_events : p[0].events;
    return 1;
}
int __wrap___poll_chk(struct pollfd *p, nfds_t n, int timeout, size_t size)
{
    assert(n <= size / sizeof(*p)); return __wrap_poll(p, n, timeout);
}

static VAStatus codec_init(struct v4l2r_context *ctx)
{
    int64_t value;
    return v4l2r_query_control_default(ctx, 1, &value) < 0 ?
        VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}
static VAStatus codec_end(struct v4l2r_context *ctx)
{
    if (sliced) return VA_STATUS_SUCCESS;
    unsigned char data[64] = {0};
    VAStatus status = v4l2r_append_output(ctx, data, sizeof(data));
    if (status != VA_STATUS_SUCCESS) return status;
    struct v4l2_ext_control control = {.id = 1, .value = 1};
    return v4l2r_decode(ctx, &control, 1, true, true);
}
static VAStatus codec_render(struct v4l2r_context *ctx, struct v4l2r_buffer *buffer)
{
    unsigned char flags = *(unsigned char *)buffer->data;
    VAStatus status;
    if (!(flags & 1)) {
        status = v4l2r_picture_next_output(ctx);
        if (status != VA_STATUS_SUCCESS) return status;
    }
    unsigned char data[64] = {0};
    status = v4l2r_append_output(ctx, data, sizeof(data));
    if (status != VA_STATUS_SUCCESS) return status;
    struct v4l2_ext_control control = {.id = 1, .value = 1};
    return v4l2r_decode(ctx, &control, 1, flags & 1, flags & 2);
}
static const struct v4l2r_codec codec = {
    .name = "model", .pixelformat = V4L2_PIX_FMT_H264, .priv_size = 16,
    .init = codec_init, .end_picture = codec_end, .render_buffer = codec_render,
};
static void setup(void)
{
    drv = calloc(1, sizeof(*drv)); assert(drv); va.pDriverData = drv;
    assert(!pthread_mutex_init(&drv->mutex, NULL));
    assert(!pthread_mutex_init(&drv->api_mutex, NULL));
    assert(!v4l2r_handles_init(&drv->configs, V4L2R_ID_OFFSET_CONFIG));
    assert(!v4l2r_handles_init(&drv->contexts, V4L2R_ID_OFFSET_CONTEXT));
    assert(!v4l2r_handles_init(&drv->surfaces, V4L2R_ID_OFFSET_SURFACE));
    assert(!v4l2r_handles_init(&drv->buffers, V4L2R_ID_OFFSET_BUFFER));
    assert(!v4l2r_handles_init(&drv->images, V4L2R_ID_OFFSET_IMAGE));
    drv->nb_decoders = 1;
    strcpy(drv->decoders[0].video_path, "model-video");
    strcpy(drv->decoders[0].media_path, "model-media");
    drv->decoders[0].nb_pixelformats = 1;
    drv->decoders[0].pixelformats[0] = V4L2_PIX_FMT_H264;
    strcpy(drv->converter.video_path, "model-converter");
    drv->converter_probed = drv->has_converter = true;
    drv->converter.nb_pixelformats = 2;
    drv->converter.pixelformats[0] = V4L2_PIX_FMT_NV12;
    drv->converter.pixelformats[1] = V4L2_PIX_FMT_NV16;
    cfg = v4l2r_handles_alloc(&drv->configs, sizeof(struct v4l2r_config));
    assert(cfg != VA_INVALID_ID);
    V4L2R_CONFIG(drv, cfg)->codec = &codec;
    V4L2R_CONFIG(drv, cfg)->profile = VAProfileH264Main;
    v4l2r_lock_surface_api(&table);
}
static VASurfaceID surface(void)
{
    VASurfaceID sid;
    assert(v4l2r_CreateSurfaces2(&va, VA_RT_FORMAT_YUV420, 64, 64, &sid, 1, NULL, 0) == VA_STATUS_SUCCESS);
    return sid;
}
static VAContextID context(void)
{
    VAContextID id;
    assert(table.vaCreateContext(&va, cfg, 64, 64, 0, NULL, 0, &id) == VA_STATUS_SUCCESS);
    return id;
}
static VAStatus picture(VAContextID id, VASurfaceID sid)
{
    VAStatus status = table.vaBeginPicture(&va, id, sid);
    return status == VA_STATUS_SUCCESS ? table.vaEndPicture(&va, id) : status;
}
struct resources {
    unsigned int fds[CONVERTER + 1];
    unsigned int maps, heaps, handle_arrays;
    size_t mapped_bytes, heap_bytes, handle_array_bytes;
    unsigned int configs, contexts, surfaces, buffers, images;
};
static bool handle_array(void *ptr)
{
    return ptr == drv->configs.slots || ptr == drv->contexts.slots ||
           ptr == drv->surfaces.slots || ptr == drv->buffers.slots ||
           ptr == drv->images.slots;
}
static unsigned int live_handles(struct v4l2r_handles *handles)
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < handles->size; i++) count += handles->slots[i] != NULL;
    return count;
}
static struct resources resources(void)
{
    struct resources r = {0};
    for (unsigned int i = 0; i < fd_count; i++)
        if (fds[i].live) r.fds[fds[i].kind]++;
    for (unsigned int i = 0; i < LIMIT; i++) {
        if (maps[i].ptr) { r.maps++; r.mapped_bytes += maps[i].size; }
        if (!heaps[i].ptr) continue;
        if (handle_array(heaps[i].ptr)) {
            r.handle_arrays++; r.handle_array_bytes += heaps[i].size;
        } else {
            r.heaps++; r.heap_bytes += heaps[i].size;
        }
    }
    r.configs = live_handles(&drv->configs);
    r.contexts = live_handles(&drv->contexts);
    r.surfaces = live_handles(&drv->surfaces);
    r.buffers = live_handles(&drv->buffers);
    r.images = live_handles(&drv->images);
    return r;
}
static void assert_resource_members(struct resources actual,
                                    struct resources expected)
{
    for (unsigned int i = 0; i <= CONVERTER; i++)
        assert(actual.fds[i] == expected.fds[i]);
    assert(actual.maps == expected.maps && actual.heaps == expected.heaps);
    assert(actual.mapped_bytes == expected.mapped_bytes &&
           actual.heap_bytes == expected.heap_bytes);
    assert(actual.configs == expected.configs &&
           actual.contexts == expected.contexts &&
           actual.surfaces == expected.surfaces &&
           actual.buffers == expected.buffers && actual.images == expected.images);
}
static void assert_resources(struct resources expected)
{
    struct resources actual = resources();
    assert_resource_members(actual, expected);
    assert(actual.handle_arrays == expected.handle_arrays &&
           actual.handle_array_bytes == expected.handle_array_bytes);
}
static void assert_lifecycle_resources(struct resources expected)
{
    struct resources actual = resources();
    /* Handle tables retain their bounded peak capacity until Terminate. */
    assert_resource_members(actual, expected);
    assert(actual.handle_arrays == expected.handle_arrays &&
           actual.handle_array_bytes >= expected.handle_array_bytes &&
           actual.handle_array_bytes <= expected.handle_array_bytes +
                                        32 * sizeof(void *));
    assert(drv->configs.size == 32 && drv->contexts.size <= 64 &&
           drv->surfaces.size == 32 && drv->buffers.size == 32 &&
           drv->images.size == 32);
}
static void close_export(VADRMPRIMESurfaceDescriptor *desc)
{
    for (unsigned int i = 0; i < desc->num_objects; i++) assert(!close(desc->objects[i].fd));
}
static void teardown(void)
{
    armed = false;
    assert(v4l2r_Terminate(&va) == VA_STATUS_SUCCESS);
    for (unsigned int i = 0; i < LIMIT; i++) {
        assert(!fds[i].live && !maps[i].ptr && !heaps[i].ptr);
    }
    memset(fds, 0, sizeof(fds)); fd_count = 0;
}
static void churn(const char *name)
{
    setup();
    struct resources baseline = resources();
    assert(baseline.configs == 1 && !baseline.contexts && !baseline.surfaces &&
           !baseline.buffers && !baseline.images);
    assert(baseline.handle_arrays == 5);
    for (unsigned int cycle = 1; cycle <= 1000; cycle++) {
        VAContextID id = context();
        VASurfaceID sid = surface();
        VADRMPRIMESurfaceDescriptor desc;
        if (!strcmp(name, "early-export")) {
            assert(table.vaExportSurfaceHandle(&va, sid,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) == VA_STATUS_SUCCESS);
            close_export(&desc);
        }
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        if (!strcmp(name, "normal")) {
            assert(table.vaExportSurfaceHandle(&va, sid,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) == VA_STATUS_SUCCESS);
            close_export(&desc);
        } else if (!strcmp(name, "held-image")) {
            VAImage image;
            void *mapped, *again;
            assert(v4l2r_DeriveImage(&va, sid, &image) == VA_STATUS_SUCCESS);
            assert(v4l2r_MapBuffer(&va, image.buf, &mapped) == VA_STATUS_SUCCESS);
            assert(image.data_size > 0);
            memset(mapped, 0x5a, image.data_size);
            assert(table.vaDestroyContext(&va, id) == VA_STATUS_SUCCESS);
            id = VA_INVALID_ID;
            assert(v4l2r_UnmapBuffer(&va, image.buf) == VA_STATUS_SUCCESS);
            assert(v4l2r_MapBuffer(&va, image.buf, &again) == VA_STATUS_SUCCESS);
            assert(again == mapped && ((unsigned char *)again)[0] == 0x5a &&
                   ((unsigned char *)again)[image.data_size - 1] == 0x5a);
            assert(v4l2r_UnmapBuffer(&va, image.buf) == VA_STATUS_SUCCESS);
            assert(v4l2r_DestroyImage(&va, image.image_id) == VA_STATUS_SUCCESS);
        } else {
            assert(!strcmp(name, "early-export"));
        }
        assert(table.vaDestroySurfaces(&va, &sid, 1) == VA_STATUS_SUCCESS);
        if (id != VA_INVALID_ID)
            assert(table.vaDestroyContext(&va, id) == VA_STATUS_SUCCESS);
        assert_resources(baseline);
        if (cycle == 100 || cycle == 500 || cycle == 1000)
            printf("CHURN %s checkpoint cycle %u\n", name, cycle);
    }
    teardown();
}
static void destroy_lifecycle(void)
{
    struct v4l2r_handles *tables[] = {
        &drv->contexts, &drv->images, &drv->buffers, &drv->surfaces,
    };
    for (unsigned int table_index = 0;
         table_index < sizeof(tables) / sizeof(tables[0]); table_index++) {
        for (;;) {
            unsigned int iter = 0;
            uint32_t id;
            if (!v4l2r_handles_next(tables[table_index], &iter, &id)) break;
            VAStatus status;
            if (tables[table_index] == &drv->contexts)
                status = table.vaDestroyContext(&va, id);
            else if (tables[table_index] == &drv->images)
                status = v4l2r_DestroyImage(&va, id);
            else if (tables[table_index] == &drv->buffers)
                status = v4l2r_DestroyBuffer(&va, id);
            else
                status = table.vaDestroySurfaces(&va, &id, 1);
            assert(status == VA_STATUS_SUCCESS);
        }
    }
}
static void inject(unsigned long request, int type, unsigned int nth, int error)
{
    fail_ioctl = request; fail_type = type; fail_ioctl_nth = nth;
    fail_errno = error; ioctl_matches = injected = op = 0; armed = true;
}
static void failed_surface(VASurfaceID sid)
{
    assert(table.vaSyncSurface(&va, sid) != VA_STATUS_SUCCESS);
    assert(table.vaSyncSurface(&va, sid) != VA_STATUS_SUCCESS);
    VADRMPRIMESurfaceDescriptor desc;
    assert(table.vaExportSurfaceHandle(&va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) != VA_STATUS_SUCCESS);
    VAImage image;
    assert(table.vaDeriveImage(&va, sid, &image) != VA_STATUS_SUCCESS);
    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    assert(v4l2r_CreateImage(&va, &format, 64, 64, &image) == VA_STATUS_SUCCESS);
    assert(table.vaGetImage(&va, sid, 0, 0, 64, 64, image.image_id) != VA_STATUS_SUCCESS);
    assert(v4l2r_DestroyImage(&va, image.image_id) == VA_STATUS_SUCCESS);
}
/* Run the successful operation trace, then fail each acquisition/operation
 * exactly once in a fresh fixture. No later success can erase a frame failure;
 * cached maps/exports remain owned and must all be released by Terminate. */
static unsigned int sweep_run(const char *name, unsigned int point)
{
    armed = false; fail_ioctl = 0; fail_at = 0; op = injected = 0;
    optional_failure = false;
    single_plane = strstr(name, "single") != NULL;
    packed = !strcmp(name, "decode-convert");
    pitch_retry = !strcmp(name, "vpp-stride");
    setup();
    struct resources baseline = resources();
    VASurfaceID sid = surface();
    VAContextID id = VA_INVALID_ID;
    struct v4l2r_context *ctx = NULL;
    VAImage image = {0};
    bool creating = !strncmp(name, "create", 6);
    bool create_vpp = !strcmp(name, "create-vpp") || !strcmp(name, "create-grown");
    bool exporting = !strcmp(name, "export") || !strcmp(name, "export-single") || !strcmp(name, "export-probes");
    bool vpp = !strcmp(name, "vpp") || pitch_retry;
    bool view = !strcmp(name, "view") || !strcmp(name, "view-dmabuf");
    bool pair = strstr(name, "pair") != NULL;
    bool attach_pair = !strcmp(name, "attach-pair");
    bool preserve = !strncmp(name, "preserve", 8);
    if (!strcmp(name, "export-probes")) {
        drv->decoders[0].nb_pixelformats = 2;
        drv->decoders[0].pixelformats[1] = V4L2_PIX_FMT_VP8;
    }
    if (vpp || create_vpp) {
        V4L2R_CONFIG(drv, cfg)->codec = NULL;
        V4L2R_CONFIG(drv, cfg)->profile = VAProfileNone;
    }
    if (!strcmp(name, "create-grown"))
        for (unsigned int i = 0; i < 32; i++) (void)context();
    if (!creating && !exporting) { id = context(); ctx = V4L2R_CONTEXT(drv, id); }
    if (!strcmp(name, "decode-early") || !strcmp(name, "view-dmabuf"))
        assert(v4l2r_surface_alloc_backing(drv, V4L2R_SURFACE(drv, sid)) == VA_STATUS_SUCCESS);
    if (view || preserve || (pair && !attach_pair) || !strncmp(name, "convert-", 8)) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
    }
    if (pair) {
        /* Synthetic two-memory-plane ownership fixture. It exercises partial
         * descriptor/map acquisition, not a claim about a hardware layout. */
        if (attach_pair) {
            assert(v4l2r_surface_alloc_backing(drv, V4L2R_SURFACE(drv, sid)) == VA_STATUS_SUCCESS);
            struct v4l2r_surface_backing *b = V4L2R_SURFACE(drv, sid)->backing;
            b->nb_planes = 2; b->plane_size[1] = b->plane_size[0];
            b->dmabuf_fd[1] = new_fd(DMABUF);
            ctx->capture_format = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
            format_fill(&ctx->capture_format);
            ctx->capture_format.fmt.pix_mp.num_planes = 2;
            ctx->capture_format.fmt.pix_mp.plane_fmt[1] = ctx->capture_format.fmt.pix_mp.plane_fmt[0];
            ctx->streaming = true;
        } else {
            ctx->captures[0].nb_planes = 2;
            ctx->captures[0].plane_size[1] = ctx->captures[0].plane_size[0];
        }
    }
    if (!strcmp(name, "convert-kick") || !strcmp(name, "convert-wait"))
        assert(v4l2r_convert_setup(ctx) == VA_STATUS_SUCCESS);
    if (!strcmp(name, "convert-wait")) v4l2r_convert_kick(ctx, 0);
    VASurfaceID src = VA_INVALID_ID;
    if (vpp) {
        src = surface();
        assert(v4l2r_surface_alloc_backing(drv, V4L2R_SURFACE(drv, src)) == VA_STATUS_SUCCESS);
        assert(v4l2r_surface_alloc_backing(drv, V4L2R_SURFACE(drv, sid)) == VA_STATUS_SUCCESS);
        assert(table.vaBeginPicture(&va, id, sid) == VA_STATUS_SUCCESS);
        VAProcPipelineParameterBuffer params = {.surface = src};
        VABufferID buf;
        assert(v4l2r_CreateBuffer(&va, id, VAProcPipelineParameterBufferType,
            sizeof(params), 1, &params, &buf) == VA_STATUS_SUCCESS);
        assert(table.vaRenderPicture(&va, id, &buf, 1) == VA_STATUS_SUCCESS);
    }
    if (view) {
        VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
        assert(v4l2r_CreateImage(&va, &format, 64, 64, &image) == VA_STATUS_SUCCESS);
    }
    fprintf(stderr, "SWEEP %s operation %u\n", name, point);
    op = injected = 0; fail_at = point; armed = true;
    VAStatus status;
    VADRMPRIMESurfaceDescriptor desc;
    if (creating) {
        status = table.vaCreateContext(&va, cfg, 64, 64, 0, NULL, 0, &id);
    } else if (exporting) {
        status = table.vaExportSurfaceHandle(&va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc);
    } else if (view) {
        status = table.vaGetImage(&va, sid, 0, 0, 64, 64, image.image_id);
    } else if (!strcmp(name, "grow")) {
        struct v4l2r_output_buffer *out = &ctx->output[0];
        out->bytesused = 4; memcpy(out->addr, "keep", 4);
        void *original = out->addr;
        status = v4l2r_output_buffer_grow(ctx, out, (size_t)out->size * 2) < 0 ?
            VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
        assert(!memcmp(out->addr, "keep", 4));
        if (status != VA_STATUS_SUCCESS) assert(out->addr == original);
    } else if (preserve) {
        status = table.vaDestroyContext(&va, id); id = VA_INVALID_ID;
    } else if (attach_pair) {
        status = v4l2r_context_bind_surface(ctx, V4L2R_SURFACE(drv, sid));
    } else if (!strcmp(name, "view-pair")) {
        struct v4l2r_frame_view frame;
        status = v4l2r_surface_capture_view(V4L2R_SURFACE(drv, sid), true, &frame);
    } else if (!strcmp(name, "export-pair")) {
        status = table.vaExportSurfaceHandle(&va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc);
    } else if (!strcmp(name, "convert-setup")) {
        status = v4l2r_convert_setup(ctx);
    } else if (!strcmp(name, "convert-kick")) {
        v4l2r_convert_kick(ctx, 0);
        status = table.vaSyncSurface(&va, sid);
    } else if (!strcmp(name, "convert-wait")) {
        status = table.vaSyncSurface(&va, sid);
    } else if (vpp) {
        status = table.vaEndPicture(&va, id);
    } else {
        status = picture(id, sid);
        if (status == VA_STATUS_SUCCESS) status = table.vaSyncSurface(&va, sid);
    }
    unsigned int count = op;
    armed = false; fail_at = 0;
    assert(point ? injected == 1 : status == VA_STATUS_SUCCESS);
    if (point) {
        if (optional_failure || preserve)
            assert(status == VA_STATUS_SUCCESS);
        else
            assert(status == VA_STATUS_ERROR_OPERATION_FAILED ||
                   status == VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    if (point) fprintf(stderr, "INJECTED %s: %s, status %#x\n", name, operations[point], status);
    if ((exporting || !strcmp(name, "export-pair")) && status == VA_STATUS_SUCCESS)
        for (unsigned int i = 0; i < desc.num_objects; i++) assert(!close(desc.objects[i].fd));
    if (view) {
        assert(table.vaGetImage(&va, sid, 0, 0, 64, 64, image.image_id) == VA_STATUS_SUCCESS);
    } else if (exporting) {
        assert(table.vaExportSurfaceHandle(&va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) == VA_STATUS_SUCCESS);
        for (unsigned int i = 0; i < desc.num_objects; i++) assert(!close(desc.objects[i].fd));
    } else if (!strcmp(name, "convert-setup") && status != VA_STATUS_SUCCESS) {
        assert(!ctx->conv);
        assert(v4l2r_convert_setup(ctx) == VA_STATUS_SUCCESS);
    } else if ((!strcmp(name, "convert-kick") || !strcmp(name, "convert-wait") || vpp) &&
               status != VA_STATUS_SUCCESS) {
        failed_surface(sid);
    }
    if (attach_pair) {
        assert(v4l2r_context_bind_surface(ctx, V4L2R_SURFACE(drv, sid)) == VA_STATUS_SUCCESS);
    } else if (!strcmp(name, "view-pair")) {
        struct v4l2r_frame_view frame;
        assert(v4l2r_surface_capture_view(V4L2R_SURFACE(drv, sid), true, &frame) == VA_STATUS_SUCCESS);
    }
    if (preserve && point && (!strcmp(operations[point], "calloc") ||
                             !strcmp(operations[point], "VIDIOC_EXPBUF")))
        failed_surface(sid);
    if (vpp) {
        /* A rejected VPP job tears down its instance and can retry. */
        assert(table.vaBeginPicture(&va, id, sid) == VA_STATUS_SUCCESS);
        VAProcPipelineParameterBuffer params = {.surface = src};
        VABufferID buf;
        assert(v4l2r_CreateBuffer(&va, id, VAProcPipelineParameterBufferType,
            sizeof(params), 1, &params, &buf) == VA_STATUS_SUCCESS);
        assert(table.vaRenderPicture(&va, id, &buf, 1) == VA_STATUS_SUCCESS);
        assert(table.vaEndPicture(&va, id) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        V4L2R_CONFIG(drv, cfg)->codec = &codec;
    }
    if (!strncmp(name, "decode", 6) && status != VA_STATUS_SUCCESS) {
        failed_surface(sid);
        VASurfaceID retry = surface();
        unsigned int before = queues;
        if (ctx->failed || (ctx->conv && ctx->conv->failed)) {
            assert(picture(id, retry) != VA_STATUS_SUCCESS && queues == before);
        } else {
            assert(picture(id, retry) == VA_STATUS_SUCCESS);
            assert(table.vaSyncSurface(&va, retry) == VA_STATUS_SUCCESS);
        }
    }
    if (create_vpp) V4L2R_CONFIG(drv, cfg)->codec = &codec;
    /* Recovery on a new model device cannot inherit the old queues. */
    VAContextID next = context(); VASurfaceID target = surface();
    assert(picture(next, target) == VA_STATUS_SUCCESS);
    assert(table.vaSyncSurface(&va, target) == VA_STATUS_SUCCESS);
    destroy_lifecycle();
    assert_lifecycle_resources(baseline);
    next = context(); target = surface();
    assert(picture(next, target) == VA_STATUS_SUCCESS);
    assert(table.vaSyncSurface(&va, target) == VA_STATUS_SUCCESS);
    destroy_lifecycle();
    assert_lifecycle_resources(baseline);
    teardown(); return count;
}
static void sweep(const char *name)
{
    unsigned int count = sweep_run(name, 0);
    for (unsigned int n = 1; n <= count; n++) sweep_run(name, n);
    printf("SWEEP %s: %u operation-index failures; recovery and resource balance verified\n", name, count);
}
static int free_entry(const struct v4l2r_context *ctx, unsigned int n)
{
    assert(n < ctx->free_count);
    return ctx->free_captures[(ctx->free_head + n) % V4L2R_MAX_CAPTURE_BUFFERS];
}
/* A surface handed back while its CAPTURE buffer is busy is decoded into an
 * idle buffer instead of waiting (V4L2R_REBIND), within the spare limit, and
 * only when no client holds the busy buffer's memory. */
static void rebind(const char *name)
{
    bool spares = !strcmp(name, "spare-limit") || !strcmp(name, "spare-first");
    const char *env = !strcmp(name, "off") ? "0" : spares ? "2" : NULL;
    if (env) assert(!setenv("V4L2R_REBIND", env, 1));
    else assert(!unsetenv("V4L2R_REBIND"));
    /* spare-limit covers the wait for the bound buffer (V4L2R_REBIND_FIRST=0). */
    if (!strcmp(name, "spare-limit")) assert(!setenv("V4L2R_REBIND_FIRST", "0", 1));
    else assert(!unsetenv("V4L2R_REBIND_FIRST"));
    setup();
    if (!strcmp(name, "env")) {
        static const struct { const char *value; unsigned int spare; } values[] = {
            {NULL, V4L2R_REBIND_DEFAULT}, {"0", 0}, {"-3", 0}, {"3", 3},
            {"99", V4L2R_REBIND_MAX},
        };
        for (unsigned int n = 0; n < sizeof(values) / sizeof(values[0]); n++) {
            if (values[n].value) assert(!setenv("V4L2R_REBIND", values[n].value, 1));
            else assert(!unsetenv("V4L2R_REBIND"));
            VAContextID id = context();
            assert(V4L2R_CONTEXT(drv, id)->rebind_spare == values[n].spare);
            assert(table.vaDestroyContext(&va, id) == VA_STATUS_SUCCESS);
        }
        assert(!unsetenv("V4L2R_REBIND"));
        teardown(); return;
    }
    VAContextID id = context();
    struct v4l2r_context *ctx = V4L2R_CONTEXT(drv, id);
    VASurfaceID sid = surface();
    struct v4l2r_surface *s = V4L2R_SURFACE(drv, sid);
    bool exported = !strcmp(name, "exported");
    bool derived = !strcmp(name, "derived") || !strcmp(name, "derived-held");
    VAImage image = {0};
    if (exported || derived) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        if (exported) {
            VADRMPRIMESurfaceDescriptor desc;
            assert(table.vaExportSurfaceHandle(&va, sid,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) == VA_STATUS_SUCCESS);
            close_export(&desc);
            assert(ctx->captures[s->capture_index].client_exported);
        } else {
            /* The driver exports the buffer for its own mapping. */
            assert(table.vaDeriveImage(&va, sid, &image) == VA_STATUS_SUCCESS);
            assert(!ctx->captures[s->capture_index].client_exported);
            if (!strcmp(name, "derived"))
                assert(v4l2r_DestroyImage(&va, image.image_id) == VA_STATUS_SUCCESS);
        }
    }
    assert(ctx->capture_memory == V4L2_MEMORY_MMAP || (!exported && !derived));
    /* The surface's decode stays in flight. */
    hw_busy = true;
    assert(picture(id, sid) == VA_STATUS_SUCCESS);
    assert(ctx->capture_memory == V4L2_MEMORY_MMAP);
    int busy = s->capture_index;
    unsigned int pool = ctx->nb_captures;
    uint64_t busy_ref = ctx->captures[busy].last_ref_seq;
    assert(busy >= 0 && (ctx->queued_capture & (UINT64_C(1) << busy)));
    assert(!ctx->free_count);
    finish_on_poll = true;
    if (!strcmp(name, "alloc-fail")) inject(VIDIOC_CREATE_BUFS, -1, 1, ENOMEM);
    assert(picture(id, sid) == VA_STATUS_SUCCESS);
    if (!strcmp(name, "alloc-fail")) { assert(injected == 1); armed = false; }
    bool waits = !strcmp(name, "off") || exported || !strcmp(name, "derived-held") ||
                 !strcmp(name, "alloc-fail");
    if (waits) {
        /* Same buffer, after waiting for it; the pool did not grow. */
        assert(s->capture_index == busy && hw_waits == 1);
        assert(ctx->nb_captures == pool && !ctx->free_count);
        assert(ctx->captures[busy].surface == s);
    } else {
        /* A new buffer, no wait. The busy one is orphaned with its decode
         * still in flight and its reference sequence intact. */
        int now = s->capture_index;
        assert(now >= 0 && now != busy && !hw_waits);
        assert(ctx->nb_captures == pool + 1);
        assert(ctx->captures[now].surface == s && !ctx->captures[busy].surface);
        assert(ctx->queued_capture & (UINT64_C(1) << busy));
        assert(ctx->captures[busy].last_ref_seq == busy_ref);
        assert(ctx->free_count == 1 && free_entry(ctx, 0) == busy);
        assert(v4l2r_surface_timestamp(ctx, sid) == v4l2r_capture_index_timestamp(now));
    }
    if (!strcmp(name, "target")) {
        /* Once idle, the orphan is recycled before the pool grows. */
        hw_busy = false; hw_finish();
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        VASurfaceID other = surface();
        assert(picture(id, other) == VA_STATUS_SUCCESS);
        assert(V4L2R_SURFACE(drv, other)->capture_index == busy);
        assert(ctx->nb_captures == pool + 1 && !ctx->free_count);
        assert(table.vaSyncSurface(&va, other) == VA_STATUS_SUCCESS);
    } else if (!strcmp(name, "orphan")) {
        /* Target and orphan busy: grow, and keep the free list in order. */
        int second = s->capture_index;
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        int third = s->capture_index;
        assert(!hw_waits && third != busy && third != second);
        assert(ctx->nb_captures == pool + 2 && ctx->free_count == 2);
        assert(free_entry(ctx, 0) == busy && free_entry(ctx, 1) == second);
        hw_finish();
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        assert(!ctx->queued_capture);
        /* Busy target, idle orphans: take the oldest, do not grow. */
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(s->capture_index == third);
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(!hw_waits && s->capture_index == busy);
        assert(ctx->nb_captures == pool + 2 && ctx->free_count == 2);
        assert(free_entry(ctx, 0) == second && free_entry(ctx, 1) == third);
    } else if (!strcmp(name, "spare-limit")) {
        /* V4L2R_REBIND=2: two spares, then the busy buffer is waited for. */
        assert(ctx->rebind_spare == 2);
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(!hw_waits && ctx->nb_captures == pool + 2 && ctx->free_count == 2);
        int last = s->capture_index;
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(hw_waits == 1 && s->capture_index == last);
        assert(ctx->nb_captures == pool + 2 && ctx->free_count == 2);
    } else if (!strcmp(name, "spare-first")) {
        /* V4L2R_REBIND=2, default V4L2R_REBIND_FIRST: two spares, then the
         * spare that frees first is waited for instead of the bound buffer. */
        assert(ctx->rebind_spare == 2 && ctx->rebind_first);
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(!hw_waits && ctx->nb_captures == pool + 2 && ctx->free_count == 2);
        int last = s->capture_index, first = free_entry(ctx, 0);
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(hw_waits == 1 && s->capture_index == first && first != last);
        assert(ctx->nb_captures == pool + 2 && ctx->free_count == 2);
    } else if (!strcmp(name, "off")) {
        assert(!ctx->rebind_spare);
    }
    hw_busy = finish_on_poll = false; hw_finish(); hw_waits = 0;
    assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
    if (!strcmp(name, "derived-held"))
        assert(v4l2r_DestroyImage(&va, image.image_id) == VA_STATUS_SUCCESS);
    assert(table.vaDestroyContext(&va, id) == VA_STATUS_SUCCESS);
    teardown();
}
int main(int argc, char **argv)
{
    struct rlimit core = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &core));
    assert(argc == 2);
    const char *test = argv[1];
    if (!strncmp(test, "sweep-", 6)) { sweep(test + 6); return 0; }
    if (!strncmp(test, "churn-", 6)) { churn(test + 6); return 0; }
    if (!strncmp(test, "rebind-", 7)) { rebind(test + 7); return 0; }
    hold = sliced = !strncmp(test, "partial-", 8);
    setup();
    VAContextID id = context(); VASurfaceID sid = surface();
    struct v4l2r_context *ctx = V4L2R_CONTEXT(drv, id);
    if (!strcmp(test, "control")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
    } else if (!strcmp(test, "rollback-error")) {
        fail_rollback = true;
        inject(VIDIOC_QBUF, 0, 1, EIO);
        assert(picture(id, sid) != VA_STATUS_SUCCESS && injected == 2);
        armed = false;
        unsigned int before = queues;
        assert(picture(id, surface()) != VA_STATUS_SUCCESS && queues == before);
        failed_surface(sid);
    } else if (!strcmp(test, "capture-queue") || !strcmp(test, "request-queue")) {
        inject(!strcmp(test, "capture-queue") ? VIDIOC_QBUF : MEDIA_REQUEST_IOC_QUEUE,
               !strcmp(test, "capture-queue") ? 0 : -1, 1, EIO);
        assert(picture(id, sid) != VA_STATUS_SUCCESS && injected == 1);
        armed = false; failed_surface(sid);
        if (!strcmp(test, "capture-queue")) {
            /* An idle request can be reinitialized. Exercise a full ring
             * twice: without release the stranded OUTPUT slot times out. */
            for (unsigned int n = 0; n < 2 * V4L2R_OUTPUT_BUFFERS; n++) {
                VASurfaceID next = surface();
                assert(picture(id, next) == VA_STATUS_SUCCESS);
                assert(table.vaSyncSurface(&va, next) == VA_STATUS_SUCCESS);
            }
            assert(picture(id, sid) == VA_STATUS_SUCCESS);
            assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        } else {
            unsigned int before = queues;
            /* CAPTURE already belongs to the queue: don't allow a later
             * request to consume the failed frame's destination. */
            assert(picture(id, surface()) != VA_STATUS_SUCCESS && queues == before);
        }
        assert(table.vaDestroyContext(&va, id) == VA_STATUS_SUCCESS);
        id = context(); assert(picture(id, surface()) == VA_STATUS_SUCCESS);
    } else if (!strncmp(test, "partial-", 8)) {
        assert(table.vaBeginPicture(&va, id, sid) == VA_STATUS_SUCCESS);
        unsigned char first = 1, last = 2;
        VABufferID one, two;
        assert(v4l2r_CreateBuffer(&va, id, VAPictureParameterBufferType, 1, 1, &first, &one) == VA_STATUS_SUCCESS);
        assert(v4l2r_CreateBuffer(&va, id, VAPictureParameterBufferType, 1, 1, &last, &two) == VA_STATUS_SUCCESS);
        assert(table.vaRenderPicture(&va, id, &one, 1) == VA_STATUS_SUCCESS);
        assert(ctx->queued_capture);
        if (!strcmp(test, "partial-client")) two = VA_INVALID_ID;
        else if (!strcmp(test, "partial-controls")) inject(VIDIOC_S_EXT_CTRLS, -1, 1, EIO);
        else if (!strcmp(test, "partial-output")) inject(VIDIOC_QBUF, 1, 1, EIO);
        else inject(MEDIA_REQUEST_IOC_QUEUE, -1, 1, EIO);
        VAStatus status = table.vaRenderPicture(&va, id, &two, 1);
        assert(status != VA_STATUS_SUCCESS);
        assert(table.vaEndPicture(&va, id) == status);
        armed = false;
        unsigned int before = queues;
        assert(picture(id, surface()) != VA_STATUS_SUCCESS && queues == before);
        failed_surface(sid);
        /* Destroying the failed context releases every held request. */
        assert(table.vaDestroyContext(&va, id) == VA_STATUS_SUCCESS);
        sliced = false; id = context();
        assert(picture(id, surface()) == VA_STATUS_SUCCESS);
    } else if (!strcmp(test, "dequeue-eintr") || !strcmp(test, "dequeue-eagain")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        inject(VIDIOC_DQBUF, 0, 1, !strcmp(test, "dequeue-eintr") ? EINTR : EAGAIN);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS && injected == 1);
    } else if (!strcmp(test, "dequeue-timeout") || !strcmp(test, "dequeue-stale-ready")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        dequeue_blocked = true;
        if (!strcmp(test, "dequeue-timeout")) poll_timeouts = 1;
        else fast_clock = true;
        failed_surface(sid);
        assert(model_ns < UINT64_C(3000000000));
        dequeue_blocked = fast_clock = false;
    } else if (!strcmp(test, "dequeue-index")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        capture_bad_index = 63;
        failed_surface(sid);
    } else if (!strcmp(test, "dequeue-error")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        inject(VIDIOC_DQBUF, 0, 1, EIO);
        failed_surface(sid); assert(injected == 1);
    } else if (!strcmp(test, "poll-interrupted")) {
        poll_error = EINTR;
        assert(v4l2r_poll_one(ctx->media_fd, POLLIN, 10) == 0);
    } else if (!strcmp(test, "poll-interrupt-deadline")) {
        poll_error = EINTR; poll_error_count = 500; fast_clock = true;
        assert(v4l2r_poll_one(ctx->media_fd, POLLIN, 2000) == -ETIMEDOUT);
        assert(poll_error_count > 0 && model_ns < UINT64_C(2100000000));
        fast_clock = false; poll_error = poll_error_count = 0;
    } else if (!strcmp(test, "converter-error-flag") || !strcmp(test, "converter-source-error") ||
               !strcmp(test, "converter-index") || !strcmp(test, "converter-source-index") ||
               !strcmp(test, "converter-stale-ready") || !strcmp(test, "converter-eintr") ||
               !strcmp(test, "converter-eagain") || !strcmp(test, "converter-source-eagain")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        assert(v4l2r_convert_setup(ctx) == VA_STATUS_SUCCESS);
        v4l2r_convert_kick(ctx, 0);
        assert(V4L2R_SURFACE(drv, sid)->convert_pending);
        bool transient = false;
        if (!strcmp(test, "converter-error-flag")) capture_flags = V4L2_BUF_FLAG_ERROR;
        else if (!strcmp(test, "converter-index")) capture_bad_index = V4L2R_CONVERT_SLOTS;
        else if (!strcmp(test, "converter-source-index")) output_bad_index = V4L2R_CONVERT_SLOTS;
        else if (!strcmp(test, "converter-stale-ready")) dequeue_blocked = fast_clock = true;
        else if (!strcmp(test, "converter-source-error")) inject(VIDIOC_DQBUF, 1, 1, EIO);
        else {
            transient = true;
            inject(VIDIOC_DQBUF, !strcmp(test, "converter-source-eagain") ? 1 : 0, 1,
                   !strcmp(test, "converter-eintr") ? EINTR : EAGAIN);
        }
        if (transient) {
            assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS && injected == 1);
            assert(!V4L2R_SURFACE(drv, sid)->convert_pending);
        } else {
            failed_surface(sid);
            assert(ctx->conv->failed && ctx->conv->fd == -1 && !ctx->conv->busy);
            assert(table.vaDestroySurfaces(&va, &sid, 1) == VA_STATUS_SUCCESS);
        }
        assert(model_ns < UINT64_C(3000000000));
        dequeue_blocked = fast_clock = false;
    } else if (!strcmp(test, "converter-export-error")) {
        assert(picture(id, sid) == VA_STATUS_SUCCESS);
        assert(table.vaSyncSurface(&va, sid) == VA_STATUS_SUCCESS);
        assert(v4l2r_convert_setup(ctx) == VA_STATUS_SUCCESS);
        /* Prepare destination so injection targets the source export. */
        assert(v4l2r_surface_convert_backing(drv, V4L2R_SURFACE(drv, sid)) == VA_STATUS_SUCCESS);
        inject(VIDIOC_EXPBUF, -1, 1, EIO);
        v4l2r_convert_kick(ctx, 0); assert(injected == 1);
        failed_surface(sid);
    } else { assert(!"unknown case"); }
    teardown(); return 0;
}
