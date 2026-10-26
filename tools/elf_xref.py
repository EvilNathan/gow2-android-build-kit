"""Busca en todo .text las instrucciones cuyo texto contenga alguna de las cadenas dadas.

Sirve para encontrar quién llama a una función o quién lee/escribe una variable global:

    py -3 tools/elf_xref.py "jal 0x13d710"            # llamadas a heap_create
    py -3 tools/elf_xref.py "-0x3b24(" "-0x3b20("     # accesos a 0x29C4DC / 0x29C4E0 (base 0x2A0000)

Las globales se acceden como "lui reg, 0x2a" + "lw/sw x, -0x3b24(reg)": se busca el desplazamiento.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("needles", nargs="+")
    parser.add_argument("--elf", default=DEFAULT_ELF)
    args = parser.parse_args()

    with open(args.elf, "rb") as f:
        text = ELFFile(f).get_section_by_name(".text")
        base, data = text["sh_addr"], text.data()
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    for off in range(0, len(data), 4):
        for ins in md.disasm(data[off:off + 4], base + off):
            line = "%s %s" % (ins.mnemonic, ins.op_str)
            if any(n in line for n in args.needles):
                print("%08x: %s" % (ins.address, line))


if __name__ == "__main__":
    main()
