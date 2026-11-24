// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * H.264 and HEVC encoding (VAEntrypointEncSlice) on a V4L2 stateful
 * memory-to-memory encoder, such as the Apple video encoder (apple-ave).
 *
 * A stateful encoder chooses the picture structure, writes the parameter
 * sets and slice headers itself and owns its reference frames. VA-API's
 * encode model is stateless: the client sends per-picture parameters and
 * expects one coded picture per input picture. The two meet when the
 * client asks for what such an encoder does by itself: no B-frames (the
 * driver reports one forward reference only), the picture types the
 * encoder picks (a periodic IDR at the client's intra period, a forced IDR
 * when the client asks for one), and no packed HEVC headers (the encoder
 * writes its own SPS/PPS/VPS in front of every IDR, in the bitstream).
 *
 * Like the video processing path (convert.c) every picture is one
 * synchronous job: vaEndPicture() queues the input surface's dma-buf,
 * waits for the coded picture and copies it into the client's coded
 * buffer. The surface is ready (vaSyncSurface) and the coded buffer
 * mappable as soon as vaEndPicture() returns.
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

#include "v4l2_request.h"

#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

/*
 * Constant quality is newer than the oldest kernel headers this driver
 * builds against (5.4). The control ID and the menu value are fixed UAPI,
 * and the encoder still decides at run time: VIDIOC_S_CTRL fails on a
 * kernel without the control and the rate control request is refused.
 */
#ifndef V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY
#define V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY	(V4L2_CID_MPEG_BASE + 645)
#define V4L2_MPEG_VIDEO_BITRATE_MODE_CQ		2
#endif

#define V4L2R_ENC_CAPTURE_BUFFERS	2
#define V4L2R_ENC_TIMEOUT_MS		2000

struct v4l2r_enc {
	int fd;
	uint32_t coded;			/* V4L2_PIX_FMT_H264 / _HEVC */
	VAProfile profile;
	uint32_t rc_mode;		/* VA_RC_* */

	/* Geometry the encoder is configured for. */
	uint32_t width, height, pitch, pixelformat;
	/* Visible picture (the VA surface) inside the buffer. */
	uint32_t crop_w, crop_h;

	/* Rate control and structure from the client's parameters. */
	uint32_t bitrate, peak, qp, intra_period;
	uint32_t fps_num, fps_den;
	bool rc_dirty;

	void *cap_map[V4L2R_ENC_CAPTURE_BUFFERS];
	size_t cap_len[V4L2R_ENC_CAPTURE_BUFFERS];
	unsigned int nb_captures;
	uint64_t frames;

	/* The picture being assembled. */
	struct v4l2r_surface *input;
	VABufferID coded_buf;
	bool idr;
	bool have_picture;
};

/* --- encoder device discovery --- */

static bool fmt_listed(int fd, enum v4l2_buf_type type, uint32_t fourcc)
{
	struct v4l2_fmtdesc d = { .type = type };

	for (d.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++)
		if (d.pixelformat == fourcc)
			return true;
	return false;
}

static void probe_encoder(struct v4l2r_driver *drv)
{
	char path[32];

	drv->encoder_probed = true;
	if (getenv("V4L2R_NO_ENCODE"))
		return;

	for (int i = 0; i < 64; i++) {
		struct v4l2_capability cap = {0};
		unsigned int caps;
		int fd;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
			close(fd);
			continue;
		}
		caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ?
		       cap.device_caps : cap.capabilities;
		if ((caps & V4L2_CAP_STREAMING) && (caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
		    fmt_listed(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_NV12)) {
			bool h264 = fmt_listed(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
					       V4L2_PIX_FMT_H264);
			bool hevc = fmt_listed(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
					       V4L2_PIX_FMT_HEVC);

			if (h264 || hevc) {
				snprintf(drv->encoder.video_path,
					 sizeof(drv->encoder.video_path), "%s", path);
				snprintf(drv->encoder.card, sizeof(drv->encoder.card),
					 "%s", cap.card);
				drv->encoder.h264 = h264;
				drv->encoder.hevc = hevc;
				drv->has_encoder = true;
				close(fd);
				v4l2r_diag(NULL, V4L2R_DIAG_LEVEL_INFO, V4L2R_DIAG_INFO,
					   "encoder", 0, "encoder %s [%s]: H.264 %s, HEVC %s",
					   path, cap.card, h264 ? "yes" : "no",
					   hevc ? "yes" : "no");
				return;
			}
		}
		close(fd);
	}
}

