/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AV1 pending-picture controls execute against an in-memory V4L2 device.
 * Captured request payloads are compared with the optional client trace. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/media.h>
#define v4l2r_codec_av1 v4l2r_test_codec_av1
#include "../src/codec_av1.c"

/* --- in-memory device: captures controls, completes queues in order --- */

struct submission {
    int request_fd;
    unsigned int count;
    struct { uint32_t id; uint32_t size; void *data; } ctrl[8];
};

#define MAX_SUBMISSIONS 64
static struct submission log_[MAX_SUBMISSIONS];
static unsigned int log_count;
static unsigned int requests_queued;
static bool fail_set_controls;

static unsigned int fifo_out[64], fifo_out_head, fifo_out_tail;
static unsigned int fifo_cap[64], fifo_cap_head, fifo_cap_tail;
static struct timeval queued_ts[64];

static void log_reset(void)
{
    for (unsigned int i = 0; i < log_count; i++)
        for (unsigned int c = 0; c < log_[i].count; c++)
            free(log_[i].ctrl[c].data);
    memset(log_, 0, sizeof(log_));
    log_count = 0;
    requests_queued = 0;
    fifo_out_head = fifo_out_tail = fifo_cap_head = fifo_cap_tail = 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (request == VIDIOC_S_EXT_CTRLS) {
        const struct v4l2_ext_controls *ext = arg;
        struct submission *s;

        if (fail_set_controls) {
            errno = EINVAL;
            return -1;
        }
        assert(log_count < MAX_SUBMISSIONS && ext->count <= 8);
        s = &log_[log_count++];
        s->request_fd = ext->which == V4L2_CTRL_WHICH_REQUEST_VAL ? ext->request_fd : -1;
        s->count = ext->count;
        for (unsigned int i = 0; i < ext->count; i++) {
            s->ctrl[i].id = ext->controls[i].id;
            s->ctrl[i].size = ext->controls[i].size;
            s->ctrl[i].data = malloc(ext->controls[i].size);
            assert(s->ctrl[i].data);
            memcpy(s->ctrl[i].data, ext->controls[i].ptr, ext->controls[i].size);
        }
        return 0;
    }
    if (request == MEDIA_REQUEST_IOC_REINIT)
        return 0;
    if (request == MEDIA_REQUEST_IOC_QUEUE) {
        requests_queued++;
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        const struct v4l2_buffer *b = arg;
        if (V4L2_TYPE_IS_OUTPUT(b->type)) {
            queued_ts[fifo_out_tail % 64] = b->timestamp;
            fifo_out[fifo_out_tail++ % 64] = b->index;
        }
        else
            fifo_cap[fifo_cap_tail++ % 64] = b->index;
        return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        /* Completion in submission order, immediately. */
        if (V4L2_TYPE_IS_OUTPUT(b->type)) {
            if (fifo_out_head == fifo_out_tail) { errno = EAGAIN; return -1; }
            b->index = fifo_out[fifo_out_head++ % 64];
        } else {
            if (fifo_cap_head == fifo_cap_tail) { errno = EAGAIN; return -1; }
            b->index = fifo_cap[fifo_cap_head++ % 64];
        }
        b->flags = 0;
        return 0;
    }
    if (request == VIDIOC_STREAMOFF)
        return 0;
    fprintf(stderr, "unexpected ioctl %#lx\n", request);
    abort();
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout;
    for (nfds_t i = 0; i < count; i++)
        fds[i].revents = fds[i].events;
    return (int)count;
}

int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t size)
{
    assert(count <= size / sizeof(*fds));
    return __wrap_poll(fds, count, timeout);
}

/* --- a registered, streaming AV1 context with four bound surfaces --- */

#define NB_SURFACES 4
static struct v4l2r_driver drv;
static struct VADriverContext va;
static struct v4l2r_context *ctx;
static VAContextID cid;
static struct av1_context codec;
static VASurfaceID sid[NB_SURFACES];
static unsigned char output_mem[V4L2R_OUTPUT_BUFFERS][8192];

