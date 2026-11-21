/*
 * Decode engine: request submission, buffer queueing and synchronization.
 *
 * Ported from the FFmpeg v4l2-request hwaccel: a circular queue of
 * mmap()ed OUTPUT buffers each paired with a media request, per-frame
 * CAPTURE buffers referenced through timestamps, and slice batching via
 * V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "v4l2_request.h"

/* Room kept at the tail of the OUTPUT buffer, zeroed before submission. */
#define V4L2R_BITSTREAM_PADDING	64

uint64_t v4l2r_surface_timestamp(struct v4l2r_context *ctx, VASurfaceID id)
{
	struct v4l2r_surface *surface = V4L2R_SURFACE_GET(ctx->drv, id);

	/* Buffer indices and their timestamps are local to a decoder context.
	 * Never alias another context's buffer, or revive detached/failed data. */
	if (!surface || surface->ctx != ctx || surface->capture_index < 0 ||
	    surface->capture_index >= V4L2R_MAX_CAPTURE_BUFFERS ||
	    (unsigned int)surface->capture_index >= ctx->nb_captures ||
	    ctx->captures[surface->capture_index].surface != surface ||
	    surface->decode_status != VA_STATUS_SUCCESS)
		return 0;

	/* Remember that the frame currently being assembled references this
	 * CAPTURE buffer, so its reuse can be gated on that frame completing. */
	if (ctx->in_picture)
		ctx->pic.ref_mask |=
			UINT64_C(1) << surface->capture_index;

	return v4l2r_capture_index_timestamp(surface->capture_index);
}

int v4l2r_set_controls(struct v4l2r_context *ctx, int request_fd,
		       struct v4l2_ext_control *controls, unsigned int count)
{
	struct v4l2_ext_controls ext_controls = {
		.controls = controls,
		.count = count,
		.request_fd = request_fd,
		.which = (request_fd >= 0) ? V4L2_CTRL_WHICH_REQUEST_VAL : 0,
	};

	if (!controls || !count)
		return 0;

	if (ioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext_controls) < 0)
		return -errno;

	return 0;
}

int v4l2r_query_control(struct v4l2r_context *ctx,
			struct v4l2_query_ext_ctrl *control)
{
	if (ioctl(ctx->video_fd, VIDIOC_QUERY_EXT_CTRL, control) < 0)
		return -errno;

	return 0;
}

int v4l2r_query_control_default(struct v4l2r_context *ctx, uint32_t id,
				int64_t *value)
{
	struct v4l2_query_ext_ctrl control = {
		.id = id,
	};
	int ret;

	ret = v4l2r_query_control(ctx, &control);
	if (ret < 0)
		return ret;

	*value = control.default_value;
	return 0;
}