bool v4l2r_encoder_supports(struct v4l2r_driver *drv, VAProfile profile)
{
	bool ok;

	pthread_mutex_lock(&drv->mutex);
	if (!drv->encoder_probed)
		probe_encoder(drv);
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		ok = drv->has_encoder && drv->encoder.h264;
		break;
	case VAProfileHEVCMain:
		ok = drv->has_encoder && drv->encoder.hevc;
		break;
	default:
		ok = false;
		break;
	}
	pthread_mutex_unlock(&drv->mutex);
	return ok;
}

uint32_t v4l2r_encoder_attribute(VAProfile profile, VAConfigAttribType type)
{
	switch (type) {
	case VAConfigAttribRTFormat:
		return VA_RT_FORMAT_YUV420;
	case VAConfigAttribRateControl:
		return VA_RC_CQP | VA_RC_CBR | VA_RC_VBR;
	case VAConfigAttribEncPackedHeaders:
		/*
		 * The encoder writes its own parameter sets in band, in front of
		 * every IDR. For H.264, offering packed sequence headers anyway
		 * lets a client build them for its container (FFmpeg writes
		 * extradata, i.e. avcC, only with this bit); the packed buffers it
		 * then sends are accepted and dropped in v4l2r_enc_render_buffer,
		 * and the client's SPS/PPS match the encoder's. Without it, Matroska
		 * refuses the stream before the first frame (empty CodecPrivate).
		 *
		 * Not for HEVC: the client's VPS/SPS/PPS would not describe the
		 * stream. The encoder picks its own short-term reference picture
		 * sets in the SPS, scaling lists, init_qp and wavefront entry
		 * points, and v4l2r_enc_render_buffer passes none of the client's
		 * choices for them on, so an hvcC built from the client's sets
		 * disagrees with the in-band ones. A client that needs a global
		 * header takes it from the first coded picture.
		 */
		if (profile == VAProfileHEVCMain)
			return VA_ENC_PACKED_HEADER_NONE;
		return VA_ENC_PACKED_HEADER_SEQUENCE;
	case VAConfigAttribEncMaxRefFrames:
		/* One forward reference, no backward: no B-frames. */
		return 1;
	case VAConfigAttribEncMaxSlices:
		return 1;
	case VAConfigAttribMaxPictureWidth:
		return 4096;
	case VAConfigAttribMaxPictureHeight:
		return profile == VAProfileHEVCMain ? 2304 : 4096;
	default:
		return VA_ATTRIB_NOT_SUPPORTED;
	}
}

/* --- context --- */

VAStatus v4l2r_enc_create(struct v4l2r_context *ctx,
			  const struct v4l2r_config *config)
{
	struct v4l2r_enc *enc;

	enc = calloc(1, sizeof(*enc));
	if (!enc)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	enc->fd = -1;
	enc->profile = config->profile;
	enc->coded = config->profile == VAProfileHEVCMain ? V4L2_PIX_FMT_HEVC :
							    V4L2_PIX_FMT_H264;
	enc->rc_mode = VA_RC_VBR;
	for (int i = 0; i < config->nb_attributes; i++)
		if (config->attributes[i].type == VAConfigAttribRateControl &&
		    config->attributes[i].value != VA_ATTRIB_NOT_SUPPORTED)
			enc->rc_mode = config->attributes[i].value;
	enc->qp = 26;
	enc->bitrate = 8000000;
	enc->fps_num = 30;
	enc->fps_den = 1;
	ctx->enc = enc;
	return VA_STATUS_SUCCESS;
}

static void enc_teardown(struct v4l2r_enc *enc)
{
	for (unsigned int i = 0; i < enc->nb_captures; i++)
		if (enc->cap_map[i] && enc->cap_map[i] != MAP_FAILED)
			munmap(enc->cap_map[i], enc->cap_len[i]);
	enc->nb_captures = 0;
	if (enc->fd >= 0) {
		close(enc->fd);
		enc->fd = -1;
	}
	enc->frames = 0;
}

void v4l2r_enc_destroy(struct v4l2r_context *ctx)
{
	if (!ctx->enc)
		return;
	enc_teardown(ctx->enc);
	free(ctx->enc);
	ctx->enc = NULL;
}

bool v4l2r_enc_holds(const struct v4l2r_context *ctx,
		     const struct v4l2r_surface *surface)
{
	return ctx->enc && ctx->enc->input == surface;
}

