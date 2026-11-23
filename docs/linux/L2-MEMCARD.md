# TASK-074 L2: crash-safe memory card saves on Linux (TASK-058)

The release's memory card capsule (`scripts/prepare_memcard_flush.py`, slice200; pinned by `memcard_capsule_sha256` in
`slice408-pcsx2.json` and `slice408-spec2.json`) now builds into the Linux game, for both games.

## What it changes

- **Before:** the donor's `iop/sio2.cpp` rewrote the whole card file in place (truncate, then write) once 50 VBlanks passed
  without a card write, and never wrote at exit. A crash or a power loss in the middle of that write left a broken card,
  and a save made less than about a second before closing the game was lost.
- **Now:** the derived `sio2.cpp` writes through `native/memcard_store.cpp`: the image goes to `<card>.tmp`, is flushed to
  the disk (`fsync`), then renamed over the card, so a crash leaves either the previous card or the new one, whole. A
  failed write keeps the card dirty and retries after another 50 VBlanks (`[mc] cannot write ...; will retry`).
- **At exit:** a pending save is written when the game returns: `sdl_window.cpp` after `emulate()` (the window path, as
  `window.cpp` on Windows), `linux_main.cpp` without a window (on Windows the slice200 port's `main.cpp` does this). A
  crash or `kill -9` writes nothing more, but the card on disk is whole.
- **Linux only:** after the rename, `memcard_store.cpp` syncs the card's folder (`fsync` on the directory), which POSIX
  needs for the rename itself to survive a power loss. Windows code is unchanged.

## Build

`cmake/LinuxHost.cmake`: `-DGT4_LINUX_MEMCARD_RUNTIME=<capsule>`; `sio2.cpp` must equal its `runtime.json` and the
release pin. It replaces the donor `iop/sio2.cpp`, links `gt4_memcard` and defines `GT4_MEMCARD_FLUSH_OVERLAY` for the
window executable. The capsule comes from `scripts/linux/v200-inputs.sh` (`<prefix>-memcard-runtime`; the same capsule
serves Spec II).

## Inputs again (2026-11-23)

The inputs had been deleted from `private/` (keeping only builds). They were derived again, all equal to their pins:

- `tools/ee_producer/build_linux.sh . private/linux074-ee-producer` (about 1 minute); the ELF is byte-identical to before.
- `scripts/linux/v200-inputs.sh` (new): VU1/VU0 images and tables, the frame-cost runtime, the Linux port, the guard with
  the frame skip hook, USB, memory card, aspect translation and release units, each checked against its pin (3 s).
- `GT4_LINUX_VU_RECOMP=private/linux074-frameskip-build/gt4_vu_recomp scripts/linux/spec2-inputs.sh <Spec II disc>`.

## Checks

- `native_memcard_store` (the atomic writer: whole write, simulated partial write, simulated crash before the rename) and
  the whole CTest: 271/271 in `private/linux074-frameskip-build` (v2.00) and `private/linux074-spec2-build` (Spec II).
- Both games boot headless with the capsule; with no save made the card is unchanged and no `.tmp` is left.
- Not yet: a save made in the game by a person; the PGO builds (their training profiles were deleted with the inputs, so
  they need new training sessions: `scripts/linux/pgo-build.sh instrument | train | optimize`).
