# Workstream X: permanent debug tools

Four tools that replace the temporary code used during M3 bring-up (`docs/m3/bringup.md`, "Steps A–C").
Each is off by default and costs one relaxed atomic load and a branch per hook while off.

| Tool | Trigger | Code |
|---|---|---|
| Render-target dump | `SOTC_DUMP_RT=<dir>` (+ `--shots N`) | `gpu/src/debug_dump.cpp` |
| Shader dumps | `SOTC_GPU_DUMP=1` | `gpu/src/debug_dump.cpp` (`Init`) |
| GPU counters | `SOTC_GPU_STATS=1` | `gpu/src/debug_dump.cpp` + hooks |
| Thread sampler | `python scripts/sample.py ...` | `scripts/sample.py` |

## Hooks

The interface is `gpu/src/sotc_debug.h` (namespace `SotcDebug`). Hook sites:

- `gpu/src/sotcgpu.cpp`: `Init` in `sotcgpu_init`, `Shutdown` in `sotcgpu_shutdown` (before the rasterizer
  goes), `OnFlip` at the top of `SubmitFlipInternal` (GPU thread), `OnScreenshotTick` in
  `sotcgpu_screenshot`, fault counts in `sotcgpu_handle_fault`.
- `gpu/src/shim_core.cpp`: `MemoryManager::CopySparseMemory` counts guest→GPU bytes. Every guest read
  of the buffer cache goes through it (stream-buffer binds, arena `SynchronizeMemory`, image uploads via
  `ObtainBufferForImage`).
- Listed patches (`third_party/shadps4/README.md`):
  - `vk_rasterizer.cpp`: `SotcDebug::OnDraw(...)` right after `BeginRendering` in `Draw` and
    `DrawIndirect`; `Count(Dispatch)` in both dispatch paths.
  - `vk_scheduler.cpp`: `Scheduler::Wait` counts the waits whose tick isn't signalled yet (`!IsFree`).
    The check runs only while stats are on.
- `gpu/CMakeLists.txt`: `src/debug_dump.cpp` added to `sotcgpu`.

`vk_rasterizer.h` needed no change: `OnDraw` is a template that receives `cb_descs`/`db_desc` by
reference, and the dump uses the public `GetTextureCache`/`GetRuntime`/`GetScheduler`.

## 1. Render-target dump: `SOTC_DUMP_RT=<dir>`

```sh
SOTC_DUMP_RT=build/game/rt python scripts/smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan \
    --seconds 70 --shots 60
```

- The screenshot tick (`SOTC_SHOTS`/`--shots`, i.e. `sotcgpu_screenshot`) arms it.
- The next flip starts recording. Each draw adds the images it binds as color attachments (the slots
  below `num_color_attachments` with an image) and as depth, with direct and indirect draw counts.
- The flip after that stops recording and, on the GPU thread, downloads each image: mip 0, layer 0,
  `Runtime::DownloadImage` into one reusable HostCached buffer, then `Scheduler::Finish`. It converts the
  image and writes `<dir>/dNNN_tMM_<address>_<format>.png`. A dump stalls the GPU thread for a moment.
- Each target logs one line (`dump_rt N tMM: address format WxH role draws direct/indirect min max
  [UNIFORM] -> file`) and appends one line to `<dir>/index.txt`. `UNIFORM` means every texel has the
  same bytes, i.e. nothing drew there (the A-step symptom: targets left at their clear value).
- One-shot per run by default. `SOTC_DUMP_RT_ALL=1` re-arms on every tick (dump numbers go up).
- Images destroyed between the draw and the dump are looked up by address and uid and reported
  `gone`. MSAA images are skipped.
- Converters: RGBA8 (incl. sRGB/snorm/uint), BGRA8, RGB10A2 (A2B10G10R10 and A2R10G10B10),
  R11G11B10F, RGBA16F, RGBA16, RG16F, RG16, RGBA32F, RG32F, R32F, R32U, R16F, R16, RG8, R8, D32
  (incl. D32S8), D24, D16, S8.
  - Color: clamp to [0, 1]. Alpha is dropped (RGB PNG); two-channel formats write blue = 0.
  - Single-channel float/uint and depth (R32F, R32U, R16F, D*, S8): normalized from min to max, so a
    depth buffer shows its range. The log has the real min/max.

