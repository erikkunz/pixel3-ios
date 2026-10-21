#!/bin/bash
# Back up what cannot be re-downloaded, from LineageOS recovery (adb root).
#
#   tools/backup-phone.sh [DIR]          (default backup/<date>)
#
# Read-only on the phone. Saves:
# - the first and last 1 MiB of every UFS LUN (sda..sdf): primary and
#   backup GPT. The iOS kernel writes to userdata on LUN 0 (sda); a sibling
#   port (ios-topaz) once had a UFS driver destroy LUN 0's GPT.
# - per-device data: persist, modem calibration and EFS (modemst1/2, fsg,
#   fsc, modemcal), devinfo, cdt, sec, misc, keystore, frp, ssd.
# Plus SHA256SUMS. Keep it off the phone; backup/ is gitignored.
#
# Restore a LUN's GPT (same recovery, or one RAM-booted with `fastboot boot`):
#   adb push lunX-head-1MiB.bin /tmp/ && adb shell dd if=/tmp/lunX-head-1MiB.bin of=/dev/block/sdX
#   (the tail goes to the last 2048 sectors; see the 'tail' line below)
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-backup/$(date +%Y%m%d-%H%M%S)}
adb devices | grep -q "recovery$" || { echo "phone not in recovery with ADB on" >&2; exit 1; }
[ "$(adb shell id -u | tr -d '\r')" = 0 ] || { echo "recovery adb shell is not root" >&2; exit 1; }
mkdir -p "$out"

for lun in sda sdb sdc sdd sde sdf; do
	sectors=$(adb shell blockdev --getsz /dev/block/$lun | tr -d '\r')
	adb exec-out "dd if=/dev/block/$lun bs=512 count=2048 2>/dev/null" > "$out/$lun-head-1MiB.bin"
	adb exec-out "dd if=/dev/block/$lun bs=512 skip=$((sectors - 2048)) count=2048 2>/dev/null" > "$out/$lun-tail-1MiB.bin"
	echo "$lun: $sectors sectors; tail = sectors $((sectors - 2048))..$((sectors - 1))" | tee -a "$out/luns.txt"
done
adb shell 'ls -l /dev/block/by-name/' | tr -d '\r' > "$out/by-name.txt"

for p in persist modemst1 modemst2 fsg fsc modemcal devinfo cdt sec misc keystore frp ssd; do
	adb exec-out "dd if=/dev/block/by-name/$p bs=1M 2>/dev/null" > "$out/$p.img"
	echo "$p: $(stat -f %z "$out/$p.img") bytes"
done

for f in "$out"/*.bin "$out"/*.img; do
	[ -s "$f" ] || { echo "EMPTY: $f" >&2; exit 1; }
done
(cd "$out" && shasum -a 256 *.bin *.img > SHA256SUMS)
echo "backup in $out ($(du -sh "$out" | cut -f1))"
