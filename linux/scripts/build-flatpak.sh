#!/usr/bin/env bash
# Builds the Flatpak from this working tree (linux/packaging/flatpak/*.yml):
# a local repository in dist/flatpak-repo and a single-file bundle
# dist/<app-id>.flatpak that `flatpak install --user` accepts.
#
#   linux/scripts/build-flatpak.sh            # build, export, bundle
#   linux/scripts/build-flatpak.sh --install  # also install for this user
#
# Environment: NPP_APP_ID to publish under another application ID (Flathub
# needs one of the publisher's domain); FLATPAK_ARCH (default: this machine).
# Needs flatpak and flatpak-builder; the GNOME runtime and SDK are installed
# from Flathub (user installation) when missing.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DEFAULT_ID=org.notepad_plus_plus.NotepadPlusPlusGtk
APP_ID=${NPP_APP_ID:-$DEFAULT_ID}
INSTALL=0
[[ ${1:-} == "--install" ]] && INSTALL=1

for tool in flatpak flatpak-builder; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed (apt install flatpak flatpak-builder)" >&2; exit 1; }
done

MANIFEST="$ROOT/linux/packaging/flatpak/$DEFAULT_ID.yml"
if [[ $APP_ID != "$DEFAULT_ID" ]]; then
    # Same manifest under another ID: the file name must match the ID.
    GEN="$ROOT/linux/packaging/flatpak/$APP_ID.yml"
    sed "s/$DEFAULT_ID/$APP_ID/g" "$MANIFEST" > "$GEN"
    trap 'rm -f "$GEN"' EXIT
    MANIFEST=$GEN
fi

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
ARCH_ARGS=()
[[ -n ${FLATPAK_ARCH:-} ]] && ARCH_ARGS=(--arch="$FLATPAK_ARCH")

cd "$ROOT"
mkdir -p dist
flatpak-builder --user --force-clean --install-deps-from=flathub "${ARCH_ARGS[@]}" \
    --repo=dist/flatpak-repo build-flatpak "$MANIFEST"
flatpak build-bundle "${ARCH_ARGS[@]}" dist/flatpak-repo "dist/$APP_ID.flatpak" "$APP_ID" \
    --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo
echo "Bundle: dist/$APP_ID.flatpak"

if (( INSTALL )); then
    flatpak install --user --noninteractive --reinstall "dist/$APP_ID.flatpak"
    echo "Run: flatpak run $APP_ID"
fi
