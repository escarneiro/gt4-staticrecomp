# Linux game window (TASK-074, plan step L2, first part)

Status: **GT4 runs in a Linux window** (2026-11-20): picture through the native Vulkan GS backend, sound, keyboard
and controller through SDL3, on Wayland and on X11 (XWayland). This is the first playable Linux build, a preview: it
has the base game only, not yet the Windows build's game patches, frame clock or other extras listed below.
Gameplay has not been tested by a person yet.

## Results (KDE Plasma Wayland session, RX 6600 with RADV, PipeWire, SDL 3.4.20)

| Check | Result | Label |
|---|---|---|
| Wayland window (`SDL_VIDEO_DRIVER=wayland`) | Vulkan surface attached, 1280x720 swapchain, 595 presentations in 600 frames, about 10.7 s (60 Hz FIFO) | Confirmed |
| X11 window through XWayland (`SDL_VIDEO_DRIVER=x11`) | the same: surface attached, 595 presentations, about 10.7 s | Confirmed |
| Audio on, paced by the sound device | 600 frames in 10.3 s, clean exit | Confirmed (sound not heard by the agent) |
| Exit | exit code 0 on both window systems (after the shutdown-order fix below) | Confirmed |
| CTest, Linux tree | 40/40: adds `native_keyboard_scancodes`; `native_vulkan_extensions` now also checks the Linux selection | Confirmed |
| Python | `test_linux_surfaces.py` 3/3 (synthetic hooks, refused anchors, real derivation changes only the attach) | Confirmed |
| Gameplay by a person (picture, sound, keyboard, controller, rumble, fullscreen) | not done yet | Unknown |

## How to run it

After the [L1 steps](L1-VULKAN.md#how-to-reproduce), derive the port with Linux surfaces and point the build at it:

```sh
private/tooling-venv/bin/python scripts/prepare_external_vulkan.py private/<port> --linux-surfaces
cmake -S . -B <build dir> ... -DGT4_LINUX_PORT_DIR=$PWD/private/<port>    # other options as in L1
cmake --build <build dir> --target gt4_linux_window
<build dir>/gt4_linux_window private/<producer>/SCUS_973.28.elf --window --ntsc --disc <iso> --memcard <card.ps2>
```

`--fullscreen` starts in fullscreen. Without `--frames` the game runs until the window is closed. The memory card file
is created on first use. Environment:

- `GT4_CONTROLS_FILE=<file>`: a controls table such as `docs/controls/keyboard-only.toml` (default: the built-in map,
  the same keys as the README table).
- `GT4_PAD=<guid>[/<serial>]`: the controller to read (default: the first one connected).
- `GT4_AUDIO=none`: no sound; the game is then paced by the clock at 60000/1001 Hz.
- `SDL_VIDEO_DRIVER=x11` or `wayland`: choose the window system (default: SDL's choice, Wayland on a Wayland session).
- `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`: software Vulkan (slow; for checks only).

Keys: the controls table (default W/Enter = Cross, S = Square, Space = Circle, Backspace/Esc = Triangle, arrows/A/D =
D-pad, Q/Shift = L1/R1, F/R = L2/R2, C/Tab = Select, P = Start), F11 or Alt+Enter = fullscreen.

## Design

- **Host** `native/linux/sdl_window.cpp` implements `native/window.hpp` for the port's `main.cpp`, as
  `native/window.cpp` does on Windows, with the same shape: one thread, and at every guest VBlank the window events,
  the pad, `ps2::gs::present()` and the pacing. Its SDL handling follows the donor's `host/window.cpp`, ported to SDL3
  (attribution in `native/THIRD_PARTY.md`).
- **Surface**: SDL3 reports the X11 display and window, or the Wayland display and surface, and the derived port's
  `attach_surface` passes them to `Context::attach_x11` / `attach_wayland` in `native/native_vulkan.cpp`, which create
  the Vulkan surface (`VK_KHR_xlib_surface`, `VK_KHR_wayland_surface`). The swapchain and presentation code is the
  Windows code, unchanged. Xlib and Wayland headers are not included: the four types the Vulkan platform headers name
  are declared instead, so Xlib's macros stay out.
- **Port**: `scripts/prepare_external_vulkan.py --linux-surfaces` splices the X11/Wayland attach into the derived
  `gpu.cpp` with checked, reversible anchors (the existing `--gs-stream-hooks` pattern). Without the option the
  derivation is byte-identical to before; `native/gpu_*.inc` is not edited.
- **Instance extensions**: `select_linux_instance_extensions` in `native/vulkan_extensions.cpp` (pure, fixture-tested):
  `VK_KHR_surface` with every available X11/Wayland surface extension and the same swapchain-maintenance family logic
  as Windows (now one shared helper). With no window-system extension the instance is headless.
- **Input**: the controls table (`controls.cpp`, unchanged) gives keyboard bindings as Win32 virtual-key codes;
  `native/keyboard_scancodes.cpp` translates them to USB HID scancodes, which are SDL's scancode values (the fixture
  checks every nameable key against SDL3's own constants). Gamepads use `read_sdl_gamepad`, moved unchanged from
  `host_io.cpp` to `native/sdl_gamepad.cpp` so both hosts share one table, then `gt4::pad::map_reading`.
- **Audio and pacing**: an SDL3 audio stream at 48 kHz stereo; the VBlank waits while more than 80 ms of sound is
  queued (the donor's rule) and drops sound above 500 ms. Presentation is FIFO.
- **Exit**: the window and SDL stay open until the process ends, because the port's global Vulkan context releases
  its swapchain and surface in its destructor at exit. Destroying the window first crashed (segfault on Wayland, abort
  on X11); now both exit with code 0.

## Not yet on Linux

- The Windows host's frame clock and audio rate control (`frame_clock.cpp`, `pacing.cpp` modes). With FIFO on a
  60 Hz display the game runs about 0.1% fast until the audio queue reaches 80 ms, then waits one frame; on a
  high-refresh display FIFO presents faster and the audio queue holds the average speed, so frame times are uneven.
  **Hypothesis**, not measured yet.
- `game_main.cpp` features: the game patches (widescreen and display aspect hooks, 360-degree camera, FoV, trigger
  range, flare limits), `game.ini` / standalone start, the memory card lock and backups.
- Racing wheels, RetroAchievements, the F7/F8 performance probe, input recording and replay, cursor auto-hide.
- VU1/VU0 AOT capsules, CoreGS 2.0 and the GS process (gt4-gs).
- The Windows build of these changes has not been compiled or tested.
