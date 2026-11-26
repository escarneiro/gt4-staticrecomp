# TASK-074 L4: the Linux AppImage

`scripts/linux/appimage.sh` builds `private/linux074-appimage/GT4Recomp-<version>-x86_64.AppImage`: the launcher and both
games in one file, the Linux counterpart of the Windows installer. It is a local test build for now; publishing it is the
owner's decision, as for the Windows packages (decision 0069).

## Contents

| Path in the image | What |
|---|---|
| `usr/bin/GT4Recomp` | the launcher, Cargo feature `appimage` |
| `usr/bin/game/gt4`, `gt4-spec2` | the game programs (the PGO builds when present, else the plain ones) |
| `usr/bin/game/shaders/*.spv` | the CoreGS 1.1 compute shaders of the v2.00 build |
| `usr/bin/game/lib/libSDL3.so.0` | the game's SDL3; the launcher names this folder in `LD_LIBRARY_PATH` for the game only |
| `usr/lib/...` | WebKitGTK, GTK and their libraries (linuxdeploy, from the build machine) |
| `usr/libexec/libwebkit2gtk-4_1-0/` | WebKitGTK's helper programs (openSUSE's folder; see below) |

The Vulkan loader, the GPU drivers, the sound server and glibc come from the system.

## The `appimage` launcher build

A portable build (`portable` feature) with two differences:

- **Folders:** `user/` and `data/` live in `$XDG_DATA_HOME/GT4Recomp` (an absolute `XDG_DATA_HOME`), else
  `~/.local/share/GT4Recomp` (`gt4l_core::paths::appimage_data_home`), because the program files are a read-only image
  mounted at a new place at each start. The single-instance scope is that folder too.
- **No self-update:** no update check at launch, and a launcher update from Settings says to download the new AppImage.
  The About page shows "AppImage".

## How the script works

1. Stages `game/` (programs, shaders, SDL3) in `private/linux074-appimage/stage`.
2. Writes a Tauri config override (`stage/tauri.appimage.json`): bundle active, target `appimage`, the PNG icon, and two
   extra file trees (`bundle.linux.appimage.files`): `game/` to `usr/bin/game`, and WebKitGTK's helper folder.
3. Runs `cargo tauri build --bundles appimage --features appimage` (Tauri CLI 2.x in `private/tooling/tauri-cli`,
   installed by the script when missing; cargo target `private/linux074-appimage-target`).

Notes from the first builds (2026-11-26, openSUSE Tumbleweed):

- **WebKitGTK helpers.** The bundler copies `WebKitWebProcess` and friends only from `<libdir>/webkit2gtk-4.1`;
  openSUSE keeps them in `/usr/libexec/libwebkit2gtk-4_1-0`. The bundler's GTK plugin rewrites `/usr` to `././` inside
  the WebKit library (paths relative to `usr/`, where AppRun starts), so the helper folder is copied to the same path
  in the image.
- **AppImage runtime.** appimagetool downloads its runtime from GitHub at every build; one download stalled for 40
  minutes without an error. The script downloads `runtime-x86_64` once (time limit, retries) into
  `private/tooling/appimage/` and passes it as `LDAI_RUNTIME_FILE`.
- **`NO_STRIP=true`:** linuxdeploy's bundled `strip` is older than this system's binaries.
- linuxdeploy and its AppImage plugin are downloaded once into `~/.cache/tauri`.

## Reach

Built on Tumbleweed, the programs need glibc 2.39 (launcher) and 2.43 (games: the `atan2f` symbol version), so the
AppImage runs on distributions at least that new. A build in an older container would widen this; it must then be
checked for identical frames, since an older glibc has another `atan2f`.

## Checks

- Start with a scratch folder: `XDG_DATA_HOME=/tmp/x <AppImage>`; the log (`/tmp/x/GT4Recomp/data/logs/latest.log`) says
  "(AppImage build) starting from /tmp/.mount_.../usr/bin", the folders under `/tmp/x/GT4Recomp`, "updates: not checked
  (AppImage build)" and "the launcher page drew its first screen".
- Size of the first build: about 157 MiB.
- Known: the launcher ignores SIGTERM (as the portable build); close it from its window.