int v4l2r_poll_until(int fd, short events, uint64_t deadline)
{
	struct pollfd pollfd = {
		.fd = fd,
		.events = events,
	};
	int ret;
	do {
		uint64_t now = v4l2r_now_ns();
		if (now >= deadline)
			return -ETIMEDOUT;
		int remaining = (int)((deadline - now + 999999) / 1000000);
		ret = poll(&pollfd, 1, remaining);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		return -errno;
	if (ret == 0)
		return -ETIMEDOUT;
	if (pollfd.revents & POLLNVAL)
		return -ENODEV;
	if (pollfd.revents & (POLLERR | POLLHUP))
		return -EPIPE;
	if (!(pollfd.revents & events))
		return -EIO;

	return 0;
}

int v4l2r_poll_one(int fd, short events, int timeout_ms)
{
	return v4l2r_poll_until(fd, events, v4l2r_now_ns() +
		(uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000);
}

/* The caller holds ctx->mutex (or exclusive teardown ownership). A failed
 * queue cannot be repaired by pretending its buffers completed. Retain the
 * first surface error, stop new submissions and leave cancellation to close. */
void v4l2r_context_fail(struct v4l2r_context *ctx)
{
	ctx->failed = true;
	for (unsigned int i = 0; i < ctx->nb_captures; i++) {
		struct v4l2r_surface *s = ctx->captures[i].surface;
		if (s && ((ctx->queued_capture & (UINT64_C(1) << i)) ||
			  s->convert_pending) && s->decode_status == VA_STATUS_SUCCESS)
			s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
	}
	if (ctx->pic.target && ctx->pic.target->decode_status == VA_STATUS_SUCCESS)
		ctx->pic.target->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
}

/* --- queue/dequeue primitives, ctx->mutex must be held --- */

/* VA id of the surface bound to a CAPTURE buffer, or VA_INVALID_ID. */
static uint32_t capture_surface_id(struct v4l2r_context *ctx, uint32_t index)
{
	if (index < ctx->nb_captures && ctx->captures[index].surface)
		return ctx->captures[index].surface->id;

	return VA_INVALID_ID;
}

static int queue_buffer(struct v4l2r_context *ctx, struct v4l2_buffer *buffer)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
	const struct v4l2r_capture_buffer *capture = NULL;

	/* A DMABUF-mode CAPTURE buffer names its memory on every queue. */
	if (!V4L2_TYPE_IS_OUTPUT(buffer->type) &&
	    buffer->memory == V4L2_MEMORY_DMABUF)
		capture = &ctx->captures[buffer->index];

	if (V4L2_TYPE_IS_MULTIPLANAR(buffer->type)) {
		planes[0].bytesused = buffer->bytesused;
		buffer->bytesused = 0;
		buffer->length = 1;
		buffer->m.planes = planes;
		if (capture) {
			buffer->length = capture->nb_planes;
			for (unsigned int i = 0; i < capture->nb_planes; i++) {
				planes[i].m.fd = capture->dmabuf_fd[i];
				planes[i].length = capture->plane_size[i];
			}
		}
	} else if (capture) {
		buffer->m.fd = capture->dmabuf_fd[0];
		buffer->length = capture->plane_size[0];
	}

	if (ioctl(ctx->video_fd, VIDIOC_QBUF, buffer) < 0)
		return -errno;

	if (V4L2_TYPE_IS_OUTPUT(buffer->type)) {
		ctx->queued_output |= 1u << buffer->index;
		v4l2r_trace("QBUF OUTPUT  #%u -> CAPTURE #%d (request %d)\n",
			  buffer->index, (int)buffer->timestamp.tv_usec - 1,
			  buffer->request_fd);
	} else {
		ctx->queued_capture |= UINT64_C(1) << buffer->index;
		/* Count decode submissions on the CAPTURE queue: one per frame,
		 * and completed only when the decode (and thus its reference
		 * reads) actually finishes, unlike the OUTPUT bitstream buffer
		 * which is released as soon as it is consumed. */
		ctx->submitted++;
		v4l2r_trace("QBUF CAPTURE #%u (surface 0x%08x), submit #%llu, "
			  "%u in flight\n", buffer->index,
			  capture_surface_id(ctx, buffer->index),
			  (unsigned long long)ctx->submitted,
			  __builtin_popcountll(ctx->queued_capture));
	}

	return 0;
}

static int queue_capture_buffer(struct v4l2r_context *ctx, uint32_t index)
{
	struct v4l2_buffer buffer = {
		.index = index,
		.type = ctx->capture_format.type,
		.memory = ctx->capture_memory,
	};

	return queue_buffer(ctx, &buffer);
}

static int queue_output_buffer(struct v4l2r_context *ctx,
			       struct v4l2r_output_buffer *output,
			       uint32_t flags)
{
	struct v4l2_buffer buffer = {
		.index = output->index,
		.type = ctx->output_format.type,
		.memory = V4L2_MEMORY_MMAP,
		.timestamp = output->timestamp,
		.bytesused = output->bytesused,
		.request_fd = output->request_fd,
		.flags = V4L2_BUF_FLAG_REQUEST_FD | flags,
	};

	return queue_buffer(ctx, &buffer);
}

static int dequeue_buffer(struct v4l2r_context *ctx, enum v4l2_buf_type type)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
	struct v4l2_buffer buffer = {
		.type = type,
		.memory = V4L2_TYPE_IS_OUTPUT(type) ? V4L2_MEMORY_MMAP :
						      ctx->capture_memory,
	};

	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		buffer.length = VIDEO_MAX_PLANES;
		buffer.m.planes = planes;
	}

	if (ioctl(ctx->video_fd, VIDIOC_DQBUF, &buffer) < 0)
		return -errno;

	if (V4L2_TYPE_IS_OUTPUT(type)) {
		/* Grown OUTPUT buffers get new kernel indices beyond the ring's
		 * four slots; the bitmasks support indices 0..31. */
		if (buffer.index >= 32 || !(ctx->queued_output & (1u << buffer.index)))
			return -ERANGE;
		ctx->queued_output &= ~(1u << buffer.index);
		v4l2r_trace("DQBUF OUTPUT  #%u (bitstream consumed)\n",
			  buffer.index);
	} else {
		if (buffer.index >= ctx->nb_captures ||
		    buffer.index >= V4L2R_MAX_CAPTURE_BUFFERS ||
		    !(ctx->queued_capture & (UINT64_C(1) << buffer.index)))
			return -ERANGE;
		ctx->queued_capture &= ~(UINT64_C(1) << buffer.index);
		ctx->completed++;
		v4l2r_trace("DQBUF CAPTURE #%u (surface 0x%08x) decode done, "
			  "completed #%llu, %u still in flight\n", buffer.index,
			  capture_surface_id(ctx, buffer.index),
			  (unsigned long long)ctx->completed,
			  __builtin_popcountll(ctx->queued_capture));
		if (buffer.index < ctx->nb_captures &&
		    ctx->captures[buffer.index].surface) {
			struct v4l2r_surface *surface = ctx->captures[buffer.index].surface;
			/* EndPicture may already have marked an incomplete submission
			 * as failed. Completion of an earlier slice cannot clear it.
			 * A new first-slice submission resets the status on reuse. */
			if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
				surface->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
				v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING,
					   V4L2R_DIAG_DECODER, "capture-dequeue", 0,
					   "decoder reported an error for surface "
					   "0x%08x (CAPTURE buffer %u)", surface->id,
					   buffer.index);
			}
			ctx->captures[buffer.index].surface->status =
				VASurfaceReady;
			/* Start the format conversion right away so it
			 * overlaps subsequent decodes. */
			if (ctx->conv && surface->decode_status == VA_STATUS_SUCCESS)
				v4l2r_convert_kick(ctx, buffer.index);
		}
	}

	return 0;
}

