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

void *drmMap(int fd, unsigned long offset, unsigned long size);
int drmUnmap(void *address, unsigned long size);

#endif
