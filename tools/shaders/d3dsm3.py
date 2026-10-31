r"""Shader Model 3 helpers for Divinity II Reborn shader overrides (no Windows SDK needed).

- disassemble(code)          -> text, via the system d3dcompiler_47.dll (D3DDisassemble)
- compile(src, entry, target)-> bytecode, via D3DCompile (vs_3_0 / ps_3_0)
- ctab(code)                 -> constant table parsed from the CTAB comment block:
                                [{'name', 'regset', 'index', 'count', 'class', 'type', 'rows', 'cols'}]

CLI:
  py -3 -I d3dsm3.py dis <shader.bin>          disassembly + constant table
  py -3 -I d3dsm3.py ctab <shader.bin>
"""
import ctypes, struct, sys

REGSETS = {0: 'b', 1: 'i', 2: 'c', 3: 's'}            # D3DXRS_BOOL, INT4, FLOAT4, SAMPLER
CLASSES = {0: 'scalar', 1: 'vector', 2: 'matrix_rows', 3: 'matrix_columns', 4: 'object', 5: 'struct'}

_dll = None


def _d3dc():
    global _dll
    if _dll is None:
        _dll = ctypes.WinDLL('d3dcompiler_47.dll')
        _dll.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        _dll.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                                    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    return _dll


def _blob(ptr):
    """Returns the bytes of an ID3DBlob and releases it."""
    if not ptr:
        return b''
    vt = ctypes.cast(ctypes.cast(ptr, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    getsize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    data = ctypes.string_at(getptr(ptr), getsize(ptr))
    release(ptr)
    return data


def disassemble(code):
    out = ctypes.c_void_p()
    hr = _d3dc().D3DDisassemble(code, len(code), 0, None, ctypes.byref(out))
    if hr < 0:
        raise OSError('D3DDisassemble failed: 0x%08x' % (hr & 0xFFFFFFFF))
    return _blob(out).rstrip(b'\0').decode('latin-1')


def compile(src, entry, target, name='override.hlsl', flags=0):
    """D3DCompile; flags 0 = default optimisation. Raises with the compiler log on failure."""
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    data = src.encode('latin-1')
    hr = _d3dc().D3DCompile(data, len(data), name.encode(), None, None, entry.encode(), target.encode(),
                            flags, 0, ctypes.byref(code), ctypes.byref(err))
    log = _blob(err).rstrip(b'\0').decode('latin-1')
    if hr < 0:
        raise RuntimeError('D3DCompile %s failed:\n%s' % (target, log))
    return shader_bytes(_blob(code)), log


def shader_bytes(code):
    """Bytecode from the version token through END (0x0000FFFF), as the game passes it to Create*Shader."""
    o = 4
    while o + 4 <= len(code):
        t = struct.unpack_from('<I', code, o)[0]
        if t == 0x0000FFFF:
            return code[:o + 4]
        o += 4 + (((t >> 16) & 0x7FFF) * 4 if (t & 0xFFFF) == 0xFFFE else 0)
    return code


def ctab(code):
    o = 4
    while o + 8 <= len(code):
        t = struct.unpack_from('<I', code, o)[0]
        if (t & 0xFFFF) != 0xFFFE:
            break
        n = (t >> 16) & 0x7FFF
        body = code[o + 4:o + 4 + n * 4]
        if body[:4] == b'CTAB':
            return _parse_ctab(body[4:])
        o += 4 + n * 4
    return []


def _cstr(b, off):
    return b[off:b.index(b'\0', off)].decode('latin-1')


def _parse_ctab(b):
    size, creator, version, nconst, info, flags, target = struct.unpack_from('<7I', b, 0)
    out = []
    for i in range(nconst):
        name, regset, index, count, reserved, typeinfo, default = struct.unpack_from('<IHHHHII', b, info + 20 * i)
        cls, typ, rows, cols, elements, members, memberinfo = struct.unpack_from('<7H', b, typeinfo)
        out.append({'name': _cstr(b, name), 'regset': REGSETS.get(regset, regset), 'index': index, 'count': count,
                    'class': CLASSES.get(cls, cls), 'type': typ, 'rows': rows, 'cols': cols, 'elements': elements})
    return out


def format_ctab(table):
    return '\n'.join('  %s%-3d x%-2d %-20s %s %dx%d' % (c['regset'], c['index'], c['count'], c['name'], c['class'],
                                                     c['rows'], c['cols']) for c in sorted(table, key=lambda c: (c['regset'], c['index'])))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] in ('dis', 'ctab'):
        code = shader_bytes(open(sys.argv[2], 'rb').read())
        if sys.argv[1] == 'dis':
            print(disassemble(code))
        print('// constant table')
        print(format_ctab(ctab(code)))
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
