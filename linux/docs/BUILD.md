# Building the Notepad++ GTK port

The port lives in `linux/` and builds against the **unmodified** upstream tree
next to it (`scintilla/`, `lexilla/`, `boostregex/`, `PowerEditor/src/`).
All commands below run from the **repository root**.

Target platform: **Ubuntu 24.04 LTS** (GTK 3.24, GCC 13, CMake 3.28). Also
tested on Ubuntu 22.04 (GCC 11, CMake 3.22), and on amd64 and arm64. Anything with GTK ≥ 3.22, a
C++20 compiler (GCC ≥ 11 or Clang ≥ 14) and CMake ≥ 3.22 should work.

- [Native build](#native-build)
- [Docker build](#docker-build)
- [Build options](#build-options)
- [Troubleshooting](#troubleshooting)

---

## Native build

### 1. Install dependencies

```bash
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config libgtk-3-dev \
    xvfb xauth \
    dpkg-dev file debhelper fakeroot lintian
```

`linux/scripts/install-build-deps.sh` runs the same command.
Only the first line is required to compile; `xvfb`/`xauth` run the GUI tests
headless, and the last line is for packaging (see [PACKAGING.md](PACKAGING.md)).

### 2. Configure, build, test

```bash
cmake -S linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

A clean build takes about 2 minutes on 2 cores: most of that is the 128
Lexilla lexer modules and Boost.Regex. Incremental rebuilds of the port code take
seconds.

The tests start a real GTK window. With no `DISPLAY`/`WAYLAND_DISPLAY` (SSH,
CI, containers) CMake wraps them in `xvfb-run` automatically; on a desktop
session they briefly use your display. The `single_instance` test runs on a
private session bus from `dbus-run-session` (package `dbus`) and is reported
as skipped without it. Tests use their own config directory, never
`~/.config/notepad-plus-plus-gtk`.

### 3. Run without installing

```bash
build/bin/notepad-plus-plus-gtk                    # empty "new 1"
build/bin/notepad-plus-plus-gtk -n120 src/main.c   # open at line 120
build/bin/notepad-plus-plus-gtk -lpython script    # force a language
```

The build tree mirrors the install layout (`build/bin`, `build/share/notepad-plus-plus-gtk`),
so the binary finds `langs.model.xml`, `stylers.model.xml` and `themes/` with
no installation. Supported switches: `--help`, or the man page in
`linux/packaging/notepad-plus-plus-gtk.1`.

### 4. Install (optional)

Prefer installing the `.deb` ([PACKAGING.md](PACKAGING.md)) so `apt` can
remove it again. For a quick local install instead:

```bash
cmake -S linux -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build && cmake --install build
```

### One-liner

```bash
linux/scripts/build-native.sh          # configure + build + test
linux/scripts/build-native.sh --deb    # ... + CPack .deb into dist/
```

---

## Docker build

The Dockerfile is `linux/docker/Dockerfile`; the **build context is the
repository root**. BuildKit (default since Docker 23) is required for
`--output` and for `linux/docker/Dockerfile.dockerignore`, which keeps the
context small.

| Target      | What you get                                                         |
|-------------|----------------------------------------------------------------------|
| `toolchain` | Ubuntu + compilers + GTK headers: an image for iterative builds      |
| `build`     | Compiles with `-Werror`, runs the test suite under Xvfb, makes `.deb` |
| `artifact`  | Only the `.deb` (use with `--output`)                                 |
| `runtime`   | **Default.** Clean Ubuntu with the `.deb` installed; runs the editor |

### Build the .deb in a container

```bash
docker build -f linux/docker/Dockerfile --target artifact \
    --output type=local,dest=dist .
ls dist/    # notepad-plus-plus-gtk_8.9.8.1-1_amd64.deb
```

The `build` stage runs the full test suite; a failing test fails the image
build, so a `.deb` in `dist/` has always passed the tests.

### Run the editor from the container (X11)

```bash
docker build -f linux/docker/Dockerfile -t notepad-plus-plus-gtk .

xhost +local:   # allow local containers to use your X server (undo: xhost -local:)
docker run --rm -e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v "$PWD":/home/npp/work notepad-plus-plus-gtk work/README.md
```

On a Wayland session GTK uses XWayland through the same `DISPLAY` socket. For
native Wayland instead, pass the compositor socket:

```bash
docker run --rm -e WAYLAND_DISPLAY -e XDG_RUNTIME_DIR=/tmp \
    -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:/tmp/$WAYLAND_DISPLAY" \
    -e GDK_BACKEND=wayland notepad-plus-plus-gtk
```

The `runtime` stage installs the `.deb` on a bare `ubuntu` image, so building
it also proves the package's `Depends:` are complete.

### Iterate inside a toolchain container

Build once, then compile your working tree with incremental rebuilds:

```bash
docker build -f linux/docker/Dockerfile --target toolchain -t npp-gtk-toolchain .
docker run --rm -it -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src npp-gtk-toolchain \
    bash -c 'cmake -S linux -B build-docker -G Ninja && cmake --build build-docker && ctest --test-dir build-docker'
```

### Wrapper script

```bash
linux/scripts/docker-build.sh deb         # .deb -> dist/
linux/scripts/docker-build.sh image       # runtime image
linux/scripts/docker-build.sh shell       # toolchain shell with the tree mounted
linux/scripts/docker-build.sh run FILE    # run the image on your X display
UBUNTU_VERSION=22.04 linux/scripts/docker-build.sh deb   # other base release
```

Ubuntu 22.04 and 24.04 are both tested. Build one `.deb` per target release:
`Depends:` names differ between releases (for example `libatk1.0-0` on 22.04
and `libatk1.0-0t64` on 24.04). To keep the files apart, give each a
release-specific revision:

```bash
docker build -f linux/docker/Dockerfile --target artifact \
    --build-arg UBUNTU_VERSION=22.04 --build-arg DEB_REVISION=1~jammy1 \
    --output type=local,dest=dist .
# -> notepad-plus-plus-gtk_8.9.8.1-1~jammy1_amd64.deb
```

### Building behind a proxy or registry mirror

- Docker Hub mirror: `docker build --build-arg UBUNTU_VERSION=24.04 ...` uses
  `FROM ubuntu:${UBUNTU_VERSION}`; retag your mirror's image as `ubuntu:24.04`
  locally (`docker pull mirror/ubuntu:24.04 && docker tag ... ubuntu:24.04`).
  The Dockerfile has no `# syntax=` line, so no frontend image is pulled.
- HTTP(S) proxy for apt: `docker build --build-arg http_proxy=... --build-arg https_proxy=...`
  (Docker's predefined proxy args; they are not stored in the image).
- Proxy only reachable on the host's loopback: add `--network host`
  (or `DOCKER_BUILD_ARGS="--network host"` for the wrapper).

---

## Build options

| CMake option                | Default   | Meaning                                              |
|-----------------------------|-----------|------------------------------------------------------|
| `CMAKE_BUILD_TYPE`          | `Release` | `Debug` enables Scintilla's `DEBUG` checks too       |
| `CMAKE_INSTALL_PREFIX`      | `/usr/local` | Use `/usr` for packages                           |
| `NPP_BUILD_TESTS`           | `ON`      | Build `nppgtk_tests` and register CTest tests        |
| `NPP_WARNINGS_AS_ERRORS`    | `OFF`     | `-Werror` for the port's own code (Docker/CI use ON) |
| `NPP_DEB_REVISION`          | `1`       | Debian revision of the CPack package                 |
| `CPACK_PACKAGE_CONTACT`     | maintainer | `Maintainer:` field of the CPack package            |

Third-party code (Scintilla, Lexilla, Boost.Regex, pugixml, uchardet) is
compiled with warnings silenced, because it is upstream code we do not patch.

Runtime environment:

| Variable            | Meaning                                                        |
|---------------------|----------------------------------------------------------------|
| `NPP_GTK_DATA_DIR`  | Override the directory with `langs.model.xml` and friends      |
| `XDG_CONFIG_HOME`   | Settings, user `langs.xml`/`stylers.xml`, extra `themes/` live in `$XDG_CONFIG_HOME/notepad-plus-plus-gtk/` |

---

## Troubleshooting

**`Package 'gtk+-3.0' not found`**: install `libgtk-3-dev`.

**`'intptr_t' does not name a type` in `Sci_Position.h` or uchardet**: you
are compiling those files outside this CMake project. Upstream's copies miss
`#include <stdint.h>`; `linux/cmake/ThirdParty.cmake` force-includes it (see
"Upstream fixes" in [PORTING.md](PORTING.md)).

**Tests hang or fail with `cannot open display`**: install `xvfb xauth`, or
unset `DISPLAY` so CTest uses `xvfb-run`.

**Toolbar icons missing / `Could not load a pixbuf ... .svg`**: install
`adwaita-icon-theme librsvg2-common` (the `.deb` recommends both).

**View > Terminal only shows "The terminal needs the VTE library"**: install
VTE for GTK 3 (`libvte-2.91-0` on Debian and Ubuntu, `vte291` on Fedora,
`vte3` on Arch). It is loaded at run time, so no rebuild is needed.

**Dark themes keep a light window frame**: the GTK chrome follows the editor
theme via `gtk-application-prefer-dark-theme`. Your GTK theme needs a dark
variant (Adwaita and Yaru have one); a theme name pointing at a theme that
isn't installed falls back to light Adwaita.

**Alt+drag moves the window instead of making a column selection**: most
Linux window managers bind Alt+drag. Use Shift+Alt+arrow keys, or rebind the
window manager modifier (e.g. GNOME: `gsettings set org.gnome.desktop.wm.preferences mouse-button-modifier '<Super>'`).
