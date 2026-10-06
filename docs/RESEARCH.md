# Shadow of the Colossus (PS4, 2018) — Native PC Port Research

> Status: research only, no code yet. Last updated 2026-10-06.
> Target dump: **CUSA08809 (EU), v1.01**. Target platforms: **Windows and Linux**.

---

## 1. What a "native port" means here

There are three ways to get a PS4 game running on a PC:

| Approach | What runs the game code | Examples | Effort |
|---|---|---|---|
| **Emulator** | The PS4 code runs natively on x86-64, inside a general-purpose OS and library emulation layer | shadPS4, fpPS4, Obliteration | Shared by every game |
| **Native port / compatibility layer** | The same native code, with the eboot linked offline and a *minimal, game-specific* runtime (Wine + DXVK, but for one title) | bloodborne_pc (bbport), universal_ps4_native | Medium, for one game |
| **Decompilation / remake** | Rewritten source code | OpenGOAL, Ship of Harkinian, PsyDoom | Very high |

The PS4 CPU is an AMD Jaguar, so it is x86-64. This means **no CPU translation or recompilation is needed**. The game's machine code runs directly on the PC CPU, and the work is all in:

1. **Loading and linking** the decrypted `eboot.bin` and the game-bundled PRX modules into host memory.
2. **Re-implementing the Sony system libraries** (HLE) the game imports: kernel, threads, filesystem, pad, audio, video out, save data, and so on.
3. **Translating the GPU work**: PM4 command buffers and GCN shaders become Vulkan and SPIR-V.
4. Smoothing over CPU and OS differences: TLS through `%fs`, SSE4a instructions that Intel CPUs lack, and the fixed address-space layout.

This project follows approach #2.

---

## 2. Reference projects

### 2.1 bloodborne_pc / bbport — the inspiration
<https://github.com/deadinside28/bloodborne_pc>

- Native Linux port of Bloodborne (CUSA03173 v1.09). The authors call it "experimental, playable".
- **CPU:** the eboot runs directly on the host CPU, with no emulation and no instruction translation. The decrypted eboot is converted **offline into a flat memory image**, with the game's own PS4 `libc` and `libSceFios2` linked in as native code.
- **HLE runtime, about 5k LOC:** implements only what Bloodborne calls. That covers memory, threads, sync, files, audio (including ATRAC9), pad, saves and AppContent.
- **GPU:** vendored shadPS4 `video_core` and `shader_recompiler`, with about 200 changes marked `bbport:`. It adds multithreaded PM4 processing (one thread decodes, another records draws) and FSR 3.1 / 4 / 4.1.1 temporal upscaling.
- **Memory models:** the default copies resources into VRAM and tracks writes with page protection, as shadPS4 does. An experimental "PC model" keeps data in system RAM and promotes hot resources to VRAM.
- **Roadmap idea:** translate `GnmDriver` calls straight to Vulkan instead of emulating the command processor, in the style of DXVK.
- **Toolchain:** GCC, CMake, Ninja, Python 3, glslang, SDL3, Vulkan headers, and an optional `shell.nix`.
- **Layout:** `build.sh`, `docs/`, `gpu/shadps4/{common,core,shader_recompiler,video_core}`, `gpu/patches/fsr-vulkan`.
- **License:** GPL-2.0-or-later, inherited from shadPS4. Users supply their own dump; no game files are included.

### 2.2 shadPS4 — the parent emulator
<https://github.com/shadps4-emu/shadps4>

- C++23, Vulkan, SDL3, GPL-2.0. Runs on Windows, Linux and macOS (Apple Silicon only).
- HLE of the PS4 libraries, native x86-64 execution, and a GCN → SPIR-V shader recompiler whose design is inspired by yuzu's Hades.
- Some firmware modules must be dumped from a real console into `sys_modules/` and loaded as LLE. Examples are `libSceNgs2`, `libSceUlt`, `libSceFont` and `libSceAudiodec`.
- Already solves the Windows-specific problems (TLS, exception handling, memory reservation), which matters because we target Windows too.

### 2.3 universal_ps4_native — the generalized version
<https://github.com/mashedpotato0/universal_ps4_native>