static int dequeue_completed_buffers(struct v4l2r_context *ctx,
				      enum v4l2_buf_type type)
{
	uint64_t deadline = v4l2r_now_ns() + (uint64_t)V4L2R_POLL_TIMEOUT_MS * 1000000;
	for (;;) {
		int ret = dequeue_buffer(ctx, type);
		if (ret == -EAGAIN)
			return 0;
		if (ret < 0 && ret != -EINTR)
			return ret;
		if (v4l2r_now_ns() >= deadline)
			return -ETIMEDOUT;
	}
}

static int wait_on_capture_locked(struct v4l2r_context *ctx, uint32_t index)
{
	uint64_t deadline = v4l2r_now_ns() + (uint64_t)V4L2R_POLL_TIMEOUT_MS * 1000000;
	if (ctx->queued_capture) {
		int ret = dequeue_completed_buffers(ctx, ctx->capture_format.type);
		if (ret < 0)
			return ret;
	}

	while (ctx->queued_capture & (UINT64_C(1) << index)) {
		int ret = v4l2r_poll_until(ctx->video_fd, POLLIN, deadline);
		if (ret < 0)
			return ret;

		ret = dequeue_buffer(ctx, ctx->capture_format.type);
		if (ret < 0 && ret != -EAGAIN && ret != -EINTR)
			return ret;
	}

	return 0;
}

VAStatus v4l2r_sync_capture(struct v4l2r_context *ctx, int capture_index)
{
	int ret;

	if (capture_index < 0 || !ctx->streaming)
		return VA_STATUS_SUCCESS;

	pthread_mutex_lock(&ctx->mutex);
	ret = ctx->failed ?
		(ctx->queued_capture & (UINT64_C(1) << capture_index) ? -EIO : 0) :
		wait_on_capture_locked(ctx, capture_index);
	if (ret < 0) {
		v4l2r_context_fail(ctx);
		/* The drain may have completed this target before a later
		 * dequeue failed. Retain the error returned by this sync even
		 * after its queued bit has cleared; unrelated completed frames
		 * remain readable. */
		struct v4l2r_surface *surface = ctx->captures[capture_index].surface;
		if (surface && surface->decode_status == VA_STATUS_SUCCESS)
			surface->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (ret < 0) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
			   v4l2r_diag_errno_category(ret), "capture-wait", ret,
			   "failed waiting on CAPTURE buffer %d", capture_index);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return VA_STATUS_SUCCESS;
}

