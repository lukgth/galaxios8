# Recover the test phone

This page holds the facts and the steps that bring the phone back after a failed boot. Measure the device again before you use an old command.

## Device facts (measured 2026-10-23)

The phone is an SM-G950F (`dreamlte`, `EXYNOS8895 EUR rev06`). The bootloader reports `G950FXXUCDVI1`. The recovery is TWRP 3.7.0_9-0.

TWRP has no `/dev/block/by-name`. The links live under `/dev/block/platform/11120000.ufs/by-name/`:

| Name | Device | Size | Use |
| --- | --- | --- | --- |
| BOOT | `sda7` | 41943040 bytes | Our loader and kernel |
| RECOVERY | `sda8` | 48234496 bytes | TWRP, never write |
| USERDATA | `sda24` | 58556678144 bytes | iOS root, Android data is lost |
| EFS | `sda3` | | Never write |

## Backups on the build host

The directory `work/` of the project holds these files. It is not in git.

| File | What it is | SHA-256 |
| --- | --- | --- |
| `work/boot_cur.img` | BOOT as found on 2026-10-23, before the first iOS write (another project's kernel) | `9c15951d21141634bf9f8add1e5fa35fedfd3b3dbc0ac46e29cd398c0a82c326` |
| `work/stock-boot.img` | Stock BOOT from the `G950FXXUCDVI1` firmware, unpacked from `AP_G950FXXUCDVI1_...tar.md5` | `cb62dd612ab38d19cb77e5b6d252cbed8a65e23b9dc25548f8deeb7de73d30db` |
| `work/RECOVERY.img` | Read-only copy of `sda8`, source of the ACPM DVFS firmware | |

## Enter TWRP

1. Hold Volume Down and Power until the screen goes black. Release both keys.
2. Hold Volume Up, Bixby and Power until the TWRP logo shows.
3. Connect the USB cable.
4. Run `adb devices -l`. The line must say `recovery` and `dreamlte`.

The phone also reboots into TWRP by itself when a test kernel uses `s8autoreset=N` and resets.

## Restore BOOT

1. Make sure that the phone reports `dreamlte` and `recovery`.
2. Write the image with `tools/dreamlte/flash-chunks.sh` and check the read-back hash. The script refuses a wrong model, a wrong state and a wrong size.
3. For a small image you can use `dd` directly:

```
adb push work/stock-boot.img /tmp/boot.img
adb shell "dd if=/tmp/boot.img of=/dev/block/sda7 bs=1048576 conv=fsync"
adb shell sync
```

4. Read `sda7` back and compare the SHA-256 with the file.

## Read the logs

The loader and the kernel log into the ramoops pmsg zone. The kernel also keeps a ring at the end of USERDATA.

1. Run `ANDROID_SERIAL=<serial> tools/dreamlte/pull-sweetlog.sh <name>`.
2. Read `logs/pmsg-<name>.txt` first. It holds the loader text and the early kernel text.
3. The pmsg zone is empty after a full power cycle. Take the log before you remove the battery or reset the phone by hand.

## Do not touch

- RECOVERY `sda8`, EFS `sda3`, the bootloader partitions, the PIT and the modem calibration.
- Heimdall with `--repartition`.
- A write to USERDATA destroys the Android data. The owner of this phone agreed to that loss.
