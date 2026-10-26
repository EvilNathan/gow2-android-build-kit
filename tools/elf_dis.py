"""Desensambla un rango del ELF del juego o lee cadenas de sus datos, sin abrir Ghidra.

    py -3 tools/elf_dis.py 0x13DC78 0x13DE28          # desensambla [inicio, fin)
    py -3 tools/elf_dis.py --str 0x2B16A8 0x2B59C0    # lee cadenas C en esas direcciones

Capstone decodifica MIPS32 estándar: las instrucciones propias del Emotion Engine (sq/lq de 128
bits, MMI, daddu...) salen como ".byte". Para leerlas, Ghidra con la extensión EE o los
comentarios "// 0x..." de generated/.
Requisitos: py -3 -m pip install capstone pyelftools
"""
import argparse
import os

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs
from elftools.elf.elffile import ELFFile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# el ELF que detectó scripts/extract-iso.ps1
with open(os.path.join(REPO, "game", "elf-name.txt"), encoding="ascii") as _f:
    DEFAULT_ELF = os.path.join(REPO, "game", "disc", _f.readline().strip())


def segment_bytes(elf, address, size):
    for seg in elf.iter_segments():
        if seg["p_type"] == "PT_LOAD" and seg["p_vaddr"] <= address < seg["p_vaddr"] + seg["p_filesz"]:
            offset = address - seg["p_vaddr"]
            return seg.data()[offset:offset + size]
    raise SystemExit("0x%X no está en ningún segmento con datos del ELF" % address)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("addresses", nargs="+", help="inicio y fin (o direcciones de cadenas con --str)")
    parser.add_argument("--str", action="store_true", help="leer cadenas C en vez de desensamblar")
    parser.add_argument("--elf", default=DEFAULT_ELF)
    args = parser.parse_args()

    with open(args.elf, "rb") as f:
        elf = ELFFile(f)
        values = [int(a, 16) for a in args.addresses]
        if args.str:
            for address in values:
                print("0x%08X %r" % (address, segment_bytes(elf, address, 256).split(b"\0")[0]))
            return
        start, end = values[0], values[1]
        md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
        md.skipdata = True
        for ins in md.disasm(segment_bytes(elf, start, end - start), start):
            print("%08x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))


if __name__ == "__main__":
    main()
