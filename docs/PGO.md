# LTO and PGO builds

`-DSOTC_LTO=ON` adds thin LTO over everything (lld; on Windows also `-DCMAKE_LINKER=lld-link`).
`-DSOTC_PGO=<profile>` adds profile-guided optimization on top. On Windows, LTO+PGO measured
-8.7% ms/flip and +8.4% flips/s. The iteration build (`build-vk`) stays without both.

## Regenerating the PGO profile

The profile comes from a run of your own game, so it is game-derived: never commit it. It goes
stale as the code changes, so refresh it before a measured run.

1. `cmake -S . -B build-vk-pgogen -G Ninja <compilers> [-DCMAKE_LINKER=lld-link] -DSOTC_VULKAN=ON
   -DSOTC_LTO=ON -DSOTC_PGO=generate`, build. Delete the old `build-vk-pgogen/pgo/*.profraw`.
   The instrumented runtime rewrites `<build>/pgo/sotc-<pid>.profraw` every 30 s.
2. One run to the altar: Windows `scripts\run.bat --exe build-vk\sotc-runtime-pgogen.exe --seconds 260`
   (the exe copied there, see below), Linux `scripts/run-vk.sh --seconds 200 --exe
   build-vk-pgogen/sotc-runtime`.
3. `llvm-profdata merge -o build-vk-pgogen/pgo/sotc.profdata build-vk-pgogen/pgo/sotc-*.profraw`,
   then build `build-vk-pgo` with `-DSOTC_LTO=ON -DSOTC_PGO=<absolute path to sotc.profdata>`.

## Pitfalls

- Windows lets exes in some build directories write anywhere in the project but blocks new ones
  (`build-vk-release`, `-lto`, `-pgo`) from creating files outside their own tree (`fopen` ->
  `ERROR_ACCESS_DENIED`: screenshots, `build/user`). Workaround: copy the variant exe into
  `build-vk\` under another name (`sotc-runtime-pgo.exe`) and run it from there.
  `test_sotcgpu_window` fails in those directories for the same reason.
- NVIDIA's driver shader cache is per executable: the first run of a newly named exe compiles every
  pipeline cold. Warm each new exe name with a short run before measuring.
- A `/O2 /Ob2` (CMake Release) LTO `test_fault` crashed 3 times (0xc0000409, the
  pthread_exit-from-fiber case), never in RelWithDebInfo LTO (30/30). Release is not a supported
  build type; if it is ever wanted, look at `thread_exit`'s `__builtin_longjmp` under full inlining.