## 2. Shader dumps: `SOTC_GPU_DUMP=1`

Sets shadPS4's `dump_shaders`. The recompiler writes IR programs (`*.irprogram.txt`), and the pipeline
cache writes SPIR-V, into `<cache_dir>/shader/dumps`. With smoke that's `build/user/gpu_cache/shader/dumps`
(`cache_dir` is `$SOTC_USER_DIR/gpu_cache`, see `runtime/src/gpu_bridge.c`).

## 3. GPU counters: `SOTC_GPU_STATS=1`

A `sotcgpu:Stats` thread logs and resets the counters once per second:

```
gpu stats (1.00s): draws 812 indirect 19544 dispatches 2011 sched-waits 0 guest->gpu 48.3 MiB faults r/w 12/6012
```

- draws / indirect: `Draw` and `DrawIndirect` calls that reached the GPU (one indirect packet counts once,
  however many draws it holds).
- dispatches: `DispatchDirect` + `DispatchIndirect`.
- sched-waits: blocking `Scheduler::Wait` (stream-buffer wraps, `Finish`, readbacks).
- guest->gpu: bytes copied out of guest memory by `CopySparseMemory`.
- faults r/w: faults the runtime routed to sotcgpu (`sotcgpu_handle_fault`).

## 4. Thread sampler: `scripts/sample.py`

```sh
python scripts/sample.py sotc-runtime.exe 15 GpuCommandProcessor JobWorker
python scripts/sample.py <pid> 10              # every thread, grouped by thread name
python scripts/sample.py --self-test           # no game: samples a child Python process
```

- Attaches to a running process by name or pid; never starts the game.
- Filters are case-insensitive substrings of the thread description (`GetThreadDescription`). shadPS4
  threads use `Common::SetCurrentThreadName` (`GpuCommandProcessor`, `sotcgpu:Present`,
  `sotcgpu:Stats`); runtime threads use `plat_thread_set_name` (`Game:Main`, the guest pthread names,
  `sotc:gpu-shots`). With no filter, each thread name is its own group.
- Each round (default every 2 ms): `SuspendThread`, `GetThreadContext` (RIP, RSP), read up to 64 KiB of
  stack (`--scan`, bounded by the stack region), `ResumeThread`. Stack words inside the exe whose
  preceding bytes are a call instruction count as return addresses. The exe image is read once from the
  target for this check.
- Naming:
  - Exe: one `llvm-symbolizer --no-inlines` batch at the end (PATH, `$LLVM_SYMBOLIZER` or
    `--symbolizer`). It takes COFF addresses as VAs, so each address is rebased onto the exe's preferred
    image base read from the PE header. The exe is linked `/BASE:0x700000000000 /DYNAMICBASE:NO`, so
    the rebase is normally zero. The self-test checks the convention: it symbolizes the entry point of a
    built `sotc-runtime.exe` and expects `mainCRTStartup`.
  - Other modules (ntdll, KERNELBASE, the Vulkan driver): nearest export below the address, read from
    the export table in the target's memory.
  - Everything else (guest code, JIT): `guest/unknown 0x...` rounded to 256 bytes.
- Output per filter: top leaf functions and top inclusive frames (leaf or on the stack, once per sample),
  as percentages of that filter's samples. `--top N` sets the list length.

## Verified here

- Both trees build; ctest passes (see the hand-off report for counts).
- `scripts/sample.py --self-test` passes: two named child threads sampled (~800 samples each in 2 s), the
  sleeper's leaf named `ntdll.dll!Zw...`, and exe symbolization of `sotc-runtime.exe`'s entry point.

## Not verified (needs a GPU run by the main session)

- `SOTC_DUMP_RT`: the PNGs, the converters on real targets, and that the dump doesn't disturb the
  next frame (images are left in TransferSrc layout; the runtime's layout tracking transitions them back).
- `SOTC_GPU_STATS` numbers against the bring-up figures (~20k indirect draws and ~2k dispatches per frame).
- `SOTC_GPU_DUMP`: dump files appear under `build/user/gpu_cache/shader/dumps`.
- `sample.py` on the game: the job worker thread names (the `JobWorker` filter in the example comes from
  the plan; check the names with `python scripts/sample.py sotc-runtime.exe 2` first), and symbol quality
  for the big exe.
