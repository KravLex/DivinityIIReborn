r"""Divinity II binary XML (Gamebryo NIF 20.3.0.9 with one xml::dom::CStreamableNode block): decode AND encode.

  py -3 -I d2xml.py roundtrip <file> [...]      decode + encode every file, compare byte for byte

Layout (decoder: tools/reborn/mindread/d2dom.py, reversed from CStreamableNode::LoadBinary @0x658CB0):
  NIF header ... u32 block_size[1] ... | block: u32 nodeCount, u32 attrCount, u32 poolSize, pool, node records | footer
  node record: u8 flags, u32 nameHash; flags&4 text, flags&2 attrs (count, count*u32 hash), flags&1 children (count);
  flags&8: both counts are u8. Records are in stack order (children pushed in index order, the last child is read
  next). Pool strings (NUL-terminated UTF-8) in record order: the node's text, then its attribute values.
The encoder rewrites only the block and its size in the header; everything else is copied from the source file.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'reborn', 'mindread'))
from d2dom import name_hash as _hash  # noqa: E402


class Raw(str):
    """A name known only by its hash (shown as H_xxxxxxxx); encodes back to the same hash."""
    def __new__(cls, h):
        s = str.__new__(cls, 'H_%08X' % h)
        s.h = h
        return s


def name_hash(s):
    return s.h if isinstance(s, Raw) else _hash(s)


class Node:
    __slots__ = ('name', 'h', 'text', 'attrs', 'children', 'small')

    def __init__(self, name=None, attrs=None, text=None, h=None):
        self.name, self.text = name, text
        self.h = name_hash(name) if h is None and name else h
        self.attrs = list(attrs or [])            # [(name, value)] in file order
        self.children = []
        self.small = None                         # flags&8 as read; None = choose when encoding

    def get(self, k, default=None):
        for a, v in self.attrs:
            if a == k:
                return v
        return default

    def set(self, k, v):
        for i, (a, _) in enumerate(self.attrs):
            if a == k:
                self.attrs[i] = (a, v)                # keep the key as read (a hash-only name encodes back)
                return
        self.attrs.append((k, v))

    def find(self, name):
        return next((c for c in self.children if c.name == name), None)


class Doc:
    def __init__(self, data, names):
        self.names = names
        p = data.index(b'\n') + 1 + 4 + 1 + 4
        nb, = struct.unpack_from('<I', data, p); p += 4
        nt, = struct.unpack_from('<H', data, p); p += 2
        for _ in range(nt):
            ln, = struct.unpack_from('<I', data, p); p += 4 + ln
        if nb != 1:
            raise ValueError('expected one block, got %d' % nb)
        p += 2 * nb
        self.size_at = p
        size, = struct.unpack_from('<I', data, p); p += 4
        ns, = struct.unpack_from('<I', data, p); p += 8
        for _ in range(ns):
            ln, = struct.unpack_from('<I', data, p); p += 4 + ln
        ng, = struct.unpack_from('<I', data, p); p += 4 + 4 * ng
        self.head = data[:p]
        self.foot = data[p + size:]
        self.root = self._parse(data[p:p + size])

    def _parse(self, b):
        nn, na, psz = struct.unpack_from('<III', b, 0)
        pool = b[12:12 + psz]
        p, pp = 12 + psz, 0

        def nxt():
            nonlocal pp
            e = pool.index(b'\0', pp)
            s = pool[pp:e].decode('utf-8')
            pp = e + 1
            return s
        root = Node()
        stack, seen_n, seen_a = [root], 0, 0
        while stack:
            n = stack.pop()
            seen_n += 1
            fl = b[p]
            n.h, = struct.unpack_from('<I', b, p + 1); p += 5
            n.name = self.names.get(n.h) or Raw(n.h)
            n.small = bool(fl & 8)
            if fl & 4:
                n.text = nxt()
            hs = ()
            if fl & 2:
                if fl & 8:
                    c = b[p]; p += 1
                else:
                    c, = struct.unpack_from('<I', b, p); p += 4
                hs = struct.unpack_from('<%dI' % c, b, p); p += 4 * c
            if fl & 1:
                if fl & 8:
                    c = b[p]; p += 1
                else:
                    c, = struct.unpack_from('<I', b, p); p += 4
                n.children = [Node() for _ in range(c)]
                stack.extend(n.children)
            n.attrs = [(self.names.get(h) or Raw(h), nxt()) for h in hs]
            seen_a += len(hs)
        if (seen_n, seen_a, pp, p) != (nn, na, psz, len(b)):
            raise ValueError('block mismatch: nodes %d/%d attrs %d/%d pool %d/%d end %d/%d'
                             % (seen_n, nn, seen_a, na, pp, psz, p, len(b)))
        return root

    def encode(self):
        recs, pool, nn, na = bytearray(), bytearray(), 0, 0
        stack = [self.root]
        while stack:
            n = stack.pop()
            nn += 1
            na += len(n.attrs)
            small = n.small if n.small is not None else (len(n.attrs) < 256 and len(n.children) < 256)
            fl = (4 if n.text is not None else 0) | (2 if n.attrs else 0) | (1 if n.children else 0) | (8 if small else 0)
            h = n.h if n.h is not None else name_hash(n.name)
            recs += struct.pack('<BI', fl, h)
            if n.text is not None:
                pool += n.text.encode('utf-8') + b'\0'
            if n.attrs:
                recs += struct.pack('<B' if small else '<I', len(n.attrs))
                recs += b''.join(struct.pack('<I', name_hash(k)) for k, _ in n.attrs)
            if n.children:
                recs += struct.pack('<B' if small else '<I', len(n.children))
                stack.extend(n.children)
            for _, v in n.attrs:
                pool += v.encode('utf-8') + b'\0'
        block = struct.pack('<III', nn, na, len(pool)) + bytes(pool) + bytes(recs)
        head = bytearray(self.head)
        struct.pack_into('<I', head, self.size_at, len(block))
        return bytes(head) + block + self.foot


def main(argv):
    if len(argv) >= 3 and argv[1] == 'roundtrip':
        bad = 0
        for path in argv[2:]:
            data = open(path, 'rb').read()
            out = Doc(data, {}).encode()
            ok = out == data
            bad += not ok
            print('%-4s %s' % ('OK' if ok else 'DIFF', path))
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
