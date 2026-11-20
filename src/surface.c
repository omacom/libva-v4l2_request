/*
 * Surfaces: creation, synchronization and zero-copy dma-buf export.
 *
 * A surface is a thin handle; the backing CAPTURE buffer is allocated by
 * the context the surface is first decoded with. Export hands out the
 * V4L2 buffer planes as DRM PRIME file descriptors without any copy.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <drm_fourcc.h>
#include <va/va_drmcommon.h>

#include "v4l2_request.h"

VAStatus v4l2r_CreateSurfaces2(VADriverContextP va_ctx, unsigned int format,
			       unsigned int width, unsigned int height,
			       VASurfaceID *surfaces, unsigned int num_surfaces,
			       VASurfaceAttrib *attrib_list,
			       unsigned int num_attribs)
{
	V4L2R_OVERLAP_UNLOCKED();
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	uint32_t fourcc = 0;
	unsigned int i;

	if (!width || !height || width > 65536 || height > 65536 ||
	    (num_surfaces && !surfaces) || (num_attribs && !attrib_list))
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	for (i = 0; i < num_attribs; i++) {
		if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
		    (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE)) {
			if (attrib_list[i].value.type != VAGenericValueTypeInteger)
				return VA_STATUS_ERROR_INVALID_PARAMETER;
			fourcc = attrib_list[i].value.value.i;
			if (fourcc != VA_FOURCC_NV12 && fourcc != VA_FOURCC_P010)
				return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
		}
	}

	for (i = 0; i < num_surfaces; i++) {
		struct v4l2r_surface *surface;
		VASurfaceID id;

		pthread_mutex_lock(&drv->mutex);
		id = v4l2r_handles_alloc(&drv->surfaces, sizeof(*surface));
		surface = V4L2R_SURFACE(drv, id);
		pthread_mutex_unlock(&drv->mutex);
		if (!surface)
			goto fail;

		surface->id = id;
		surface->width = width;
		surface->height = height;
		surface->rt_format = format;
		surface->fourcc = fourcc;
		surface->capture_index = -1;
		surface->status = VASurfaceReady;

		surfaces[i] = id;
	}

	return VA_STATUS_SUCCESS;

fail:
	pthread_mutex_lock(&drv->mutex);
	while (i--)
		v4l2r_handles_free(&drv->surfaces, surfaces[i]);
	pthread_mutex_unlock(&drv->mutex);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus v4l2r_CreateSurfaces(VADriverContextP va_ctx, int width, int height,
			      int format, int num_surfaces,
			      VASurfaceID *surfaces)
{
	if (width <= 0 || height <= 0 || num_surfaces < 0)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	return v4l2r_CreateSurfaces2(va_ctx, format, width, height, surfaces,
				     num_surfaces, NULL, 0);
}

VAStatus v4l2r_DestroySurfaces(VADriverContextP va_ctx, VASurfaceID *surface_list,
			       int num_surfaces)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	if (num_surfaces < 0 || (num_surfaces && !surface_list))
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	/* EndPicture still holds the target pointer even before its first
	 * CAPTURE allocation. Reject the whole list before freeing anything. */
	for (int i = 0; i < num_surfaces; i++) {
		/* Duplicate IDs would pass lookup twice and cause a partial free. */
		for (int j = 0; j < i; j++) {
			if (surface_list[i] == surface_list[j])
				return VA_STATUS_ERROR_INVALID_SURFACE;
		}
		pthread_mutex_lock(&drv->mutex);
		struct v4l2r_surface *surface = V4L2R_SURFACE(drv, surface_list[i]);
		bool image_busy = surface && surface->image_refs;
		/* VPP retains its input between RenderPicture and EndPicture.
		 * That source need not belong to the VPP context itself. */
		bool vpp_busy = false;
		unsigned int iter = 0;
		struct v4l2r_context *ctx;
		while (surface && (ctx = v4l2r_handles_next(&drv->contexts, &iter, NULL))) {
			if (ctx->in_picture && ctx->vpp && ctx->vpp->have_params &&
			    ctx->vpp->src_surface == surface) {
				vpp_busy = true;
				break;
			}
			/* An encoder reads its input at EndPicture. */
			if (ctx->in_picture && v4l2r_enc_holds(ctx, surface)) {
				vpp_busy = true;
				break;
			}
		}
		pthread_mutex_unlock(&drv->mutex);
		if (!surface)
			return VA_STATUS_ERROR_INVALID_SURFACE;
		if (image_busy || vpp_busy)
			return VA_STATUS_ERROR_SURFACE_BUSY;
		if (surface->ctx && surface->ctx->in_picture &&
		    surface->ctx->pic.target == surface)
			return VA_STATUS_ERROR_SURFACE_BUSY;
	}

	for (int i = 0; i < num_surfaces; i++) {
		struct v4l2r_surface *surface;

		surface = V4L2R_SURFACE_GET(drv, surface_list[i]);
		if (!surface)
			return VA_STATUS_ERROR_INVALID_SURFACE;

		/* Wait for any pending decode (and conversion, which holds a
		 * pointer to the surface); the CAPTURE buffer itself stays
		 * with the context (V4L2 cannot free single buffers) but is
		 * detached from the surface and returned to the free pool
		 * for reuse by later decodes. */
		v4l2r_surface_ready(surface);
		if (surface->ctx && surface->capture_index >= 0)
			v4l2r_context_release_capture(surface->ctx,
						      surface->capture_index);

		v4l2r_surface_free_backing(surface);

		pthread_mutex_lock(&drv->mutex);
		v4l2r_handles_free(&drv->surfaces, surface_list[i]);
		pthread_mutex_unlock(&drv->mutex);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_SyncSurface(VADriverContextP va_ctx, VASurfaceID render_target)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;

	surface = V4L2R_SURFACE_GET(drv, render_target);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	return v4l2r_surface_ready(surface);
}

