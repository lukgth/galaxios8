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
/* _IOC_SIZE is not in Darwin's sys/ioctl.h, but the encoding is the same. */
#define S8_IOC_SIZE(r)		(((r) >> 16) & 0x3fff)

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xff);

	if (nr < DRM_COMMAND_BASE_) {
		errno = EINVAL;
		return -1;
	}
	return s8_drm_ioctl(fd, request, arg);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	unsigned long request = (3u << 30) | ((size & 0x3fff) << 16) | ('d' << 8) |
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

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	(void)handle;
	errno = ENOSYS;
	return -1;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	(void)handle;
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)num_handles;
	(void)timeout_nsec;
	(void)flags;
	(void)first_signaled;
	return 0;	/* SUBMIT waits for the job, so a fence is always ready */
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	(void)handle;
	(void)sync_file_fd;
	return 0;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	(void)handle;
	(void)sync_file_fd;
	errno = ENOSYS;
	return -1;
}

/* The S8Mali user client has no dma-buf export or import yet. */
int
drmPrimeHandleToFD(int fd, unsigned int handle, unsigned int flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, unsigned int *handle)
{
	(void)fd;
	(void)prime_fd;
	(void)handle;
	errno = ENOSYS;
	return -1;
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
