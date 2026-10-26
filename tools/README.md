# tools/

Python analysis scripts for investigating blockers without opening Ghidra. They read **your local copy** of the game (`game/disc/`); they neither contain nor generate anything from the game that gets uploaded to the repo.

```powershell
py -3 -m pip install capstone pyelftools
```

| Script | What it's for |
|---|---|
| `elf_dis.py 0xSTART 0xEND` | Disassemble a range of the ELF. `--str 0xADDR …` reads strings (resource names, error messages). |
| `elf_xref.py "jal 0x13d710"` | Find who calls a function or who accesses a global (`"-0x3b24("` with base `0x2A0000`). |
| `irx_dis.py game/disc/X.IRX 0xSTART 0xEND` | Disassemble an IOP module, marking its imports (`sysmem:4`, `cdvdman:6`…). Addresses are relative to the module: add the `base=` that appears in the log when it is loaded. |
| `irx_imports.py game/disc/IOP_MOD/*.IRX` | Lists, by library and ordinal, the IOP functions that the IRX request (`thbase 43 SMPD.IRX`) and the libraries they export themselves (`libsd`, `snd989`…). For cross-checking against what the runtime implements (NOTES.md, "IOP imports used by GoW II"). No dependencies. |
| `vu_dis.py dump.bin 0xSTART 0xEND` | Disassemble VU1 microcode from a dump (16 KB of code at the start): the upper part (`MUL`, `MADD`, `CLIP`, `FTOI4`…) and the lower part (branches, integer arithmetic, loads/stores, `DIV`, `XGKICK`), with the I/E/M/D/T bits. `--trace trace.txt [output.txt]` annotates an instruction trace (lines `PC c=… U=… L=…`, like the one from `[vu-trace]`) with the disassembly of each line. No dependencies. |
| `scan_missing_functions.py` | Find functions that Ghidra doesn't detect: virtual methods (`vfn_`), `JAL` targets in gaps (`fn_`) and pointers that the code builds with `lui`+`addiu` (`cfn_`, callbacks). **Run it after every Ghidra export** and regenerate `funcmap.csv` with `make_funcmap.py --extra` (they don't go in `entry_points`: there the recompiler doesn't register their resume points). |
| `make_funcmap.py [--extra out/scan_missing.txt]` | Generates `config/funcmap.csv`: the functions from the Ghidra export (without the internal labels `entry_XXXX`, `caseD_*`) and, with `--extra`, the scanner's. |
| `find_split_switches.py [--fix]` | Searches `config/funcmap.csv` for functions split in the middle of a `switch`: a function that starts right after a `jr $reg` (not `$ra`), where the scanner took the first case of a jump table for the start of a function. Symptom in the log: `missing-target kind=IndirectJump`. `--fix` joins each pair (then you have to regenerate). Blocker 16. |
| `gsdump/gsdump_z.py dump.gs [frame]` | Reads a PCSX2 GS dump (`.gs` **uncompressed**: *GS Dump Compression = Uncompressed*) and summarizes, per draw state and in order of appearance, how many vertices there are and their Z range (`FRAME`, `ZBUF`, `TEST`, `TEX0`). For comparing what reaches the GS on the emulated PS2 with the port's registers. |
| `gsdump/gsdump_zsim.py dump.gs frame [alpha]` | Simulates only the frame's Z buffer on a pixel grid with the GS rules (linear interpolation, `ZTST`, `ZMSK`, saturation; with `alpha`, the alpha test with texture alpha `0x80`) and prints which draw leaves the Z at each point. That is how we saw that the Z clear was missing (blocker 15). |
| `gsdump/gsdump_draws.py dump.gs [frame] [tbp,...]` | Lists one frame's draws grouped by drawing state, in order of appearance: frame and mask, primitive, texture (`TBP`, format, size, `TCC`/`TFX`, palette), `ALPHA`, `TEST`, `TEXA`, the bounding box in screen pixels, the vertex alpha and colours and, for sprites, their width/height and texel span (min/median/max). The same fields as the port's local F8 log, to compare particles and post-processing passes draw by draw (it found blockers 25 and 26). |
| `gsdump/gsdump_zclear.py dump.gs frame` | Lists the sprites with a Z format drawn into the Z memory (`FRAME.FBP` = 120): coordinates, color, `XYOFFSET`, `SCISSOR`. |

Capstone decodes standard MIPS32: what is specific to the Emotion Engine (128-bit `sq`/`lq`, MMI, `daddu`…) comes out as `.byte`. For that, use Ghidra with the EE extension or the `// 0x…` comments in the files in `generated/`.