VAStatus v4l2r_QuerySurfaceStatus(VADriverContextP va_ctx,
				  VASurfaceID render_target,
				  VASurfaceStatus *status)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;

	if (!status)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	surface = V4L2R_SURFACE_GET(drv, render_target);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/* A frame held back for reordering reports rendering until submitted. */
	if (surface->ctx && surface->ctx->in_picture &&
	    surface->ctx->pic.target == surface) {
		*status = VASurfaceRendering;
		return VA_STATUS_SUCCESS;
	}
	VAStatus flush_status = v4l2r_flush_surface(surface);
	if (flush_status != VA_STATUS_SUCCESS)
		return flush_status;

	if (surface->ctx && surface->capture_index >= 0 &&
	    surface->status == VASurfaceRendering)
		v4l2r_reap_capture(surface->ctx);

	*status = surface->status;

	/* Decoded but not yet run through the format converter. */
	if (*status == VASurfaceReady && surface->convert_pending)
		*status = VASurfaceRendering;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_QuerySurfaceAttributes(VADriverContextP va_ctx, VAConfigID config,
				      VASurfaceAttrib *attrib_list,
				      unsigned int *num_attribs)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_config *cfg;
	struct v4l2r_config snapshot;
	unsigned int i = 0;

	if (!num_attribs)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	/* Dimension enumeration performs device I/O. Keep immutable config
	 * values after releasing the handle-table lock, not a borrowed object
	 * that DestroyConfig can free while an ioctl is in progress. */
	pthread_mutex_lock(&drv->mutex);
	cfg = V4L2R_CONFIG(drv, config);
	if (!cfg) {
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_INVALID_CONFIG;
	}
	snapshot = *cfg;
	pthread_mutex_unlock(&drv->mutex);
	cfg = &snapshot;

	if (!attrib_list) {
		*num_attribs = 8;
		return VA_STATUS_SUCCESS;
	}

	if (*num_attribs < 8) {
		*num_attribs = 8;
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	}
	struct v4l2r_dimensions dimensions;
	VAStatus status = v4l2r_config_dimensions(drv, cfg, &dimensions);
	if (status != VA_STATUS_SUCCESS)
		return status;

	attrib_list[i].type = VASurfaceAttribPixelFormat;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = VA_FOURCC_NV12;
	i++;

	/*
	 * With a format converter available, 10-bit configs hand out NV12
	 * surfaces (decoders here produce packed NV15, which the converter
	 * turns into NV12 in hardware; nothing can display NV15 directly).
	 * Advertising P010 alongside would make FFmpeg pick it as the exact
	 * match and bypass the converter, landing on the slow CPU readback
	 * path, so it is only offered when no converter exists.
	 */
	if ((cfg->rt_format & VA_RT_FORMAT_YUV420_10) &&
	    !v4l2r_converter_available(drv)) {
		attrib_list[i].type = VASurfaceAttribPixelFormat;
		attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
		attrib_list[i].value.type = VAGenericValueTypeInteger;
		attrib_list[i].value.value.i = VA_FOURCC_P010;
		i++;
	}

	attrib_list[i].type = VASurfaceAttribMemoryType;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
				       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
	i++;

	attrib_list[i].type = VASurfaceAttribMinWidth;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = dimensions.min_width;
	i++;

	attrib_list[i].type = VASurfaceAttribMinHeight;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = dimensions.min_height;
	i++;

	attrib_list[i].type = VASurfaceAttribMaxWidth;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = dimensions.max_width;
	i++;

	attrib_list[i].type = VASurfaceAttribMaxHeight;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = dimensions.max_height;
	i++;

	*num_attribs = i;
	return VA_STATUS_SUCCESS;
}

