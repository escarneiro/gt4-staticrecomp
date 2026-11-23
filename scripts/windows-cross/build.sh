#!/usr/bin/env bash
# TASK-074 side tool: a Windows test build of GT4 v2.00 cross-compiled on Linux (clang-cl, lld-link, xwin), to run under
# wine. The same CMake as scripts/coregs2_bench/game/configure.cmd, with the inputs of prepare-inputs.sh and
# GT4_WINDOWS_CROSS_TEST=ON; CoreGS 2.0 shaders from the system glslangValidator (its own hash, as the Linux build). Plain Release, no PGO; the executable never equals a release pin.
#
#   scripts/windows-cross/build.sh ee      the EE game library from private/linux074-release-units (private/wincross-ee-build)
#   scripts/windows-cross/build.sh game    the game (private/wincross-build/gt4vulkan.exe)
# docs/linux/WINDOWS-CROSS.md.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd -P)
private=$root/private
sysroot=${GT4_XWIN_SYSROOT:-$private/tooling/msvc-sysroot}
suffix=${GT4_LLVM_SUFFIX:--23}
python=$private/tooling-venv/bin/python
flags="/O2 /Ob2 /DNDEBUG /clang:-msse4.1 /Z7 -gline-tables-only"
toolchain=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$root/scripts/windows-cross/clang-cl-xwin.cmake"
           "-DGT4_XWIN_SYSROOT=$sysroot" "-DGT4_LLVM_SUFFIX=$suffix")
ee=$private/wincross-ee-build
game=$private/wincross-build

case ${1:-} in
ee)
    cmake -S "$root/tools/ee_producer" -B "$ee" "${toolchain[@]}" "-DCMAKE_CXX_FLAGS_RELEASE=$flags" \
        "-DGT4_DONOR=$private/references/ps2-web-recomp" "-DGT4_PYTHON=$python" \
        "-DGT4_GENERATED=$private/linux074-release-units" -DGT4_BUILD_TRANSLATOR=OFF -DGT4_FAST_HEADERS=ON
    cmake --build "$ee" --target ps2game
    sha256sum "$ee/ps2game.lib"
    ;;
game)
    [[ -f $ee/ps2game.lib ]] || { echo "run '$0 ee' first" >&2; exit 2; }
    library_sha=$(sha256sum "$ee/ps2game.lib" | cut -c1-64)
    cmake -S "$root" -B "$game" "${toolchain[@]}" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$flags" "-DCMAKE_C_FLAGS_RELEASE=$flags" \
        "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" \
        -DGT4_WINDOWS_CROSS_TEST=ON "-DGT4_VULKAN_HEADERS=$private/tooling/vulkan-sdk-cross/Include" \
        "-DGT4_ASSETS_ROOT=$root" -DGT4_EE_PROFILE=aspect -DGT4_TARGET=v200 \
        -DGT4_HW_GS=ON "-DGT4_HW_VULKAN_SDK=$private/tooling/vulkan-sdk-cross" "-DGT4_HW_GLSLANG=$(command -v glslangValidator)" \
        "-DGT4_HW_GLSLANG_SHA256=$(sha256sum "$(command -v glslangValidator)" | cut -c1-64)" \
        "-DGT4_IPU_RUNTIME_SOURCE=$private/slice365-ipu-fast-runtime" \
        "-DGT4_LICENSE_GUARD_SOURCE=$private/slice376-frame-render-license-guard-runtime" \
        "-DGT4_MEMCARD_RUNTIME_SOURCE=$private/slice200-memcard-runtime" \
        "-DGT4_PERF_RUNTIME_SOURCE=$private/slice262-frame-cost-runtime" \
        "-DGT4_SPU2_RUNTIME_SOURCE=$private/slice396-spu2-runtime" \
        "-DGT4_USB_RUNTIME_SOURCE=$private/slice409-usb-runtime" \
        "-DGT4_PORT_DIR=$private/slice200-port" "-DGT4_PYTHON=$python" -DGT4_RELEASE_DISTRIBUTION=ON \
        "-DGT4_SDL3_ROOT=$private/tooling/sdl3-3.4.18" "-DGT4_SHADER_DIR=$private/slice153-pacing-shaders" \
        "-DGT4_VU0_GENERATED_DIR=$private/slice394-v200-vu0-gen" "-DGT4_VU0_IMAGE_FILE=$private/slice160-vu0-image/vu0_image.bin" \
        "-DGT4_VU1_GENERATED_DIR=$private/slice397-vu1-gen" \
        "-DGT4_EE_LIBRARY=$ee/ps2game.lib" "-DGT4_EE_LIBRARY_SHA256=$library_sha" "${@:2}"
    cmake --build "$game" --target gt4vulkan
    sha256sum "$game/gt4vulkan.exe"
    ;;
*)
    echo "usage: $0 ee | game [extra cmake arguments...]" >&2
    exit 2
    ;;
esac