static void setup(void)
{
    memset(&drv, 0, sizeof(drv));
    memset(&codec, 0, sizeof(codec));
    assert(!pthread_mutex_init(&drv.mutex, NULL));
    assert(!pthread_mutex_init(&drv.api_mutex, NULL));
    assert(!v4l2r_handles_init(&drv.configs, V4L2R_ID_OFFSET_CONFIG));
    assert(!v4l2r_handles_init(&drv.surfaces, V4L2R_ID_OFFSET_SURFACE));
    assert(!v4l2r_handles_init(&drv.images, V4L2R_ID_OFFSET_IMAGE));
    assert(!v4l2r_handles_init(&drv.buffers, V4L2R_ID_OFFSET_BUFFER));
    assert(!v4l2r_handles_init(&drv.contexts, V4L2R_ID_OFFSET_CONTEXT));
    va.pDriverData = &drv;
    assert(v4l2r_CreateSurfaces2(&va, VA_RT_FORMAT_YUV420, 64, 64, sid,
                                 NB_SURFACES, NULL, 0) == VA_STATUS_SUCCESS);

    cid = v4l2r_handles_alloc(&drv.contexts, sizeof(*ctx));
    ctx = V4L2R_CONTEXT(&drv, cid);
    assert(ctx && !pthread_mutex_init(&ctx->mutex, NULL));
    ctx->drv = &drv;
    ctx->id = cid;
    ctx->diag_serial = v4l2r_diag_context_serial();
    ctx->profile = VAProfileAV1Profile0;
    ctx->codec = &v4l2r_test_codec_av1;
    ctx->codec_priv = &codec;
    ctx->bit_depth = 8;
    ctx->is_avd = true;
    ctx->video_fd = 100;
    ctx->media_fd = -1;
    ctx->streaming = true;
    ctx->nb_captures = NB_SURFACES;
    ctx->capture_memory = V4L2_MEMORY_MMAP;
    ctx->capture_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ctx->capture_format.fmt.pix_mp = (struct v4l2_pix_format_mplane) {
        .width = 64, .height = 64, .pixelformat = V4L2_PIX_FMT_NV12,
        .num_planes = 1, .plane_fmt = {{ .bytesperline = 64, .sizeimage = 64 * 96 }},
    };
    ctx->output_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    for (unsigned int i = 0; i < V4L2R_OUTPUT_BUFFERS; i++) {
        ctx->output[i].index = i;
        ctx->output[i].request_fd = 200 + (int)i;
        ctx->output[i].addr = output_mem[i];
        ctx->output[i].size = sizeof(output_mem[i]);
    }
    for (unsigned int i = 0; i < NB_SURFACES; i++) {
        struct v4l2r_surface *surface = V4L2R_SURFACE(&drv, sid[i]);

        ctx->captures[i].surface = surface;
        ctx->captures[i].nb_planes = 1;
        ctx->captures[i].plane_size[0] = 64 * 96;
        for (unsigned int p = 0; p < VIDEO_MAX_PLANES; p++)
            ctx->captures[i].dmabuf_fd[p] = -1;
        surface->ctx = ctx;
        surface->capture_index = (int)i;
    }
}

static void teardown(void)
{
    av1_uninit(ctx);
    v4l2r_handles_destroy(&drv.contexts);
    v4l2r_handles_destroy(&drv.buffers);
    v4l2r_handles_destroy(&drv.images);
    v4l2r_handles_destroy(&drv.surfaces);
    v4l2r_handles_destroy(&drv.configs);
}


static VADecPictureParameterBufferAV1 pic;
static VASliceParameterBufferAV1 slice;
static unsigned char tiles[16];

static void picture(unsigned int target, unsigned int type, unsigned int hint)
{
    memset(&pic, 0, sizeof(pic));
    pic.current_frame = sid[target];
    pic.frame_width_minus1 = pic.frame_height_minus1 = 63;
    pic.tile_cols = pic.tile_rows = 1;
    pic.primary_ref_frame = 7;
    pic.pic_info_fields.bits.frame_type = type;
    pic.pic_info_fields.bits.show_frame = 1;
    pic.seq_info_fields.fields.subsampling_x = 1;
    pic.seq_info_fields.fields.subsampling_y = 1;
    pic.seq_info_fields.fields.enable_order_hint = 1;
    pic.order_hint_bits_minus_1 = 7;
    pic.order_hint = hint;
    for (unsigned int i = 0; i < 8; i++)
        pic.ref_frame_map[i] = VA_INVALID_SURFACE;
    memset(&slice, 0, sizeof(slice));
    slice.slice_data_size = sizeof(tiles);
}

