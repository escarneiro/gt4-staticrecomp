# TASK-074 (Linux build, plan step L1: host-only part; docs/linux/PLAN.md on branch linux/main): the Linux host-only build.
#
# Builds the platform-neutral host libraries and their CTest fixtures with Clang or GCC on Linux x86-64, so the pure
# rules (pacing, display aspect, hooks tables, controls, MTVU journal, memory card writer, performance accounting...) are
# checked on Linux too. It needs no private assets: no donor runtime, no EE game library, no SDL3 package, no Python
# environment. The game executable, the Vulkan/GPU fixtures and everything that talks to Win32 (window, WinMM/XInput
# input and audio, WinHTTP achievements, the GS stream shared-memory transport) are not built here yet; they are the
# later Linux steps (TASK-074 steps L1-L4).
#
# The Windows build never reads this file: the root CMakeLists.txt includes it only when WIN32 is false and then
# returns, so the qualified Windows recipe is unchanged.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
   OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "The host-only build outside Windows targets Linux x86-64")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU)$")
    message(FATAL_ERROR "The Linux host build needs Clang or GCC (found ${CMAKE_CXX_COMPILER_ID})")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Same code-generation switches as the Windows tree (root CMakeLists.txt): SSE4.1 is the CPU floor; AVX2 and FMA
# contraction stay A/B options.
option(GT4_FP_CONTRACT_OFF "Build with -ffp-contract=off (control: no fused multiply-add)" OFF)
option(GT4_X86_64_V3 "Build for x86-64-v3 (AVX2, FMA); implies GT4_FP_CONTRACT_OFF" OFF)
option(GT4_SSE41 "Build for SSE4.1 (the launcher's CPU floor)" ON)
if(GT4_FP_CONTRACT_OFF OR GT4_X86_64_V3)
    add_compile_options(-ffp-contract=off)
endif()
if(GT4_X86_64_V3)
    add_compile_options(-march=x86-64-v3)
elseif(GT4_SSE41)
    add_compile_options(-msse4.1)
endif()

set(GT4_TARGET "v200" CACHE STRING "Game target for the host hooks")
set_property(CACHE GT4_TARGET PROPERTY STRINGS v200 spec2)
if(GT4_TARGET STREQUAL "spec2")
    add_compile_definitions(GT4_TARGET_SPEC2)
elseif(NOT GT4_TARGET STREQUAL "v200")
    message(FATAL_ERROR "Unknown GT4_TARGET '${GT4_TARGET}'; expected v200 or spec2")
endif()
# The perf accounting enum carries the MTVU categories in every target (native/CMakeLists.txt, TASK-011 step 3).
add_compile_definitions(GT4_MTVU_RUNTIME)

set(GT4_NATIVE "${CMAKE_SOURCE_DIR}/native")
# Counterpart of /W4 in the Windows tree.
set(GT4_WARNINGS -Wall -Wextra)

enable_testing()

# gt4_host_target(<name>): applies the shared include path and warning level to a host target.
function(gt4_host_target name)
    target_include_directories(${name} PRIVATE "${GT4_NATIVE}")
    target_compile_options(${name} PRIVATE ${GT4_WARNINGS})
endfunction()

function(gt4_host_library name)
    list(TRANSFORM ARGN PREPEND "${GT4_NATIVE}/")
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC "${GT4_NATIVE}")
    target_compile_options(${name} PRIVATE ${GT4_WARNINGS})
endfunction()

function(gt4_host_fixture name)
    list(TRANSFORM ARGN PREPEND "${GT4_NATIVE}/")
    add_executable(${name} ${ARGN})
    gt4_host_target(${name})
endfunction()

# Libraries (same names and sources as native/CMakeLists.txt).
gt4_host_library(gt4_pacing pacing.cpp)
gt4_host_library(gt4_quickfix frame_skip.cpp window_mode.cpp race_display.cpp race_widescreen.cpp cursor_autohide.cpp
    display_aspect.cpp flare_limits.cpp camera_360.cpp fov_adjust.cpp trigger_range.cpp)
if(GT4_TARGET STREQUAL "spec2")
    gt4_host_library(gt4_quickfix_v200_fixture race_display.cpp race_widescreen.cpp display_aspect.cpp flare_limits.cpp
        camera_360.cpp fov_adjust.cpp trigger_range.cpp)
    target_compile_definitions(gt4_quickfix_v200_fixture PUBLIC GT4_TARGET_FIXTURE_V200)
    set(GT4_HOOK_FIXTURE_LIB gt4_quickfix_v200_fixture)
else()
    set(GT4_HOOK_FIXTURE_LIB gt4_quickfix)
