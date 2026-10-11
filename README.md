# shadow-pc

A native PC port of **Shadow of the Colossus (PS4, 2018)** for Windows and Linux, in the style of
[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). The game's own x86-64 code runs directly
on the PC CPU, a small game-specific runtime replaces the PS4 system libraries, and a GPU layer based on
[shadPS4](https://github.com/shadps4-emu/shadps4) translates the GPU work to Vulkan.

**Status:** Milestone 3. On Windows (NVIDIA) and Linux (AMD, RADV) the game boots, plays the intro and
renders gameplay through Vulkan, with gamepad and keyboard input. It is not playable at full speed yet: the
frame is bound by the CPU time of the single GPU thread (about 12-13 fps at the altar on a Ryzen 5 3600 +
RTX 2060, about 22 fps in the temple on a Ryzen 5 5600 + RX 9070 XT). Sound works with your own firmware module (see "Audio").
The current plan is in [docs/NEXT.md](docs/NEXT.md); [docs/RESEARCH.md](docs/RESEARCH.md) has the background.

Target dump: **CUSA08809 (EU)**, developed against the base game (v1.00, see [docs/RECON.md](docs/RECON.md)).

## Legal

No game files, firmware or keys are included, and none will ever be accepted into this repository.
You need your own copy of the game, dumped and decrypted on your own PS4.

The program itself (the runtime and the GPU layer) contains no Sony code. Everything game-derived is made
on your machine from your own files: the converter links your dump into a local boot image
(`build/game/boot.img`), which stays on your disk and must never be shared. The same goes for the optional
firmware module in "Audio".

## Build and run

You need clang 19+ (clang-cl on Windows: MSVC can't compile the `sysv_abi` calls), CMake 3.24+, Ninja,
Python 3.10+, `pip install capstone`, and a Vulkan 1.3 driver.

```sh
git submodule update --init --depth 1        # then the same inside third_party/sirit and third_party/zydis
cmake -S . -B build-vk -G Ninja -DCMAKE_C_COMPILER=clang-cl -DSOTC_VULKAN=ON
      # Linux: -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-vk --parallel 8
ctest --test-dir build-vk                    # unit tests, no game files needed
```

Without `-DSOTC_VULKAN=ON` you get the null-GPU build (`build`): it runs the game code without graphics and
is what most C tests use.

For the fastest build, `-DSOTC_LTO=ON` adds link-time optimization and `-DSOTC_PGO=<profile>` adds
profile-guided optimization on top (about 9% faster on Windows). The profile comes from one instrumented
run of your own game, so both are opt-in: see [docs/NEXT.md](docs/NEXT.md), "Regenerating the PGO
profile". On Windows, LTO also needs `-DCMAKE_LINKER=lld-link`.

Then, on Windows (PowerShell; `scripts/*.sh` are the Linux equivalents):

```powershell
$env:SOTC_GAME_DIR = 'D:\CUSA08809\uroot'    # your own dump, the folder with eboot.bin
scripts\convert.ps1                          # writes build/game/boot.img and a boot.txt report
scripts\run.bat --label first --seconds 600  # starts the game in a window (Linux: scripts/run-vk.sh)
```

The converter names imports using a shadPS4 checkout at `../shadps4` (commit `14ada48`), or wherever
`SOTC_SHADPS4` points. Without it no import resolves and the game stops at once.

`SOTC_LOG=3` gives more detail. `SOTC_STRICT=1` stops at the first unimplemented import. All `SOTC_*`
variables are in [docs/ENV.md](docs/ENV.md).

## Audio

The game's sound engine mixes through Sony's `libSceNgs2` system library, and no usable reimplementation
of it exists (shadPS4 has the same gap). Like shadPS4, the port runs the real module from **your own PS4
firmware**: put your decrypted `libSceNgs2.sprx` in a folder and point the converter at it:

```powershell
$env:SOTC_FW_DIR = 'D:\ps4-firmware'         # the folder that holds your libSceNgs2.sprx
scripts\convert.ps1
```

The converter links the module into your local boot image and never copies it anywhere else. Without
`SOTC_FW_DIR` (or without the file) the game runs silently. Re-run the conversion after setting
or changing `SOTC_FW_DIR`. Voices and effects play; ATRAC9-encoded sounds (likely some music and
streams) are still silent until the decoder is added (`docs/next/audio.md`).

Audio settings (all optional): `SOTC_AUDIO=0` turns sound off, `SOTC_AUDIO_VOLUME` sets the
volume and `SOTC_AUDIO_LATENCY_MS` the output latency; see [docs/ENV.md](docs/ENV.md).

## Layout

- `tools/elfscan`: inventories a PS4 ELF (imports, relocations, patch candidates).
- `tools/sotcconv`: links the eboot and bundled PRX into a boot image; `codescan.py` finds the patch sites.
- `runtime/`: the C runtime:
  - `loader.c`: image loading, relocations, import stubs, patches
  - `tls.c`, `sse4a.c`, `platform_{win,posix}_*.c`, `window.c` (SDL3 window and input)
  - `hle/`: the PS4 system libraries implemented so far
- `gpu/`: sotcgpu, a C interface over shadPS4's `video_core` (vendored in `third_party/shadps4`).
- `scripts/`: convert, run and measure (`smoke.py`, `flipcpu.py`), plus debugging helpers.
- `docs/`: the plan (`NEXT.md`), environment variables (`ENV.md`), debugging (`DEBUGGING.md`), research
  and results.

## Tools

### `tools/elfscan` — import inventory (Phase 2)

Reads a decrypted `eboot.bin`, `.prx` or `.sprx`, either a plain ELF or a fake-signed SELF. It lists:
- the modules and libraries it needs
- every imported symbol, by NID
- relocation counts
- byte-pattern candidates the loader must patch: SSE4a instructions and `%fs` TLS reads

```sh
python tools/elfscan/elfscan.py path/to/CUSA08809/eboot.bin
# Name NIDs and mark which ones shadPS4 already implements:
python tools/elfscan/elfscan.py eboot.bin --shadps4 path/to/shadps4 --format md -o reports/imports.md
# Run the tests (no game files needed):
python -m unittest discover -s tools/elfscan
python -m unittest discover -s tools/sotcconv
```

## License

GPL-2.0-or-later. `third_party/shadps4` is vendored from shadPS4 (GPL-2.0); its local patches are listed
in `third_party/shadps4/README.md`.
`third_party/LibAtrac9` (the ATRAC9 decoder behind libSceAjm) is LibAtrac9 by Alex Barney, MIT
licence (`third_party/LibAtrac9/LICENSE`), as shadPS4's fork `shadps4-emu/ext-LibAtrac9` (commit `946e05a`).