VAStatus v4l2r_enc_begin_picture(struct v4l2r_context *ctx,
				 struct v4l2r_surface *input)
{
	struct v4l2r_enc *enc = ctx->enc;

	enc->input = input;
	enc->coded_buf = VA_INVALID_ID;
	enc->idr = false;
	enc->have_picture = false;
	/* The input is read, not written: it is not the picture target
	 * (a target is busy until its picture ends). */
	ctx->in_picture = true;
	return VA_STATUS_SUCCESS;
}

static void misc_param(struct v4l2r_enc *enc, const VAEncMiscParameterBuffer *misc,
		       size_t bytes)
{
	if (bytes < sizeof(*misc))
		return;
	bytes -= sizeof(*misc);

	switch (misc->type) {
	case VAEncMiscParameterTypeRateControl: {
		const VAEncMiscParameterRateControl *rc = (const void *)misc->data;
		uint32_t target;

		if (bytes < sizeof(*rc) || !rc->bits_per_second)
			break;
		target = rc->target_percentage && rc->target_percentage < 100 ?
			 (uint32_t)((uint64_t)rc->bits_per_second *
				    rc->target_percentage / 100) :
			 rc->bits_per_second;
		if (target != enc->bitrate || rc->bits_per_second != enc->peak) {
			enc->bitrate = target;
			enc->peak = rc->bits_per_second;
			enc->rc_dirty = true;
		}
		break;
	}
	case VAEncMiscParameterTypeFrameRate: {
		const VAEncMiscParameterFrameRate *fr = (const void *)misc->data;
		uint32_t num, den;

		if (bytes < sizeof(*fr) || !fr->framerate)
			break;
		/* Low 16 bits numerator, high 16 bits denominator (0 = 1). */
		num = fr->framerate & 0xffff;
		den = fr->framerate >> 16;
		enc->fps_num = num ? num : 30;
		enc->fps_den = den ? den : 1;
		break;
	}
	default:
		break;
	}
}

VAStatus v4l2r_enc_render_buffer(struct v4l2r_context *ctx,
				 struct v4l2r_buffer *buf)
{
	struct v4l2r_enc *enc = ctx->enc;
	size_t bytes = v4l2r_buffer_bytes(buf);

	switch (buf->type) {
	case VAEncSequenceParameterBufferType:
		if (enc->coded == V4L2_PIX_FMT_H264) {
			const VAEncSequenceParameterBufferH264 *s = buf->data;

			if (bytes < sizeof(*s))
				return VA_STATUS_ERROR_INVALID_BUFFER;
			enc->intra_period = s->intra_idr_period ? s->intra_idr_period :
					    s->intra_period;
			if (s->bits_per_second && s->bits_per_second != enc->peak) {
				enc->peak = enc->bitrate = s->bits_per_second;
				enc->rc_dirty = true;
			}
		} else {
			const VAEncSequenceParameterBufferHEVC *s = buf->data;

			if (bytes < sizeof(*s))
				return VA_STATUS_ERROR_INVALID_BUFFER;
			enc->intra_period = s->intra_idr_period ? s->intra_idr_period :
					    s->intra_period;
			if (s->bits_per_second && s->bits_per_second != enc->peak) {
				enc->peak = enc->bitrate = s->bits_per_second;
				enc->rc_dirty = true;
			}
		}
		return VA_STATUS_SUCCESS;

	case VAEncPictureParameterBufferType:
		if (enc->coded == V4L2_PIX_FMT_H264) {
			const VAEncPictureParameterBufferH264 *p = buf->data;

			if (bytes < sizeof(*p))
				return VA_STATUS_ERROR_INVALID_BUFFER;
			enc->coded_buf = p->coded_buf;
			enc->idr = p->pic_fields.bits.idr_pic_flag;
			if (p->pic_init_qp && p->pic_init_qp != enc->qp) {
				enc->qp = p->pic_init_qp;
				enc->rc_dirty = true;
			}
		} else {
			const VAEncPictureParameterBufferHEVC *p = buf->data;

			if (bytes < sizeof(*p))
				return VA_STATUS_ERROR_INVALID_BUFFER;
			enc->coded_buf = p->coded_buf;
			enc->idr = p->pic_fields.bits.idr_pic_flag;
			if (p->pic_init_qp && p->pic_init_qp != enc->qp) {
				enc->qp = p->pic_init_qp;
				enc->rc_dirty = true;
			}
		}
		enc->have_picture = true;
		return VA_STATUS_SUCCESS;

	case VAEncMiscParameterBufferType:
		misc_param(enc, buf->data, bytes);
		return VA_STATUS_SUCCESS;

	case VAEncSliceParameterBufferType:
	case VAEncPackedHeaderParameterBufferType:
	case VAEncPackedHeaderDataBufferType:
		/* One slice per picture, headers written by the encoder. */
		return VA_STATUS_SUCCESS;

	default:
		return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
	}
}

