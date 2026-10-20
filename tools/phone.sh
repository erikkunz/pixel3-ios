#!/bin/bash
# Talk to the phone's userdata from LineageOS recovery (adb root shell).
#
#   tools/phone.sh log [OUT.txt]      kernel console log from the last boot
#   tools/phone.sh sync BASE.img      make BASE.img match the phone exactly
#
# Get there: phone in fastboot -> `fastboot reboot recovery` -> in the
# recovery menu, Advanced -> Enable ADB. Nothing here writes to the phone.
#
# log:  the iOS kernel keeps its console in a ring in the last 4100 KiB of
#       userdata (header block "SWEETLOG", then 4 MiB), flushed about once
#       a second; tools/phonelog.py turns it into text.
# sync: hashes the phone's first size(BASE) bytes of userdata in 4 MiB
#       pieces, compares with BASE and copies only the differing pieces
#       into BASE. Afterwards BASE is a correct OLD for tools/simgdiff.py.
set -euo pipefail
cd "$(dirname "$0")/.."
PART=/dev/block/by-name/userdata
CHUNK=$((4 << 20))

need_recovery() {
	adb devices | grep -q "recovery$" || {
		echo "phone is not in recovery with ADB on (fastboot reboot recovery; Advanced -> Enable ADB)" >&2
		exit 1
	}
	[ "$(adb shell id -u | tr -d '\r')" = 0 ] || { echo "recovery adb shell is not root" >&2; exit 1; }
}

part_size() {
	adb shell blockdev --getsize64 "$PART" | tr -d '\r'
}

cmd_log() {
	local out=${1:-work/phone-log.txt}
	need_recovery
	local size skip
	size=$(part_size)
	skip=$((size / 4096 - 1025))
	mkdir -p work
	adb exec-out "dd if=$PART bs=4096 skip=$skip count=1025 2>/dev/null" > work/phone-log.bin
	python3 tools/phonelog.py work/phone-log.bin > "$out"
	echo "$out: $(wc -l < "$out") lines"
}

cmd_sync() {
	local base=$1
	need_recovery
	local bytes n
	bytes=$(stat -f %z "$base")
	n=$(( (bytes + CHUNK - 1) / CHUNK ))
	[ "$(part_size)" -ge "$bytes" ] || { echo "userdata is smaller than $base" >&2; exit 1; }
	echo "hashing $n x 4 MiB on the phone and here..."
	adb shell "i=0; while [ \$i -lt $n ]; do dd if=$PART bs=$CHUNK skip=\$i count=1 2>/dev/null | md5sum; i=\$((i+1)); done" |
		tr -d '\r' | awk '{print $1}' > work/phone.md5 &
	python3 - "$base" "$CHUNK" > work/local.md5 <<'EOF'
import hashlib, sys
with open(sys.argv[1], 'rb') as f:
    while True:
        b = f.read(int(sys.argv[2]))
        if not b:
            break
        print(hashlib.md5(b).hexdigest())
EOF
	wait
	[ "$(wc -l < work/phone.md5)" -eq "$n" ] || { echo "phone hash list incomplete" >&2; exit 1; }
	local diffs
	diffs=$(paste work/local.md5 work/phone.md5 | awk '$1 != $2 {print NR - 1}')
	echo "$(echo "$diffs" | grep -c . || true) of $n pieces differ"
	for i in $diffs; do
		adb exec-out "dd if=$PART bs=$CHUNK skip=$i count=1 2>/dev/null" > work/piece.bin
		python3 - "$base" "$i" "$CHUNK" work/piece.bin <<'EOF'
import sys
base, i, chunk, piece = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = open(piece, 'rb').read()
with open(base, 'r+b') as f:
    f.seek(i * chunk)
    want = min(chunk, f.seek(0, 2) - i * chunk)
    if len(data) < want:
        sys.exit('short read of piece %d' % i)
    f.seek(i * chunk)
    f.write(data[:want])
EOF
	done
	rm -f work/piece.bin
	echo "$base now matches the phone"
}

case ${1:-} in
log) shift; cmd_log "$@" ;;
sync) [ $# -eq 2 ] || { sed -n '2,6p' "$0"; exit 2; }; cmd_sync "$2" ;;
*) sed -n '2,16p' "$0"; exit 2 ;;
esac
