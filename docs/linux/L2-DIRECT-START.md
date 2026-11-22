# Linux direct start from game.ini (TASK-074, plan step L2)

The game starts on Linux without environment variables or scripts: `gt4_linux_window` with no ELF argument (no arguments,
or only `--` options) reads `game.ini` and builds the command line and environment that `scripts/linux/play.sh` passes.
A start with an ELF argument (`play.sh`, the benches, the pins) is unchanged. The Linux counterpart of the Windows direct
start (TASK-097, `native/standalone.cpp`), without the Windows install layout (receipt, launcher lock, sessions), which
belongs with a Linux launcher (step L4).

Spec II (`GT4_TARGET=spec2`, [L2-SPEC2](L2-SPEC2.md)) reads `game-spec2.ini` instead, finds its ELF
(`SCUS_974.36`) in `private/linux074-spec2-elf` and defaults to its own card `card-spec2.ps2`; keys and precedence are
the same (`native/linux/linux_standalone.hpp` `kV200Game` / `kSpec2Game`).

## Folders

| What | Where |
|---|---|
| `game.ini`, `controls.toml` | `$XDG_CONFIG_HOME/gt4recomp`, else `~/.config/gt4recomp` |
| default memory card `card.ps2` | `$XDG_DATA_HOME/gt4recomp`, else `~/.local/share/gt4recomp` |
| CoreGS 2.0 shader cache | `$XDG_CACHE_HOME/gt4recomp/shaders`, else `~/.cache/gt4recomp/shaders` (play.sh's folder) |

## game.ini

The first start without the file writes a commented template (every key with play.sh's choices, the ELF found beside
the build) and stops with a message (stderr, and a message box when a window would open) asking for `disc=`.

- The keys are the Windows launcher's, read by the same parser (`gt4::standalone::parse_config`): `fullscreen`,
  `present_mode`, `output_size`, `pad`, `hw_scale`, `hw_postfx`, `hw_hud`, `hw_frame_skip_adaptive`, `hw_present_mode`,
  `camera_360`, `fov_adjust`, `trigger_full_range`, `wheel_file`, ... Unknown keys and refused values are logged and
  ignored, never fatal.
- Linux-only path keys: `elf` (the reconstructed `SCUS_973.28.elf`; when absent, found beside the executable or in
  `../linux074-ee-producer`), `disc` (required) and `memcard` (default above). `~/` expands to HOME; a relative path is
  taken from the config folder.
- Order of every value: command-line option or environment variable > `game.ini` > built-in default. The built-in
  defaults are play.sh's fixed settings: CoreGS 2.0 (`GT4_RENDERER=hw`, `GT4_HW_CORE=2`) at 3x, 480p, 16:9 hold,
  automatic display aspect, fifo. Keys whose Windows meaning is "absent = off" keep it (no `hw_postfx` line: no FXAA).
- Options: `--disc`, `--memcard`, `--ntsc`/`--pal` and `--window`/`--fullscreen` replace their value; `--no-window` runs
  headless (tests); anything else (`--frames N`, ...) is passed on.

## Code and checks

- `native/linux/linux_standalone.hpp/.cpp` (new, pure logic over injected access); `native/linux/linux_main.cpp` uses it
  for a start without an ELF argument; `standalone.hpp` declares `coregs20_environment` (already defined in
  `standalone.cpp`; no Windows code change).
- CTest `native_linux_standalone` (`native/linux/linux_standalone_fixture.cpp`): folders, the template parses without
  warnings and starts with play.sh's settings, path expansion, precedence (environment, game.ini, defaults; options),
  refusals for a missing disc or ELF. Passed, with `native_standalone`.
- In the game (2026-11-22, PGO build, scratch XDG folders): the first start wrote the template and stopped (exit 3); with
  `disc=` set, a headless 600-frame start ran from game.ini alone (`GT4_HW_SCALE=3`, `GT4_HW_POSTFX=fxaa` from the file,
  a new card in the data folder). An explicit start (ELF argument) printed no `[start]` line and ran as before.
