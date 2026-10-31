/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in HEVC reference-control trace (issue #84). Every record must match
 * the controls the driver actually queued, tracing must not change those
 * controls or the VA status, and nothing is written unless enabled. The
 * HEVC backend's codec ops are driven against an in-memory V4L2 device that
 * captures every VIDIOC_S_EXT_CTRLS payload; the trace is then checked
 * against that device-side capture, not against the driver's own state. */
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
#define v4l2r_codec_hevc v4l2r_test_codec_hevc
#include "../src/codec_hevc.c"

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
static unsigned int last_output_bytesused;
static bool fail_set_controls;

static unsigned int fifo_out[64], fifo_out_head, fifo_out_tail;
static unsigned int fifo_cap[64], fifo_cap_head, fifo_cap_tail;

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
            last_output_bytesused = b->m.planes[0].bytesused;
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

/* --- a registered, streaming HEVC context with four bound surfaces --- */

#define NB_SURFACES 4
static struct v4l2r_driver drv;
static struct VADriverContext va;
static struct v4l2r_context *ctx;
static VAContextID cid;
static struct hevc_context codec;
static VASurfaceID sid[NB_SURFACES];
static unsigned char output_mem[V4L2R_OUTPUT_BUFFERS][8192];

static void setup(bool avd, int64_t decode_mode, unsigned int max_slice_params)
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
    ctx->profile = VAProfileHEVCMain;
    ctx->codec = &v4l2r_test_codec_hevc;
    ctx->codec_priv = &codec;
    ctx->bit_depth = 8;
    ctx->is_avd = avd;
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
    codec.decode_mode = decode_mode;
    codec.start_code = V4L2_STATELESS_HEVC_START_CODE_NONE;
    codec.max_slice_params = max_slice_params;
    codec.max_entry_point_offsets = 0;
}

static void teardown(void)
{
    hevc_uninit(ctx);
    v4l2r_handles_destroy(&drv.contexts);
    v4l2r_handles_destroy(&drv.buffers);
    v4l2r_handles_destroy(&drv.images);
    v4l2r_handles_destroy(&drv.surfaces);
    v4l2r_handles_destroy(&drv.configs);
}

/* --- synthetic pictures and slice headers --- */

static VAPictureParameterBufferHEVC pic;
static VASliceParameterBufferHEVC slices[32];
static unsigned int nb_slices;
static uint8_t slice_data[32 * 64];
static unsigned int slice_data_used;

static uint8_t *hdr;
static unsigned int hdr_pos;

static void bits(uint32_t value, unsigned int n)
{
    while (n--) {
        hdr[hdr_pos / 8] |= (n < 32 ? (value >> n) & 1 : 0) << (7 - hdr_pos % 8);
        hdr_pos++;
    }
}

static void ue(uint32_t value)
{
    unsigned int n = 0;
    uint32_t code = value + 1;
    for (uint32_t v = code; v > 1; v >>= 1)
        n++;
    bits(0, n);
    bits(code, n + 1);
}

