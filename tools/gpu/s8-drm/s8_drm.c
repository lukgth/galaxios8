/*
 * s8-drm: the small part of libdrm that Mesa's panfrost driver uses, on the
 * S8Mali IOKit user client of the Galaxy S8 kernel (iokit/Kernel/arm/S8Mali.cpp).
 *
 * A DRM request becomes a call of the user client. The selector is the
 * request's command number: DRM_IOCTL_PANFROST_* carry DRM_COMMAND_BASE
 * (0x40), and the user client uses 0x00..0x08, so the base is subtracted.
 * The request's structure goes in and comes back unchanged, as panfrost's
 * kernel side expects.
 *
 * Memory: the user client's client memory of type N is the BO whose handle
 * is N, or whose GPU address is N. MMAP_BO returns the GPU address, so
 * s8_drm_map(fd, gpu_address, size) maps the same buffer into this process.
 *
 * Synchronisation: SUBMIT in the kernel is synchronous (it polls the job
 * slot), so a wait always succeeds and the syncobj calls are stubs. That is
 * enough for panfrost without explicit fences.
 *
 * This file is part of the project, not of Mesa. Mesa is patched to call
 * s8_drm_open/s8_drm_map where it calls drmOpen/mmap (see tools/gpu/README.md).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "s8_drm.h"

#define S8_GPU_SERVICE		"S8Mali"
#define DRM_COMMAND_BASE_	0x40

static io_connect_t s8_connection;
static int s8_fd = -1;

static int
s8_call(unsigned selector, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(s8_connection, selector,
	    data, size, data, &returned);

	switch (result) {
	case kIOReturnSuccess:      return 0;
	case kIOReturnNoMemory:     return ENOMEM;
	case kIOReturnBadArgument:  return EINVAL;
	case kIOReturnTimeout:      return ETIMEDOUT;
	case kIOReturnBusy:         return EBUSY;
	case kIOReturnNotFound:     return ENOENT;
	case kIOReturnUnsupported:  return ENOSYS;
	default:                    return EIO;
	}
}

int
s8_drm_open(void)
{
	io_service_t service;

	if (s8_fd >= 0) {
		return dup(s8_fd);
	}
	service = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching(S8_GPU_SERVICE));
	if (service == 0) {
		errno = ENODEV;
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &s8_connection) !=
	    KERN_SUCCESS) {
		IOObjectRelease(service);
		errno = ENODEV;
		return -1;
	}
	IOObjectRelease(service);
	s8_fd = open("/dev/null", O_RDWR);
	return s8_fd >= 0 ? dup(s8_fd) : -1;
}

int
s8_drm_ioctl(int fd, unsigned long request, void *arg)
{
	unsigned selector;
	int error;

	(void)fd;
	selector = (unsigned)(request & 0xff);
	if (selector >= DRM_COMMAND_BASE_) {
		selector -= DRM_COMMAND_BASE_;
	}
	error = s8_call(selector, arg, IOCPARM_LEN(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

void *
s8_drm_map(int fd, uint64_t offset, size_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = size;

	(void)fd;
	if (IOConnectMapMemory64(s8_connection, (uint32_t)offset, mach_task_self(),
	    &address, &mapped, kIOMapAnywhere) != KERN_SUCCESS) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return (void *)(uintptr_t)address;
}

int
s8_drm_unmap(uint64_t address, size_t size)
{
	return IOConnectUnmapMemory64(s8_connection, 0, mach_task_self(),
	    (mach_vm_address_t)address) == KERN_SUCCESS ? 0 : -1;
}

/* Sync objects: submits are synchronous, so every wait succeeds. */
int
s8_drm_syncobj_create(uint32_t *handle, uint32_t flags)
{
	(void)flags;
	*handle = 1;
	return 0;
}

int
s8_drm_syncobj_destroy(uint32_t handle)
{
	(void)handle;
	return 0;
}

int
s8_drm_syncobj_wait(uint32_t handle, int64_t timeout_ns)
{
	(void)handle;
	(void)timeout_ns;
	return 0;
}
