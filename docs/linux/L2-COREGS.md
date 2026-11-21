# CoreGS 2.0 on Linux (TASK-074, plan step L2)

Clean room: CoreGS 2.0 (`native/hw_gs`, TASK-027) may not be edited by an agent that has read PCSX2 sources. The
agent that wrote this port read no PCSX2 source, the `gt4recomp-gs` fork or any `private/tooling/pcsx2-*` or
`private/slice189-pcsx2-ref` material, and the changes below only replace Windows APIs, plus one shader workaround.

## Part 1: the libraries, shaders and tests (2026-11-21)

Status: **CoreGS 2.0 builds on Linux and passes its test suite on the RX 6600 (RADV)**. It is not yet connected to
the game; that is part 2.

### Results

| Check | Result | Label |
|---|---|---|
| Build (`-DGT4_LINUX_HW_GS=ON`, clang 23.1.4) | every CoreGS library, the 36 shaders (system glslangValidator 16.6.0), the writeback tests and the replay tool build | Confirmed |
| CTest on RADV (RX 6600, Mesa 26.2.5) | **258 entries: 257 pass, 1 skipped** (`hw_replace_q07_pcsx2_dumps` needs private texture dumps) | Confirmed |
| CTest on lavapipe (software, for comparison) | 253 pass; `hw_cache_equals_writeback` and `hw_scale_display_shadow` end in a segfault, `hw_core2_rop` and `hw_core2_prepare_shaders` fail | Confirmed; cause Unknown |
| Shaders | all built by `glslangValidator` 16.6.0 (`c4652e3f...`); the Windows build pins its own `glslangValidator.exe` by hash (IN-201), no Linux binary is pinned | Confirmed |

### A Mesa RADV bug, worked around in one shader

`hw_tone_hle_exact` (the tone-sequence strip dispatch, IN-383, which runs every race frame) failed on RADV only:
3,584 of 4,096 pixels wrong, exactly every strip but the first. The SPIR-V is valid (`spirv-val`) with the right
push-constant layout (`ArrayStride 4`), lavapipe gives the exact result, and RADV gives the same wrong result on every
run, with its ACO and its LLVM compiler alike. Reading `pc.quadrants[strip]` through constant indices (a `switch`)
makes RADV exact. So RADV (Mesa 26.2.5) mis-reads a push-constant array indexed by a value that varies per
invocation. `native/hw_gs/shaders/hw_tone_strips.comp` now reads it through the `switch`: the same value, on every
driver. The only other push-constant arrays (`core2_compose.comp` `c1`/`c2`) are read with constant indices. The
shaders are not pinned; Windows behaviour is unchanged by construction but has not been re-run.

### Source changes (all behind `#ifdef _WIN32` / `#ifndef _WIN32` unless noted)

- `hw_gs_vulkan_loader.*`: `libvulkan.so.1` with `dlopen`; the Xlib and Wayland surface entry points (each optional)
  instead of the Win32 one; the four X11/Wayland types are declared, not taken from Xlib.h. `<cstdint>` included
  (Windows got it through `<windows.h>`).
- `hw_gs_backend.*`: the present-wait poll sleeps with `std::this_thread::sleep_for` instead of a waitable timer;
  Linux instances enable `VK_KHR_surface` with the X11/Wayland extensions; new `Backend::attach_x11` and
  `attach_wayland` (the Win32 `attach_window` refuses on Linux); the window size for a surface that reports none
  (Wayland) comes from attach and `resize_window`; the performance HUD stays off on Linux with a message
  (`GT4_HW_HUD=1`).
- `core2/core2_gpu.cpp`, `core2/core2_renderer.cpp`: `GT4_HW_SHADER_CACHE`, `GT4_HW_TEXTURES` and
  `GT4_HW_TEXTURE_DUMP_DIR` are read with `getenv` (Linux paths are bytes); `<x86intrin.h>` and `<cmath>`.
- `writeback/hw_gs_vulkan_device_table.cpp`: the already loaded `libvulkan.so.1` (`RTLD_NOLOAD`).
- `hw_gs_stream_sink.cpp`: the profile lines' wall clock and CPU times through two helpers (QueryPerformanceCounter and
  GetThreadTimes/GetProcessTimes on Windows, `clock_gettime` on Linux). The Windows results are the same; the text
  moved into the helpers.
- `hw_gs_prepare.cpp`, `writeback/tests/hw_writeback_tests.cpp`, `writeback/hw_writeback_replay.cpp`: `_putenv_s`,
  `_getpid`, `_exit` and the 64-bit seeks from `native/host_compat.hpp` (which gains `_getpid` and `<unistd.h>`).
- New Linux-only files: `hud/hw_hud_system_linux.cpp` (the HUD system interface: no font, no readings) and
  `hw_cpu_sampler_linux.cpp` (the TASK-027 CPU sampler, a Windows diagnostic, off on Linux).
- `shaders/CMakeLists.txt`: the GPU exactness fixture links the Vulkan SDK import library on Windows (unchanged lines)
  and `Vulkan::Vulkan` on Linux.
- `cmake/LinuxCoreGS.cmake` (new): the targets and tests of `native/hw_gs/CMakeLists.txt` for Linux, which stays the
  Windows build file. Not built on Linux: `hw_gs_smoke` (a Win32 window tool) and `hw_cpu_sampler_fixture`;
  `hw_hud_fixture` is built but not registered (two checks need Windows to draw the font).

### How to build and test

```sh
cmake -S . -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DGT4_LINUX_HW_GS=ON
cmake --build <build dir>
ctest --test-dir <build dir> -j 4
```

Needs `glslangValidator` (openSUSE: `glslang-devel`) and the Vulkan headers and loader; no private assets.

## Part 2 (next): CoreGS in the game

The game reaches CoreGS through the GS stream seam with `GT4_RENDERER=hw` (in-process sink, no GS process): the
`--gs-stream` runtime capsule (slice191), a port derived with `--gs-stream-hooks` (and `--linux-surfaces`), the
`gt4_gs_stream_client` library (its process and shared-memory transport are Win32 and are not needed in hw mode), and
the Linux window host attaching its X11/Wayland window to the hw backend.
