# Workstream P: frame pacing (M3)

## Result
Null GPU, `python scripts/smoke.py --seconds 120`, same machine, other agents building at the same time:

| build | flips | flips/s |
|---|---|---|
| baseline (82f056e, scratch build), two 60 s runs | 480, 540 after boot | 9.4, 10.4 |
| baseline, from the task description | 1140 frames per 120 s | 9.5 |
| P (this work), 120 s | 4080 frames, 4020 flips | mean 34.8; 60.0 while loading (first ~20 s), then a flat 30.0 in every 10 s window |
| P, 60 s run | 2280 frames | mean 40 including the loading phase |

The target (>= 29) is met: gameplay sits at exactly 30.0 flips/s. That is the game's own cap (it is a 30 fps
title: `flip_rate` 1 / VSyncWindow), so nothing more is available on the null GPU. smoke ends with
"still running", 0 unimplemented. The only ERROR lines are the flip-queue flood below (a logging problem, not a
failure). One extra run died at startup with `VirtualAlloc2 ... failed (1455)` (commit limit while other agents
were running): environmental, not reproduced.

## Where the baseline lost its time
The baseline's game main thread used 1.2% of a core: it was waiting, not working. `Sleep()` and
`SleepConditionVariableCS` ran on the 15.6 ms timer tick, and every wait was rounded up to a whole millisecond:
- AudioOut: a 256-frame buffer is 5.33 ms; `plat_sleep_us` rounded it up to 6 ms and then to the 15.6 ms tick, so audio
  drained about 3x too slowly, and the game blocks on the sound thread. That matches the observed ~1/3 of 30 fps.
- The vblank thread (`videoout.c`) and timed `plat_cond_wait` had the same rounding.

This is inferred from the baseline thread CPU times and rates; I did not instrument the baseline. With
`timeBeginPeriod(1)`, deadline sleeps and a spin tail, AudioOut and vblank sleeps return within ~20 us of their
deadline (`late=` column below) and the game runs at its own rate.