endif()
gt4_host_library(gt4_vulkan_extensions vulkan_extensions.cpp)
gt4_host_library(gt4_usb usb_ohci.cpp usb_dfp.cpp)
gt4_host_library(gt4_memcard memcard_store.cpp)
gt4_host_library(gt4_license_guard license_guard.cpp)
gt4_host_library(gt4_pad_mapping pad_mapping.cpp pad_choice.cpp input_recording.cpp)
gt4_host_library(gt4_perf_probe perf_probe.cpp perf_clock.cpp)
gt4_host_library(gt4_mtvu_measure mtvu_measure.cpp)
gt4_host_library(gt4_mtvu_journal mtvu_journal.cpp)
# The .gscap file format and the READ_FIFO probe only; the shared-memory transport (gs_stream_ipc.cpp) is Win32.
gt4_host_library(gt4_gs_stream_files gs_stream/gs_stream_file.cpp gs_stream/gs_readback_probe.cpp)
target_include_directories(gt4_gs_stream_files PUBLIC "${GT4_NATIVE}/gs_stream")

# Fixtures and their tests (test names match the Windows tree, so results compare one to one).
gt4_host_fixture(vulkan_extensions_fixture vulkan_extensions_fixture.cpp)
target_link_libraries(vulkan_extensions_fixture PRIVATE gt4_vulkan_extensions)
add_test(NAME native_vulkan_extensions COMMAND $<TARGET_FILE:vulkan_extensions_fixture>)

gt4_host_fixture(usb_dfp_fixture usb_dfp_fixture.cpp)
target_link_libraries(usb_dfp_fixture PRIVATE gt4_usb)
add_test(NAME native_usb_dfp COMMAND $<TARGET_FILE:usb_dfp_fixture>)

gt4_host_fixture(memcard_store_fixture memcard_store_fixture.cpp)
target_link_libraries(memcard_store_fixture PRIVATE gt4_memcard)
add_test(NAME native_memcard_store COMMAND $<TARGET_FILE:memcard_store_fixture> "${CMAKE_BINARY_DIR}/memcard_store_work")

gt4_host_fixture(license_guard_fixture license_guard_fixture.cpp)
target_link_libraries(license_guard_fixture PRIVATE gt4_license_guard)
add_test(NAME native_license_guard COMMAND $<TARGET_FILE:license_guard_fixture>)

gt4_host_fixture(standalone_fixture standalone_fixture.cpp standalone.cpp)
target_link_libraries(standalone_fixture PRIVATE gt4_pad_mapping)
target_compile_definitions(standalone_fixture PRIVATE GT4_STANDALONE_SAMPLE_DIR="${GT4_NATIVE}/standalone_samples")
add_test(NAME native_standalone COMMAND $<TARGET_FILE:standalone_fixture>)

gt4_host_fixture(game_target_fixture game_target_fixture.cpp game_target_spec2_fixture.cpp)
set_source_files_properties("${GT4_NATIVE}/game_target_fixture.cpp" PROPERTIES COMPILE_DEFINITIONS GT4_TARGET_FIXTURE_V200)
add_test(NAME native_game_target COMMAND $<TARGET_FILE:game_target_fixture>)

gt4_host_fixture(spec2_iop_fixture spec2_iop_fixture.cpp)
add_test(NAME native_spec2_iop COMMAND $<TARGET_FILE:spec2_iop_fixture>)

gt4_host_fixture(spec2_hooks_fixture spec2_hooks_fixture.cpp race_widescreen.cpp race_display.cpp license_guard.cpp)
target_compile_definitions(spec2_hooks_fixture PRIVATE GT4_TARGET_SPEC2)
add_test(NAME native_spec2_hooks COMMAND $<TARGET_FILE:spec2_hooks_fixture>)

gt4_host_fixture(mtvu_measure_fixture mtvu_measure_fixture.cpp)
target_link_libraries(mtvu_measure_fixture PRIVATE gt4_mtvu_measure)
add_test(NAME native_mtvu_measure COMMAND $<TARGET_FILE:mtvu_measure_fixture>)

gt4_host_fixture(mtvu_ring_fixture mtvu_ring_fixture.cpp)
add_test(NAME native_mtvu_ring COMMAND $<TARGET_FILE:mtvu_ring_fixture>)

gt4_host_fixture(mtvu_journal_fixture mtvu_journal_fixture.cpp)
target_link_libraries(mtvu_journal_fixture PRIVATE gt4_mtvu_journal)
add_test(NAME native_mtvu_journal COMMAND $<TARGET_FILE:mtvu_journal_fixture>)

gt4_host_fixture(mtvu_perf_fixture mtvu_perf_fixture.cpp)
target_link_libraries(mtvu_perf_fixture PRIVATE gt4_perf_probe)
add_test(NAME native_mtvu_perf COMMAND $<TARGET_FILE:mtvu_perf_fixture>)

