r"""08.11.2026: windows fade in (the user's UI idea 5).

A DoAction is added to the first frame of a window movie's main timeline (before its first ShowFrame, after the game's
own frame-1 scripts): the root starts at _alpha 0 and a helper clip "zFade" (createEmptyMovieClip at a high depth)
raises _parent._alpha by time (FADE_MS), then removes itself. The game's bytecode and its own onEnterFrame handlers
are not touched. Only the fade-in: the exe closes the windows.
08.11: by time, not by frames: the UI runs at the game's ~110 fps, 4 frames of 25 % were 40 ms (the user saw nothing).

  py -3 -I gfx_fade.py <movie.gfx in> <out>
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_hoverzoom import _push, _func2, _tag, _tags, _get, _set, R1, GETVAR, SETMEM, GETMEM, CALLM, POP  # noqa: E402

FADE_MS = 250
DEPTH = 16380
WINDOWS = ('inventory', 'equipment', 'questLog', 'skillTree', 'mainMap', 'trade', 'charms', 'achievements', 'save',
           'bt_chest', 'bt_skilltrainer', 'bt_enchant', 'bt_disenchant', 'bt_alchemistGarden', 'waypointShrine',
           'options', 'controlMapping')
ADD2, LESS2, NOT = b'\x47', b'\x48', b'\x12'
GETTIME, SUB, MUL, DIV, DUP = b'\x34', b'\x0b', b'\x0c', b'\x0d', b'\x4c'


def _if(skip):
    return b'\x9d\x02\x00' + struct.pack('<h', skip)


def _jump(skip):
    return b'\x99\x02\x00' + struct.pack('<h', skip)


def _store(reg):
    return b'\x87\x01\x00' + bytes([reg])


def script(ms=FADE_MS):
    """zFade.t0 = getTimer(); each frame a = (getTimer() - t0) * 100 / ms; a < 100: p._alpha = a, else p._alpha = 100
    and the clip removes itself"""
    R2, R3 = ('r', 2), ('r', 3)
    done = _set(R2, '_alpha', _push(100)) + _push(0, R1, 'removeMovieClip') + CALLM + POP
    step = _set(R2, '_alpha', _push(R3)) + _jump(len(done))
    body = (_get(R1, '_parent') + _store(2) + POP +
            GETTIME + _get(R1, 't0') + SUB + _push(100) + MUL + _push(ms) + DIV + _store(3) + POP +
            _push(R3, 100) + LESS2 + NOT + _if(len(step)) + step + done)
    code = (_push('this') + GETVAR + _push('_alpha', 0) + SETMEM +
            _push(DEPTH, 'zFade', 2, 'this') + GETVAR + _push('createEmptyMovieClip') + CALLM +
            DUP + _push('t0') + GETTIME + SETMEM +
            _push('onEnterFrame') + _func2(body, regs=4) + SETMEM + b'\0')
    return code


def patch(gfx, ms=FADE_MS):
    if gfx[:3] != b'CFX':
        raise SystemExit('not a compressed GFX')
    n = struct.unpack_from('<I', gfx, 4)[0]
    u = gfx[:8] + zlib.decompress(gfx[8:])[:n - 8]
    nbits = u[8] >> 3
    p = 8 + (5 + 4 * nbits + 7) // 8 + 4
    first_show = next((at for code, at, hl, ln in _tags(u, p, len(u)) if code == 1), None)
    if first_show is None:
        raise SystemExit('no ShowFrame in the main timeline')
    nu = u[:first_show] + _tag(12, script(ms)) + u[first_show:]
    nu = nu[:4] + struct.pack('<I', len(nu)) + nu[8:]
    return nu[:8] + zlib.compress(nu[8:], 9)


if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(patch(open(sys.argv[1], 'rb').read()))
