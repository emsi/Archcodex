#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"
command -v flatpak-builder >/dev/null
command -v flatpak >/dev/null
command -v ostree >/dev/null
output=$(python3 scripts/release.py prepare)
version=$(cat VERSION)
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)
mkdir -p .build dist
target_arch=$(flatpak --default-arch)
# Keep this build's runtimes/remotes separate from the user's installed applications.
export FLATPAK_USER_DIR="$repo_dir/.build/flatpak"
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
  --default-branch=stable --repo=.build/repository \
  .build/flatpak-app packaging/io.github.emsi.Archcodex.json
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
  .build/repository "$output/Archcodex-$version-$target_arch.flatpak" io.github.emsi.Archcodex stable
python3 scripts/release.py finish --build-dir .build/flatpak-app --repository .build/repository
