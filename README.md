# shadow-pc

A native PC port of **Shadow of the Colossus (PS4, 2018)** for Windows and Linux, in the style of
[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). The game's own x86-64 code runs directly
on the PC CPU, a small game-specific runtime replaces the PS4 system libraries, and a GPU layer based on
[shadPS4](https://github.com/shadps4-emu/shadps4) translates the GPU work to Vulkan.

**Status:** early recon. Nothing runs yet. See [docs/RESEARCH.md](docs/RESEARCH.md) for the full plan.

Target dump: **CUSA08809 (EU) v1.01**.

## Legal

No game files, firmware or keys are included, and none will ever be accepted into this repository.
You need your own copy of the game, dumped and decrypted on your own PS4.

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
```

Needs Python 3.10+ and nothing else.

## License

GPL-2.0-or-later. Parts of this project will vendor shadPS4, which is GPL-2.0.
