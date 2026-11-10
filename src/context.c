/*
 * VA context lifecycle: decoder device selection, queue setup and
 * surface-to-CAPTURE-buffer binding.
 *
 * The CAPTURE side is configured lazily on the first decode, once the
 * codec controls (e.g. the SPS) are known, so the kernel driver can pick
 * a suitable frame format - mirroring the FFmpeg hwaccel probe order.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "v4l2_request.h"

static int set_format(struct v4l2r_context *ctx, enum v4l2_buf_type type,
		      uint32_t pixelformat, uint32_t width, uint32_t height,
		      uint32_t buffersize)
{
	struct v4l2_format format = {
		.type = type,
	};

	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		format.fmt.pix_mp.width = width;
		format.fmt.pix_mp.height = height;
		format.fmt.pix_mp.pixelformat = pixelformat;
		format.fmt.pix_mp.plane_fmt[0].sizeimage = buffersize;
		format.fmt.pix_mp.num_planes = 1;
	} else {
		format.fmt.pix.width = width;
		format.fmt.pix.height = height;
		format.fmt.pix.pixelformat = pixelformat;
		format.fmt.pix.sizeimage = buffersize;
	}

	if (ioctl(ctx->video_fd, VIDIOC_S_FMT, &format) < 0)
		return -errno;

	return 0;
}

static int query_buffer_capabilities(struct v4l2r_context *ctx,
				     enum v4l2_buf_type type,
				     uint32_t *capabilities)
{
	struct v4l2_create_buffers buffers = {
		.count = 0,
		.memory = V4L2_MEMORY_MMAP,
		.format.type = type,
	};

	if (ioctl(ctx->video_fd, VIDIOC_CREATE_BUFS, &buffers) < 0)
		return -errno;

	*capabilities = buffers.capabilities;
	return 0;
}

static bool try_output_format(struct v4l2r_context *ctx, uint32_t pixelformat)
{
	struct v4l2_fmtdesc fmtdesc = {
		.type = ctx->output_format.type,
	};

	while (ioctl(ctx->video_fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0) {
		if (fmtdesc.pixelformat == pixelformat)
			return true;
		fmtdesc.index++;
	}

	return false;
}

static bool dimension_axis(uint32_t *min, uint32_t *max, uint32_t step,
			   uint32_t floor, uint32_t ceiling)
{
	if (!*min || *min > *max || !step || *min > ceiling)
		return false;
	uint64_t first = *min;
	if (first < floor)
		first += ((floor - first + step - 1) / step) * step;
	uint32_t last = *max < ceiling ? *max : ceiling;
	if (first > last)
		return false;
	*min = first;
	*max = first + ((last - first) / step) * step;
	return true;
}

/* width == 0 queries the envelope; otherwise validate one coded-size pair. */
static bool query_dimensions(int fd, uint32_t pixelformat, bool is_avd,
			    uint32_t width, uint32_t height,
			    struct v4l2r_dimensions *bounds)
{
	struct v4l2_frmsizeenum frmsize = {
		.pixel_format = pixelformat,
	};
	/* asahi-7.1.13-3 / 94fb23346d522edf53722357c426a3e58030beea:
	 * avd_enum_framesizes advertises 1 while avd-vp9.c rejects coded
	 * dimensions below 64. The format descriptor caps VP9 at 4096.
	 * This is a kernel contract, NOT evidence of a firmware minimum.
	 * Allocation steps (64 x 16) are not coded-size steps: 66x66 is valid. */
	bool avd_vp9 = is_avd && pixelformat == v4l2_fourcc('V', 'P', '9', 'F');
	uint32_t floor = avd_vp9 ? V4L2R_AVD_VP9_MIN_DIMENSION : 1;
	uint32_t ceiling = avd_vp9 ? V4L2R_AVD_VP9_MAX_DIMENSION : 65536;
	*bounds = (struct v4l2r_dimensions){0};
	for (;;) {
		int ret = ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize);
		if (ret < 0) {
			/* Preserve ENOTTY compatibility; EINVAL at index zero is
			 * an empty/unsupported format, not a license to guess. */
			if (!frmsize.index && errno == ENOTTY) {
				*bounds = (struct v4l2r_dimensions){floor, floor, ceiling, ceiling};
				return !width || (width >= floor && width <= ceiling &&
						 height >= floor && height <= ceiling);
			}
			return !width && frmsize.index && errno == EINVAL && bounds->min_width;
		}
		uint32_t min_w, min_h, max_w, max_h, step_w = 1, step_h = 1;
		bool discrete = frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE;
		if (discrete) {
			min_w = max_w = frmsize.discrete.width;
			min_h = max_h = frmsize.discrete.height;
		} else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
			   frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
			min_w = frmsize.stepwise.min_width;
			min_h = frmsize.stepwise.min_height;
			max_w = frmsize.stepwise.max_width;
			max_h = frmsize.stepwise.max_height;
			if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
				step_w = frmsize.stepwise.step_width;
				step_h = frmsize.stepwise.step_height;
			}
		} else {
			return false;
		}
		if (dimension_axis(&min_w, &max_w, step_w, floor, ceiling) &&
		    dimension_axis(&min_h, &max_h, step_h, floor, ceiling)) {
			if (!bounds->min_width || min_w < bounds->min_width) bounds->min_width = min_w;
			if (!bounds->min_height || min_h < bounds->min_height) bounds->min_height = min_h;
			if (max_w > bounds->max_width) bounds->max_width = max_w;
			if (max_h > bounds->max_height) bounds->max_height = max_h;
			if (width && width >= min_w && width <= max_w &&
			    height >= min_h && height <= max_h &&
			    !((width - min_w) % step_w) && !((height - min_h) % step_h))
				return true;
		}
		/* V4L2 only permits one stepwise/continuous entry. */
		if (!discrete)
			return !width && bounds->min_width;
		if (++frmsize.index == 0)
			return false;
	}
}

bool v4l2r_context_dimensions(struct v4l2r_context *ctx,
			      uint32_t width, uint32_t height)
{
	if (!width || !height)
		return false;
	/* Context creation already checked this pair on the selected decoder.
	 * Only a differing coded size needs another enumeration. */
	if (width == ctx->picture_width && height == ctx->picture_height)
		return true;
	struct v4l2r_dimensions bounds;
	return query_dimensions(ctx->video_fd, ctx->codec->pixelformat,
				ctx->is_avd, width, height, &bounds);
}

