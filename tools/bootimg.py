#!/usr/bin/env python3
"""Inspect and build Android boot images (header v0-v2), no dependencies.

  bootimg.py info  BOOT.img
  bootimg.py dtbs  BOOT.img OUT.dtb      # DTBs from the dtb section or appended to the kernel
  bootimg.py pack  -o OUT.img --kernel Image --dtb X.dtb [--like STOCK.img]
                   [--header-version 1|2] [--ramdisk RD] [--cmdline STR]

Header v1 (what the Pixel 3 shipped with) carries the DTB appended to the
kernel; header v2 adds a separate dtb section. --like copies the load
addresses, page size and OS version/patch level from a stock image so the
bootloader sees the same values it is used to.
"""
import argparse
import gzip
import hashlib
import io
import struct
import sys

MAGIC = b"ANDROID!"
FDT_MAGIC = b"\xd0\x0d\xfe\xed"
V0 = struct.Struct("<8s10I16s512s32s1024s")	# through extra_cmdline
V1_EXTRA = struct.Struct("<IQI")		# recovery_dtbo_size, recovery_dtbo_offset, header_size
V2_EXTRA = struct.Struct("<IQ")			# dtb_size, dtb_addr


def pages(n, page):
    return (n + page - 1) // page


def parse(data):
    if data[:8] != MAGIC:
        sys.exit("not an Android boot image (no ANDROID! magic)")
    f = V0.unpack_from(data, 0)
    h = dict(
        kernel_size=f[1], kernel_addr=f[2], ramdisk_size=f[3], ramdisk_addr=f[4],
        second_size=f[5], second_addr=f[6], tags_addr=f[7], page_size=f[8],
        header_version=f[9], os_version_raw=f[10],
        name=f[11].rstrip(b"\0").decode(errors="replace"),
        cmdline=(f[12].rstrip(b"\0") + f[14].rstrip(b"\0")).decode(errors="replace"),
        recovery_dtbo_size=0, dtb_size=0, dtb_addr=0,
    )
    if h["header_version"] > 2:
        sys.exit("header v%d is not handled here (Pixel 3 uses v1/v2)" % h["header_version"])
    off = V0.size
    if h["header_version"] >= 1:
        h["recovery_dtbo_size"], h["recovery_dtbo_offset"], h["header_size"] = V1_EXTRA.unpack_from(data, off)
        off += V1_EXTRA.size
    if h["header_version"] >= 2:
        h["dtb_size"], h["dtb_addr"] = V2_EXTRA.unpack_from(data, off)
    p = h["page_size"]
    pos = p
    for part in ("kernel", "ramdisk", "second", "recovery_dtbo", "dtb"):
        size = h[part + "_size"]
        h[part] = data[pos:pos + size]
        pos += pages(size, p) * p
    return h


def os_version_str(raw):
    if not raw:
        return "unset"
    ver, patch = raw >> 11, raw & 0x7ff
    return "%d.%d.%d, patch %04d-%02d" % (ver >> 14, (ver >> 7) & 0x7f, ver & 0x7f,
                                         2000 + (patch >> 4), patch & 0xf)


def find_dtbs(blob):
    """Return (offset, bytes) for each well-formed FDT found in blob."""
    out, i = [], 0
    while True:
        i = blob.find(FDT_MAGIC, i)
        if i < 0 or i + 40 > len(blob):
            return out
        total, _, _, _, _, version = struct.unpack_from(">6I", blob, i + 4)
        if 40 <= total <= len(blob) - i and 16 <= version <= 17:
            out.append((i, blob[i:i + total]))
            i += total
        else:
            i += 4


def dtb_model(dtb):
    """Best-effort model string of a DTB (first 'model' property)."""
    try:
        off_struct, off_strings = struct.unpack_from(">II", dtb, 8)
        p = off_struct
        while p < len(dtb):
            tok = struct.unpack_from(">I", dtb, p)[0]
            p += 4
            if tok == 1:
                end = dtb.index(b"\0", p)
                p = (end + 4) & ~3
            elif tok == 3:
                ln, no = struct.unpack_from(">II", dtb, p)
                name = dtb[off_strings + no:dtb.index(b"\0", off_strings + no)]
                if name == b"model":
                    return dtb[p + 8:p + 8 + ln].rstrip(b"\0").decode(errors="replace")
                p = (p + 8 + ln + 3) & ~3
            elif tok in (2, 4):
                continue
            else:
                break
    except (struct.error, ValueError):
        pass
    return "?"


def kernel_kind(k):
    if k[:2] == b"\x1f\x8b":
        return "gzip"
    if k[:4] == b"\x02\x21\x4c\x18" or k[:4] == b"\x04\x22\x4d\x18":
        return "lz4"
    if k[56:60] == b"ARM\x64":
        return "arm64 Image (uncompressed), text_offset=0x%x image_size=0x%x" % struct.unpack_from("<QQ", k, 8)
    return "unknown"


