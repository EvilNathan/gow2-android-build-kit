"""Genera config/funcmap.csv a partir del export de Ghidra (config/gow_functions.csv) y del escáner.

ExportPS2Functions.java mezcla en el CSV las funciones con las etiquetas de código que caen dentro
de ellas (entry_XXXXXXXX, caseD_*, default*: destinos de saltos y casos de switch). ps2_recomp
convierte cada fila en una función aparte, así que con las etiquetas saldrían ~38 000 funciones
solapadas en vez de ~5 900: aquí se quedan solo las funciones.

Con --extra se añaden además las funciones que Ghidra no ve (métodos virtuales, callbacks), con el
formato que imprime tools/scan_missing_functions.py ("vfn_xxxxxxxx@0xXXXXXXXX"). Cada una acaba donde
empieza la siguiente función conocida. Van aquí y no en entry_points de la plantilla porque
ps2_recomp llama "entry_XXXX" a las funciones de entry_points y no registra sus puntos de
reanudación (isEntryFunctionName): un hilo suspendido dentro de una llamada hecha desde una de ellas
no se podía reanudar (docs/NOTES.md, "Blockers", fase 2).

    py -3 tools/make_funcmap.py                                   # solo Ghidra
    py -3 tools/scan_missing_functions.py --csv config/funcmap.csv > out/scan_missing.txt
    py -3 tools/make_funcmap.py --extra out/scan_missing.txt      # Ghidra + escáner
"""
import argparse
import csv
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LABEL = re.compile(r"^(entry_[0-9a-fA-F]+|caseD_.*|default.*)$")
EXTRA = re.compile(r'"([A-Za-z_][A-Za-z0-9_]*)@0x([0-9A-Fa-f]+)"')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", default=os.path.join(REPO, "config", "gow_functions.csv"))
    parser.add_argument("--out", default=os.path.join(REPO, "config", "funcmap.csv"))
    parser.add_argument("--extra", help="salida de scan_missing_functions.py con las funciones que Ghidra no ve")
    args = parser.parse_args()

    with open(args.csv, newline="") as f:
        rows = list(csv.DictReader(f))
    funcs = [(int(r["Start"], 16), int(r["End"], 16), r["Name"]) for r in rows if not LABEL.match(r["Name"])]
    ghidra_count = len(funcs)

    extras = []
    if args.extra:
        with open(args.extra, encoding="utf-8") as f:
            extras = [(m.group(1), int(m.group(2), 16)) for m in EXTRA.finditer(f.read())]
        starts = sorted({s for s, _, _ in funcs} | {a for _, a in extras})
        known = {s for s, _, _ in funcs}
        for name, address in extras:
            if address in known:
                continue
            i = starts.index(address)
            if i + 1 >= len(starts):
                raise SystemExit("%s es la última función: no se sabe dónde acaba." % name)
            funcs.append((address, starts[i + 1], name))

    funcs.sort()
    if len({s for s, _, _ in funcs}) != len(funcs):
        raise SystemExit("Hay funciones con la misma dirección de inicio: revisa el export de Ghidra.")
    for a, b in zip(funcs, funcs[1:]):
        if a[1] > b[0]:
            raise SystemExit("Se solapan %s y %s: revisa sus límites en Ghidra." % (a[2], b[2]))

    with open(args.out, "w", newline="") as f:
        f.write("Name,Start,End,Size\n")
        for start, end, name in funcs:
            f.write("%s,0x%08X,0x%08X,%d\n" % (name, start, end, end - start))
    print("%d funciones (%d de Ghidra, de %d filas del export; %d del escáner) -> %s"
          % (len(funcs), ghidra_count, len(rows), len(funcs) - ghidra_count, args.out))


if __name__ == "__main__":
    main()