/*
 * Make a surface's pixels available for reading: submit a decode still held
 * back for reordering, wait for the decode itself, then for any pending
 * format conversion into the backing.
 */
VAStatus v4l2r_surface_ready(struct v4l2r_surface *surface)
{
	VAStatus status;

	/* BeginPicture reserves the target before there is a queued request to
	 * wait on. Reading/exporting it now would expose old or partial pixels. */
	if (surface->ctx && surface->ctx->in_picture &&
	    surface->ctx->pic.target == surface)
		return VA_STATUS_ERROR_SURFACE_BUSY;

	status = v4l2r_flush_surface(surface);
	if (status != VA_STATUS_SUCCESS)
		return status;

	if (!surface->ctx || surface->capture_index < 0)
		return surface->decode_status;

	uint64_t t0 = v4l2r_now_ns();
	status = v4l2r_sync_capture(surface->ctx, surface->capture_index);
	v4l2r_trace("export sync (surface 0x%08x buf #%d): %.2f ms\n",
		    surface->id, surface->capture_index,
		    (v4l2r_now_ns() - t0) / 1e6);
	if (status != VA_STATUS_SUCCESS)
		return status;
	if (surface->decode_status != VA_STATUS_SUCCESS)
		return surface->decode_status;

	return v4l2r_convert_wait(surface);
}

/* --- standalone surface backing --- */

void v4l2r_surface_free_backing(struct v4l2r_surface *surface)
{
	struct v4l2r_surface_backing *backing = surface->backing;

	if (!backing)
		return;

	for (unsigned int i = 0; i < VIDEO_MAX_PLANES; i++) {
		if (backing->map[i])
			munmap(backing->map[i], backing->plane_size[i]);
		if (backing->dmabuf_fd[i] >= 0)
			close(backing->dmabuf_fd[i]);
	}

	free(backing);
	surface->backing = NULL;
}

/*
 * Apple AVD sizes the reference data it stores after the picture from the bit
 * depth of the last SPS, not from the CAPTURE pixel format: P010 set before any
 * SPS gets 8-bit sized storage, and once a 10-bit stream starts the decode
 * context rejects the exported buffer as too small. Give the probe instance a
 * 10-bit SPS of the requested size before sizing a P010 CAPTURE format.
 * Decoders that do not derive anything from the SPS here are unaffected, and a
 * rejected control is harmless: the probe goes on as before.
 */
static void probe_set_bit_depth(int fd, uint32_t coded, uint32_t pixelformat,
				uint32_t width, uint32_t height)
{
#if HAVE_V4L2_PIX_FMT_P010
#if HAVE_V4L2_CTRL_H264
	struct v4l2_ctrl_h264_sps h264 = {0};
#endif
#if HAVE_V4L2_CTRL_HEVC
	struct v4l2_ctrl_hevc_sps hevc = {0};
#endif
	struct v4l2_ext_control control = {0};
	struct v4l2_ext_controls controls = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = 1,
		.controls = &control,
	};

	if (pixelformat != V4L2_PIX_FMT_P010)
		return;

	switch (coded) {
#if HAVE_V4L2_CTRL_H264
	case V4L2_PIX_FMT_H264_SLICE:
		h264.profile_idc = 110;		/* High 10 */
		h264.chroma_format_idc = 1;
		h264.bit_depth_luma_minus8 = 2;
		h264.bit_depth_chroma_minus8 = 2;
		h264.pic_width_in_mbs_minus1 = (width + 15) / 16 - 1;
		h264.pic_height_in_map_units_minus1 = (height + 15) / 16 - 1;
		h264.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
		control.id = V4L2_CID_STATELESS_H264_SPS;
		control.ptr = &h264;
		control.size = sizeof(h264);
		break;
#endif
#if HAVE_V4L2_CTRL_HEVC
	case V4L2_PIX_FMT_HEVC_SLICE:
		hevc.pic_width_in_luma_samples = width;
		hevc.pic_height_in_luma_samples = height;
		hevc.chroma_format_idc = 1;
		hevc.bit_depth_luma_minus8 = 2;
		hevc.bit_depth_chroma_minus8 = 2;
		control.id = V4L2_CID_STATELESS_HEVC_SPS;
		control.ptr = &hevc;
		control.size = sizeof(hevc);
		break;
#endif
	default:
		return;
	}

	/* Referenced by the codec cases above; the no-op casts keep them used
	 * when neither codec is compiled in. */
	(void)width;
	(void)height;
	(void)ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls);
#else
	/* Without the P010 UAPI there is nothing to pre-size: the P010 capture
	 * format cannot be requested in the first place. */
	(void)fd;
	(void)coded;
	(void)pixelformat;
	(void)width;
	(void)height;
