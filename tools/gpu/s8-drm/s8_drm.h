#ifndef S8_DRM_H
#define S8_DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

/* The panfrost DRM requests on the S8Mali user client. */
int s8_drm_open(void);
int s8_drm_ioctl(int fd, unsigned long request, void *arg);
void *s8_drm_map(int fd, uint64_t offset, size_t size);
int s8_drm_unmap(uint64_t address, size_t size);

/* Submits are synchronous: a wait always succeeds. */
int s8_drm_syncobj_create(uint32_t *handle, uint32_t flags);
int s8_drm_syncobj_destroy(uint32_t handle);
int s8_drm_syncobj_wait(uint32_t handle, int64_t timeout_ns);

#endif
