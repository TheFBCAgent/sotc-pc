# Audio (workstream A)

## Findings

### Imports (`build/game/boot.txt`)

- `libSceAudioOut` (7): Init, Open, Close, Output, Outputs, GetPortState, SetMixLevelPadSpk.
- `libSceNgs2` (11): SystemCreateWithAllocator, SystemDestroy, SystemRender, RackQueryBufferSize,
  RackCreate, RackCreateWithAllocator, RackDestroy, RackGetVoiceHandle, VoiceControl, VoiceGetState,
  VoiceGetStateFlags.
- `libSceAudio3d` (11): imported, but never initialized in the logs (sndx loads it only with
  `loadAudio3d=1`).
- None of `libSceAjm`, `libSceUlt`, `libSceAudiodec` or `libSceAvPlayer` is imported. No Wwise or Fmod
  runtime is linked: the eboot's `...SoundWwise...` names are Bluepoint's loader types, mapped onto
  Sony's engine below.

### The sound engine: Sony's sndx over Ngs2

Strings in the eboot:
`C:\sound\sndx\branches\ps4sndx20170803_4500_ico_sotc_no_irreverb\runtime\source\sndx\sndx_engine_ngs2_orbis.c`,
`sndx_audioout_orbis.c`, `sndx_voice.c`, `sndx_stream.c`, `sndx_buss_fx.c`, ... It is Sony's internal
"sndx" engine (Japan Studio / team ICO), with `VOICE_ALLOC_AT9`, `ADPCM Manager`,
`Sg2ExStAdpcmOpen`, `Loaded NGS2 module`, `Started audio out service`, `Failed to create audio out
thread`. Banks and streams are `$/sound/shadow/*.sxd`, `music.sxd1`, `str_bgm.sxd1`, ... So:

1. sndx's own **audio out thread** calls `sceNgs2SystemRender` once per grain (256 frames) and hands
   the result to `sceAudioOutOutput(s)`. That call blocking is what paces it.
2. Voices are **Ngs2 sampler voices**; some are ATRAC9 (`VOICE_ALLOC_AT9`), some ADPCM. Ngs2 decodes
   ATRAC9 itself through **libSceAjm**: `libSceNgs2.sprx` imports 10 Ajm functions (see below).

Log of the last run (`build/game/logs/20261011-023954-release.log`), at 32.7 s:

```
ngs2: system ... rate=48000 grain=256 (null renderer)
audioout: open user=255 type=0 len=256 fmt=5 -> 0x20000000   main, F32 8ch
audioout: open user=255 type=0 len=256 fmt=5 -> 0x20000001   main, F32 8ch
audioout: open user=255 type=1 len=256 fmt=5 -> 0x20010008   bgm,  F32 8ch
audioout: open user=1   type=4 len=256 fmt=3 -> 0x20040011   pad speaker, F32 mono
```

No unimplemented audio imports and no audio stubs are hit.

### What the runtime did before this change

- `hle/audio.c`: AudioOut validated ports and only slept to a per-port 48 kHz deadline. The PCM
  pointer was never read. Audio3d is a null queue model.
- `hle/ngs2.c`: a null Ngs2. Handles are real and the game's allocator callbacks balance, but
  `SystemRender` writes zeros and every voice reports idle at once ("finished"). **So the game
  produces silence today**, whatever AudioOut does with it.

### `D:\libSceNgs2.sprx` (the user's own firmware module, decrypted)

`python tools/elfscan/elfscan.py D:\libSceNgs2.sprx`: libSceNgs2 1.1, 47 exports, 57 imports:
- libSceAjm (10): Initialize, Finalize, ModuleRegister, ModuleUnregister, InstanceCreate,
  InstanceDestroy, BatchJobControlBufferRa, BatchJobRunSplitBufferRa, BatchStartBuffer, BatchWait.
- libSceLibcInternal (32): math, mem/str, fopen/fread. Linking it with sotcconv resolves all 31
  function links against the game's `libc.prx` by NID (checked, see the next point).
- libkernel (15): mutexes, `pthread_self`, `__stack_chk_*`, `sceKernelGetTscFrequency` exist in
  the runtime. **Missing: `pthread_equal`, `sceKernelGetCompiledSdkVersion`, `usleep`.**

