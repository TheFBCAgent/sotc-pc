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
2. **LLE Ngs2 + Ajm HLE without ATRAC9.** `scripts/convert.*`: add the sprx from the firmware
   folder `SOTC_FW_DIR` (default off, so nothing changes for anyone without it). Add the three
   libkernel functions and `hle/ajm.c` (port of shadPS4's ajm.cpp, ajm_context.cpp,
   ajm_instance.cpp, ajm_batch.cpp; GPL-2.0, credit it). ATRAC9 jobs return the right sizes and
   silence. ADPCM and PCM voices should play.
3. **ATRAC9.** Vendor LibAtrac9 (MIT, `third_party/LibAtrac9`, as shadPS4's externals) and port
   `ajm_at9.cpp`.

Decided by the user (2026-10-11): the firmware module comes from a firmware folder, `SOTC_FW_DIR`
(a folder holding `libSceNgs2.sprx`; theirs is `D:\`), read in place and never copied or
committed; LibAtrac9 (MIT) as a submodule is approved for step 3.

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

## Step 2: what was implemented

- **Converter.** `sotcconv.py --fw-dir DIR` links the firmware modules found in `DIR`
  (`FW_MODULES = ["libSceNgs2.sprx"]`) after the default/`--module` ones (a module given both
  ways is linked once). It always prints one line: `firmware: linking libSceNgs2.sprx from DIR`,
  `firmware: libSceNgs2.sprx not in DIR: not linked, audio stays silent`, or `firmware: no folder
  (SOTC_FW_DIR unset): ...`. `scripts/convert.sh` and `scripts\convert.ps1` pass
  `--fw-dir "$SOTC_FW_DIR"` only when the variable is set (the .ps1 appends `\.` so `D:\` survives
  PowerShell's quoting). An absolute `--module` path keeps working. Tests (`FirmwareFolderTests`,
  synthetic module named libSceNgs2): absolute `--module`, `--fw-dir` with and without the file,
  both at once, none.
- **libkernel / libc.** `pthread_equal` (libScePosix table, found from libkernel), `usleep`,
  `sceKernelGetCompiledSdkVersion` (the boot image's procparam SDK version, `g_rt.sdk_version`,
  0x04508101 for SotC). The conversion also showed `libSceLibcInternal sincosf`: the game's
  libc.prx lacks it, so `kernel_misc.c` has it now (a `libSceLibcInternal` table).
- **`runtime/src/hle/ajm.c`** (new; after shadPS4's `libraries/ajm/` and bbport's
  `runtime_ajm.c`): contexts (8), ModuleRegister/Unregister, instances (`id | codec << 14`, flags
  version/channels/format/gapless loop/codec flags), all four batch builders (ControlBufferRa,
  RunBufferRa, RunSplitBufferRa, InlineBuffer), BatchStartBuffer (validates the whole batch first:
  a malformed one runs nothing and fills the `AjmBatchError`; then runs the jobs synchronously),
  BatchWait (frees the id; a caller that never waits gets ids reused, not an error), BatchCancel,
  Memory(Un)Register, InstanceCodecType. Only codec 1 (ATRAC9) instances exist; others return
  `CODEC_NOT_SUPPORTED` with a warning. Jobs: Reset, the input sideband (format, gapless,
  resample, the 8-byte AT9 init), RIFF headers (codec flag 1), and the run loop of shadPS4's
  `AjmInstance::ExecuteJob` (NOT_INITIALIZED, NOT_ENOUGH_ROOM, PARTIAL_INPUT; multi-frame runs
  until one of those). ATRAC9 parses the config like LibAtrac9 (rate, channels, frame bytes,
  frames per superframe), consumes `frame_bytes` per frame and the rest of the superframe at its
  end or at the gapless end, and writes zeros of the exact size (skip/total gapless samples
  honoured). Sidebands: result, stream, format (with bitrate), gapless, multi-frame, codec info.
  Logs: `ajm: context N`, `ajm: ATRAC9 C ch R Hz, ...` for the first 16 inits, `ajm: malformed
  batch`, and totals on Finalize.
- **Test `runtime/tests/test_ajm.c`**: errors (context, revision, registration), the builders'
  chunk sizes, an inline init chunk, format sideband (48 kHz stereo, 192-byte frames, 4 per
  superframe), one frame from a superframe (consumed 192, written 1024, codec info), split
  input/output buffers over 11 frames, not-enough-room, partial input, gapless skip 100 / total
  300 (written 300 samples, rest of the superframe skipped), a malformed batch (nothing runs),
  the statistics instance, 300 unwaited batches; and `pthread_equal`, `usleep`,
  `sceKernelGetCompiledSdkVersion`, `sincosf`. ctest 21/21 (null build).
- **Conversion check** (`--fw-dir D:\`, scratch output): `module libSceNgs2 base 0x03bdc000 size
  0x64000`, `11 eboot -> libSceNgs2`, `31 libSceNgs2 -> libc (libSceLibcInternal)`; new system
  imports: the 10 `libSceAjm` ones, `pthread_equal`, `pthread_self`,
  `sceKernelGetCompiledSdkVersion`, `usleep`, `sincosf` (plus more refs to mutexes, stack guard,
  TSC frequency). Every import now has an HLE function except `Need_sceLibcInternal` (data,
  already imported by the eboot before).

Not done / open: the runtime starts Ngs2's module init with `(0, NULL, NULL)` like libc/Fios2; if
it needs real args, the game run shows it (`module libSceNgs2 started: <r>`). Non-ATRAC9 Ajm codecs
(MP3, AAC) are refused; Ngs2's sampler shouldn't need them.

README text for the main session (not edited here): under the setup/convert steps, "Sound needs
your own decrypted firmware `libSceNgs2.sprx`: put it in a folder and set `SOTC_FW_DIR` to that
folder before `scripts/convert.*` (e.g. `$env:SOTC_FW_DIR = 'D:\'`). Without it the game runs
silent. Firmware files are never included or accepted in this repository."

### Game run to verify step 2 (main session)

Convert with the firmware folder (this rewrites `build/game/boot.img`), then run:

```
$env:SOTC_GAME_DIR = 'D:\CUSA08809\uroot'; $env:SOTC_FW_DIR = 'D:\'; scripts\convert.ps1
scripts\run.bat --label audio2 --seconds 300
```

Expect: `firmware: linking libSceNgs2.sprx from D:\` from the converter; in the log
`starting module libSceNgs2` / `module libSceNgs2 started: 0`, `ajm: context 1`, `ajm: context 1
registers codec 1`, `ajm: ATRAC9 ...` lines once voices start, no `unimplemented` for libSceAjm,
libkernel or `sincosf`, and the `audioout: port N ... first non-silent buffer` / 30 s stats lines
with non-zero peaks: ADPCM/PCM voices should be audible, ATRAC9 ones (music, probably) silent
until step 3. No more `ngs2: system ... (null renderer)` line (the HLE Ngs2 is bypassed). To go
back to the silent null Ngs2: `Remove-Item Env:SOTC_FW_DIR; scripts\convert.ps1`.

## Step 3 plan

LibAtrac9 as `third_party/LibAtrac9` (C, MIT); port `ajm_at9.cpp` (frame decode into S16/F32,
gapless skip, multiple frames per run). No AT9 data can be committed (game data), so the unit test
checks the job plumbing with LibAtrac9's own init on a synthetic config; the sound check is a game run.

### Step 2 game run (main session, 2026-10-11, `audio2`)

`build/game/boot.img` converted with `SOTC_FW_DIR=D:\` (`firmware: linking libSceNgs2.sprx`).
`module libSceNgs2 started: 0`; `audio: wasapi, device 48000 Hz 2 ch, 480 frames per buffer`;
the four ports open at ~67 s (late: a cold NVIDIA shader cache after a PC crash, 66 s preload).
Port 0 (main): `first non-silent buffer` at 98.8 s, then peaks 0.229 / 0.232 / 0.118 / 0.149 per
30 s, 0 underruns, 0 drops; port 1 and bgm stay at 0.000. No `ajm` line at all in 240 s: Ngs2
never set up Ajm up to the intro, so these are ADPCM/PCM voices. The game renders as before.
Not yet heard by the user. A PC crash right after a conversion left `boot.img` with ~58 MB of
zeroed blocks (`too many unresolved data imports` at load): reconvert after a crash.

## Step 3: what was implemented

- **LibAtrac9** as the submodule `third_party/LibAtrac9`: shadPS4's fork
  `https://github.com/shadps4-emu/ext-LibAtrac9.git` at the commit shadPS4 pins (`946e05a`,
  "Prevent bitstream overruns"). It is Alex Barney's MIT decoder plus `Atrac9DecodeS32`/`F32` and a
  planar ("nointerleave") argument. `third_party/CMakeLists.txt` builds `C/src/*.c` as the static
  library `LibAtrac9` (warnings off, `-lm`), before the `SOTC_VULKAN` early return, so both builds
  have it. `sotc_core` links it. Licence row: README "License".
- **`runtime/src/hle/ajm.c`** decodes ATRAC9 for real (a port of shadPS4's `ajm_at9.cpp`):
  - Initialize/Reset open a new LibAtrac9 handle (`Atrac9InitDecoder` on the 4-byte config) and
    take channels, rate, frame samples, frames per superframe and superframe size from
    `Atrac9GetCodecInfo`. A config with channel config index 6 or 7 is refused before LibAtrac9
    sees it (LibAtrac9 indexes its 6-entry table unchecked). A bad config leaves the instance
    uninitialized (`NOT_INITIALIZED` on the next run). InstanceDestroy and Finalize free the handle
    and the per-instance PCM buffer.
  - One frame per step, into S16 (`Atrac9Decode`), S32 or F32 (PCM / 32767) after the instance
    format, interleaved, or planar when the instance's codec flags have `NonInterleavedOutput`
    (0x100; a planar frame cut by gapless skip/total writes each channel plane cut the same way).
  - **Frames vary in size.** LibAtrac9 returns the bytes it used (the blocks are byte aligned);
    the superframe has the fixed size. Consumption is that count, and at the end of a superframe
    the rest (padding) is skipped. Steps 1-2 assumed `frame_bytes` per frame, which was wrong
    (harmless while the output was silence). The decode only sees the rest of the current
    superframe, and a reported count past it is clamped (the bit reader reads zeros past the end).
  - Gapless skip/total as before. When the total ends inside a superframe, the remaining frames
    are decoded and dropped (shadPS4 does the same), so the decoder's frame index stays in step
    with the next superframe, whose first block has to say "first in superframe". They count in
    the multi-frame sideband, as in shadPS4.
  - A LibAtrac9 error: result `CODEC_ERROR | FATAL` (0xC0000000), `internal_result` = the
    LibAtrac9 status, and nothing of that frame consumed or written (shadPS4).
  - Logs: `ajm: context N (ATRAC9 through LibAtrac9)`, `ajm: ATRAC9 C ch R Hz, ...` for the first
    16 inits, `ajm: N ATRAC9 frames decoded (E decode errors)` at the first frame and every 100000,
    `ajm: Atrac9Decode failed: 0x8...` for the first 16 errors, and the totals (with errors) on
    Finalize.
- **Test `runtime/tests/test_ajm.c`**: no real ATRAC9 can be committed, so it builds a synthetic
  stream. It is a hand-made mono block that LibAtrac9 accepts: scale factor mode 1 over base 20,
  and the 0x55 filler becomes non-zero spectra. A frame is two such blocks (channel config 1,
  stereo), 160 bytes. A superframe is 4 frames plus 128 padding bytes (768). LibAtrac9 itself is
  the reference: the Ajm output must equal its PCM byte for byte. Covered:
  - one frame (consumed 160, written 1024, codec info);
  - 11 frames over split input/output buffers, crossing the superframe padding;
  - not enough room, partial input;
  - all-zero data: a codec error, nothing consumed;
  - gapless skip 100 / total 300 (4 frames counted, the right sample ranges);
  - the next superframe after the end decodes with no error (the decoder stayed in step);
  - a reset decodes from the start;
  - F32 over 12 frames;
  - S32 planar;
  - a bad config gives `NOT_INITIALIZED`.

  The earlier checks stay (errors, builders, malformed batch, statistics, 300 unwaited batches,
  the kernel functions). ctest 21/21 (null build). Random garbage isn't tested: LibAtrac9 reads
  its tables out of bounds on some random input (seen under ASan), so real streams are the only
  safe input.

### Uncertain until the game runs

- What Ngs2 asks for: format (S16 or F32), channel count, planar or interleaved, gapless, and
  whether it ever sets `ParseRiffHeader`. The first 16 `ajm: ATRAC9 ...` lines show format and flags.
- That Ngs2 hands over whole superframes. A `PARTIAL_INPUT` loop would show as voices that never
  advance. shadPS4 has the same check (`GetMinimumInputSize` = the rest of the superframe).
- That LibAtrac9's F32 scale (PCM / 32767) is what Ngs2 expects. Too-quiet or clipped music would
  point here.
- Drained frames counting in `num_frames` (shadPS4's choice).
- Any `Atrac9Decode failed` line means a desync or a format we got wrong. Note the frame index it
  logs.

## Game run to verify step 3 (main session)

Same converted `boot.img` as step 2 (firmware Ngs2 from `SOTC_FW_DIR`; there is no need to reconvert
unless it is missing). Run long enough to reach the intro and the first gameplay:

```
scripts\run.bat --label audio3 --seconds 600      # Linux: scripts/run-vk.sh --label audio3 --seconds 600
```

Expect:
- `ajm: context 1 (ATRAC9 through LibAtrac9)`, `ajm: context 1 registers codec 1`;
- once ATRAC9 voices start, `ajm: ATRAC9 2 ch 48000 Hz, 256 samples x N frames per M-byte
  superframe, format F, flags X` lines;
- `ajm: 1 ATRAC9 frames decoded (0 decode errors)`, then a line every 100000 frames;
- **no** `Atrac9Decode failed` lines;
- `audioout` 30 s stat lines: port 0 (main) as in step 2, and now **non-zero peaks on port 1 /
  `bgm`** after the intro (music), where step 2 showed 0.000. 0 underruns as before.

In step 2 no `ajm` line came in 240 s: Ngs2 had not set up Ajm by the intro. So the 600 s run (into
gameplay) matters. Ask the user to listen for music and for distortion or wrong speed (a wrong
format or channel count). If no `ajm` line shows up in 600 s, ATRAC9 isn't used up to that point and
step 3 can't be judged yet.

## For the main session

- **CLAUDE.md**, Map, `hle/` list (M2 line, or a new audio line): "`ajm.c` (ATRAC9 through
  LibAtrac9, `third_party/LibAtrac9`, MIT)". Hard rules / licences: nothing new beyond the README
  row already added. **Submodules**: the `git submodule update --init` line covers the new
  `third_party/LibAtrac9` (no nested submodules).
- **README "Audio"**: after a good game run, replace "ATRAC9-encoded sounds (likely some music and
  streams) are still silent until the decoder is added" with "ATRAC9-encoded sounds (music,
  streams) are decoded with LibAtrac9".
- **docs/NEXT.md** item 7: step 3 is done in code. What is left is the game run above.
