#!/usr/bin/env python3
"""Raw disk image -> Android sparse image, for `fastboot flash`.

  img2simg.py RAW.img OUT.simg [--block 4096]

All-zero blocks become FILL chunks with value 0: the device writes the
zeros itself, so a mostly empty 16 GiB volume transfers only what it holds.
Never DONT_CARE: that keeps whatever the partition held before, and zero
blocks inside files (padding, zeroed pages) would then read back as the
old data. That is what broke the first flash (launchd failed its code
signature page check and crashed).

Format (libsparse): 28-byte header, then chunks with a 12-byte header:
RAW (0xCAC1) followed by its blocks, FILL (0xCAC2) followed by a 4-byte
fill value.
"""
import argparse
import os
import struct
import sys

RAW, FILL = 0xCAC1, 0xCAC2


def convert(src, dst, bs):
    size = os.path.getsize(src)
    if size % bs:
        sys.exit('%s: size is not a multiple of %d' % (src, bs))
    total = size // bs
    zero = bytes(bs)
    chunks = 0
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        out.write(bytes(28))			# header, written last
        run_kind, run_data, run_len = None, [], 0

        def flush():
            nonlocal chunks, run_data, run_len
            if not run_len:
                return
            data = b''.join(run_data) if run_kind == RAW else bytes(4)	# FILL value 0
            out.write(struct.pack('<HHII', run_kind, 0, run_len, 12 + len(data)))
            out.write(data)
            chunks += 1
            run_data, run_len = [], 0

        for _ in range(total):
            b = f.read(bs)
            kind = FILL if b == zero else RAW
            # Keep RAW chunks well below fastboot's download size.
            if kind != run_kind or (kind == RAW and run_len >= 16384):
                flush()
                run_kind = kind
            if kind == RAW:
                run_data.append(b)
            run_len += 1
        flush()
        out.seek(0)
        out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, bs, total, chunks, 0))
    print('%s: %d blocks of %d, %d chunks, %d bytes' % (dst, total, bs, chunks, os.path.getsize(dst)))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('raw')
    ap.add_argument('out')
    ap.add_argument('--block', type=int, default=4096)
    a = ap.parse_args()
    convert(a.raw, a.out, a.block)