gt4_host_fixture(mtvu_vif_batch_fixture mtvu_vif_batch_fixture.cpp)
add_test(NAME native_mtvu_vif_batch COMMAND $<TARGET_FILE:mtvu_vif_batch_fixture>)

gt4_host_fixture(mtvu_switch_fixture mtvu_switch_fixture.cpp)
add_test(NAME native_mtvu_switches COMMAND $<TARGET_FILE:mtvu_switch_fixture>)

gt4_host_fixture(gs_readback_probe_fixture gs_stream/gs_readback_probe_fixture.cpp)
target_link_libraries(gs_readback_probe_fixture PRIVATE gt4_gs_stream_files)
add_test(NAME native_gs_readback_probe
    COMMAND $<TARGET_FILE:gs_readback_probe_fixture> "${CMAKE_BINARY_DIR}/gs-readback-probe-work")

gt4_host_fixture(gs_submit_ring_fixture gs_submit_ring_fixture.cpp)
add_test(NAME native_gs_submit_ring COMMAND $<TARGET_FILE:gs_submit_ring_fixture>)

gt4_host_fixture(spu2_envelope_fixture spu2_envelope_fixture.cpp)
add_test(NAME native_spu2_envelope COMMAND $<TARGET_FILE:spu2_envelope_fixture>)

gt4_host_fixture(ipu_fast_fixture ipu_fast_fixture.cpp)
add_test(NAME native_ipu_fast COMMAND $<TARGET_FILE:ipu_fast_fixture>)

gt4_host_fixture(frame_skip_fixture frame_skip_fixture.cpp frame_skip.cpp)
target_link_libraries(frame_skip_fixture PRIVATE gt4_pacing)
add_test(NAME native_frame_skip COMMAND $<TARGET_FILE:frame_skip_fixture>)

gt4_host_fixture(pacing_fixture pacing_fixture.cpp)
target_link_libraries(pacing_fixture PRIVATE gt4_pacing)
add_test(NAME native_pacing COMMAND $<TARGET_FILE:pacing_fixture>)

