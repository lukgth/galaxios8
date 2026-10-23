# GPU user space: what exists here and what needs a Mac

The kernel side of the Mali driver is in this repository and runs on the phone. The user space side, Mesa Panfrost and the CoreAnimation bridge, needs the iOS SDK and must be built on a Mac. This page states the contract between the two and the steps for the Mac.

## The kernel contract

`iokit/Kernel/arm/S8Mali.cpp` publishes the service `S8Mali` with the user client `S8MaliUserClient`. Every DRM `ioctl` of `include/uapi/drm/panfrost_drm.h` becomes one call of the client with the struct passed in and back.

| Selector | Request | In and out |
| --- | --- | --- |
| 0x00 | `DRM_IOCTL_PANFROST_SUBMIT` | `struct drm_panfrost_submit` |
| 0x01 | `DRM_IOCTL_PANFROST_WAIT_BO` | `struct drm_panfrost_wait_bo` |
| 0x02 | `DRM_IOCTL_PANFROST_CREATE_BO` | `struct drm_panfrost_create_bo` |
| 0x03 | `DRM_IOCTL_PANFROST_MMAP_BO` | `struct drm_panfrost_mmap_bo` |
| 0x04 | `DRM_IOCTL_PANFROST_GET_PARAM` | `struct drm_panfrost_get_param` |
| 0x05 | `DRM_IOCTL_PANFROST_GET_BO_OFFSET` | `struct drm_panfrost_get_bo_offset` |
| 0x08 | `DRM_IOCTL_PANFROST_MADVISE` | `struct drm_panfrost_madvise` |

`CREATE_BO` returns a handle and the GPU address of the buffer. That address is the memory type for `IOConnectMapMemory64`, and it is also what `MMAP_BO` returns, so `mmap` on the GPU address in the shim maps the buffer.

The submit call is synchronous: the kernel programs the job slot, polls it and returns when the job is done. Fences and interrupts are later work.

## What is in this directory

`s8-drm/` is a libdrm stand-in in C. It opens the service, turns `drmIoctl` into `IOConnectCallStructMethod`, maps buffers and stubs the sync objects, because a submit cannot outlive its call.

It is not built here. It needs the iOS SDK.

## Building this on a macOS runner

No Mac is needed here. Two workflows do the work:

- `.github/workflows/ios-userland.yml` builds `s8_test` and `libs8drm.a`
  with Xcode and the iPhoneOS SDK, and uploads them as the `ios-userland`
  artifact.
- `.github/workflows/mesa.yml` fetches Mesa 25.0.7, configures it with a
  cross file that targets `arm64-apple-ios16.0`, and uploads what it built.

`gh run download RUN_ID -n ios-userland -D out` puts the binaries on this
workstation. The root image then gets them:

1. Mount `ios16-root.img` read-write with `-o force`.
2. Copy `s8_test` to `/usr/bin/s8_test`, mode 0755, owner 0:0.
3. Add `/Library/LaunchDaemons/com.s8.gputest.plist` with `RunAtLoad`.
4. Unmount and run `fsck.hfsplus -f -n`.
5. Write the image to USERDATA with `flash-chunks.sh`.

The test writes `/var/log/s8-gpu-test.txt`, and the kernel logs the same
calls into the ring: `S8 GPU: GET_PARAM`, `S8 GPU: user BO`, `S8 GPU:
submit ok`. Read the ring from TWRP, no root pull needed.

## The Mac steps

1. Build Mesa 25.0.7 (`-Dgallium-drivers=panfrost -Dplatforms= -Dosmesa=true`, no LLVM) with the iOS SDK, as `hackios-xnu-userland/tools/mk-mesa.sh` does for their GPU. Keep 25.0.7: OSMesa is gone in 25.1.
2. Patch Mesa to use `s8-drm` where it uses libdrm: `drmOpen`, `drmIoctl`, `drmMap` and the syncobj calls. Mesa's `src/gallium/drivers/panfrost/kmod/` is the only place that talks to the kernel.
3. Patch the panfrost backend for Darwin the way HackiOS patched freedreno: no `mmap` on a device file; the buffer map goes through `s8_drm_map`.
4. Add `panfrost` to the driver list `DRIVERS=softpipe,panfrost` so the first tests can compare a software and a hardware frame.
5. The CoreAnimation bridge is `userland/src/eagl.c` of HackiOS, MIT licensed. It replaces `EAGLContext` in backboardd and uses OSMesa for contexts. Port the addresses to 20H392.
6. Inject the library into backboardd. Our AMFI stand-in is permissive, so `DYLD_INSERT_LIBRARIES` is the first thing to try.

## The order of tests on the phone

| Step | Test | Output |
| --- | --- | --- |
| G1 | kernel self test, `s8gpu=1` | `S8 GPU:` lines in the log ring |
| G2 | BOs and a GPU fault | `S8 GPU: BO job` and `unmapped job` lines |
| G3 | `s8-drm` opens the service from a small test binary | handle and GPU address printed |
| G4 | a triangle through OSMesa into an IOSurface | photo of the phone |
| G5 | backboardd draws with Panfrost | frame time in the log |

Do not report G4 or G5 from a software renderer.
