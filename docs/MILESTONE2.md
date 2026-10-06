# Milestone 2: the main loop runs (no rendering, no sound)

**Goal:** the game reaches its main loop and keeps running without deadlocks:
- it submits frames to a **null GPU** that completes work instantly
- every non-GPU import is implemented, or deliberately stubbed with `HLE_STUB_OK`

**Exit criteria**
- `python scripts/smoke.py --seconds 120` reports `still running`, with `frames` increasing and no errors.
- The `unimplemented` list is empty, apart from rendering-only GnmDriver calls agreed at integration.
- `ctest --test-dir build` and both Python suites pass.

**Starting point** (M1, from `scripts/smoke.py`): the game hangs in `sceKernelWaitEventFlag`. The first unimplemented imports, in call order:
1. `sceLibcHeapGetTraceInfo`
2. `sceGnmGetTheTessellationFactorRingBufferBaseAddress`
3. `sceKernelConvertUtcToLocaltime`
4. DiscMap `Func_7C980FFB0AA27E7A`
5. `sceKernelCreateEventFlag`
6. Fiber: OptParamInitialize, InitializeImpl, GetInfo, Rename, Run
7. `sceCommonDialogInitialize`
8. `sceNetInit`
9. `sceUserServiceInitialize`
10. `sceShareUtilityInitializeEx2`
11. `sceKernelPollEventFlag`, `sceKernelSetEventFlag`
12. `sceGnmRegisterOwner`, `sceGnmMapComputeQueue`
13. `sceKernelWaitEventFlag` (hangs here)

The full import list is in `build/game/boot.txt`.

## Workstreams: run these in parallel

Every workstream:
- runs as its own agent in its own git worktree
- **creates only the files it owns**: HLE sources, a test file, and a notes file
- edits no shared file (`hle.h`, `hle.c`, `CMakeLists.txt`, `loader.c` and so on)

New `runtime/src/hle/*.c` and `runtime/tests/test_*.c` files are picked up automatically.

| WS | Scope | Owns | Needs | Size |
|---|---|---|---|---|
| **A. Kernel sync** | Event flags: Create, Delete, Set, Clear, Poll, Wait, with AND/OR, CLEAR_ALL/PAT and timeouts. Equeues and user events: CreateEqueue, DeleteEqueue, WaitEqueue, AddUserEventEdge, TriggerUserEvent, DeleteUserEvent, GetEventFilter, plus the event data accessors. Implements `equeue.h` and **deletes `hle/equeue_stub.c`**. Also `sceKernelConvertUtcToLocaltime`/`LocaltimeToUtc`, `sceKernelGetModuleInfoFromAddr`, `sceKernelInternalMemoryGetModuleSegmentInfo`, `sceLibcHeapGetTraceInfo` | `hle/evflag.c`, `hle/equeue.c`, `hle/kernel_misc.c`, `tests/test_sync.c` | — | M |
| **B. Fibers** | The 8 libSceFiber imports: `_sceFiberInitializeImpl`, OptParamInitialize, Run, Switch, ReturnToThread, GetInfo, Rename, Finalize. Hand-written context switch on the guest-supplied stack (SysV callee-saved registers, MXCSR, x87 control word); set `tcb->fiber`. **Windows:** swap TEB `StackBase`/`StackLimit` per fiber, or host `__chkstk` probes will fault. A test runs ping-pong fibers that call a host function using a large stack frame | `hle/fiber.c`, `tests/test_fiber.c` | — | M, risky |
| **C. System services** | **UserService:** one user, id 1, name from `SOTC_USER_NAME` or "Wander"; events: none. **SystemService:** status, `ParamGetInt` (language, enter button), safe area, ReceiveEvent (none), HideSplashScreen, LoadExec (logs). **Plus:** AppContent, Rtc, DiscMap, NpManager, NpTrophy (handles; unlock only logs), ScreenShot, GameLiveStreaming, VideoRecording, ShareUtility, Ime, SystemGesture, PngEnc (fails cleanly), CommonDialog. **Pad:** one connected controller, neutral input (real input in M5) | `hle/userservice.c`, `hle/systemservice.c`, `hle/np.c`, `hle/misc_services.c`, `hle/pad.c`, `tests/test_services.c` | — | M |
| **D. Dialogs + SaveData** | Msg, Error and SaveData dialogs as state machines that finish at once with result OK. SaveData stored in `SOTC_USER_DIR/savedata/<dirName>/`, using `hle_file_mount("/savedata0", ...)`: Initialize3, Mount2, UmountWithBackup, DirNameSearch, SetParam, SaveIcon, GetMountInfo, Delete, CheckBackupData, RestoreBackupData, Terminate | `hle/dialogs.c`, `hle/savedata.c`, `tests/test_savedata.c` | — | M |
| **E. Offline network** | libSceNet, NetCtl, Http, Ssl, plus the libScePosix socket calls (socket, bind, connect, send, recv, select…). Init succeeds; NetCtl reports disconnected; sockets and requests fail with the proper error codes | `hle/net.c`, `hle/http.c`, `tests/test_net.c` | — | S |
| **F. Null audio** | **AudioOut:** ports; `Output` blocks for one buffer's duration at 48 kHz, which paces the game. **Audio3d:** stubs. **Ngs2:** minimal HLE with system, rack and voice handles, QueryBufferSize, and Render producing silence. Real Ngs2 (LLE plus Ajm/ATRAC9) is M5 | `hle/audio.c`, `hle/ngs2.c`, `tests/test_audio.c` | — | M |
| **G1. GnmDriver** | All 103 GnmDriver imports. Port shadPS4's packet builders (`../shadps4/src/core/libraries/gnmdriver/gnmdriver.cpp`, GPL) so command buffers are well-formed PM4. Init and registration calls (RegisterOwner, RegisterResource, MapComputeQueue, tessellation ring…). `sceGnmSubmitCommandBuffers` / `SubmitAndFlip` / `SubmitDone` call `pm4_execute()` | `hle/gnmdriver.c`, `tests/test_gnm.c` | G2's `pm4.h` (agree on it first) | L |
| **G2. Null PM4 + VideoOut** | `pm4.h`/`pm4.c`: walk PM4 type-3 packets and apply only their memory side effects (EVENT_WRITE_EOP, RELEASE_MEM, WRITE_DATA, DMA_DATA for labels, interrupts), then trigger EOP events through `equeue_trigger(..., EVFILT_GRAPHICS_CORE, ...)`. Ignore draws. **VideoOut:** Open, RegisterBuffers, SetBufferAttribute, SubmitFlip, a 60 Hz vblank thread that triggers flip and vblank events (`EVFILT_VIDEO_OUT`), WaitVblank, GetResolutionStatus, IsFlipPending, flip and vblank event data. **Logs `videoout: frame N` at info level every 60 flips** (`scripts/smoke.py` counts frames from this) | `hle/pm4.h`, `hle/pm4.c`, `hle/videoout.c`, `tests/test_pm4.c` | — | M–L |
| **H. Linux** | Needs the user to run `wsl --install -d Ubuntu`. Build with clang, then run ctest, the Python tests and the smoke test. Fix `platform_posix.c` | `runtime/src/platform_posix.c` | WSL | S–M |

