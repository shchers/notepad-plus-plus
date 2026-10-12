#!/usr/bin/env bash
# Installs everything needed to build, test and package the GTK port on
# Ubuntu 24.04 / Debian 12+. Run with sudo or as root.
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    exec sudo -- "$0" "$@"
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libgtk-3-dev \
    xvfb xauth \
    dpkg-dev file \
    debhelper fakeroot lintian

echo "Build dependencies installed."
