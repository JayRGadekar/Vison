#!/usr/bin/env sh
# Fetch a licence-clean ffmpeg into build/bin so packaging can bundle it.
#
# Vison ships without ffmpeg by default - the app finds a system copy and
# warns when there is none (see the ffmpeg notice in App.tsx). That is a
# reasonable default for someone building from source, but it means most end
# users - who do not have ffmpeg on PATH - get numbered PNG frames instead of
# an mp4 on every video generation. This script closes that gap for anyone
# packaging a release: it fetches the exact build
# scripts/check-ffmpeg-license.cjs already recommends in its own error
# message - BtbN's LGPL, libvpx-enabled Windows build - and drops it where
# package.json's extraResources picks it up automatically.
#
# Deliberately points at the *unversioned* "master-latest" asset name rather
# than a version-pinned one: BtbN prunes old per-version filenames when a new
# ffmpeg release ships, which would silently 404 a pinned URL later. This
# name is stable; only its contents rotate.
#
# check-ffmpeg-license.cjs re-verifies whatever ends up in build/bin at pack
# time regardless of how it got there, so this script cannot itself cause a
# GPL binary to ship - packaging still refuses that independently.
#
# Safe to re-run: leaves an existing build/bin/ffmpeg.exe alone unless
# --force is passed.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST="$ROOT/build/bin/ffmpeg.exe"
URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl.zip"

if [ -e "$DEST" ] && [ "${1:-}" != "--force" ]; then
    echo "== $DEST already present, leaving it alone (pass --force to refetch)"
    exit 0
fi

command -v curl >/dev/null 2>&1 || { echo "curl is required (ships with Git for Windows)." >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || { echo "unzip is required (ships with Git for Windows)." >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== downloading $URL"
curl -sSL -o "$WORK/ffmpeg.zip" "$URL"

echo "== extracting"
unzip -q "$WORK/ffmpeg.zip" -d "$WORK/extracted"

EXE=$(find "$WORK/extracted" -iname "ffmpeg.exe" | head -1)
if [ -z "$EXE" ]; then
    echo "ffmpeg.exe not found inside the downloaded archive - BtbN may have changed their layout." >&2
    exit 1
fi

mkdir -p "$ROOT/build/bin"
cp "$EXE" "$DEST"
echo "== placed at $DEST"
echo "== packaging (npm run build) verifies its licence automatically via check-ffmpeg-license.cjs"
