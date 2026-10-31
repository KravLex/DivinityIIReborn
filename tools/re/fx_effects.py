r"""Map every shader of Divinity II's compiled shader libraries to its effect, technique and pass.

Usage:
  py -3 -I fx_effects.py <CompiledShaders.fxbin> [<CompiledSpeedTreeShaders.fxbin> ...] [--json out.json] [--table out.txt]

Parses the D3DX9 effect binary (fx_2_0, magic 0xFEFF0901) the way Wine's d3dx9 effect loader does:
  u32 tag, u32 start; data = blob + 8; stream = data + start:
  u32 parameters, u32 techniques, u32 unknown, u32 objects
  parameter  = { u32 type_off, u32 value_off, u32 flags, u32 annotations, annotations x {u32, u32} }
  technique  = { u32 name_off, u32 annotations, u32 passes, annotations x {u32, u32}, passes }
  pass       = { u32 name_off, u32 annotations, u32 states, annotations x {u32, u32}, states x {u32 op, u32 index, u32 type_off, u32 value_off} }
  u32 strings, u32 resources
  string     = { u32 object_id, u32 size, bytes (aligned to 4) }
  resource   = { u32 technique, u32 pass, u32 element, u32 state, u32 usage, u32 size, bytes (aligned to 4) }
Names live in the data area: u32 length + chars. A resource whose bytes are D3D9 shader bytecode is the
shader bound by that pass state. The SHA-1 is over the bytecode exactly as the game passes it to
IDirect3DDevice9::Create{Vertex,Pixel}Shader (version token .. END token), which is what DXVK hashes.

--table writes "sha1 vs|ps effect technique pass" lines for the Reborn DXVK frame census (d3d9.shaderNames).
"""
import hashlib, json, ntpath, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fxbin_index import FX_MAGIC, parse_library  # noqa: E402


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


class Effect:
    def __init__(self, blob):
        if u32(blob, 0) != FX_MAGIC:
            raise ValueError('not an fx_2_0 effect')
        self.blob = blob
        self.data = 8
        self.p = self.data + u32(blob, 4)
        self.codes = {}                        # sha1 -> shader bytecode, filled by parse()

    def rd(self):
        v = u32(self.blob, self.p)
        self.p += 4
        return v

    def name(self, off):
        o = self.data + off
        n = u32(self.blob, o)
        return self.blob[o + 4:o + 4 + n].rstrip(b'\0').decode('latin-1')

    def skip_annotations(self, n):
        self.p += 8 * n

    def parse(self):
        nparams, ntech, _, nobjects = self.rd(), self.rd(), self.rd(), self.rd()
        for _ in range(nparams):
            self.rd(); self.rd(); self.rd()
            self.skip_annotations(self.rd())
        techniques = []
        for _ in range(ntech):
            tname = self.name(self.rd())
            nann, npass = self.rd(), self.rd()
            self.skip_annotations(nann)
            passes = []
            for _ in range(npass):
                pname = self.name(self.rd())
                nann, nstates = self.rd(), self.rd()
                self.skip_annotations(nann)
                states = [(self.rd(), self.rd(), self.rd(), self.rd()) for _ in range(nstates)]
                passes.append({'name': pname, 'states': len(states), 'vs': None, 'ps': None})
            techniques.append({'name': tname, 'passes': passes})
        nstrings, nres = self.rd(), self.rd()
        for _ in range(nstrings):
            self.rd()
            size = self.rd()
            self.p += (size + 3) & ~3
        shaders = []
        for _ in range(nres):
            tech, pas, elem, state, usage = self.rd(), self.rd(), self.rd(), self.rd(), self.rd()
            size = self.rd()
            code = self.blob[self.p:self.p + size]
            self.p += (size + 3) & ~3
            if size < 8 or tech == 0xFFFFFFFF:
                continue
            ver = u32(code, 0)
            if (ver >> 16) not in (0xFFFE, 0xFFFF):
                continue                       # preshader / expression, not a shader object
            kind = 'vs' if (ver >> 16) == 0xFFFE else 'ps'
            code = code[:shader_length(code)]
            entry = {'kind': kind, 'model': '%s_%d_%d' % (kind, (ver >> 8) & 0xFF, ver & 0xFF),
                     'sha1': hashlib.sha1(code).hexdigest(), 'size': len(code)}
            self.codes[entry['sha1']] = code
            if tech < len(techniques) and pas < len(techniques[tech]['passes']):
                techniques[tech]['passes'][pas][kind] = entry
            shaders.append(dict(entry, technique=tech, pass_=pas, usage=usage))
        if self.p > len(self.blob):
            raise ValueError('effect overrun: stream at %d of %d' % (self.p, len(self.blob)))
        return {'objects': nobjects, 'techniques': techniques, 'shader_resources': len(shaders),
                'trailing': len(self.blob) - self.p}


def shader_length(code):
    """Bytes from the version token through the END token (0x0000FFFF), skipping comment blocks."""
    o = 4
    while o + 4 <= len(code):
        t = u32(code, o)
        if t == 0x0000FFFF:
            return o + 4
        o += 4 + (((t >> 16) & 0x7FFF) * 4 if (t & 0xFFFF) == 0xFFFE else 0)
    return len(code)


def main(argv):
    opts = {k: argv[argv.index(k) + 1] for k in ('--json', '--table') if k in argv}
    libs = [a for a in argv[1:] if a not in opts and a not in opts.values()]
    if not libs:
        print(__doc__)
        return 2
    report, rows, unmatched = [], [], 0
    for lib in libs:
        _, _, _, effects = parse_library(open(lib, 'rb').read())
        for path, blob in effects:
            ename = ntpath.basename(path)
            info = Effect(blob).parse()
            report.append(dict(info, library=os.path.basename(lib), effect=ename))
            npass = sum(len(t['passes']) for t in info['techniques'])
            bound = 0
            for t in info['techniques']:
                for p in t['passes']:
                    for kind in ('vs', 'ps'):
                        if p[kind]:
                            bound += 1
                            rows.append((p[kind]['sha1'], kind, ename, t['name'], p['name'] or '-'))
            unmatched += info['shader_resources'] - bound
            print('%-36s techniques %3d  passes %3d  shaders %3d  trailing %d' % (
                ename, len(info['techniques']), npass, info['shader_resources'], info['trailing']))
    uniq = {r[0] for r in rows}
    print('== %d shader bindings, %d unique bytecodes, %d resources without a pass slot' % (len(rows), len(uniq), unmatched))
    if '--json' in opts:
        with open(opts['--json'], 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
    if '--table' in opts:
        with open(opts['--table'], 'w', encoding='ascii', newline='\n') as f:
            f.write('# sha1 stage effect technique pass  (Divinity II v1.4.700.56 shader libraries; fx_effects.py)\n')
            for r in sorted(set(rows)):
                f.write(' '.join(r) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
