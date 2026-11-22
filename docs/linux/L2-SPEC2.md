# Spec II on Linux: its own translated code, capsules and executable (TASK-074 with TASK-100)

The second target ([decision 0072](../decisions/0072-spec2-second-target.md), [SPEC2-PLAN](../SPEC2-PLAN.md)) built and
run on Linux. Every Spec II input is derived on Linux from the player's Spec II 1.10 disc and is **byte-identical to the
Windows 1.3.0-rc14 pins** (`docs/inputs/slice408-spec2.json` and the pins it names). The executable is a separate build
(`GT4_TARGET=spec2`); the v2.00 build, its inputs and its pins are untouched.

## Inputs (2026-11-22, this machine: clang 23.1.4, the Linux translator of [L1](L1-GAME-LIBRARY.md))

| Input | How | Result against the pin | Label |
|---|---|---|---|
| Disc | `GT4.SCUS97436_Spec_II.iso` | size and SHA-256 equal `spec2-iso.json` (`6a3484b9...`) | Confirmed |
| Derived ELF | `spec2_elf_patch.py --iso` | `36f60b1a...` = `spec2-elf.json` (35 injector stores, data segment split) | Confirmed |
| Entry seeds | delta of `spec2_entry_seeds.py` against the S7b translation (below) | 1,299 seeds, `041c1864...` = `spec2-ee-generated.json` | Confirmed |
| Translation | Linux `eerecomp` with those seeds | 346 units, inventory `2d8c92a4...`, 93,030,717 bytes = `spec2-ee-generated.json`; stats line identical | Confirmed |
| Aspect units (TASK-166) | `spec2_aspect_patch.py` table and checks, Linux translator | patched ELF `ef808768...`; only `unit_0031`/`unit_003A` change; inventory `e436aa0a...` | Confirmed |
| Release units | `camera360_units.py spec2` | output inventory `7105d292...` = slice408-spec2 | Confirmed |
| VU0 image and table | upload of `spec2-vu.json` padded to 4 KiB; 14 entries; `gt4_vu_recomp` | image `e5ea672f...`, table `3d045021...`, manifest `2eb28373...` = slice408-spec2 | Confirmed |
| VU1 table | the v2.00 one (`spec2-vu.json`: the decrypted image is identical) | same pin in both slice408 files | Confirmed |
| Runtime capsule | `prepare_perf_runtime.py --mtvu --gs-stream --frame-cost --vu0-aot --spec2-iop` | 8 of 8 sources = `slice276-spec2-iop-runtime.json` (with `sif.cpp`, the DNAS310 reboot rule) | Confirmed |
| License guard capsule | `prepare_license_guard.py --display-aspect --frame-counters` | 3 of 3 = slice408-spec2 (its `dispatch.cpp` is this plain derivation; v2.00's pin differs) | Confirmed |
| Port, shaders, USB capsule | the v2.00 ones | same pins in both targets | Confirmed |

**The seed chain.** The pinned seed file is a delta: Windows ran `spec2_entry_seeds.py --generated <S7b units>`, and
the S7b translation itself had been seeded by hand with plugin entries. Seeding only the plugin's two literal code
pointers (`0x0068C6E8`, `0x0068C7C4`, the tables the dataword scan cannot see) reproduces the recorded S7b statistics
exactly (1,285,650 instructions, 17,080 functions, 148,756 entries), and the delta against it is the pinned file byte for
byte. Seeding every candidate at once (`--all`) gives the same entries but a different function split (34,263 functions)
and a different inventory, so the chain matters.

`scripts/linux/spec2-inputs.sh <Spec II disc> [prefix]` runs every step above into fresh `private/<prefix>-*` folders
and stops at the first value that differs from its pin (about 25 s).

## Build

`cmake/LinuxHost.cmake` is target-aware: with `GT4_TARGET=spec2` the VU tables, the guard and the release units are
checked against `slice408-spec2.json`, the runtime capsule must be the slice276 profile (its eighth source `sif.cpp`
replaces the donor's `iop/sif.cpp`), and `GT4_LINUX_PRODUCER` (the v2.00 library) is refused: the game code comes from
`GT4_LINUX_GAME_UNITS` alone. The rest of the configure line is the v2.00 one:

```sh
P=$PWD/private
cmake -S . -B private/linux074-spec2-build -G Ninja <the v2.00 settings: compiler, donor, port, shaders, VU1, USB, HW GS> \
  -DGT4_TARGET=spec2 -DGT4_LINUX_GAME_UNITS=$P/linux074-spec2-release-units \
  -DGT4_LINUX_RUNTIME_CAPSULE=$P/linux074-spec2-iop-runtime -DGT4_LINUX_LICENSE_GUARD=$P/linux074-spec2-guard-runtime \
  -DGT4_LINUX_VU0_GENERATED_DIR=$P/linux074-spec2-vu0-gen -DGT4_LINUX_VU0_IMAGE=$P/linux074-spec2-vu0-image/vu0_image.bin
cmake --build private/linux074-spec2-build
```

PGO and ThinLTO as for v2.00, with `GT4_LINUX_GAME=spec2` (`private/linux074-spec2-pgo-gen`, `-pgo`, `-pgo-build`):

```sh
GT4_LINUX_GAME=spec2 scripts/linux/pgo-build.sh instrument
GT4_LINUX_GAME=spec2 scripts/linux/pgo-build.sh train <Spec II disc> <card>
GT4_LINUX_GAME=spec2 scripts/linux/pgo-build.sh optimize
```

## Starting it

- Direct start: `private/linux074-spec2-pgo-build/gt4_linux_window` (or `linux074-spec2-build`) without arguments reads
  `~/.config/gt4recomp/game-spec2.ini`: the same keys as `game.ini` ([L2-DIRECT-START](L2-DIRECT-START.md)), its own
  `disc`, its ELF (found in `private/linux074-spec2-elf`) and its own default card `~/.local/share/gt4recomp/card-spec2.ps2`.
  The two games never share a config file or a card.
- Script: `GT4_LINUX_GAME=spec2 scripts/linux/play.sh <Spec II disc> <card>`.

## Checks

| Check | Result | Label |
|---|---|---|
| CTest, Spec II build (without the two long VU differential runs) | 269 of 269 passed (1 skipped: needs PCSX2 dumps), including `native_spec2_iop`, `native_spec2_hooks`, `native_game_target`, `vu0_aot_registry` on the Spec II image | Confirmed |
| `native_linux_standalone` in both builds | v2.00 and Spec II tables, each build picks its own | Confirmed |
| Headless boot, native backend, BUILD.md press schedule, new card, 9,600 frames | no halt, no missing code; the DNAS310 IOP reboot selects the asynchronous FILEIO server (`LOADFILE 3100`); frames: language, card dialogs, intro movie, Spec II title, Arcade Mode Selection | Confirmed |
| Headless run to a race screen, 21,000 frames | City Courses, Costa di Amalfi, car selection, Driving Options, 3D pre-race screen; camera 360, FoV and trigger range report Spec II addresses; first `RaceDisplay::render` at VBlank 19,083 | Confirmed |
| A race played on Linux | not yet (tester) | Unknown |

Not on Linux yet for either target: see [L2-PERFORMANCE](L2-PERFORMANCE.md) "Not yet on Linux".
