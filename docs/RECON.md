# Recon: CUSA08809 eboot (Phase 2)

> Generated with `tools/elfscan` on 2026-10-06. Full per-NID reports are in `reports/`, which is
> gitignored and local only. Regenerate them with:
> `python tools/elfscan/elfscan.py <game>/eboot.bin --shadps4 ../shadps4 --format md -o reports/imports.md`

## The dump

| | |
|---|---|
| Source | Your own dump of the base game |
| Version | **Base game, v1.00**: `gameversion.txt` → `BuildVersion=2017-11-09.106777`, `RetailBuild=1`, `titleskuid=SCEE` |
| Missing | The **v1.01 patch**, which AjncSystem's fixes target. It has to be dumped from your own PS4. |
| Layout | `eboot.bin`, `sce_module/{libc.prx, libSceFios2.prx}`, `cachedFiles.psarc`, `pak0-3.psarc`, `pakN.psarc`, `sce_discmap.plt`, `coredata/enginesupport/DefaultCmdLineArgs.txt`, `misc/GameCommandLineArgs.txt`, `CommandLineArgs.txt` |

## eboot.bin

| | |
|---|---|
| Type | `ET_SCE_EXEC_ASLR` (position independent), entry `0xF120` |
| Module name | `Shadow_Shipping` |
| SDK | `0x04508101` (SDK 4.508) |
| Code | about 22.6 MB (R-X); data about 25 MB in the file, 37 MB in memory; TLS 32 bytes |
| Relocations | 709,058 `RELATIVE`, 2,704 `R_X86_64_64`, 571 `JUMP_SLOT` |
| Imports | **588** from 49 modules; 436 named and implemented in shadPS4 |

### Import surface (HLE work list)

| Group | Imports | Notes |
|---|---|---|
| libc (game-bundled `libc.prx`) | 141 | Link in natively, as bbport does. libc.prx itself imports 87, mostly from libkernel. |
| libkernel + libScePosix | 66 + 45 | All known to shadPS4 |
| libSceGnmDriver | 103 | GPU, all known to shadPS4 |
| libSceFios2 (game-bundled `libSceFios2.prx`) | 34 | Link in natively. It imports 83: libkernel, LibcInternal, DiscMap, Sysmodule. |
| libSceNet / NetCtl / Http / Ssl | 27 / 4 / 20 / 2 | Online features, can be stubbed |
| libSceVideoOut | 21 | Presentation |
| Audio: AudioOut / Audio3d / Ngs2 | 7 / 11 / 11 | Ngs2 needs the firmware `.sprx` (LLE) at first |
| SaveData / SaveDataDialog | 11 / 9 | |
| libScePad / SystemGesture / Ime | 9 / 9 / 3 | Input |
| libSceFiber | 8 | |
| Dialogs, trophies, PSN, share, screenshot, etc. | about 40 | Mostly stubs |
| **Unknown to shadPS4** | 2 | `libSceShareUtility`: `8hZ2EEl2Tto`, `j7DlalBzHh8`. Stub them. |

Not used: `libSceAvPlayer` and `libSceVideodec`, so the intro video doesn't go through Sony's player. `libSceUlt` doesn't appear in the eboot either. AjncSystem's requirement for it probably comes from Ngs2's own dependencies; to be confirmed when the firmware module is scanned.

### Patch candidates (raw byte matches, to be confirmed by disassembly)

| Pattern | Hits | Notes |
|---|---|---|
| SSE4a `EXTRQ` | 42 | Clustered around `0xF7E3A5`. Likely one hot routine (Oodle?). This is the Intel crash in AjncSystem fix 01. |
| SSE4a `INSERTQ` | 1 | `0xC59653` |
| `mov r64, fs:[0]` | 38 | Guest TLS base reads that need patching on Windows |

The bundled `libc.prx` and `libSceFios2.prx` contain none of these patterns.

## Engine command-line files: a ready-made settings hook

The Bluepoint engine reads `+cvar=value` lines from text files at boot:

- `CommandLineArgs.txt`: `+r_fps=0`, `+renderErrorMsgs=0`
- `misc/GameCommandLineArgs.txt`: conditional cvars such as `[PackageBuild][shipping]shaderTypeToLoad = Shipping`, `[!RetailBuild]LoadCmdsFromFile=...`, `[!Tools][!RetailBuild]LoadCmdsFromFile=$/misc/DebugMenu.txt`
- `coredata/enginesupport/DefaultCmdLineArgs.txt`

**Implication:** the runtime's file layer can overlay these files to change engine settings (frame rate, rendering) without patching any code. Next steps: list the cvar names in the eboot's strings and find how `r_fps` is used.

## Next steps
1. Dump the **v1.01 patch** from your own PS4 and rescan the patched eboot.
2. Dump the firmware `libSceNgs2.sprx` and `libSceUlt.sprx`, scan them, and add their imports to the work list.
3. Load the eboot in Ghidra: disassemble the SSE4a sites, find the cvar table and `r_fps`, and find the frame-step logic.
4. Phase 3: write the loader skeleton that links in `libc.prx` and `libSceFios2.prx`.