VAStatus v4l2r_config_dimensions(struct v4l2r_driver *drv,
				const struct v4l2r_config *config,
				struct v4l2r_dimensions *bounds)
{
	*bounds = (struct v4l2r_dimensions){1, 1, 65536, 65536};
	if (!config->codec) /* Video processing has no coded-format contract. */
		return VA_STATUS_SUCCESS;
	*bounds = (struct v4l2r_dimensions){0};
	for (unsigned int i = 0; i < drv->nb_decoders; i++) {
		const struct v4l2r_decoder *decoder = &drv->decoders[i];
#if VA_CHECK_VERSION(1, 18, 0)
		if (config->profile == VAProfileH264High10 && !decoder->h264_10bit)
			continue;
#endif
		bool accepts = false;
		for (unsigned int j = 0; j < decoder->nb_pixelformats; j++)
			accepts |= decoder->pixelformats[j] == config->codec->pixelformat;
		if (!accepts)
			continue;
		int fd = open(decoder->video_path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;
		struct v4l2_capability cap = {0};
		struct v4l2r_dimensions candidate;
		bool valid = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
			query_dimensions(fd, config->codec->pixelformat,
				!strcmp((const char *)cap.driver, "avd"), 0, 0, &candidate);
		close(fd);
		if (!valid)
			continue;
		/* A config precedes decoder selection. Report the envelope of
		 * its candidates; CreateContext checks each candidate exactly. */
		if (!bounds->min_width || candidate.min_width < bounds->min_width)
			bounds->min_width = candidate.min_width;
		if (!bounds->min_height || candidate.min_height < bounds->min_height)
			bounds->min_height = candidate.min_height;
		if (candidate.max_width > bounds->max_width) bounds->max_width = candidate.max_width;
		if (candidate.max_height > bounds->max_height) bounds->max_height = candidate.max_height;
	}
	return bounds->min_width ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}

/*
 * Pick a CAPTURE format. Preference order:
 *  1. linear formats representable as VA images with matching bit depth,
 *     so display interop (dma-buf import) and readback work best,
 *  2. the driver preferred (default) format with matching bit depth,
 *  3. any known format with matching bit depth,
 *  4. any known format.
 */
static int select_capture_format(struct v4l2r_context *ctx,
				 struct v4l2r_surface *surface)
{
	enum v4l2_buf_type type = ctx->capture_format.type;
	const struct v4l2r_format_info *info;
	struct v4l2_format format = {
		.type = type,
	};
	struct v4l2_fmtdesc fmtdesc = {
		.type = type,
	};
	uint32_t best = 0;
	int best_score = -1;

	while (ioctl(ctx->video_fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0) {
		info = v4l2r_format_by_pixelformat(fmtdesc.pixelformat);
		fmtdesc.index++;

		if (!info)
			continue;

		bool depth_ok = !ctx->bit_depth ||
				info->bit_depth == ctx->bit_depth;
		int score = 0;

		if (depth_ok)
			score += 2;
		if (info->linear && info->va_fourcc && depth_ok)
			score += 4;

		if (score > best_score) {
			best_score = score;
			best = info->pixelformat;
		}
	}

	/* Prefer the driver default over other non-linear candidates. */
	if (best_score < 6 &&
	    ioctl(ctx->video_fd, VIDIOC_G_FMT, &format) >= 0) {
		info = v4l2r_format_by_pixelformat(v4l2r_format_pixelformat(&format));
		if (info && (!ctx->bit_depth || info->bit_depth == ctx->bit_depth))
			best = info->pixelformat;
	}

	if (!best)
		return -EINVAL;

	/* Clients may allocate a padded surface larger than the coded picture.
	 * Keep its advertised chroma offset when importing pre-exported memory. */
	return set_format(ctx, type, best,
			surface->backing ? surface->backing->width : ctx->picture_width,
			surface->backing ? surface->backing->height : ctx->picture_height, 0);
}

/* --- OUTPUT bitstream buffers with their media requests --- */

static void output_buffer_cleanup(struct v4l2r_context *ctx,
				  struct v4l2r_output_buffer *output)
{
	(void)ctx;

	if (output->request_fd >= 0) {
		close(output->request_fd);
		output->request_fd = -1;
	}

	if (output->addr) {
		munmap(output->addr, output->size);
		output->addr = NULL;
	}
}

/* Create and map one OUTPUT buffer; sizeimage overrides the size from the
 * queue format when nonzero (used to grow buffers mid-stream). */
static int output_buffer_setup(struct v4l2r_context *ctx,
			       struct v4l2r_output_buffer *output,
			       uint32_t sizeimage)
{
	struct v4l2_create_buffers buffers = {
		.count = 1,
		.memory = V4L2_MEMORY_MMAP,
		.format = ctx->output_format,
	};
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
	struct v4l2_buffer buffer = {0};
	off_t offset;
	void *addr;

	if (sizeimage) {
		if (V4L2_TYPE_IS_MULTIPLANAR(buffers.format.type))
			buffers.format.fmt.pix_mp.plane_fmt[0].sizeimage =
				sizeimage;
		else
			buffers.format.fmt.pix.sizeimage = sizeimage;
	}

	if (ioctl(ctx->video_fd, VIDIOC_CREATE_BUFS, &buffers) < 0) {
		int ret = -errno;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "output-alloc", ret, "failed to create OUTPUT buffer: %s",
			   strerror(-ret));
		return ret;
	}

	/* The queued_output/queued_request bitmasks track buffers by index. */
	if (buffers.index >= 32) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "output-alloc", -ENOSPC,
			   "OUTPUT buffer index %u exceeds the tracked range",
			   buffers.index);
		return -ENOSPC;
	}

	buffer.type = ctx->output_format.type;
	buffer.index = buffers.index;
	if (V4L2_TYPE_IS_MULTIPLANAR(buffer.type)) {
		buffer.length = VIDEO_MAX_PLANES;
		buffer.m.planes = planes;
	}

	if (ioctl(ctx->video_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
		int ret = -errno;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
			   "output-query", ret, "failed to query OUTPUT buffer %u: %s",
			   buffers.index, strerror(-ret));
		return ret;
	}

	output->index = buffer.index;
	if (V4L2_TYPE_IS_MULTIPLANAR(buffer.type)) {
		output->size = buffer.m.planes[0].length;
		offset = buffer.m.planes[0].m.mem_offset;
	} else {
		output->size = buffer.length;
		offset = buffer.m.offset;
	}

	addr = mmap(NULL, output->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    ctx->video_fd, offset);
	if (addr == MAP_FAILED) {
		int ret = -errno;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "output-map", ret, "failed to map OUTPUT buffer %u: %s",
			   output->index, strerror(-ret));
		return ret;
	}
	output->addr = addr;
	output->bytesused = 0;

	if (ioctl(ctx->media_fd, MEDIA_IOC_REQUEST_ALLOC, &output->request_fd) < 0) {
		int ret = -errno;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
			   ret == -ENOMEM ? V4L2R_DIAG_ALLOCATION : V4L2R_DIAG_KERNEL,
			   "request-alloc", ret, "failed to allocate request: %s",
			   strerror(-ret));
		output->request_fd = -1;
		return ret;
	}

	v4l2r_trace("allocated OUTPUT buffer #%u (%u bytes)\n",
		  output->index, output->size);

	return 0;
}

/*
 * Replace an OUTPUT buffer whose bitstream ran out of room with a larger
 * one, preserving the data staged so far. V4L2 cannot free individual
 * buffers, so the replaced one stays allocated (but idle) until the
 * context is destroyed; sizes double, so this happens at most a few times
 * per context and lets the initial allocation stay far below the raw
 * frame size.
 */