static void picture_init(unsigned int cur, int32_t poc, bool idr, bool ltr_sps)
{
    memset(&pic, 0, sizeof(pic));
    pic.pic_width_in_luma_samples = 64;
    pic.pic_height_in_luma_samples = 64;
    pic.pic_fields.bits.chroma_format_idc = 1;
    pic.log2_min_luma_coding_block_size_minus3 = 0;
    pic.log2_diff_max_min_luma_coding_block_size = 1; /* CTB 16: 16 CTBs */
    pic.slice_parsing_fields.bits.long_term_ref_pics_present_flag = ltr_sps;
    pic.slice_parsing_fields.bits.IdrPicFlag = idr;
    pic.slice_parsing_fields.bits.RapPicFlag = idr;
    pic.CurrPic = (VAPictureHEVC) { .picture_id = sid[cur], .pic_order_cnt = poc };
    for (unsigned int i = 0; i < 15; i++) {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    nb_slices = 0;
    slice_data_used = 0;
    memset(slice_data, 0, sizeof(slice_data));
}

static void add_ref(unsigned int slot, unsigned int surface, int32_t poc, uint32_t flags)
{
    pic.ReferenceFrames[slot] = (VAPictureHEVC) {
        .picture_id = sid[surface], .pic_order_cnt = poc, .flags = flags,
    };
}

/* One slice: header bits the driver's parser accepts for this picture
 * (every optional PPS/SPS feature off), then eight payload bytes. */
static void add_slice(unsigned int slice_type, const uint8_t *l0, unsigned int n0,
                      const uint8_t *l1, unsigned int n1)
{
    VASliceParameterBufferHEVC *s = &slices[nb_slices];
    bool first = nb_slices == 0;
    bool idr = pic.slice_parsing_fields.bits.IdrPicFlag;
    unsigned int nal = idr ? HEVC_NAL_IDR_W_RADL : 1;
    unsigned int rps_start;

    hdr = slice_data + slice_data_used;
    hdr_pos = 0;
    bits(nal << 9 | 1, 16);
    bits(first, 1);
    if (idr)
        bits(0, 1);                 /* no_output_of_prior_pics */
    ue(0);                          /* pps id */
    if (!first)
        bits(nb_slices, 4);         /* slice_segment_address, 16 CTBs */
    ue(slice_type);
    if (!idr) {
        bits((uint32_t)pic.CurrPic.pic_order_cnt & 15, 4);
        bits(0, 1);                 /* short_term_ref_pic_set_sps_flag */
        rps_start = hdr_pos;
        ue(n0 ? 1 : 0);             /* num_negative_pics */
        ue(n1 ? 1 : 0);             /* num_positive_pics */
        if (n0) { ue(0); bits(1, 1); }
        if (n1) { ue(0); bits(1, 1); }
        pic.st_rps_bits = hdr_pos - rps_start;
        if (pic.slice_parsing_fields.bits.long_term_ref_pics_present_flag)
            ue(0);                  /* num_long_term_pics */
    }
    if (slice_type != V4L2_HEVC_SLICE_TYPE_I) {
        bits(1, 1);                 /* num_ref_idx_active_override_flag */
        ue(n0 - 1);
        if (slice_type == V4L2_HEVC_SLICE_TYPE_B) {
            ue(n1 - 1);
            bits(0, 1);             /* mvd_l1_zero_flag */
        }
        ue(0);                      /* five_minus_max_num_merge_cand */
    }
    ue(0);                          /* slice_qp_delta = 0 */
    bits(1, 1);                     /* rbsp trailing */
    unsigned int header_bytes = (hdr_pos + 7) / 8;
    memset(slice_data + slice_data_used + header_bytes, 0xa5, 8);

    *s = (VASliceParameterBufferHEVC) {
        .slice_data_size = header_bytes + 8,
        .slice_data_offset = slice_data_used,
        .slice_data_byte_offset = header_bytes,
        .num_ref_idx_l0_active_minus1 = n0 ? n0 - 1 : 0,
        .num_ref_idx_l1_active_minus1 = n1 ? n1 - 1 : 0,
    };
    s->LongSliceFlags.fields.slice_type = slice_type;
    for (unsigned int i = 0; i < n0; i++)
        s->RefPicList[0][i] = l0[i];
    for (unsigned int i = 0; i < n1; i++)
        s->RefPicList[1][i] = l1[i];
    slice_data_used += header_bytes + 8;
    nb_slices++;
    assert(slice_data_used <= sizeof(slice_data) && nb_slices <= 32);
}

static VAStatus decode_picture(void)
{
    struct v4l2r_surface *target = V4L2R_SURFACE_GET(&drv, pic.CurrPic.picture_id);
    struct v4l2r_buffer buf;
    VAStatus status;

    assert(target);
    assert(v4l2r_picture_begin(ctx, target) == VA_STATUS_SUCCESS);
    ctx->in_picture = true;
    assert(hevc_begin_picture(ctx) == VA_STATUS_SUCCESS);
    buf = (struct v4l2r_buffer) { .type = VAPictureParameterBufferType,
        .data = &pic, .nb_elements = 1, .element_size = sizeof(pic) };
    status = hevc_render_buffer(ctx, &buf);
    if (status == VA_STATUS_SUCCESS) {
        buf = (struct v4l2r_buffer) { .type = VASliceParameterBufferType,
            .data = slices, .nb_elements = nb_slices, .element_size = sizeof(slices[0]) };
        status = hevc_render_buffer(ctx, &buf);
    }
    if (status == VA_STATUS_SUCCESS) {
        buf = (struct v4l2r_buffer) { .type = VASliceDataBufferType,
            .data = slice_data, .nb_elements = 1, .element_size = slice_data_used };
        status = hevc_render_buffer(ctx, &buf);
    }
    if (status == VA_STATUS_SUCCESS)
        status = hevc_end_picture(ctx);
    ctx->in_picture = false;
    return status;
}

/* --- the sequences --- */

static const uint8_t L0_S0[] = { 0 };
static const uint8_t L1_S1[] = { 1 };
static const uint8_t L0_S1S0[] = { 1, 0 };
static const uint8_t L0_S0S1[] = { 0, 1 };

/* IDR, P, B, then a P whose VA slot order is not decode order. */
static void sequence_basic(bool ltr_sps, VAStatus expect[4])
{
    picture_init(0, 0, true, ltr_sps);
    add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
    expect[0] = decode_picture();

    picture_init(1, 4, false, ltr_sps);
    add_ref(0, 0, 0, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_slice(V4L2_HEVC_SLICE_TYPE_P, L0_S0, 1, NULL, 0);
    expect[1] = decode_picture();

    picture_init(2, 2, false, ltr_sps);
    add_ref(0, 0, 0, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_ref(1, 1, 4, VA_PICTURE_HEVC_RPS_ST_CURR_AFTER);
    add_slice(V4L2_HEVC_SLICE_TYPE_B, L0_S0, 1, L1_S1, 1);
    expect[2] = decode_picture();

    /* VA slots: [0] = POC 4, [1] = POC 0, [2] = POC 2 (decode order 0, 4, 2). */
    picture_init(3, 8, false, ltr_sps);
    add_ref(0, 1, 4, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_ref(1, 0, 0, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_ref(2, 2, 2, 0); /* in the DPB, in no current list */
    add_slice(V4L2_HEVC_SLICE_TYPE_P, L0_S0S1, 2, NULL, 0);
    expect[3] = decode_picture();
}

/* Inside an LTR-capable SPS: a P picture that really uses a long-term ref. */
static VAStatus sequence_long_term(void)
{
    VAStatus st[4];

    sequence_basic(true, st);
    for (unsigned int i = 0; i < 4; i++)
        assert(st[i] == VA_STATUS_SUCCESS);
    picture_init(0, 12, false, true);
    add_ref(0, 3, 8, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_ref(1, 1, 4, VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR);
    add_slice(V4L2_HEVC_SLICE_TYPE_P, L0_S1S0, 2, NULL, 0);
    return decode_picture();
}

/* --- record checks against the device-side capture --- */

static char trace_buf[65536];

static void read_sink(FILE *sink)
{
    size_t n;

    fflush(sink);
    rewind(sink);
    n = fread(trace_buf, 1, sizeof(trace_buf) - 1, sink);
    trace_buf[n] = '\0';
}

static char *nth_line(unsigned int n)
{
    char *line = trace_buf;

    for (unsigned int i = 0; i < n; i++) {
        line = strchr(line, '\n');
        assert(line);
        line++;
    }
    return line;
}

static unsigned int line_count(void)
{
    unsigned int n = 0;
    for (const char *p = trace_buf; *p; p++)
        n += *p == '\n';
    return n;
}

static bool line_has(const char *line, const char *needle)
{
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    return memmem(line, len, needle, strlen(needle)) != NULL;
}

static const void *captured_control(const struct submission *s, uint32_t id, uint32_t *size)
{
    for (unsigned int i = 0; i < s->count; i++)
        if (s->ctrl[i].id == id) {
            if (size)
                *size = s->ctrl[i].size;
            return s->ctrl[i].data;
        }
    return NULL;
}

/* Build, from the captured control bytes alone, what the record must say. */
static void expect_record(const char *line, const struct submission *s)
{
    uint32_t size = 0;
    const struct v4l2_ctrl_hevc_decode_params *dp =
        captured_control(s, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS, &size);
    const struct v4l2_ctrl_hevc_slice_params *sp =
        captured_control(s, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, NULL);
    char want[2048];
    size_t pos = 0;

    assert(dp && size == sizeof(*dp));
    assert(s->request_fd >= 0);

    pos = (size_t)snprintf(want, sizeof(want), ",\"poc\":%d,\"irap\":%u,\"idr\":%u,",
                           dp->pic_order_cnt_val,
                           !!(dp->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC),
                           !!(dp->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC));
    assert(line_has(line, want));

    pos = (size_t)snprintf(want, sizeof(want), ",\"dpb\":[");
    for (unsigned int i = 0; i < dp->num_active_dpb_entries; i++)
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos,
                                "%s{\"i\":%u,\"buf\":%lld,\"poc\":%d,\"lt\":%u,\"field\":%u}",
                                i ? "," : "", i,
                                (long long)(dp->dpb[i].timestamp / 1000) - 1,
                                dp->dpb[i].pic_order_cnt_val,
                                !!(dp->dpb[i].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE),
                                !!dp->dpb[i].field_pic);
    pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"st_before\":[");
    for (unsigned int i = 0; i < dp->num_poc_st_curr_before; i++)
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "%s%u", i ? "," : "",
                                dp->poc_st_curr_before[i]);
    pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"st_after\":[");
    for (unsigned int i = 0; i < dp->num_poc_st_curr_after; i++)
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "%s%u", i ? "," : "",
                                dp->poc_st_curr_after[i]);
    pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"lt_curr\":[");
    for (unsigned int i = 0; i < dp->num_poc_lt_curr; i++)
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "%s%u", i ? "," : "",
                                dp->poc_lt_curr[i]);
    pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"slices\":[");
    if (sp) {
        uint32_t sp_size = 0;
        captured_control(s, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, &sp_size);
        unsigned int n = sp_size / sizeof(*sp);
        for (unsigned int i = 0; i < n && i < 16; i++) {
            const struct v4l2_ctrl_hevc_slice_params *p = &sp[i];
            const char *type = p->slice_type == V4L2_HEVC_SLICE_TYPE_B ? "B" :
                               p->slice_type == V4L2_HEVC_SLICE_TYPE_P ? "P" : "I";
            unsigned int n0 = p->slice_type != V4L2_HEVC_SLICE_TYPE_I ? p->num_ref_idx_l0_active_minus1 + 1 : 0;
            unsigned int n1 = p->slice_type == V4L2_HEVC_SLICE_TYPE_B ? p->num_ref_idx_l1_active_minus1 + 1 : 0;

            pos += (size_t)snprintf(want + pos, sizeof(want) - pos,
                                    "%s{\"i\":%u,\"type\":\"%s\",\"nal\":%u,\"l0\":[",
                                    i ? "," : "", i, type, p->nal_unit_type);
            for (unsigned int k = 0; k < n0; k++)
                pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "%s%u", k ? "," : "", p->ref_idx_l0[k]);
            pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"l1\":[");
            for (unsigned int k = 0; k < n1; k++)
                pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "%s%u", k ? "," : "", p->ref_idx_l1[k]);
            pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"tmvp\":0}");
        }
        if (n > 16)
            pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "],\"slices_omitted\":%u", n - 16);
        else
            pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "]");
    } else {
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos, "]");
    }
    assert(pos < sizeof(want));
    if (!line_has(line, want)) {
        fprintf(stderr, "record: %.*s\nwant:   %s\n",
                (int)(strchr(line, '\n') - line), line, want);
        abort();
    }
}

