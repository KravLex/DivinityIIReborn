r"""Gamebryo KFM 2.2.0.0b (Divinity II): the list of animation sequences of an actor - read, add a sequence, write.

  py -3 -I kfm.py roundtrip <file.kfm>...     parse and write again: must be byte equal
  py -3 -I kfm.py dump <file.kfm>

Layout (checked on the game's files by the round trip): text line ";Gamebryo KFM File Version 2.2.0.0b\n", u8 (1 =
little endian), string model path, string model root, u32 default sync transition type, u32 default non-sync type,
f32 default sync duration, f32 default non-sync duration, u32 sequence count, sequences, then the tail (sequence
groups etc., kept as bytes). Sequence: u32 id, string .kf file, u32 animation index in that file, u32 transition
count, transitions. Transition: u32 destination id, u32 type; type 5 (default) has nothing more, any other type: f32
duration, u32 blend pair count + pairs (two strings: start key, target key, e.g. "end" / "start"), u32 chain count + entries
(u32 id, f32 duration). Strings: u32 length + text.
"""
import struct
import sys

NO_DATA = (4, 5)           # transition types without duration/blend/chain data (checked on the game's files)


class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4
        return v

    def count(self, limit=4096):
        """a list length; a misread one fails at once instead of looping for ever"""
        v = self.u32()
        if v > limit:
            raise ValueError('count %d at %d' % (v, self.p - 4))
        return v

    def f32(self):
        v = struct.unpack_from('<f', self.d, self.p)[0]; self.p += 4
        return v

    def u8(self):
        v = self.d[self.p]; self.p += 1
        return v

    def s(self):
        n = self.count(1024); v = self.d[self.p:self.p + n].decode('latin1'); self.p += n
        return v

    def raw(self, n):
        v = bytes(self.d[self.p:self.p + n]); self.p += n
        return v


class Kfm:
    def __init__(self, data):
        nl = data.index(b'\n') + 1
        self.head = bytes(data[:nl])
        r = R(data, nl)
        self.endian = r.u8()
        self.model, self.root = r.s(), r.s()
        self.sync_type, self.nonsync_type = r.u32(), r.u32()
        self.sync_dur, self.nonsync_dur = r.f32(), r.f32()
        self.seqs = []
        for _ in range(r.count()):
            q = {'id': r.u32(), 'file': r.s(), 'index': r.u32(), 'trans': []}
            for _ in range(r.count()):
                t = {'dest': r.u32(), 'type': r.u32()}
                if t['type'] not in NO_DATA:
                    t['dur'] = r.f32()
                    t['blend'] = [(r.s(), r.s()) for _ in range(r.count())]      # (start key, target key)
                    t['chain'] = [(r.u32(), r.f32()) for _ in range(r.count())]
                q['trans'].append(t)
            self.seqs.append(q)
        self.tail = bytes(data[r.p:])

    def encode(self):
        out = bytearray(self.head)

        def s(v):
            b = v.encode('latin1'); out.extend(struct.pack('<I', len(b)) + b)
        out.append(self.endian)
        s(self.model); s(self.root)
        out += struct.pack('<IIffI', self.sync_type, self.nonsync_type, self.sync_dur, self.nonsync_dur, len(self.seqs))
        for q in self.seqs:
            out += struct.pack('<I', q['id']); s(q['file']); out += struct.pack('<II', q['index'], len(q['trans']))
            for t in q['trans']:
                out += struct.pack('<II', t['dest'], t['type'])
                if t['type'] not in NO_DATA:
                    out += struct.pack('<fI', t['dur'], len(t['blend']))
                    for k1, k2 in t['blend']:
                        s(k1); s(k2)
                    out += struct.pack('<I', len(t['chain']))
                    for cid, cd in t['chain']:
                        out += struct.pack('<If', cid, cd)
        return bytes(out + self.tail)

    def add_sequence(self, seq_id, kf_file, index=0, to_type=5, from_type=5):
        """a new sequence with default transitions to every other one, and from every other one to it; seq_id None =
        the next free number after the highest (the game's ids are dense from 0; 900 made the hero vanish, 02.11)"""
        if seq_id is None:
            seq_id = max(q['id'] for q in self.seqs) + 1
        if any(q['id'] == seq_id for q in self.seqs):
            raise ValueError('sequence id %d already used' % seq_id)
        others = [q['id'] for q in self.seqs]
        for q in self.seqs:
            q['trans'].append({'dest': seq_id, 'type': from_type})
        self.seqs.append({'id': seq_id, 'file': kf_file, 'index': index,
                          'trans': [{'dest': o, 'type': to_type} for o in others]})
        return seq_id


def main(argv):
    if len(argv) >= 3 and argv[1] == 'roundtrip':
        bad = 0
        for p in argv[2:]:
            d = open(p, 'rb').read()
            try:
                ok = Kfm(d).encode() == d
            except Exception as e:
                ok = False
                print('ERROR', p, e)
            print('OK  ' if ok else 'FAIL', p)
            bad += not ok
        return 1 if bad else 0
    if len(argv) == 3 and argv[1] == 'dump':
        k = Kfm(open(argv[2], 'rb').read())
        print(k.model, k.root, k.sync_type, k.nonsync_type, k.sync_dur, k.nonsync_dur, 'tail', len(k.tail), k.tail[:40])
        for q in k.seqs:
            types = {}
            for t in q['trans']:
                types[t['type']] = types.get(t['type'], 0) + 1
            print(q['id'], q['file'], q['index'], len(q['trans']), types)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
