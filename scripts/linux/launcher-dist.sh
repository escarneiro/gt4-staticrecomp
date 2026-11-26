#!/usr/bin/env bash
# TASK-074 L4: a Linux launcher folder, the Linux counterpart of the Windows portable zip (launcher/tools/build-release.ps1
# -Portable and scripts/make_distribution.py). Layout (docs/linux/L4-LAUNCHER.md):
#
#   <folder>/GT4Recomp              the launcher (portable build: user data in <folder>/user, large data in <folder>/data)
#   <folder>/game/gt4               GT4 v2.00   (gt4_linux_window of GT4_LINUX_GT4_BUILD)
#   <folder>/game/gt4-spec2         Spec II     (gt4_linux_window of GT4_LINUX_SPEC2_BUILD), when that build exists
#   <folder>/game/shaders/*.spv     the nine compute shaders of CoreGS 1.1, from the v2.00 build's GT4_LINUX_SHADER_DIR
#
# Usage: scripts/linux/launcher-dist.sh
#
# Environment (each optional):
#   GT4_LINUX_LAUNCHER_DIST  the folder (default private/linux074-launcher-dist)
#   GT4_LINUX_GT4_BUILD      v2.00 game build (default private/linux074-pgo-build, else private/linux074-frameskip-build)
#   GT4_LINUX_SPEC2_BUILD    Spec II game build (default private/linux074-spec2-pgo-build, else private/linux074-spec2-build)
#   CARGO_TARGET_DIR         the launcher's cargo build folder (default private/linux074-launcher-target)
#
# Running it again on an existing folder replaces only the program files (GT4Recomp and game/); user/ and data/ (settings,
# saves, prepared game files) are never touched. The game programs are built from translated game code: the folder stays
# in private/ and is never committed or published.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
dist=${GT4_LINUX_LAUNCHER_DIST:-$root/private/linux074-launcher-dist}
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-$root/private/linux074-launcher-target}

first_build() {  # <candidate folder>...: the first that holds gt4_linux_window
    for candidate in "$@"; do
        if [[ -x $candidate/gt4_linux_window ]]; then echo "$candidate"; return; fi
    done
}
gt4_build=${GT4_LINUX_GT4_BUILD:-$(first_build "$root/private/linux074-pgo-build" "$root/private/linux074-frameskip-build")}
spec2_build=${GT4_LINUX_SPEC2_BUILD:-$(first_build "$root/private/linux074-spec2-pgo-build" "$root/private/linux074-spec2-build")}
[[ -n $gt4_build && -x $gt4_build/gt4_linux_window ]] || { echo "no GT4 v2.00 game build (set GT4_LINUX_GT4_BUILD)" >&2; exit 2; }
shaders=$(sed -n 's/^GT4_LINUX_SHADER_DIR:PATH=//p' "$gt4_build/CMakeCache.txt")
[[ -n $shaders && -f $shaders/raster.spv ]] || { echo "the v2.00 build names no shader folder (GT4_LINUX_SHADER_DIR)" >&2; exit 2; }

# The launcher: the portable build kind, with Tauri's custom protocol (the pages come from the bundled ui/ folder, as
# `cargo tauri build` does).
(cd "$root/launcher" && cargo build --release -p gt4l-tauri --features portable,tauri/custom-protocol)

mkdir -p "$dist"
install -m 755 "$CARGO_TARGET_DIR/release/GT4Recomp" "$dist/GT4Recomp.new"
mv -f "$dist/GT4Recomp.new" "$dist/GT4Recomp"
rm -rf "$dist/game.new"
mkdir -p "$dist/game.new/shaders"
install -m 755 "$gt4_build/gt4_linux_window" "$dist/game.new/gt4"
if [[ -n $spec2_build && -x $spec2_build/gt4_linux_window ]]; then
    install -m 755 "$spec2_build/gt4_linux_window" "$dist/game.new/gt4-spec2"
fi
install -m 644 "$shaders"/*.spv "$dist/game.new/shaders/"
rm -rf "$dist/game"
mv "$dist/game.new" "$dist/game"

echo "launcher folder: $dist"
echo "  v2.00 from $gt4_build"
[[ -x $dist/game/gt4-spec2 ]] && echo "  Spec II from $spec2_build" || echo "  Spec II: no build found"
( cd "$dist" && sha256sum GT4Recomp game/gt4 game/gt4-spec2 2>/dev/null || true )
echo "start it with: $dist/GT4Recomp"