int v4l2r_output_buffer_grow(struct v4l2r_context *ctx,
			     struct v4l2r_output_buffer *output,
			     size_t min_size)
{
	struct v4l2r_output_buffer grown = { .request_fd = -1 };
	uint32_t size = output->size;
	int ret;

	while ((size_t)size < min_size) {
		if (size > UINT32_MAX / 2)
			return -ENOSPC;
		size *= 2;
	}

	ret = output_buffer_setup(ctx, &grown, size);
	if (ret < 0 || grown.size < min_size) {
		output_buffer_cleanup(ctx, &grown);
		return ret < 0 ? ret : -ENOSPC;
	}

	memcpy(grown.addr, output->addr, output->bytesused);
	grown.bytesused = output->bytesused;

	/* The replaced buffer is idle - next_output() waited for its dequeue
	 * before the picture started - but its request may still be marked in
	 * flight; forget it along with the buffer. */
	pthread_mutex_lock(&ctx->mutex);
	ctx->queued_request &= ~(1u << output->index);
	output_buffer_cleanup(ctx, output);
	*output = grown;
	pthread_mutex_unlock(&ctx->mutex);

	v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_INFO, V4L2R_DIAG_INFO, "output-grow", 0,
		   "grew OUTPUT buffer to %u bytes", grown.size);

	return 0;
}

/* --- CAPTURE buffer pool --- */

static void capture_buffer_cleanup(struct v4l2r_context *ctx,
				   struct v4l2r_capture_buffer *capture)
{
	(void)ctx;

	for (unsigned int i = 0; i < VIDEO_MAX_PLANES; i++) {
		if (capture->map[i]) {
			munmap(capture->map[i], capture->plane_size[i]);
			capture->map[i] = NULL;
		}
		if (capture->dmabuf_fd[i] >= 0) {
			close(capture->dmabuf_fd[i]);
			capture->dmabuf_fd[i] = -1;
		}
	}

	if (capture->surface) {
		capture->surface->ctx = NULL;
		capture->surface->capture_index = -1;
		capture->surface = NULL;
	}
}

static void free_list_push(struct v4l2r_context *ctx, int index)
{
	unsigned int tail = (ctx->free_head + ctx->free_count) %
			    V4L2R_MAX_CAPTURE_BUFFERS;

	/* Balanced by owned<->free transitions, so free_count never exceeds
	 * nb_captures and the ring cannot overflow. */
	ctx->free_captures[tail] = (uint8_t)index;
	ctx->free_count++;
}

static int free_list_pop(struct v4l2r_context *ctx)
{
	int index;

	if (!ctx->free_count)
		return -1;

	index = ctx->free_captures[ctx->free_head];
	ctx->free_head = (ctx->free_head + 1) % V4L2R_MAX_CAPTURE_BUFFERS;
	ctx->free_count--;

	return index;
}

/* Per-CAPTURE-buffer size implied by the negotiated format, in bytes. */
static size_t capture_format_bytes(const struct v4l2_format *fmt)
{
	size_t bytes = 0;

	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
		for (unsigned int i = 0; i < fmt->fmt.pix_mp.num_planes; i++)
			bytes += fmt->fmt.pix_mp.plane_fmt[i].sizeimage;
	} else {
		bytes = fmt->fmt.pix.sizeimage;
	}

	return bytes;
}

/* Allocate one new CAPTURE buffer, returning its index (unbound). */
static int capture_buffer_new(struct v4l2r_context *ctx)
{
	struct v4l2_create_buffers buffers = {
		.count = 1,
		.memory = ctx->capture_memory,
		.format = ctx->capture_format,
	};
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
	struct v4l2_buffer buffer = {0};
	struct v4l2r_capture_buffer *capture;
	size_t bufsize = capture_format_bytes(&ctx->capture_format);

	if (ctx->nb_captures >= V4L2R_MAX_CAPTURE_BUFFERS) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "capture-alloc", -ENOSPC,
			   "CAPTURE buffer limit reached (%u buffers, ~%llu MiB); "
			   "refusing to allocate more", ctx->nb_captures,
			   (unsigned long long)((uint64_t)ctx->nb_captures *
						bufsize >> 20));
		return -ENOSPC;
	}

	if (ioctl(ctx->video_fd, VIDIOC_CREATE_BUFS, &buffers) < 0) {
		int ret = -errno;

		/* CREATE_BUFS mostly fails for lack of (CMA) memory; keep other
		 * rejections apart so a bad format is not mistaken for OOM. */
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
			   ret == -EINVAL ? V4L2R_DIAG_KERNEL : V4L2R_DIAG_ALLOCATION,
			   "capture-alloc", ret,
			   "failed to allocate CAPTURE buffer #%u (%zu bytes; "
			   "already %u buffers ~%llu MiB allocated): %s",
			   ctx->nb_captures, bufsize, ctx->nb_captures,
			   (unsigned long long)((uint64_t)ctx->nb_captures *
						bufsize >> 20),
			   strerror(-ret));
		return ret;
	}

	if (buffers.index >= V4L2R_MAX_CAPTURE_BUFFERS) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "capture-alloc", -ENOSPC,
			   "CAPTURE buffer index %u exceeds the tracked range",
			   buffers.index);
		return -ENOSPC;
	}

	buffer.type = ctx->capture_format.type;
	buffer.index = buffers.index;
	if (V4L2_TYPE_IS_MULTIPLANAR(buffer.type)) {
		buffer.length = VIDEO_MAX_PLANES;
		buffer.m.planes = planes;
	}

	if (ioctl(ctx->video_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
		int ret = -errno;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
			   "capture-query", ret, "failed to query CAPTURE buffer %u: %s",
			   buffers.index, strerror(-ret));
		return ret;
	}

	capture = &ctx->captures[buffer.index];
	memset(capture, 0, sizeof(*capture));

	if (V4L2_TYPE_IS_MULTIPLANAR(buffer.type)) {
		capture->nb_planes = ctx->capture_format.fmt.pix_mp.num_planes;
		for (unsigned int i = 0; i < capture->nb_planes; i++) {
			/* mem_offset is only meaningful for MMAP buffers. */
			capture->plane_mem_offset[i] =
				ctx->capture_memory == V4L2_MEMORY_MMAP ?
				buffer.m.planes[i].m.mem_offset : 0;
			capture->plane_size[i] = buffer.m.planes[i].length;
		}
	} else {
		capture->nb_planes = 1;
		capture->plane_mem_offset[0] =
			ctx->capture_memory == V4L2_MEMORY_MMAP ?
			buffer.m.offset : 0;
		capture->plane_size[0] = buffer.length;
	}

	for (unsigned int i = 0; i < VIDEO_MAX_PLANES; i++)
		capture->dmabuf_fd[i] = -1;

	if (buffer.index >= ctx->nb_captures)
		ctx->nb_captures = buffer.index + 1;

	{
		size_t got = 0;
		for (unsigned int i = 0; i < capture->nb_planes; i++)
			got += capture->plane_size[i];
		v4l2r_trace("allocated CAPTURE buffer #%u (%zu bytes); "
			  "%u buffers, ~%llu MiB total\n", buffer.index, got,
			  ctx->nb_captures,
			  (unsigned long long)((uint64_t)ctx->nb_captures *
					       got >> 20));
	}

	return buffer.index;
}