void v4l2r_reap_capture(struct v4l2r_context *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	if (!ctx->failed && ctx->queued_capture &&
	    dequeue_completed_buffers(ctx, ctx->capture_format.type) < 0)
		v4l2r_context_fail(ctx);
	pthread_mutex_unlock(&ctx->mutex);
}

static int wait_completed_locked(struct v4l2r_context *ctx, uint64_t target)
{
	uint64_t deadline = v4l2r_now_ns() + (uint64_t)V4L2R_POLL_TIMEOUT_MS * 1000000;
	if (ctx->queued_capture) {
		int ret = dequeue_completed_buffers(ctx, ctx->capture_format.type);
		if (ret < 0)
			return ret;
	}

	/* Dequeue CAPTURE buffers in order until enough frames have completed.
	 * Completion is in submission order, so this drains exactly the frames
	 * submitted up to the target sequence. */
	while (ctx->completed < target && ctx->queued_capture) {
		int ret = v4l2r_poll_until(ctx->video_fd, POLLIN, deadline);
		if (ret < 0)
			return ret;

		ret = dequeue_buffer(ctx, ctx->capture_format.type);
		if (ret < 0 && ret != -EAGAIN && ret != -EINTR)
			return ret;
	}

	return ctx->completed >= target ? 0 : -ENODATA;
}

VAStatus v4l2r_wait_completed(struct v4l2r_context *ctx, uint64_t target)
{
	int ret;

	if (!ctx->streaming || ctx->completed >= target)
		return VA_STATUS_SUCCESS;

	pthread_mutex_lock(&ctx->mutex);
	ret = ctx->failed ? -EIO : wait_completed_locked(ctx, target);
	if (ret < 0)
		v4l2r_context_fail(ctx);
	pthread_mutex_unlock(&ctx->mutex);

	if (ret < 0) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
			   v4l2r_diag_errno_category(ret), "completion-wait", ret,
			   "failed waiting for %llu CAPTURE completions (at %llu)",
			   (unsigned long long)target,
			   (unsigned long long)ctx->completed);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return VA_STATUS_SUCCESS;
}

/*
 * Reserve the next OUTPUT buffer from the circular queue, waiting for its
 * previous use to be dequeued first.
 */
static struct v4l2r_output_buffer *next_output(struct v4l2r_context *ctx)
{
	struct v4l2r_output_buffer *output;
	uint8_t index;
	int ret = 0;
	uint64_t deadline = v4l2r_now_ns() + (uint64_t)V4L2R_POLL_TIMEOUT_MS * 1000000;

	if (ctx->failed)
		return NULL;

	pthread_mutex_lock(&ctx->mutex);

	index = ctx->next_output;
	output = &ctx->output[index];
	ctx->next_output = (index + 1) % V4L2R_OUTPUT_BUFFERS;

	if (ctx->queued_output) {
		ret = dequeue_completed_buffers(ctx, ctx->output_format.type);
		if (ret < 0)
			goto fail;
	}

	while (ctx->queued_output & (1u << output->index)) {
		ret = v4l2r_poll_until(ctx->video_fd, POLLOUT, deadline);
		if (ret < 0)
			goto fail;

		ret = dequeue_buffer(ctx, ctx->output_format.type);
		if (ret < 0 && ret != -EAGAIN && ret != -EINTR)
			goto fail;
	}

	pthread_mutex_unlock(&ctx->mutex);

	output->bytesused = 0;
	return output;

fail:
	v4l2r_context_fail(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, v4l2r_diag_errno_category(ret),
		   "output-wait", ret, "failed waiting on OUTPUT buffer %u",
		   output->index);
	return NULL;
}

/*
 * The OUTPUT buffer of the prior use of a request may be dequeued before
 * the request itself has completed (multi stage decoders); wait for the
 * request too before reusing its file descriptor.
 */