gt4_host_fixture(controls_fixture controls_fixture.cpp controls.cpp)
target_compile_definitions(controls_fixture PRIVATE GT4_CONTROLS_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/docs/controls")
add_test(NAME native_controls COMMAND $<TARGET_FILE:controls_fixture>)

gt4_host_fixture(input_recording_fixture input_recording_fixture.cpp)
target_link_libraries(input_recording_fixture PRIVATE gt4_pad_mapping)
add_test(NAME native_input_recording COMMAND $<TARGET_FILE:input_recording_fixture> native_input_recording.txt)

gt4_host_fixture(pad_mapping_fixture pad_mapping_fixture.cpp controls.cpp)
target_link_libraries(pad_mapping_fixture PRIVATE gt4_pad_mapping)
add_test(NAME native_pad_mapping COMMAND $<TARGET_FILE:pad_mapping_fixture>)

gt4_host_fixture(shape_observer_fixture shape_observer_fixture.cpp shape_observer.cpp)
add_test(NAME native_shape_observer COMMAND $<TARGET_FILE:shape_observer_fixture>)

gt4_host_fixture(perf_accounting_fixture perf_accounting_fixture.cpp)
add_test(NAME native_performance_accounting COMMAND $<TARGET_FILE:perf_accounting_fixture>)

gt4_host_fixture(perf_probe_fixture perf_probe_fixture.cpp)
target_link_libraries(perf_probe_fixture PRIVATE gt4_perf_probe)
add_test(NAME native_performance_probe COMMAND $<TARGET_FILE:perf_probe_fixture>)

gt4_host_fixture(fpu_chop_fixture fpu_chop_fixture.cpp)
add_test(NAME native_fpu_chop COMMAND $<TARGET_FILE:fpu_chop_fixture>)

gt4_host_fixture(quickfix_fixture quickfix_fixture.cpp)
target_link_libraries(quickfix_fixture PRIVATE gt4_quickfix)
add_test(NAME native_quickfix COMMAND $<TARGET_FILE:quickfix_fixture>)

gt4_host_fixture(cursor_autohide_fixture cursor_autohide_fixture.cpp)
target_link_libraries(cursor_autohide_fixture PRIVATE gt4_quickfix)
add_test(NAME native_cursor_autohide COMMAND $<TARGET_FILE:cursor_autohide_fixture>)

# The guest-hook fixtures assert the v2.00 chains (GT4_HOOK_FIXTURE_LIB, as in the Windows tree).
foreach(hook IN ITEMS race_display race_widescreen display_aspect camera_360 fov_adjust trigger_range flare_limits)
    gt4_host_fixture(${hook}_fixture ${hook}_fixture.cpp)
    target_link_libraries(${hook}_fixture PRIVATE ${GT4_HOOK_FIXTURE_LIB})
    add_test(NAME native_${hook} COMMAND $<TARGET_FILE:${hook}_fixture>)
endforeach()

foreach(v200_fixture IN ITEMS license_guard_fixture race_display_fixture race_widescreen_fixture display_aspect_fixture
                               shape_observer_fixture)
    target_compile_definitions(${v200_fixture} PRIVATE GT4_TARGET_FIXTURE_V200)
endforeach()

# Same scheduling as the Windows tree: fixtures that write files or measure time run alone.
set_tests_properties(native_mtvu_measure native_mtvu_ring native_mtvu_journal native_mtvu_perf native_mtvu_vif_batch
    native_quickfix native_race_display native_race_widescreen native_shape_observer native_performance_accounting
    native_performance_probe native_gs_submit_ring native_flare_limits native_gs_readback_probe
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Optional headless smoke runner (TASK-074 L1): the donor runtime (the same source list as RUNTIME_SOURCES in
# native/CMakeLists.txt, without the Vulkan GS backend, so frames come from the donor's CPU rasterizer) and the donor's
# headless host/main.cpp, linked with a Linux candidate game library from scripts/ee_producer.py. VU1 microcode runs
# in the donor interpreter (the weak register_generated_vu). No window, audio or input: this proves the game code
# boots and draws on Linux, nothing more. Both paths stay empty by default; the host-only build above needs neither.
set(GT4_DONOR "" CACHE PATH "Verified ps2-web-recomp checkout (private/references/ps2-web-recomp)")
set(GT4_LINUX_PRODUCER "" CACHE PATH "Linux candidate EE producer directory holding linux-library.json")
if(GT4_DONOR OR GT4_LINUX_PRODUCER)
    if(NOT GT4_DONOR OR NOT GT4_LINUX_PRODUCER)
        message(FATAL_ERROR "The headless smoke runner needs both GT4_DONOR and GT4_LINUX_PRODUCER")
    endif()
    if(NOT EXISTS "${GT4_DONOR}/runtime/include/ps2/system.hpp" OR NOT EXISTS "${GT4_DONOR}/host/main.cpp")
        message(FATAL_ERROR "GT4_DONOR is not a ps2-web-recomp checkout; run scripts/ee_producer.py prepare first")
    endif()
    # The library must be the one ee_producer.py record-library verified and recorded, byte for byte.
    set(gt4_linux_library "${GT4_LINUX_PRODUCER}/game-build/libps2game.a")
    if(NOT EXISTS "${GT4_LINUX_PRODUCER}/linux-library.json" OR NOT EXISTS "${gt4_linux_library}")
        message(FATAL_ERROR "GT4_LINUX_PRODUCER has no recorded Linux library; run ee_producer.py record-library")
    endif()
    file(READ "${GT4_LINUX_PRODUCER}/linux-library.json" gt4_linux_receipt)
    string(JSON gt4_linux_expected GET "${gt4_linux_receipt}" library sha256)
    file(SHA256 "${gt4_linux_library}" gt4_linux_actual)
    if(NOT gt4_linux_actual STREQUAL gt4_linux_expected)
        message(FATAL_ERROR "libps2game.a differs from its linux-library.json record")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GT4_LINUX_PRODUCER}/linux-library.json"
        "${gt4_linux_library}")

    set(gt4_headless_runtime)
    foreach(source IN ITEMS memory dispatch system kernel hw cop disc dmac vif vu_micro vu_jit vu_jit_web
                            ipu/ipu gs/gif gs/queue gs/raster iop/cpu iop/kernel iop/libs iop/hw iop/spu2 iop/sio2
                            iop/io iop/cdvd iop/sif iop/cdvdfsv)
        list(APPEND gt4_headless_runtime "${GT4_DONOR}/runtime/src/${source}.cpp")
    endforeach()
    # gs/gs.cpp holds the GS model; the Vulkan backend (gs/gpu/gpu.cpp, PS2_GPU) is not part of the smoke runner.
    list(APPEND gt4_headless_runtime "${GT4_DONOR}/runtime/src/gs/gs.cpp")
    add_library(gt4_ps2runtime_headless STATIC ${gt4_headless_runtime})
    target_include_directories(gt4_ps2runtime_headless PUBLIC "${GT4_DONOR}/runtime/include"
        PRIVATE "${GT4_DONOR}/runtime/src" "${GT4_DONOR}/runtime/src/gs")
    find_package(Threads REQUIRED)
    target_link_libraries(gt4_ps2runtime_headless PUBLIC Threads::Threads)

    add_executable(gt4_headless_smoke "${GT4_DONOR}/host/main.cpp")
    # The game library and the runtime call each other (dispatch tables and runtime services): link them as a group.
    target_link_libraries(gt4_headless_smoke PRIVATE
        "$<LINK_GROUP:RESCAN,${gt4_linux_library},gt4_ps2runtime_headless>")
endif()