static VAStatus assemble(void)
{
    struct v4l2r_surface *target = V4L2R_SURFACE_GET(&drv, pic.current_frame);
    struct v4l2r_buffer buf;
    VAStatus status;
    assert(v4l2r_picture_begin(ctx, target) == VA_STATUS_SUCCESS);
    ctx->in_picture = true;
    assert(av1_begin_picture(ctx) == VA_STATUS_SUCCESS);
    buf = (struct v4l2r_buffer) { .type = VAPictureParameterBufferType,
        .data = &pic, .nb_elements = 1, .element_size = sizeof(pic) };
    status = av1_render_buffer(ctx, &buf);
    if (status == VA_STATUS_SUCCESS) {
        buf = (struct v4l2r_buffer) { .type = VASliceParameterBufferType,
            .data = &slice, .nb_elements = 1, .element_size = sizeof(slice) };
        status = av1_render_buffer(ctx, &buf);
    }
    if (status == VA_STATUS_SUCCESS) {
        buf = (struct v4l2r_buffer) { .type = VASliceDataBufferType,
            .data = tiles, .nb_elements = 1, .element_size = sizeof(tiles) };
        status = av1_render_buffer(ctx, &buf);
    }
    if (status == VA_STATUS_SUCCESS)
        status = av1_end_picture(ctx);
    ctx->in_picture = false;
    ctx->pic = (struct v4l2r_picture){0};
    return status;
}

static const struct v4l2_ctrl_av1_frame *frame(unsigned int n)
{
    assert(n < log_count);
    for (unsigned int i = 0; i < log_[n].count; i++)
        if (log_[n].ctrl[i].id == V4L2_CID_STATELESS_AV1_FRAME)
            return log_[n].ctrl[i].data;
    abort();
}

static void snapshot(const char *name)
{
    FILE *out = fopen(name, "wb");
    assert(out);
    assert(fwrite(&log_count, sizeof(log_count), 1, out) == 1);
    assert(fwrite(&requests_queued, sizeof(requests_queued), 1, out) == 1);
    assert(fwrite(queued_ts, sizeof(queued_ts[0]), fifo_out_tail, out) == fifo_out_tail);
    assert(fwrite(fifo_cap, sizeof(fifo_cap[0]), fifo_cap_tail, out) == fifo_cap_tail);
    for (unsigned int n = 0; n < log_count; n++) {
        assert(fwrite(&log_[n].request_fd, sizeof(int), 1, out) == 1);
        for (unsigned int i = 0; i < log_[n].count; i++) {
            assert(fwrite(&log_[n].ctrl[i].id, sizeof(uint32_t), 1, out) == 1);
            assert(fwrite(&log_[n].ctrl[i].size, sizeof(uint32_t), 1, out) == 1);
            assert(fwrite(log_[n].ctrl[i].data, log_[n].ctrl[i].size, 1, out) == 1);
        }
    }
    assert(!fclose(out));
}

static void admission_case(const char *name)
{
    bool reject = !strcmp(name, "required-missing") || !strcmp(name, "required-foreign") ||
                  !strcmp(name, "required-failed") || !strcmp(name, "required-detached") ||
                  !strcmp(name, "primary-bounds") || !strcmp(name, "slot-bounds");
    picture(0, 0, 0);
    assert(assemble() == VA_STATUS_SUCCESS && !requests_queued);
    picture(1, 1, 4);
    pic.ref_frame_map[0] = sid[0];
    pic.primary_ref_frame = 0;
    assert(assemble() == VA_STATUS_SUCCESS && requests_queued == 1);
    picture(2, 1, 2);
    pic.ref_frame_map[3] = sid[1];
    pic.ref_frame_idx[1] = 3;
    pic.primary_ref_frame = 1;
    if (!strcmp(name, "required-missing"))
        pic.ref_frame_map[3] = VA_INVALID_SURFACE;
    if (!strcmp(name, "required-foreign") || !strcmp(name, "required-failed") ||
        !strcmp(name, "required-detached")) {
        struct v4l2r_surface *s = V4L2R_SURFACE(&drv, sid[3]);
        pic.ref_frame_map[3] = sid[3];
        if (!strcmp(name, "required-foreign")) s->ctx = NULL;
        if (!strcmp(name, "required-failed")) s->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
        if (!strcmp(name, "required-detached")) s->capture_index = -1;
    }
    if (!strcmp(name, "primary-bounds")) pic.primary_ref_frame = 8;
    if (!strcmp(name, "slot-bounds")) pic.ref_frame_idx[1] = 8;
    if (!strcmp(name, "key-default")) {
        pic.pic_info_fields.bits.frame_type = 0;
        pic.ref_frame_map[3] = VA_INVALID_SURFACE;
    }
    if (!strcmp(name, "intra-default")) {
        pic.pic_info_fields.bits.frame_type = 2;
        pic.ref_frame_map[3] = VA_INVALID_SURFACE;
    }
    if (!strcmp(name, "resilient-default")) {
        pic.pic_info_fields.bits.error_resilient_mode = 1;
        pic.ref_frame_map[3] = VA_INVALID_SURFACE;
    }
    if (!strcmp(name, "none-default")) {
        pic.primary_ref_frame = 7;
        pic.ref_frame_map[3] = VA_INVALID_SURFACE;
    }
    assert(assemble() == (reject ? VA_STATUS_ERROR_INVALID_BUFFER : VA_STATUS_SUCCESS));
    assert(requests_queued == 2 && log_count == 2);
    assert(codec.pend_valid == !reject);
    assert(av1_flush(ctx, NULL) == VA_STATUS_SUCCESS);
    assert(requests_queued == (reject ? 2u : 3u));
    if (!reject) {
        assert(frame(2)->primary_ref_frame == pic.primary_ref_frame);
        assert(frame(2)->reference_frame_ts[3] ==
               (!strcmp(name, "valid-primary") ? 2000u : 0u));
        for (unsigned int i = 0; i < 8; i++)
            if (i != 3) assert(!frame(2)->reference_frame_ts[i]);
    }
}

