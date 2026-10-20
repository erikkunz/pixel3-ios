#!/bin/bash
# Build the Pixel 3 probe and wrap it in boot images for `fastboot boot`.
#
#   ./build.sh [STOCK_BOOT.img]
#
# With a stock boot.img (e.g. LineageOS's), its DTBs and load addresses
# are reused, which is the safest match for your bootloader. Without one,
# the mainline sdm845-google-blueline.dtb in ../dtb/ is used.
#
# Needs LLVM (clang, ld.lld, llvm-objcopy, llvm-nm, llvm-readelf) and
# python3. On macOS: `brew install llvm` and run with
#   LLVM=$(brew --prefix llvm)/bin ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
LLVM=${LLVM:-}
bin() { if [ -n "$LLVM" ]; then echo "$LLVM/$1"; else echo "$1"; fi; }
CC=$(bin clang)
TARGET=--target=aarch64-none-elf
CFLAGS="$TARGET ${EXTRA_CFLAGS:-} -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector -mno-outline-atomics -Wall -Wextra"
mkdir -p out
SRCS="start.S probe.c fb.c fdt.c"
OBJS=""
for f in $SRCS; do
	o=out/${f%.*}.o
	$CC $CFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done
# The image runs wherever ABL loads it, with no relocation pass: only the
# header's image_size may be an absolute address.
abs=$($(bin llvm-readelf) -r out/probe.o out/fb.o out/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$(bin ld.lld) -T link.ld --no-pie --build-id=none -o out/probe.elf $OBJS
$(bin llvm-objcopy) -O binary out/probe.elf out/Image
# Pad the file to image_size so .bss and the stack exist as loaded zeros
# and an appended DTB starts exactly where image_size says the image ends.
size=$(( 16#$($(bin llvm-nm) out/probe.elf | awk '$3=="_end"{print $1}') ))
python3 - "$size" <<'EOF'
import sys
want = int(sys.argv[1])
with open("out/Image", "r+b") as f:
    have = f.seek(0, 2)
    if have > want:
        sys.exit("Image larger than image_size")
    f.truncate(want)
EOF

STOCK=${1:-}
if [ -n "$STOCK" ]; then
	python3 ../tools/bootimg.py dtbs "$STOCK" out/stock.dtb
	DTB=out/stock.dtb
	LIKE="--like $STOCK"
else
	DTB=../dtb/sdm845-google-blueline.dtb
	LIKE=""
fi
python3 ../tools/bootimg.py pack -o out/probe-v1.img --header-version 1 --kernel out/Image --dtb "$DTB" $LIKE
python3 ../tools/bootimg.py pack -o out/probe-v2.img --header-version 2 --kernel out/Image --dtb "$DTB" $LIKE
python3 ../tools/bootimg.py info out/probe-v1.img | sed -n '1,3p;/DTBs/,$p'
ls -l out/Image out/probe-v1.img out/probe-v2.img
