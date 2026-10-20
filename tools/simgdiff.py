#!/usr/bin/env python3
"""Sparse image holding only the blocks where NEW differs from OLD.

  simgdiff.py OLD.img NEW.img OUT.simg [--block 4096]

For `fastboot flash userdata OUT.simg` when the partition holds exactly OLD:
unchanged blocks are DONT_CARE (the partition already has them), changed
blocks are RAW (or FILL when they became all zeros). OLD must match the
phone byte for byte, so after the phone has run read-write, refresh it
first with `tools/phone.sh sync OLD.img` (LineageOS recovery).

NEW may be longer than OLD (a grown volume); it may not be shorter.
"""
import argparse
import os
import struct
import sys

RAW, FILL, DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
STEP = 256			# blocks compared per read


def diff(old_path, new_path, out_path, bs):
    old_size, new_size = os.path.getsize(old_path), os.path.getsize(new_path)
    if new_size % bs or new_size < old_size:
        sys.exit('NEW must be a multiple of %d bytes and not shorter than OLD' % bs)
    total = new_size // bs
    zero = bytes(bs)
    chunks, changed = [], 0		# (kind, nblocks, [data])

    def add(kind, data):
        if chunks and chunks[-1][0] == kind and (kind != RAW or chunks[-1][1] < 16384):
            chunks[-1][1] += 1
            if kind == RAW:
                chunks[-1][2].append(data)
        else:
            chunks.append([kind, 1, [data] if kind == RAW else []])

    with open(old_path, 'rb') as fo, open(new_path, 'rb') as fn:
        for start in range(0, total, STEP):
            a, b = fo.read(STEP * bs), fn.read(STEP * bs)
            if a == b:
                for _ in range(len(b) // bs):
                    add(DONT_CARE, None)
                continue
            for i in range(0, len(b), bs):
                nb = b[i:i + bs]
                if a[i:i + bs] == nb:
                    add(DONT_CARE, None)
                else:
                    changed += 1
                    add(FILL if nb == zero else RAW, nb)

    with open(out_path, 'wb') as out:
        out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, bs, total, len(chunks), 0))
        for kind, n, data in chunks:
            body = b''.join(data) if kind == RAW else bytes(4) if kind == FILL else b''
            out.write(struct.pack('<HHII', kind, 0, n, 12 + len(body)))
            out.write(body)
    print('%s: %d of %d blocks changed (%.1f MiB), %d chunks, %d bytes'
          % (out_path, changed, total, changed * bs / 2**20, len(chunks), os.path.getsize(out_path)))
    return changed


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('old')
    ap.add_argument('new')
    ap.add_argument('out')
    ap.add_argument('--block', type=int, default=4096)
    a = ap.parse_args()
    diff(a.old, a.new, a.out, a.block)