/* Exercise the production codec entry points, including a previously held
 * grain-free picture and clean continuation after an unsupported picture. */
static void grain_case(const char *name)
{
    bool apply = strcmp(name, "grain-sequence-only") != 0;
    bool reject = strcmp(name, "grain-sequence-only") != 0 &&
                  strcmp(name, "grain-other-inplace") != 0;
    ctx->is_avd = strcmp(name, "grain-other-inplace") != 0 &&
                  strcmp(name, "grain-other-display") != 0 &&
                  strcmp(name, "grain-no-control") != 0;
    codec.has_film_grain = strcmp(name, "grain-no-control") != 0;
    picture(0, 0, 0);
    assert(assemble() == VA_STATUS_SUCCESS);
    assert(codec.pend_valid && !requests_queued);
    picture(1, 0, 1);
    pic.seq_info_fields.fields.film_grain_params_present = 1;
    pic.film_grain_info.film_grain_info_fields.bits.apply_grain = apply;
    pic.current_display_picture = !strcmp(name, "grain-avd-inplace") ||
                                  !strcmp(name, "grain-other-inplace") ||
                                  !strcmp(name, "grain-no-control") ?
                                  pic.current_frame : sid[2];
    assert(assemble() == (reject ? VA_STATUS_ERROR_UNIMPLEMENTED : VA_STATUS_SUCCESS));
    if (reject) {
        assert(!codec.have_pic && codec.pend_valid);
        assert(codec.pend_current == sid[0]);
        assert(!requests_queued && !log_count);
        assert(av1_flush(ctx, NULL) == VA_STATUS_SUCCESS);
        assert(requests_queued == 1 && frame(0)->order_hint == 0);
    } else {
        assert(requests_queued == 1 && codec.pend_current == sid[1]);
        assert(av1_flush(ctx, NULL) == VA_STATUS_SUCCESS);
        assert(requests_queued == 2 && frame(1)->order_hint == 1);
    }
    picture(3, 0, 3);
    assert(assemble() == VA_STATUS_SUCCESS);
    assert(av1_flush(ctx, NULL) == VA_STATUS_SUCCESS);
    assert(requests_queued == (reject ? 2u : 3u));
    assert(frame(requests_queued - 1)->order_hint == 3);
}

static pthread_barrier_t gate_barrier;
static bool gate_expected;

