r"""Read-only dump of a Scaleform .gfx (SWF with the CFX/GFX signature): tags and AVM1 ActionScript as text.

  py -3 -I gfx_avm1.py <file.gfx> [out.txt]

Tag layout and AVM1 opcodes follow the public SWF 8 specification. Only what is needed to read the scripts of the
game's UI movies is decoded: DoAction (12), DoInitAction (59), DefineSprite (39, recursive), ExportAssets (56),
ImportAssets(2) (57/71), FrameLabel (43), the GFx external image tags (1001, 1009). Constants of a ConstantPool are
substituted into Push. Branch targets are printed as absolute offsets inside the action block.
"""
import struct
import sys
import zlib

TAGS = {0: 'End', 1: 'ShowFrame', 12: 'DoAction', 26: 'PlaceObject2', 39: 'DefineSprite', 43: 'FrameLabel',
        56: 'ExportAssets', 57: 'ImportAssets', 59: 'DoInitAction', 70: 'PlaceObject3', 71: 'ImportAssets2',
        1001: 'GFX_DefineExternalImage', 1009: 'GFX_DefineSubImage'}
OPS = {0x04: 'NextFrame', 0x05: 'PrevFrame', 0x06: 'Play', 0x07: 'Stop', 0x08: 'ToggleQuality', 0x09: 'StopSounds',
       0x0A: 'Add', 0x0B: 'Subtract', 0x0C: 'Multiply', 0x0D: 'Divide', 0x0E: 'Equals', 0x0F: 'Less', 0x10: 'And',
       0x11: 'Or', 0x12: 'Not', 0x13: 'StringEquals', 0x14: 'StringLength', 0x15: 'StringExtract', 0x17: 'Pop',
       0x18: 'ToInteger', 0x1C: 'GetVariable', 0x1D: 'SetVariable', 0x20: 'SetTarget2', 0x21: 'StringAdd',
       0x22: 'GetProperty', 0x23: 'SetProperty', 0x24: 'CloneSprite', 0x25: 'RemoveSprite', 0x26: 'Trace',
       0x27: 'StartDrag', 0x28: 'EndDrag', 0x29: 'StringLess', 0x2A: 'Throw', 0x2B: 'CastOp', 0x2C: 'ImplementsOp',
       0x30: 'RandomNumber', 0x31: 'MBStringLength', 0x32: 'CharToAscii', 0x33: 'AsciiToChar', 0x34: 'GetTime',
       0x35: 'MBStringExtract', 0x36: 'MBCharToAscii', 0x37: 'MBAsciiToChar', 0x3A: 'Delete', 0x3B: 'Delete2',
       0x3C: 'DefineLocal', 0x3D: 'CallFunction', 0x3E: 'Return', 0x3F: 'Modulo', 0x40: 'NewObject',
       0x41: 'DefineLocal2', 0x42: 'InitArray', 0x43: 'InitObject', 0x44: 'TypeOf', 0x45: 'TargetPath',
       0x46: 'Enumerate', 0x47: 'Add2', 0x48: 'Less2', 0x49: 'Equals2', 0x4A: 'ToNumber', 0x4B: 'ToString',
       0x4C: 'PushDuplicate', 0x4D: 'StackSwap', 0x4E: 'GetMember', 0x4F: 'SetMember', 0x50: 'Increment',
       0x51: 'Decrement', 0x52: 'CallMethod', 0x53: 'NewMethod', 0x54: 'InstanceOf', 0x55: 'Enumerate2',
       0x60: 'BitAnd', 0x61: 'BitOr', 0x62: 'BitXor', 0x63: 'BitLShift', 0x64: 'BitRShift', 0x65: 'BitURShift',
       0x66: 'StrictEquals', 0x67: 'Greater', 0x68: 'StringGreater', 0x69: 'Extends',
       0x81: 'GotoFrame', 0x83: 'GetURL', 0x87: 'StoreRegister', 0x88: 'ConstantPool', 0x8A: 'WaitForFrame',
       0x8B: 'SetTarget', 0x8C: 'GotoLabel', 0x8D: 'WaitForFrame2', 0x8E: 'DefineFunction2', 0x8F: 'Try',
       0x94: 'With', 0x96: 'Push', 0x99: 'Jump', 0x9A: 'GetURL2', 0x9B: 'DefineFunction', 0x9D: 'If', 0x9E: 'Call',
       0x9F: 'GotoFrame2'}


def cstr(b, p):
    e = b.index(b'\0', p)
    return b[p:e].decode('latin1'), e + 1