The converter already links it: `--module` paths are joined to `--game`, and an absolute path wins:

```
python tools/sotcconv/sotcconv.py --game D:\CUSA08809\uroot --shadps4 ..\shadps4 ^
  --module sce_module/libc.prx --module sce_module/libSceFios2.prx --module D:\libSceNgs2.sprx ^
  -o <scratch>\boot.img --report <scratch>\boot.txt
```

gives `module libSceNgs2 base 0x03bdc000 size 0x64000`, `11 eboot -> libSceNgs2`, `31 libSceNgs2 ->
libc (libSceLibcInternal)`, and the only new system imports are the 10 `libSceAjm` ones. The
runtime starts every module's init before the eboot (`thread.c`), so Ngs2's would run too.

`D:\libSceUlt.sprx` is not needed: neither the eboot nor Ngs2 imports Ult.

## Options

**(a) HLE Ngs2 + host AudioOut.** shadPS4's `libraries/ngs2/` is structures and stubs only: no
sampler, no mixer, no effects (every `ngs2_<rack>.cpp` is 12 lines). An HLE Ngs2 means writing
Sony's sampler (PCM, VAG/HEVAG ADPCM, ATRAC9 through a decoder), envelopes, filters, pitch, the
submixer, reverb, EQ and the mastering rack, from the undocumented `VoiceControl` parameter blocks.
A large project, and each mismatch is an audible bug.

**(b) LLE `libSceNgs2.sprx` + HLE Ajm + host AudioOut.** Sony's own mixer runs natively like the
eboot (x86-64). What the runtime must add is small and well understood: 3 libkernel functions and
an Ajm HLE (shadPS4 `libraries/ajm/`: context, instances, the batch format, ~1000 lines C++)
with an ATRAC9 decoder (LibAtrac9, MIT, what shadPS4 and bbport use). The converter already links
the module. This is what shadPS4 does for every Ngs2 game, and what `RESEARCH.md` / `MILESTONE3.md`
planned.

**(c) Anything better?** No. Replacing sndx's output (hooking its mixer in the eboot) would still
need Ngs2's voices. Audio3d is not used.

Threading and timing (both options): the game's audio thread is its own (sndx's audio out
thread); it does not wait on the frame. At 10-22 fps the main thread is slow, but the audio thread
renders 256-frame grains on its own (Ngs2 runs natively), so sound keeps its rate as long as that
thread gets CPU time. If the thread is starved the device underruns (crackles), but nothing waits on audio, so
audio can't stall the game.

## Recommendation

**(b)**, in three steps, each one game run to check:

1. **Host AudioOut (done in this change).** Every playable port gets an SDL3 stream on the
   default device; SDL mixes the ports and downmixes 8ch to the device. Output is paced by the
   device's queue, not by a timer. Nothing audible yet (Ngs2 is still null), but the
   `audioout: port N ... first non-silent buffer` / per-30 s stats lines prove the path.
2. **LLE Ngs2 + Ajm HLE without ATRAC9.** `scripts/convert.*`: add the sprx when
   `SOTC_NGS2_SPRX` is set (default off, so nothing changes for anyone without it). Add the three
   libkernel functions and `hle/ajm.c` (port of shadPS4's ajm.cpp, ajm_context.cpp,
   ajm_instance.cpp, ajm_batch.cpp; GPL-2.0, credit it). ATRAC9 jobs return the right sizes and
   silence. ADPCM and PCM voices should play.