static void *gate_thread(void *unused)
{
    (void)unused;
    int ret = pthread_barrier_wait(&gate_barrier);
    assert(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(v4l2r_trace_enabled() == gate_expected);
    v4l2r_trace("concurrent trace gate\n");
    return NULL;
}

static void gate_case(const char *name, FILE *sink)
{
    pthread_t threads[32];
    char text[8192];
    gate_expected = strcmp(name, "gate-unset") != 0;
    if (gate_expected)
        assert(!setenv("LIBVA_V4L2_TRACE", !strcmp(name, "gate-empty") ? "" : "1", 1));
    else
        assert(!unsetenv("LIBVA_V4L2_TRACE"));
    assert(!pthread_barrier_init(&gate_barrier, NULL, 32));
    for (unsigned int i = 0; i < 32; i++)
        assert(!pthread_create(&threads[i], NULL, gate_thread, NULL));
    for (unsigned int i = 0; i < 32; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&gate_barrier));
    if (gate_expected) assert(!unsetenv("LIBVA_V4L2_TRACE"));
    else assert(!setenv("LIBVA_V4L2_TRACE", "1", 1));
    assert(v4l2r_trace_enabled() == gate_expected); /* First observation is retained. */
    fflush(stderr);
    rewind(sink);
    text[fread(text, 1, sizeof(text) - 1, sink)] = 0;
    unsigned int count = 0;
    for (char *p = text; (p = strstr(p, "concurrent trace gate")); p++) count++;
    assert(count == (gate_expected ? 32u : 0u));
}

