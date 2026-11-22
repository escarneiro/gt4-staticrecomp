#!/usr/bin/env bash
# TASK-074 + TASK-100: derive every Spec II input of the Linux build from the player's Spec II disc, each checked against
# the Windows release pins (docs/linux/L2-SPEC2.md). Nothing is written over: every output folder must be new.
#
# Usage: scripts/linux/spec2-inputs.sh <Spec II 1.10 disc image> [prefix]
#   prefix (default linux074-spec2) names the folders under private/:
#     <prefix>-elf             derived SCUS_974.36 (spec2_elf_patch.py)                    pin docs/inputs/spec2-elf.json
#     <prefix>-ee-producer     entry-seeds.txt and generated/ (the S8c translation)         pin spec2-ee-generated.json
#     <prefix>-aspect          the TASK-166 patched ELF and its translation                 pin slice408-spec2.json
#     <prefix>-release-units   camera360_units.py spec2 output (GT4_LINUX_GAME_UNITS)       pin slice408-spec2.json
#     <prefix>-vu0-image, -vu0-gen   the Spec II VU0 image and table                         pin slice408-spec2.json
#     <prefix>-iop-runtime     prepare_perf_runtime.py ... --spec2-iop (slice276 profile)  pin slice276-spec2-iop-runtime.json
#     <prefix>-guard-runtime   prepare_license_guard.py --display-aspect --frame-counters  pin slice408-spec2.json
#
# Needs: private/tooling-venv (scripts/requirements.txt), the Linux translator private/linux074-ee-producer/tool-build/eerecomp
# (tools/ee_producer/build_linux.sh) and gt4_vu_recomp from a configured Linux build (GT4_LINUX_VU_RECOMP, default
# private/linux074-pgo-build/gt4_vu_recomp). The VU1 table, port, shaders and USB capsule are the v2.00 ones (same pins).
set -euo pipefail

