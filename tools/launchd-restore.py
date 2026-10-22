#!/usr/bin/env python3
"""Put launchd jobs back that iosroot-tweaks.py removed (its DENY list).

  sudo tools/launchd-restore.py MOUNTED_ROOT ORIGINAL_LAUNCHD_PLIST LABEL...

ORIGINAL_LAUNCHD_PLIST is System/Library/xpc/launchd.plist from the IPSW
(`ipsw extract --files --pattern 'System/Library/xpc/launchd.plist$'`).
Each job whose Label is given is copied into the image's launchd.plist
under its original key, with _PanicOnCrash renamed as the tweaks do.
Owner and mode of the file are kept. Run as root.
"""
import os
import plistlib
import sys


def main():
    if len(sys.argv) < 4 or os.geteuid() != 0:
        sys.exit(__doc__)
    root, orig, labels = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    with open(orig, 'rb') as f:
        source = plistlib.load(f)['LaunchDaemons']
    st = os.stat(path)
    with open(path, 'rb') as f:
        data = plistlib.load(f)
    jobs = data['LaunchDaemons']
    for key, job in source.items():
        if job.get('Label') in labels:
            job = dict(job)
            if '_PanicOnCrash' in job:
                job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            print('restored' if key not in jobs else 'replaced', key)
            jobs[key] = job
            labels.discard(job['Label'])
    for label in sorted(labels):
        print('not in the original:', label)
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)


if __name__ == '__main__':
    main()
