# TASK-074 L2: the smaller Windows host pieces on Linux

The Linux window host (`native/linux/sdl_window.cpp`) gained three pieces of `native/window.cpp`, each with the same rule
and the same switches as on Windows.

## Cursor auto-hide (TASK-060)

The mouse cursor hides after 3 s without movement while it is over the game picture, and shows at the first movement
(`native/cursor_autohide.hpp`, the same pure rule). SDL gives the position relative to the window and the window under the
mouse (`SDL_GetMouseFocus`), which Wayland allows where a screen position is not; `SDL_HideCursor` acts only over this
window, so outside it the cursor is never touched. `GT4_CURSOR_AUTOHIDE=0` turns it off; any value but 0 or 1 stops the
start. Printed at start: `[window] cursor auto-hide on (...)`.

## The latency shift in frame pacing (IN-521)

CoreGS 2.0's display queue control (`native/hw_gs/hw_present_latency.hpp`, on by default, `GT4_HW_LATENCY_CONTROL=0` off)
asks the pacer to run its grid earlier when frames keep missing the flip (`pacing::grid_shift_us`). The Linux
`release_frame` now takes that shift once and moves the grid, as Windows does; before, the requests were never applied.
CoreGS prints `[hw] latency control: frames miss the flip, grid ...` when it acts. It needs the shown times of the
presentation; whether a given Linux driver reports them is not checked here.

## Input recording and replay (IN-520)

- `GT4_INPUT_RECORD=<file>` (with a window): the pad state the game receives is written at each change; F9 ends the
  recording and closes the game; closing the window or the desktop's quit (also Ctrl-C, which SDL turns into a quit) leaves
  a complete file.
- `GT4_INPUT_REPLAY=<file>` (with or without a window): the file drives the pad at the same VBlanks, instead of the
  keyboard and pads. Both together, a missing or malformed file, or recording without a window stop the start.
- Same text format as Windows (`native/input_recording.hpp`), so a recording from either system replays on the other.

## Checks (2026-11-23)

- CTest 271/271 (`native_cursor_autohide`, `native_input_recording` and the rest).
- Replay headless, v2.00, 3,000 frames with Start pressed 7 times: 96 of 120 frames differ from the run without input,
  and two replays give identical frames.
- A windowed recording ended by Ctrl-C: `# GT4Recomp input recording 1`, `v 1 ...`, `end 671`; it replays.
- Not yet: the tester seeing the cursor hide, recording a real drive with F9, the latency control acting on Linux.
