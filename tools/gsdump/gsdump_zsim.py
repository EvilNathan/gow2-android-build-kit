"""Simula solo el buffer de Z de un frame de un volcado .gs de PCSX2 (sin comprimir) en una cuadrícula de píxeles:
interpolación lineal del Z en pantalla, ZTST, ZMSK, saturación al máximo del formato. Las pruebas de alfa se dan por
superadas. Imprime, por punto, el dibujo que deja el último Z escrito y los que fallaron contra él.
Uso: py -3 -I gsdump_zsim.py <archivo.gs> <frame>"""
import struct, sys

path = sys.argv[1]; want = int(sys.argv[2])
data = open(path, 'rb').read()
pos = 0
def u32():
    global pos
    v = struct.unpack_from('<I', data, pos)[0]; pos += 4; return v
assert u32() == 0xFFFFFFFF
hsize = u32(); state_size = struct.unpack_from('<9I', data, pos)[1]; pos += hsize + state_size + 0x2000

XS = list(range(16, 512, 32)); YS = list(range(8, 448, 24))
zb = {(x, y): 0 for x in XS for y in YS}
owner = {(x, y): 'clear?' for x in XS for y in YS}
lost = {(x, y): {} for x in XS for y in YS}   # draws that failed here: label -> count
colour = {(x, y): '-' for x in XS for y in YS}  # last draw that wrote colour (passed its tests)
R = {'prim': 0, 'tex0': [0, 0], 'test': [0, 0], 'zbuf': [0, 0], 'frame': [0, 0], 'xyoff': [0, 0], 'a': 0x80}
ALPHA = len(sys.argv) > 3 and sys.argv[3] == 'alpha'   # alpha test with texture alpha 0x80 and the vertex alpha
alpha_stats = {}
queue = []; frame = 0; drawn = 0
prim_n = {3: 3, 4: 3, 5: 3, 6: 2}

def label(c):
    tme = (R['prim'] >> 4) & 1
    return 'fbp=%u tbp=%x zm=%d zt=%u p=%u' % (R['frame'][c] & 0x1FF, (R['tex0'][c] & 0x3FFF) if tme else 0,
                                           (R['zbuf'][c] >> 32) & 1, (R['test'][c] >> 17) & 3, R['prim'] & 7)

