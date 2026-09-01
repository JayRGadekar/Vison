#!/usr/bin/env sh
# Reconstruct third_party/ after a fresh clone.
#
# stable-diffusion.cpp and vision.cpp are vendored as full git clones rather
# than submodules (see PATCHES.md), and .gitignore excludes them, so a fresh
# clone of Vison has neither. This script restores both at the pinned commits
# and applies our local patches, which is the only thing that makes the tree
# buildable. Without the sd.cpp patch, FLUX is rejected as an unknown
# architecture and image generation does not work at all.
#
# Safe to re-run: existing clones are left alone unless they are missing.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TP="$ROOT/third_party"

SD_URL="https://github.com/leejet/stable-diffusion.cpp.git"
SD_REF="487de75"
VC_URL="https://github.com/Acly/vision.cpp.git"
VC_REF="26a7529"

clone_at() {
    url=$1; ref=$2; dest=$3
    if [ -d "$dest/.git" ]; then
        echo "== $dest already present, leaving it alone"
        return 0
    fi
    echo "== cloning $url -> $dest"
    git clone "$url" "$dest"
    git -C "$dest" checkout "$ref"
    # ggml is a submodule of stable-diffusion.cpp and must come along.
    git -C "$dest" submodule update --init --recursive
}

clone_at "$SD_URL" "$SD_REF" "$TP/stable-diffusion.cpp"
clone_at "$VC_URL" "$VC_REF" "$TP/vision.cpp"

# Apply patches only if they are not in already, so re-running is harmless.
for p in "$TP"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "== $p"
    if git -C "$TP/stable-diffusion.cpp" apply --check --reverse --ignore-whitespace "$p" 2>/dev/null; then
        echo "   already applied"
    elif git -C "$TP/stable-diffusion.cpp" apply --ignore-whitespace "$p"; then
        echo "   applied"
    else
        echo "   FAILED - see PATCHES.md and apply by hand" >&2
        exit 1
    fi
done

echo
echo "third_party is ready. See PATCHES.md for what each patch does and why."
