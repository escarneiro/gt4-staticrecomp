# TASK-074 side tool: a Windows test build cross-compiled on Linux, run under wine

Why: this branch changes Windows files too (`native/window.cpp`, `native/CMakeLists.txt`, `configure.cmd`), and no Windows
machine has built it. This build shows that the Windows code compiles with the release's compiler family and that it
starts. **It is not a release recipe, and it does not replace a test on Windows:** the CRT and SDK come from xwin, not
Visual Studio; the executable never equals a release pin; wine is not Windows (the responsiveness timer of decision 0076,
XInput, driver behaviour are not tested by it).

## Toolchain

- clang-cl and lld-link of the system LLVM (23 here), with `llvm-rc-23`, `llvm-mt-23`, `llvm-lib-23`:
  `scripts/windows-cross/clang-cl-xwin.cmake` (`GT4_XWIN_SYSROOT`, `GT4_LLVM_SUFFIX=-23`). Release only: xwin has no debug CRT.
- MSVC CRT and Windows SDK 10.0.26100: `xwin` 0.10.0 (`cargo install xwin --locked --root private/tooling/xwin-bin`), then
  `xwin --accept-license --arch x86_64 --cache-dir private/tooling/xwin-cache splat --output private/tooling/msvc-sysroot`
  (the person running it accepts the Microsoft licence; 631 MB).
- SDL3 3.4.18: the two packages of `docs/inputs/slice210-sdl3.json`, downloaded from the SDL release, unpacked to
  `private/tooling/sdl3-3.4.18` (`devel/`, `runtime/`); the three pinned files match.
- Vulkan: `private/tooling/vulkan-sdk-cross`, `Include/` = links to the system `vulkan/` and `vk_video/` headers,
  `Lib/vulkan-1.lib` = `llvm-dlltool` import library from the `vk*` exports of the system loader. The donor backend's
  headers come from the same folder (`GT4_VULKAN_HEADERS`, new; the default path is unchanged).
- CoreGS 2.0 shaders: the system glslangValidator with its own hash as the pin, as the Linux build.

## Inputs (`scripts/windows-cross/prepare-inputs.sh`)

Each folder of `scripts/coregs2_bench/game/configure.cmd` (v2.00) is made under its own `private/` name, checked against
its pin, and never overwritten:

| Folder | From | Check |
|---|---|---|
| `slice200-port` | `prepare_external_vulkan.py --gs-stream-hooks`, CRLF | gpu.cpp, gpu.hpp, gs.cpp = slice262 manifest; **main.cpp = slice191** (see below) |
| `slice153-pacing-shaders` | `linux074-shaders` | 9/9 = slice262 manifest |
| `slice262-frame-cost-runtime` | `linux074-frame-cost-runtime`, receipt as CRLF | 7/7 sources and the receipt |
| `slice376-frame-render-license-guard-runtime` | `linux074-frameskip-guard-runtime` | 3/3 sources and the receipt |
| `slice200-memcard-runtime`, `slice365-ipu-fast-runtime`, `slice396-spu2-runtime` | `prepare_memcard_flush.py`, `prepare_ipu_fast.py`, `prepare_spu2_runtime.py` | slice408 pins |
| `slice409-usb-runtime` | `linux074-usb-runtime` | slice409 pin |
| `slice397-vu1-gen`, `slice394-v200-vu0-gen`, `slice160-vu0-image` | `linux074-vu1-gen`, `-vu0-gen`, `-vu0-image` | slice408 pins |

**The port's main.cpp.** The slice200 port's `main.cpp` (TASK-059: `--portable-check` and the windowless exit flush) was
edited by hand on the owner's machine; that edit is not in this repository, so its pin cannot be reproduced. The
derivation gives the slice191 `main.cpp` exactly (the file it was edited from). `GT4_WINDOWS_CROSS_TEST=ON` (new, off by
default) lets the port's `main.cpp` equal the slice191 pin, and says so at configure time; every other input keeps its
pin. So this build has no `--portable-check`, and without a window it does not flush the card at exit (the window path
does, in `window.cpp`).

The VU1 corpus (`slice155-vu1-full-corpus`) only feeds an optional CTest differential and is left out.

## Build and run

```
scripts/windows-cross/prepare-inputs.sh
scripts/windows-cross/build.sh ee        # ps2game.lib from private/linux074-release-units, ~1 min
scripts/windows-cross/build.sh game      # private/wincross-build/gt4vulkan.exe
cp private/tooling/sdl3-3.4.18/runtime/SDL3.dll private/wincross-build/
mkdir -p private/wincross-build/shaders && cp private/slice153-pacing-shaders/*.spv private/wincross-build/shaders/

export WINEPREFIX=$PWD/private/wine-gt4      # its own prefix, not ~/.wine
w() { winepath -w "$1"; }
GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=auto GT4_PRESENT_MODE=fifo GT4_RENDERER=hw GT4_HW_CORE=2 \
GT4_HW_SCALE=3 GT4_FRAME_SKIP=1 wine private/wincross-build/gt4vulkan.exe "$(w private/linux074-ee-producer/SCUS_973.28.elf)" \
    --window --ntsc --disc "$(w <disc image>)" --memcard "$(w <memory card>)"
```

## Results (2026-11-23, wine 11.21 Staging, RX 6600 RADV through winevulkan)

- The EE library (326 units) and `gt4vulkan.exe` (`72db3a51...`) compile and link with no error.
- Without a window: boots through the IOPRP300 reboot and the IRX modules; the frame skip hook counts every frame
  (31,197 in 60 s).
- With a window, CoreGS 2.0 at 2x: the window, pacing, responsiveness timer, cursor auto-hide, CORE2 renderer and the GS
  worker start; it runs until stopped. Wine reports the swapchain "suboptimal at the same size" (2560x1080) about every
  frame; the game keeps it and goes on (wine behaviour, Hypothesis).
- Not done: Spec II, the CTest fixtures under wine, a session played by a person, the launcher and installer.
