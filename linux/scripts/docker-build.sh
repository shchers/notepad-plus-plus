#!/usr/bin/env bash
# Docker wrapper (context = repository root).
#
#   linux/scripts/docker-build.sh deb       # build + test in a container, .deb -> dist/
#   linux/scripts/docker-build.sh image     # runnable image "notepad-plus-plus-gtk"
#   linux/scripts/docker-build.sh shell     # toolchain container with the tree mounted at /src
#   linux/scripts/docker-build.sh run FILE  # run the image on this X11 display
#
# Environment: UBUNTU_VERSION (default 24.04), PLATFORM (e.g. linux/arm64:
# build for another architecture; needs qemu-user-static / binfmt on an
# amd64 host, or run natively on an arm64 machine), DOCKER_BUILD_ARGS (extra
# flags, e.g. "--network host" behind a proxy).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DOCKERFILE="$ROOT/linux/docker/Dockerfile"
UBUNTU_VERSION=${UBUNTU_VERSION:-24.04}
read -r -a EXTRA <<<"${DOCKER_BUILD_ARGS:-}"
PLATFORM_ARGS=()
[[ -n ${PLATFORM:-} ]] && PLATFORM_ARGS=(--platform "$PLATFORM")
build() { docker build "${EXTRA[@]}" "${PLATFORM_ARGS[@]}" --build-arg UBUNTU_VERSION="$UBUNTU_VERSION" -f "$DOCKERFILE" "$@" "$ROOT"; }

case ${1:-deb} in
deb)
    build --target artifact --output "type=local,dest=$ROOT/dist"
    ls -1 "$ROOT"/dist/*.deb
    ;;
image)
    build -t notepad-plus-plus-gtk
    ;;
shell)
    build --target toolchain -t npp-gtk-toolchain
    exec docker run --rm -it -u "$(id -u):$(id -g)" -v "$ROOT":/src -w /src \
        npp-gtk-toolchain bash
    ;;
run)
    shift
    [[ -n ${DISPLAY:-} ]] || { echo "DISPLAY is not set" >&2; exit 1; }
    mounts=()
    for f in "$@"; do mounts+=(-v "$(realpath "$f")":"/home/npp/$(basename "$f")"); done
    exec docker run --rm -e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix "${mounts[@]}" \
        notepad-plus-plus-gtk "${@##*/}"
    ;;
*)
    sed -n '2,10p' "$0"; exit 2 ;;
esac