static int wait_on_request(struct v4l2r_context *ctx,
			   struct v4l2r_output_buffer *output)
{
	if (ctx->queued_request & (1u << output->index)) {
		int ret = v4l2r_poll_one(output->request_fd, POLLPRI,
					 V4L2R_POLL_TIMEOUT_MS);
		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   v4l2r_diag_errno_category(ret),
				   "request-wait", ret,
				   "failed waiting on request %d",
				   output->request_fd);
			return ret;
		}
		ctx->queued_request &= ~(1u << output->index);
	}

	if (ioctl(output->request_fd, MEDIA_REQUEST_IOC_REINIT) < 0) {
		int ret = -errno;

		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
			   "request-reinit", ret, "failed to reinit request %d: %s",
			   output->request_fd, strerror(-ret));
		return ret;
	}

	ctx->queued_request &= ~(1u << output->index);

	return 0;
}

VAStatus v4l2r_picture_begin(struct v4l2r_context *ctx,
			     struct v4l2r_surface *target)
{
	struct v4l2r_output_buffer *output;

	output = next_output(ctx);
	if (!output)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* The target's CAPTURE buffer is bound (and, if reused, drained of its
	 * references) at decode time, so there is nothing to sync here; only
	 * reserve the OUTPUT buffer and reset the per-frame reference set. */
	ctx->pic.output = output;
	ctx->pic.target = target;
	ctx->pic.ref_mask = 0;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_picture_next_output(struct v4l2r_context *ctx)
{
	struct v4l2r_output_buffer *output = next_output(ctx);

	if (!output)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	ctx->pic.output = output;
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_append_output(struct v4l2r_context *ctx, const void *data,
			     size_t size)
{
	struct v4l2r_output_buffer *output = ctx->pic.output;
	size_t needed;

	if (!output)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (size > UINT32_MAX - V4L2R_BITSTREAM_PADDING ||
	    output->bytesused > UINT32_MAX - V4L2R_BITSTREAM_PADDING - size) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING, V4L2R_DIAG_BITSTREAM,
			   "bitstream-append", -EOVERFLOW,
			   "bitstream data (%zu bytes) exceeds the OUTPUT size "
			   "limit (%u used)", size, output->bytesused);
		return VA_STATUS_ERROR_INVALID_BUFFER;
	}
	needed = (size_t)output->bytesused + size + V4L2R_BITSTREAM_PADDING;
	if (needed > output->size) {
		int ret = v4l2r_output_buffer_grow(ctx, output, needed);

		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   V4L2R_DIAG_ALLOCATION, "output-grow", ret,
				   "bitstream data (%zu bytes) overflows OUTPUT buffer %u (%u of %u used)",
				   size, output->index, output->bytesused,
				   output->size);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
	}

	memcpy(output->addr + output->bytesused, data, size);
	output->bytesused += size;

	return VA_STATUS_SUCCESS;
}

static VAStatus queue_decode(struct v4l2r_context *ctx,
			     struct v4l2_ext_control *controls,
			     unsigned int count,
			     bool first_slice, bool last_slice)
{
	struct v4l2r_output_buffer *output = ctx->pic.output;
	struct v4l2r_surface *target = ctx->pic.target;
	uint32_t flags;
	bool attached = false;
	int ret;

	if (!output || !target || target->capture_index < 0) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_CLIENT,
			   "decode", 0, "no picture or bound target to submit");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->failed)
		goto fail;

	ret = wait_on_request(ctx, output);
	if (ret < 0) {
		v4l2r_context_fail(ctx);
		goto fail;
	}

	if (ctx->queued_output &&
	    dequeue_completed_buffers(ctx, ctx->output_format.type) < 0) {
		v4l2r_context_fail(ctx);
		goto fail;
	}

	ret = v4l2r_set_controls(ctx, output->request_fd, controls, count);
	if (ret < 0) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
			   "set-controls", ret,
			   "failed to set %u control(s) for request %d: %s",
			   count, output->request_fd, strerror(-ret));
		goto fail;
	}

	/* Zero padding after the bitstream data, some decoders require it. */
	memset(output->addr + output->bytesused, 0, V4L2R_BITSTREAM_PADDING);

	/* The CAPTURE buffer index is the base for V4L2 frame references. */
	output->timestamp = (struct timeval) {
		.tv_sec = 0,
		.tv_usec = target->capture_index + 1,
	};

