# M3 workstream S: stack and fault safety

## Files

| File | Change |
|---|---|
| `runtime/src/platform_win_fault.c` | The VEH runs the runtime's handler on a per-thread **emergency stack**. The nested-fault report is unchanged. The VEH is registered only once. |
| `runtime/src/platform_posix_fault.c` | The same API on top of `sigaltstack`: `PLAT_FAULT_STACK_SIZE`, init/exit, freeing. |
| `runtime/src/platform.h` (shared; faults section only) | `PLAT_FAULT_STACK_SIZE`, `plat_fault_thread_init/exit`, `plat_fault_on_emergency_stack`. **Merge at integration.** |
| `runtime/src/hle/thread.c` | Fixes on the thread-exit path; emergency stack per guest thread (see below). |
| `runtime/src/hle/fiber.c` | Red-line stack guard, room check, `hle_fiber_thread_exit`. |
| `runtime/src/sse4a.c` | Comment only: the Windows finding and the route to SSE4a emulation on Intel. |
| `tools/sotcconv/codescan.py` | Red-zone analysis, trampoline builder `redzone_patch()`, and report mode `python codescan.py --redzone [--list] files...`. `scan()` is unchanged. |
| `runtime/tests/test_fault.c` | New: proof, costs, emergency stack, thread exit. |
| `docs/m3/s.patch` | `sotcconv.py` change that turns on the red-zone trampolines. **Apply at integration, then reconvert.** |

## 1. Red zone

### Proof (`test_fault.c`, Windows 10 19045, Ryzen 5 3600)

A SysV leaf function written in asm fills the red zone (16 qwords below rsp), then stores to a read-only page. The handler makes the page writable and retries. The test then counts how many of the 16 qwords survived.

| Case | Qwords intact |
|---|---|
| Bare store, clean AVX state | **16** |
| Bare store, upper ymm dirty (`vcmpeqps ymm1,ymm1,ymm1` before the store) | **5** |
| Store wrapped in `lea rsp,[rsp-128]` … `lea rsp,[rsp+128]` (both AVX states) | 16 |

The kernel does write the exception frame below rsp, but with a gap. A dump of the 1 KiB below rsp shows the layout:
- CONTEXT starts at rsp-0x718/0x728, EXCEPTION_RECORD at rsp-0x228/0x238, and the machine frame at about rsp-0x198.
- Above those comes the XSAVE area. With the AVX state clean (XSTATE_BV bit clear), XSAVEC doesn't write the YMM component, so the bytes from rsp-0x130 to rsp survive.
- With live upper-ymm state, the 256-byte YMM area is written up to **rsp-0x10**, and the red zone is gone.

So the AjncSystem symptom is real. It is limited to faults taken while the AVX state is dirty, but that is common in game code built for Jaguar's AVX. This conclusion only holds for this Windows build and CPU; other xstate layouts (AVX-512, CET) differ, so the fix doesn't rely on the gap.

**Can the handler run on a separate stack before the kernel writes the frame? No.**
- Windows has no sigaltstack. KiDispatchException always builds the frame on the faulting thread's user stack.
- VEH, SEH and the instrumentation callback all run after that, on that stack.
- The test shows the clobber happens even though the runtime handler now runs on the emergency stack: the AVX case still reads 5/16 with the switch in place.
- Only a debugger (another process) sees first-chance exceptions before the user frame is built. That would cost a cross-process round trip per fault (tens of µs), against about 1.5 µs now.

### Counts (`python tools/sotcconv/codescan.py --redzone --list`, the user's v1.00 dump)

The model is shadPS4's static red-zone protection (cpu_patches.cpp), ported to capstone:
- liveness of the 128 bytes below rsp, translated across push/pop/add/sub/lea rsp, solved backwards over the control flow
- functions with jump tables, unknown rsp writes or an escaping red-zone address count as live everywhere
- "risky" means a memory access through a non-stack, non-rip pointer while some red-zone byte is live

That is exactly what a GPU page-tracking fault, or a split-mapping retry, can hit.

| Module | Functions | Use the red zone | Risky functions | Risky accesses (may write) | In ymm-using functions |
|---|---|---|---|---|---|
| eboot | 55,790 | 282 (all leaf; 4 conservative) | 282 | 9,779 (2,492) | 42 functions |
| libc | 2,463 | 27 | 5 | 47 (17) | 0 |
| Fios2 | 731 | 5 | 5 | 291 (102) | 1 |
| **Total** | 58,984 | **314** | **292** | **10,117 (2,611)** | 43 |

