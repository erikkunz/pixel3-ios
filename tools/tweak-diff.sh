#!/bin/bash
# Re-run tools/iosroot-tweaks.py on the root image and make a diff flash.
#
#   tools/tweak-diff.sh            (asks for the sudo password once)
#
# First, with the phone in recovery (ADB on): tools/phone.sh sync work/ios-root.img
# so the base matches the phone. Writes work/ios-root-new.img and
# work/diff.simg; flashing is left to you:
#   fastboot flash userdata work/diff.simg
#   mv work/ios-root-new.img work/ios-root.img     (new base, after the flash)
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f work/ios-root-new.img work/diff.simg
cp -c work/ios-root.img work/ios-root-new.img
sudo tools/rootimg.sh mount work/ios-root-new.img
trap 'sudo tools/rootimg.sh umount' EXIT
sudo python3 tools/iosroot-tweaks.py work/root
sudo tools/rootimg.sh umount
trap - EXIT
python3 tools/simgdiff.py work/ios-root.img work/ios-root-new.img work/diff.simg
ls -la work/diff.simg