#endif
}

/*
 * Allocate dma-buf backing for a surface that is not bound to a decode
 * context, using the CAPTURE queue of one of the enumerated decoders on
 * a throwaway file descriptor. Each open of a mem2mem device is its own
 * instance, and the exported dma-bufs keep the memory alive after close.
 */
static VAStatus backing_alloc(struct v4l2r_driver *drv,
			      struct v4l2r_surface *surface,
			      uint32_t width, uint32_t height,
			      uint32_t pixelformat)
{
	struct v4l2r_surface_backing *backing;
	bool saw_enomem = false;

	backing = calloc(1, sizeof(*backing));
	if (!backing)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	for (unsigned int i = 0; i < VIDEO_MAX_PLANES; i++)
		backing->dmabuf_fd[i] = -1;

	/* Try each enumerated decoder until one produces the layout. */
	for (unsigned int n = 0; n < drv->nb_decoders; n++) {
		struct v4l2_capability capability = {0};
		struct v4l2_create_buffers buffers;
		struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
		struct v4l2_buffer buffer = {0};
		struct v4l2_format format = {0};
		unsigned int capabilities;
		bool mplane;
		int fd;

		fd = open(drv->decoders[n].video_path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0)
			goto next;

		capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
			       capability.device_caps : capability.capabilities;
		mplane = capabilities & V4L2_CAP_VIDEO_M2M_MPLANE;

		/*
		 * Stateless decoders derive the CAPTURE geometry from the
		 * coded OUTPUT format and ignore the dimensions passed to
		 * S_FMT(CAPTURE) (e.g. cedrus clamps CAPTURE to the OUTPUT
		 * size, 16x16 on a freshly opened fd). Set a coded OUTPUT
		 * format at the wanted size first so the CAPTURE queue reports
		 * the resolution we actually asked for.
		 */
		struct v4l2_format out = {
			.type = mplane ?
				V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
				V4L2_BUF_TYPE_VIDEO_OUTPUT,
		};
		uint32_t coded = drv->decoders[n].nb_pixelformats ?
				 drv->decoders[n].pixelformats[0] : 0;

		if (mplane) {
			out.fmt.pix_mp.width = width;
			out.fmt.pix_mp.height = height;
			out.fmt.pix_mp.pixelformat = coded;
			out.fmt.pix_mp.num_planes = 1;
		} else {
			out.fmt.pix.width = width;
			out.fmt.pix.height = height;
			out.fmt.pix.pixelformat = coded;
		}

		if (coded && ioctl(fd, VIDIOC_S_FMT, &out) < 0)
			goto next;
		probe_set_bit_depth(fd, coded, pixelformat, width, height);

		format.type = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
				       V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (mplane) {
			format.fmt.pix_mp.width = width;
			format.fmt.pix_mp.height = height;
			format.fmt.pix_mp.pixelformat = pixelformat;
			format.fmt.pix_mp.num_planes = 1;
		} else {
			format.fmt.pix.width = width;
			format.fmt.pix.height = height;
			format.fmt.pix.pixelformat = pixelformat;
		}

		if (ioctl(fd, VIDIOC_S_FMT, &format) < 0)
			goto next;

		/* The device must produce exactly the requested layout. */
		if (mplane) {
			if (format.fmt.pix_mp.pixelformat != pixelformat ||
			    format.fmt.pix_mp.width < width ||
			    format.fmt.pix_mp.height < height)
				goto next;
			backing->width = format.fmt.pix_mp.width;
			backing->height = format.fmt.pix_mp.height;
			backing->pitch = format.fmt.pix_mp.plane_fmt[0].bytesperline;
			backing->nb_planes = format.fmt.pix_mp.num_planes;
		} else {
			if (format.fmt.pix.pixelformat != pixelformat ||
			    format.fmt.pix.width < width ||
			    format.fmt.pix.height < height)
				goto next;
			backing->width = format.fmt.pix.width;
			backing->height = format.fmt.pix.height;
			backing->pitch = format.fmt.pix.bytesperline;
			backing->nb_planes = 1;
		}

		/* Export may precede context creation, so the codec is unknown.
		 * Reserve enough tail storage for every codec with this layout;
		 * AVD keeps codec-specific reference data after the visible image. */
		struct v4l2_format allocation = format;
		for (unsigned int c = 1; c < drv->decoders[n].nb_pixelformats; c++) {
			struct v4l2_format candidate = format;
			if (mplane)
				out.fmt.pix_mp.pixelformat = drv->decoders[n].pixelformats[c];
			else
				out.fmt.pix.pixelformat = drv->decoders[n].pixelformats[c];
			if (ioctl(fd, VIDIOC_S_FMT, &out) < 0)
				continue;
			probe_set_bit_depth(fd, drv->decoders[n].pixelformats[c],
					    pixelformat, width, height);
			if (ioctl(fd, VIDIOC_S_FMT, &candidate) < 0)
				continue;
			if (v4l2r_format_pixelformat(&candidate) != pixelformat ||
			    v4l2r_format_width(&candidate) != backing->width ||
			    v4l2r_format_height(&candidate) != backing->height ||
			    v4l2r_format_bytesperline(&candidate) != backing->pitch)
				continue;
			if (mplane) {
				if (candidate.fmt.pix_mp.num_planes != backing->nb_planes)
					continue;
				for (unsigned int i = 0; i < backing->nb_planes; i++)
					if (candidate.fmt.pix_mp.plane_fmt[i].sizeimage >
					    allocation.fmt.pix_mp.plane_fmt[i].sizeimage)
						allocation.fmt.pix_mp.plane_fmt[i].sizeimage =
							candidate.fmt.pix_mp.plane_fmt[i].sizeimage;
			} else if (candidate.fmt.pix.sizeimage > allocation.fmt.pix.sizeimage) {
				allocation.fmt.pix.sizeimage = candidate.fmt.pix.sizeimage;
			}
		}
		if (mplane)
			out.fmt.pix_mp.pixelformat = coded;
		else
			out.fmt.pix.pixelformat = coded;
		if (ioctl(fd, VIDIOC_S_FMT, &out) < 0)
			goto next;
		probe_set_bit_depth(fd, coded, pixelformat, width, height);
		if (ioctl(fd, VIDIOC_S_FMT, &format) < 0)
			goto next;

		buffers = (struct v4l2_create_buffers) {
			.count = 1,
			.memory = V4L2_MEMORY_MMAP,
			.format = allocation,
		};
		/* Keep coherent MMAP. This backing is exported and the
		 * allocating queue is closed; the live decoder imports the
		 * dma-buf. Cache hints on this throwaway fd do not establish
		 * a CPU/importer sync contract (docs/EARLY_EXPORT_COHERENCE.md).
		 * Snapshot errno before close so CMA exhaustion stays
		 * ALLOCATION_FAILED. */
		if (ioctl(fd, VIDIOC_CREATE_BUFS, &buffers) < 0) {
			if (errno == ENOMEM)
				saw_enomem = true;
			goto next;
		}

		buffer.type = format.type;
		buffer.index = buffers.index;
		if (mplane) {
			buffer.length = VIDEO_MAX_PLANES;
			buffer.m.planes = planes;
		}
		if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0)
			goto next;

		if (mplane) {
			backing->nb_planes = buffer.length;
			for (unsigned int i = 0; i < buffer.length; i++)
				backing->plane_size[i] = buffer.m.planes[i].length;
		} else {
			backing->nb_planes = 1;
			backing->plane_size[0] = buffer.length;
		}

		for (unsigned int i = 0; i < backing->nb_planes; i++) {
			struct v4l2_exportbuffer exportbuffer = {
				.type = format.type,
				.index = buffer.index,
				.plane = i,
				.flags = O_RDWR | O_CLOEXEC,
			};

			if (ioctl(fd, VIDIOC_EXPBUF, &exportbuffer) < 0) {
				for (unsigned int j = 0; j < i; j++) {
					close(backing->dmabuf_fd[j]);
					backing->dmabuf_fd[j] = -1;
				}
				goto next;
			}

			backing->dmabuf_fd[i] = exportbuffer.fd;
		}

		backing->pixelformat = pixelformat;
		close(fd);

		surface->backing = backing;
		return VA_STATUS_SUCCESS;

next:
		close(fd);
	}

	free(backing);
	return saw_enomem ? VA_STATUS_ERROR_ALLOCATION_FAILED :
			    VA_STATUS_ERROR_OPERATION_FAILED;
}