Each of the 292 functions both keeps live data in the red zone and dereferences other pointers while it does. Any of those pointers that lands on a GPU-watched page can fault there.

### Fix: conversion-time trampolines (`docs/m3/s.patch`)

Each risky access is moved into a trampoline. The original bytes become `jmp rel32` plus int3 padding, and the trampoline is:

```
lea rsp,[rsp-128]   ; red zone is now above rsp: the kernel frame lands below it
<the access>         ; rip-relative displacements re-encoded
lea rsp,[rsp+128]
<displaced instructions to make up 5 bytes>   ; jcc/jmp re-encoded as rel32
jmp back
```

`lea` leaves the flags alone. A fault inside the trampoline is handled and retried in place.

What stays out of trampolines:
- calls, so C++ unwinding never meets a trampoline without an FDE
- fs/SSE4a patch sites, and relocated words
- spans that would cover a branch target; the span may start up to 4 instructions earlier to avoid one

All trampolines go into one extra R+X segment after the last module. **The loader, `bootimg.h` and the format are unchanged**: the loader already maps, copies and protects any segment. `--no-redzone` turns it off.

Results:
- **10,033 of 10,117 accesses protected (99.2%)**, in 9,035 trampolines (235,190 bytes, +0.24 MB image).
- 84 are left: 46 in functions with jump tables, 36 next to branch targets, 2 stack-dependent. `--list` prints their addresses (eboot: 0x1bda10, 0x8d6f79, …, mostly in a few functions around 0xe6a000).
- Every trampoline was checked by disassembly: same instructions, same resolved branch/rip targets, and a correct jump back. 9,035 checked, 0 mismatches.
- `test_fault` proves the wrapped access keeps the red zone with a dirty AVX state.

Cost:
- **Trampoline overhead**: an access that doesn't fault takes 19.1 ns bare and 17.8 ns wrapped in the microbenchmark, i.e. the `lea` pair is lost in the noise. The extra two jumps cost a few cycles per protected access executed.
- **Game**:

  | Runtime and image | Frames |
  |---|---|
  | Baseline | 360/480/480 |
  | New runtime, plain image | 360/480/420 |
  | New runtime, trampolined image | 420/360/480 |

  Each column is one of three interleaved 40 s runs of the same build, on a machine shared with the other agents. The differences are within noise.
- **120 s smoke with the trampolined image**: still running, 1260 frames, 0 unimplemented, no errors.

Rejected alternatives:
- **Write-watch (`MEM_WRITE_WATCH` / `GetWriteWatch`)**: only for `VirtualAlloc` private memory. GPU memory here is direct memory, which is views of a pagefile-backed section, so it can't be write-watched.
- **A handler on a separate stack**: impossible, see above.
- **Restoring the red zone after the fact**: impossible, the bytes are gone before user mode runs.
- **Whole-function rewriting**: needs every rsp-relative access and every push/pop in the function adjusted. Per-access trampolines give the same protection with local, verifiable edits.

Residual risk: the 84 unprotected sites, and faults in guest code outside `.eh_frame` functions. Those can still lose the red zone if they fault with a dirty AVX state. If a crash report ever points at one of the listed addresses, resolving that function's jump table is the next step.

## 2. Emergency stack (AjncSystem #02)

- `veh()` handles the exception codes it cares about and checks for nesting, both on the faulting stack, then calls `call_on_stack(dispatch, &f, top, limit)`, an asm thunk:
  - it switches rsp to the thread's emergency stack and points TEB StackBase/StackLimit at it, so `__chkstk` in large host frames probes the right stack
  - it calls main.c's `on_fault` (GPU bridge, SSE4a, memory retry, report)
  - it switches back
- The thunk has SEH unwind codes (rbp frame), so an unwinder can walk out of it.
- **Per thread**: 256 KiB reserved = a 4 KiB no-access guard plus 252 KiB committed.
  - `thread.c` allocates it when a guest thread starts, and frees it when the thread exits.
  - The main thread gets it in `plat_install_fault_handler`. Any other host thread gets one lazily on its first fault.
  - If allocation fails, the handler runs in place, as before.