static int set_ctrl(int fd, uint32_t id, int32_t value)
{
	struct v4l2_control c = { .id = id, .value = value };

	return ioctl(fd, VIDIOC_S_CTRL, &c);
}

/*
 * The AVE's constant-quality mode codes every frame with one QP taken from
 * a 101-step quality table (drivers/media/platform/apple/ave/ave-sess.c,
 * ave_cq_qp[]). Pick the lowest quality whose QP is at most the one asked
 * for, so VA_RC_CQP gets exactly that QP where the table has it.
 */
static const uint8_t ave_cq_qp[101] = {
	48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 47, 47, 47, 46, 46, 46, 45, 45, 44,
	44, 44, 43, 43, 43, 42, 42, 42, 41, 41, 40, 40, 40, 39, 39, 39, 38, 38, 37, 37,
	37, 36, 36, 35, 35, 35, 34, 34, 33, 33, 33, 32, 32, 31, 31, 30, 30, 29, 29, 29,
	28, 28, 27, 27, 26, 26, 25, 25, 24, 24, 23, 23, 22, 22, 21, 21, 20, 20, 19, 18,
	18, 17, 17, 16, 15, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
	0,
};

static int32_t qp_to_quality(uint32_t qp)
{
	for (int q = 0; q <= 100; q++)
		if (ave_cq_qp[q] <= qp)
			return q;
	return 100;
}

static int enc_apply_rc(struct v4l2r_enc *enc)
{
	int ret = 0;

	switch (enc->rc_mode & (VA_RC_CQP | VA_RC_CBR | VA_RC_VBR)) {
	case VA_RC_CQP:
		ret |= set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
				V4L2_MPEG_VIDEO_BITRATE_MODE_CQ);
		ret |= set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
				qp_to_quality(enc->qp));
		break;
	case VA_RC_CBR:
		ret |= set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
				V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
		ret |= set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE, enc->bitrate);
		break;
	default:
		ret |= set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
				V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
		ret |= set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE, enc->bitrate);
		if (enc->peak > enc->bitrate)
			set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, enc->peak);
		break;
	}
	enc->rc_dirty = false;
	return ret;
}