def raster(vs):
    global drawn
    c = (R['prim'] >> 9) & 1
    if frame != want: return
    fpsm = (R['frame'][c] >> 24) & 0x3F
    if (R['frame'][c] & 0x1FF) == 0x78 and fpsm in (0x30, 0x31, 0x32, 0x3A) and len(vs) == 2:
        # a Z-format sprite into the Z memory: clears the Z under it (to 0 here: the game's clear colour)
        ox = (R['xyoff'][c] & 0xFFFF) / 16.0; oy = ((R['xyoff'][c] >> 32) & 0xFFFF) / 16.0
        (x0, y0, _, _), (x1, y1, _, _) = vs
        for (px, py) in zb:
            if min(x0, x1) / 16.0 - ox <= px < max(x0, x1) / 16.0 - ox and min(y0, y1) / 16.0 - oy <= py < max(y0, y1) / 16.0 - oy:
                zb[(px, py)] = 0; owner[(px, py)] = 'zclear'
        return
    if (R['frame'][c] & 0x1FF) != 8: return
    test = R['test'][c]; zte = (test >> 16) & 1; ztst = (test >> 17) & 3 if zte else 1
    zbuf = R['zbuf'][c]; zmsk = (zbuf >> 32) & 1; zpsm = ((zbuf >> 24) & 0xF) | 0x30
    zmax = 0xFFFFFFFF if zpsm == 0x30 else 0xFFFF if zpsm in (0x32, 0x3A) else 0xFFFFFF
    ox = (R['xyoff'][c] & 0xFFFF) / 16.0; oy = ((R['xyoff'][c] >> 32) & 0xFFFF) / 16.0
    pts = [(x / 16.0 - ox, y / 16.0 - oy, z) for x, y, z, _ in vs]
    alphas = [a for _, _, _, a in vs]
    lab = label(c); drawn += 1
    st = alpha_stats.setdefault(lab, [255, 0, 0]); st[0] = min(st[0], min(alphas)); st[1] = max(st[1], max(alphas)); st[2] += 1
    ate = test & 1; atst = (test >> 1) & 7; aref = (test >> 4) & 0xFF; afail = (test >> 12) & 3
    iip = (R['prim'] >> 3) & 1
    sprite = len(pts) == 2
    tme = (R['prim'] >> 4) & 1
    if sprite and not tme and ztst == 1 and drawn > 200:   # the later full-screen clears (Z-only before the HUD)
        return
    for (px, py) in zb:
        if sprite:
            (x0, y0, _), (x1, y1, z) = pts
            if not (min(x0, x1) <= px < max(x0, x1) and min(y0, y1) <= py < max(y0, y1)): continue
            zf = z
        else:
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = pts
            d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if d == 0: continue
            a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d
            b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d
            g = 1 - a - b
            if a < 0 or b < 0 or g < 0: continue
            zf = a * z0 + b * z1 + g * z2
            av = (a * alphas[0] + b * alphas[1] + g * alphas[2]) if iip else alphas[2]
            if ALPHA and ate and tme:
                A = int(0x80 * int(av)) >> 7
                passes = {0: False, 1: True, 2: A < aref, 3: A <= aref, 4: A == aref, 5: A >= aref, 6: A > aref, 7: A != aref}[atst]
                if not passes and afail in (0, 3, 1):   # KEEP / RGB_ONLY / FB_ONLY: no Z write
                    continue
        zf = min(max(int(zf), 0), zmax)
        cur = zb[(px, py)]
        ok = ztst == 1 or (ztst == 2 and zf >= cur) or (ztst == 3 and zf > cur)
        if not ok:
            lost[(px, py)][lab] = lost[(px, py)].get(lab, 0) + 1
            continue
        if ((R['frame'][c] >> 32) & 0xFFFFFFFF) != 0xFFFFFFFF:
            colour[(px, py)] = '%s z=%06x' % (lab, zf)
        if not zmsk:
            zb[(px, py)] = zf; owner[(px, py)] = '%s z=%06x' % (lab, zf)

def kick(x, y, z, do):
    t = R['prim'] & 7
    queue.append((x, y, z, R['a']))
    n = prim_n.get(t)
    if n is None: queue.clear(); return
    if len(queue) < n: return
    if do: raster(queue[-n:])
    if t == 3 or t == 6: queue.clear()          # triangle / sprite list
    elif t == 4: del queue[0]                   # strip
    elif t == 5: del queue[1]                   # fan keeps the first

def reg(addr, v):
    if addr == 0x00: R['prim'] = v & 0x7FF; queue.clear()
    elif addr == 0x01: R['a'] = (v >> 24) & 0xFF                  # RGBAQ (A+D layout)
    elif addr in (0x06, 0x07): R['tex0'][addr - 6] = v
    elif addr in (0x47, 0x48): R['test'][addr - 0x47] = v
    elif addr in (0x4E, 0x4F): R['zbuf'][addr - 0x4E] = v
    elif addr in (0x4C, 0x4D): R['frame'][addr - 0x4C] = v
    elif addr in (0x18, 0x19): R['xyoff'][addr - 0x18] = v
    elif addr in (0x04, 0x0C): kick(v & 0xFFFF, (v >> 16) & 0xFFFF, (v >> 32) & 0xFFFFFF, addr == 0x04)
    elif addr in (0x05, 0x0D): kick(v & 0xFFFF, (v >> 16) & 0xFFFF, (v >> 32) & 0xFFFFFFFF, addr == 0x05)