/*
 * Wait for external consumers of the buffer's exported dma-buf(s) to finish
 * reading. POLLOUT on a dma-buf completes only once all fences on its
 * reservation object have signalled, which for an imported buffer includes the
 * read fence a GPU attaches while sampling it. Buffers that were never exported
 * carry fd == -1 and are skipped.
 */
static int capture_wait_readers(struct v4l2r_context *ctx,
				struct v4l2r_capture_buffer *capture, int index)
{
	for (unsigned int i = 0; i < capture->nb_planes; i++) {
		if (capture->dmabuf_fd[i] < 0)
			continue;

		int ret = v4l2r_poll_one(capture->dmabuf_fd[i], POLLOUT,
					 V4L2R_POLL_TIMEOUT_MS);
		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   v4l2r_diag_errno_category(ret), "reader-wait",
				   ret, "failed waiting for readers of CAPTURE "
				   "buffer %d plane %u", index, i);
			return ret;
		}
	}
	return 0;
}

/*
 * Does standalone backing have exactly the layout of this context's CAPTURE
 * format? Only then can the decoder write into it in place of an MMAP
 * buffer: the client has already been told this pitch and chroma offset
 * through vaExportSurfaceHandle(), and the kernel refuses an imported
 * dma-buf smaller than sizeimage.
 */
static bool backing_matches_capture(const struct v4l2r_context *ctx,
				    const struct v4l2r_surface_backing *backing)
{
	const struct v4l2_format *fmt = &ctx->capture_format;
	bool mplane = V4L2_TYPE_IS_MULTIPLANAR(fmt->type);
	unsigned int nb_planes = mplane ? fmt->fmt.pix_mp.num_planes : 1;

	if (backing->pixelformat != v4l2r_format_pixelformat(fmt) ||
	    backing->width != v4l2r_format_width(fmt) ||
	    backing->height != v4l2r_format_height(fmt) ||
	    backing->pitch != v4l2r_format_bytesperline(fmt))
		return false;

	/* Say which check failed: the callers only print the layout above. */
	if (backing->nb_planes != nb_planes) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_CLIENT,
			   "backing-match", 0,
			   "backing has %u plane(s), the CAPTURE format %u",
			   backing->nb_planes, nb_planes);
		return false;
	}

	for (unsigned int i = 0; i < nb_planes; i++) {
		unsigned int sizeimage = mplane ?
			fmt->fmt.pix_mp.plane_fmt[i].sizeimage :
			fmt->fmt.pix.sizeimage;

		if (backing->plane_size[i] < sizeimage) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_CLIENT,
				   "backing-match", 0,
				   "backing plane %u is %u bytes, the CAPTURE format needs %u",
				   i, (unsigned int)backing->plane_size[i], sizeimage);
			return false;
		}
	}

	return true;
}

/*
 * DMABUF mode: point CAPTURE buffer |index| at the surface's standalone
 * backing, so the decoder writes into the very dma-buf the client already
 * exported. A surface without backing gets one allocated (a recycled
 * buffer could keep its old memory, but a fresh backing keeps every
 * surface's export stable and independent of buffer recycling).
 */
static int capture_attach_backing(struct v4l2r_context *ctx, int index,
				  struct v4l2r_surface *surface)
{
	struct v4l2r_capture_buffer *capture = &ctx->captures[index];
	struct v4l2r_surface_backing *backing;

	if (!surface->backing &&
	    v4l2r_surface_alloc_backing(ctx->drv, surface) != VA_STATUS_SUCCESS) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "backing-alloc", -ENOMEM,
			   "failed to allocate dma-buf backing for surface 0x%08x",
			   surface->id);
		return -ENOMEM;
	}
	backing = surface->backing;

	if (!backing_matches_capture(ctx, backing)) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_CLIENT,
			   "backing-match", -EINVAL,
			   "surface 0x%08x backing (%.4s %ux%u pitch %u) does not "
			   "match the CAPTURE format (%.4s %ux%u pitch %u)",
			   surface->id, (const char *)&backing->pixelformat,
			   backing->width, backing->height, backing->pitch,
			   (const char *)&(uint32_t){v4l2r_format_pixelformat(&ctx->capture_format)},
			   v4l2r_format_width(&ctx->capture_format),
			   v4l2r_format_height(&ctx->capture_format),
			   v4l2r_format_bytesperline(&ctx->capture_format));
		return -EINVAL;
	}

	capture_buffer_cleanup(ctx, capture);

	capture->nb_planes = backing->nb_planes;
	for (unsigned int i = 0; i < backing->nb_planes; i++) {
		int fd = fcntl(backing->dmabuf_fd[i], F_DUPFD_CLOEXEC, 0);

		if (fd < 0) {
			int ret = -errno;

			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   V4L2R_DIAG_ALLOCATION, "backing-attach", ret,
				   "failed to duplicate backing dma-buf: %s",
				   strerror(-ret));
			for (unsigned int j = 0; j < i; j++) {
				close(capture->dmabuf_fd[j]);
				capture->dmabuf_fd[j] = -1;
			}
			return ret;
		}
		capture->dmabuf_fd[i] = fd;
		capture->plane_size[i] = backing->plane_size[i];
		capture->plane_mem_offset[i] = 0;
	}

	return 0;
}

/*
 * Ensure the surface has a CAPTURE buffer and that the buffer is safe to
 * decode into now. A surface keeps the same buffer for its whole lifetime -
 * so vaExportSurfaceHandle() is stable per VASurfaceID - and the buffer is
 * only allocated (or an orphan from a destroyed surface recycled) on the
 * surface's first decode.
 *
 * Before a later decode overwrites the buffer, wait until its own previous
 * decode has finished and, crucially, until every frame that referenced its
 * previous contents has completed. The kernel matches reference frames purely
 * by CAPTURE buffer timestamp and offers no protection against overwriting
 * one still in use (dev-stateless-decoder.rst), so this wait is what prevents
 * reference corruption.
 */
static int capture_buffer_bind(struct v4l2r_context *ctx,
			       struct v4l2r_surface *surface)
{
	int index = surface->capture_index;

	/* Refresh the completion counter with anything already finished. */
	v4l2r_reap_capture(ctx);
	if (ctx->failed)
		return -EIO;

