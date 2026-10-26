"""Imprime los sprites con formato de Z dibujados en la memoria del Z (FRAME.FBP = 120) de un frame de un volcado .gs:
coordenadas, color, XYOFFSET y SCISSOR del contexto. Uso: py -3 -I gsdump_zclear.py <archivo.gs> <frame>"""
import struct, sys
data = open(sys.argv[1], 'rb').read(); want = int(sys.argv[2]); pos = 0
def u32():
    global pos
    v = struct.unpack_from('<I', data, pos)[0]; pos += 4; return v
assert u32() == 0xFFFFFFFF
hs = u32(); ss = struct.unpack_from('<9I', data, pos)[1]; pos += hs + ss + 0x2000
R = {'prim': 0, 'frame': [0, 0], 'xyoff': [0, 0], 'scissor': [0, 0], 'rgba': 0}
q = []; frame = 0; shown = 0
def reg(a, v):
    global shown
    if a == 0: R['prim'] = v & 0x7FF; q.clear()
    elif a == 1: R['rgba'] = v & 0xFFFFFFFF
    elif a in (0x4C, 0x4D): R['frame'][a - 0x4C] = v
    elif a in (0x18, 0x19): R['xyoff'][a - 0x18] = v
    elif a in (0x40, 0x41): R['scissor'][a - 0x40] = v
    elif a in (0x04, 0x05):
        c = (R['prim'] >> 9) & 1; fr = R['frame'][c]
        q.append((v & 0xFFFF, (v >> 16) & 0xFFFF))
        if (R['prim'] & 7) == 6 and len(q) == 2:
            if frame == want and (fr & 0x1FF) == 120 and ((fr >> 24) & 0x3F) in (0x30, 0x31, 0x32, 0x3A) and shown < 80:
                shown += 1
                ox, oy = R['xyoff'][c] & 0xFFFF, (R['xyoff'][c] >> 32) & 0xFFFF
                sc = R['scissor'][c]
                print('sprite (%.1f,%.1f)-(%.1f,%.1f) rgba=%08x ofs=(%.1f,%.1f) scissor x %d-%d y %d-%d fbw=%d' % (
                    q[0][0] / 16 - ox / 16, q[0][1] / 16 - oy / 16, q[1][0] / 16 - ox / 16, q[1][1] / 16 - oy / 16, R['rgba'],
                    ox / 16, oy / 16, sc & 0x7FF, (sc >> 16) & 0x7FF, (sc >> 32) & 0x7FF, (sc >> 48) & 0x7FF, (fr >> 16) & 0x3F))
            q.clear()
class P:
    def __init__(s): s.b = b''; s.left = 0
    def feed(s, ch):
        s.b += ch
        while True:
            if s.left == 0:
                if len(s.b) < 16: return
                lo, hi = struct.unpack_from('<QQ', s.b, 0); s.b = s.b[16:]
                n = lo & 0x7FFF; s.flg = (lo >> 58) & 3; s.nr = ((lo >> 60) & 0xF) or 16
                s.rd = [(hi >> (4 * i)) & 0xF for i in range(s.nr)]
                if (lo >> 46) & 1 and s.flg == 0: reg(0, (lo >> 47) & 0x7FF)
                s.left = n if s.flg == 2 else ((n * s.nr + 1) // 2 * 2 if s.flg == 1 else n * s.nr); s.ri = 0; s.cnt = n * s.nr
                continue
            if s.flg == 2:
                k = min(s.left, len(s.b) // 16)
                if k == 0: return
                s.b = s.b[16 * k:]; s.left -= k; continue
            if s.flg == 1:
                if len(s.b) < 8: return
                v = struct.unpack_from('<Q', s.b, 0)[0]; s.b = s.b[8:]
                if s.ri < s.cnt: reg(s.rd[s.ri % s.nr], v)
                s.ri += 1; s.left -= 1; continue
            if len(s.b) < 16: return
            lo, hi = struct.unpack_from('<QQ', s.b, 0); s.b = s.b[16:]
            r = s.rd[s.ri % s.nr]; s.ri += 1; s.left -= 1
            if r == 0: reg(0, lo)
            elif r == 1: R['rgba'] = (lo & 0xFF) | (((lo >> 32) & 0xFF) << 8) | ((hi & 0xFF) << 16) | (((hi >> 32) & 0xFF) << 24)
            elif r in (4, 5): reg(r, (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16))
            elif r == 0xE: reg(hi & 0xFF, lo)
ps = [P() for _ in range(4)]
while pos < len(data):
    t = data[pos]; pos += 1
    if t == 0:
        p = data[pos]; pos += 1; n = u32(); ps[p & 3].feed(data[pos:pos + n]); pos += n
    elif t == 1: pos += 1; frame += 1
    elif t == 2: pos += 4
    elif t == 3: pos += 0x2000
    if frame > want: break