3. **ATRAC9.** Vendor LibAtrac9 (MIT, `third_party/LibAtrac9`, as shadPS4's externals) and port
   `ajm_at9.cpp`.

Decisions for the user before step 2: link the firmware module from `SOTC_NGS2_SPRX` (it is read
in place and never copied); step 3 adds a third-party submodule (LibAtrac9, MIT).

## Step 1: what was implemented

- `runtime/src/audio_host.{c,h}` (new): SDL3 playback. `audio_host_init` runs on the main thread
  after `SDL_Init` (`window.c`, one line) and opens the default device. One stream per AudioOut
  port slot, created on first Output, never freed (a reopened slot is reconfigured), so Close can
  race Output safely. The input format is the port's (S16/F32; 1, 2 or 8 channels); PS4's
  non-STD 8-channel order (L R C LFE Ls Rs Lb Rb) is remapped to SDL's with an input channel map.
- `hle/audio.c`: ports of type main, bgm, voice, personal and aux feed their stream. Output pushes
  the buffer, then returns once the stream's queue is at most the target (`SOTC_AUDIO_LATENCY_MS`,
  default 60 ms, at least two device buffers). Below the target it returns at once, so a cushion
  builds up at the start and after a stall. If the device doesn't drain within 100 ms (device
  stalled), the port drops its queue and paces on the clock for 1 s. Pad speaker and Audio3d
  ports, `SOTC_AUDIO=0`, headless runs and failed device opens keep the old clock pacing, unchanged.
- Logs: `audio: <driver>, device ... queue N frames`, `audioout: initialized (host audio|null
  output)`, `audioout: port N (type) first non-silent buffer`, and per port every 30 s:
  `audioout: port N (main 8ch f32): B buffers, peak P, U underruns, D drops, queued Q frames`.
- `docs/ENV.md`: `SOTC_AUDIO`, `SOTC_AUDIO_VOLUME`, `SOTC_AUDIO_LATENCY_MS`.
- Test `runtime/tests/test_audio_host.c` (SDL's dummy audio driver, which consumes in real time):
  the channel maps and S16 conversion read back from an unbound stream; peaks; filling the queue
  doesn't block; 75 paced outputs take ~400 ms (measured 424), main + bgm `Outputs` too (409);
  the pad speaker stays on the clock (160 ms for 31); Close empties a reused stream.
  `test_audio` (the clock path) is unchanged and passes.

### Game run to verify step 1 (main session)

```
scripts\run.bat --label audio1 --seconds 300
```

Expect in the log: `audio: wasapi, device 48000 Hz 2 ch, ...`, `audioout: initialized (host audio)`,
the four opens at ~33 s, and the 30 s stats lines for ports 0, 1 and 8 with `peak 0.000` (Ngs2 still
renders silence) and few underruns. If a port shows a non-zero peak, sndx mixes something itself
and it should be audible. Compare fps with `--env SOTC_AUDIO=0` only if it looks different (it
shouldn't: the audio thread sleeps on the queue instead of the clock).

## Step 2 plan (next session)

1. `scripts/convert.sh` / `convert.ps1`: when `SOTC_NGS2_SPRX` is set, pass
   `--module sce_module/libc.prx --module sce_module/libSceFios2.prx --module "$SOTC_NGS2_SPRX"`.
   A sotcconv test for an absolute `--module` path.
2. libkernel: `pthread_equal`, `sceKernelGetCompiledSdkVersion` (the eboot's procparam SDK
   version), `usleep`.
3. `hle/ajm.c`: contexts (`sceAjmInitialize`), `ModuleRegister` for codec 1 (At9Dec), instances
   (flags: channels, format, codec), and the batch: `BatchJobControlBufferRa` (Reset, Initialize
   with the 4-byte AT9 config, sideband result/format) and `BatchJobRunSplitBufferRa` (split
   input/output buffers, sideband result/stream/mframe), `BatchStartBuffer` runs the batch
   synchronously (or on a worker), `BatchWait` returns its result. Layouts:
   `../shadps4/src/core/libraries/ajm/ajm.h`, `ajm_batch.cpp`. ATRAC9 run jobs: consume the
   input frames (superframe size from the config), write zeros, report `output_written`.
4. Test `runtime/tests/test_ajm.c`: build batches with the guest-side helpers and run them.
5. Game run with `SOTC_NGS2_SPRX=D:\libSceNgs2.sprx` converted image: `starting module libSceNgs2`,
   no `unimplemented libSceAjm`, the stats lines with non-zero peaks, sound from ADPCM voices.

## Step 3 plan

LibAtrac9 as `third_party/LibAtrac9` (C, MIT); port `ajm_at9.cpp` (frame decode into S16/F32,
gapless skip, multiple frames per run). No AT9 data can be committed (game data), so the unit test
checks the job plumbing with LibAtrac9's own init on a synthetic config; the sound check is a game run.