- **POSIX**: the same API on `sigaltstack`, which already existed per thread. It is now also freed at thread exit.
- **Fast path**: the switch costs **2.4 ns** against about 1.5 to 2.2 µs for a handled fault, so every fault uses it, including handled page-tracking faults.
- **Proof** (`test_fault`): a fault taken with only 4 KiB left on a 16 KiB stack, above a no-access page, is handled by a handler with a 64 KiB frame. This used to be a silent process death.
- What still runs on the faulting stack:
  - the kernel frame (about 0x720 to 0x900 bytes, depending on xstate)
  - ntdll's dispatcher
  - `veh()`'s small frame

  A stack with less than about 2 KiB left still can't take a fault.

## 3. Fibers

- **Red line**: `_sceFiberInitializeImpl` fills the lowest 512 bytes above the stack signature with the size-check pattern; at most half of a small stack is used for this.
  - Every switch away (`sceFiberSwitch`, `sceFiberReturnToThread`) checks the red line along with the signature.
  - The first damage logs, once per fiber, `fiber 'X': stack use came within N bytes of the end`. This comes before a real overflow can silently corrupt the memory below the stack (fiber stacks have no guard page).
- **Room check**: the fiber HLE entry points warn once if they start with less than 2 KiB of fiber stack left.
  - `fiber_room()` and `fiber_check_room()` in `fiber.c` could be exported through `hle.h` (shared) for evflag/sync/log paths.
- **`hle_fiber_thread_exit()`**: pthread_exit from inside sceFiberRun:
  - drops the fiber state
  - restores the thread's TEB StackBase/StackLimit/DeallocationStack before the longjmp
  - marks the fiber Idle
- 120 s smoke: no red-line or room warnings, so the game's 16 KiB job fibers stay clear of the bottom 512 bytes.

## 4. Threads

Fixed:
- **Stale registers after `pthread_exit` (real bug).** After `__builtin_longjmp` only rbp and rsp are restored, and clang had cached the address of `t_self` in rbx across the setjmp. A pthread_exit from code that changed rbx (here, a fiber) then crashed in `thread_body`. M2's `t = t_self` reload didn't help, because the address of `t_self` itself was in rbx. Everything after the setjmp now lives in noinline `thread_finish()` / `main_thread_finish()`. `test_fault` reproduced the crash and now passes.
- **No exit point.** pthread_exit on a thread with no exit point (a host thread, a key destructor, or the main thread before its setjmp) used to jump through an empty buffer. It now reports `log_fatal`. The guest main thread arms its exit point before the libc/Fios2 module inits, and its exit runs the key destructors.
- **Bad join/detach.** A second join, or a join or detach of a detached thread, used a freed or closed host handle. Join and detach now take `g_lock` and return EINVAL. The guest main thread counts as detached for the guest, because its host handle belongs to `host_main_loop`.
- **Leak.** The TCB and DTV of an exited thread were never freed. They are now (`tls_destroy`), together with the emergency stack. `guest_thread` stays, as the handle others join on. The main thread keeps its TCB.

## Tests and smoke

- `ctest`: 12/12 pass, including the new `test_fault`. Both Python suites pass, with and without `s.patch` applied.
- `test_fault` output:
  ```
  red zone (16 qwords) after a handled fault: bare store 16 intact, rsp lowered by 128 16
    with dirty upper ymm (AVX state in use): bare store 5, rsp lowered by 128 16
  store without a fault: 19.11 ns bare, 17.83 ns with rsp lowered around it
  handled write fault: ~3 us each (protect + fault + unprotect); fault alone ~1.5-2.2 us
  emergency-stack switch (incl. TEB bounds swap): 2.4 ns per fault
  fault with 4 KiB of a 16 KiB stack left, 64 KiB handler frame: handled
  pthread_exit from a fiber: joined, value 0x1234
  ```
- `smoke.py --seconds 120`:

  | Image | Result |
  |---|---|
  | Plain `boot.img` | still running, 1380 frames, 0 unimplemented, one intermittent `pm4: packet … overruns buffer` |
  | `s.patch` image | still running, 1260 frames, 0 unimplemented, no errors |

  The pm4 overrun and an occasional `videoout: flip queue full` also show up in the unmodified HEAD runtime, built separately and run interleaved, so they predate this work. They belong to the GPU/pacing workstreams.

## Integration steps

1. Merge the `platform.h` faults-section additions.
2. `git apply docs/m3/s.patch`, then rerun `scripts/convert.*`. The report gains a `red zone:` line.
3. Optional: declare `fiber_check_room` in `hle.h` and call it from HLE paths that fibers reach.
