#!/usr/bin/env python3
"""Write the chunks of NEW.img that differ from BASE.img to USERDATA.

  apply-root-changes.py BASE.img NEW.img [--force]

The phone must be in TWRP. For every changed chunk the phone's current bytes
are read first:

  - they equal BASE there: the chunk is written and verified;
  - they differ (the running OS changed that block): the chunk is skipped and
    the number is printed, so nothing the OS wrote is overwritten.

--force writes such chunks anyway.

BASE.img must be the image the phone was last flashed from.
"""
import hashlib
import os
import subprocess
import sys

SUB = 1 << 20		# granularity of a single write

DEV = '/dev/block/sda24'
CHUNK = 32 << 20
MIB = 32


def adb(*args):
    r = subprocess.run(['adb', *args], capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f'adb {" ".join(args)}: {r.stderr.decode(errors="replace")}')
    return r.stdout


def adb_shell(cmd):
    return adb('shell', cmd)


def chunk_sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    base_path, new_path = sys.argv[1:3]
    force = '--force' in sys.argv

    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise SystemExit('set ANDROID_SERIAL to the S8 serial')
    os.environ['ANDROID_SERIAL'] = serial
    if adb_shell('getprop ro.product.device').decode().strip() != 'dreamlte':
        raise SystemExit('the phone is not a dreamlte')
    if adb('get-state').decode().strip() != 'recovery':
        raise SystemExit('the phone is not in recovery')
    if os.path.getsize(base_path) != os.path.getsize(new_path):
        raise SystemExit('the images differ in size')
    adb_shell('umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true')

    total = os.path.getsize(base_path) // CHUNK
    written = drifted = same = 0
    with open(base_path, 'rb') as base, open(new_path, 'rb') as new:
        for i in range(total):
            b = base.read(CHUNK)
            n = new.read(CHUNK)
            if b == n:
                same += 1
                continue
            skip = i * MIB
            live = adb_shell(f'dd if={DEV} bs=1048576 skip={skip} count={MIB} 2>/dev/null')
            if len(live) != CHUNK:
                raise SystemExit(f'chunk {i}: read back {len(live)} bytes')
            # One megabyte at a time: a changed file usually touches one
            # block, and a drifted block is left to the running system.
            for so in range(0, CHUNK, SUB):
                sb = b[so:so + SUB]
                sn = n[so:so + SUB]
                if sb == sn:
                    continue
                sl = live[so:so + SUB]
                if chunk_sha(sl) != chunk_sha(sb) and not force:
                    print(f'chunk {i} +{so >> 20} MiB: the phone differs, skipped')
                    drifted += 1
                    continue
                mib = skip + (so >> 20)
                path = f'/tmp/s{mib}.bin'
                with open('/tmp/s8-sub.bin', 'wb') as f:
                    f.write(sn)
                adb('push', '/tmp/s8-sub.bin', path)
                adb_shell(f'dd if={path} of={DEV} bs=1048576 seek={mib} count=1 '
                          f'conv=notrunc 2>/dev/null; sync; rm -f {path}')
                back = adb_shell(f'dd if={DEV} bs=1048576 skip={mib} count=1 2>/dev/null')
                if chunk_sha(back) != chunk_sha(sn):
                    raise SystemExit(f'chunk {i} +{so >> 20} MiB: write did not verify')
                written += 1
                print(f'chunk {i} +{so >> 20} MiB: written')
    print(f'unchanged {same}, written {written}, drifted {drifted}')


if __name__ == '__main__':
    main()