	/*
	 * The first buffer fixes the queue's memory type. Normally the
	 * decoder allocates (MMAP). But a client that exports a surface
	 * before decoding into it - Chromium creates each VA surface, calls
	 * vaExportSurfaceHandle() at once and imports the dma-buf into its
	 * GPU, then decodes - already holds the standalone backing that
	 * export produced, and would keep showing that never-written
	 * memory if the decode went to a fresh MMAP buffer. In that case
	 * run the queue in DMABUF mode and decode into the exported memory.
	 * The format converter chain has its own backing rules, keep MMAP
	 * there.
	 */
	if (!ctx->capture_memory) {
		if (!ctx->conv && surface->backing) {
			ctx->capture_memory = V4L2_MEMORY_DMABUF;
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_INFO, V4L2R_DIAG_INFO,
				   "capture-memory", 0,
				   "surface 0x%08x was exported before its first "
				   "decode; decoding into client-exported dma-bufs",
				   surface->id);
		} else {
			ctx->capture_memory = V4L2_MEMORY_MMAP;
		}
	}
	if (!ctx->conv && surface->backing &&
	    (ctx->capture_memory != V4L2_MEMORY_DMABUF ||
	     !backing_matches_capture(ctx, surface->backing))) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_CLIENT,
			   "capture-bind", -EINVAL,
			   "cannot decode into the already-exported surface layout");
		return -EINVAL;
	}

	if (index < 0) {
		/* First decode into this surface: recycle a buffer orphaned by
		 * a destroyed surface, or grow the pool. */
		bool recycled = true;

		index = free_list_pop(ctx);
		if (index < 0) {
			recycled = false;
			index = capture_buffer_new(ctx);
			if (index < 0)
				return index;
		}

		if (ctx->capture_memory == V4L2_MEMORY_DMABUF) {
			int ret;

			/* A recycled buffer may still be decoding into its
			 * old memory; let that finish before re-pointing it. */
			if (recycled && v4l2r_sync_capture(ctx, index) != VA_STATUS_SUCCESS) {
				free_list_push(ctx, index);
				return -EIO;
			}
			ret = capture_attach_backing(ctx, index, surface);
			if (ret < 0) {
				free_list_push(ctx, index);
				return ret;
			}
		}

		ctx->captures[index].surface = surface;
		surface->ctx = ctx;
		surface->capture_index = index;

		v4l2r_trace("bind surface 0x%08x -> CAPTURE buffer #%d (%s)\n",
			  surface->id, index, recycled ? "recycled" : "new");
	}

	/* Do not overwrite the buffer while its own last decode is in flight,
	 * while any frame still references its current contents, or while
	 * the format converter is still reading it. */
	uint64_t t0 = v4l2r_now_ns();
	if (v4l2r_sync_capture(ctx, index) != VA_STATUS_SUCCESS)
		return -EIO;
	uint64_t t1 = v4l2r_now_ns();
	if (v4l2r_wait_completed(ctx, ctx->captures[index].last_ref_seq) != VA_STATUS_SUCCESS)
		return -EIO;
	uint64_t t2 = v4l2r_now_ns();
	if (v4l2r_convert_drain_index(ctx, index) != VA_STATUS_SUCCESS)
		return -EIO;

	/*
	 * The decoder writes into this buffer and the kernel offers no implicit
	 * synchronisation against an external consumer of the exported dma-buf.
	 * When a GPU is sampling it (e.g. mpv --vo=gpu importing the dma-buf as
	 * an EGL image), reuse would race the decode against the still in-flight
	 * read and tear the picture. POLLOUT on a dma-buf blocks until every
	 * fence on its reservation - including the GPU's read fence - signals, so
	 * wait for readers to finish before handing the buffer back to decode.
	 * No-op for a buffer never exported (fd < 0) or already idle.
	 */
	if (capture_wait_readers(ctx, &ctx->captures[index], index) < 0)
		return -EIO;
	uint64_t t3 = v4l2r_now_ns();

	v4l2r_trace("bind wait (surface 0x%08x buf #%d): sync %.2f refwait %.2f "
		    "readers %.2f ms\n", surface->id, index,
		    (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6);

	return 0;
}

void v4l2r_context_release_capture(struct v4l2r_context *ctx, int index)
{
	if (index < 0 || index >= (int)ctx->nb_captures)
		return;

	/* Only owned buffers transition to free; ignore an already-free one. */
	if (!ctx->captures[index].surface)
		return;

	/* Orphan the buffer for later recycling. It keeps its last_ref_seq so
	 * a future decode that recycles it still waits out any frame that
	 * referenced its contents. */
	v4l2r_trace("release surface 0x%08x, orphan CAPTURE buffer #%d "
		  "(free list now %u)\n",
		  ctx->captures[index].surface->id, index, ctx->free_count + 1);
	ctx->captures[index].surface->capture_index = -1;
	ctx->captures[index].surface = NULL;
	free_list_push(ctx, index);
}

