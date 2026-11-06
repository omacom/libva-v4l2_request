/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/resource.h>

#include "v4l2_request.h"

static unsigned long reject;
static uint32_t capture_format;
static uint32_t expected_coded_format;
static uint32_t expected_control;
static unsigned int controls_seen;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	(void)fd;
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (request == reject) {
		errno = EINVAL;
		return -1;
	}
	if (request == VIDIOC_S_FMT) {
		struct v4l2_format *format = arg;

		assert(v4l2r_format_pixelformat(format) == expected_coded_format);
		if (V4L2_TYPE_IS_MULTIPLANAR(format->type)) {
			format->fmt.pix_mp.width = 1280;
			format->fmt.pix_mp.height = 720;
		} else {
			format->fmt.pix.width = 1280;
			format->fmt.pix.height = 720;
		}
		return 0;
	}
	if (request == VIDIOC_S_EXT_CTRLS) {
		struct v4l2_ext_controls *controls = arg;

		assert(expected_control);
		assert(controls->count == 1);
		assert(controls->controls[0].id == expected_control);
#if HAVE_V4L2_CTRL_HEVC
		if (expected_control == V4L2_CID_STATELESS_HEVC_SPS) {
			struct v4l2_ctrl_hevc_sps *sps = controls->controls[0].ptr;

			assert(sps->chroma_format_idc == 1);
			assert(sps->bit_depth_luma_minus8 == 2);
			assert(sps->bit_depth_chroma_minus8 == 2);
		}
#endif
#if HAVE_V4L2_CTRL_VP9
		if (expected_control == V4L2_CID_STATELESS_VP9_FRAME) {
			struct v4l2_ctrl_vp9_frame *frame = controls->controls[0].ptr;

			assert(frame->profile == 2 && frame->bit_depth == 10);
			assert(frame->flags & V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING);
			assert(frame->flags & V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING);
		}
#endif
#if HAVE_V4L2_CTRL_AV1
		if (expected_control == V4L2_CID_STATELESS_AV1_SEQUENCE) {
			struct v4l2_ctrl_av1_sequence *sequence = controls->controls[0].ptr;

			assert(sequence->seq_profile == 0 && sequence->bit_depth == 10);
			assert(sequence->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X);
			assert(sequence->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y);
		}
#endif
		controls_seen++;
		return 0;
	}
	assert(request == VIDIOC_ENUM_FMT);
	struct v4l2_fmtdesc *format = arg;
	if (format->index || !capture_format) {
		errno = EINVAL;
		return -1;
	}
	format->pixelformat = capture_format;
	return 0;
}

