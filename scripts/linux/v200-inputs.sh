#!/usr/bin/env bash
# TASK-074: derive every v2.00 input of the Linux build after the L1 producer (tools/ee_producer/build_linux.sh), each
# checked against its pin, as docs/linux/L2-PERFORMANCE.md, L2-COREGS.md, L2-PATCHES.md and L2-FRAME-SKIP.md describe them
# one by one. The counterpart of spec2-inputs.sh. Nothing is written over: every output folder must be new.
#
# Usage: scripts/linux/v200-inputs.sh [prefix]
#   prefix (default linux074) names the folders under private/ (GT4_LINUX_* in parentheses):
#     <prefix>-vu1-image, -vu1-gen   VU1 image and AOT table (GT4_LINUX_VU1_GENERATED_DIR)   pin slice408-pcsx2.json
#     <prefix>-vu0-image, -vu0-gen   VU0 image and AOT table (GT4_LINUX_VU0_*)                 pin slice408-pcsx2.json
#     <prefix>-frame-cost-runtime    prepare_perf_runtime.py --gs-stream --frame-cost (GT4_LINUX_RUNTIME_CAPSULE)
#                                                                                              pin slice262-frame-cost-runtime.json
#     <prefix>-port-hw               prepare_external_vulkan.py --gs-stream-hooks --linux-surfaces (GT4_LINUX_PORT_DIR)
#     <prefix>-frameskip-guard-runtime  prepare_license_guard.py ... --frame-skip (GT4_LINUX_LICENSE_GUARD, both games)
#                                                                                              pin slice376-frame-render-license-guard-runtime.json
#     <prefix>-usb-runtime           prepare_usb_runtime.py (GT4_LINUX_USB_RUNTIME)            pin slice409-usb-runtime.json
#     <prefix>-memcard-runtime       prepare_memcard_flush.py (GT4_LINUX_MEMCARD_RUNTIME)      pin slice408-pcsx2.json
#     <prefix>-ipu-fast-runtime      prepare_ipu_fast.py (GT4_LINUX_IPU_RUNTIME, both games)   pin slice408-pcsx2.json
#     <prefix>-spu2-runtime          prepare_spu2_runtime.py (GT4_LINUX_SPU2_RUNTIME, both games) pin slice408-pcsx2.json
#     <prefix>-aspect                the TASK-080 patched ELF and its translation (pinned seeds)
#     <prefix>-release-units         camera360_units.py v200 output (GT4_LINUX_GAME_UNITS)     pin slice408-pcsx2.json
#
# Needs: private/tooling-venv, the producer private/<prefix>-ee-producer (SCUS_973.28.elf and tool-build/eerecomp) and
# gt4_vu_recomp from a configured Linux build (GT4_LINUX_VU_RECOMP, default private/<prefix>-frameskip-build/gt4_vu_recomp).
# The shaders (<prefix>-shaders) are the L1 ones.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd -P)
cd "$root"
prefix=${1:-linux074}
py=private/tooling-venv/bin/python
out=$root/private/$prefix
elf=$out-ee-producer/SCUS_973.28.elf
translator=$out-ee-producer/tool-build/eerecomp
vu_recomp=${GT4_LINUX_VU_RECOMP:-$out-frameskip-build/gt4_vu_recomp}
for tool in "$py" "$translator" "$vu_recomp"; do [[ -x $tool ]] || { echo "missing $tool" >&2; exit 2; }; done
[[ -f $elf ]] || { echo "missing $elf (run tools/ee_producer/build_linux.sh first)" >&2; exit 2; }
folders="vu1-image vu1-gen vu0-image vu0-gen frame-cost-runtime port-hw frameskip-guard-runtime usb-runtime memcard-runtime spu2-runtime ipu-fast-runtime aspect release-units"
for folder in $folders; do
    [[ ! -e $out-$folder ]] || { echo "$out-$folder exists; choose a new prefix" >&2; exit 2; }