- A title-agnostic generalization of the bbport model, for Linux.
- `link_modules.py` re-links `eboot.bin` and the bundled `.prx` files into a native executable.
- Rewrites TLS access from `%fs` to `%gs` so it does not collide with glibc's TLS, and keeps all guest memory under 1 TiB (40-bit) to match the PS4 GPU descriptor encoding.
- A `--permissive` mode stubs unhandled syscalls, which helps when bringing up new titles.
- **License CC BY-NC-SA 4.0.** This is *not* GPL-compatible. Treat it as reading material only and do not copy its code.

### 2.4 AjncSystem's SotC shadPS4 build — the most important find
<https://github.com/AjncSystem/ShadPS4-ShadowoftheColossus-Build-customizado-97-corrigida>

- A GPL fork of shadPS4 (base commit `259e815a`) targeting **exactly our dump: CUSA08809 EU v1.01**, on Windows 11.
- 29 fixes, applied one commit at a time and described by the author as "97% fixed". Open-world performance went from 13–15 fps to **17–18 fps**.
- Needs `libSceNgs2.sprx` and `libSceUlt.sprx` from the user's own PS4.
- This is our **reference oracle** and our **map of what SotC stresses**:

| # | Area | Symptom → fix | Scope |
|---|---|---|---|
| 01 | CPU | Crash at about 35 s on Intel CPUs (red zone wiped) → relocate short SSE4a instructions | generic |
| 02 | CPU | Silent exit → fault handler on an alternate stack | generic |
| 03 | Threads | Crash when loading a save → consistent thread/fiber stack protection | generic |
| 04 | Memory | Stale GPU data and TDR → reapply page protection after `SplitRegion` | generic |
| 05 | CPU | Crashes in 3 game functions → selective red-zone protection | **SotC** |
| 06 | Vulkan | Stalls → stage mask on wait semaphores | generic |
| 07 | Vulkan | Hangs forever on device loss → handle the loss | generic |
| 08 | Vulkan | Crash when skipping the intro video → depth format conversion through a buffer | generic |
| 09 | Vulkan | Crash in the first frames → stop pipeline stage info leaking between cached pipelines | generic |
| 10 | Vulkan | Stale data → fence coherence for indirect and vertex reads | generic |
| 11 | Vulkan | Crash on 8 GB cards → degrade gracefully when VRAM runs out | generic |
| 12 | Memory | Freeze → remove a deadlock between the fault handler and the texture cache | generic |
| 13 | GPU | Boot crash with a large cache → bigger SRT walker buffer | generic |
| 14 | Shader | Wrong reductions on NVIDIA → wave64 ReadLane on 32-wide subgroups | generic |
| 15 | Shader | Lighting stripes → wave64 uniform branches and LDS barriers | **SotC** |
| 16 | Shader | Dark rectangles on the ground → early depth tests | **SotC** |
| 17 | Shader | Black blocks (depth of field) → runtime-indexed ReadConst | generic |
| 18 | Shader | Asserts and hangs → V_MAD_LEGACY, garbage descriptors, loop budget | generic |
| 19 | Perf | 13–15 → 17–18 fps from periodic command submission (`SOTC_FLUSH_EVERY`) | **SotC** |
| 20 | Perf | Batched readbacks (`SOTC_RB_HOT`) | generic |
| 21 | Perf | Clean SRT walker reads (`SOTC_CLEAN_READS`) | generic |
| 22 | Perf | Readback ahead (`SOTC_RB_AHEAD`) | generic |
| 23 | Image | Shader patch: less motion-blur ghosting and a less blown-out sun | **SotC** |
| 24 | Shader | Temple light shafts wiped out → negated compares with NaN | generic |
| 25 | Shader | White flashes at the bird lake → instance ID relative to the draw start | generic |
| 26 | Shader | Wrong auto-exposure → subgroup barriers in LDS reductions | **SotC** |
| 27 | Memory | Heap corruption after a few minutes → track stack pages as whole units; stop GPU readbacks writing to the stack | generic |
| 28 | Memory | Corrupted memory → GPU readbacks that span several mappings | generic |
| 29 | Vulkan | Crash in the opening cutscene → null descriptor for buffers at unmapped addresses | generic |

