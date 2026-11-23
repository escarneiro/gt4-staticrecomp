#!/usr/bin/env bash
# TASK-074 L2 performance: profile-guided optimisation (PGO) and ThinLTO of the whole Linux game, the Linux counterpart
# of scripts/coregs2_bench/pgo-all.sh (cmake/LinuxHost.cmake: GT4_LINUX_PGO, GT4_LINUX_THINLTO). Three steps:
#
#   scripts/linux/pgo-build.sh instrument            build the instrumented game (private/linux074-pgo-gen)
#   scripts/linux/pgo-build.sh train <disc> <card>   play it through play.sh; close the window normally to write the
#                                                    profile (Ctrl-C or a crash writes nothing). Run it more than once
#                                                    to cover more scenes: every session adds a profile.
#   scripts/linux/pgo-build.sh optimize              merge the profiles, build with PGO and ThinLTO
#                                                    (private/linux074-pgo-build); play it with
#                                                    GT4_LINUX_BUILD=private/linux074-pgo-build scripts/linux/play.sh ...
#
# GT4_LINUX_GAME=spec2 does the same for Spec II (docs/linux/L2-SPEC2.md): private/linux074-spec2-build as the plain build,
# private/linux074-spec2-pgo-gen, linux074-spec2-pgo and linux074-spec2-pgo-build.
#
# Every GT4_* setting is copied from the plain release build (GT4_LINUX_BASE_BUILD, default
# private/linux074-release-build, else private/linux074-pgo-build), so the PGO build gets the same units, capsules and pins. The profile holds only
# function hashes and counters, but of code translated from the game: it stays in private/.
# Needs llvm-profdata, llvm-ar and llvm-ranlib of the clang in use (openSUSE: zypper install llvm).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
case ${GT4_LINUX_GAME:-gt4} in
    gt4)   prefix=linux074; plain=linux074-release-build ;;
    spec2) prefix=linux074-spec2; plain=linux074-spec2-build ;;
    *)     echo "GT4_LINUX_GAME must be gt4 or spec2" >&2; exit 2 ;;
esac
gen=$root/private/$prefix-pgo-gen
use=$root/private/$prefix-pgo-build
# The settings come from the plain release build, else from the PGO build itself (it carries the same GT4_* entries).
base=${GT4_LINUX_BASE_BUILD:-$root/private/$plain}
if [[ -z ${GT4_LINUX_BASE_BUILD:-} && ! -f $base/CMakeCache.txt && -f $use/CMakeCache.txt ]]; then base=$use; fi
profiles=$root/private/$prefix-pgo
step=${1:-}

base_arguments() {  # the plain build's compiler and GT4_* cache entries as -D arguments, one per line
    [[ -f $base/CMakeCache.txt ]] || { echo "no configured release build in $base" >&2; exit 2; }
    grep -E '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_C_COMPILER|GT4_[A-Z0-9_]+):[A-Z]+=' "$base/CMakeCache.txt" \
        | grep -vE '^GT4_(LINUX_PGO|LINUX_PGO_PROFILE|LINUX_THINLTO|LLVM_AR|LLVM_RANLIB):' | sed 's/^/-D/'
}

tool() {  # <name>: the LLVM tool next to the build's clang, else on PATH
    local clang dir major
    clang=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$base/CMakeCache.txt")
    dir=$(dirname "$(readlink -f "$clang")")
    major=$("$clang" --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
    for candidate in "$dir/$1" "$1" "$1-$major"; do
        if command -v "$candidate" > /dev/null; then command -v "$candidate"; return; fi
    done
    echo "$1 not found (openSUSE: zypper install llvm)" >&2
    exit 2
}

configure_and_build() {  # <build dir> <extra -D arguments...>
    local dir=$1; shift
    mapfile -t arguments < <(base_arguments)
    cmake -S "$root" -B "$dir" -G Ninja "${arguments[@]}" "$@"
    cmake --build "$dir" --target gt4_linux_window
}

case $step in
instrument)
    configure_and_build "$gen" -DGT4_LINUX_PGO=generate -DGT4_LINUX_THINLTO=OFF
    echo "instrumented game: $gen/gt4_linux_window"
    ;;
train)
    [[ $# -ge 3 ]] || { echo "usage: $0 train <disc image> <memory card> [game arguments...]" >&2; exit 2; }
    [[ -x $gen/gt4_linux_window ]] || { echo "run '$0 instrument' first" >&2; exit 2; }
    mkdir -p "$profiles/raw"
    shift
    # %p: one file per process; %m: per binary signature, so a rebuilt instrumented game never merges into old files.
    GT4_LINUX_BUILD=$gen LLVM_PROFILE_FILE="$profiles/raw/play-%p-%m.profraw" "$root/scripts/linux/play.sh" "$@"
    ls -l "$profiles"/raw/*.profraw
    ;;
optimize)
    shopt -s nullglob
    raw=("$profiles"/raw/*.profraw)
    [[ ${#raw[@]} -gt 0 ]] || { echo "no profiles in $profiles/raw: run '$0 train' and close the window normally" >&2; exit 2; }
    "$(tool llvm-profdata)" merge -o "$profiles/game.profdata" "${raw[@]}"
    # Objects from an earlier profile must not survive into the ThinLTO link: start from an empty folder (keeping its
    # settings when they are the base).
    if [[ $base == "$use" ]]; then
        mkdir -p "$profiles"
        cp "$use/CMakeCache.txt" "$profiles/base-CMakeCache.txt"
        base=$profiles/base-settings
        mkdir -p "$base"
        cp "$profiles/base-CMakeCache.txt" "$base/CMakeCache.txt"
    fi
    rm -rf "$use"
    configure_and_build "$use" -DGT4_LINUX_PGO=use "-DGT4_LINUX_PGO_PROFILE=$profiles/game.profdata" -DGT4_LINUX_THINLTO=ON
    ( cd "$use" && sha256sum gt4_linux_window "$profiles/game.profdata" | tee receipt.txt )
    echo "play it with: GT4_LINUX_GAME=${GT4_LINUX_GAME:-gt4} scripts/linux/play.sh <disc> <card> (uses $use)"
    ;;
*)
    echo "usage: $0 instrument | train <disc> <card> [game arguments...] | optimize" >&2
    exit 2
    ;;
esac