def actions(b, start, end, out, indent, pool):
    p = start
    while p < min(end, len(b)) and (b[p] < 0x80 or p + 3 <= len(b)):   # a block may end at the tag end
        at = p
        op = b[p]; p += 1
        if op == 0:
            out.append('%s%06X End' % (indent, at))
            continue
        ln, data = 0, b''
        if op >= 0x80:
            ln = struct.unpack_from('<H', b, p)[0]; p += 2
            data = b[p:p + ln]; p += ln
        name = OPS.get(op, 'op_%02X' % op)
        arg = ''
        if op == 0x88:
            n = struct.unpack_from('<H', data, 0)[0]; q = 2; pool[:] = []
            for _ in range(n):
                s, q = cstr(data, q); pool.append(s)
            arg = '%d strings' % n
        elif op == 0x96:
            q, vals = 0, []
            while q < len(data):
                t = data[q]; q += 1
                if t == 0: s, q = cstr(data, q); vals.append(repr(s))
                elif t == 1: vals.append('%g' % struct.unpack_from('<f', data, q)[0]); q += 4
                elif t == 2: vals.append('null')
                elif t == 3: vals.append('undefined')
                elif t == 4: vals.append('r%d' % data[q]); q += 1
                elif t == 5: vals.append('true' if data[q] else 'false'); q += 1
                elif t == 6:
                    hi, lo = struct.unpack_from('<II', data, q); q += 8
                    vals.append('%g' % struct.unpack('<d', struct.pack('<II', lo, hi))[0])
                elif t == 7: vals.append(str(struct.unpack_from('<i', data, q)[0])); q += 4
                elif t == 8: i = data[q]; q += 1; vals.append(repr(pool[i]) if i < len(pool) else 'c%d' % i)
                elif t == 9:
                    i = struct.unpack_from('<H', data, q)[0]; q += 2
                    vals.append(repr(pool[i]) if i < len(pool) else 'c%d' % i)
                else: vals.append('?type%d' % t); break
            arg = ', '.join(vals)
        elif op in (0x99, 0x9D):
            arg = '-> %06X' % (p + struct.unpack_from('<h', data, 0)[0])
        elif op == 0x87:
            arg = 'r%d' % data[0]
        elif op in (0x8B, 0x8C):
            arg = repr(cstr(data, 0)[0])
        elif op == 0x83:
            a, q = cstr(data, 0); t, _ = cstr(data, q); arg = '%r %r' % (a, t)
        elif op == 0x9B:
            fname, q = cstr(data, 0)
            n = struct.unpack_from('<H', data, q)[0]; q += 2; params = []
            for _ in range(n):
                s, q = cstr(data, q); params.append(s)
            size = struct.unpack_from('<H', data, q)[0]
            out.append('%s%06X function %s(%s) {' % (indent, at, fname, ', '.join(params)))
            actions(b, p, p + size, out, indent + '    ', pool)
            out.append(indent + '}')
            p += size
            continue
        elif op == 0x8E:
            fname, q = cstr(data, 0)
            n = struct.unpack_from('<H', data, q)[0]; regs = data[q + 2]; q += 5; params = []
            for _ in range(n):
                reg = data[q]; s, q = cstr(data, q + 1); params.append('%s=r%d' % (s, reg) if reg else s)
            size = struct.unpack_from('<H', data, q)[0]
            out.append('%s%06X function2 %s(%s) regs %d {' % (indent, at, fname, ', '.join(params), regs))
            actions(b, p, p + size, out, indent + '    ', pool)
            out.append(indent + '}')
            p += size
            continue
        out.append('%s%06X %s %s' % (indent, at, name, arg))


def tags(b, p, out, depth):
    while p + 2 <= len(b):
        h = struct.unpack_from('<H', b, p)[0]; p += 2
        code, ln = h >> 6, h & 63
        if ln == 63:
            ln = struct.unpack_from('<I', b, p)[0]; p += 4
        start = p
        ind = '  ' * depth
        if code == 39:
            sid, frames = struct.unpack_from('<HH', b, p)
            out.append('%sDefineSprite id %d frames %d' % (ind, sid, frames))
            tags(b[p + 4:p + ln], 0, out, depth + 1)
        elif code in (12, 59):
            q = p
            if code == 59:
                out.append('%sDoInitAction sprite %d' % (ind, struct.unpack_from('<H', b, p)[0])); q += 2
            else:
                out.append('%sDoAction' % ind)
            try:
                actions(b, q, p + ln, out, ind + '    ', [])
            except (ValueError, struct.error, IndexError):
                out.append(ind + '    (actions not decoded)')
        elif code == 56:
            n = struct.unpack_from('<H', b, p)[0]; q = p + 2; r = []
            try:
                for _ in range(n):
                    cid = struct.unpack_from('<H', b, q)[0]; s, q = cstr(b, q + 2); r.append('%d=%s' % (cid, s))
            except ValueError:
                r.append('(truncated)')
            out.append('%sExportAssets %s' % (ind, ' '.join(r)))
        elif code in (57, 71):
            url, q = cstr(b, p)
            if code == 71: q += 2
            n = struct.unpack_from('<H', b, q)[0]; q += 2; r = []
            for _ in range(n):
                cid = struct.unpack_from('<H', b, q)[0]; s, q = cstr(b, q + 2); r.append('%d=%s' % (cid, s))
            out.append('%sImportAssets from %s: %s' % (ind, url, ' '.join(r)))
        elif code == 43:
            out.append('%sFrameLabel %s' % (ind, cstr(b, p)[0]))
        elif code == 1001:
            out.append('%sGFX_DefineExternalImage id %d %r' % (ind, struct.unpack_from('<H', b, p)[0],
                                                               b[p + 8:p + ln].split(b'\0')[0][-40:]))
        elif code == 0:
            out.append('%sEnd' % ind)
            if depth:
                return
        p = start + ln


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    d = open(argv[1], 'rb').read()
    b = zlib.decompress(d[8:]) if d[:1] == b'C' else d[8:]
    nbits = b[0] >> 3
    out = []
    tags(b, (5 + 4 * nbits + 7) // 8 + 4, out, 0)
    text = '\n'.join(out)
    if len(argv) > 2:
        open(argv[2], 'w', encoding='utf-8').write(text)
        print('%d lines -> %s' % (len(out), argv[2]))
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