/* Every submission with a request must have exactly one record, in order. */
static void check_records_match_capture(void)
{
    unsigned int rec = 0;

    for (unsigned int i = 0; i < log_count; i++) {
        if (log_[i].request_fd < 0)
            continue;
        expect_record(nth_line(rec), &log_[i]);
        rec++;
    }
    assert(rec == line_count());
    assert(rec == requests_queued);
}

/* Byte-for-byte copy of the control payloads of one run. */
struct run_capture {
    unsigned int submissions;
    unsigned int requests;
    size_t bytes;
    unsigned char *data;
};

static struct run_capture snapshot_capture(void)
{
    struct run_capture c = { .submissions = log_count, .requests = requests_queued };
    size_t total = 0;

    for (unsigned int i = 0; i < log_count; i++)
        for (unsigned int k = 0; k < log_[i].count; k++)
            total += sizeof(uint32_t) * 3 + log_[i].ctrl[k].size;
    c.data = malloc(total ? total : 1);
    assert(c.data);
    for (unsigned int i = 0; i < log_count; i++)
        for (unsigned int k = 0; k < log_[i].count; k++) {
            uint32_t head[3] = { (uint32_t)log_[i].request_fd, log_[i].ctrl[k].id, log_[i].ctrl[k].size };
            memcpy(c.data + c.bytes, head, sizeof(head));
            c.bytes += sizeof(head);
            memcpy(c.data + c.bytes, log_[i].ctrl[k].data, log_[i].ctrl[k].size);
            c.bytes += log_[i].ctrl[k].size;
        }
    return c;
}

