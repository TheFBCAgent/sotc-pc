# shadow-pc

A native PC port of **Shadow of the Colossus (PS4, 2018)** for Windows and Linux, in the style of
[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). The game's own x86-64 code runs directly
on the PC CPU, a small game-specific runtime replaces the PS4 system libraries, and a GPU layer based on
[shadPS4](https://github.com/shadps4-emu/shadps4) translates the GPU work to Vulkan.

**Status:** Milestone 1. The game's own code runs on Windows: libc and Fios2 start, the eboot runs its initialization and loads its first archive data, then waits on kernel services that aren't implemented yet. No graphics yet. See [docs/RECON.md](docs/RECON.md) for the latest results and [docs/RESEARCH.md](docs/RESEARCH.md) for the plan.

Target dump: **CUSA08809 (EU) v1.01**.

## Legal

No game files, firmware or keys are included, and none will ever be accepted into this repository.
You need your own copy of the game, dumped and decrypted on your own PS4.

## Build and run

You need clang (clang-cl on Windows), CMake 3.24+, Ninja, Python 3.10+, and `pip install capstone`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl   # use clang on Linux
cmake --build build
ctest --test-dir build                                       # unit tests, no game files needed
```

Then, on Windows (PowerShell; `scripts/*.sh` are the Linux equivalents):

```powershell
$env:SOTC_GAME_DIR = 'D:\CUSA08809\uroot'   # your own dump, the folder with eboot.bin
scripts\convert.ps1                          # writes build/game/boot.img and a boot.txt report
scripts\run.ps1                              # starts the game
python scripts/smoke.py --seconds 60         # timed run + summary (unimplemented imports, frames)
```

`SOTC_LOG=3` gives more detail. `SOTC_STRICT=1` stops at the first unimplemented import.

The converter names imports using a shadPS4 checkout at `../shadps4`, or wherever `SOTC_SHADPS4` points.

## Layout

- `tools/elfscan`: inventories a PS4 ELF (imports, relocations, patch candidates).
- `tools/sotcconv`: links the eboot and bundled PRX into a boot image; `codescan.py` finds the patch sites.
- `runtime/`: the C runtime:
  - `loader.c`: image loading, relocations, import stubs, patches
  - `tls.c`, `sse4a.c`, `platform_{win,posix}.c`
  - `hle/`: the PS4 system libraries implemented so far
- `docs/`: research, recon notes and results.

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

GPL-2.0-or-later. Parts of this project will vendor shadPS4, which is GPL-2.0.
