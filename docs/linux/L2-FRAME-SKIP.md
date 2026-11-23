# TASK-074 L2: frame skip on Linux (TASK-082, TASK-146)

The launcher's Frame rate setting: "60 FPS" (the default) passes `GT4_FRAME_SKIP=1`, which leaves frames out in a running
race only when the PC falls behind; "30 FPS" passes `GT4_FRAME_SKIP_FORCE=2`, one frame drawn in two. The simulation keeps
60 Hz either way. The rule and the policy are in `native/frame_skip.hpp`; the state of the study in
`docs/TASK-082-UNLOCKED-FPS-STATE.md`.

## What was missing

Two pieces, not one:

1. **The hook.** The frame skip acts from the runtime's dispatch (`scripts/frame_skip_overlay.inc`): the main loop's render
   and idle functors return at once on a frame left out. The hook is in the license guard capsule's `dispatch.cpp`. The
   Linux build used a guard equal to the slice408 pins (`license_guard_source_sha256`, the slice262 capsule for v2.00 and
   slice277 for Spec II), which has no frame skip hook, so `GT4_FRAME_SKIP` was read and configured but nothing ever
   called it. **Confirmed:** `scripts/coregs2_bench/game/configure.cmd`, which builds the Windows release (both games),
   uses `private/slice376-frame-render-license-guard-runtime` instead; `scripts/pin_release_130.py` writes the slice262
   and slice277 names into the slice40x pins regardless. The pins are left as they are.
2. **The host side.** `native/window.cpp` `note_frame_skip_vblank` and the pacing inputs of `release_frame`. Ported to
   `native/linux/sdl_window.cpp`: at every VBlank the controller gets the EE's slack against the grid, the GS worker's
   lateness and cost (set by CoreGS 2.0, `hw_gs_stream_sink.cpp`) and the EE's own frame time without its waits for the
   GS worker; the pacer owes up to 12 periods while the gate is open, and the minimum interval (IN-519) is off while
   frames are being left out. Without a window the controller still gets its VBlanks (no pacer: only a fixed pattern
   leaves frames out). The presenter side (`present_paced`, the display hold) is shared code and was already there.

## The guard capsule

`prepare_license_guard.py --display-aspect --frame-counters --frame-skip` at the current overlays gives the slice376
capsule byte for byte (all three sources equal `docs/inputs/slice376-frame-render-license-guard-runtime.json`):

```
cd scripts && ../private/tooling-venv/bin/python prepare_license_guard.py "$(realpath ..)/private/linux074-frameskip-guard-runtime" \
    --display-aspect --frame-counters --frame-skip
```

(The output path must be the resolved path: the script refuses a path through a symlink.) The overlay hashes match
the slice376 receipt: guard `7765a1c7...`, aspect `65992ad9...`, frame skip `a058dcce...`. The same capsule serves both
games, as on Windows.

`cmake/LinuxHost.cmake` now accepts `GT4_LINUX_LICENSE_GUARD` equal to either the slice408 pins (no hook) or the slice376
capsule (with the hook), and says which at configure time.

## Starting it

- `scripts/linux/play.sh`: `GT4_LINUX_FRAME_RATE=60` (default), `30` or `off`, CoreGS 2.0 only, as the launcher.
- `game.ini`: `hw_frame_skip_adaptive=1` (60 FPS, now written uncommented in a new file) or `hw_frame_skip=2` (30 FPS).
  An existing `game.ini` is not rewritten.
- Both need `GT4_FORCE_480P=1` (play.sh and the direct start set it) and print `[frame-skip] on: ...` at start, then a
  report line every 600 VBlanks. `GT4_FRAME_SKIP_LOG=<csv>` writes one row per frame.

## Builds and checks (2026-11-23)

- Plain `private/linux074-frameskip-build`: CTest 271/271. Spec II's plain build `linux074-spec2-build` uses the same capsule.
- PGO + ThinLTO with the existing profiles: `private/linux074-pgo-build` (`ac2e93e2...`) and `private/linux074-spec2-pgo-build`
  (`33c01d78...`). The profile was trained without the hook; only `dispatch.cpp` changed.
- Tester: races at 30 and 60 FPS on the plain build work as expected.