/* Backing for a bare (unbound) surface, sized like the surface itself. */
VAStatus v4l2r_surface_alloc_backing(struct v4l2r_driver *drv,
				     struct v4l2r_surface *surface)
{
	uint32_t pixelformat = V4L2_PIX_FMT_NV12;

	/* With a converter around, 10-bit configs hand out NV12 surfaces;
	 * bare P010 only works on decoders that produce it natively. */
	if (surface->fourcc == VA_FOURCC_P010 ||
	    (!surface->fourcc &&
	     (surface->rt_format & VA_RT_FORMAT_YUV420_10) &&
	     !v4l2r_converter_available(drv))) {
#if HAVE_V4L2_PIX_FMT_P010
		pixelformat = V4L2_PIX_FMT_P010;
#else
		/* No P010 capture format in the installed UAPI (first in Linux
		 * 6.0): refuse rather than allocate NV12 storage that would
		 * reinterpret 10-bit data as 8-bit. */
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
#endif
	}

	return backing_alloc(drv, surface, surface->width, surface->height,
			     pixelformat);
}

/*
 * Backing used as the destination of the format converter: NV12 with the
 * exact layout of the converter's CAPTURE format. Kept (not replaced by
 * the decode CAPTURE buffer) for the whole life of the bound surface.
 */