#if HAVE_V4L2_M2M_HOLD_CAPTURE_BUF
	flags = last_slice ? 0 : V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
#else
	/* The flag first appeared in Linux 5.9, before the first stateless
	 * codec pixelformat (5.11): without it no codec is compiled in and
	 * decoding cannot be reached. */
	flags = 0;
	(void)last_slice;
#endif
	ret = queue_output_buffer(ctx, output, flags);
	if (ret < 0) {
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
			   v4l2r_diag_errno_category(ret), "output-queue", ret,
			   "failed to queue OUTPUT buffer %u: %s",
			   output->index, strerror(-ret));
		goto fail;
	}
	attached = true;

	if (first_slice) {
		/* Queue the specific CAPTURE buffer tied to the target
		 * surface so frames land where VA expects them. */
		ret = queue_capture_buffer(ctx, target->capture_index);
		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   v4l2r_diag_errno_category(ret),
				   "capture-queue", ret,
				   "failed to queue CAPTURE buffer %d: %s",
				   target->capture_index, strerror(-ret));
			goto fail;
		}
		target->decode_status = VA_STATUS_SUCCESS;
		ctx->captures[target->capture_index].decode_seq = ctx->submitted;

		/* ctx->submitted is now this frame's sequence number. Mark every
		 * buffer it references as needed until this frame completes, so a
		 * later decode cannot overwrite a reference still in use. */
		for (unsigned int i = 0; i < ctx->nb_captures; i++) {
			if (ctx->pic.ref_mask & (UINT64_C(1) << i))
				ctx->captures[i].last_ref_seq = ctx->submitted;
		}
	}

	if (ioctl(output->request_fd, MEDIA_REQUEST_IOC_QUEUE) < 0) {
		ret = -errno;
		v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR, V4L2R_DIAG_KERNEL,
			   "request-queue", ret, "failed to queue request %d: %s",
			   output->request_fd, strerror(-ret));
		goto fail;
	}

	ctx->queued_request |= 1u << output->index;
	target->status = VASurfaceRendering;

	pthread_mutex_unlock(&ctx->mutex);
	return VA_STATUS_SUCCESS;

fail:
	/* An OUTPUT attached to an idle request cannot dequeue. Release it
	 * now, before next_output() would wait on the stranded ring slot. */
	if (attached) {
		if (ioctl(output->request_fd, MEDIA_REQUEST_IOC_REINIT) < 0)
			v4l2r_context_fail(ctx);
		else
			ctx->queued_output &= ~(1u << output->index);
	}
	/* CAPTURE has no request binding to undo. It may also contain an
	 * earlier slice. New work must not consume that failed destination. */
	if (ctx->queued_capture & (UINT64_C(1) << target->capture_index))
		v4l2r_context_fail(ctx);
	if (target->decode_status == VA_STATUS_SUCCESS)
		target->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
	pthread_mutex_unlock(&ctx->mutex);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

VAStatus v4l2r_decode(struct v4l2r_context *ctx,
		      struct v4l2_ext_control *controls, unsigned int count,
		      bool first_slice, bool last_slice)
{
	VAStatus status;

	if (ctx->failed)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* First submission of the context configures the CAPTURE side using
	 * the codec controls that are now known (e.g. SPS bit depth). Set
	 * them without a request first so the driver can base its CAPTURE
	 * format decision on them. */
	if (!ctx->streaming) {
		int ret = v4l2r_set_controls(ctx, -1, controls, count);
		if (ret < 0) {
			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_WARNING,
				   V4L2R_DIAG_KERNEL, "set-initial-controls", ret,
				   "failed to set initial controls: %s",
				   strerror(-ret));
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
	}

	/* Bind a fresh CAPTURE buffer to the target once per frame (on its
	 * first slice); this also performs the one-time CAPTURE queue setup
	 * on the very first decode and syncs the picked buffer. */
	if (first_slice) {
		status = v4l2r_context_bind_surface(ctx, ctx->pic.target);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	/* Without HOLD_CAPTURE_BUF support every slice must be submitted as
	 * a full frame. */
#if HAVE_V4L2_M2M_HOLD_CAPTURE_BUF
	if (!(ctx->output_capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF))
		return queue_decode(ctx, controls, count, true, true);
#endif

	return queue_decode(ctx, controls, count, first_slice, last_slice);
}
