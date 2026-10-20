# Shared build steps for the Pixel 3 bare-metal programs; source it from
# a program directory, then call build_image.
#
# Needs LLVM (clang, ld.lld, llvm-objcopy, llvm-nm, llvm-readelf) and
# python3. On macOS: `brew install llvm lld` and run with
#   LLVM=$(brew --prefix llvm)/bin ./build.sh
# Tools missing from $LLVM come from PATH (Homebrew ships ld.lld in its
# own `lld` formula).
LLVM=${LLVM:-}
COMMON=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bin() { if [ -n "$LLVM" ] && [ -x "$LLVM/$1" ]; then echo "$LLVM/$1"; else echo "$1"; fi; }
CC=$(bin clang)
CFLAGS="--target=aarch64-none-elf ${EXTRA_CFLAGS:-} -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector -mno-outline-atomics -Wall -Wextra -I$COMMON"

# build_image NAME "SOURCES" [STOCK_BOOT.img]
#   -> out/Image and out/NAME.img (boot header v1, gzip'd Image + DTB)
# With a stock boot.img (e.g. LineageOS's), its DTBs and load addresses
# are reused. Without one, the mainline DTB in dtb/ is used.
build_image() {
	local name=$1 srcs=$2 stock=${3:-}
	mkdir -p out
	local objs="" cobjs="" f o
	for f in "$COMMON/start.S" "$COMMON/board.c" "$COMMON/fb.c" "$COMMON/fdt.c" $srcs; do
		o=out/$(basename "${f%.*}").o
		$CC $CFLAGS -c "$f" -o "$o"
		objs="$objs $o"
		case $f in *.c) cobjs="$cobjs $o" ;; esac
	done
	# The image runs wherever ABL loads it, with no relocation pass: only the
	# header's image_size may be an absolute address.
	local abs
	abs=$($(bin llvm-readelf) -r $cobjs | grep -c ABS64 || true)
	[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; return 1; }
	$(bin ld.lld) -T "$COMMON/link.ld" --no-pie --build-id=none -o "out/$name.elf" $objs
	$(bin llvm-objcopy) -O binary "out/$name.elf" out/Image
	# Pad the file to image_size so .bss and the stack exist as loaded zeros
	# and an appended DTB starts exactly where image_size says the image ends.
	local size
	size=$(( 16#$($(bin llvm-nm) "out/$name.elf" | awk '$3=="_end"{print $1}') ))
	python3 - "$size" <<'EOF'
import sys
want = int(sys.argv[1])
with open("out/Image", "r+b") as f:
    have = f.seek(0, 2)
    if have > want:
        sys.exit("Image larger than image_size")
    f.truncate(want)
EOF
	local dtb like=""
	if [ -n "$stock" ]; then
		python3 "$COMMON/../tools/bootimg.py" dtbs "$stock" out/stock.dtb
		dtb=out/stock.dtb
		like="--like $stock"
	else
		dtb=$COMMON/../dtb/sdm845-google-blueline.dtb
	fi
	# The Pixel 3 bootloader rejects an uncompressed Image ("Error verifying
	# the received boot.img: Unsupported"); a gzip'd one with the DTB
	# appended (header v1, like Image.gz-dtb) boots.
	gzip -9 -n -c out/Image > out/Image.gz
	python3 "$COMMON/../tools/bootimg.py" pack -o "out/$name.img" --header-version 1 \
		--kernel out/Image.gz --dtb "$dtb" $like
	python3 "$COMMON/../tools/bootimg.py" info "out/$name.img" | sed -n '1,3p;/DTBs/,$p'
	ls -l out/Image "out/$name.img"
}
