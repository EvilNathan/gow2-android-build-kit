"""Busca en config/funcmap.csv funciones partidas a mitad de un switch: una función que empieza justo después de un
"jr $reg" (no $ra) con su delay slot, es decir, donde el escáner tomó el primer caso de una tabla de saltos por el
comienzo de una función (normalmente un vfn_). El recompilador solo genera el switch si la tabla y sus destinos caen
dentro de la misma función; si no, los demás casos dan "[guest-branch:missing-target] kind=IndirectJump".

Uso: py -3 -I tools/find_split_switches.py [--fix]   (--fix une cada pareja en config/funcmap.csv)
"""
import csv, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
elf_name = open(os.path.join(ROOT, 'game', 'elf-name.txt')).readline().strip()
elf = open(os.path.join(ROOT, 'game', 'disc', elf_name), 'rb').read()
phoff, phentsize, phnum = struct.unpack_from('<I', elf, 0x1C)[0], *struct.unpack_from('<HH', elf, 0x2A)
segments = []
for i in range(phnum):
    p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<5I', elf, phoff + i * phentsize)
    if p_type == 1:
        segments.append((p_vaddr, p_offset, p_filesz))

def word(addr):
    for vaddr, off, size in segments:
        if vaddr <= addr < vaddr + size:
            return struct.unpack_from('<I', elf, off + addr - vaddr)[0]
    return None

path = os.path.join(ROOT, 'config', 'funcmap.csv')
rows = list(csv.DictReader(open(path, newline='')))
by_start = sorted(rows, key=lambda r: int(r['Start'], 16))
pairs = []
for prev, cur in zip(by_start, by_start[1:]):
    start = int(cur['Start'], 16)
    jr = word(start - 8)
    if jr is None:
        continue
    is_jr = (jr >> 26) == 0 and (jr & 0x3F) == 0x08
    rs = (jr >> 21) & 31
    if is_jr and rs != 31 and int(prev['End'], 16) >= start - 8:
        pairs.append((prev, cur, rs))
for prev, cur, rs in pairs:
    print('%s %s-%s  +  %s %s-%s   (jr $%d en %08x)' % (prev['Name'], prev['Start'], prev['End'], cur['Name'], cur['Start'],
                                                      cur['End'], rs, int(cur['Start'], 16) - 8))
print('%d funciones partidas' % len(pairs))

if '--fix' in sys.argv and pairs:
    drop = {id(cur) for _, cur, _ in pairs}
    for prev, cur, _ in pairs:
        prev['End'] = cur['End']
        prev['Size'] = str(int(cur['End'], 16) - int(prev['Start'], 16))
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=['Name', 'Start', 'End', 'Size'])
        w.writeheader()
        for r in rows:
            if id(r) not in drop:
                w.writerow(r)
    print('funcmap.csv: %d parejas unidas' % len(pairs))