VAStatus v4l2r_flush_surface(struct v4l2r_surface *surface)
{
	if (surface && surface->ctx && surface->ctx->failed)
		return surface->decode_status;
	if (surface && surface->ctx && surface->ctx->codec &&
	    surface->ctx->codec->flush)
		return surface->ctx->codec->flush(surface->ctx, surface);
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_context_bind_surface(struct v4l2r_context *ctx,
				    struct v4l2r_surface *surface)
{
	enum v4l2_buf_type type;
	bool starting = !ctx->streaming;
	int ret;

	if (ctx->failed)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (surface->ctx && surface->ctx != ctx) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "capture-bind", 0,
			   "surface 0x%08x belongs to another context", surface->id);
		return VA_STATUS_ERROR_SURFACE_BUSY;
	}

	if (starting) {
		ret = select_capture_format(ctx, surface);
		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   V4L2R_DIAG_UNSUPPORTED, "capture-format", ret,
				   "failed to select a CAPTURE format: %s",
				   strerror(-ret));
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}

		if (ioctl(ctx->video_fd, VIDIOC_G_FMT, &ctx->capture_format) < 0) {
			ret = -errno;
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
				   "capture-format", ret,
				   "failed to read the CAPTURE format: %s",
				   strerror(-ret));
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}

		ret = query_buffer_capabilities(ctx, ctx->capture_format.type,
						&ctx->capture_capabilities);
		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
				   "capture-caps", ret,
				   "failed to query CAPTURE capabilities: %s",
				   strerror(-ret));
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}

		/*
		 * When the decoder can only produce a format nothing can
		 * consume directly (packed 10-bit NV15), chain the hardware
		 * format converter behind it. Without a converter such a
		 * stream cannot be presented at all: fail the decode so the
		 * client falls back to software instead of getting frames
		 * it cannot display.
		 */
		{
			const struct v4l2r_format_info *info =
				v4l2r_format_by_pixelformat(
					v4l2r_format_pixelformat(&ctx->capture_format));

			if (info && !info->va_fourcc) {
				v4l2r_convert_setup(ctx);
				if (!ctx->conv) {
					v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
						   V4L2R_DIAG_UNSUPPORTED,
						   "capture-format", 0,
						   "no usable format converter for %.4s, "
						   "refusing hardware decoding",
						   (const char *)&info->pixelformat);
					return VA_STATUS_ERROR_OPERATION_FAILED;
				}
			}
		}

		type = ctx->output_format.type;
		if (ioctl(ctx->video_fd, VIDIOC_STREAMON, &type) < 0) {
			ret = -errno;
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   V4L2R_DIAG_KERNEL, "output-streamon", ret,
				   "failed to start OUTPUT streaming: %s",
				   strerror(-ret));
			/* A converter may already own buffers. Re-running setup
			 * would replace that live instance and lose its resources. */
			v4l2r_context_fail(ctx);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
		ctx->output_streaming = true;
	}

	/* Bind (and, if reused, drain the references of) the surface's CAPTURE
	 * buffer for this frame's decode. */
	ret = capture_buffer_bind(ctx, surface);
	if (ret < 0) {
		/* A failed first bind follows OUTPUT STREAMON. Repeating format
		 * setup on that partially configured instance is not valid. */
		if (starting)
			v4l2r_context_fail(ctx);
		return ret == -ENOMEM || ret == -ENOSPC ?
			VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/* The decode context provides the real storage now; drop any
	 * standalone backing from pre-decode export probing. With a
	 * conversion chain the backing IS the presented storage - keep it
	 * (v4l2r_surface_convert_backing replaces a mismatched one). In
	 * DMABUF mode the backing IS the CAPTURE buffer's memory, keep it
	 * too (the buffer holds its own fds, so this only affects the
	 * client-visible export, which must stay stable). */
	if (!ctx->conv && ctx->capture_memory != V4L2_MEMORY_DMABUF)
		v4l2r_surface_free_backing(surface);

	if (starting) {
		type = ctx->capture_format.type;
		if (ioctl(ctx->video_fd, VIDIOC_STREAMON, &type) < 0) {
			ret = -errno;
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   V4L2R_DIAG_KERNEL, "capture-streamon", ret,
				   "failed to start CAPTURE streaming: %s",
				   strerror(-ret));
			v4l2r_context_fail(ctx);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}

		ctx->streaming = true;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_INFO, V4L2R_DIAG_INFO,
			   "capture-format", 0, "using CAPTURE format %.4s (%ux%u)",
			   (const char *)&(uint32_t){v4l2r_format_pixelformat(&ctx->capture_format)},
			   v4l2r_format_width(&ctx->capture_format),
			   v4l2r_format_height(&ctx->capture_format));
	}

	return VA_STATUS_SUCCESS;
}

/* --- VA entrypoints --- */

VAStatus v4l2r_CreateContext(VADriverContextP va_ctx, VAConfigID config_id,
			     int picture_width, int picture_height, int flag,
			     VASurfaceID *render_targets, int num_render_targets,
			     VAContextID *context_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	const struct v4l2r_decoder *decoder = NULL;
	struct v4l2r_config *config;
	struct v4l2r_context *ctx;
	struct v4l2_capability capability = {0};
	unsigned int capabilities;
	uint32_t buffersize;
	VAContextID id;
	VAStatus status;
	int ret;

	(void)flag;

	config = V4L2R_CONFIG_GET(drv, config_id);
	if (!config) {
		v4l2r_diag(NULL, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "create-context", 0, "invalid config 0x%08x", config_id);
		return VA_STATUS_ERROR_INVALID_CONFIG;
	}
	if (!context_id || num_render_targets < 0 ||
	    (num_render_targets && !render_targets) ||
	    (config->codec && (picture_width <= 0 || picture_height <= 0))) {
		v4l2r_diag(NULL, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "create-context", 0,
			   "invalid context parameters (%dx%d, %d render targets)",
			   picture_width, picture_height, num_render_targets);
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
	/* Render targets are a hint, not a transfer of ownership from another
	 * context. Validate the list before allocating device resources. The
	 * first successful BeginPicture reserves an unbound surface. */
	for (int i = 0; i < num_render_targets; i++) {
		if (!V4L2R_SURFACE_GET(drv, render_targets[i])) {
			v4l2r_diag(NULL, V4L2R_DIAG_LEVEL_DEBUG,
				   V4L2R_DIAG_CLIENT, "create-context", 0,
				   "invalid render target 0x%08x",
				   render_targets[i]);
			return VA_STATUS_ERROR_INVALID_SURFACE;
		}
	}

	pthread_mutex_lock(&drv->mutex);
	id = v4l2r_handles_alloc(&drv->contexts, sizeof(*ctx));
	ctx = V4L2R_CONTEXT(drv, id);
	pthread_mutex_unlock(&drv->mutex);
	if (!ctx) {
		v4l2r_diag(NULL, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_ALLOCATION,
			   "create-context", -ENOMEM,
			   "failed to allocate a context");
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	ctx->drv = drv;
	ctx->id = id;
	ctx->diag_serial = v4l2r_diag_context_serial();
	ctx->config_id = config_id;
	ctx->profile = config->profile;
	ctx->codec = config->codec;
	ctx->picture_width = picture_width;
	ctx->picture_height = picture_height;
	ctx->bit_depth = v4l2r_profile_bit_depth(config->profile);
	ctx->video_fd = -1;
	ctx->media_fd = -1;
	pthread_mutex_init(&ctx->mutex, NULL);
	for (unsigned int i = 0; i < V4L2R_OUTPUT_BUFFERS; i++)
		ctx->output[i].request_fd = -1;
	/* A failed QUERYBUF can leave a hole before the next kernel index.
	 * Untouched slots must never appear to own descriptor zero. */
	for (unsigned int i = 0; i < V4L2R_MAX_CAPTURE_BUFFERS; i++)
		for (unsigned int p = 0; p < VIDEO_MAX_PLANES; p++)
			ctx->captures[i].dmabuf_fd[p] = -1;

	/* Video processing contexts drive the format converter instead of a
	 * decoder: no codec, no decoder device, no queues. */
	if (!ctx->codec) {
		status = v4l2r_vpp_create(ctx);
		if (status != VA_STATUS_SUCCESS)
			goto fail;

		*context_id = id;
		return VA_STATUS_SUCCESS;
	}

	if (ctx->codec->priv_size) {
		ctx->codec_priv = calloc(1, ctx->codec->priv_size);
		if (!ctx->codec_priv) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   V4L2R_DIAG_ALLOCATION, "create-context",
				   -ENOMEM, "failed to allocate codec state");
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto fail;
		}
	}

	/* Find a decoder that takes this codec and open it. Probing stops at
	 * the first device where the whole OUTPUT setup succeeds. */
	status = VA_STATUS_ERROR_OPERATION_FAILED;
	for (unsigned int i = 0; i < drv->nb_decoders; i++) {
		decoder = &drv->decoders[i];
#if VA_CHECK_VERSION(1, 18, 0)
		if (ctx->profile == VAProfileH264High10 && !decoder->h264_10bit)
			continue;
#endif

		ctx->video_fd = open(decoder->video_path, O_RDWR | O_NONBLOCK);
		if (ctx->video_fd < 0)
			continue;

		if (ioctl(ctx->video_fd, VIDIOC_QUERYCAP, &capability) < 0)
			goto next;
		ctx->is_avd = !strcmp((const char *)capability.driver, "avd");
		ctx->av1_submit_at_end = ctx->is_avd;

		capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
			       capability.device_caps : capability.capabilities;

		if (capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) {
			ctx->output_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			ctx->capture_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		} else if (capabilities & V4L2_CAP_VIDEO_M2M) {
			ctx->output_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			ctx->capture_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		} else {
			goto next;
		}

		if (!try_output_format(ctx, ctx->codec->pixelformat))
			goto next;

		struct v4l2r_dimensions dimensions;
		if (!query_dimensions(ctx->video_fd, ctx->codec->pixelformat,
					    ctx->is_avd, picture_width, picture_height,
					    &dimensions))
			goto next;

		/* Initial bitstream buffer size: compressed frames rarely
		 * exceed a quarter of the raw size, and the buffers grow on
		 * demand (v4l2r_output_buffer_grow) - pre-booking the raw
		 * frame size would pin tens of megabytes of CMA per 4K
		 * context across the 4-buffer ring. */
		buffersize = (uint64_t)ctx->picture_width * ctx->picture_height / 4;
		if (buffersize < 1024 * 1024)
			buffersize = 1024 * 1024;

		ret = set_format(ctx, ctx->output_format.type,
				 ctx->codec->pixelformat, picture_width, picture_height,
				 buffersize);
		if (ret < 0)
			goto next;

		if (ioctl(ctx->video_fd, VIDIOC_G_FMT, &ctx->output_format) < 0)
			goto next;

		/* Query OUTPUT capabilities only now that the coded format is
		 * set: whether the queue supports requests, and crucially whether
		 * it supports holding the CAPTURE buffer across the slices of a
		 * frame (V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF), which cedrus
		 * only reports once the OUTPUT format is a slice format. Querying
		 * before S_FMT misses it and breaks multi-slice frames (each slice
		 * would re-queue the same CAPTURE buffer). */
		ret = query_buffer_capabilities(ctx, ctx->output_format.type,
						&ctx->output_capabilities);
		if (ret < 0 ||
		    !(ctx->output_capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS))
			goto next;

		ctx->media_fd = open(decoder->media_path, O_RDWR);
		if (ctx->media_fd < 0)
			goto next;

		/*
		 * Only accept this decoder if the codec can actually be driven on
		 * it. Some devices match the pixelformat but need controls we
		 * cannot supply (e.g. rkvdec2 HEVC, which requires the SPS RPS
		 * tables); their init rejects the device, so fall through and try
		 * the next candidate. On SoCs exposing several nodes for one codec
		 * (e.g. RK3399: rkvdec + hantro-G2 for HEVC) this lands on the one
		 * we can drive.
		 */
		if (ctx->codec->init) {
			status = ctx->codec->init(ctx);
			if (status != VA_STATUS_SUCCESS) {
				if (ctx->codec->uninit)
					ctx->codec->uninit(ctx);
				if (ctx->codec_priv)
					memset(ctx->codec_priv, 0,
					       ctx->codec->priv_size);
				goto next;
			}
		}

		break;

next:
		if (ctx->media_fd >= 0) {
			close(ctx->media_fd);
			ctx->media_fd = -1;
		}
		close(ctx->video_fd);
		ctx->video_fd = -1;
		decoder = NULL;
	}

	if (ctx->video_fd < 0 || !decoder) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_UNSUPPORTED,
			   "create-context", 0,
			   "no drivable decoder for %.4s at %dx%d",
			   (const char *)&ctx->codec->pixelformat,
			   picture_width, picture_height);
		goto fail;
	}

	v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_INFO, V4L2R_DIAG_INFO, "create-context", 0,
		   "decoding %s via %s [%s] (media %s)",
		   ctx->codec->name, decoder->video_path, decoder->card,
		   decoder->media_path);
	bool hold_capture = false;
