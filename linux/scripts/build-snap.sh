#!/usr/bin/env bash
# Builds the snap from the committed tree (HEAD).
#
#   linux/scripts/build-snap.sh             # assemble the project, run snapcraft pack
#   linux/scripts/build-snap.sh --prepare   # only assemble dist/snap-project (CI uses
#                                           # snapcore/action-build on it)
#
# Environment: NPP_APP_ID (default org.notepad_plus_plus.NotepadPlusPlusGtk).
# snapcraft builds in LXD or Multipass; see linux/docs/PACKAGING.md.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
APP_ID=${NPP_APP_ID:-org.notepad_plus_plus.NotepadPlusPlusGtk}
# Same source of truth as CMake: VERSION_PRODUCT_VALUE in resource.h.
VERSION=$(sed -n 's/.*VERSION_PRODUCT_VALUE[^0-9]*\([0-9][0-9.]*[0-9]\).*/\1/p' "$ROOT/PowerEditor/src/resource.h")
[[ -n $VERSION ]] || { echo "Cannot read the version from PowerEditor/src/resource.h" >&2; exit 1; }

PROJECT="$ROOT/dist/snap-project"
rm -rf "$PROJECT"
mkdir -p "$PROJECT/snap/gui"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@APP_ID@/$APP_ID/g" "$ROOT/linux/packaging/snap/snapcraft.yaml.in" \
    > "$PROJECT/snap/snapcraft.yaml"
cp "$ROOT/linux/packaging/icons/hicolor/256x256/apps/notepad-plus-plus-gtk.png" "$PROJECT/snap/gui/"
git -C "$ROOT" archive --format=tar HEAD | xz -T0 > "$PROJECT/notepad-plus-plus-gtk-src.tar.xz"
echo "Snap project: $PROJECT (version $VERSION, from HEAD $(git -C "$ROOT" rev-parse --short HEAD))"

[[ ${1:-} == "--prepare" ]] && exit 0
command -v snapcraft >/dev/null || { echo "snapcraft is not installed (sudo snap install snapcraft --classic)" >&2; exit 1; }
cd "$PROJECT"
snapcraft pack
mv -f ./*.snap "$ROOT/dist/"
ls -1 "$ROOT"/dist/*.snap
