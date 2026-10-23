/*
 * libdrm's names on top of the S8Mali user client, for Mesa's panfrost
 * driver. See xf86drm.h and s8_drm.c.
 *
 * A panfrost request carries DRM_COMMAND_BASE (0x40) in its number; the
 * user client's selectors are the plain command numbers 0x00..0x08.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "xf86drm.h"
#include "s8_drm.h"

#define DRM_COMMAND_BASE_	0x40

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xff);
	unsigned size = (unsigned)_IOC_SIZE(request);

	if (nr < DRM_COMMAND_BASE_) {
		errno = EINVAL;
		return -1;
	}
	return s8_drm_ioctl(fd, request, arg);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	unsigned long request = (3u << 30) | (size << 16) | ('d' << 8) |
	    (DRM_COMMAND_BASE_ + (index & 0xff));

	return drmIoctl(fd, request, data);
}

int
drmOpen(const char *name, const char *busid)
{
	(void)name;
	(void)busid;
	return s8_drm_open();
}

int
drmClose(int fd)
{
	return close(fd);
}

drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr v = calloc(1, sizeof(*v));

	(void)fd;
	if (v == NULL) {
		return NULL;
	}
	v->version_major = 1;
	v->version_minor = 0;
	v->version_patchlevel = 0;
	v->name = strdup("panfrost");
	v->name_len = 8;
	v->date = strdup("");
	v->date_len = 0;
	v->desc = strdup("S8Mali");
	v->desc_len = 6;
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v == NULL) {
		return;
	}
	free(v->name);
	free(v->date);
	free(v->desc);
	free(v);
}

void *
drmMap(int fd, unsigned long offset, unsigned long size)
{
	return s8_drm_map(fd, offset, size);
}

int
drmUnmap(void *address, unsigned long size)
{
	return s8_drm_unmap((uint64_t)(uintptr_t)address, size);
}
