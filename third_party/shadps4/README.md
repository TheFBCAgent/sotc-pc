# Vendored shadPS4

These files come from [shadPS4](https://github.com/shadps4-emu/shadPS4), licensed GPL-2.0 (see `LICENSE`).

- **Commit:** `14ada48a1320` ("vk_rasterizer: Apply pending fast clears on FMASK decompress (#5283)")
- **Paths:** `src/common`, `src/video_core`, `src/shader_recompiler`, `LICENSE`
- **Imported with:** `python scripts/import_shadps4.py --src ../shadps4 --commit 14ada48`

`sotcgpu` (`gpu/`) builds these sources. It replaces what they take from shadPS4's `core/` with shim headers
under `gpu/shim/`.

## Local patches

Local patches are allowed: M4's AjncSystem fixes will be patches on this copy. Every patch must be listed
below: the file, what changed, and why. Re-running `import_shadps4.py` drops them, so re-apply them
afterwards.

| File | Change | Why |
|---|---|---|
| (none yet) | | |