**Parallelism**
- **A, B, C, D, E, F, G1 and G2 can all start now.** That's eight agents with no shared files; G1 and G2 agree on `pm4.h` first, which G2 owns.
- **H** starts whenever WSL exists.
- **Shared types:** a workstream that needs something new in a shared header defines it locally (static) and records it in its notes; the integrator promotes it.

**Serial steps**
1. ~~Foundation~~: done. Self-registering `HLE_LIBRARY`, globbed sources and tests, `kobj.h`, `equeue.h` plus the stub, the mount API, `HLE_STUB_OK` and `scripts/smoke.py`.
2. **Integration** (one agent or the main session), repeated until the exit criteria hold:
   - merge the worktrees
   - delete `equeue_stub.c` once A is in
   - build, run ctest, `scripts/convert.ps1`, then `python scripts/smoke.py`
   - fix the first blocker in call order
   - re-run
3. Fold the `docs/m2/*.md` notes into `docs/RECON.md`, then commit.

**Expected unblock order:**
1. A (event flags) and B (fibers) are first in call order.
2. Then C (UserService), then G1 (Gnm init).
3. The main loop then needs A + G2: EOP events and flip events.

## Conventions every workstream follows

- **Read `CLAUDE.md` first.** It covers HLE conventions, error codes and the build and test commands.
- **Semantics come from** `../shadps4/src/core/libraries/<lib>/`. It's GPL-2.0 like this repo, so porting is fine; credit it in a file comment.
- **One `HLE_LIBRARY(var, "libSceX", HLE(fn), ...)` per library**, with names exactly as in `build/game/boot.txt`.
- **Unknown, harmless calls** use `HLE_STUB_OK(name)`. Anything that returns data must fill it in.
- **Tests:** `runtime/tests/test_<ws>.c` with its own `main()`; it gets built and run by ctest automatically. Call `ps4_*` functions through `hle_lookup()`, since they are `static`.
- **Notes:** write `docs/m2/<ws>.md` saying what is implemented, what is stubbed, open questions, and any shared-header change wanted.
- **Don't commit**; the integrator commits.

## Agent prompt template

> Read CLAUDE.md and docs/MILESTONE2.md, workstream **X**. Implement only the files listed as
> owned by X. Use ../shadps4 for semantics. Add runtime/tests/test_<x>.c. Build
> (`cmake --build build`) and run `ctest --test-dir build`. If the game dump is available,
> run `python scripts/smoke.py` and report how the unimplemented list changed. Write
> docs/m2/<x>.md. Don't edit shared files and don't commit.

## Later milestones

- **M3:** real GPU, using shadPS4 `video_core` and `shader_recompiler` behind a `sotcgpu_*` C interface, with an SDL3 window. Replaces G2's null executor.
- **M4:** AjncSystem's 29 SotC fixes (see `docs/RESEARCH.md` §2.4).
- **M5:**
  - Ngs2 LLE (`D:\libSceNgs2.sprx`) with an Ajm HLE using LibAtrac9
  - SDL3 pad and keyboard/mouse input
  - saves polish
- **M6:** PC features: resolution and FSR, Pro "Performance mode", a frame-rate study.
