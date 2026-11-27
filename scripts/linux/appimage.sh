#!/usr/bin/env bash
# TASK-074 L4: the Linux AppImage of the launcher and both games, the Linux counterpart of the Windows installer.
#
#   GT4Recomp-<version>-x86_64.AppImage
#     usr/bin/GT4Recomp            the launcher (Cargo feature `appimage`: settings, saves and prepared game files in
#                                  $XDG_DATA_HOME/GT4Recomp, else ~/.local/share/GT4Recomp; no self-update)
#     usr/bin/game/gt4             GT4 v2.00   (gt4_linux_window of GT4_LINUX_GT4_BUILD)
#     usr/bin/game/gt4-spec2       Spec II     (gt4_linux_window of GT4_LINUX_SPEC2_BUILD), when that build exists
#     usr/bin/game/shaders/*.spv   the compute shaders of CoreGS 1.1, from the v2.00 build's GT4_LINUX_SHADER_DIR
#     usr/bin/game/lib/            libraries of the game a system may lack (SDL3), named for the game process only
#     usr/lib/...                  WebKitGTK, GTK, SDL3 and the other libraries both programs need (linuxdeploy)
#
# The Vulkan loader, the GPU drivers, the sound server and the C library come from the system. The programs are built
# on this machine, so the AppImage runs on distributions whose glibc is as new as this one (see docs/linux/L4-APPIMAGE.md).
#
# Usage: scripts/linux/appimage.sh
#
# Environment (each optional):
#   GT4_LINUX_APPIMAGE_OUT   output folder (default private/linux074-appimage)
#   GT4_LINUX_GT4_BUILD      v2.00 game build (default private/linux074-pgo-build, else private/linux074-frameskip-build)
#   GT4_LINUX_SPEC2_BUILD    Spec II game build (default private/linux074-spec2-pgo-build, else private/linux074-spec2-build)
#   CARGO_TARGET_DIR         the launcher's cargo build folder (default private/linux074-appimage-target)
#   GT4_RELEASE_LABEL        release label, rc<N> (for example rc15): the launcher reports <version>-rc<N> and the file is
#                            GT4Recomp-<version>-rc<N>-x86_64.AppImage (as build-release.ps1 -Label on Windows); empty = final
#   GT4_APPIMAGE_RUNTIME     the AppImage type 2 runtime (default private/tooling/appimage/runtime-x86_64; downloaded when missing)
#   GT4_TAURI_CLI            cargo-tauri 2.x (default private/tooling/tauri-cli/bin/cargo-tauri; installed there when missing)
#
# The bundler downloads linuxdeploy and its AppImage plugin from GitHub on the first run (kept in ~/.cache/tauri).
# The game programs are built from translated game code: the AppImage stays in private/ until the owner publishes it.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
out=${GT4_LINUX_APPIMAGE_OUT:-$root/private/linux074-appimage}
stage=$out/stage
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-$root/private/linux074-appimage-target}
tauri=${GT4_TAURI_CLI:-$root/private/tooling/tauri-cli/bin/cargo-tauri}

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

# WebKitGTK's helper programs. The bundler copies them only from <libdir>/webkit2gtk-4.1 (Debian, Fedora, Arch);
# openSUSE keeps them in /usr/libexec/libwebkit2gtk-4_1-0. The library names the folder it was built with, and the
# bundler rewrites that name to a path inside the AppImage, so the folder is copied to the same place in it.
webkit_helpers=""
for candidate in /usr/libexec/libwebkit2gtk-4_1-0 /usr/libexec/webkit2gtk-4.1 /usr/lib64/webkit2gtk-4.1 /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1 /usr/lib/webkit2gtk-4.1; do
    if [[ -x $candidate/WebKitWebProcess ]]; then webkit_helpers=$candidate; break; fi
done
[[ -n $webkit_helpers ]] || { echo "WebKitWebProcess not found (install WebKitGTK 4.1)" >&2; exit 2; }

if [[ ! -x $tauri ]]; then
    echo "installing the Tauri CLI 2.x into $(dirname "$(dirname "$tauri")")"
    cargo install tauri-cli --version "^2" --locked --root "$(dirname "$(dirname "$tauri")")"