## What changed
- `platform_win_thread.c`
  - `plat_tls_init()` runs `timing_init()` once: `timeBeginPeriod(1)` for the life of the process and an FLS slot for the
    per-thread timer (winmm is requested with `#pragma comment(lib)`, because CMakeLists is shared). It also runs on the
    first sleep, for tests that skip `plat_tls_init`.
  - `plat_sleep_until_ns(deadline)`: with >= 1 ms left, sleep on the thread's high-resolution waitable timer
    (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`; a plain timer before Win10 1803) aimed `slack` early. `slack` adapts:
    1.5 x a moving average of the measured timer lateness + 0.1 ms, clamped to 0.2..2 ms. The rest is a spin with
    `SwitchToThread`. With < 1 ms left it only spins.
  - `plat_sleep_us(us)` = `plat_sleep_until_ns(now + us)`.
  - `plat_cond_wait` timeouts are deadline based: whole milliseconds on the condition variable while >= 2 ms remain
    (ending >= 1 ms early), then the mutex is dropped and the thread yield-spins on a signal counter (`seq`, bumped by
    signal and broadcast) until a signal or the deadline. Before: `(us+999)/1000` plus the tick. A signal that lands
    in the spin tail may also wake another waiter, which is a spurious wakeup; callers must already tolerate those.
  - `SOTC_TIMING=1`: per-call-site wait accounting (count, wall time, mean, lateness), dumped to the log every 10 s.
- `platform_posix_thread.c`: `plat_sleep_until_ns` with `clock_nanosleep(TIMER_ABSTIME)` (not built or run here).
- `platform.h`: the one declaration.
- `hle/audio.c`: `pace_reserve` returns an absolute deadline and `pace_sleep` calls `plat_sleep_until_ns`: no per-call
  rounding and no drift.
- `tests/test_timing.c`: `plat_sleep_us` from 0 to 16.7 ms, a 60 Hz deadline loop, timed cond waits and signals, and 60
  AudioOut buffers at 5.33 ms. It uses medians, so one preempted try does not fail it. 12/12 ctest pass. Measured:
  sleeps are never early, the deadline loop's worst lateness is 249 us, 60 audio buffers take 320000 us.
- `scripts/smoke.py`: `--gpu`, `--shots N` (prints how many were saved), flips/s (mean after `--skip` seconds of boot,
  per-10 s rates, 1% low), plus `--exe`, `--threads` (per-thread CPU time, Windows) and `--timing`. Existing output
  and options are unchanged. The 1% low is exact only when videoout prints a per-flip line (request 2); without it
  the report shows the slowest 60-flip window and says so. Self-tested on synthetic logs.

## Wait accounting, 120 s null run (`SOTC_TIMING=1`)
Call sites resolved with `llvm-symbolizer --relative-address --obj=build/sotc-runtime.exe 0x<offset>`.

| site | kind | n | total wall | mean | note |
|---|---|---|---|---|---|
| `pm4.c` `queue_main` | cond | 1.33 M | 898 s | 675 us | GPU queue threads idle (several threads) |
| `evflag.c` `evf_wait` | cond | 761 k | 412 s | 541 us | guest threads idle |
| `equeue.c` `sceKernelWaitEqueue` | cond | 4132 | 120 s | 29 ms | game waits on the flip/vblank event: the 30 fps cap |
| `videoout.c:222` vblank | sleep | 7179 | 120 s | 16663 us, late 19 us | exact 60 Hz |
| `audio.c` `sceAudioOutOutputs` | sleep | 22413 | 119 s | 5316 us, late 23 us | exact 48 kHz / 256 frames |
| `thread.c` `scePthreadYield` | yield | 533 M | 72 s | 0.1 us | BPE job workers spin |
| `pm4.c` `wait_until` | sleep | 434 k | 24 s | 56 us | 50 us poll |

Thread CPU over 120 s: the 4 BPE job workers 81-95% of a core each (spin-yield loops in the game, mostly kernel time in
`SwitchToThread`), Game:Main 77%, GPU:gfx 13%, SoundSystemSndx 4%, compute queues 1-11%. Nothing is starved.

## Requests for GI's files (not edited)
1. `hle/videoout.c:252` `flip queue full` and the matching `gnm: flip of buffer N failed: 0x80290012` in `gnmdriver.c`:
   84,741 ERROR lines per 120 s (~700/s, throughout the run), because the game retries a flip while the queue is full
   (as on hardware). That is expected, not an error. Make the video-out message `log_debug`, or rate-limit it (log the
   first one, then a count every 5 s), and the gnm one likewise. Gain: a ~10 MB smaller log, less contention on the log
   lock, and smoke.py's "errors / crash" section is usable again (it counts ERROR lines).
2. `hle/videoout.c`, per-flip timestamps for a real 1% low. Where a flip request is consumed (the flip-handling code that
   sets `vs->process_time`, only when a request was dequeued), print exactly
   `videoout: flip <running flip count> at <plat_monotonic_ns()/1000> us`
   (`log_info("videoout: flip %llu at %llu us", ...)`). That is 30-60 lines/s, 4-7 k lines per 2 minutes. smoke.py
   already parses it and falls back to the `frame N` lines when it is absent.
3. `hle/pm4.c` `wait_until`: after 64 yields it polls with `plat_sleep_us(50)`. With the precise sleep that is now a 50 us
   spin (before: a 1-16 ms sleep), costing ~24 s of CPU per 120 s on the queue threads. Better: wait on a condition
   variable that the fence/label/register writer signals (keeping the deadline for `pm4_execute`), or poll at >= 1 ms
   (only waits of 1 ms or more use the timer). Expected gain: ~0.2 core on the null GPU, more once real GPU work runs
   beside the game.
4. For the integrator, not GI's file: `ps4_scePthreadYield` / `sched_yield` (`thread.c`) are plain `SwitchToThread`. The 4
   BPE workers call it ~4 M times/s each and burn 80-95% of a core. It is the game's own idle loop, so it is as on a
   PS4; a backoff after N consecutive yields in `plat_yield` would cut the power draw. I did not add one: it changes
   scheduling for every spin lock in the game, and the only gain is CPU time.

## Open issues
- The Linux path (`clock_nanosleep`) is untested (no Linux build here).
- The spin tail burns up to `slack` (0.2-2 ms, typically ~0.4 ms) per sleep of >= 1 ms and the whole of any sleep < 1 ms.
  Accepted for the accuracy; SoundSystemSndx shows 4% of a core.
- Vulkan present pacing is not measured (M3 integration). Vblank and present should agree on one clock;
  `plat_sleep_until_ns` is the primitive for it.
