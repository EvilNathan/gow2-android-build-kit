"""Lists the draws of one frame of an (uncompressed) PCSX2 .gs dump, grouped by drawing state: texture, blend,
vertex colour and, for sprites, their size and bounding box in screen pixels (XYOFFSET already subtracted).
Made to compare particles (bubbles, steam) with the port's F8 log.
Usage: py -3 -I gsdump_draws.py <file.gs> [frame] [tbp,tbp,...]   (format: pcsx2/GS/GSDump.h)"""
import struct, sys
from collections import OrderedDict

path = sys.argv[1]
want = int(sys.argv[2]) if len(sys.argv) > 2 else 1
only = {int(t, 16) for t in sys.argv[3].split(',')} if len(sys.argv) > 3 else None
data = open(path, 'rb').read()
pos = 0
def u32():
    global pos
    v = struct.unpack_from('<I', data, pos)[0]; pos += 4; return v
assert u32() == 0xFFFFFFFF, 'old format'
hsize = u32()
hdr = struct.unpack_from('<9I', data, pos)
state_size = hdr[1]
pos += hsize
pos += state_size + 0x2000

R = {'prim': 0, 'tex0': [0, 0], 'tex1': [0, 0], 'alpha': [0, 0], 'test': [0, 0], 'frame': [0, 0],
     'xyoff': [0, 0], 'fba': [0, 0], 'texa': 0, 'pabe': 0, 'rgbaq': 0, 'uv': 0, 'st': 0}
frames = [OrderedDict()]
order = [0]
verts = []          # vertices of the primitive being built: (x, y, u, v, rgba)

def ctx():
    return (R['prim'] >> 9) & 1

def state_key():
    c = ctx(); p = R['prim']
    tme = (p >> 4) & 1; tex0 = R['tex0'][c] if tme else 0
    fr = R['frame'][c]
    return ('fbp=%x fpsm=%x fbmsk=%x prim=%u tme=%d fst=%d abe=%d tbp=%x tbw=%u psm=%x tw=%u th=%u tcc=%u tfx=%u cbp=%x cpsm=%x '
            'tex1=%x alpha=%x test=%x fba=%u texa=%x pabe=%u') % (
        fr & 0x1FF, (fr >> 24) & 0x3F, (fr >> 32) & 0xFFFFFFFF, p & 7, tme, (p >> 8) & 1, (p >> 6) & 1,
        tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F, 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF),
        (tex0 >> 34) & 1, (tex0 >> 35) & 3, (tex0 >> 37) & 0x3FFF, (tex0 >> 51) & 0xF,
        R['tex1'][c] & 0xFFFFFFFF if tme else 0, R['alpha'][c], R['test'][c] & 0x7FFFF, R['fba'][c] & 1, R['texa'], R['pabe'] & 1)

def group():
    key = state_key()
    g = frames[-1].get(key)
    if g is None:
        order[0] += 1
        g = frames[-1][key] = {'o': order[0], 'prims': 0, 'w': [], 'h': [], 'du': [], 'dv': [],
                               'bb': [1e9, 1e9, -1e9, -1e9], 'a': [], 'rgb': []}
    return g

NV = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}

def vertex(x, y, kick):
    c = ctx(); ox, oy = R['xyoff'][c] & 0xFFFF, (R['xyoff'][c] >> 32) & 0xFFFF
    sx, sy = (x - ox) / 16.0, (y - oy) / 16.0
    if (R['prim'] >> 8) & 1:
        u, v = (R['uv'] & 0x3FFF) / 16.0, ((R['uv'] >> 16) & 0x3FFF) / 16.0
    else:
        s, t = struct.unpack('<ff', struct.pack('<Q', R['st']))
        q = struct.unpack('<f', struct.pack('<I', (R['rgbaq'] >> 32) & 0xFFFFFFFF))[0] or 1.0
        tex0 = R['tex0'][c]
        u, v = s / q * (1 << ((tex0 >> 26) & 0xF)), t / q * (1 << ((tex0 >> 30) & 0xF))
    verts.append((sx, sy, u, v, R['rgbaq'] & 0xFFFFFFFF))
    p = R['prim'] & 7
    n = NV.get(p, 3)
    if len(verts) < n:
        return
    if not kick:
        verts.pop(0); return
    g = group(); g['prims'] += 1
    xs = [q[0] for q in verts[-n:]]; ys = [q[1] for q in verts[-n:]]
    bb = g['bb']; bb[0] = min(bb[0], *xs); bb[1] = min(bb[1], *ys); bb[2] = max(bb[2], *xs); bb[3] = max(bb[3], *ys)
    last = verts[-1]
    g['a'].append(last[4] >> 24); g['rgb'].append(last[4] & 0xFFFFFF)
    if p == 6:
        a, b = verts[-2], verts[-1]
        g['w'].append(abs(b[0] - a[0])); g['h'].append(abs(b[1] - a[1]))
        g['du'].append(abs(b[2] - a[2])); g['dv'].append(abs(b[3] - a[3]))
    if p in (0, 1, 3, 6):
        verts.clear()
    elif p in (2, 4):
        verts.pop(0)
    elif p == 5:
        del verts[1]

