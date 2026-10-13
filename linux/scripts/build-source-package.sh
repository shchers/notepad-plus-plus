#!/usr/bin/env bash
# Source packages for a Launchpad PPA, one per Ubuntu series, from the
# committed tree (HEAD).
#
#   linux/scripts/build-source-package.sh [options] SERIES...
#
#   -k KEYID       sign with this GPG key (default: unsigned, for checking)
#   -r N           PPA revision (default 1): version <upstream>-1~ppaN~<series>1
#   -u ppa:USER/PPA  upload the signed .changes with dput
#
# Example: linux/scripts/build-source-package.sh -k 0xABCD -u ppa:me/npp-gtk jammy noble
#
# Each series gets its own Debian revision suffix, so one upload per series
# can coexist in the PPA. The orig tarball (git archive of HEAD) is made
# once per upstream version and reused afterwards, as archives require; it
# is kept in dist/ppa/. Needs dpkg-dev, devscripts (dch), and dput to upload.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
KEY="" REV=1 UPLOAD=""
while getopts "k:r:u:" opt; do
    case $opt in
    k) KEY=$OPTARG ;;
    r) REV=$OPTARG ;;
    u) UPLOAD=$OPTARG ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# > 0 )) || { echo "usage: $0 [-k KEYID] [-r N] [-u ppa:USER/PPA] SERIES..." >&2; exit 2; }
[[ -z $UPLOAD || -n $KEY ]] || { echo "Uploading needs a signed package: give -k KEYID" >&2; exit 2; }
for tool in dpkg-buildpackage dch; do
    command -v "$tool" >/dev/null || { echo "$tool is missing (apt install dpkg-dev devscripts)" >&2; exit 1; }
done
[[ -z $UPLOAD ]] || command -v dput >/dev/null || { echo "dput is missing (apt install dput)" >&2; exit 1; }
if [[ -n $(git -C "$ROOT" status --porcelain --untracked-files=no) ]]; then
    echo "Uncommitted changes: the package is built from HEAD; commit first." >&2
    exit 1
fi

CHANGELOG="$ROOT/linux/debian/changelog"
PKG=$(dpkg-parsechangelog -l "$CHANGELOG" -S Source)
VERSION=$(dpkg-parsechangelog -l "$CHANGELOG" -S Version)
UPSTREAM=${VERSION%-*}
OUT="$ROOT/dist/ppa"
mkdir -p "$OUT"
ORIG="$OUT/${PKG}_${UPSTREAM}.orig.tar.xz"
if [[ -f $ORIG ]]; then
    echo "Reusing $ORIG"
else
    git -C "$ROOT" archive --format=tar --prefix="${PKG}-${UPSTREAM}/" HEAD | xz -T0 > "$ORIG"
fi

for SERIES in "$@"; do
    PPA_VERSION="${UPSTREAM}-1~ppa${REV}~${SERIES}1"
    WORK=$(mktemp -d)
    tar -xJf "$ORIG" -C "$WORK"
    SRC="$WORK/${PKG}-${UPSTREAM}"
    # The committed tree may be newer than the orig (a later revision):
    # dpkg-source records the difference as debian/patches/debian-changes.
    git -C "$ROOT" archive --format=tar HEAD | tar -x -C "$SRC"
    cp -rL "$SRC/linux/debian" "$SRC/debian"
    (cd "$SRC" && DEBFULLNAME=${DEBFULLNAME:-$(dpkg-parsechangelog -l debian/changelog -S Maintainer | sed 's/ <.*//')} \
        DEBEMAIL=${DEBEMAIL:-$(dpkg-parsechangelog -l debian/changelog -S Maintainer | sed 's/.*<\(.*\)>/\1/')} \
        dch -b -v "$PPA_VERSION" -D "$SERIES" --force-distribution "PPA build for Ubuntu $SERIES.")
    cp "$ORIG" "$WORK/"
    SIGN=(-us -uc)
    [[ -n $KEY ]] && SIGN=(-k"$KEY")
    # -sa: every upload carries the (identical) orig, so any series can be
    # the first one Launchpad sees for this upstream version.
    (cd "$SRC" && dpkg-buildpackage -S -d -sa "${SIGN[@]}")
    mv -f "$WORK/${PKG}_${PPA_VERSION}"* "$OUT/"
    rm -rf "$WORK"
    echo "Source package: $OUT/${PKG}_${PPA_VERSION}.dsc"
    if [[ -n $UPLOAD ]]; then
        dput "$UPLOAD" "$OUT/${PKG}_${PPA_VERSION}_source.changes"
    fi
done