VAStatus v4l2r_surface_convert_backing(struct v4l2r_driver *drv,
				       struct v4l2r_surface *surface)
{
	struct v4l2r_convert *conv = surface->ctx ? surface->ctx->conv : NULL;
	const struct v4l2_pix_format_mplane *pix_mp;
	VAStatus status;

	if (!conv)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	pix_mp = &conv->capture_format.fmt.pix_mp;

	/* Backing left over from pre-decode probing may have the wrong
	 * size; replace it. */
	if (surface->backing &&
	    (surface->backing->pixelformat != V4L2_PIX_FMT_NV12 ||
	     surface->backing->nb_planes != 1 ||
	     surface->backing->pitch != pix_mp->plane_fmt[0].bytesperline ||
	     surface->backing->plane_size[0] < pix_mp->plane_fmt[0].sizeimage))
		v4l2r_surface_free_backing(surface);

	if (surface->backing)
		return VA_STATUS_SUCCESS;

	status = backing_alloc(drv, surface, pix_mp->width, pix_mp->height,
			       V4L2_PIX_FMT_NV12);
	if (status != VA_STATUS_SUCCESS)
		return status;

	if (surface->backing->nb_planes != 1 ||
	    surface->backing->pitch != pix_mp->plane_fmt[0].bytesperline ||
	    surface->backing->plane_size[0] < pix_mp->plane_fmt[0].sizeimage) {
		v4l2r_diag(surface->ctx, V4L2R_DIAG_LEVEL_ERROR,
			   V4L2R_DIAG_UNSUPPORTED, "converter-backing", 0,
			   "conversion backing layout mismatch (pitch %u vs %u)",
			   surface->backing->pitch,
			   pix_mp->plane_fmt[0].bytesperline);
		v4l2r_surface_free_backing(surface);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return VA_STATUS_SUCCESS;
}

/* --- unified surface memory view --- */

int v4l2r_export_capture_dmabufs(struct v4l2r_context *ctx,
				 struct v4l2r_capture_buffer *capture,
				 int capture_index)
{
	for (unsigned int i = 0; i < capture->nb_planes; i++) {
		struct v4l2_exportbuffer exportbuffer = {
			.type = ctx->capture_format.type,
			.index = capture_index,
			.plane = i,
			.flags = O_RDWR | O_CLOEXEC,
		};

		if (capture->dmabuf_fd[i] >= 0)
			continue;

		if (ioctl(ctx->video_fd, VIDIOC_EXPBUF, &exportbuffer) < 0) {
			int ret = -errno;

			v4l2r_diag(ctx, V4L2R_DIAG_LEVEL_ERROR,
				   v4l2r_diag_errno_category(ret), "capture-export",
				   ret, "failed to export CAPTURE buffer %d plane %u: %s",
				   capture_index, i, strerror(-ret));
			return ret;
		}

		capture->dmabuf_fd[i] = exportbuffer.fd;
	}

	return 0;
}

VAStatus v4l2r_surface_capture_view(struct v4l2r_surface *surface,
				    bool need_maps,
				    struct v4l2r_frame_view *view)
{
	struct v4l2r_context *ctx = surface->ctx;
	struct v4l2r_capture_buffer *capture;
	const struct v4l2_format *format;

	memset(view, 0, sizeof(*view));

	if (!ctx || surface->capture_index < 0)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	capture = &ctx->captures[surface->capture_index];
	format = &ctx->capture_format;

	view->info = v4l2r_capture_format_info(ctx);
	if (!view->info)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	view->pixelformat = view->info->pixelformat;
	view->width = v4l2r_format_width(format);
	view->height = v4l2r_format_height(format);
	view->pitch = v4l2r_format_bytesperline(format);
	view->nb_planes = capture->nb_planes;

