#!/bin/bash
# End-to-end shim test in QEMU with test/testkernel.c standing in for XNU.
#
#   test/run-qemu.sh [CMDLINE]      -> out/qemu-fb.png
#
# Green screen: every check in testkernel.c passed. Red: one white bar per
# failed check, counted from the left. The default cmdline passes;
# anything else should fail check 4 (a test of the test).
# Needs Xcode's clang (Mach-O), qemu-system-aarch64 and the shim toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
cmdline=${1:-testkernel ok}
mkdir -p out
xcrun clang -target arm64-apple-macos13 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
	-mgeneral-regs-only -nostdlib -static -Wl,-e,_start -Wl,-image_base,0xfffffe0007004000 \
	-Wl,-pagezero_size,0 -Wl,-fixup_chains -o out/testkernel test/testkernel.c
python3 ../tools/mkadt.py --qemu out/qemu.adt
python3 ../tools/mkpayload.py out/testkernel out/qemu.adt out/testpayload.bin --cmdline "$cmdline"
EXTRA_CFLAGS=-DQEMU ./build.sh out/testpayload.bin >/dev/null
rm -f out/qemu-fb.raw
# 6 GiB of RAM from 0x40000000 covers the framebuffer and the kernel RAM.
{ sleep 6; printf 'pmemsave 0x9d400000 9331200 out/qemu-fb.raw\ninfo registers\nquit\n'; } |
	qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a55 -m 6G -kernel out/Image \
		-display none -monitor stdio -serial null | grep -a ' PC=' || true
python3 - out/qemu-fb.raw out/qemu-fb.png <<'EOF'
import struct, sys, zlib
w, h = 1080, 2160
d = open(sys.argv[1], 'rb').read()
rows = []
for y in range(0, h, 2):
    r = bytearray([0])
    for x in range(0, w, 2):
        b, g, red, _ = d[(y * w + x) * 4:(y * w + x) * 4 + 4]
        r += bytes([red, g, b])
    rows.append(bytes(r))
def chunk(t, p):
    return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n'
    + chunk(b'IHDR', struct.pack('>IIBBBBB', w // 2, h // 2, 8, 2, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
px = d[(h // 4 * w + 10) * 4:(h // 4 * w + 10) * 4 + 4]
print('screen:', 'GREEN (pass)' if px[1] > 0x80 and px[2] < 0x80 else 'RED (fail)' if px[2] > 0x80 else 'other: ' + px.hex())
EOF