/* Run a sequence twice, trace off then on; the device must see the same
 * bytes and the client the same statuses; the records must match. */
typedef void (*sequence_fn)(VAStatus *st, unsigned int *n);

static void run_twice(bool avd, int64_t mode, unsigned int max_slices,
                      sequence_fn seq, FILE *sink)
{
    VAStatus st_off[8], st_on[8];
    unsigned int n_off = 0, n_on = 0;
    struct run_capture off, on;

    v4l2r_hevc_trace_configure(&(struct v4l2r_hevc_trace_options) { .enable = false, .sink = sink });
    log_reset();
    setup(avd, mode, max_slices);
    seq(st_off, &n_off);
    off = snapshot_capture();
    teardown();
    read_sink(sink);
    assert(line_count() == 0);                  /* disabled: nothing written */

    v4l2r_hevc_trace_configure(&(struct v4l2r_hevc_trace_options) { .enable = true, .sink = sink });
    log_reset();
    setup(avd, mode, max_slices);
    seq(st_on, &n_on);
    on = snapshot_capture();
    read_sink(sink);
    check_records_match_capture();
    teardown();

    assert(n_off == n_on && !memcmp(st_off, st_on, n_on * sizeof(*st_on)));
    assert(off.submissions == on.submissions && off.requests == on.requests);
    assert(off.bytes == on.bytes && !memcmp(off.data, on.data, on.bytes));
    free(off.data);
    free(on.data);
}