#ifdef V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF
	hold_capture = ctx->output_capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
#endif
	v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_INFO,
		   "context-capabilities", 0,
		   "decoder %s: %ux%u, hold-capture %s, avd %s",
		   decoder->card, picture_width, picture_height,
		   hold_capture ? "yes" : "no",
		   ctx->is_avd ? "yes" : "no");

	for (unsigned int i = 0; i < V4L2R_OUTPUT_BUFFERS; i++) {
		ret = output_buffer_setup(ctx, &ctx->output[i], 0);
		if (ret < 0) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto fail;
		}
	}

	*context_id = id;
	return VA_STATUS_SUCCESS;

fail:
	v4l2r_DestroyContext(va_ctx, id);
	return status;
}

/* Transfer MMAP storage to the still-live surface before freeing the context.
 * Already-exported DMABUF surfaces have their own backing and need no transfer.
 */
static void preserve_capture(struct v4l2r_context *ctx, unsigned int index)
{
	struct v4l2r_capture_buffer *capture = &ctx->captures[index];
	struct v4l2r_surface *surface = capture->surface;
	struct v4l2r_surface_backing *backing;

	if (!surface || surface->backing)
		return;
	backing = calloc(1, sizeof(*backing));
	if (!backing || v4l2r_export_capture_dmabufs(ctx, capture, index) < 0) {
		free(backing);
		surface->decode_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		return;
	}
	for (unsigned int i = 0; i < VIDEO_MAX_PLANES; i++)
		backing->dmabuf_fd[i] = -1;
	backing->pixelformat = v4l2r_format_pixelformat(&ctx->capture_format);
	backing->width = v4l2r_format_width(&ctx->capture_format);
	backing->height = v4l2r_format_height(&ctx->capture_format);
	backing->pitch = v4l2r_format_bytesperline(&ctx->capture_format);
	backing->nb_planes = capture->nb_planes;
	for (unsigned int i = 0; i < capture->nb_planes; i++) {
		backing->plane_size[i] = capture->plane_size[i];
		backing->dmabuf_fd[i] = capture->dmabuf_fd[i];
		backing->map[i] = capture->map[i];
		capture->dmabuf_fd[i] = -1;
		capture->map[i] = NULL;
	}
	surface->backing = backing;
}

