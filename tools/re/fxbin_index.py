r"""Index a Divinity II compiled shader library (*.fxbin).

Container layout (verified on CompiledShaders.fxbin and CompiledSpeedTreeShaders.fxbin, Steam v1.4.700.56):
  u32 len, char[len] title ("Compiled shader library\0")
  u8  1, u32 len, char[len] platform ("WIN32\0")
  main library:      u32 1, u32 count, count x { u32 len, char[len] source path, u32 size, u8[size] effect }
  speedtree library: u32 count,        count x { u32 size, u8[size] effect }
  effect = D3DX9 effect binary (fx_2_0, magic 0xFEFF0901)

For every effect it lists the embedded vs/ps bytecode blobs (found by version token + END token)
with their SHA-1, so a runtime layer can recognise which engine pass is being drawn.

Usage: py -3 -I fxbin_index.py <lib.fxbin> [--json out.json] [--dump DIR]
"""
import hashlib, json, ntpath, os, re, struct, sys

FX_MAGIC = 0xFEFF0901
VER = re.compile(rb'[\x00-\x03][\x01-\x03][\xfe\xff]\xff')

def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]

def read_str(b, o):
    n = u32(b, o)
    return b[o + 4:o + 4 + n].rstrip(b'\0').decode('latin-1'), o + 4 + n

def parse_library(b):
    title, o = read_str(b, 0)
    if not title.startswith('Compiled'):
        raise ValueError('not a compiled shader library: %r' % title)
    if b[o] != 1:
        raise ValueError('unexpected byte after title: %d' % b[o])
    platform, o = read_str(b, o + 1)
    # SpeedTree library: u32 count, then { u32 size, effect } without source paths.
    named = not (o + 12 <= len(b) and u32(b, o + 8) == FX_MAGIC)
    if named:
        one, count = u32(b, o), u32(b, o + 4)
        o += 8
    else:
        one, count = None, u32(b, o)
        o += 4
    effects = []
    for i in range(count):
        if named:
            path, o = read_str(b, o)
        else:
            path = '%s#%d.fx' % (title.split()[1], i)
        size = u32(b, o)
        blob = b[o + 4:o + 4 + size]
        if len(blob) != size or u32(blob, 0) != FX_MAGIC:
            raise ValueError('bad effect blob for %s at 0x%x' % (path, o))
        effects.append((path, blob))
        o += 4 + size
    if o != len(b):
        raise ValueError('trailing %d bytes after %d effects' % (len(b) - o, count))
    return title, platform, one, effects

def shaders_in(blob):
    """D3D9 shader bytecode: version token, tokens..., END 0x0000FFFF. Comment tokens are skipped by length."""
    out, pos = [], 0
    while True:
        m = VER.search(blob, pos)
        if not m:
            break
        start = m.start()
        if start % 4:
            pos = start + 1
            continue
        o = start + 4
        ok = False
        while o + 4 <= len(blob):
            t = u32(blob, o)
            if t == 0x0000FFFF:
                ok = True
                o += 4
                break
            if (t & 0xFFFF) == 0xFFFE:          # comment: length in DWORDs in bits 16..30
                o += 4 + ((t >> 16) & 0x7FFF) * 4
            else:
                o += 4
        if not ok:
            pos = start + 4
            continue
        code = blob[start:o]
        v = u32(code, 0)
        kind = 'vs' if (v >> 16) == 0xFFFE else 'ps'
        out.append({'offset': start, 'size': len(code), 'model': '%s_%d_%d' % (kind, (v >> 8) & 0xFF, v & 0xFF),
                    'sha1': hashlib.sha1(code).hexdigest()})
        pos = o
    return out

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    lib = open(argv[1], 'rb').read()
    title, platform, _, effects = parse_library(lib)
    report = {'library': os.path.basename(argv[1]), 'platform': platform, 'effects': []}
    dump = argv[argv.index('--dump') + 1] if '--dump' in argv else None
    total = 0
    for path, blob in effects:
        name = ntpath.basename(path)
        sh = shaders_in(blob)
        total += len(sh)
        report['effects'].append({'source': path, 'name': name, 'size': len(blob),
                                  'sha1': hashlib.sha1(blob).hexdigest(), 'shaders': sh})
        print('%-44s %7d B  %3d shaders' % (name, len(blob), len(sh)))
        if dump:
            os.makedirs(dump, exist_ok=True)
            open(os.path.join(dump, name + 'o'), 'wb').write(blob)
    print('== %s: %d effects, %d shader blobs' % (platform, len(effects), total))
    if '--json' in argv:
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