class Path:
    def __init__(self): self.buf = b''; self.left = 0
    def feed(self, chunk):
        self.buf += chunk
        while True:
            if self.left == 0:
                if len(self.buf) < 16: return
                lo, hi = struct.unpack_from('<QQ', self.buf, 0); self.buf = self.buf[16:]
                nloop = lo & 0x7FFF; self.flg = (lo >> 58) & 3; self.nreg = ((lo >> 60) & 0xF) or 16
                self.rd = [(hi >> (4 * i)) & 0xF for i in range(self.nreg)]
                if (lo >> 46) & 1 and self.flg == 0: reg(0, (lo >> 47) & 0x7FF)
                self.left = nloop if self.flg == 2 else ((nloop * self.nreg + 1) // 2 * 2 if self.flg == 1 else nloop * self.nreg)
                self.ri = 0; self.count = nloop * self.nreg
                continue
            if self.flg == 2:
                n = min(self.left, len(self.buf) // 16)
                if n == 0: return
                self.buf = self.buf[16 * n:]; self.left -= n; continue
            if self.flg == 1:
                if len(self.buf) < 8: return
                v = struct.unpack_from('<Q', self.buf, 0)[0]; self.buf = self.buf[8:]
                if self.ri < self.count: reg(self.rd[self.ri % self.nreg], v)
                self.ri += 1; self.left -= 1; continue
            if len(self.buf) < 16: return
            lo, hi = struct.unpack_from('<QQ', self.buf, 0); self.buf = self.buf[16:]
            r = self.rd[self.ri % self.nreg]; self.ri += 1; self.left -= 1
            if r == 0x0: reg(0, lo)
            elif r == 0x1: R['a'] = (hi >> 32) & 0xFF                 # PACKED RGBAQ: A in bits 96-103
            elif r in (0x4, 0xC): kick(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (hi >> 4) & 0xFFFFFF, r == 0x4 and not ((hi >> 47) & 1))
            elif r in (0x5, 0xD): kick(lo & 0xFFFF, (lo >> 32) & 0xFFFF, hi & 0xFFFFFFFF, r == 0x5 and not ((hi >> 47) & 1))
            elif r in (0x6, 0x7): reg(r, lo)
            elif r == 0xE: reg(hi & 0xFF, lo)

paths = [Path() for _ in range(4)]
while pos < len(data):
    t = data[pos]; pos += 1
    if t == 0:
        p = data[pos]; pos += 1; size = u32(); paths[p & 3].feed(data[pos:pos + size]); pos += size
    elif t == 1: pos += 1; frame += 1
    elif t == 2: pos += 4
    elif t == 3: pos += 0x2000
    if frame > want: break

print('triangles/sprites on fbp 8 in frame %d: %d' % (want, drawn))
for k in ('tbp=35c0', 'tbp=3600', 'tbp=3540', 'tbp=34c0', 'tbp=2cc0', 'tbp=3ea3', 'tbp=3b30'):
    for lab, (lo, hi, n) in alpha_stats.items():
        if k in lab: print('vertex alpha %-40s %02x-%02x (%d prims)' % (lab, lo, hi, n))
for y in YS:
    row = []
    for x in XS:
        o = owner[(x, y)]
        row.append(o.split(' ')[1].replace('tbp=', '') if o != 'clear?' else '-')
    print('y=%3d ' % y + ' '.join('%5s' % r for r in row))
print('\nlast colour writer (tbp):')
for y in YS:
    print('y=%3d ' % y + ' '.join('%5s' % (colour[(x, y)].split(' ')[1].replace('tbp=', '') if colour[(x, y)] != '-' else '-') for x in XS))
print()
for p in [(208, 8), (208, 32), (336, 56), (112, 248), (400, 248)]:
    if p in owner:
        print(p, 'winner:', owner[p])
        for k, n in sorted(lost[p].items(), key=lambda kv: -kv[1])[:6]: print('     lost x%d %s' % (n, k))