def cmd_info(a):
    h = parse(open(a.image, "rb").read())
    print("header version : %d" % h["header_version"])
    print("page size      : %d" % h["page_size"])
    print("kernel         : %d bytes at 0x%08x (%s)" % (h["kernel_size"], h["kernel_addr"], kernel_kind(h["kernel"])))
    print("ramdisk        : %d bytes at 0x%08x" % (h["ramdisk_size"], h["ramdisk_addr"]))
    print("second         : %d bytes at 0x%08x" % (h["second_size"], h["second_addr"]))
    print("tags           : 0x%08x" % h["tags_addr"])
    if h["header_version"] >= 1:
        print("recovery dtbo  : %d bytes" % h["recovery_dtbo_size"])
    if h["header_version"] >= 2:
        print("dtb section    : %d bytes at 0x%x" % (h["dtb_size"], h["dtb_addr"]))
    print("os version     : %s" % os_version_str(h["os_version_raw"]))
    print("cmdline        : %s" % h["cmdline"])
    base = guess_base(h)
    print("derived        : base 0x%x kernel_offset 0x%x ramdisk_offset 0x%x tags_offset 0x%x" % (
        base, h["kernel_addr"] - base, h["ramdisk_addr"] - base, h["tags_addr"] - base))
    src = h["dtb"] if h["dtb_size"] else h["kernel"]
    where = "dtb section" if h["dtb_size"] else "appended to kernel"
    dtbs = find_dtbs(src)
    print("DTBs           : %d (%s)" % (len(dtbs), where))
    for off, d in dtbs:
        print("   +0x%08x %7d bytes  %s" % (off, len(d), dtb_model(d)))


def guess_base(h):
    # Qualcomm convention: base 0, kernel_offset 0x8000 -> kernel_addr 0x8000.
    return h["kernel_addr"] - 0x8000 if h["kernel_addr"] >= 0x8000 else 0


def cmd_dtbs(a):
    h = parse(open(a.image, "rb").read())
    src = h["dtb"] if h["dtb_size"] else h["kernel"]
    dtbs = find_dtbs(src)
    if not dtbs:
        sys.exit("no DTBs found")
    with open(a.out, "wb") as f:
        for _, d in dtbs:
            f.write(d)
    print("wrote %d DTB(s), %d bytes, to %s" % (len(dtbs), sum(len(d) for _, d in dtbs), a.out))


def empty_ramdisk():
    """gzip'd newc cpio containing only the trailer (ABL dislikes size 0)."""
    name = b"TRAILER!!!\0"
    hdr = b"070701" + b"".join(b"%08X" % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    rec = hdr + name
    rec += b"\0" * ((4 - len(rec) % 4) % 4)
    rec += b"\0" * ((512 - len(rec) % 512) % 512)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as g:
        g.write(rec)
    return buf.getvalue()


def parse_os(version, patch):
    if not version:
        return 0
    a, b, c = (int(x) for x in (version.split(".") + ["0", "0"])[:3])
    y, m = (int(x) for x in patch.split("-")[:2])
    return ((a << 14 | b << 7 | c) << 11) | ((y - 2000) << 4 | m)


def cmd_pack(a):
    like = parse(open(a.like, "rb").read()) if a.like else None
    page = like["page_size"] if like else 4096
    kernel_addr = like["kernel_addr"] if like else 0x00008000
    ramdisk_addr = like["ramdisk_addr"] if like else 0x01000000
    second_addr = like["second_addr"] if like else 0x00f00000
    tags_addr = like["tags_addr"] if like else 0x00000100
    osv = like["os_version_raw"] if like else parse_os(a.os_version, a.os_patch_level)
    dtb_addr = 0x01f00000

    kernel = open(a.kernel, "rb").read()
    dtb = open(a.dtb, "rb").read() if a.dtb else b""
    if dtb and not find_dtbs(dtb):
        sys.exit("%s does not look like a DTB" % a.dtb)
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else empty_ramdisk()
    hv = a.header_version
    if hv == 1:
        kernel, dtb = kernel + dtb, b""		# appended DTB
    elif hv != 2:
        sys.exit("only header v1 or v2")
    cmdline = a.cmdline.encode()
    if len(cmdline) > 511 + 1023:
        sys.exit("cmdline too long")
    second = b""
    rdtbo = b""

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    sha.update(rdtbo)
    sha.update(struct.pack("<I", len(rdtbo)))
    if hv >= 2:
        sha.update(dtb)
        sha.update(struct.pack("<I", len(dtb)))
    ident = sha.digest().ljust(32, b"\0")

    hdr = V0.pack(MAGIC, len(kernel), kernel_addr, len(ramdisk), ramdisk_addr, 0, second_addr,
                  tags_addr, page, hv, osv, b"", cmdline[:512], ident, cmdline[512:])
    header_size = V0.size + V1_EXTRA.size + (V2_EXTRA.size if hv >= 2 else 0)
    hdr += V1_EXTRA.pack(0, 0, header_size)
    if hv >= 2:
        hdr += V2_EXTRA.pack(len(dtb), dtb_addr)

    def pad(b):
        return b + b"\0" * ((page - len(b) % page) % page)

    out = pad(hdr) + pad(kernel) + pad(ramdisk)
    if hv >= 2:
        out += pad(dtb)
    with open(a.out, "wb") as f:
        f.write(out)
    print("wrote %s: header v%d, kernel %d bytes%s, ramdisk %d bytes%s" % (
        a.out, hv, len(kernel), " (DTB appended)" if hv == 1 and a.dtb else "",
        len(ramdisk), ", dtb %d bytes" % len(dtb) if hv >= 2 else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("image")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("dtbs")
    p.add_argument("image")
    p.add_argument("out")
    p.set_defaults(fn=cmd_dtbs)
    p = sub.add_parser("pack")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--kernel", required=True)
    p.add_argument("--dtb")
    p.add_argument("--ramdisk")
    p.add_argument("--like", help="stock boot.img to copy addresses and OS version from")
    p.add_argument("--header-version", type=int, default=1)
    p.add_argument("--cmdline", default="console=null")
    p.add_argument("--os-version", default="12.0.0")
    p.add_argument("--os-patch-level", default="2022-10")
    p.set_defaults(fn=cmd_pack)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
