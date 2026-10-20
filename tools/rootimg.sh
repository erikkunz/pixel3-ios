#!/bin/bash
# Mount a raw iOS root image read-write on the Mac to change files in it.
#
#   sudo tools/rootimg.sh mount IMG [DIR]     (DIR default work/root)
#   sudo tools/rootimg.sh umount [DIR]
#
# Root because files keep their owners. The usual change cycle:
#   tools/phone.sh sync work/ios-root.img           (base = phone, recovery)
#   cp -c work/ios-root.img work/ios-root-new.img   (APFS clone: instant)
#   sudo tools/rootimg.sh mount work/ios-root-new.img
#   ... edit work/root/...; sudo tools/rootimg.sh umount
#   tools/simgdiff.py work/ios-root.img work/ios-root-new.img work/diff.simg
#   fastboot flash userdata work/diff.simg           (seconds)
#   mv work/ios-root-new.img work/ios-root.img       (new base)
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(id -u) == 0 ]] || { echo "run with sudo" >&2; exit 1; }

case ${1:-} in
mount)
	img=$2 dir=${3:-work/root}
	dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$img" | awk 'NR==1{print $1}')
	mkdir -p "$dir"
	mount -t hfs "$dev" "$dir"
	echo "$img on $dir ($dev)"
	;;
umount)
	dir=${2:-work/root}
	dev=$(mount | awk -v d="$(cd "$dir" && pwd)" '$3 == d {print $1}')
	[[ -n $dev ]] || { echo "$dir is not mounted" >&2; exit 1; }
	sync
	umount "$dir"
	fsck_hfs -fn "$dev" | tail -1
	hdiutil detach "$dev" >/dev/null
	echo "unmounted $dir"
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac
