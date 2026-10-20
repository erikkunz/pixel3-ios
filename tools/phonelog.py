#!/usr/bin/env python3
"""Decode the iOS kernel's persistent console ring (from tools/phone.sh log).

  phonelog.py RING.bin > log.txt

RING.bin is the last 4100 KiB of userdata: one header block
  "SWEETLOG" | u32 version | u32 ring size | u64 total bytes | u64 flush seq
then the ring; byte n of the console is at ring offset n % size.
"""
import struct
import sys

BLOCK = 4096

data = open(sys.argv[1], 'rb').read()
magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', data, 0)
if magic != b'SWEETLOG':
    sys.exit('no SWEETLOG header (no read-write boot since the last flash?)')
ring = data[BLOCK:BLOCK + size]
if total <= size:
    text = ring[:total]
else:
    cut = total % size
    text = ring[cut:] + ring[:cut]
sys.stderr.write('version %d, ring %d bytes, %d bytes logged, flush #%d%s\n'
                 % (version, size, total, seq, ', oldest lines lost' if total > size else ''))
sys.stdout.write(text.decode('utf-8', 'replace'))
