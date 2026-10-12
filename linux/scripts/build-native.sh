#!/usr/bin/env bash
# Native build + tests + (optionally) CPack .deb.
#
#   linux/scripts/build-native.sh            # Release build in ./build, run tests
#   linux/scripts/build-native.sh --deb      # ... and produce dist/*.deb via CPack
#   BUILD_TYPE=Debug BUILD_DIR=build-dbg linux/scripts/build-native.sh
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build}
BUILD_TYPE=${BUILD_TYPE:-Release}
MAKE_DEB=0
[[ ${1:-} == "--deb" ]] && MAKE_DEB=1

generator=()
command -v ninja >/dev/null && generator=(-G Ninja)

cmake -S "$ROOT/linux" -B "$BUILD_DIR" "${generator[@]}" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

if (( MAKE_DEB )); then
    cpack --config "$BUILD_DIR/CPackConfig.cmake" -B "$BUILD_DIR/pkg"
    mkdir -p "$ROOT/dist"
    cp "$BUILD_DIR"/pkg/*.deb "$ROOT/dist/"
    echo "Package(s):"; ls -1 "$ROOT"/dist/*.deb
fi

echo "Run it:  $BUILD_DIR/bin/notepad-plus-plus-gtk"
