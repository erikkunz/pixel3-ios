#!/bin/bash
# Build the iOS root for the Pixel 3 on macOS: one case-sensitive, journaled
# HFS+ volume with the iPhone X OS image and both cryptexes, as a raw disk
# image for the userdata partition (kernel boot-arg rd=ufsroot0).
#
#   sudo tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]
#
# SIZE defaults to 16g. The three images come from the IPSW (BuildManifest:
# OS, Cryptex1,SystemOS, Cryptex1,AppOS); macOS mounts them natively. Runs as
# root because the copy must keep every file's owner (root, mobile, _logd,
# ...) and read 0700 directories. Afterwards run tools/iosroot-tweaks.py on
# the mounted result (this script does it if it finds it next to itself).
#
# Layout choices (what the iOS 16 userland expects, per ios-sweet's notes):
# - On a real device the cryptexes live on the preboot volume and are
#   grafted in at boot. Here they are copied to /private/preboot/Cryptexes/
#   {OS,App}, the shared cache is hard-linked to
#   /System/Library/Caches/com.apple.dyld (where dyld looks at
#   ignition_level=1), and every cryptex file missing from the OS root is
#   hard-linked to its root path.
# - Files are copied decompressed (the kernel's HFS has no decmpfs reader
#   for LZFSE/LZBITMAP): rsync reads through the APFS driver, and xattrs,
#   including com.apple.decmpfs, are not copied.
# - logd's stores and the plain system keybag are created as restore would.
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,11p' "$0"; exit 2; }
[[ $(id -u) == 0 ]] || { echo "run with sudo (owners must be preserved)" >&2; exit 1; }
os=$1 sysos=$2 appos=$3 out=$4 size=${5:-16g}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
RSYNC=$(command -v /opt/homebrew/bin/rsync || command -v rsync)
"$RSYNC" --version | sed -n 1p	# not head: SIGPIPE + pipefail

work=$(mktemp -d "$(dirname "$out")/.iosroot.XXXXXX")
devs=()
cleanup() {
	for m in "$work"/root "$work"/os "$work"/sysos "$work"/appos; do
		mount | grep -q " on $m " && umount "$m" 2>/dev/null || true
	done
	for d in ${devs[@]+"${devs[@]}"}; do hdiutil detach -quiet "$d" 2>/dev/null || true; done
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

attach_ro() {	# IMAGE MOUNTPOINT [raw]
	mkdir -p "$2"
	local key=()
	[[ ${3:-} == raw ]] && key=(-imagekey diskimage-class=CRawDiskImage)
	local dev
	dev=$(hdiutil attach -readonly -nobrowse -owners on ${key[@]+"${key[@]}"} -mountpoint "$2" "$1" | awk 'NR==1{print $1}')
	devs+=("$dev")
}
attach_ro "$os" "$work/os"
attach_ro "$sysos" "$work/sysos" raw
attach_ro "$appos" "$work/appos" raw

# The target: a raw image (byte for byte what goes on userdata).
mkfile -n "$size" "$out"
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$out" | awk 'NR==1{print $1}')
devs=("$dev" ${devs[@]+"${devs[@]}"})
newfs_hfs -s -J -v iOSRoot "${dev/disk/rdisk}"
mkdir -p "$work/root"
mount -t hfs "$dev" "$work/root"	# root's mounts honor owners
root=$work/root

copy() {
	echo "copying ${1#"$work/"} -> /${2#"$root"}"
	mkdir -p "$2"
	"$RSYNC" -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os" "$root"
copy "$work/sysos" "$root/private/preboot/Cryptexes/OS"
copy "$work/appos" "$root/private/preboot/Cryptexes/App"

dsc=System/Library/Caches/com.apple.dyld
mkdir -p "$root/$dsc"
for f in "$root/private/preboot/Cryptexes/OS/$dsc"/*; do
	ln "$f" "$root/$dsc/${f##*/}"
done

for c in OS App; do
	(cd "$root/private/preboot/Cryptexes/$c" && find . ! -type d ! -path "./$dsc/*") |
	while IFS= read -r f; do
		f=${f#./}
		[[ -e $root/$f || -L $root/$f ]] && continue
		mkdir -p "$root/$(dirname "$f")"
		src=$root/private/preboot/Cryptexes/$c/$f
		if [[ -L $src ]]; then cp -P "$src" "$root/$f"; else ln "$src" "$root/$f"; fi
		echo "linked $c:/$f"
	done
done

db=$root/private/var/db
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

install -d -o 0 -g 0 -m 0700 "$root/private/var/keybags"
cat > "$root/private/var/keybags/systembag.kb" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>KeyBagKeys</key><data>UElYRUwz</data></dict></plist>
EOF
chown 0:0 "$root/private/var/keybags/systembag.kb"
chmod 0600 "$root/private/var/keybags/systembag.kb"

if [[ -x $here/iosroot-tweaks.py ]]; then
	python3 "$here/iosroot-tweaks.py" "$root"
fi

df -h "$root"
sync
umount "$root"
fsck_hfs -fn "$dev"
echo "done: $out"
