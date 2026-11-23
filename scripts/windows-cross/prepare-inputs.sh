#!/usr/bin/env bash
# TASK-074 side tool: the inputs of a Windows game build (scripts/coregs2_bench/game/configure.cmd, v2.00), derived on Linux
# under the private/ names native/CMakeLists.txt expects, each checked against its pin before it is kept. A folder that
# already exists is checked and never overwritten. Needs the Linux inputs of docs/linux/L2-*.md (linux074-*) and the
# derivation scripts. The Windows files are CRLF where the pins were taken from a CRLF checkout (port, runtime receipts).
# docs/linux/WINDOWS-CROSS.md.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd -P)
private=$root/private
python=$private/tooling-venv/bin/python
# The derivation scripts write only to immediate children of private/: staging folders live there, removed at exit.
stage=wincross-stage-$$
work=$private/$stage
mkdir "$work"
cleanup() { rm -rf "$work" "$private/$stage"-*; }
trap cleanup EXIT

sha() { sha256sum "$1" | cut -c1-64; }
crlf() { sed 's/\r$//; s/$/\r/' "$1" > "$2"; }
pin() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))
for key in sys.argv[2:]: d=d[int(key)] if isinstance(d, list) else d[key]
print(d)' "$@"; }
expect() {  # <file> <expected sha256> <what>
    local actual
    actual=$(sha "$1")
    [[ $actual == "$2" ]] || { echo "$3: $(basename "$1") is $actual, pin $2" >&2; exit 1; }
}
keep() {  # <staged folder> <private name>: move into place, or check an existing folder holds the same files
    local target=$private/$2
    if [[ -e $target ]]; then
        diff -rq "$1" "$target" > /dev/null || { echo "$target exists and differs; not overwritten" >&2; exit 1; }
        echo "kept   $2 (already there, same files)"
    else
        mv "$1" "$target"
        echo "made   $2"
    fi
}

inputs=$root/docs/inputs
release=$inputs/slice408-pcsx2.json
frame=$inputs/slice262-frame-cost-runtime.json

# Port: the slice200 adapter. gpu.cpp, gpu.hpp and gs.cpp are the --gs-stream-hooks derivation as CRLF; main.cpp is the
# slice191 derivation (GT4_WINDOWS_CROSS_TEST), since the slice200 edit is not in the repository.
"$python" "$root/scripts/prepare_external_vulkan.py" "$private/$stage-port" --gs-stream-hooks > /dev/null
mkdir "$work/port"
for f in gpu.cpp gpu.hpp gs.cpp; do
    crlf "$private/$stage-port/$f" "$work/port/$f"
    expect "$work/port/$f" "$(pin "$frame" inputs PORT "$f")" port
done
crlf "$private/$stage-port/main.cpp" "$work/port/main.cpp"
expect "$work/port/main.cpp" "$(pin "$inputs/slice191-gs-stream-runtime.json" inputs PORT main.cpp)" "port (slice191 main)"
keep "$work/port" slice200-port

# Shaders: the nine SPIR-V files of the frame-cost manifest, as built for Linux.
mkdir "$work/shaders"
for f in $(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["inputs"]["SHADERS"]))' "$frame"); do
    cp "$private/linux074-shaders/$f" "$work/shaders/$f"
    expect "$work/shaders/$f" "$(pin "$frame" inputs SHADERS "$f")" shaders
done
keep "$work/shaders" slice153-pacing-shaders

# Runtime capsule: the Linux frame-cost capsule; its receipt is pinned as CRLF.
mkdir "$work/runtime"
for f in dmac.cpp gif.cpp hw.cpp memory.cpp system.cpp vif.cpp vu_micro.cpp; do
    cp "$private/linux074-frame-cost-runtime/$f" "$work/runtime/$f"
    expect "$work/runtime/$f" "$(pin "$frame" source_sha256 "$f")" runtime
