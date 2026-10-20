#!/usr/bin/env python3
"""Apple Device Tree (ADT): build, serialize, parse and print.

  adt.py dump FILE.adt

The flattened form iBoot hands to XNU, little endian:
  node     = u32 n_props, u32 n_children, prop[n_props], node[n_children]
  prop     = char name[32], u32 length, value (padded to 4 bytes)
Bit 31 of length marks a placeholder that iBoot would normally fill in.
"""
import struct
import sys


def u32(*v):
    return struct.pack('<%dI' % len(v), *v)


def u64(*v):
    return struct.pack('<%dQ' % len(v), *v)


def strs(*v):
    """One or more NUL-terminated strings (a 'compatible'-style list)."""
    return b''.join(s.encode() + b'\0' for s in v)


class Node:
    def __init__(self, name, props=None, children=None):
        # props: list of (name, value bytes, placeholder flag)
        self.props = [('name', strs(name), False)]
        for k, v in (props or {}).items():
            self.props.append((k, v, False))
        self.children = list(children or [])

    @property
    def name(self):
        return self.get('name').rstrip(b'\0').decode()

    def get(self, key):
        for k, v, _ in self.props:
            if k == key:
                return v
        return None

    def set(self, key, value):
        for i, (k, _, ph) in enumerate(self.props):
            if k == key:
                self.props[i] = (k, value, ph)
                return
        self.props.append((key, value, False))

    def child(self, name):
        return next((c for c in self.children if c.name == name), None)

    def walk(self, path=''):
        here = path + '/' + self.name if path or self.name != 'device-tree' else ''
        yield here or '/', self
        for c in self.children:
            yield from c.walk(here)


def serialize(node):
    out = bytearray(u32(len(node.props), len(node.children)))
    for key, val, placeholder in node.props:
        name = key.encode()
        assert len(name) < 32, key
        out += name.ljust(32, b'\0')
        out += u32(len(val) | (placeholder << 31))
        out += val + bytes(-len(val) % 4)
    for c in node.children:
        out += serialize(c)
    return bytes(out)


def parse(buf, off=0):
    """Return (root node, offset after it)."""
    n_props, n_children = struct.unpack_from('<II', buf, off)
    off += 8
    node = Node.__new__(Node)
    node.props, node.children = [], []
    for _ in range(n_props):
        key = buf[off:off + 32].split(b'\0', 1)[0].decode()
        length, = struct.unpack_from('<I', buf, off + 32)
        size = length & 0x7fffffff
        node.props.append((key, bytes(buf[off + 36:off + 36 + size]), bool(length >> 31)))
        off += 36 + size + (-size % 4)
    for _ in range(n_children):
        c, off = parse(buf, off)
        node.children.append(c)
    return node, off


def show(val):
    parts = val[:-1].split(b'\0')
    if val.endswith(b'\0') and all(p and all(32 <= b < 127 for b in p) for p in parts):
        return ' | '.join(repr(p.decode()) for p in parts)
    if len(val) == 4:
        return '0x%x' % struct.unpack('<I', val)
    if len(val) % 8 == 0 and 0 < len(val) <= 64:
        return '<' + ' '.join('0x%x' % x for x in struct.unpack('<%dQ' % (len(val) // 8), val)) + '>'
    return val[:32].hex() + ('... (%d bytes)' % len(val) if len(val) > 32 else '')


def dump(root, out=sys.stdout):
    for path, node in root.walk():
        print(path, file=out)
        for k, v, ph in node.props:
            if k != 'name':
                print('    %-28s %s%s' % (k, show(v), '  [placeholder]' if ph else ''), file=out)


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != 'dump':
        sys.exit(__doc__)
    dump(parse(open(sys.argv[2], 'rb').read())[0])