def write_reg(addr, v):
    if addr == 0x00: R['prim'] = v & 0x7FF; verts.clear()
    elif addr == 0x01: R['rgbaq'] = v
    elif addr == 0x02: R['st'] = v
    elif addr == 0x03: R['uv'] = v
    elif addr in (0x06, 0x07): R['tex0'][addr - 0x06] = v
    elif addr in (0x14, 0x15): R['tex1'][addr - 0x14] = v
    elif addr in (0x18, 0x19): R['xyoff'][addr - 0x18] = v
    elif addr == 0x3B: R['texa'] = v
    elif addr in (0x42, 0x43): R['alpha'][addr - 0x42] = v
    elif addr in (0x47, 0x48): R['test'][addr - 0x47] = v
    elif addr == 0x49: R['pabe'] = v
    elif addr in (0x4A, 0x4B): R['fba'][addr - 0x4A] = v
    elif addr in (0x4C, 0x4D): R['frame'][addr - 0x4C] = v
    elif addr in (0x04, 0x0C): vertex(v & 0xFFFF, (v >> 16) & 0xFFFF, addr == 0x04)
    elif addr in (0x05, 0x0D): vertex(v & 0xFFFF, (v >> 16) & 0xFFFF, addr == 0x05)

class Path:
    def __init__(self):
        self.buf = b''; self.left = 0; self.flg = 0; self.nreg = 0; self.regsd = []; self.ri = 0
    def feed(self, chunk):
        self.buf += chunk
        while True:
            if self.left == 0:
                if len(self.buf) < 16: return
                lo, hi = struct.unpack_from('<QQ', self.buf, 0); self.buf = self.buf[16:]
                nloop = lo & 0x7FFF; pre = (lo >> 46) & 1
                self.flg = (lo >> 58) & 3; self.nreg = ((lo >> 60) & 0xF) or 16
                self.regsd = [(hi >> (4 * i)) & 0xF for i in range(self.nreg)]
                if pre and self.flg == 0: write_reg(0x00, (lo >> 47) & 0x7FF)
                if self.flg == 2: self.left = nloop
                elif self.flg == 1: self.left = (nloop * self.nreg + 1) // 2 * 2
                else: self.left = nloop * self.nreg
                self.ri = 0; self.count = nloop * self.nreg
                continue
            if self.flg == 2:
                n = min(self.left, len(self.buf) // 16)
                if n == 0: return
                self.buf = self.buf[16 * n:]; self.left -= n; continue
            if self.flg == 1:
                if len(self.buf) < 8: return
                v = struct.unpack_from('<Q', self.buf, 0)[0]; self.buf = self.buf[8:]
                if self.ri < self.count:
                    write_reg(self.regsd[self.ri % self.nreg], v)
                self.ri += 1; self.left -= 1; continue
            if len(self.buf) < 16: return
            lo, hi = struct.unpack_from('<QQ', self.buf, 0); self.buf = self.buf[16:]
            r = self.regsd[self.ri % self.nreg]; self.ri += 1; self.left -= 1
            if r == 0x0: write_reg(0x00, lo & 0x7FF)
            elif r == 0x1:   # PACKED RGBAQ (Q comes from the last ST)
                rgba = (lo & 0xFF) | ((lo >> 32 & 0xFF) << 8) | ((hi & 0xFF) << 16) | ((hi >> 32 & 0xFF) << 24)
                R['rgbaq'] = (R['rgbaq'] & ~0xFFFFFFFF) | rgba
            elif r == 0x2:   # PACKED ST: S, T and Q
                R['st'] = lo; R['rgbaq'] = (R['rgbaq'] & 0xFFFFFFFF) | ((hi & 0xFFFFFFFF) << 32)
            elif r == 0x3: R['uv'] = (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16)
            elif r == 0x4: vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, not ((hi >> 47) & 1))   # PACKED XYZF2
            elif r == 0x5: vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, not ((hi >> 47) & 1))   # PACKED XYZ2
            elif r == 0x6: write_reg(0x06, lo)
            elif r == 0x7: write_reg(0x07, lo)
            elif r == 0xE: write_reg(hi & 0xFF, lo)                                         # A+D
            elif r == 0xF: pass                                                             # NOP

paths = [Path() for _ in range(4)]
while pos < len(data):
    t = data[pos]; pos += 1
    if t == 0:
        p = data[pos]; pos += 1; size = u32()
        paths[p & 3].feed(data[pos:pos + size]); pos += size
    elif t == 1:
        pos += 1; frames.append(OrderedDict()); order[0] = 0
    elif t == 2:
        pos += 4
    elif t == 3:
        pos += 0x2000
    else:
        print('unknown id', t, 'at', pos); break

def rng(v):
    if not v: return '-'
    s = sorted(v)
    return '%.1f/%.1f/%.1f' % (s[0], s[len(s) // 2], s[-1])

print('frames:', len(frames), [sum(g['prims'] for g in f.values()) for f in frames])
f = frames[min(want, len(frames) - 1)]
for key, g in sorted(f.items(), key=lambda kv: kv[1]['o']):
    tbp = int(key.split('tbp=')[1].split()[0], 16)
    if only is not None and tbp not in only:
        continue
    bb = g['bb']
    line = '#%03d n=%-5d %s bbox=%.0f-%.0f,%.0f-%.0f vtxA=%s rgb=%s' % (g['o'], g['prims'], key, bb[0], bb[2], bb[1], bb[3], rng(g['a']), ','.join(sorted({'%06x' % c for c in g['rgb']})[:4]))
    if g['w']:
        line += ' w=%s h=%s du=%s dv=%s' % (rng(g['w']), rng(g['h']), rng(g['du']), rng(g['dv']))
    print(line)
