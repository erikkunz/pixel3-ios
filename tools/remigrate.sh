#!/bin/bash
# Let iOS run its first-boot data migration again (it registers the
# removable system apps and sets up wallpapers/posters) and put
# mobileassetd back, as one diff flash.
#
#   tools/remigrate.sh            (asks for the sudo password once)
#
# First, in recovery: tools/phone.sh sync work/ios-root.img. Needs the
# IPSW's original launchd.plist in work/orig (see tools/launchd-restore.py).
# Then flash work/diff.simg to userdata and mv work/ios-root-new.img
# work/ios-root.img. Undo if the migration hangs: tools/prefs-fix.sh
# com.apple.migration ... (or rerun tools/iosroot-tweaks.py).
set -euo pipefail
cd "$(dirname "$0")/.."
orig=$(find work/orig -path '*System/Library/xpc/launchd.plist' | head -1)
[ -n "$orig" ] || { echo "no original launchd.plist in work/orig" >&2; exit 1; }
rm -f work/ios-root-new.img work/diff.simg
cp -c work/ios-root.img work/ios-root-new.img
sudo tools/rootimg.sh mount work/ios-root-new.img
trap 'sudo tools/rootimg.sh umount' EXIT
sudo python3 tools/migration-reset.py work/root
sudo python3 tools/launchd-restore.py work/root "$orig" com.apple.mobileassetd
sudo tools/rootimg.sh umount
trap - EXIT
python3 tools/simgdiff.py work/ios-root.img work/ios-root-new.img work/diff.simg
ls -la work/diff.simg