	if (v4l2r_export_capture_dmabufs(ctx, capture,
					 surface->capture_index) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	for (unsigned int i = 0; i < capture->nb_planes; i++) {
		view->plane_size[i] = capture->plane_size[i];
		view->dmabuf_fd[i] = capture->dmabuf_fd[i];
	}

	if (need_maps) {
		for (unsigned int i = 0; i < capture->nb_planes; i++) {
			if (!capture->map[i]) {
				void *addr;

				/* DMABUF-mode buffers have no mmap offset on
				 * the video node; map the dma-buf itself. */
				if (ctx->capture_memory == V4L2_MEMORY_DMABUF)
					addr = mmap(NULL, capture->plane_size[i],
						    PROT_READ | PROT_WRITE,
						    MAP_SHARED,
						    capture->dmabuf_fd[i], 0);
				else
					addr = mmap(NULL, capture->plane_size[i],
						    PROT_READ | PROT_WRITE,
						    MAP_SHARED, ctx->video_fd,
						    capture->plane_mem_offset[i]);
				if (addr == MAP_FAILED)
					return VA_STATUS_ERROR_OPERATION_FAILED;
				capture->map[i] = addr;
			}
			view->map[i] = capture->map[i];
		}
	}

	return VA_STATUS_SUCCESS;
}

/*
 * Resolve the memory behind a surface: the CAPTURE buffer of the decode
 * context when bound, standalone backing (allocated on demand) otherwise.
 */
VAStatus v4l2r_surface_view(struct v4l2r_driver *drv,
			    struct v4l2r_surface *surface, bool need_maps,
			    struct v4l2r_frame_view *view)
{
	memset(view, 0, sizeof(*view));

	/* With a conversion chain the decode CAPTURE buffer holds the raw
	 * decoder format; what clients see is the converted NV12 backing. */
	if (surface->ctx && surface->ctx->conv) {
		if (!surface->backing &&
		    v4l2r_surface_convert_backing(drv, surface) !=
		    VA_STATUS_SUCCESS)
			return VA_STATUS_ERROR_OPERATION_FAILED;
	} else if (surface->ctx && surface->capture_index >= 0) {
		return v4l2r_surface_capture_view(surface, need_maps, view);
	}

	/* Unbound surface: use (or create) standalone backing. */
	if (!surface->backing) {
		VAStatus status = v4l2r_surface_alloc_backing(drv, surface);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	view->info = v4l2r_format_by_pixelformat(surface->backing->pixelformat);
	if (!view->info)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	view->pixelformat = surface->backing->pixelformat;
	view->width = surface->backing->width;
	view->height = surface->backing->height;
	view->pitch = surface->backing->pitch;
	view->nb_planes = surface->backing->nb_planes;

	for (unsigned int i = 0; i < surface->backing->nb_planes; i++) {
		view->plane_size[i] = surface->backing->plane_size[i];
		view->dmabuf_fd[i] = surface->backing->dmabuf_fd[i];
	}

	if (need_maps) {
		for (unsigned int i = 0; i < surface->backing->nb_planes; i++) {
			if (!surface->backing->map[i]) {
				void *addr = mmap(NULL,
					surface->backing->plane_size[i],
					PROT_READ | PROT_WRITE, MAP_SHARED,
					surface->backing->dmabuf_fd[i], 0);
				if (addr == MAP_FAILED)
					return VA_STATUS_ERROR_OPERATION_FAILED;
				surface->backing->map[i] = addr;
			}
			view->map[i] = surface->backing->map[i];
		}
	}

	return VA_STATUS_SUCCESS;
}

/*
 * Describe the surface memory layout for DRM consumers, following the
 * layout rules of the FFmpeg v4l2-request hwcontext.
 */
static VAStatus fill_prime_descriptor(const struct v4l2r_frame_view *view,
				      uint32_t flags,
				      VADRMPRIMESurfaceDescriptor *desc)
{
	const struct v4l2r_format_info *info = view->info;
	uint32_t width = view->width;
	uint32_t height = view->height;
	uint32_t pitch = view->pitch;
	uint32_t pixelformat __attribute__((unused)) = view->pixelformat;
	uint64_t modifier;
	uint32_t offset1, pitch1;
	bool two_planes;

	modifier = info->drm_modifier;

	memset(desc, 0, sizeof(*desc));
	desc->fourcc = info->drm_format;
	desc->width = width;
	desc->height = height;
	desc->num_objects = view->nb_planes;

	for (unsigned int i = 0; i < view->nb_planes; i++) {
		int fd = dup(view->dmabuf_fd[i]);
		if (fd < 0) {
			for (unsigned int j = 0; j < i; j++)
				close(desc->objects[j].fd);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
		desc->objects[i].fd = fd;
		desc->objects[i].size = view->plane_size[i];
		desc->objects[i].drm_format_modifier = modifier;
	}

	/* AFBC formats expose a single plane, everything else has separate
	 * luma and chroma planes. */
	two_planes = (modifier >> 56) != DRM_FORMAT_MOD_VENDOR_ARM;

	offset1 = pitch * height;
	pitch1 = pitch;

#if defined(V4L2_PIX_FMT_NV12MT_COL128) && defined(V4L2_PIX_FMT_NV12MT_10_COL128)
	if (pixelformat == V4L2_PIX_FMT_NV12MT_COL128 ||
	    pixelformat == V4L2_PIX_FMT_NV12MT_10_COL128) {
		/* Raspberry Pi SAND tiling: pitch carries the column height. */
		pitch = height;
		pitch1 = pitch / 2;
		offset1 = 0;
	}
#endif
#if defined(V4L2_PIX_FMT_NV12_COL128) && defined(V4L2_PIX_FMT_NV12_10_COL128)
	if (pixelformat == V4L2_PIX_FMT_NV12_COL128 ||
	    pixelformat == V4L2_PIX_FMT_NV12_10_COL128) {
		uint64_t sand_modifier =
			DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(pitch);
		for (unsigned int i = 0; i < desc->num_objects; i++)
			desc->objects[i].drm_format_modifier = sand_modifier;
		offset1 = 128 * height;
		pitch = width;
		if (pixelformat == V4L2_PIX_FMT_NV12_10_COL128)
			pitch *= 2;
		pitch1 = pitch;
	}
#endif

	if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) && two_planes &&
	    view->nb_planes == 1 &&
	    (info->drm_format == DRM_FORMAT_NV12 ||
	     info->drm_format == DRM_FORMAT_P010) &&
	    modifier == DRM_FORMAT_MOD_LINEAR) {
		bool p010 = info->drm_format == DRM_FORMAT_P010;

		desc->num_layers = 2;
		desc->layers[0].drm_format = p010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
		desc->layers[0].num_planes = 1;
		desc->layers[0].object_index[0] = 0;
		desc->layers[0].offset[0] = 0;
		desc->layers[0].pitch[0] = pitch;
		desc->layers[1].drm_format = p010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
		desc->layers[1].num_planes = 1;
		desc->layers[1].object_index[0] = 0;
		desc->layers[1].offset[0] = offset1;
		desc->layers[1].pitch[0] = pitch1;

		return VA_STATUS_SUCCESS;
	}

