#!/usr/bin/env python3
"""Write only the chunks that differ between two images to a partition.

  flash-diff.py OLD.img NEW.img MANIFEST.json PART [CHUNK_MIB]

OLD.img is the image the phone currently holds (read back from it), NEW.img
the edited one, MANIFEST.json its chunk manifest from chunk-manifest.py.
PART is BOOT or USERDATA. Each changed chunk is pushed, written with dd and
read back; the SHA-256 must match. Unchanged chunks are not touched, so the
live file system keeps every change the OS made after the last write.

Safety: same checks as flash-chunks.sh (serial, model, state, partition
size), and it refuses to run if the two files differ in size.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile

PART = {'BOOT': '/dev/block/sda7', 'USERDATA': '/dev/block/sda24'}


def sha_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def adb(*args, check=True):
    r = subprocess.run(['adb', *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit(f'adb {" ".join(args)} failed: {r.stderr.strip()}')
    return r.stdout.strip()


def main():
    old, new, manifest_path, part = sys.argv[1:5]
    if part not in PART:
        raise SystemExit('only BOOT or USERDATA can be written')
    dev = PART[part]
    man = json.load(open(manifest_path))
    size = man['chunk_mib'] << 20

    if os.path.getsize(old) != os.path.getsize(new):
        raise SystemExit('the images differ in size')

    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise SystemExit('set ANDROID_SERIAL to the S8 serial (adb devices -l)')
    os.environ['ANDROID_SERIAL'] = serial
    if adb('shell', 'getprop', 'ro.product.device') != 'dreamlte':
        raise SystemExit('the phone is not a dreamlte')
    if adb('get-state') != 'recovery':
        raise SystemExit('the phone is not in recovery')
    psize = int(adb('shell', 'blockdev', '--getsize64', dev) or 0)
    if psize < len(man['chunks']) * size:
        raise SystemExit(f'{dev} is {psize} bytes, the image needs more')

    changed = []
    with open(old, 'rb') as fo, open(new, 'rb') as fn:
        for c in man['chunks']:
            i = c['i']
            if fo.read(size) != fn.read(size):
                changed.append(c)
    print(f'{len(changed)} of {len(man["chunks"])} chunks differ, writing to {dev}')

    adb('shell', 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true', check=False)
    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, 'c.bin')
    try:
        for n, c in enumerate(changed, 1):
            i, want = c['i'], c['sha']
            with open(new, 'rb') as f:
                f.seek(i * size)
                blob = f.read(size)
            assert hashlib.sha256(blob).hexdigest() == want
            with open(path, 'wb') as f:
                f.write(blob)
            for attempt in range(1, 6):
                subprocess.run(['adb', 'push', path, '/tmp/c.bin'],
                               check=True, capture_output=True)
                adb('shell', f'dd if=/tmp/c.bin of={dev} bs=1048576 seek={i * man["chunk_mib"]} '
                    f'count={man["chunk_mib"]} conv=notrunc 2>/dev/null; sync; rm -f /tmp/c.bin')
                got = adb('shell', f'dd if={dev} bs=1048576 skip={i * man["chunk_mib"]} '
                          f'count={man["chunk_mib"]} 2>/dev/null | sha256sum')
                if got.split()[0] == want:
                    break
                print(f'chunk {i}: try {attempt} mismatch', file=sys.stderr)
            else:
                raise SystemExit(f'chunk {i} failed five times')
            if n % 8 == 0 or n == len(changed):
                print(f'{n}/{len(changed)} chunks written')
    finally:
        subprocess.run(['rm', '-rf', tmp])


if __name__ == '__main__':
    main()