VAStatus v4l2r_DestroyContext(VADriverContextP va_ctx, VAContextID context_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	enum v4l2_buf_type type;
	unsigned int iter = 0;
	struct v4l2r_surface *surface;
	struct v4l2r_surface *abandoned;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	/* A missing EndPicture cannot leave a successful frame behind or
	 * submit an incomplete held-back picture during teardown. */
	abandoned = ctx->in_picture ? ctx->pic.target : NULL;
	if (abandoned) {
		abandoned->decode_status = ctx->picture_status != VA_STATUS_SUCCESS ?
			ctx->picture_status : VA_STATUS_ERROR_OPERATION_FAILED;
		abandoned->status = VASurfaceReady;
	}

	/* Finish the last frames before STREAMOFF cancels the queue. The VA
	 * surfaces may still be downloaded after this context goes away. Keep
	 * failures on the surface instead of turning an aborted decode into a
	 * successful read of stale pixels. */
	if (ctx->streaming && !ctx->failed) {
		for (unsigned int i = 0; i < ctx->nb_captures; i++) {
			surface = ctx->captures[i].surface;
			if (surface && surface != abandoned) {
				VAStatus status = v4l2r_flush_surface(surface);
				if (status != VA_STATUS_SUCCESS)
					surface->decode_status = status;
			}
		}
		v4l2r_wait_completed(ctx, ctx->submitted);
		for (unsigned int i = 0; i < ctx->nb_captures; i++) {
			surface = ctx->captures[i].surface;
			if (!surface)
				continue;
			if (ctx->queued_capture & (UINT64_C(1) << i))
				surface->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
			else if (surface->decode_status == VA_STATUS_SUCCESS)
				surface->decode_status = v4l2r_convert_wait(surface);
			surface->status = VASurfaceReady;
		}
	}

	if (ctx->video_fd >= 0 && (ctx->streaming || ctx->output_streaming)) {
		type = ctx->output_format.type;
		ioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type);
		if (ctx->streaming) {
			type = ctx->capture_format.type;
			ioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type);
		}
	}

	v4l2r_convert_destroy(ctx);
	v4l2r_vpp_destroy(ctx);

	for (unsigned int i = 0; i < V4L2R_OUTPUT_BUFFERS; i++)
		output_buffer_cleanup(ctx, &ctx->output[i]);

	for (unsigned int i = 0; i < ctx->nb_captures; i++) {
		preserve_capture(ctx, i);
		capture_buffer_cleanup(ctx, &ctx->captures[i]);
	}

	/* Detach surfaces that were attached but never bound. */
	pthread_mutex_lock(&drv->mutex);
	while ((surface = v4l2r_handles_next(&drv->surfaces, &iter, NULL))) {
		if (surface->ctx == ctx) {
			surface->ctx = NULL;
			surface->capture_index = -1;
		}
	}
	pthread_mutex_unlock(&drv->mutex);

	if (ctx->video_fd >= 0)
		close(ctx->video_fd);
	if (ctx->media_fd >= 0)
		close(ctx->media_fd);

	if (ctx->codec && ctx->codec->uninit && ctx->codec_priv)
		ctx->codec->uninit(ctx);
	free(ctx->codec_priv);
	pthread_mutex_destroy(&ctx->mutex);

	pthread_mutex_lock(&drv->mutex);
	/* Client-owned buffers remain mappable/destroyable, but can never be
	 * submitted through a later context which reuses this numeric ID. */
	struct v4l2r_buffer *buffer;
	iter = 0;
	while ((buffer = v4l2r_handles_next(&drv->buffers, &iter, NULL))) {
		if (buffer->context_id == context_id)
			buffer->context_id = VA_INVALID_ID;
	}
	v4l2r_handles_free(&drv->contexts, context_id);
	pthread_mutex_unlock(&drv->mutex);

	return VA_STATUS_SUCCESS;
}

/* --- picture level entrypoints --- */

VAStatus v4l2r_BeginPicture(VADriverContextP va_ctx, VAContextID context_id,
			    VASurfaceID render_target)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	struct v4l2r_surface *surface;
	VAStatus status;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	surface = V4L2R_SURFACE_GET(drv, render_target);

	if (!ctx) {
		v4l2r_diag(NULL, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "begin-picture", 0, "invalid context 0x%08x",
			   context_id);
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	}
	if (ctx->in_picture) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_CLIENT,
			   "begin-picture", 0,
			   "BeginPicture while a picture is already open");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	if (ctx->failed)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	if (!surface) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "begin-picture", 0, "invalid render target 0x%08x",
			   render_target);
		return VA_STATUS_ERROR_INVALID_SURFACE;
	}
	if (surface->ctx && surface->ctx != ctx) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "begin-picture", 0,
			   "render target 0x%08x belongs to another context",
			   render_target);
		return VA_STATUS_ERROR_SURFACE_BUSY;
	}

	ctx->picture_status = VA_STATUS_SUCCESS;
	if (ctx->vpp) {
		status = v4l2r_vpp_begin_picture(ctx, surface);
	} else {
		status = v4l2r_picture_begin(ctx, surface);
		if (status == VA_STATUS_SUCCESS) {
			ctx->in_picture = true;
			if (ctx->codec->begin_picture)
				status = ctx->codec->begin_picture(ctx);
		}
	}
	if (status != VA_STATUS_SUCCESS) {
		ctx->in_picture = false;
		ctx->pic = (struct v4l2r_picture){0};
		return status;
	}
	surface->ctx = ctx;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_RenderPicture(VADriverContextP va_ctx, VAContextID context_id,
			     VABufferID *buffers, int num_buffers)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	VAStatus status;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!ctx->in_picture) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_CLIENT,
			   "render-picture", 0, "RenderPicture without BeginPicture");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	if (ctx->picture_status != VA_STATUS_SUCCESS)
		return ctx->picture_status;
	if (num_buffers < 0 || (num_buffers && !buffers)) {
		ctx->picture_status = VA_STATUS_ERROR_INVALID_PARAMETER;
		return ctx->picture_status;
	}

	for (int i = 0; i < num_buffers; i++) {
		struct v4l2r_buffer *buffer;

		buffer = V4L2R_BUFFER_GET(drv, buffers[i]);
		if (!buffer || buffer->context_id != context_id) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
				   "render-picture", 0, "invalid or foreign buffer 0x%08x",
				   buffers[i]);
			ctx->picture_status = VA_STATUS_ERROR_INVALID_BUFFER;
			return ctx->picture_status;
		}

		status = ctx->vpp ? v4l2r_vpp_render_buffer(ctx, buffer) :
			 ctx->codec->render_buffer(ctx, buffer);
		if (status != VA_STATUS_SUCCESS) {
			ctx->picture_status = status;
			return status;
		}
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_EndPicture(VADriverContextP va_ctx, VAContextID context_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	VAStatus status;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!ctx->in_picture) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_CLIENT,
			   "end-picture", 0, "EndPicture without BeginPicture");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	status = ctx->picture_status;
	if (status == VA_STATUS_SUCCESS)
		status = ctx->vpp ? v4l2r_vpp_end_picture(ctx) :
			 ctx->codec->end_picture(ctx);
	if (status != VA_STATUS_SUCCESS && ctx->pic.target) {
		ctx->pic.target->decode_status = status;
		/* Render may reject the next slice without calling the queue
		 * engine. Earlier slices can still own a held CAPTURE buffer. */
		int index = ctx->pic.target->capture_index;
		pthread_mutex_lock(&ctx->mutex);
		if (index >= 0 && (ctx->queued_capture & (UINT64_C(1) << index)))
			v4l2r_context_fail(ctx);
		pthread_mutex_unlock(&ctx->mutex);
	}

	ctx->in_picture = false;
	ctx->pic = (struct v4l2r_picture){0};

	return status;
}