static void seq_basic_avd(VAStatus *st, unsigned int *n) { sequence_basic(false, st); *n = 4; }
static void seq_basic_ltr(VAStatus *st, unsigned int *n) { sequence_basic(true, st); *n = 4; }
static void seq_long_term(VAStatus *st, unsigned int *n) { st[0] = sequence_long_term(); *n = 1; }

static void seq_slices(VAStatus *st, unsigned int *n)
{
    picture_init(0, 0, true, false);
    add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
    st[0] = decode_picture();
    /* Three slices with a two-slice control: a batch flush mid-picture. */
    picture_init(1, 4, false, false);
    add_ref(0, 0, 0, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_slice(V4L2_HEVC_SLICE_TYPE_P, L0_S0, 1, NULL, 0);
    add_slice(V4L2_HEVC_SLICE_TYPE_P, L0_S0, 1, NULL, 0);
    add_slice(V4L2_HEVC_SLICE_TYPE_P, L0_S0, 1, NULL, 0);
    st[1] = decode_picture();
    *n = 2;
}

static void seq_many_slices(VAStatus *st, unsigned int *n)
{
    picture_init(0, 0, true, false);
    for (unsigned int i = 0; i < 18; i++)
        add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
    st[0] = decode_picture();
    *n = 1;
}

/* --- cases --- */

static void case_match(bool avd, bool ltr_sps)
{
    FILE *sink = tmpfile();
    VAStatus st[4];
    unsigned int n;

    assert(sink);
    run_twice(avd, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 1,
              ltr_sps ? seq_basic_ltr : seq_basic_avd, sink);
    /* Beyond matching the capture: the documented meaning of the fields. */
    for (unsigned int i = 0; i < 4; i++) {
        char want[64];
        snprintf(want, sizeof(want), "\"pic\":%u,\"req\":%u,\"first\":1,\"last\":1,\"target\":%u,", i + 1, i + 1, i);
        assert(line_has(nth_line(i), want));
        snprintf(want, sizeof(want), ",\"ltr_sps\":%u,\"reorder\":%u,", ltr_sps, avd && !ltr_sps);
        assert(line_has(nth_line(i), want));
        assert(line_has(nth_line(i), "\"lt\":1") == false);
        assert(line_has(nth_line(i), "\"lt_curr\":[]"));
    }
    assert(line_has(nth_line(0), "\"idr\":1") && line_has(nth_line(0), "\"dpb\":[]"));
    assert(line_has(nth_line(2), "\"type\":\"B\""));
    if (avd && !ltr_sps) {
        /* AVD remap: DPB in decode order 0, 4, 2 although VA slots said 4, 0, 2. */
        assert(line_has(nth_line(3), "\"dpb\":[{\"i\":0,\"buf\":0,\"poc\":0,"));
        assert(line_has(nth_line(3), "{\"i\":1,\"buf\":1,\"poc\":4,"));
        assert(line_has(nth_line(3), "{\"i\":2,\"buf\":2,\"poc\":2,"));
        assert(line_has(nth_line(3), "\"l0\":[1,0]"));
    } else {
        /* VA slot order preserved: zero long-term pictures under an
         * LTR-capable SPS keep the gate off, exactly like a generic decoder. */
        assert(line_has(nth_line(3), "\"dpb\":[{\"i\":0,\"buf\":1,\"poc\":4,"));
        assert(line_has(nth_line(3), "{\"i\":1,\"buf\":0,\"poc\":0,"));
        assert(line_has(nth_line(3), "{\"i\":2,\"buf\":2,\"poc\":2,"));
        assert(line_has(nth_line(3), "\"l0\":[0,1]"));
    }
    (void)st; (void)n;
    fclose(sink);
}

static void case_long_term(void)
{
    FILE *sink = tmpfile();

    assert(sink);
    run_twice(true, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 1, seq_long_term, sink);
    assert(line_count() == 5);
    assert(line_has(nth_line(4), "\"ltr_sps\":1,\"reorder\":0,\"total_curr\":2,"));
    assert(line_has(nth_line(4), "{\"i\":1,\"buf\":1,\"poc\":4,\"lt\":1,"));
    assert(line_has(nth_line(4), "\"st_before\":[0],\"st_after\":[],\"lt_curr\":[1]"));
    assert(line_has(nth_line(4), "\"l0\":[1,0]"));
    fclose(sink);
}

static void case_slices(void)
{
    FILE *sink = tmpfile();

    assert(sink);
    run_twice(true, V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED, 2, seq_slices, sink);
    assert(line_count() == 3);
    assert(line_has(nth_line(0), "\"pic\":1,\"req\":1,\"first\":1,\"last\":1,"));
    assert(line_has(nth_line(1), "\"pic\":2,\"req\":2,\"first\":1,\"last\":0,"));
    assert(line_has(nth_line(1), "{\"i\":1,\"type\":\"P\""));
    assert(!line_has(nth_line(1), "{\"i\":2,"));
    assert(line_has(nth_line(2), "\"pic\":2,\"req\":3,\"first\":0,\"last\":1,"));
    assert(line_has(nth_line(2), "{\"i\":0,\"type\":\"P\""));
    assert(!line_has(nth_line(2), "{\"i\":1,\"type\""));
    fclose(sink);
}

static void case_many_slices(void)
{
    FILE *sink = tmpfile();

    assert(sink);
    run_twice(true, V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED, 32, seq_many_slices, sink);
    assert(line_count() == 1);
    assert(line_has(nth_line(0), "{\"i\":15,\"type\":\"I\""));
    assert(!line_has(nth_line(0), "{\"i\":16,"));
    assert(line_has(nth_line(0), "\"slices_omitted\":2}"));
    assert(strlen(trace_buf) < 4096);
    fclose(sink);
}

/* A rejected submission leaves no record and the same status either way. */
static void case_failure(void)
{
    FILE *sink = tmpfile();
    VAStatus off, on;

    assert(sink);
    for (int enable = 0; enable < 2; enable++) {
        v4l2r_hevc_trace_configure(&(struct v4l2r_hevc_trace_options) { .enable = enable, .sink = sink });
        log_reset();
        setup(true, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 1);
        picture_init(0, 0, true, false);
        add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
        fail_set_controls = true;
        if (enable)
            on = decode_picture();
        else
            off = decode_picture();
        fail_set_controls = false;
        teardown();
    }
    assert(off == VA_STATUS_ERROR_OPERATION_FAILED && on == off);
    read_sink(sink);
    assert(line_count() == 0 && requests_queued == 0);
    fclose(sink);
}

/* Environment contract: off by default, "stderr" or a writable file
 * enables, an unusable sink disables with a warning that never echoes it. */
static void case_environment(void)
{
    FILE *diag_sink = tmpfile();
    char path[] = "/tmp/hevc-reftrace-XXXXXX";
    int fd;

    assert(diag_sink);
    v4l2r_diag_configure(&(struct v4l2r_diag_options) { .mode = V4L2R_DIAG_MODE_JSON, .sink = diag_sink });

    unsetenv("LIBVA_V4L2_HEVC_REFTRACE");
    v4l2r_hevc_trace_configure(NULL);
    assert(!v4l2r_hevc_trace_enabled());

    setenv("LIBVA_V4L2_HEVC_REFTRACE", "", 1);
    v4l2r_hevc_trace_configure(NULL);
    assert(!v4l2r_hevc_trace_enabled());

    setenv("LIBVA_V4L2_HEVC_REFTRACE", "stderr", 1);
    v4l2r_hevc_trace_configure(NULL);
    assert(v4l2r_hevc_trace_enabled());

    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("LIBVA_V4L2_HEVC_REFTRACE", path, 1);
    v4l2r_hevc_trace_configure(NULL);
    assert(v4l2r_hevc_trace_enabled());
    log_reset();
    setup(true, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 1);
    picture_init(0, 0, true, false);
    add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
    assert(decode_picture() == VA_STATUS_SUCCESS);
    teardown();
    v4l2r_hevc_trace_configure(NULL);           /* closes the file */
    {
        FILE *f = fopen(path, "r");
        assert(f);
        assert(fread(trace_buf, 1, sizeof(trace_buf) - 1, f) > 0);
        fclose(f);
        assert(strstr(trace_buf, "\"schema\":\"" V4L2R_HEVC_TRACE_SCHEMA "\""));
        assert(strstr(trace_buf, "\"pic\":1,\"req\":1,"));
        /* Never the path itself, in the trace or the diagnostics. */
        assert(!strstr(trace_buf, "hevc-reftrace-"));
    }
    unlink(path);

    setenv("LIBVA_V4L2_HEVC_REFTRACE", "/nonexistent-dir-hevc-reftrace/trace.jsonl", 1);
    v4l2r_hevc_trace_configure(NULL);
    assert(!v4l2r_hevc_trace_enabled());
    fflush(diag_sink);
    rewind(diag_sink);
    {
        size_t n = fread(trace_buf, 1, sizeof(trace_buf) - 1, diag_sink);
        trace_buf[n] = '\0';
        assert(strstr(trace_buf, "\"op\":\"hevc-reftrace\""));
        assert(strstr(trace_buf, "\"category\":\"client\""));
        assert(!strstr(trace_buf, "nonexistent-dir"));
    }
    unsetenv("LIBVA_V4L2_HEVC_REFTRACE");
    v4l2r_hevc_trace_configure(NULL);
    fclose(diag_sink);
    v4l2r_diag_configure(NULL);
}

/* Real writer output for the checker's schema validation (stdout). */
/* A named FIFO without a reader must not hang the client in trace init. */
static void case_fifo(void)
{
    char directory[] = "/tmp/hevc-reftrace-fifo-XXXXXX";
    char path[256];
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/pipe", directory) > 0);
    assert(mkfifo(path, 0600) == 0);
    assert(setenv("LIBVA_V4L2_HEVC_REFTRACE", path, 1) == 0);
    v4l2r_hevc_trace_configure(NULL);
    assert(!v4l2r_hevc_trace_enabled());
    unsetenv("LIBVA_V4L2_HEVC_REFTRACE");
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

static void case_write_failure(void)
{
    FILE *sink = fopen("/dev/full", "w");
    VAStatus status[4];
    struct run_capture captures[2];
    assert(sink);
    for (unsigned int enabled = 0; enabled < 2; enabled++) {
        v4l2r_hevc_trace_configure(&(struct v4l2r_hevc_trace_options) {
            .enable = enabled, .sink = sink });
        log_reset();
        setup(true, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 32);
        sequence_basic(false, status);
        for (unsigned int i = 0; i < 4; i++)
            assert(status[i] == VA_STATUS_SUCCESS);
        captures[enabled] = snapshot_capture();
        assert(!v4l2r_hevc_trace_enabled());
        teardown();
    }
    assert(captures[0].bytes == captures[1].bytes);
    assert(!memcmp(captures[0].data, captures[1].data, captures[0].bytes));
    assert(captures[0].requests == captures[1].requests);
    free(captures[0].data);
    free(captures[1].data);
    v4l2r_hevc_trace_configure(NULL);
    fclose(sink);
}

static void emit_samples(void)
{
    VAStatus st[8];
    unsigned int n;

    v4l2r_hevc_trace_configure(&(struct v4l2r_hevc_trace_options) { .enable = true, .sink = stdout });
    log_reset();
    setup(true, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 1);
    seq_long_term(st, &n);
    assert(st[0] == VA_STATUS_SUCCESS);
    teardown();
    log_reset();
    setup(false, V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED, 2);
    seq_slices(st, &n);
    assert(st[0] == VA_STATUS_SUCCESS && st[1] == VA_STATUS_SUCCESS);
    teardown();
    v4l2r_hevc_trace_configure(NULL);
}

int main(int argc, char **argv)
{
    struct rlimit core = { 0, 0 };
    const char *which = argc > 1 ? argv[1] : "";

    assert(!setrlimit(RLIMIT_CORE, &core));
    unsetenv("LIBVA_V4L2_HEVC_REFTRACE");
    v4l2r_diag_configure(&(struct v4l2r_diag_options) { .mode = V4L2R_DIAG_MODE_TEXT, .sink = stderr });

    if (!strcmp(which, "match-avd"))
        case_match(true, false);
    else if (!strcmp(which, "match-ltr-sps"))
        case_match(true, true);
    else if (!strcmp(which, "match-generic"))
        case_match(false, false);
    else if (!strcmp(which, "long-term"))
        case_long_term();
    else if (!strcmp(which, "slices"))
        case_slices();
    else if (!strcmp(which, "many-slices"))
        case_many_slices();
    else if (!strcmp(which, "failure"))
        case_failure();
    else if (!strcmp(which, "environment"))
        case_environment();
    else if (!strcmp(which, "fifo"))
        case_fifo();
    else if (!strcmp(which, "write-failure"))
        case_write_failure();
    else if (!strcmp(which, "emit-samples"))
        emit_samples();
    else {
        fprintf(stderr, "usage: %s match-avd|match-ltr-sps|match-generic|long-term|"
                "slices|many-slices|failure|environment|fifo|write-failure|emit-samples\n", argv[0]);
        return 2;
    }
    if (strcmp(which, "emit-samples"))
        printf("hevc-reftrace %s: PASS\n", which);
    return 0;
}
