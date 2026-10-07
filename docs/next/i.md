# Workstream I: input at low frame rates

## Problem
`scePadReadState` and `scePadRead(num == 1)` returned only the current state. At the title screen
(0–1 fps), a press that began and ended between two reads was lost. A `SOTC_PAD_SCRIPT` press lasts
150 ms, so with one read per second it was nearly always lost. Real hardware polls at about 250 Hz, so the
game never sees this problem there.

## Fix (runtime/src/hle/pad.c only; the window.c interface is unchanged)
`pad_input_update()` (the window thread, about every 8 ms) still stores the current snapshot and queues a sample
for every change. Two press latches are added. The existing spinlock protects both, so guest reader threads and the window thread stay
consistent.

- **Single reads** (`scePadReadState`, `scePadRead` with `num == 1`) return the current buttons ORed
  with `g_latch_single`. `g_latch_single` collects every button whose *press edge* (0 → 1) arrived after
  the previous single read, and the read clears it.
  - Press and release between two reads: the next read shows it pressed, and the read after that shows it released.
  - Held button: it stays pressed (through the current state). A button that was held across a read and released before the
    next read reads as released; that isn't a new press, so it doesn't count.
  - Release, then press again, between two reads while the button was held before: it reads as continuously held.
    That edge is lost; it's harmless for menus.
  - Sticks and triggers are always the current values. The timestamp is still that of the last change.
- **Multi reads** (`scePadRead` with `num > 1`) already drain a 64-deep queue of samples, the way
  shadPS4 does (`input/controller.cpp` keeps a ring of states). That queue holds both the press sample and the
  release sample. The one remaining loss was an overflow: more than 64 changes between reads, for example stick
  noise at 1 fps. A dropped sample's buttons are now ORed into `g_latch_dropped`, and that latch is ORed into the
  first sample of the next multi read.
- Each path has its own latch, so a game that uses both styles loses nothing on either.
- `scePadOpen` (ConnectController) clears both latches along with the queue.

## Tests
`runtime/tests/test_pad.c` drives `pad_input_update()`, the same entry point that window.c's gamepad, keyboard
and `SOTC_PAD_SCRIPT` paths use:
- press and release between two reads: pressed once, then released (`scePadReadState` and `scePadRead(1)`);
- a held button stays pressed, its release shows at once, and a press while another button is held;
- sticks report the current value;
- a pad-script simulation: 8 ms polling on a virtual clock, 150 ms presses at 1.2 s and 2.5 s, and the game
  reading once per second. Both single reads and multi reads see each press exactly once;
- queue overflow: a press followed by 100 stick changes is still reported in the first multi-read sample;
- reopening clears the latches.

The existing `test_window` pad mode, which covers the old semantics, still passes.

## Not done / for integration
- Not verified in the game. The main session should run the title screen with
  `SOTC_PAD_SCRIPT="<t>:cross"` and check that the press registers at 0–1 fps.
- Which read style SotC uses is unknown. Both are imported (`scePadRead` and `scePadReadState`, one reference each).
