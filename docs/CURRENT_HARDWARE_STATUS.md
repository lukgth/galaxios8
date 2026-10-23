# Current hardware status

Measured on the SM-G950F of this workstation on 2026-10-24. The checkout is `8f44837`. Claims come from `/proc` and device tree reads in TWRP and from the kernel log ring of the phone. A claim without a log line is marked as not tested.

## Device

| Item | Value |
| --- | --- |
| Model | SM-G950F, `dreamlte`, `EXYNOS8895 EUR rev06` |
| Bootloader | `G950FXXUCDVI1` |
| Recovery | TWRP 3.7.0_9-0 |
| RAM in TWRP | 3564116 KiB |
| BOOT | `sda7`, 41943040 bytes |
| RECOVERY | `sda8`, 48234496 bytes, untouched |
| USERDATA | `sda24`, 58556678144 bytes |

## Subsystems

| Area | State | Evidence |
| --- | --- | --- |
| Boot to iOS | Works | `hfs: mounted iOSRoot`, launchd, 397 execs, SpringBoard threads in `logs/sweetlog-s8base1.txt` |
| CPUs | 8 of 8 | `A5XELTE CPU: 8/8 CPUs initialized, SGI1 verified` |
| CPU clocks | A53 1690 MHz, Mongoose 1703 MHz | `S8 DVFS: big 1703000 lit 1690000 kHz` |
| Thermal | 40 C at start, 44-53 C in 7 minutes | `S8 DVFS: ... C (max ..)` lines |
| RAM | 2 GiB | `vm_page_bootstrap: 127920 free pages` with 16 KiB pages |
| Storage | HFS+ root in USERDATA, read and write | `hfs: mounted iOSRoot`, journal initialized |
| Display | CPU render, 1125x2436 scaled to the panel | `SWEET FB: staged present, stage 1125x2436` |
| Touch | Works | `S8 TOUCH: S6SY761 id ac3761`, tap events |
| Buttons | Enabled in the build | not pressed in this run |
| Battery | Real level and voltage | `71% 4226mV external=1 charging=0` |
| Charging | Not verified | no charge current is set by the kernel yet |
| RTC | Set by the loader | `s8rtc=...` in the boot arguments |
| Logs | Loader and kernel text in ramoops and in a ring at the end of USERDATA | `logs/sweetlog-*.txt`, `logs/pmsg-*.txt` |
| GPU | Driver built, self test not yet run | kernel did not load the class in the first attempt |
| Wi-Fi | No driver | hardware identified, `docs/DREAMLTE_WIFI.md` |
| Bluetooth | No driver | hardware identified, `docs/DREAMLTE_BLUETOOTH.md` |
| Audio | No driver | hardware identified, `docs/DREAMLTE_AUDIO.md` |
| Metal | Not started | `docs/DREAMLTE_METAL_STATUS.md` |

## Images of this run

| Image | SHA-256 | Notes |
| --- | --- | --- |
| `work/ios16-root.img` | `b3dbbee9e6de766fe4aa4469b2be79d4f1b3df28ac666b32e5ae69bc61f2e961` | 16 GiB HFS+, 12 GiB used |
| `work/s8-test2-boot.img` | `e653cb3a0a0eed0710b4d4a54db751b885470f22cefeca353cffca3e26be1f3d` | baseline, `s8autoreset=420` |
| `work/s8-gpu1-boot.img` | `04e0c35998be0a178088da93cb73ff4ce0fe81b6dd5f2fa7fc91d8db43c4a5d8` | first Mali attempt, no driver loaded |
| `work/boot_cur.img` | `9c15951d21141634bf9f8add1e5fa35fedfd3b3dbc0ac46e29cd398c0a82c326` | BOOT as found, another project's kernel |
| `work/stock-boot.img` | `cb62dd612ab38d19cb77e5b6d252cbed8a65e23b9dc25548f8deeb7de73d30db` | stock BOOT of `G950FXXUCDVI1` |

## How this build was made

1. The kernel comes from GitHub Actions (`xnu` workflow) with every patch in `xnu/patches`.
2. The loader is built on Linux with `gcc-aarch64-linux-gnu` and the ACPM DVFS firmware taken from this phone's TWRP image.
3. `tools/dreamlte/make-boot.sh` packs the BOOT image with the stock header of `G950FXXUCDVI1` and `S8_DRAM_MIB=2048`.
4. `tools/dreamlte/flash-chunks.sh` writes BOOT or USERDATA in verified chunks.
5. `tools/dreamlte/cycle.sh` writes BOOT, reboots, waits for TWRP and pulls the logs.

## Known bugs

- All rendering is on the CPU. Frames can tear when the panel scans while the copy runs.
- The scheduler treats the eight cores as equal.
- No Wi-Fi, Bluetooth, audio, cameras, sensors or GPS.
- Security is off: permissive AMFI, sandbox and a key store stand-in. Do not store personal data.
