"""Desensamblador de microcódigo de VU: la parte upper (FMAC: ADD/MUL/MADD/CLIP/FTOI...) y la lower
(saltos, aritmética entera, cargas/escrituras, DIV, XGKICK...), con los bits I/E/M/D/T.

    py -3 tools/vu_dis.py volcado.bin 0xINICIO 0xFIN
    py -3 tools/vu_dis.py --trace out/logs/vu1_trace.txt [salida.txt]

El volcado empieza con los 16 KB de código de la VU1 (como out/logs/vu1_atasco.bin o vu1_trace_start.bin
de las trazas locales). Con --trace, anota cada línea de una traza de instrucciones ("PC c=… U=… L=…")
con su desensamblado. Sin dependencias."""
import re, struct, sys

LOWER7 = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU",
          0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET",
          0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET",
          0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE",
          0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
LOWEROP = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
SPECIAL2 = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD",
            0x38: "DIV", 0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR",
            0x3E: "ILWR", 0x3F: "ISWR", 0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR",
            0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP", 0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD",
            0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy", 0x75: "EATANxz", 0x76: "ESUM", 0x78: "ESQRT",
            0x79: "ERSQRT", 0x7A: "ERCPR", 0x7B: "WAITP", 0x7C: "ESIN", 0x7D: "EATAN", 0x7E: "EEXP"}

BC = "xyzw"
# Upper: (nombre, forma). Formas: bc = fd, fs, ft.bc | q/i = fd, fs, Q/I | v = fd, fs, ft
UPPER = {}
for i, n in enumerate(("ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL")):
    for b in range(4):
        UPPER[i * 4 + b] = (n + BC[b], "bc")
UPPER.update({0x1C: ("MULq", "q"), 0x1D: ("MAXi", "i"), 0x1E: ("MULi", "i"), 0x1F: ("MINIi", "i"),
              0x20: ("ADDq", "q"), 0x21: ("MADDq", "q"), 0x22: ("ADDi", "i"), 0x23: ("MADDi", "i"),
              0x24: ("SUBq", "q"), 0x25: ("MSUBq", "q"), 0x26: ("SUBi", "i"), 0x27: ("MSUBi", "i"),
              0x28: ("ADD", "v"), 0x29: ("MADD", "v"), 0x2A: ("MUL", "v"), 0x2B: ("MAX", "v"),
              0x2C: ("SUB", "v"), 0x2D: ("MSUB", "v"), 0x2E: ("OPMSUB", "v"), 0x2F: ("MINI", "v")})
# Upper especiales (op 0x3C-0x3F): código = (op & 3) | ((ins >> 4) & 0x7C). Formas: abc = ACC, fs, ft.bc
UPPER_SP = {}
for i, n in enumerate(("ADDA", "SUBA", "MADDA", "MSUBA")):
    for b in range(4):
        UPPER_SP[i * 4 + b] = (n + BC[b], "abc")
for b in range(4):
    UPPER_SP[0x18 + b] = ("MULA" + BC[b], "abc")
UPPER_SP.update({0x10: ("ITOF0", "t"), 0x11: ("ITOF4", "t"), 0x12: ("ITOF12", "t"), 0x13: ("ITOF15", "t"),
                 0x14: ("FTOI0", "t"), 0x15: ("FTOI4", "t"), 0x16: ("FTOI12", "t"), 0x17: ("FTOI15", "t"),
                 0x1C: ("MULAq", "aq"), 0x1D: ("ABS", "t"), 0x1E: ("MULAi", "ai"), 0x1F: ("CLIP", "clip"),
                 0x20: ("ADDAq", "aq"), 0x21: ("MADDAq", "aq"), 0x22: ("ADDAi", "ai"), 0x23: ("MADDAi", "ai"),
                 0x24: ("SUBAq", "aq"), 0x25: ("MSUBAq", "aq"), 0x26: ("SUBAi", "ai"), 0x27: ("MSUBAi", "ai"),
                 0x28: ("ADDA", "av"), 0x29: ("MADDA", "av"), 0x2A: ("MULA", "av"), 0x2C: ("SUBA", "av"),
                 0x2D: ("MSUBA", "av"), 0x2E: ("OPMULA", "av"), 0x2F: ("NOP", "nop")})


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dest_of(ins):
    return "".join(c for c, b in zip("xyzw", (24, 23, 22, 21)) if ins >> b & 1)


def upper(ins):
    op = ins & 0x3F
    ft, fs, fd, bc = (ins >> 16) & 31, (ins >> 11) & 31, (ins >> 6) & 31, ins & 3
    d = dest_of(ins)
    if op >= 0x3C:
        code = (op & 3) | ((ins >> 4) & 0x7C)
        n, form = UPPER_SP.get(code, ("?up%02X" % code, "nop"))
        if form == "nop":
            return n
        if form == "abc":
            return "%s.%s ACC, vf%d, vf%d%s" % (n, d, fs, ft, BC[bc])
        if form in ("aq", "ai"):
            return "%s.%s ACC, vf%d, %s" % (n, d, fs, "Q" if form == "aq" else "I")
        if form == "av":
            return "%s.%s ACC, vf%d, vf%d" % (n, d, fs, ft)
        if form == "t":
            return "%s.%s vf%d, vf%d" % (n, d, ft, fs)
        if form == "clip":
            return "CLIPw.xyz vf%d, vf%dw" % (fs, ft)
    n, form = UPPER.get(op, ("?up%02X" % op, "v"))
    if form == "bc":
        return "%s.%s vf%d, vf%d, vf%d%s" % (n, d, fd, fs, ft, BC[bc])
    if form in ("q", "i"):
        return "%s.%s vf%d, vf%d, %s" % (n, d, fd, fs, "Q" if form == "q" else "I")
    return "%s.%s vf%d, vf%d, vf%d" % (n, d, fd, fs, ft)


