// SPDX-License-Identifier: MIT
// Keep the MSM uAPI in C: older libdrm headers use the C++ keyword "or".
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <msm_drm.h>

// This uAPI flag may be missing from older libdrm headers.
#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif

int irisva_drm_is_msm(int drm_fd);

int irisva_msm_allocate(int drm_fd, uint64_t size, int *coherent) {
    *coherent = 0;
    // MSM ioctl numbers are driver-private: on another DRM driver (Panthor on
    // CIX Sky1, for example) the same number selects an unrelated ioctl.
    if (!irisva_drm_is_msm(drm_fd)) {
        errno = ENODEV;
        return -1;
    }
    struct drm_msm_gem_new allocation = {.size = size, .flags = MSM_BO_CACHED_COHERENT};
    if (drmIoctl(drm_fd, DRM_IOCTL_MSM_GEM_NEW, &allocation) < 0) {
        // Unsupported cache modes return EINVAL. Do not hide allocation or
        // permission failures by retrying with different memory attributes.
        if (errno != EINVAL && errno != EOPNOTSUPP)
            return -1;
        allocation.flags = MSM_BO_WC;
        if (drmIoctl(drm_fd, DRM_IOCTL_MSM_GEM_NEW, &allocation) < 0)
            return -1;
    }
    int prime_fd = -1;
    int result = drmPrimeHandleToFD(drm_fd, allocation.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd);
    int saved_errno = errno;
    struct drm_gem_close close_handle = {.handle = allocation.handle};
    if (drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_handle) < 0) {
        if (prime_fd >= 0)
            close(prime_fd);
        return -1;
    }
    if (result < 0) {
        errno = saved_errno;
        return -1;
    }
    *coherent = allocation.flags == MSM_BO_CACHED_COHERENT;
    return prime_fd;
}

int irisva_drm_is_msm(int drm_fd) {
    drmVersionPtr version = drmGetVersion(drm_fd);
    if (!version)
        return 0;
    int msm = version->name && !strcmp(version->name, "msm");
    drmFreeVersion(version);
    return msm;
}