static void probe_10bit(void)
{
#if HAVE_V4L2_PIX_FMT_P010
	capture_format = V4L2_PIX_FMT_P010;

#if HAVE_V4L2_CTRL_HEVC
	expected_coded_format = V4L2_PIX_FMT_HEVC_SLICE;
	expected_control = V4L2_CID_STATELESS_HEVC_SPS;
	assert(v4l2r_probe_hevc_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT));
	assert(v4l2r_probe_hevc_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	assert(controls_seen == 2);
	capture_format = V4L2_PIX_FMT_NV12;
	assert(!v4l2r_probe_hevc_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	capture_format = V4L2_PIX_FMT_P010;
	reject = VIDIOC_S_EXT_CTRLS;
	assert(!v4l2r_probe_hevc_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	reject = 0;
#endif

#if HAVE_V4L2_CTRL_VP9
	expected_coded_format = V4L2_PIX_FMT_VP9_FRAME;
	expected_control = V4L2_CID_STATELESS_VP9_FRAME;
	assert(v4l2r_probe_vp9_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT));
	assert(v4l2r_probe_vp9_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	capture_format = V4L2_PIX_FMT_NV12;
	assert(!v4l2r_probe_vp9_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	capture_format = V4L2_PIX_FMT_P010;
	reject = VIDIOC_S_FMT;
	assert(!v4l2r_probe_vp9_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	reject = 0;
#endif

#if HAVE_V4L2_CTRL_AV1
	expected_coded_format = V4L2_PIX_FMT_AV1_FRAME;
	expected_control = V4L2_CID_STATELESS_AV1_SEQUENCE;
	assert(v4l2r_probe_av1_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT));
	assert(v4l2r_probe_av1_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
	capture_format = V4L2_PIX_FMT_NV12;
	assert(!v4l2r_probe_av1_10bit(123, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
#endif
#endif
}

static void init_driver(struct v4l2r_driver *drv, struct VADriverContext *va,
			uint32_t pixelformat)
{
	memset(drv, 0, sizeof(*drv));
	memset(va, 0, sizeof(*va));
	assert(!pthread_mutex_init(&drv->mutex, NULL));
	assert(!v4l2r_handles_init(&drv->configs, V4L2R_ID_OFFSET_CONFIG));
	drv->nb_decoders = 1;
	drv->converter_probed = true;
	drv->decoders[0].pixelformats[0] = pixelformat;
	drv->decoders[0].nb_pixelformats = 1;
	va->pDriverData = drv;
}

static void destroy_driver(struct v4l2r_driver *drv)
{
	v4l2r_handles_destroy(&drv->configs);
	pthread_mutex_destroy(&drv->mutex);
}

#if HAVE_V4L2_CTRL_AV1
static VAStatus create_config(struct VADriverContext *va, VAProfile profile,
			      unsigned int rt_format, VAConfigID *id)
{
	VAConfigAttrib attrib = {
		.type = VAConfigAttribRTFormat,
		.value = rt_format,
	};

	*id = VA_INVALID_ID;
	return v4l2r_CreateConfig(va, profile, VAEntrypointVLD, &attrib, 1, id);
}

static unsigned int get_rt_format(struct VADriverContext *va, VAProfile profile)
{
	VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat };

	assert(v4l2r_GetConfigAttributes(va, profile, VAEntrypointVLD,
					 &attrib, 1) == VA_STATUS_SUCCESS);
	return attrib.value;
}
#endif

static void profile_gates(void)
{
	struct v4l2r_driver drv;
	struct VADriverContext va;
#if HAVE_V4L2_CTRL_AV1
	VAConfigID id;
#endif

#if HAVE_V4L2_CTRL_HEVC
	init_driver(&drv, &va, V4L2_PIX_FMT_HEVC_SLICE);
	assert(!v4l2r_decoder_supports_profile(&drv.decoders[0],
					       VAProfileHEVCMain10));
	drv.decoders[0].hevc_10bit = true;
	assert(v4l2r_decoder_supports_profile(&drv.decoders[0],
					      VAProfileHEVCMain10));
	destroy_driver(&drv);
#endif

#if HAVE_V4L2_CTRL_VP9
	init_driver(&drv, &va, V4L2_PIX_FMT_VP9_FRAME);
	assert(!v4l2r_decoder_supports_profile(&drv.decoders[0],
					       VAProfileVP9Profile2));
	drv.decoders[0].vp9_10bit = true;
	assert(v4l2r_decoder_supports_profile(&drv.decoders[0],
					      VAProfileVP9Profile2));
	destroy_driver(&drv);
#endif

#if HAVE_V4L2_CTRL_AV1
	init_driver(&drv, &va, V4L2_PIX_FMT_AV1_FRAME);
	assert(get_rt_format(&va, VAProfileAV1Profile0) == VA_RT_FORMAT_YUV420);
	assert(create_config(&va, VAProfileAV1Profile0,
			     VA_RT_FORMAT_YUV420_10, &id) ==
	       VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
	drv.decoders[0].av1_10bit = true;
	assert(get_rt_format(&va, VAProfileAV1Profile0) ==
	       (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10));
	assert(create_config(&va, VAProfileAV1Profile0,
			     VA_RT_FORMAT_YUV420_10, &id) == VA_STATUS_SUCCESS);
	assert(v4l2r_DestroyConfig(&va, id) == VA_STATUS_SUCCESS);
	destroy_driver(&drv);
#endif
}

int main(void)
{
	struct rlimit core = { 0, 0 };

	assert(!setrlimit(RLIMIT_CORE, &core));
	probe_10bit();
	profile_gates();
	return 0;
}