fi

# The AppImage runtime (the small program at the front of the file). appimagetool downloads it from GitHub at every
# build unless it is given one, and that download can stall without an error; it is fetched once here, with a time
# limit, and handed to the bundler (LDAI_RUNTIME_FILE).
runtime=${GT4_APPIMAGE_RUNTIME:-$root/private/tooling/appimage/runtime-x86_64}
if [[ ! -s $runtime ]]; then
    mkdir -p "$(dirname "$runtime")"
    echo "downloading the AppImage runtime to $runtime"
    curl -fL --connect-timeout 20 --max-time 300 --retry 3 -o "$runtime.part" \
        https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
    mv "$runtime.part" "$runtime"
fi
[[ $(head -c 4 "$runtime" | od -An -c | tr -d ' ') == 177ELF ]] || { echo "$runtime is not an ELF program" >&2; exit 2; }
export LDAI_RUNTIME_FILE=$runtime

# The game folder, as it sits beside the launcher in the portable folder (scripts/linux/launcher-dist.sh).
rm -rf "$stage"
mkdir -p "$stage/game/shaders"
install -m 755 "$gt4_build/gt4_linux_window" "$stage/game/gt4"
if [[ -n $spec2_build && -x $spec2_build/gt4_linux_window ]]; then
    install -m 755 "$spec2_build/gt4_linux_window" "$stage/game/gt4-spec2"
fi
install -m 644 "$shaders"/*.spv "$stage/game/shaders/"
# The libraries of the game that a system may lack go to game/lib; the launcher names that folder for the game only
# (gt4l_core::launch::game_library_path). SDL3 is the one so far; everything else the game links is part of every desktop.
mkdir -p "$stage/game/lib"
sdl3=$(ldd "$stage/game/gt4" | awk '$1 == "libSDL3.so.0" { print $3 }')
[[ -n $sdl3 && -f $sdl3 ]] || { echo "the game does not name libSDL3.so.0 (ldd)" >&2; exit 2; }
install -m 644 "$(readlink -f "$sdl3")" "$stage/game/lib/libSDL3.so.0"

# The bundle settings for this build only (tauri.conf.json keeps "active": false for the Windows builds).
version=$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' "$root/launcher/crates/gt4l-tauri/tauri.conf.json" | head -n1)
label=${GT4_RELEASE_LABEL:-}
[[ -z $label || $label =~ ^rc[1-9][0-9]*$ ]] || { echo "GT4_RELEASE_LABEL must be empty or rc<N> (for example rc15), got '$label'" >&2; exit 2; }
export GT4_RELEASE_LABEL=$label
release_version=$version${label:+-$label}
cat > "$stage/tauri.appimage.json" <<EOF
{
  "bundle": {
    "active": true,
    "targets": ["appimage"],
    "icon": ["../../assets/brand/launcher.png"],
    "category": "Game",
    "shortDescription": "Gran Turismo 4, recompiled",
    "linux": {
      "appimage": {
        "bundleMediaFramework": false,
        "files": {
          "/usr/bin/game": "$stage/game",
          "$webkit_helpers": "$webkit_helpers"
        }
      }
    }
  }
}
EOF

# NO_STRIP: linuxdeploy's own strip is older than this system's binaries (it refuses their .relr.dyn sections).
(cd "$root/launcher/crates/gt4l-tauri" && NO_STRIP=true "$tauri" build --bundles appimage --features appimage --config "$stage/tauri.appimage.json")

built=$(ls -t "$CARGO_TARGET_DIR"/release/bundle/appimage/*.AppImage | head -n1)
mkdir -p "$out"
target=$out/GT4Recomp-$release_version-x86_64.AppImage
cp "$built" "$target"
chmod 755 "$target"
echo "  v2.00 from $gt4_build"
[[ -x $stage/game/gt4-spec2 ]] && echo "  Spec II from $spec2_build"
echo "  WebKit helpers from $webkit_helpers"
(cd "$out" && sha256sum "$(basename "$target")" | tee "$(basename "$target").sha256")
echo "start it with: $target"
