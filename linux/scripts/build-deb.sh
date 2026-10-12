#!/usr/bin/env bash
# Debian-policy build with debhelper (dpkg-buildpackage), the route to use for
# a PPA or distribution. Produces dist/*.deb and runs lintian.
#
#   linux/scripts/build-deb.sh              # binary package, unsigned
#   linux/scripts/build-deb.sh --source     # also a source package (.dsc), needs git
#
# debian/ lives in linux/debian so the port stays self-contained; this script
# copies it to the package root, where dpkg-buildpackage expects it.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WITH_SOURCE=0
[[ ${1:-} == "--source" ]] && WITH_SOURCE=1

cd "$ROOT"
if [[ -e debian && ! -L debian ]] && ! cmp -s <(cd debian && find . -type f | sort | xargs cat) \
                                           <(cd linux/debian && find -L . -type f | sort | xargs cat); then
    echo "A different ./debian already exists; remove it first." >&2
    exit 1
fi
rm -rf debian
cp -rL linux/debian debian
trap 'rm -rf "$ROOT/debian" "$ROOT/.pc"' EXIT   # .pc: dpkg-source quilt state

VERSION=$(dpkg-parsechangelog -S Version)
UPSTREAM=${VERSION%-*}
PKG=$(dpkg-parsechangelog -S Source)

if (( WITH_SOURCE )); then
    # 3.0 (quilt) needs the pristine upstream tarball next to the tree. It is
    # the whole committed tree: to dpkg-source only the top-level debian/ is
    # packaging, linux/debian/ is ordinary upstream content.
    # Archives reject a changed orig for the same upstream version, so an
    # existing one is reused; later changes under linux/ then become
    # debian/patches/debian-changes (single-debian-patch + auto-commit).
    ORIG="../${PKG}_${UPSTREAM}.orig.tar.xz"
    if [[ -f $ORIG ]]; then
        echo "Reusing existing $ORIG"
    else
        git -C "$ROOT" archive --format=tar --prefix="${PKG}-${UPSTREAM}/" HEAD | xz -T0 > "$ORIG"
    fi
    dpkg-buildpackage -us -uc
else
    dpkg-buildpackage -us -uc -b
fi

mkdir -p dist
mv -f ../"${PKG}"_"${VERSION}"_*.deb dist/
mv -f ../"${PKG}"-dbgsym_"${VERSION}"_*.ddeb dist/ 2>/dev/null || true
mv -f ../"${PKG}"_"${VERSION}"*.{dsc,debian.tar.*,buildinfo,changes} dist/ 2>/dev/null || true
rm -rf obj-* 2>/dev/null || true

if command -v lintian >/dev/null; then
    lintian --info --display-info dist/"${PKG}"_"${VERSION}"_*.deb || true
fi
echo "Package(s):"; ls -1 dist/
