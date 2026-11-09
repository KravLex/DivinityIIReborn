r"""The hero's skeleton (Skeleton.nif): rest transforms, forward kinematics and a stick figure picture.

  py -3 -I skeleton.py picture <Skeleton.nif> <out.png> [kf] [sequence] [time]   front and side view

A bone's world transform is parent_world * local (Gamebryo: rotation matrix M stored row by row, a point p of the
child goes to parent space as M @ p + t; scale ignored, it is 1 for these bones). Used to design our own animation:
poses are given as rotations of the rest pose, and every key is written as an absolute local rotation (quaternion).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nif_write                    # noqa: E402


class Skeleton:
    def __init__(self, data):
        n = nif_write.Nif(data)
        self.nif = n
        self.bones = {}                                   # name -> (parent name, rest t, rest R)
        name = lambda b: n.strings[b['name']] if b['name'] >= 0 else None
        parent = {}
        for i, b in enumerate(n.blocks):
            if b.type in ('NiNode', 'NiBillboardNode') and b.raw is None:
                for c in b['children']:
                    if c >= 0:
                        parent[c] = i
        for i, b in enumerate(n.blocks):
            if b.type in ('NiNode',) and b.raw is None and name(b):
                p = parent.get(i)
                self.bones[name(b)] = (name(n.blocks[p]) if p is not None else None,
                                       np.array(b['translation'], float),
                                       np.array(b['rotation'], float).reshape(3, 3))
        self.order = []                                   # parents before children
        seen = set()

        def visit(bn):
            if bn in seen:
                return
            p = self.bones[bn][0]
            if p is not None and p in self.bones:
                visit(p)
            seen.add(bn)
            self.order.append(bn)
        for bn in self.bones:
            visit(bn)

    def world(self, local=None):
        """world (R, t) of every bone; local: name -> (R, t) overrides of the rest transforms"""
        local = local or {}
        out = {}
        for bn in self.order:
            p, t, R = self.bones[bn]
            if bn in local:
                R, t = local[bn][0], (local[bn][1] if local[bn][1] is not None else t)
            if p is None or p not in out:
                out[bn] = (R, t)
            else:
                PR, Pt = out[p]
                out[bn] = (PR @ R, PR @ t + Pt)
        return out


def quat_to_m(q):
    w, x, y, z = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def m_to_quat(m):
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = np.sqrt(tr + 1.0) * 2
        q = [0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [(m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def axis_angle(axis, deg):
    a = np.asarray(axis, float); a = a / np.linalg.norm(a)
    th = np.radians(deg); c, s = np.cos(th), np.sin(th)
    x, y, z = a
    return np.array([[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                     [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                     [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]])


def picture(sk, world, path, size=512):
    """front (x, z) and side (y, z) stick figures side by side"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                    'tools', 'reborn'))
    from fx_select import sheet
    imgs = []
    for ax in (0, 1):
        img = np.zeros((size, size, 4), np.uint8); img[..., 3] = 255
        sc = size / 240.0

        def px(p):
            return int(size / 2 + p[ax] * sc), int(size - 20 - p[2] * sc)
        for bn, (p, _, _) in sk.bones.items():
            if p in world and bn in world and p not in ('Scene Root', 'Reference', 'Reference NonAccum') \
                    and 'Footsteps' not in bn:
                x0, y0 = px(world[p][1]); x1, y1 = px(world[bn][1])
                n = max(abs(x1 - x0), abs(y1 - y0), 1)
                col = (255, 120, 80) if 'Left' in bn else (80, 180, 255) if 'Right' in bn else (230, 230, 230)
                for k in range(n + 1):
                    x = x0 + (x1 - x0) * k // n; y = y0 + (y1 - y0) * k // n
                    if 0 <= x < size and 0 <= y < size:
                        img[max(0, y - 1):y + 2, max(0, x - 1):x + 2, :3] = col
        imgs.append(('front' if ax == 0 else 'side', img))
    sheet(path, imgs, tile=size, cols=2)


def main(argv):
    if len(argv) >= 4 and argv[1] == 'picture':
        sk = Skeleton(open(argv[2], 'rb').read())
        picture(sk, sk.world(), argv[3])
        print('picture ->', argv[3])
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