static VAStatus enc_configure(struct v4l2r_context *ctx,
			      const struct v4l2r_frame_view *in,
			      uint32_t vis_w, uint32_t vis_h)
{
	struct v4l2r_enc *enc = ctx->enc;
	struct v4l2r_driver *drv = ctx->drv;
	struct v4l2_format fmt = {0};
	struct v4l2_requestbuffers rb;
	struct v4l2_streamparm parm = {0};
	enum v4l2_buf_type type;

	if (enc->fd >= 0 && enc->width == in->width && enc->height == in->height &&
	    enc->pitch == in->pitch && enc->pixelformat == in->pixelformat &&
	    enc->crop_w == vis_w && enc->crop_h == vis_h)
		return VA_STATUS_SUCCESS;

	enc_teardown(enc);
	enc->fd = open(drv->encoder.video_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (enc->fd < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Coded format first: the raw side follows its size. */
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.pixelformat = enc->coded;
	fmt.fmt.pix_mp.width = in->width;
	fmt.fmt.pix_mp.height = in->height;
	fmt.fmt.pix_mp.num_planes = 1;
	if (ioctl(enc->fd, VIDIOC_S_FMT, &fmt) < 0)
		goto fail;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.pixelformat = in->pixelformat;
	fmt.fmt.pix_mp.width = in->width;
	fmt.fmt.pix_mp.height = in->height;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline = in->pitch;
	if (ioctl(enc->fd, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix_mp.pixelformat != in->pixelformat ||
	    fmt.fmt.pix_mp.width != in->width || fmt.fmt.pix_mp.height != in->height)
		goto fail;

	/*
	 * Surface backing is rounded up (e.g. a 3024x1964 screen in a
	 * 3072x1968 buffer): code only the surface itself. An encoder without
	 * crop support keeps the whole buffer.
	 */
	if (vis_w < in->width || vis_h < in->height) {
		struct v4l2_selection sel = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.target = V4L2_SEL_TGT_CROP,
			.r = { 0, 0, vis_w, vis_h },
		};

		if (ioctl(enc->fd, VIDIOC_S_SELECTION, &sel) < 0 ||
		    sel.r.width != vis_w || sel.r.height != vis_h)
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_UNSUPPORTED,
				   "enc-setup", 0,
				   "encoder cannot crop %ux%u to %ux%u; coding the whole buffer",
				   in->width, in->height, vis_w, vis_h);
	}

	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	parm.parm.output.timeperframe.numerator = enc->fps_den;
	parm.parm.output.timeperframe.denominator = enc->fps_num;
	ioctl(enc->fd, VIDIOC_S_PARM, &parm);

	set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
	set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
		 enc->intra_period ? (int32_t)enc->intra_period : 0);
	if (enc->coded == V4L2_PIX_FMT_H264)
		set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			 enc->profile == VAProfileH264ConstrainedBaseline ?
				V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE :
			 enc->profile == VAProfileH264High ?
				V4L2_MPEG_VIDEO_H264_PROFILE_HIGH :
				V4L2_MPEG_VIDEO_H264_PROFILE_MAIN);
	else
		set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			 V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
	if (enc_apply_rc(enc) < 0)
		goto fail;

	rb = (struct v4l2_requestbuffers) {
		.count = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_DMABUF,
	};
	if (ioctl(enc->fd, VIDIOC_REQBUFS, &rb) < 0 || !rb.count)
		goto fail;

	rb = (struct v4l2_requestbuffers) {
		.count = V4L2R_ENC_CAPTURE_BUFFERS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (ioctl(enc->fd, VIDIOC_REQBUFS, &rb) < 0 || !rb.count)
		goto fail;
	if (rb.count > V4L2R_ENC_CAPTURE_BUFFERS)
		rb.count = V4L2R_ENC_CAPTURE_BUFFERS;

	for (unsigned int i = 0; i < rb.count; i++) {
		struct v4l2_plane plane = {0};
		struct v4l2_buffer b = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1,
			.m.planes = &plane,
		};

		if (ioctl(enc->fd, VIDIOC_QUERYBUF, &b) < 0)
			goto fail;
		enc->cap_len[i] = plane.length;
		enc->cap_map[i] = mmap(NULL, plane.length, PROT_READ, MAP_SHARED,
				       enc->fd, plane.m.mem_offset);
		if (enc->cap_map[i] == MAP_FAILED)
			goto fail;
		enc->nb_captures = i + 1;
		if (ioctl(enc->fd, VIDIOC_QBUF, &b) < 0)
			goto fail;
	}

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(enc->fd, VIDIOC_STREAMON, &type) < 0)
		goto fail;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(enc->fd, VIDIOC_STREAMON, &type) < 0)
		goto fail;

	enc->width = in->width;
	enc->height = in->height;
	enc->pitch = in->pitch;
	enc->pixelformat = in->pixelformat;
	enc->crop_w = vis_w;
	enc->crop_h = vis_h;

	v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_INFO, V4L2R_DIAG_INFO, "enc-setup", 0,
		   "encoding %ux%u of %ux%u (pitch %u) to %.4s via %s [%s], rc 0x%x, gop %u",
		   vis_w, vis_h, in->width, in->height, in->pitch, (const char *)&enc->coded,
		   drv->encoder.video_path, drv->encoder.card, enc->rc_mode,
		   enc->intra_period);
	return VA_STATUS_SUCCESS;

fail:
	v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL, "enc-setup",
		   -errno, "encoder setup for %ux%u %.4s failed", in->width,
		   in->height, (const char *)&in->pixelformat);
	enc_teardown(enc);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

static int dqbuf_wait(int fd, struct v4l2_buffer *b, uint64_t deadline)
{
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT | POLLPRI };
		uint64_t now;

		if (ioctl(fd, VIDIOC_DQBUF, b) == 0)
			return 0;
		if (errno != EAGAIN)
			return -errno;
		now = v4l2r_now_ns();
		if (now >= deadline)
			return -ETIMEDOUT;
		if (poll(&pfd, 1, (int)((deadline - now) / 1000000) + 1) < 0 &&
		    errno != EINTR)
			return -errno;
	}
}