	desc->num_layers = 1;
	desc->layers[0].drm_format = info->drm_format;
	desc->layers[0].num_planes = two_planes ? 2 : 1;
	desc->layers[0].object_index[0] = 0;
	desc->layers[0].offset[0] = 0;
	desc->layers[0].pitch[0] = pitch;
	if (two_planes) {
		desc->layers[0].object_index[1] = view->nb_planes > 1 ? 1 : 0;
		desc->layers[0].offset[1] = view->nb_planes > 1 ? 0 : offset1;
		desc->layers[0].pitch[1] = pitch1;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_ExportSurfaceHandle(VADriverContextP va_ctx, VASurfaceID surface_id,
				   uint32_t mem_type, uint32_t flags,
				   void *descriptor)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;
	struct v4l2r_frame_view view;
	VAStatus status;

	if (!descriptor)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

	surface = V4L2R_SURFACE_GET(drv, surface_id);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/* vaExportSurfaceHandle() does no synchronization, and the exported
	 * dma-buf carries no fence. Wait for the decode (and any format
	 * conversion) to finish so a consumer that does not vaSyncSurface()
	 * itself (e.g. --vo=dmabuf-wayland) cannot read a partially decoded
	 * frame. No-op for unbound or already-idle surfaces. */
	status = v4l2r_surface_ready(surface);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = v4l2r_surface_view(drv, surface, false, &view);
	if (status != VA_STATUS_SUCCESS)
		return status;

	return fill_prime_descriptor(&view, flags, descriptor);
}

/* --- unsupported display path --- */

VAStatus v4l2r_PutSurface(VADriverContextP va_ctx, VASurfaceID surface,
			  void *draw, short srcx, short srcy,
			  unsigned short srcw, unsigned short srch,
			  short destx, short desty,
			  unsigned short destw, unsigned short desth,
			  VARectangle *cliprects, unsigned int number_cliprects,
			  unsigned int flags)
{
	(void)va_ctx; (void)surface; (void)draw; (void)srcx; (void)srcy;
	(void)srcw; (void)srch; (void)destx; (void)desty; (void)destw;
	(void)desth; (void)cliprects; (void)number_cliprects; (void)flags;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_LockSurface(VADriverContextP va_ctx, VASurfaceID surface,
			   unsigned int *fourcc, unsigned int *luma_stride,
			   unsigned int *chroma_u_stride,
			   unsigned int *chroma_v_stride,
			   unsigned int *luma_offset,
			   unsigned int *chroma_u_offset,
			   unsigned int *chroma_v_offset,
			   unsigned int *buffer_name, void **buffer)
{
	(void)va_ctx; (void)surface; (void)fourcc; (void)luma_stride;
	(void)chroma_u_stride; (void)chroma_v_stride; (void)luma_offset;
	(void)chroma_u_offset; (void)chroma_v_offset; (void)buffer_name;
	(void)buffer;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_UnlockSurface(VADriverContextP va_ctx, VASurfaceID surface)
{
	(void)va_ctx; (void)surface;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
