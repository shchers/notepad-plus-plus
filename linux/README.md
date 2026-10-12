# Notepad++ for Linux — native GTK port (preview)

An **unofficial** native Linux build of the Notepad++ source tree. It uses
Scintilla's GTK 3 backend, all Lexilla lexers, Notepad++'s Boost regex engine,
and the upstream language, style and theme definitions, with no Wine
involved. The port lives entirely in this `linux/` directory and builds
against the unmodified upstream sources around it.

```
build/bin/notepad-plus-plus-gtk -n42 src/main.cpp
```

## Quick start

```bash
# Ubuntu 24.04
sudo apt-get install -y build-essential cmake ninja-build pkg-config libgtk-3-dev xvfb xauth

cmake -S linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
build/bin/notepad-plus-plus-gtk
```

Build a `.deb`:

```bash
cpack --config build/CPackConfig.cmake -B build/pkg     # quick (CPack)
linux/scripts/build-deb.sh                              # Debian-policy (needs debhelper)
docker build -f linux/docker/Dockerfile --target artifact --output type=local,dest=dist .   # in Docker
sudo apt install ./dist/notepad-plus-plus-gtk_*.deb
```

## Documentation

| Document | Contents |
|---|---|
| [docs/BUILD.md](docs/BUILD.md) | Native and Docker builds, options, running from a container, troubleshooting |
| [docs/PACKAGING.md](docs/PACKAGING.md) | `.deb` with CPack or debhelper, PPA/source packages, versioning, verification, apt repo |
| [docs/PORTING.md](docs/PORTING.md) | Survey of the upstream code, architecture, feature status, module map, roadmap, plugin ABI |

## What works

Tabs in two views (move and clone documents between them), open/save with
encoding detection (UTF-8, BOM, UTF-16, legacy code pages via uchardet),
syntax highlighting for all 95 Notepad++ languages, the 22 upstream colour
themes, find/replace with Notepad++'s regex semantics, folding, bookmarks,
auto-indent, line operations, EOL conversion, sessions in Notepad++'s
`session.xml` format with snapshots of unsaved documents, recent files,
backup on save, detection of files changed outside the editor, a single
running instance, import of a Windows `config.xml`, and Notepad++ keyboard
shortcuts and command-line switches. Docking panels, plugins, macros and the
Preferences dialog are not ported yet. The full table is in
[docs/PORTING.md §4](docs/PORTING.md#4-what-works-today).

## Layout

```
linux/
├── CMakeLists.txt          entry point (reads version from PowerEditor/src/resource.h)
├── cmake/                  third-party targets, generated language table, install, CPack
├── compat/lexuser/         empty <windows.h> stand-in used only for lexilla/lexers/LexUser.cxx
├── src/
│   ├── core/               UI-free model: languages, styles, lexer keyword plans,
│   │                       buffers, sessions, recent files, config.xml import
│   ├── platform/           OS services: XDG paths, encodings, file I/O and
│   │                       monitoring (GIO), settings
│   ├── ui/                 GTK 3: EditorView (Scintilla), MainWindow, FindBar
│   └── main.cpp            CLI compatible with Notepad++ switches
├── tests/                  headless CTest suite (Xvfb)
├── packaging/              .desktop, icons, man page, copyright, CPack maintainer scripts
├── debian/                 debhelper packaging (copied to the root by scripts/build-deb.sh)
├── docker/                 multi-stage Dockerfile (toolchain, build, artifact, runtime)
├── scripts/                install-build-deps, build-native, build-deb, docker-build
└── docs/                   BUILD, PACKAGING, PORTING
```

## Licence

GPL-3 (version 3 only), as upstream Notepad++. Bundled components keep their
own licences; see `packaging/copyright`. "Notepad++" is the name of the
upstream project, and this port is not endorsed by it.
