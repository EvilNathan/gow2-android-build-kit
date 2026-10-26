"""Desensambla un módulo IRX del IOP (p. ej. game/disc/SMPD_IOP.IRX) marcando sus imports.

    py -3 tools/irx_dis.py game/disc/989NOMID.IRX 0x1878 0x1A40

Los IRX son reubicables: las direcciones son relativas al módulo. Para pasarlas a direcciones del
IOP emulado, súmales la base con que se cargó (sale en el log: "[IOP] loaded IRX id=7 ... base=0x25e00").
Las llamadas a librerías del IOP se marcan como "IMPORT libreria:ordinal" (p. ej. sysmem:4 =
AllocSysMemory, thsemap:6 = WaitSema, cdvdman:6 = sceCdRead).
Requisitos: py -3 -m pip install capstone
"""
import argparse
import struct

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs

IMPORT_MAGIC = 0x41E00000  # cabecera de una tabla de imports del IRX
JR_RA = 0x03E00008


def text_section(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    strtab_off = sections[shstrndx][4]
    for name, _type, _flags, _addr, offset, size in sections:
        if data[strtab_off + name:data.index(b"\0", strtab_off + name)] == b".text":
            return data[offset:offset + size]
    raise SystemExit("el IRX no tiene .text")


def find_imports(code):
    """Cada import es un stub "jr ra; addiu zero, zero, ordinal" tras la cabecera de su librería."""
    imports = {}
    i = 0
    while i < len(code) - 24:
        if struct.unpack_from("<I", code, i)[0] != IMPORT_MAGIC:
            i += 4
            continue
        library = code[i + 12:i + 20].split(b"\0")[0].decode(errors="replace")
        j = i + 20
        while j < len(code) - 8:
            first, second = struct.unpack_from("<II", code, j)
            if first != JR_RA or (second >> 16) != 0x2400:
                break
            imports[j] = "%s:%d" % (library, second & 0xFFFF)
            j += 8
        i = j
    return imports


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("irx")
    parser.add_argument("start")
    parser.add_argument("end")
    args = parser.parse_args()

    code = text_section(open(args.irx, "rb").read())
    imports = find_imports(code)
    start, end = int(args.start, 16), int(args.end, 16)
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    md.skipdata = True
    for ins in md.disasm(code[start:end], start):
        line = "%06x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str)
        if ins.mnemonic == "jal" and int(ins.op_str, 16) in imports:
            line += "    ; IMPORT " + imports[int(ins.op_str, 16)]
        print(line)


if __name__ == "__main__":
    main()
