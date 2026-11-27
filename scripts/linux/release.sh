#!/usr/bin/env bash
# TASK-074 L4: builds the Linux AppImage of a release and uploads it to a GitHub release as a draft.
#
# Usage: scripts/linux/release.sh <tag>        for example: scripts/linux/release.sh v1.3.0-rc15
#
# What it does, in order (it stops at the first problem):
#   1. checks the tag: vX.Y.Z (final) or vX.Y.Z-rcN; X.Y.Z must be the launcher version (tauri.conf.json), and the GitHub
#      release text must exist: docs/release-notes/linux/<X.Y.Z[-rcN]>.md;
#   2. checks the working tree has no uncommitted change to tracked files (the release is built from a commit);
#   3. tags HEAD (annotated) when the tag does not exist; an existing tag must point at HEAD;
#   4. builds the AppImage with that label (scripts/linux/appimage.sh, GT4_RELEASE_LABEL=rcN);
#   5. pushes the tag to the remote and uploads the AppImage and SHA256SUMS to a **draft** release of that tag. A draft is
#      private to the repository's owner. Running it again for the same tag replaces the draft's assets. A release that is
#      already published is never changed.
#
# It never publishes. To publish a checked draft: an rc as a pre-release, never Latest
#   gh release edit <tag> --repo <owner/repo> --draft=false --prerelease --latest=false
# and a final release as Latest
#   gh release edit <tag> --repo <owner/repo> --draft=false --latest
#
# Environment (each optional):
#   GT4_RELEASE_REMOTE   the git remote of the repository the release goes to (default: fork)
#   GT4_RELEASE_REPO     owner/repo on GitHub (default: read from the remote's URL)
#   GT4_RELEASE_NO_BUILD=1  upload the AppImage already in private/linux074-appimage instead of building it again
#   and every variable of scripts/linux/appimage.sh (game builds, output folder).
#
# The AppImage contains game programs built from translated game code (the Windows installer ships the same kind of
# program; decision 0069): whoever runs this publishes them under their own GitHub account.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
die() { echo "release: $*" >&2; exit 2; }

tag=${1:-}
[[ $tag =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-(rc[1-9][0-9]*))?$ ]] || die "usage: $0 vX.Y.Z[-rcN] (got '${tag}')"
version=${BASH_REMATCH[1]}
label=${BASH_REMATCH[3]:-}
release_version=$version${label:+-$label}

launcher_version=$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' launcher/crates/gt4l-tauri/tauri.conf.json | head -n1)
[[ $version == "$launcher_version" ]] || die "the tag says $version but the launcher is $launcher_version (tauri.conf.json)"
notes=docs/release-notes/linux/$release_version.md
[[ -s $notes ]] || die "write the release text first: $notes"

remote=${GT4_RELEASE_REMOTE:-fork}
url=$(git remote get-url "$remote") || die "no git remote '$remote' (set GT4_RELEASE_REMOTE)"
repo=${GT4_RELEASE_REPO:-$(sed -E 's#^(https://github\.com/|git@github\.com:)##; s#\.git$##' <<<"$url")}
[[ $repo =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "cannot read owner/repo from $url (set GT4_RELEASE_REPO)"

[[ -z $(git status --porcelain --untracked-files=no) ]] || die "commit or stash the changes to tracked files first"
head=$(git rev-parse HEAD)
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    [[ $(git rev-parse "$tag^{commit}") == "$head" ]] || die "the tag $tag exists here and points at $(git rev-parse --short "$tag^{commit}"), not HEAD $(git rev-parse --short HEAD). If it is a copy of another repository's tag, remove the local copy (git tag -d $tag) and run this again."
else
    git tag -a "$tag" -m "GT4Recomp $release_version (Linux AppImage)"
    echo "tagged $(git rev-parse --short HEAD) as $tag"
fi

# A published release is never touched; a draft gets its assets replaced.
state=$(gh release view "$tag" --repo "$repo" --json isDraft -q .isDraft 2>/dev/null || echo none)
[[ $state != false ]] || die "$tag is already published on $repo; it is not changed (make a new rc)"

out=${GT4_LINUX_APPIMAGE_OUT:-$root/private/linux074-appimage}
appimage=$out/GT4Recomp-$release_version-x86_64.AppImage
if [[ ${GT4_RELEASE_NO_BUILD:-} != 1 ]]; then
    GT4_RELEASE_LABEL=$label GT4_LINUX_APPIMAGE_OUT=$out scripts/linux/appimage.sh
fi
[[ -x $appimage ]] || die "no AppImage at $appimage"
(cd "$out" && sha256sum "$(basename "$appimage")" > SHA256SUMS)

git push "$remote" "refs/tags/$tag"
if [[ $state == none ]]; then
    prerelease=()
    [[ -n $label ]] && prerelease=(--prerelease)
    gh release create "$tag" --repo "$repo" --draft "${prerelease[@]}" --verify-tag \
        --title "GT4Recomp $release_version (Linux AppImage)" --notes-file "$notes" \
        "$appimage" "$out/SHA256SUMS"
else
    gh release edit "$tag" --repo "$repo" --notes-file "$notes"
    gh release upload "$tag" --repo "$repo" --clobber "$appimage" "$out/SHA256SUMS"
fi
echo
echo "draft release $tag on $repo: $(gh release view "$tag" --repo "$repo" --json url -q .url)"
cat "$out/SHA256SUMS"
echo "publish it (after testing the download) with:"
if [[ -n $label ]]; then
    echo "  gh release edit $tag --repo $repo --draft=false --prerelease --latest=false"
else
    echo "  gh release edit $tag --repo $repo --draft=false --latest"
fi
