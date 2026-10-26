"""Lista qué funciones de cada librería del IOP piden los IRX dados, y qué librerías exportan ellos.

    py -3 tools/irx_imports.py game/disc/IOP_MOD/SMPD.IRX game/disc/IOP_MOD/989NOMID.IRX ...

Cada IRX lleva una tabla de imports por librería (marca 0x41E00000, versión, nombre de 8 bytes) seguida
de un stub de 8 bytes por función: "jr ra" + "addiu $zero, $zero, ordinal". Es lo mismo que decodifica
el runtime en ps2xIOP/src/emulator/imports/iop_imports.cpp. Sirve para cruzar lo que pide el juego con
lo que implementa el runtime (ver NOTES.md, "IOP imports used by GoW II").
La salida describe archivos del juego: se queda en local (regla 1).
"""
import glob
import struct
import sys
from collections import defaultdict

IMPORT_MAGIC = 0x41E00000
EXPORT_MAGIC = 0x41C00000


def words(data):
    for off in range(0, len(data) - 3, 4):
        yield off, struct.unpack_from('<I', data, off)[0]


def name_at(data, off):
    return data[off:off + 8].split(b'\0')[0].decode('ascii', 'replace')


def scan(path):
    data = open(path, 'rb').read()
    imports, exports = [], []
    for off, w in words(data):
        if w == IMPORT_MAGIC:
            version = struct.unpack_from('<H', data, off + 8)[0]
            lib = name_at(data, off + 12)
            ordinals = []
            stub = off + 20
            while stub + 8 <= len(data):
                a, b = struct.unpack_from('<II', data, stub)
                if a == 0 and b == 0:
                    break
                if a != 0x03E00008 or (b & 0xFFFF0000) != 0x24000000:
                    ordinals = None
                    break
                ordinals.append(b & 0xFFFF)
                stub += 8
            if ordinals:
                imports.append((lib, version, ordinals))
        elif w == EXPORT_MAGIC:
            version = struct.unpack_from('<H', data, off + 8)[0]
            lib = name_at(data, off + 12)
            count = 0
            cur = off + 20
            while cur + 4 <= len(data) and struct.unpack_from('<I', data, cur)[0] != 0:
                count += 1
                cur += 4
            if lib.isprintable() and lib:
                exports.append((lib, version, count))
    return imports, exports


def main():
    uses = defaultdict(set)
    exported = {}
    paths = [p for arg in sys.argv[1:] for p in (sorted(glob.glob(arg)) or [arg])]  # PowerShell no expande *.IRX
    for path in paths:
        mod = path.replace('\\', '/').rsplit('/', 1)[-1]
        imports, exports = scan(path)
        for lib, version, count in exports:
            exported[lib] = f'{mod} v{version >> 8}.{version & 0xFF} ({count})'
        for lib, _version, ordinals in imports:
            for o in ordinals:
                uses[(lib, o)].add(mod)
    print('## Exports (librerías que da un IRX del juego)')
    for lib, who in sorted(exported.items()):
        print(f'{lib}\t{who}')
    print('## Imports')
    for (lib, o), mods in sorted(uses.items()):
        print(f'{lib}\t{o}\t{",".join(sorted(mods))}')


if __name__ == '__main__':
    main()
