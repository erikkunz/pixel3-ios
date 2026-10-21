#!/usr/bin/env python3
"""Make iOS run its first-boot data migration on the next boot.

  sudo tools/migration-reset.py MOUNTED_ROOT

Removes the "migration already done" markers iosroot-tweaks.py used to set
(LastSystemVersion, DMUserDataDisposition in com.apple.migration) and
creates datamigrator's own sentinel, /private/var/datamigrator/migrate
("Migration sentinel present. Migration necessary."). The full migration
also seeds the LaunchServices database, which lsd otherwise waits for
forever (__LAUNCH_SERVICES_CLIENTS_ARE_WAITING_FOR_THE_DATABASE_TO_FINISH_
SEEDING__). Run as root.
"""
import os
import plistlib
import sys

MOBILE = 501


def main():
    if len(sys.argv) != 2 or os.geteuid() != 0:
        sys.exit(__doc__)
    root = sys.argv[1]
    pref = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.migration.plist')
    if os.path.exists(pref):
        st = os.stat(pref)
        with open(pref, 'rb') as f:
            data = plistlib.load(f)
        removed = [k for k in ('LastSystemVersion', 'DMUserDataDisposition') if data.pop(k, None) is not None]
        with open(pref, 'wb') as f:
            plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
        os.chown(pref, st.st_uid, st.st_gid)
        os.chmod(pref, st.st_mode & 0o7777)
        print('com.apple.migration: removed', removed or 'nothing', '; left', sorted(data))
    d = os.path.join(root, 'private/var/datamigrator')
    if not os.path.isdir(d):
        os.makedirs(d, 0o755)
        os.chown(d, MOBILE, MOBILE)
    sentinel = os.path.join(d, 'migrate')
    open(sentinel, 'a').close()
    os.chown(sentinel, MOBILE, MOBILE)
    os.chmod(sentinel, 0o644)
    print('sentinel', os.path.relpath(sentinel, root))


if __name__ == '__main__':
    main()