VAStatus v4l2r_enc_end_picture(struct v4l2r_context *ctx)
{
	struct v4l2r_enc *enc = ctx->enc;
	struct v4l2r_driver *drv = ctx->drv;
	struct v4l2r_frame_view view;
	struct v4l2r_buffer *coded;
	VACodedBufferSegment *seg;
	struct v4l2_plane plane;
	struct v4l2_buffer b;
	uint64_t deadline;
	size_t capacity;
	uint32_t rows;
	VAStatus status;
	int ret;

	if (!enc->have_picture || !enc->input)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	coded = V4L2R_BUFFER_GET(drv, enc->coded_buf);
	if (!coded || coded->type != VAEncCodedBufferType ||
	    coded->context_id != ctx->id)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	status = v4l2r_surface_ready(enc->input);
	if (status != VA_STATUS_SUCCESS)
		return status;
	status = v4l2r_surface_view(drv, enc->input, false, &view);
	if (status != VA_STATUS_SUCCESS)
		return status;
	if (view.nb_planes != 1 || view.pixelformat != V4L2_PIX_FMT_NV12)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	pthread_mutex_lock(&ctx->mutex);

	status = enc_configure(ctx, &view,
			       enc->input->width && enc->input->width <= view.width ?
				enc->input->width : view.width,
			       enc->input->height && enc->input->height <= view.height ?
				enc->input->height : view.height);
	if (status != VA_STATUS_SUCCESS)
		goto done;
	if (enc->rc_dirty && enc_apply_rc(enc) < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto done;
	}
	/* The encoder places its periodic IDRs itself; force the others. */
	if (enc->idr && enc->frames &&
	    (!enc->intra_period || enc->frames % enc->intra_period))
		set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);

	/* Luma rows then chroma rows at the surface pitch: the encoder takes
	 * the line size from the payload and repacks a different one. */
	rows = view.height + (view.height + 1) / 2;
	memset(&plane, 0, sizeof(plane));
	plane.m.fd = view.dmabuf_fd[0];
	plane.length = view.plane_size[0];
	plane.bytesused = view.pitch * rows;
	b = (struct v4l2_buffer) {
		.index = 0,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_DMABUF,
		.length = 1,
		.m.planes = &plane,
		.timestamp.tv_usec = enc->frames % 1000000,
		.timestamp.tv_sec = enc->frames / 1000000,
	};
	if (ioctl(enc->fd, VIDIOC_QBUF, &b) < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto done;
	}

	deadline = v4l2r_now_ns() + (uint64_t)V4L2R_ENC_TIMEOUT_MS * 1000000;
	memset(&plane, 0, sizeof(plane));
	b = (struct v4l2_buffer) {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.length = 1,
		.m.planes = &plane,
	};
	ret = dqbuf_wait(enc->fd, &b, deadline);
	if (ret < 0 || b.index >= enc->nb_captures || (b.flags & V4L2_BUF_FLAG_ERROR)) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
			   ret == -ETIMEDOUT ? V4L2R_DIAG_TIMEOUT : V4L2R_DIAG_KERNEL,
			   "enc-frame", ret, "no coded picture for frame %llu",
			   (unsigned long long)enc->frames);
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto done;
	}

	/* Coded buffers are a VACodedBufferSegment followed by the data. */
	seg = coded->data;
	capacity = v4l2r_buffer_bytes(coded) - sizeof(*seg);
	memset(seg, 0, sizeof(*seg));
	seg->buf = (uint8_t *)coded->data + sizeof(*seg);
	seg->size = plane.bytesused - plane.data_offset;
	if (seg->size > capacity) {
		seg->size = capacity;
		seg->status = VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
	}
	memcpy(seg->buf, (uint8_t *)enc->cap_map[b.index] + plane.data_offset, seg->size);

	/* Give the capture buffer back, then get the input surface back. */
	plane.bytesused = 0;
	ioctl(enc->fd, VIDIOC_QBUF, &b);
	memset(&plane, 0, sizeof(plane));
	b = (struct v4l2_buffer) {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_DMABUF,
		.length = 1,
		.m.planes = &plane,
	};
	if (dqbuf_wait(enc->fd, &b, deadline) < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto done;
	}

	enc->frames++;
	enc->input->status = VASurfaceReady;
	enc->input->decode_status = VA_STATUS_SUCCESS;
	status = VA_STATUS_SUCCESS;

done:
	if (status != VA_STATUS_SUCCESS)
		enc_teardown(enc);	/* restart with a fresh instance */
	pthread_mutex_unlock(&ctx->mutex);
	return status;
}