def lower(ins, pc):
    op = ins >> 25
    it, is_, id_ = (ins >> 16) & 31, (ins >> 11) & 31, (ins >> 6) & 31
    dest = dest_of(ins)
    if op == 0x40:
        f = ins & 0x3F
        if f in LOWEROP:
            n = LOWEROP[f]
            if n == "IADDI":
                return "IADDI vi%d, vi%d, %d" % (it, is_, sext(id_, 5))
            return "%s vi%d, vi%d, vi%d" % (n, id_, is_, it)
        if f >= 0x3C:
            code = ((ins >> 4) & 0x7C) | (ins & 3)
            n = SPECIAL2.get(code, "?%02X" % code)
            fsf, ftf = BC[(ins >> 21) & 3], BC[(ins >> 23) & 3]
            if n in ("DIV", "RSQRT"):
                return "%s Q, vf%d%s, vf%d%s" % (n, is_, fsf, it, ftf)
            if n == "SQRT":
                return "SQRT Q, vf%d%s" % (it, ftf)
            if n in ("MOVE", "MR32"):
                return "%s.%s vf%d, vf%d" % (n, dest, it, is_)
            if n == "MTIR":
                return "MTIR vi%d, vf%d%s" % (it, is_, fsf)
            if n == "MFIR":
                return "MFIR.%s vf%d, vi%d" % (dest, it, is_)
            if n in ("ILWR", "ISWR"):
                return "%s.%s vi%d, (vi%d)" % (n, dest, it, is_)
            if n in ("LQI", "LQD"):
                return "%s.%s vf%d, (vi%d%s)" % (n, dest, it, is_, "++" if n == "LQI" else "--")
            if n in ("SQI", "SQD"):
                return "%s.%s vf%d, (vi%d%s)" % (n, dest, is_, it, "++" if n == "SQI" else "--")
            if n in ("XGKICK", "XTOP", "XITOP"):
                return "%s vi%d" % (n, is_ if n == "XGKICK" else it)
            if n == "MFP":
                return "MFP.%s vf%d, P" % (dest, it)
            if n.startswith("E"):
                return "%s P, vf%d" % (n, is_)
            if n in ("WAITQ", "WAITP"):
                return n
            return "%s.%s (it=%d is=%d)" % (n, dest, it, is_)
        return "LOWEROP?%02X" % f
    n = LOWER7.get(op, "?op%02X" % op)
    imm11 = sext(ins & 0x7FF, 11)
    if n in ("B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        target = (pc + 8 + imm11 * 8) & 0x3FFF
        if n in ("B",):
            return "B 0x%04X" % target
        if n == "BAL":
            return "BAL vi%d, 0x%04X" % (it, target)
        if n in ("IBEQ", "IBNE"):
            return "%s vi%d, vi%d, 0x%04X" % (n, it, is_, target)
        return "%s vi%d, 0x%04X" % (n, is_, target)
    if n in ("JR", "JALR"):
        return "%s vi%d" % (n, is_) if n == "JR" else "JALR vi%d, vi%d" % (it, is_)
    if n in ("IADDIU", "ISUBIU"):
        imm15 = ((ins >> 10) & 0x7800) | (ins & 0x7FF)
        return "%s vi%d, vi%d, %d" % (n, it, is_, imm15)
    if n in ("LQ", "SQ"):
        return "%s.%s vf%d, %d(vi%d)" % (n, dest, it if n == "LQ" else is_, imm11, is_ if n == "LQ" else it)
    if n in ("ILW", "ISW"):
        return "%s.%s vi%d, %d(vi%d)" % (n, dest, it, imm11, is_)
    if n.startswith("F"):
        return "%s vi1, 0x%X" % (n, ins & 0xFFFFFF)
    return n


def pair(up, lo, pc):
    flags = "".join(f for f, b in (("I", 31), ("E", 30), ("M", 29), ("D", 28), ("T", 27)) if up >> b & 1)
    lw = ("LOI 0x%08X (%g)" % (lo, struct.unpack("<f", struct.pack("<I", lo))[0])) if (up >> 31) & 1 else lower(lo, pc)
    return "[%-2s] %-34s | %s" % (flags, upper(up), lw)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--trace":
        out = open(sys.argv[3], "w", encoding="utf-8") if len(sys.argv) > 3 else sys.stdout
        line_re = re.compile(r"^([0-9A-F]{4}) c=\d+ U=([0-9A-F]{8}) L=([0-9A-F]{8})")
        for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
            m = line_re.match(line)
            if m:
                pc, up, lo = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
                out.write("%04X %s\n      %s" % (pc, pair(up, lo, pc), line[m.end():].lstrip()))
            else:
                out.write(line)
        return
    data = open(sys.argv[1], "rb").read()
    start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
    for pc in range(start, end, 8):
        lo, up = struct.unpack_from("<II", data, pc)
        print("%04X: %s" % (pc, pair(up, lo, pc)))


main()
