#!/usr/bin/env bash
# Installs everything needed to build, test and package the GTK port on
# Ubuntu 22.04+ / Debian 12+ (amd64 and arm64). Run with sudo or as root.
# appstream and desktop-file-utils let the test suite validate the
# AppStream metadata and the desktop file, Enchant and English, German and
# Russian dictionaries the Spell Checker tests (language detection); devscripts is for PPA uploads.
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    exec sudo -- "$0" "$@"
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libgtk-3-dev \
    xvfb xauth dbus \
    dpkg-dev file \
    debhelper fakeroot lintian devscripts \
    appstream desktop-file-utils \
    libenchant-2-2 hunspell-en-us hunspell-de-de hunspell-ru

echo "Build dependencies installed."