done
crlf "$private/linux074-frame-cost-runtime/runtime.json" "$work/runtime/runtime.json"
expect "$work/runtime/runtime.json" "$(pin "$frame" receipt_sha256)" "runtime receipt"
keep "$work/runtime" slice262-frame-cost-runtime

# License guard with the frame skip hook (docs/linux/L2-FRAME-SKIP.md); configure.cmd uses it for both games.
guard=$inputs/slice376-frame-render-license-guard-runtime.json
mkdir "$work/guard"
for f in cdvdfsv.cpp dispatch.cpp kernel.cpp; do
    cp "$private/linux074-frameskip-guard-runtime/$f" "$work/guard/$f"
    expect "$work/guard/$f" "$(pin "$guard" source_sha256 "$f")" guard
done
cp "$private/linux074-frameskip-guard-runtime/runtime.json" "$work/guard/runtime.json"
if [[ $(sha "$work/guard/runtime.json") != "$(pin "$guard" receipt_sha256)" ]]; then
    crlf "$private/linux074-frameskip-guard-runtime/runtime.json" "$work/guard/runtime.json"
    expect "$work/guard/runtime.json" "$(pin "$guard" receipt_sha256)" "guard receipt"
fi
keep "$work/guard" slice376-frame-render-license-guard-runtime

# Memory card, IPU and SPU2 capsules: derived fresh, checked against the slice408 pins.
"$python" "$root/scripts/prepare_memcard_flush.py" "$private/$stage-memcard" > /dev/null
mv "$private/$stage-memcard" "$work/memcard"
expect "$work/memcard/sio2.cpp" "$(pin "$release" memcard_capsule_sha256 sio2.cpp)" memcard
keep "$work/memcard" slice200-memcard-runtime
"$python" "$root/scripts/prepare_ipu_fast.py" "$private/$stage-ipu" > /dev/null
mv "$private/$stage-ipu" "$work/ipu"
expect "$work/ipu/runtime.json" "$(pin "$release" ipu_capsule_sha256)" ipu
keep "$work/ipu" slice365-ipu-fast-runtime
"$python" "$root/scripts/prepare_spu2_runtime.py" "$private/$stage-spu2" > /dev/null
mv "$private/$stage-spu2" "$work/spu2"
expect "$work/spu2/runtime.json" "$(pin "$release" spu2_capsule_sha256)" spu2
keep "$work/spu2" slice396-spu2-runtime

# USB capsule (racing wheels), as LinuxHost.cmake checks it.
cp -r "$private/linux074-usb-runtime" "$work/usb"
for index in 0 1; do
    name=$(basename "$(pin "$inputs/slice409-usb-runtime.json" files $index path)")
    expect "$work/usb/$name" "$(pin "$inputs/slice409-usb-runtime.json" files $index sha256)" usb
done
keep "$work/usb" slice409-usb-runtime

# VU tables and the VU0 image.
cp -r "$private/linux074-vu1-gen" "$work/vu1"
expect "$work/vu1/vu1_generated.cpp" "$(pin "$release" vu1_aot generated_source_sha256)" vu1
expect "$work/vu1/manifest.json" "$(pin "$release" vu1_aot generated_manifest_sha256)" vu1
keep "$work/vu1" slice397-vu1-gen
cp -r "$private/linux074-vu0-gen" "$work/vu0"
expect "$work/vu0/vu0_generated.cpp" "$(pin "$release" vu0_aot table_sha256)" vu0
expect "$work/vu0/manifest.json" "$(pin "$release" vu0_aot manifest_sha256)" vu0
keep "$work/vu0" slice394-v200-vu0-gen
mkdir "$work/vu0image"
cp "$private/linux074-vu0-image/vu0_image.bin" "$work/vu0image/"
expect "$work/vu0image/vu0_image.bin" "$(pin "$release" vu0_aot image_sha256)" "vu0 image"
keep "$work/vu0image" slice160-vu0-image

echo "all Windows inputs equal their pins (port main.cpp: the slice191 one)"
