r"""08.11.2026: skill tree hover zoom (docs/SKILLTREE_HOVER_ZOOM.md).

skillTree.gfx: the skill cell (DefineSprite 33, exported as skill_id) sets hotspot_mc.onRollOver / onRollOut in its
frame script (highlight + FSCommand skillRollOver/Out). A second DoAction is appended to that frame; it wraps the two
handlers: on roll over the cell grows to ZOOM % around its centre and is brought above its neighbours
(swapDepths(_parent.getNextHighestDepth())), then the game's handler runs; on roll out it goes back. The game's own
bytecode is not touched: only the sprite's and the file's lengths change.

  py -3 -I gfx_hoverzoom.py <skillTree.gfx in> <out>     (test: writes the patched movie)
"""
import struct
import sys
import zlib

ZOOM = 140                                   # 08.11 the user: 130 -> 140
SPRITE = 33

# ---- a tiny AVM1 assembler (SWF 8 action records) ----


def _push(*items):
    b = bytearray()
    for it in items:
        if it is None:
            b += b'\x03'                                    # undefined
        elif isinstance(it, bool):
            b += b'\x05' + bytes([1 if it else 0])
        elif isinstance(it, int):
            b += b'\x07' + struct.pack('<i', it)
        elif isinstance(it, tuple) and it[0] == 'r':
            b += b'\x04' + bytes([it[1]])
        else:
            b += b'\x00' + it.encode('latin1') + b'\0'
    return b'\x96' + struct.pack('<H', len(b)) + bytes(b)


R1, R2 = ('r', 1), ('r', 2)
GETVAR, SETVAR, GETMEM, SETMEM, CALLM, POP = b'\x1c', b'\x1d', b'\x4e', b'\x4f', b'\x52', b'\x17'
SUB, MUL, DIV, EQ2, NOT = b'\x0b', b'\x0c', b'\x0d', b'\x49', b'\x12'


def _store(reg):
    return b'\x87\x01\x00' + bytes([reg])


def _if(skip):                                              # jump forward over `skip` bytes when true
    return b'\x9d\x02\x00' + struct.pack('<h', skip)


def _func2(body, regs=3):
    """an anonymous DefineFunction2 with r1 = this (PreloadThis, SuppressArguments, SuppressSuper)"""
    head = b'\0' + struct.pack('<HB', 0, regs) + bytes([0x29, 0x00]) + struct.pack('<H', len(body))
    return b'\x8e' + struct.pack('<H', len(head)) + head + body


def _set(obj, name, value_code):
    return _push(obj, name) + value_code + SETMEM


def _get(obj, name):
    return _push(obj, name) + GETMEM