[[ $# -ge 1 ]] || { echo "usage: $0 <Spec II disc image> [prefix]" >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
disc=$1
prefix=${2:-linux074-spec2}
py=private/tooling-venv/bin/python
translator=private/linux074-ee-producer/tool-build/eerecomp
vu_recomp=${GT4_LINUX_VU_RECOMP:-private/linux074-pgo-build/gt4_vu_recomp}
out=private/$prefix
for tool in "$py" "$translator" "$vu_recomp"; do [[ -x $tool ]] || { echo "missing $tool" >&2; exit 2; }; done
for folder in elf ee-producer aspect release-units vu0-image vu0-gen iop-runtime guard-runtime; do
    [[ ! -e $out-$folder ]] || { echo "$out-$folder exists; choose a new prefix" >&2; exit 2; }
done
pin() { $py -I -c "import json,sys; d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split('/'): d=d[k]
print(d)" "docs/inputs/$1" "$2"; }
same() {  # <what> <actual> <pinned>
    if [[ $2 != "$3" ]]; then echo "FAILED $1: $2, pin $3" >&2; exit 1; fi
    echo "ok  $1 ${2:0:16}"
}

# 1. The derived ELF: the plugin injector's 35 stores applied, the data segment split around the plugin.
$py -I scripts/spec2_elf_patch.py --iso "$disc" --output "$out-elf" > /dev/null
elf=$out-elf/SCUS_974.36
same "derived ELF" "$(sha256sum "$elf" | cut -c1-64)" "$(pin spec2-elf.json derived/sha256)"

# 2. The S8c entry seeds. Windows took them as the delta of scripts/spec2_entry_seeds.py against the S7b translation, which
# had seeded the plugin's two literal code pointers (0x0068C6E8, 0x0068C7C4: the plugin tables the dataword scan cannot see).
# That translation is rebuilt here (1,285,650 instructions, 17,080 functions, 148,756 entries, as recorded for S7b).
mkdir -p "$out-ee-producer/s7b"
printf '2\n0x0068C6E8\n0x0068C7C4\n' > "$out-ee-producer/s7b/plugin-seeds.txt"
"$translator" "$elf" --out "$out-ee-producer/s7b/generated" --entries "$out-ee-producer/s7b/plugin-seeds.txt" \
    > "$out-ee-producer/s7b/generation.log"
$py scripts/spec2_entry_seeds.py --elf "$elf" --generated "$out-ee-producer/s7b/generated" \
    --output "$out-ee-producer/entry-seeds.txt" --json "$out-ee-producer/entry-seeds.json" > /dev/null
same "entry seeds" "$(sha256sum "$out-ee-producer/entry-seeds.txt" | cut -c1-64)" "$(pin spec2-ee-generated.json entry_seeds/sha256)"
rm -rf "$out-ee-producer/s7b/generated"

# 3. The Spec II translation (the Linux translator; the check is the output inventory, as for v2.00 in L1).
"$translator" "$elf" --out "$out-ee-producer/generated" --entries "$out-ee-producer/entry-seeds.txt" > "$out-ee-producer/generation.log"
inventory=$($py -I -c "import sys; sys.path.insert(0,'scripts'); import spec2_aspect_patch as a
print(a.inventory_digest(a.inventory(sys.argv[1])))" "$out-ee-producer/generated")
same "translation" "$inventory" "$(pin spec2-ee-generated.json inventory_sha256)"

# 4. TASK-166: the two race HUD instructions patched (spec2_aspect_patch.py's table and checks, with the Linux translator).
$py -I - "$elf" "$out-ee-producer/generated" "$out-aspect" "$translator" "$out-ee-producer/entry-seeds.txt" <<'EOF'
import subprocess, sys
from pathlib import Path
sys.path.insert(0, "scripts")
import spec2_aspect_patch as a
elf, base, out, translator, seeds = map(Path, sys.argv[1:])
out.mkdir()
patched = a.patch_elf(elf.read_bytes())
(out / "SCUS_974.36.aspect.elf").write_bytes(patched)
result = subprocess.run([str(translator), str(out / "SCUS_974.36.aspect.elf"), "--out", str(out / "generated"),
                         "--entries", str(seeds)], capture_output=True, text=True, check=True)
(out / "generation-aspect.log").write_text(result.stdout + result.stderr)
base_records = {row["name"]: row for row in a.inventory(base)}
changed = sorted(row["name"] for row in a.inventory(out / "generated") if row != base_records[row["name"]])
expected = sorted({a.unit_holding(out / "generated", patch.address) for patch in a.PATCHES})
if changed != expected:
    raise SystemExit(f"changed units {changed}, expected {expected}")
print(a.sha256_bytes(patched))
EOF
same "aspect ELF" "$(sha256sum "$out-aspect/SCUS_974.36.aspect.elf" | cut -c1-64)" "$(pin slice408-spec2.json spec2_aspect_units/patched_elf_sha256)"

# 5. The release hook sites (camera, FoV, trigger range, overlay canvas): the units the build compiles.
$py -I scripts/camera360_units.py spec2 "$out-aspect/generated" "$out-release-units" > /dev/null
units=$($py -I -c "import json,sys; print(json.load(open(sys.argv[1]))['output_inventory_sha256'])" "$out-release-units.camera360-units.json")
same "release units" "$units" "$(pin slice408-spec2.json camera360_units/output_inventory_sha256)"

# 6. VU0: the Spec II upload (docs/inputs/spec2-vu.json) padded to 4 KiB, the 14 VCALLMS/CMSAR0 entries, the emitter.
mkdir -p "$out-vu0-image"
$py -I - "$elf" "$out-vu0-image" <<'EOF'
import json, sys
from pathlib import Path
elf, folder = Path(sys.argv[1]), Path(sys.argv[2])
vu0 = json.load(open("docs/inputs/spec2-vu.json"))["vu0"]
upload = elf.read_bytes()[vu0["file_offset"]:vu0["file_offset"] + vu0["upload_bytes"]]
(folder / "vu0_image.bin").write_bytes(upload + bytes(4096 - len(upload)))
(folder / "entries.txt").write_text("\n".join("0058 0078 00B8 00F8 0258 0268 0490 05C0 0648 07A0 07B0 0848 0850 08B0".split()) + "\n")
EOF
same "VU0 image" "$(sha256sum "$out-vu0-image/vu0_image.bin" | cut -c1-64)" "$(pin slice408-spec2.json vu0_aot/image_sha256)"
"$vu_recomp" --unit 0 --namespace gt4::vugen::vu0 --entry-policy vcallms --image "$out-vu0-image/vu0_image.bin" \
    --entries "$out-vu0-image/entries.txt" "$out-vu0-gen" > /dev/null
same "VU0 table" "$(sha256sum "$out-vu0-gen/vu0_generated.cpp" | cut -c1-64)" "$(pin slice408-spec2.json vu0_aot/table_sha256)"

# 7. The runtime capsule (slice276: frame cost plus the DNAS310 IOP reboot rule) and the license guard capsule.
$py scripts/prepare_perf_runtime.py "$out-iop-runtime" --assets-root "$root" --mtvu --gs-stream --frame-cost --vu0-aot --spec2-iop > /dev/null
for source in system.cpp vif.cpp vu_micro.cpp gif.cpp memory.cpp hw.cpp dmac.cpp sif.cpp; do
    same "capsule $source" "$(sha256sum "$out-iop-runtime/$source" | cut -c1-64)" "$(pin slice276-spec2-iop-runtime.json source_sha256/$source)"
done
$py scripts/prepare_license_guard.py "$out-guard-runtime" --assets-root "$root" --display-aspect --frame-counters > /dev/null
for source in cdvdfsv.cpp dispatch.cpp kernel.cpp; do
    same "guard $source" "$(sha256sum "$out-guard-runtime/$source" | cut -c1-64)" "$(pin slice408-spec2.json license_guard_source_sha256/$source)"
done
echo "every Spec II input equals its pin; configure with docs/linux/L2-SPEC2.md"
