#!/usr/bin/env python3
"""Add a daemon job to an iOS root image's launchd table.

  add-launchd-job.py ROOT_DIR LABEL PROGRAM [ARG...]

ROOT_DIR is the mounted root file system. iOS 16 launchd does not read
/Library/LaunchDaemons at boot: it reads the job table in
/System/Library/xpc/launchd.plist (see docs/DREAMLTE_GPU.md). This script adds
or replaces one entry there and leaves every other job alone.

The program must exist in the image. /bin/sh does not exist on iOS, so pass
the real binary.
"""
import os
import plistlib
import sys

TABLE = 'System/Library/xpc/launchd.plist'


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    root, label, program, *args = sys.argv[1:]
    path = os.path.join(root, TABLE)
    if not os.path.exists(os.path.join(root, program.lstrip('/'))):
        raise SystemExit(f'{program} is not in the image')

    with open(path, 'rb') as f:
        table = plistlib.load(f)

    jobs = table['LaunchDaemons']
    key = '/System/Library/LaunchDaemons/%s.plist' % label
    jobs[key] = {
        'Label': label,
        'ProgramArguments': [program, *args],
        'RunAtLoad': True,
        'UserName': 'root',
        'ProcessType': 'Background',
        'StandardOutPath': '/var/log/%s.out' % label,
        'StandardErrorPath': '/var/log/%s.err' % label,
    }
    with open(path, 'wb') as f:
        plistlib.dump(table, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, 0, 0)
    os.chmod(path, 0o644)
    print(f'{label} added, {len(jobs)} jobs in the table')


if __name__ == '__main__':
    main()
