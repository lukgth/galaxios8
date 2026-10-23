/*
 * xf86drm.h for iOS: the small part of libdrm that Mesa's panfrost driver
 * uses, over the S8Mali IOKit user client (s8_drm.c, s8_drm_libdrm.c).
 *
 * The DRM_IOCTL_* macros and DRM_COMMAND_BASE come from Mesa's own copy of
 * drm-uapi/drm.h, which panfrost_drm.h includes. This header only adds the
 * functions libdrm would provide.
 */
#ifndef S8_XF86DRM_H
#define S8_XF86DRM_H

#include <stdint.h>

typedef struct _drmVersion {
	int version_major;
	int version_minor;
	int version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);

int drmOpen(const char *name, const char *busid);
int drmClose(int fd);

/* Sync objects: the S8Mali client waits inside SUBMIT, so nothing waits here. */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
    unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
    uint32_t src_handle, uint64_t src_point, uint32_t flags);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmCloseBufferHandle(int fd, uint32_t handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

int drmPrimeHandleToFD(int fd, unsigned int handle, unsigned int flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, unsigned int *handle);

void *drmMap(int fd, unsigned long offset, unsigned long size);
int drmUnmap(void *address, unsigned long size);

#endif