def script(zoom=ZOOM):
    """08.11: the cell grows around the centre of its hotspot (bounds of hotspot_mc in the cell's parent, taken at the
    first roll over): growing then only widens the hotspot under the mouse. Around the cell's own size the hotspot
    moved away from a mouse at its edge -> roll out -> shrink -> roll over: the cell jumped back and forth."""
    ADD = b'\x47'
    R3 = ('r', 3)
    # zx, zy = the cell's position at 100 %; zcx, zcy = the hotspot's centre in the same (parent) space
    centre = lambda key, lo, hi: (_push(R2, key) + _get(R3, lo) + _get(R3, hi) + ADD + _push(2) + DIV + SETMEM)
    save = (_set(R2, 'zx', _get(R2, '_x')) + _set(R2, 'zy', _get(R2, '_y')) +
            _get(R2, '_parent') + _push(1, R1, 'getBounds') + CALLM + _store(3) + POP +
            centre('zcx', 'xMin', 'xMax') + centre('zcy', 'yMin', 'yMax'))
    # x = c - (c - x0) * zoom / 100: the centre stays where it is
    place = lambda pos, c: (_push(R2, pos) + _get(R2, c) + _get(R2, c) + _get(R2, 'z' + pos[1]) + SUB +
                            _push(zoom) + MUL + _push(100) + DIV + SUB + SETMEM)
    first = _get(R2, 'zx') + _push(None) + EQ2 + NOT        # zx != undefined -> skip the save
    over = (_get(R1, '_parent') + _store(2) + POP +
            first + _if(len(save)) + save +
            _set(R2, '_xscale', _push(zoom)) + _set(R2, '_yscale', _push(zoom)) +
            place('_x', 'zcx') + place('_y', 'zcy') +
            _push(0) + _get(R2, '_parent') + _push('getNextHighestDepth') + CALLM +   # depth
            _push(1, R2, 'swapDepths') + CALLM + POP +
            _push(R1, 1) + _get(R2, 'zr') + _push('call') + CALLM + POP)
    back = (_set(R2, '_xscale', _push(100)) + _set(R2, '_yscale', _push(100)) +
            _set(R2, '_x', _get(R2, 'zx')) + _set(R2, '_y', _get(R2, 'zy')))
    known = _get(R2, 'zx') + _push(None) + EQ2              # zx == undefined -> skip the restore
    out = (_get(R1, '_parent') + _store(2) + POP +
           known + _if(len(back)) + back +
           _push(R1, 1) + _get(R2, 'zo') + _push('call') + CALLM + POP)
    # 09.11: with the button held the mouse leaving gives onDragOut / onReleaseOutside, not onRollOut: the cell stayed big while a skill
    # was dragged to the panel. These only shrink it, the game's handler is not called (the game sets none of them)
    shrink = _get(R1, '_parent') + _store(2) + POP + known + _if(len(back)) + back
    code = (_push('this') + GETVAR + _push('zr', 'hotspot_mc') + GETVAR + _push('onRollOver') + GETMEM + SETMEM +
            _push('this') + GETVAR + _push('zo', 'hotspot_mc') + GETVAR + _push('onRollOut') + GETMEM + SETMEM +
            _push('hotspot_mc') + GETVAR + _push('onRollOver') + _func2(over, regs=4) + SETMEM +
            _push('hotspot_mc') + GETVAR + _push('onRollOut') + _func2(out) + SETMEM +
            _push('hotspot_mc') + GETVAR + _push('onDragOut') + _func2(shrink) + SETMEM +
            _push('hotspot_mc') + GETVAR + _push('onReleaseOutside') + _func2(shrink) + SETMEM + b'\0')
    return code

# ---- SWF tags ----


def _tag(code, body):
    if len(body) < 0x3f and code not in (39,):
        return struct.pack('<H', (code << 6) | len(body)) + body
    return struct.pack('<HI', (code << 6) | 0x3f, len(body)) + body


def _tags(b, p, end):
    while p < end:
        cl = struct.unpack_from('<H', b, p)[0]
        code, ln, hl = cl >> 6, cl & 0x3f, 2
        if ln == 0x3f:
            ln, hl = struct.unpack_from('<I', b, p + 2)[0], 6
        yield code, p, hl, ln
        p += hl + ln
        if code == 0:
            return


def patch(gfx, zoom=ZOOM):
    if gfx[:3] != b'CFX':
        raise SystemExit('skillTree.gfx: not a compressed GFX')
    n = struct.unpack_from('<I', gfx, 4)[0]
    u = gfx[:8] + zlib.decompress(gfx[8:])[:n - 8]
    nbits = u[8] >> 3
    p = 8 + (5 + 4 * nbits + 7) // 8 + 4                    # RECT, frame rate, frame count
    for code, at, hl, ln in _tags(u, p, len(u)):
        if code == 39 and struct.unpack_from('<H', u, at + hl)[0] == SPRITE:
            body = u[at + hl:at + hl + ln]
            inner = list(_tags(body, 4, len(body)))
            acts = [t for t in inner if t[0] == 12]
            shows = [t for t in inner if t[0] == 1]
            if len(acts) != 1 or not shows or acts[0][1] > shows[0][1]:
                raise SystemExit('skillTree.gfx: sprite %d is not one DoAction before ShowFrame' % SPRITE)
            a = acts[0]
            src = body[a[1] + a[2]:a[1] + a[2] + a[3]]
            if b'hotspot_mc' not in src or b'onRollOver' not in src:
                raise SystemExit('skillTree.gfx: sprite %d does not set hotspot_mc.onRollOver' % SPRITE)
            cut = a[1] + a[2] + a[3]
            nb = body[:cut] + _tag(12, script(zoom)) + body[cut:]
            nu = u[:at] + _tag(39, nb) + u[at + hl + ln:]
            nu = nu[:4] + struct.pack('<I', len(nu)) + nu[8:]
            return nu[:8] + zlib.compress(nu[8:], 9)
    raise SystemExit('skillTree.gfx: no sprite %d' % SPRITE)


if __name__ == '__main__':
    out = patch(open(sys.argv[1], 'rb').read())
    open(sys.argv[2], 'wb').write(out)
    print('%d bytes' % len(out))
