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

Packages: `.deb` (CPack or debhelper), a PPA, Flatpak and snap, for amd64
and arm64 — see [docs/PACKAGING.md](docs/PACKAGING.md). Build a `.deb`:

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
| [docs/PACKAGING.md](docs/PACKAGING.md) | `.deb` with CPack or debhelper, PPA, Flatpak, snap, arm64, AppStream, versioning, verification |
| [docs/PORTING.md](docs/PORTING.md) | Survey of the upstream code, architecture, feature status, module map, roadmap, plugin design |
| [docs/PLUGINS.md](docs/PLUGINS.md) | Writing and porting plugins: the SDK, plugin folders, messages and notifications, menus, toolbar, docked panels |
| [docs/REMOTE_FILES.md](docs/REMOTE_FILES.md) | The bundled Remote Files plugin: editing files on FTP and SFTP servers, or over a shell link |
| [docs/SPELL_CHECK.md](docs/SPELL_CHECK.md) | The bundled Spell Checker plugin: spell checking as you type, with Enchant's dictionaries |

## What works

Tabs in two views (move and clone documents between them), open/save with
encoding detection (UTF-8, BOM, UTF-16, legacy code pages via uchardet),
syntax highlighting for all 95 Notepad++ languages and User Defined
Languages, the 22 upstream colour themes and the Style Configurator, the
Find / Replace / Find in Files / Mark dialog with Notepad++'s regex
semantics and its search results panel, incremental search, the Edit menu's
line, case and blank operations, the Column Editor, macros and Run commands,
the Shortcut Mapper, Preferences, all Notepad++ translations, printing,
docking panels (Function List, Folder as Workspace, Document Map, Document
List, Character Panel, Clipboard History, Project Panels, a terminal),
folding, bookmarks, sessions in Notepad++'s `session.xml` format with
snapshots of unsaved documents, recent files, backup on save, detection of
files changed outside the editor and a single running instance. Notepad++'s
own files work as they are: `shortcuts.xml` (keys, macros, Run commands),
`stylers.xml` and themes, `userDefineLang.xml`, `session.xml`, function list
rules and `.workspace` files, and a Windows `config.xml` can be imported.
Plugins use Notepad++'s plugin interface: their source builds against the
SDK in `sdk/` (upstream's plugin headers on a small `windows.h`) into a
`.so`, with a GTK user interface — see [docs/PLUGINS.md](docs/PLUGINS.md)
and the sample in `sdk/examples/HelloGtk`; Plugins ▸ Plugins Admin installs,
removes and deactivates them. One plugin comes with the editor: Remote
Files, for editing files on FTP and SFTP servers (or over a Midnight
Commander-style shell link) in the spirit of NppFTP
([docs/REMOTE_FILES.md](docs/REMOTE_FILES.md)). Preferences has every page that applies on
Linux, including auto-completion with upstream's API files, smart
highlighting, matching tags and clickable links. The full table is in
[docs/PORTING.md §4](docs/PORTING.md#4-what-works-today).

## Layout

```
linux/
├── CMakeLists.txt          entry point (reads version from PowerEditor/src/resource.h)
├── cmake/                  third-party targets, generated language and command tables, install, CPack
├── compat/lexuser/         empty <windows.h> stand-in used only for lexilla/lexers/LexUser.cxx
├── src/
│   ├── core/               UI-free model: languages, styles, UDLs, lexer keyword plans,
│   │                       buffers, sessions, recent files, config.xml import, search
│   │                       engine, Find in Files, Edit operations, shortcuts.xml, nativeLang,
│   │                       function list parser, workspaces
│   ├── platform/           OS services: XDG paths, encodings, file I/O and
│   │                       monitoring (GIO), settings
│   ├── ui/                 GTK 3: EditorView (Scintilla), MainWindow, menus and commands,
│   │                       Find dialog, search results, dialogs, printing, dock and panels,
│   │                       plugin manager and plugin messages
│   └── main.cpp            CLI compatible with Notepad++ switches
├── plugins/NppRemote/      bundled plugin: Remote Files (FTP on POSIX sockets, SFTP and shell link via OpenSSH)
├── plugins/SpellCheck/     bundled plugin: Spell Checker (as you type, dictionaries via Enchant)
├── sdk/                    plugin SDK: include/ (windows.h, tchar.h, NppGtk.h), pkg-config
│                           template, examples/HelloGtk
├── tests/                  headless CTest suite (Xvfb), test plugins
├── packaging/              .desktop, AppStream metainfo, icons, man page, copyright,
│                           CPack maintainer scripts, flatpak/ manifest, snap/ recipe
├── debian/                 debhelper packaging (copied to the root by scripts/build-deb.sh)
├── docker/                 multi-stage Dockerfile (toolchain, build, artifact, runtime)
├── scripts/                install-build-deps, build-native, build-deb, docker-build,
│                           build-source-package (PPA), build-flatpak, build-snap
└── docs/                   BUILD, PACKAGING, PORTING, PLUGINS, REMOTE_FILES
```

## Licence

GPL-3 (version 3 only), as upstream Notepad++. Bundled components keep their
own licences; see `packaging/copyright`. "Notepad++" is the name of the
upstream project, and this port is not endorsed by it.
