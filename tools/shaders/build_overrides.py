r"""Build Divinity II Reborn shader overrides (DXVK patch 0010, d3d9.shaderOverridePath).

Usage: py -3 -I build_overrides.py <Packed dir> <sources dir> <output dir>

Every *.hlsl in <sources dir>/<set>/ starts with
  // replaces: <sha1 of the original bytecode> <vs_3_0|ps_3_0> ...
  // preshader-registers: c3 c4 c5          (optional: registers the original effect's preshader writes)
                                           (or that DXVK sets: c39, the TAA frame noise of patch 0012)
and is compiled with the system d3dcompiler_47 (entry main) to <output dir>/<set>/<sha1>.dxso.
  #include "name.hlsli"   is resolved from <sources dir>/common (nested includes allowed)
  <set>/include-sets.txt  lists other source sets (one per line) whose shaders are also built into this set;
                          a shader of the set itself wins over an included one replacing the same original
<sources dir>/common holds only include files and is not a set; post/ and third_party/ (SMAA, build_post.py) neither.

The original shader is looked up in the game's shader libraries (read only) and the replacement is checked
against its constant table, because the game (D3DX effects) keeps setting the original registers:
  - every sampler of the replacement must be a sampler of the original at the same index;
  - every float/int/bool constant must lie in a register range the original uses or in the preshader list.
A mismatch is an error and nothing is written for that shader.
"""
import glob, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 're'))
sys.path.insert(0, os.path.join(HERE, '..', 'asset_inspector'))

import d3dsm3  # noqa: E402


def original_shaders(packed):
    import dv2_inspect
    import fx_effects
    from fxbin_index import parse_library
    arc = dv2_inspect.Dv2(os.path.join(packed, 'MainDataStartup.dv2'))
    codes = {}
    for e in arc.entries:
        if e.name.lower().endswith('.fxbin'):
            for _, blob in parse_library(arc.read(e))[3]:
                eff = fx_effects.Effect(blob)
                eff.parse()
                codes.update(eff.codes)
    return codes


def resolve_includes(text, common_dir):
    def repl(m):
        return open(os.path.join(common_dir, m.group(1)), encoding='utf-8').read()
    for _ in range(8):                                     # nested includes, bounded
        text, n = re.subn(r'^#include "([^"]+)"\s*$', repl, text, flags=re.M)
        if not n:
            break
    return text


# include files; SMAA passes and their third-party sources (tools/shaders/build_post.py)
NOT_SETS = {'common', 'post', 'third_party'}


def set_sources(src_dir):
    """(set, source path) for every shader of every set, own shaders first, then those of included sets
    whose original is not replaced by the set itself."""
    for set_dir in sorted(d for d in glob.glob(os.path.join(src_dir, '*')) if os.path.isdir(d)):
        set_name = os.path.basename(set_dir)
        if set_name in NOT_SETS:
            continue
        own = sorted(glob.glob(os.path.join(set_dir, '*.hlsl')))
        replaced = {replaces(p) for p in own}
        for p in own:
            yield set_name, p
        inc_file = os.path.join(set_dir, 'include-sets.txt')
        if os.path.exists(inc_file):
            for inc in open(inc_file, encoding='utf-8').read().split():
                for p in sorted(glob.glob(os.path.join(src_dir, inc, '*.hlsl'))):
                    if replaces(p) not in replaced:
                        replaced.add(replaces(p))
                        yield set_name, p


def replaces(path):
    m = re.search(r'^// replaces: ([0-9a-f]{40})', open(path, encoding='utf-8').read(), re.M)
    return m.group(1) if m else path


def registers(table):
    used = {}
    for c in table:
        for i in range(c['index'], c['index'] + max(1, c['count'])):
            used[(c['regset'], i)] = c['name']
    return used


def check(new_table, orig_table, preshader):
    orig = registers(orig_table)
    errors = []
    for (regset, index), name in sorted(registers(new_table).items()):
        reg = '%s%d' % (regset, index)
        if (regset, index) in orig:
            continue
        if regset == 's':
            errors.append('sampler %s (%s) is not a sampler of the original' % (reg, name))
        elif reg not in preshader:
            errors.append('%s (%s) is neither used by the original nor a preshader register' % (reg, name))
    return errors


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    packed, src_dir, out_dir = argv[1:]
    originals = original_shaders(packed)
    failed = 0
    for set_name, path in set_sources(src_dir):
        text = resolve_includes(open(path, encoding='utf-8').read(), os.path.join(src_dir, 'common'))
        m = re.search(r'^// replaces: ([0-9a-f]{40}) ([vp]s_3_0)', text, re.M)
        if not m:
            print('SKIP %s: no "// replaces:" header' % path)
            continue
        sha1, target = m.groups()
        pm = re.search(r'^// preshader-registers:(.*)$', text, re.M)
        preshader = set(pm.group(1).split()) if pm else set()
        name = '%s <- %s/%s' % (set_name, os.path.basename(os.path.dirname(path)), os.path.basename(path))
        orig = originals.get(sha1)
        if orig is None:
            print('FAIL %s: original %s not in the game shader libraries' % (name, sha1))
            failed += 1
            continue
        try:
            code, log = d3dsm3.compile(text, 'main', target, name=os.path.basename(path))
        except RuntimeError as e:
            print('FAIL %s: %s' % (name, e))
            failed += 1
            continue
        errors = check(d3dsm3.ctab(code), d3dsm3.ctab(orig), preshader)
        if errors:
            print('FAIL %s:\n  %s' % (name, '\n  '.join(errors)))
            failed += 1
            continue
        out = os.path.join(out_dir, set_name)
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, sha1 + '.dxso'), 'wb').write(code)
        slots = re.search(r'approximately (\d+) instruction slots', d3dsm3.disassemble(code))
        oslots = re.search(r'approximately (\d+) instruction slots', d3dsm3.disassemble(orig))
        print('OK   %-55s -> %s.dxso  %d bytes, %s slots (original %s)%s' % (
            name, sha1[:12], len(code), slots.group(1) if slots else '?', oslots.group(1) if oslots else '?',
            ('  warnings: ' + log.strip().replace('\n', ' | ')) if log.strip() else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