int main(int argc, char **argv)
{
    char text[32768], match[256], log_path[4096];
    assert(argc == 4);
    assert(snprintf(log_path, sizeof(log_path), "%s.log", argv[3]) < (int)sizeof(log_path));
    FILE *sink = fopen(log_path, "w+");
    assert(sink);
    if (!strncmp(argv[1], "gate-", 5)) {
        assert(dup2(fileno(sink), STDERR_FILENO) == STDERR_FILENO);
        gate_case(argv[1], sink);
        fclose(sink);
        printf("PASS %s concurrent first-observation trace gate\n", argv[1]);
        return 0;
    }
    bool enabled = !strcmp(argv[2], "on");
    if (enabled)
        assert(!setenv("LIBVA_V4L2_TRACE", "1", 1));
    else
        assert(!unsetenv("LIBVA_V4L2_TRACE"));
    assert(dup2(fileno(sink), STDERR_FILENO) == STDERR_FILENO);
    setup();
    if (!strncmp(argv[1], "grain-", 6)) {
        grain_case(argv[1]);
        snapshot(argv[3]);
        teardown();
        log_reset();
        fclose(sink);
        printf("PASS %s trace=%s grain admission and pending-picture preservation\n", argv[1], argv[2]);
        return 0;
    }
    if (!strncmp(argv[1], "required-", 9) || !strcmp(argv[1], "primary-bounds") ||
        !strcmp(argv[1], "slot-bounds") || !strcmp(argv[1], "valid-primary") ||
        !strcmp(argv[1], "key-default") || !strcmp(argv[1], "intra-default") ||
        !strcmp(argv[1], "resilient-default") || !strcmp(argv[1], "none-default")) {
        admission_case(argv[1]);
        snapshot(argv[3]);
        fflush(stderr);
        rewind(sink);
        text[fread(text, 1, sizeof(text) - 1, sink)] = 0;
        if (!enabled) assert(!strstr(text, "AV1 job"));
        else {
            assert(strstr(text, "job=1 target=") && strstr(text, "job=2 target="));
            assert((strstr(text, "job=3 target=") != NULL) == (requests_queued == 3));
            if (requests_queued == 3)
                assert(strstr(text, !strcmp(argv[1], "valid-primary") ?
                       "primary=1 cdf=reference cdf_slot=3" : "cdf=default cdf_slot=-1"));
        }
        teardown();
        log_reset();
        fclose(sink);
        printf("PASS %s trace=%s primary-reference admission and queue boundary\n", argv[1], argv[2]);
        return 0;
    }
    picture(0, 0, 0);
    assert(assemble() == VA_STATUS_SUCCESS);
    assert(!requests_queued); /* EndPicture defers the key picture. */
    if (!strcmp(argv[1], "failure")) {
        fail_set_controls = true;
        assert(av1_flush(ctx, NULL) == VA_STATUS_ERROR_OPERATION_FAILED);
        assert(!requests_queued && !log_count);
    } else {
        picture(1, 1, 4);
        for (unsigned int i = 0; i < 8; i++)
            pic.ref_frame_map[i] = sid[0];
        pic.primary_ref_frame = 0;
        assert(assemble() == VA_STATUS_SUCCESS);
        assert(requests_queued == 1 && log_count == 1);
        assert(frame(0)->refresh_frame_flags == 0xff);
        picture(2, 1, 2); /* Hidden reordered picture; slot3 owns hint4. */
        for (unsigned int i = 0; i < 8; i++)
            pic.ref_frame_map[i] = sid[0];
        pic.ref_frame_map[3] = sid[1];
        pic.ref_frame_idx[1] = 3;
        pic.primary_ref_frame = 1;
        pic.pic_info_fields.bits.show_frame = 0;
        if (!strcmp(argv[1], "default") || !strcmp(argv[1], "default-missing") ||
            !strcmp(argv[1], "default-foreign"))
            pic.pic_info_fields.bits.error_resilient_mode = 1;
        if (!strcmp(argv[1], "default-missing"))
            pic.ref_frame_map[3] = VA_INVALID_SURFACE;
        if (!strcmp(argv[1], "default-foreign"))
            {
            pic.ref_frame_map[3] = sid[3];
            V4L2R_SURFACE(&drv, sid[3])->ctx = NULL;
            V4L2R_SURFACE(&drv, sid[3])->codec_tag = 4;
        }
        assert(assemble() == VA_STATUS_SUCCESS);
        assert(requests_queued == 2 && log_count == 2);
        assert(frame(1)->reference_frame_ts[0] == 1000);
        assert(frame(1)->primary_ref_frame == 0);
        assert(av1_flush(ctx, V4L2R_SURFACE(&drv, sid[3])) == VA_STATUS_SUCCESS);
        assert(requests_queued == 2); /* An unrelated read must not flush. */
        assert(av1_flush(ctx, NULL) == VA_STATUS_SUCCESS);
        assert(requests_queued == 3 && log_count == 3);
        for (unsigned int n = 0; n < 3; n++) {
            assert(fifo_cap[n] == n);
            assert(queued_ts[n].tv_sec == 0 && queued_ts[n].tv_usec == (long)n + 1);
            assert(log_[n].request_fd == 200 + (int)n);
        }
        const struct v4l2_ctrl_av1_frame *f = frame(2);
        assert(f->primary_ref_frame == 1 && f->ref_frame_idx[1] == 3);
        assert(f->reference_frame_ts[0] == 1000);
        assert(f->reference_frame_ts[3] ==
               ((!strcmp(argv[1], "default-missing") || !strcmp(argv[1], "default-foreign")) ? 0 : 2000));
        assert(f->order_hints[2] == (!strcmp(argv[1], "default-missing") ? 0 : 4));
        assert(ctx->captures[0].last_ref_seq == 3);
        if (!strcmp(argv[1], "basic") || !strcmp(argv[1], "default"))
            assert(ctx->captures[1].last_ref_seq == 3);
        assert(av1_flush(ctx, NULL) == VA_STATUS_SUCCESS && requests_queued == 3);
    }
    snapshot(argv[3]);
    fflush(stderr);
    rewind(sink);
    size_t n = fread(text, 1, sizeof(text) - 1, sink);
    text[n] = 0;
    if (!enabled)
        assert(!strstr(text, "AV1 job") && !strstr(text, "AV1 dpb"));
    else if (!strcmp(argv[1], "failure")) {
        assert(strstr(text, "job=1 target=") && strstr(text, "status=1 submitted=0"));
    } else {
        assert(strstr(text, "job=1 target=") && strstr(text, "cdf=default cdf_slot=-1"));
        assert(strstr(text, "job=3 target=") && strstr(text, "primary=1"));
        assert(strstr(text, (!strcmp(argv[1], "default") || !strcmp(argv[1], "default-missing") ||
                !strcmp(argv[1], "default-foreign")) ?
               "primary=1 cdf=default cdf_slot=-1" : "primary=1 cdf=reference cdf_slot=3"));
        for (unsigned int job = 1; job <= 3; job++)
            for (unsigned int slot = 0; slot < 8; slot++) {
                snprintf(match, sizeof(match), "job=%u slot=%u ", job, slot);
                assert(strstr(text, match));
            }
        for (unsigned int job = 1; job <= 3; job++) {
            const struct v4l2_ctrl_av1_frame *f = frame(job - 1);
            for (unsigned int i = 0; i < 8; i++) {
                VASurfaceID id = job == 1 ? VA_INVALID_SURFACE :
                                 job == 2 ? sid[0] : pic.ref_frame_map[i];
                snprintf(match, sizeof(match), "job=%u slot=%u surface=0x%08x timestamp=%llu",
                         job, i, id, (unsigned long long)f->reference_frame_ts[i]);
                assert(strstr(text, match));
            }
        }
        assert(strstr(text, "idx=0,3,0,0,0,0,0 hints=0,"));
    }
    teardown();
    log_reset();
    fclose(sink);
    printf("PASS %s trace=%s captured request payloads and pending lifetime\n", argv[1], argv[2]);
    return 0;
}