done
pin() { $py -I -c "import json,sys; d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split('/'): d=d[int(k)] if isinstance(d, list) else d[k]
print(d)" "docs/inputs/$1" "$2"; }
hash() { sha256sum "$1" | cut -c1-64; }
same() {  # <what> <actual> <pinned>
    if [[ $2 != "$3" ]]; then echo "FAILED $1: $2, pin $3" >&2; exit 1; fi
    echo "ok  $1 ${2:0:16}"
}

# 1. VU1 and VU0: the images from the ELF, their entry lists, the AOT tables (L2-PERFORMANCE.md).
$py scripts/vu1_image.py build --elf "$elf" --pinned-tail --out "$out-vu1-image" > /dev/null
$py scripts/vu1_image.py entries --image "$out-vu1-image/vu1_image.bin" --policy all --out "$out-vu1-image/entries-all.txt" > /dev/null
same "VU1 image" "$(hash "$out-vu1-image/vu1_image.bin")" "$(pin slice408-pcsx2.json vu1_aot/image_sha256)"
"$vu_recomp" --programs-per-file 16 --entry-policy all --image "$out-vu1-image/vu1_image.bin" \
    --entries "$out-vu1-image/entries-all.txt" "$out-vu1-gen" > /dev/null
same "VU1 table" "$(hash "$out-vu1-gen/vu1_generated.cpp")" "$(pin slice408-pcsx2.json vu1_aot/generated_source_sha256)"
$py scripts/vu0_image.py build --elf "$elf" --out "$out-vu0-image" > /dev/null
$py scripts/vu0_image.py entries --image "$out-vu0-image/vu0_image.bin" --out "$out-vu0-image/entries.txt" > /dev/null
same "VU0 image" "$(hash "$out-vu0-image/vu0_image.bin")" "$(pin slice408-pcsx2.json vu0_aot/image_sha256)"
"$vu_recomp" --unit 0 --namespace gt4::vugen::vu0 --entry-policy vcallms --image "$out-vu0-image/vu0_image.bin" \
    --entries "$out-vu0-image/entries.txt" "$out-vu0-gen" > /dev/null
same "VU0 table" "$(hash "$out-vu0-gen/vu0_generated.cpp")" "$(pin slice408-pcsx2.json vu0_aot/table_sha256)"

# 2. The capsules: runtime (frame cost), port, guard with the frame skip hook, USB, memory card.
$py scripts/prepare_perf_runtime.py "$out-frame-cost-runtime" --gs-stream --frame-cost > /dev/null
for source in dmac.cpp gif.cpp hw.cpp memory.cpp system.cpp vif.cpp vu_micro.cpp; do
    same "runtime $source" "$(hash "$out-frame-cost-runtime/$source")" "$(pin slice262-frame-cost-runtime.json source_sha256/$source)"
done
$py scripts/prepare_external_vulkan.py "$out-port-hw" --gs-stream-hooks --linux-surfaces > /dev/null
echo "ok  port (no pin: the Linux surfaces; its Win32 derivation equals the slice262 port, docs/linux/WINDOWS-CROSS.md)"
$py scripts/prepare_license_guard.py "$out-frameskip-guard-runtime" --display-aspect --frame-counters --frame-skip > /dev/null
for source in cdvdfsv.cpp dispatch.cpp kernel.cpp; do
    same "guard $source" "$(hash "$out-frameskip-guard-runtime/$source")" \
        "$(pin slice376-frame-render-license-guard-runtime.json source_sha256/$source)"
done
$py scripts/prepare_usb_runtime.py "$out-usb-runtime" > /dev/null
for index in 0 1; do
    name=$(basename "$(pin slice409-usb-runtime.json files/$index/path)")
    same "USB $name" "$(hash "$out-usb-runtime/$name")" "$(pin slice409-usb-runtime.json files/$index/sha256)"
done
$py scripts/prepare_memcard_flush.py "$out-memcard-runtime" > /dev/null
same "memcard sio2.cpp" "$(hash "$out-memcard-runtime/sio2.cpp")" "$(pin slice408-pcsx2.json memcard_capsule_sha256/sio2.cpp)"
$py scripts/prepare_spu2_runtime.py "$out-spu2-runtime" > /dev/null
same "SPU2 receipt" "$(hash "$out-spu2-runtime/runtime.json")" "$(pin slice408-pcsx2.json spu2_capsule_sha256)"
$py scripts/prepare_ipu_fast.py "$out-ipu-fast-runtime" > /dev/null
same "IPU receipt" "$(hash "$out-ipu-fast-runtime/runtime.json")" "$(pin slice408-pcsx2.json ipu_capsule_sha256)"

# 3. The release units: the aspect-patched ELF translated with the pinned seeds, then the hook sites (L2-PATCHES.md).
$py -I - "$elf" "$out-aspect" "$translator" <<'EOF'
import subprocess, sys
from pathlib import Path
sys.path.insert(0, "scripts")
import aspect_elf_patch as ap, ee_entries
elf, out, translator = map(Path, sys.argv[1:])
out.mkdir()
(out / "SCUS_973.28.aspect.elf").write_bytes(ap.patch_elf(elf.read_bytes()))
seed_file, seeds, pin = ee_entries.seed_set_pin(ap.SEED_SET)
result = subprocess.run([str(translator), str(out / "SCUS_973.28.aspect.elf"), "--out", str(out / "generated"), "--stats",
                         "--entries", str(seed_file)], capture_output=True, text=True, check=True)
(out / "generation.log").write_text(result.stdout + result.stderr)
EOF
$py -I scripts/camera360_units.py v200 "$out-aspect/generated" "$out-release-units" > /dev/null
units=$($py -I -c "import json,sys; print(json.load(open(sys.argv[1]))['output_inventory_sha256'])" "$out-release-units.camera360-units.json")
same "release units" "$units" "$(pin slice408-pcsx2.json camera360_units/output_inventory_sha256)"
echo "every v2.00 input equals its pin"
