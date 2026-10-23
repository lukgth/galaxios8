# Memory on the Galaxy S8

This page holds the physical memory map of this phone, what XNU uses today, and what must change before XNU can use more.

## Ranges measured on the phone

Read from TWRP (`/proc/iomem`) on 2026-10-23. The kernel of TWRP is Linux 4.4, so the names below are its names.

| Range | Size | What it is |
| --- | --- | --- |
| `0x80000000`-`0xbc7fffff` | 1 GiB minus 56 MiB | System RAM, low bank, firmware carve-outs inside |
| `0xc0000000`-`0xffffffff` | 1 GiB | System RAM, second low bank |
| `0x880000000`-`0x8ffffffff` | 2 GiB | System RAM, high bank |

TWRP reports 3564116 KiB of RAM in total (about 3.5 GiB of the 4 GB package).

## Reserves inside the low 2 GiB

The device tree of TWRP lists these reserved regions. Do not use them.

| Region | Range | Use |
| --- | --- | --- |
| `secure_camera` | `0xd0000000`-`0xd1400000` | TrustZone |
| `video_stream` | from `0xe3c00000` | camera, modem, ABOX and others above it |
| camera ion | `0xc0400000`-`0xce800000` | holds the framebuffer at `0xcc000000` |
| ramoops | `0x92000000`, 2 MiB | loader and kernel logs |

The loop `0xd1400000`-`0xe3c00000` has no reserve. The loader and the GPU experiments use it.

## What XNU uses today

XNU takes one contiguous range. The high bank is the only range without firmware carve-outs, so XNU gets the high bank and nothing else.

| Setting | Result |
| --- | --- |
| `S8_DRAM_MIB=512` | the old default, 512 MiB |
| `S8_DRAM_MIB=2048` | 2 GiB, the current daily image |
| more than 2048 | not possible without multi-range support in pmap |

At 2 GiB the loader's UFS probe buffer at `0x8f0000000` lies inside XNU RAM, so the loader zeroes that area again after the probe.

### Fixes that made 2 GiB work

1. The kernel was built with macOS policies, and the compressor reserved 16 times RAM of virtual space. The S8 build now uses the iOS sizing.
2. The commpage text must sit at a 16 KiB aligned slot, because XNU uses 16 KiB VM pages on 4 KiB hardware pages. A random slot caused `write_pte_fast: PTE write is unaligned`.
3. `kmem` keeps 1 GiB of the largest kernel map hole out of its sizing.

Runs c19 and c23a/b/c booted three times in a row at 2 GiB with a clean restart.

## What more RAM needs

To use the low bank as well, pmap must accept several physical ranges. The steps:

1. Add the ranges to the Apple Device Tree and to `arm_vm_init`.
2. Teach the VM page tables and `pmap` about a second and third RAM range.
3. Keep the firmware carve-outs out of the free lists.
4. Test with cold boots, warm boots, jetsam pressure and a soak.

Do not advertise 4 GB. The phone does not have 4 GB of usable RAM, and the low banks hold firmware that must stay reserved.
