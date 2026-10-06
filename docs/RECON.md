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

## Firmware modules (from the user's PS4)

Both are plain decrypted modules and contain no SSE4a or `fs:[0]` patterns.

| Module | Size | Imports | Exports | Depends on |
|---|---|---|---|---|
| `libSceNgs2.sprx` 1.1 | 372 KB | 57 (50 known to shadPS4) | 47 | libSceAjm (10), libSceLibcInternal (32), libkernel (15) |
| `libSceUlt.sprx` 1.1 | 239 KB | 42 (35 known to shadPS4) | 64 | libSceFiber (7), libSceLibcInternal (11), libkernel (20), ulobjmgr (4) |

- **Ngs2:** loading it LLE means the runtime must provide **libSceAjm**, the audio codec manager that decodes ATRAC9 (bbport uses LibAtrac9). Its LibcInternal imports should resolve against `libc.prx` by NID, like Fios2's.
- **Ult:** neither the eboot nor Ngs2 imports it. AjncSystem's build requires it, but our runtime may not. Keep it on hand and load it only if something asks for it.

## Milestone 1 result: the game's code runs on Windows (2026-10-06)

`tools/sotcconv` builds `build/game/boot.img` (72 MB, in 7 s) from eboot, libc.prx and libSceFios2.prx:
- 714,369 relocations
- 2,889 cross-module links resolved ahead of time:
  - eboot → libc: 2,826
  - eboot → Fios2: 36
  - Fios2 → libc (through LibcInternal): 16
  - libc → Fios2: 11
- 490 system imports left for the runtime, all named using shadPS4's aerolib table

Exact patch sites, decoded with capstone over the 55,790 functions listed in `.eh_frame`. A linear sweep over the executable segment is useless, because that segment also holds read-only data.

| Site | Count | Handling |
|---|---|---|
| `mov rax, qword ptr fs:[0]` | 38, all this one form | Rewritten in place to a `gs:` read: a TEB TLS slot on Windows, the GS base on Linux |
| `extrq xmm, xmm` | 96 | Native on AMD (the dev machine is a Ryzen 5 3600); emulated on SIGILL on Linux/Intel |
| `insertq xmm, xmm, imm, imm` | 2 | Same |

libc.prx and libSceFios2.prx have no fs reads and no SSE4a.

`sotc-runtime` (clang-cl, Windows 10):
1. Loads the image at `0x10000000`. `0x400000` is already taken on Windows, and the image is position independent.
2. Starts libc and Fios2, then enters the eboot.
3. The game prints its own startup log:
   - memory setup
   - `Build Version: 2017-11-09.106777`
   - its engine command line (`+r_fps=0 ... +StartupScript=$/scripts/shadow/shadowmain.sgpr +VSyncWindow=10%`)
4. It loads `$/workspaces/_ps4/shadow.cwks` from the archives.

It then waits forever in `sceKernelWaitEventFlag`, on an event flag that the stubbed `sceKernelCreateEventFlag` never created. The first unimplemented imports, in call order:

1. libSceLibcInternalExt:sceLibcHeapGetTraceInfo
2. libSceGnmDriver:sceGnmGetTheTessellationFactorRingBufferBaseAddress
3. libkernel:sceKernelConvertUtcToLocaltime
4. libSceDiscMap:Func_7C980FFB0AA27E7A
5. libkernel:sceKernelCreateEventFlag
6. libSceFiber: sceFiberOptParamInitialize, _sceFiberInitializeImpl, sceFiberGetInfo, sceFiberRename, sceFiberRun
7. libSceCommonDialog:sceCommonDialogInitialize
8. libSceNet:sceNetInit
9. libSceUserService:sceUserServiceInitialize
10. libSceShareUtility:sceShareUtilityInitializeEx2
11. libkernel: sceKernelPollEventFlag, sceKernelSetEventFlag
12. libSceGnmDriver: sceGnmRegisterOwner, sceGnmMapComputeQueue
13. libkernel:sceKernelWaitEventFlag (hangs here)

Milestone 2 starts with event flags and fibers, then the user service and the event queue, then the GnmDriver functions needed before M3. Fibers matter early: the engine runs work on them, and a stubbed `sceFiberRun` that returns 0 without running anything fails silently.

Linux is not tested yet: there is no WSL distribution on the dev machine.

## Next steps
1. Dump the **v1.01 patch** from your own PS4 and rescan the patched eboot.
2. ~~Dump and scan the firmware modules~~ Done; see above.
3. Load the eboot in Ghidra: disassemble the SSE4a sites, find the cvar table and `r_fps`, and find the frame-step logic.
4. Phase 3: write the loader skeleton that links in `libc.prx` and `libSceFios2.prx`.
