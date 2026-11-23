# TASK-074 side tool: cross-compile the Windows game on Linux with the release's compiler family (clang-cl, lld-link), for a
# test under wine. Not a release recipe: the MSVC CRT and Windows SDK come from xwin (`xwin --accept-license splat`, the
# Microsoft licence accepted by the person who runs it), not from Visual Studio, so executables never equal the release pins.
#
#   cmake -S . -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/windows-cross/clang-cl-xwin.cmake \
#         -DGT4_XWIN_SYSROOT=<splat output> ...
# docs/linux/WINDOWS-CROSS.md has the whole recipe.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(GT4_XWIN_SYSROOT "" CACHE PATH "xwin splat output (crt/ and sdk/)")
if(NOT GT4_XWIN_SYSROOT OR NOT EXISTS "${GT4_XWIN_SYSROOT}/crt/include/vcruntime.h")
    message(FATAL_ERROR "GT4_XWIN_SYSROOT must be an xwin splat output with crt/include/vcruntime.h")
endif()
# Forwarded to try_compile projects, which read the toolchain file again.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GT4_XWIN_SYSROOT GT4_LLVM_SUFFIX)

# The LLVM tools of the clang in use. openSUSE names them with the major version (llvm-rc-23).
set(GT4_LLVM_SUFFIX "" CACHE STRING "Suffix of the versioned LLVM tools, e.g. -23")
find_program(CMAKE_C_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_LINKER NAMES lld-link REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES llvm-rc${GT4_LLVM_SUFFIX} llvm-rc REQUIRED)
find_program(CMAKE_MT NAMES llvm-mt${GT4_LLVM_SUFFIX} llvm-mt REQUIRED)
find_program(CMAKE_AR NAMES llvm-lib${GT4_LLVM_SUFFIX} llvm-lib REQUIRED)

set(gt4_xwin_includes
    "${GT4_XWIN_SYSROOT}/crt/include"
    "${GT4_XWIN_SYSROOT}/sdk/include/ucrt"
    "${GT4_XWIN_SYSROOT}/sdk/include/um"
    "${GT4_XWIN_SYSROOT}/sdk/include/shared"
    "${GT4_XWIN_SYSROOT}/sdk/include/winrt")
set(gt4_xwin_compile "--target=x86_64-pc-windows-msvc")
foreach(dir IN LISTS gt4_xwin_includes)
    string(APPEND gt4_xwin_compile " /imsvc\"${dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${gt4_xwin_compile}")
set(CMAKE_CXX_FLAGS_INIT "${gt4_xwin_compile}")
# llvm-rc finds windows.h through the same folders.
set(CMAKE_RC_FLAGS_INIT "")
foreach(dir IN LISTS gt4_xwin_includes)
    string(APPEND CMAKE_RC_FLAGS_INIT " /I \"${dir}\"")
endforeach()

set(gt4_xwin_link
    "/libpath:\"${GT4_XWIN_SYSROOT}/crt/lib/x86_64\" /libpath:\"${GT4_XWIN_SYSROOT}/sdk/lib/um/x86_64\" /libpath:\"${GT4_XWIN_SYSROOT}/sdk/lib/ucrt/x86_64\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${gt4_xwin_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${gt4_xwin_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${gt4_xwin_link}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# xwin splats the release CRT only (no msvcrtd.lib): compiler checks link as Release, and builds use Release.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