**What this tells us about the game:** it runs heavy compute work (wave64 reductions, LDS, auto-exposure), relies on GPU→CPU readbacks, puts real pressure on VRAM, and uses Jaguar-only SSE4a instructions. The GPU side is where most of the work is.

### 2.5 Upstream shadPS4 compatibility status
- [#194 CUSA08034 (US)](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/194): "Boots" on v0.7.0. Hangs after the splash screen, with `InvalidFileFourCC` errors loading `$/materials/!cookedshaders/version57/pbr/.../_ps4/*.csdr`, vertex explosions and `vk_scheduler` errors.
- [#2442 CUSA08809 (EU)](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/2442): "Nothing" (black screen) on v0.15.0, as of April 2026.
- Other trackers: [fpPS4 #492](https://github.com/red-prig/fpps4-game-compatibility/issues/492) and [Obliteration #36](https://github.com/obhq/compatibility/issues/36).

---

## 3. Shadow of the Colossus technical profile

| Item | Value |
|---|---|
| Developer / engine | Bluepoint Games, Bluepoint Engine |
| Release | February 2018, PS4 |
| IDs | CUSA08809 (EU, **ours**), CUSA08034 (US); Asian releases also exist |
| PS4 Pro | Cinematic mode (30 fps, dynamic 4K) and Performance mode (targets 60 fps) |
| Heritage | Gameplay is built on the **original PS2 game code**. Bluepoint wrapped it so it could talk to the new renderer. The original physics depends on frame rate, and collision is sensitive to geometry changes. |

**Port implications of that heritage**
- Unlocking the frame rate is risky because physics, animation and collision may be tied to the frame step. The safe first target is the Pro **Performance mode** (60 fps), which the game already supports. A per-console "is Pro" flag (`sceKernelIsNeoMode`) in the HLE could switch it on.
- Game logic may be easier to reverse than usual, because it descends from code the community already knows well from the PS2 game.

**Data and asset formats** (from [ReSHax #589](https://reshax.com/topic/589-shadow-of-the-colossus-remake-ps4-models/))
- Archives: `cachedFiles.psarc`, plus `pak0.psarc` up to `pakN.psarc`. One archive is **over 12 GB**, so file I/O must be 64-bit everywhere.
- Models: `.cmdl` (container), `.cmsh` (vertices), `.cclm` (layout, `MLOC` footer), `.cskl` (skeleton, `LEKS` footer).
- Textures: `.ctxr` and `.ctxc` (the top mip), stored with the PS4 tiling/swizzle layout.
- Shaders: `.csdr` cooked GCN shaders under `materials/!cookedshaders/version57/`. Their custom FourCC is what trips upstream shadPS4.
- Compression: **Oodle Mermaid** (`0x8C0A` header). The decompressor is most likely statically linked into the eboot. If so, we just run Bluepoint's own code and need no Oodle license or reimplementation. This still needs confirming in Ghidra.
- Community tools: UnPSARC v1.5, the QuickBMS `brink.bms` script, Aqua Toolset (cmdl → FBX) and RawTex.

**Firmware modules we know of:** `libSceNgs2.sprx` (audio) and `libSceUlt.sprx` (user-level threads/fibers), both confirmed by AjncSystem. The intro video probably also needs the video decoder or AvPlayer path. The complete list comes from the Phase 2 import scan.

---

## 4. Prerequisites checklist

**Legal and source material (all from your own hardware and purchase)**
- [ ] A PS4 on a jailbreakable firmware, running GoldHEN.
- [ ] Your own copy of SotC, **CUSA08809 v1.01** (base game plus the 1.01 patch).
- [ ] A game dump made with the Homebrew Store / GoldHEN dumper (see the [ConsoleMods guide](https://ConsoleMods.org/wiki/PS4:Ripping_PS4_Game_Backups)). It must include the **decrypted** `eboot.bin` and `sce_module/*.prx`.
- [ ] Firmware modules dumped from the same console: `libSceNgs2.sprx`, `libSceUlt.sprx`, and whatever else Phase 2 turns up.
- [ ] An exFAT USB drive at least twice the size of the game.

**PC**
- [ ] A Vulkan 1.3 GPU. Compute-heavy workloads mean NVIDIA (wave32) gets the extra shader fixes; AMD RDNA is the closest match to the PS4. You need 8 GB of VRAM or more.
- [ ] A fast SSD, about 50 GB free for the dump.
- Windows: Visual Studio 2022 (clang-cl recommended, as shadPS4 uses), Vulkan SDK, CMake, Ninja, Git, Python 3.
- Linux: GCC 13+ or Clang 17+, CMake, Ninja, Vulkan headers and loader, glslang, SDL3, Python 3.

**Reverse-engineering tools**
- Ghidra with a PS4 ELF/PRX loader plugin, used for imports by NID and for finding game functions (frame step, fps limiter, resolution, input).
- RenderDoc for Vulkan frame captures, to compare against the AjncSystem baseline.
- shadPS4's NID → symbol tables, to name imports.

---

## 5. Proposed architecture

```
shadow-pc/
├─ CMakeLists.txt              # one CMake build for Windows and Linux
├─ LICENSE                     # GPL-2.0-or-later
├─ docs/                       # RESEARCH.md, imports.md, roadmap, per-phase notes
├─ tools/
│  ├─ elfscan/                 # lists eboot/prx imports by NID → library/function
│  └─ linker/                  # offline: eboot + game PRX → flat image + metadata
│                              #   relocations, NID resolution, TLS fixups, SSE4a patch sites
├─ runtime/                    # the game-specific HLE layer (bbport-style, kept minimal)
│  ├─ kernel/                  # memory (direct/flexible), threads, sync, time, events
│  ├─ fs/                      # /app0 mapping, Fios2, 64-bit psarc I/O
│  ├─ pad/  savedata/  appcontent/  videoout/  audio/ (Ngs2, ATRAC9)  ult/
│  └─ nid_table.*              # only what SotC imports
├─ platform/
│  ├─ common/                  # SDL3 window/input, logging, config
│  ├─ win/                     # VEH fault handling, VirtualAlloc2 placeholders, TLS patching
│  └─ linux/                   # signals + sigaltstack, mmap MAP_FIXED_NOREPLACE, %fs→%gs
├─ gpu/
│  ├─ shadps4/                 # vendored video_core + shader_recompiler (+ common)
│  └─ patches/                 # AjncSystem fixes, ported as clearly labeled commits
└─ scripts/                    # run.ps1 / run.sh, reading SOTC_GAME_DIR and SOTC_SYSMOD_DIR
```

**Cross-platform notes**
- **TLS:** PS4 code reads TLS through `%fs:0`. On Linux, either use `%gs` for the host (bbport and universal_ps4_native style), or patch guest `fs:` accesses. On Windows `%fs` isn't usable from user mode (x64 uses `%gs` for the TEB), so guest `fs:` accesses must be patched or trapped, as shadPS4 does. **Reuse shadPS4's approach for both platforms** so there is one code path.
- **SSE4a** (`EXTRQ`/`INSERTQ`): Jaguar has it and Intel does not. Patch the instructions at link time; AjncSystem fix 01 shows the red-zone trap to avoid.
- **Address space:** reserve the PS4 layout at fixed addresses, keeping GPU-visible memory under 40 bits. Use `mmap` with `MAP_FIXED_NOREPLACE` on Linux and `VirtualAlloc2` placeholders on Windows.
- **Faults:** GPU write-tracking depends on page-protection faults. Use signals plus `sigaltstack` on Linux and a vectored exception handler on Windows. The handler must not deadlock with the texture cache (fix 12).
- **Linking strategy:** load the offline image at runtime (bbport) instead of emitting a host PE/ELF. This avoids PE relocation and section-alignment limits on Windows.

---

## 6. Phased roadmap

| Phase | Work | Exit criterion |
|---|---|---|
| **0. Prerequisites** | Dump the game, patch and firmware modules from your own PS4 | Decrypted `eboot.bin`, `sce_module/`, `sys_modules/` on disk |
| **1. Baseline** | Build the AjncSystem fork on Windows and play through key scenes | Reaches gameplay; logs, screenshots and fps recorded for the title screen, Colossus 1, bird lake and temple light shafts |
| **2. Recon** | `tools/elfscan`: import list by NID, grouped by library; Ghidra pass for frame step, fps cap, `IsNeoMode`, resolution, `.csdr` loader | `docs/imports.md` with an HLE checklist and LLE module list |
| **3. Skeleton** | CMake, platform layer, offline linker, runtime that loads the image and jumps to the entry point; permissive stubs that log unknown NIDs | Game reaches `main` and runs its first threads on both OSes |
| **4. Boot to menu** | Filesystem and psarc, Fios2, VideoOut, intro video, shader loading past `InvalidFileFourCC` | Title menu renders |
| **5. Graphics parity** | Vendor shadPS4 GPU core and port the 29 AjncSystem fixes | Screenshots match the baseline at the fixed scenes |
| **6. Audio, input, saves** | Ngs2 (LLE first), ATRAC9, SDL3 gamepad plus keyboard and mouse, save data | A full colossus fight with sound and a working save and load |
| **7. PC features** | Resolution scaling, FSR, Pro Performance mode, multithreaded PM4; fps unlock only after frame-step analysis | Beats the 17–18 fps baseline by a wide margin; stable 60 fps in Performance mode |

---

## 7. Risks

- **Legal:** stay GPL-2.0-or-later, which is required because we vendor shadPS4. **Never** commit or distribute game files, firmware, keys or Sony SDK material. Do not copy code from universal_ps4_native (CC BY-NC-SA); use it only as reading material.
- **GPU performance:** even the tuned emulator only reaches about 17–18 fps, so emulating PM4 and doing readbacks is expensive. Real gains probably need bbport-style multithreaded PM4, fewer readbacks, and eventually GnmDriver → Vulkan translation.
- **Frame-rate coupling:** gameplay with PS2 heritage may break above 30 or 60 fps. Mitigate with Performance mode first, then decouple carefully using findings from Ghidra.
- **Bluepoint's custom `.csdr` shaders** are the upstream blocker. AjncSystem gets past them, so study that fork first.
- **Audio:** Ngs2 is complex. Start with the LLE `.sprx`, which needs Ult fibers working, and consider HLE later.
- **Windows:** TLS patching, SSE4a and the fault-handler cases have each caused crashes before (fixes 01–05 and 27).
- **Scope creep:** keep the HLE surface limited to SotC's import list, as bbport does.

---

## 8. Sources

- bloodborne_pc (bbport): <https://github.com/deadinside28/bloodborne_pc>
- shadPS4: <https://github.com/shadps4-emu/shadps4>
- universal_ps4_native: <https://github.com/mashedpotato0/universal_ps4_native>
- AjncSystem SotC shadPS4 build: <https://github.com/AjncSystem/ShadPS4-ShadowoftheColossus-Build-customizado-97-corrigida>
- shadPS4 compatibility, CUSA08034: <https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/194>
- shadPS4 compatibility, CUSA08809: <https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/2442>
- fpPS4 compatibility #492: <https://github.com/red-prig/fpps4-game-compatibility/issues/492>
- Obliteration compatibility #36: <https://github.com/obhq/compatibility/issues/36>
- ReSHax, SotC remake models and formats: <https://reshax.com/topic/589-shadow-of-the-colossus-remake-ps4-models/>
- Game Informer, How Bluepoint rebuilt SotC for PS4: <https://www.gameinformer.com/b/features/archive/2017/12/15/how-bluepoint-rebuilt-shadow-of-the-colossus-for-ps4.aspx>
- PlayStation Blog, Masters of the Remaster: <https://blog.playstation.com/2018/01/12/masters-of-the-remaster-inside-bluepoint-games/>
- Gematsu, PS4 Pro enhancements: <https://gematsu.com/2017/12/shadow-colossus-special-edition-ps4-pro-enhancements-announced>
- ConsoleMods, Ripping PS4 Game Backups: <https://ConsoleMods.org/wiki/PS4:Ripping_PS4_Game_Backups>
