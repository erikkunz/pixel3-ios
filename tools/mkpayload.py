#!/usr/bin/env python3
"""Pack an XNU kernel, an ADT and boot-args into the shim's payload.

  mkpayload.py KERNEL ADT OUT [--cmdline "..."] [--ramdisk IMAGE]

The shim acts as iBoot: it copies the kernel's segments to RAM, puts the
ADT and boot_args behind them and jumps to the entry point. Everything
that needs a Mach-O parser happens here instead:

- The kernel is linked with chained fixups (LC_DYLD_CHAINED_FIXUPS). With
  no kernel collection loader in the way, nobody would apply them, so
  every chained pointer is rewritten here to its plain, unslid target.
- Segments become (vmaddr, vmsize, data) records; the shim zero-fills
  vmsize beyond the data.
- The RAM the kernel gets (base, size) is read from the ADT's /chosen
  dram-base and dram-size, so mkadt.py is the single source for it.

--ramdisk adds an image right after the kernel and records it in
/chosen/memory-map as "RAMDisk" (root device md0, boot-arg rd=md0).

Payload layout (little endian):
  0x000  char magic[8] "PIXEL3PL"
  0x008  u32  version (1), u32 nsegs
  0x010  u64  virt_base     VA that corresponds to dram_base
  0x018  u64  entry         entry point VA (LC_UNIXTHREAD pc)
  0x020  u64  dram_base, dram_size
  0x030  u64  kernel_end    VA just past the last segment (16K aligned)
  0x038  u64  adt_off, adt_size   ADT bytes inside the payload
  0x048  char cmdline[608]
  0x2a8  seg[nsegs]: u64 vmaddr, vmsize, data_off, data_size
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

MAGIC = b'PIXEL3PL'
HDR = struct.Struct('<8sII7Q608s')
SEG = struct.Struct('<4Q')
assert HDR.size == 0x2a8

PAGE = 0x4000		# XNU arm64 page size
L2_BLOCK = 0x2000000	# start.s maps the kernel with 32 MiB L2 blocks

LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_DYLD_CHAINED_FIXUPS = 0x80000034


class MachO:
    def __init__(self, data):
        self.data = data
        magic, self.cputype, _, self.filetype, ncmds = struct.unpack_from('<5I', data)
        if magic != 0xfeedfacf:
            sys.exit('kernel: not a 64-bit Mach-O')
        self.segments, self.entry, self.fixups = [], None, None
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, off)
            if cmd == LC_SEGMENT_64:
                name = data[off + 8:off + 24].split(b'\0', 1)[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, off + 24)
                self.segments.append((name, vmaddr, vmsize, fileoff, filesize))
            elif cmd == LC_UNIXTHREAD:
                # flavor, count, then arm_thread_state64: x0-x28, fp, lr, sp, pc
                self.entry = struct.unpack_from('<Q', data, off + 16 + 32 * 8)[0]
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                self.fixups = struct.unpack_from('<I', data, off + 8)[0]
            off += size
        if self.entry is None:
            sys.exit('kernel: no LC_UNIXTHREAD')

    def file_offset(self, vmaddr):
        for _, va, _, fo, fs in self.segments:
            if va <= vmaddr < va + fs:
                return vmaddr - va + fo
        raise ValueError('VA 0x%x not backed by file data' % vmaddr)


def resolve_fixups(m):
    """Rewrite every chained rebase in m.data in place; return the count."""
    if m.fixups is None:
        return 0
    d = m.data
    hdr = m.fixups
    _, starts_off, _, _, n_imports, _, _ = struct.unpack_from('<7I', d, hdr)
    if n_imports:
        sys.exit('kernel: %d chained imports (binds); only rebases can be resolved' % n_imports)
    image = hdr + starts_off
    base_va = m.segments[0][1] if m.segments[0][0] != '__PAGEZERO' else m.segments[1][1]
    n_segs, = struct.unpack_from('<I', d, image)
    count = 0
    for i in range(n_segs):
        info, = struct.unpack_from('<I', d, image + 4 + 4 * i)
        if not info:
            continue
        s = image + info
        _, page_size, fmt, seg_offset, _, n_pages = struct.unpack_from('<IHHQIH', d, s)
        for page in range(n_pages):
            start, = struct.unpack_from('<H', d, s + 22 + 2 * page)
            if start == 0xffff:			# no fixups on this page
                continue
            if start & 0x8000:
                sys.exit('kernel: DYLD_CHAINED_PTR_START_MULTI is not handled')
            loc = m.file_offset(base_va + seg_offset + page * page_size + start)
            while True:
                raw, = struct.unpack_from('<Q', d, loc)
                target, nxt = decode(raw, fmt, base_va)
                struct.pack_into('<Q', d, loc, target)
                count += 1
                if not nxt:
                    break
                loc += 4 * nxt
    return count


def decode(raw, fmt, base_va):
    """Plain target VA and next-link (in 4-byte units) of one chained rebase."""
    if fmt in (2, 6):		# DYLD_CHAINED_PTR_64 (VA) / _64_OFFSET (offset from base)
        if raw >> 63:
            sys.exit('kernel: chained bind found')
        target = raw & (1 << 36) - 1
        if fmt == 6:
            target += base_va
        return target | (raw >> 36 & 0xff) << 56, raw >> 51 & 0xfff
    if fmt == 8:		# DYLD_CHAINED_PTR_64_KERNEL_CACHE (offset from base)
        if raw >> 63:
            sys.exit('kernel: authenticated pointer; arm64e kernels are not supported')
        return base_va + (raw & (1 << 30) - 1), raw >> 51 & 0xfff
    sys.exit('kernel: chained pointer format %d is not supported (plain arm64 only)' % fmt)


def chosen_dram(root):
    chosen = root.child('chosen')
    return (struct.unpack('<Q', chosen.get('dram-base'))[0],
            struct.unpack('<Q', chosen.get('dram-size'))[0])


def build(kernel, adt_blob, cmdline, ramdisk=None):
    m = MachO(bytearray(kernel))
    if m.cputype != 0x0100000c:
        sys.exit('kernel: not arm64 (cputype 0x%x)' % m.cputype)
    nfix = resolve_fixups(m)
    segs = [s for s in m.segments if s[2] and s[0] != '__PAGEZERO']
    text = segs[0]
    if text[0] != '__TEXT' or text[3] != 0:
        sys.exit('kernel: first segment is not __TEXT at file offset 0')

    # start.s maps from the 32 MiB block holding the Mach-O header up;
    # dram_base must be the start of that block.
    virt_base = text[1] & ~(L2_BLOCK - 1)
    kernel_end = -(-max(va + vs for _, va, vs, _, _ in segs) // PAGE) * PAGE

    root, _ = adt.parse(adt_blob)
    dram_base, dram_size = chosen_dram(root)
    if dram_base % L2_BLOCK or dram_size % L2_BLOCK:
        sys.exit('ADT dram-base/size must be 32 MiB aligned')

    pieces = [(va, vs, bytes(m.data[fo:fo + fs])) for _, va, vs, fo, fs in segs]
    rd = None
    if ramdisk:
        size = -(-len(ramdisk) // PAGE) * PAGE
        rd = (kernel_end - virt_base + dram_base, size)
        pieces.append((kernel_end, size, ramdisk))
        kernel_end += size
        mmap = root.child('chosen').child('memory-map')
        if mmap.get('RAMDisk') is not None:
            sys.exit('ADT already has a RAMDisk')
        mmap.set('RAMDisk', adt.u64(*rd))
        adt_blob = adt.serialize(root)

    # The ADT and boot_args (one page) go right after kernel_end; all of it
    # must fit in the kernel's RAM with room to spare for the VM.
    used = kernel_end - virt_base + len(adt_blob) + 2 * PAGE
    if used > dram_size // 2:
        sys.exit('payload needs 0x%x of 0x%x bytes of kernel RAM' % (used, dram_size))

    out = bytearray(HDR.size + SEG.size * len(pieces))
    table = []
    for va, vs, data in pieces:
        out += bytes(-len(out) % 16)
        table.append((va, vs, len(out), len(data)))
        out += data
    out += bytes(-len(out) % 16)
    adt_off = len(out)
    out += adt_blob
    out += bytes(-len(out) % 16)

    cl = cmdline.encode()
    if len(cl) >= 608:
        sys.exit('cmdline longer than 607 bytes')
    HDR.pack_into(out, 0, MAGIC, 1, len(pieces), virt_base, m.entry, dram_base, dram_size,
                  kernel_end, adt_off, len(adt_blob), cl)
    for i, t in enumerate(table):
        SEG.pack_into(out, HDR.size + SEG.size * i, *t)

    report = [
        'fixups      %d' % nfix,
        'virt_base   0x%x -> dram 0x%x size 0x%x' % (virt_base, dram_base, dram_size),
        'entry       0x%x (pa 0x%x)' % (m.entry, m.entry - virt_base + dram_base),
        'kernel_end  0x%x (pa 0x%x)' % (kernel_end, kernel_end - virt_base + dram_base),
        'adt         %d bytes' % len(adt_blob),
    ]
    if rd:
        report.append('ramdisk     pa 0x%x size 0x%x' % rd)
    report += ['  %-16s 0x%x +0x%x' % (n, va, vs) for n, va, vs, _, _ in segs]
    report.append('payload     %d bytes' % len(out))
    return bytes(out), report


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Pixel 3 shim payload packer')
    ap.add_argument('kernel')
    ap.add_argument('adt')
    ap.add_argument('out')
    ap.add_argument('--cmdline', default='debug=0xc serial=3 cpus=1 keepsyms=1')
    ap.add_argument('--ramdisk', help='root image for md0 (boot with rd=md0)')
    a = ap.parse_args()
    blob, report = build(open(a.kernel, 'rb').read(), open(a.adt, 'rb').read(), a.cmdline,
                         open(a.ramdisk, 'rb').read() if a.ramdisk else None)
    open(a.out, 'wb').write(blob)
    print('\n'.join(report))
