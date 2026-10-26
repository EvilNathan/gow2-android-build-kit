"""Lee un volcado .gs de PCSX2 (sin comprimir) y resume el Z de los vértices por estado de dibujo, frame a frame.
Uso: py -3 -I gsdump_z.py <archivo.gs> [frame]   (formato: pcsx2/GS/GSDump.h)"""
import struct, sys
from collections import OrderedDict

path = sys.argv[1]
want = int(sys.argv[2]) if len(sys.argv) > 2 else 1
data = open(path, 'rb').read()
pos = 0
def u32():
    global pos
    v = struct.unpack_from('<I', data, pos)[0]; pos += 4; return v
assert u32() == 0xFFFFFFFF, 'formato antiguo'
hsize = u32()
hdr = struct.unpack_from('<9I', data, pos)
state_size = hdr[1]
pos += hsize
pos += state_size + 0x2000

# estado del GS que importa
regs = {'prim': 0, 'tex0': [0, 0], 'test': [0, 0], 'zbuf': [0, 0], 'frame': [0, 0]}
frames = [OrderedDict()]
order = [0]

def ctx():
    return (regs['prim'] >> 9) & 1

def vertex(z, kick):
    c = ctx()
    tme = (regs['prim'] >> 4) & 1
    tex0 = regs['tex0'][c]
    tbp = (tex0 & 0x3FFF) if tme else 0
    tpsm = ((tex0 >> 20) & 0x3F) if tme else 0
    test = regs['test'][c]
    zb = regs['zbuf'][c]
    fr = regs['frame'][c]
    key = 'fbp=%u fpsm=%x fbmsk=%x zbp=%x zpsm=%x zmsk=%d ztst=%u tme=%d tbp=%x tpsm=%x prim=%u test=%x' % (
        fr & 0x1FF, (fr >> 24) & 0x3F, (fr >> 32) & 0xFFFFFFFF, zb & 0x1FF, ((zb >> 24) & 0xF) | 0x30, (zb >> 32) & 1, (test >> 17) & 3, tme, tbp, tpsm,
        regs['prim'] & 7, test & 0x7FFFF)
    g = frames[-1].get(key)
    if g is None:
        order[0] += 1
        g = frames[-1][key] = [order[0], 0, 0xFFFFFFFF, 0]
    g[1] += 1
    g[2] = min(g[2], z); g[3] = max(g[3], z)

def write_reg(addr, v):
    if addr == 0x00: regs['prim'] = v & 0x7FF
    elif addr in (0x06, 0x07): regs['tex0'][addr - 0x06] = v
    elif addr in (0x47, 0x48): regs['test'][addr - 0x47] = v
    elif addr in (0x4E, 0x4F): regs['zbuf'][addr - 0x4E] = v
    elif addr in (0x4C, 0x4D): regs['frame'][addr - 0x4C] = v
    elif addr in (0x04, 0x0C): vertex((v >> 32) & 0xFFFFFF, addr == 0x04)
    elif addr in (0x05, 0x0D): vertex((v >> 32) & 0xFFFFFFFF, addr == 0x05)

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
                if pre and self.flg == 0: regs['prim'] = (lo >> 47) & 0x7FF
                if self.flg == 2: self.left = nloop          # IMAGE: qwords
                elif self.flg == 1: self.left = (nloop * self.nreg + 1) // 2 * 2  # REGLIST: dwords (rounded to qword)
                else: self.left = nloop * self.nreg          # PACKED: qwords
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
            if r == 0x0: regs['prim'] = lo & 0x7FF
            elif r == 0x4: vertex((hi >> 4) & 0xFFFFFF, not ((hi >> 47) & 1))   # PACKED XYZF2
            elif r == 0x5: vertex(hi & 0xFFFFFFFF, not ((hi >> 47) & 1))        # PACKED XYZ2
            elif r == 0x6: write_reg(0x06, lo)
            elif r == 0x7: write_reg(0x07, lo)
            elif r == 0xE: write_reg(hi & 0xFF, lo)                            # A+D

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
        print('id desconocido', t, 'en', pos); break

print('frames:', len(frames), [sum(g[1] for g in f.values()) for f in frames])
f = frames[min(want, len(frames) - 1)]
for key, (o, n, lo, hi) in sorted(f.items(), key=lambda kv: kv[1][0]):
    print('#%03d x%-6d %s z=%06x-%06x' % (o, n, key, lo, hi))
