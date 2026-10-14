# Packaging and distributing the GTK port

| Format | Tooling | Use it for |
|--------|---------|------------|
| **.deb, route A** | `cpack` from the CMake build | Quick local packages, CI artifacts, GitHub releases |
| **.deb, route B** | `dpkg-buildpackage` + `linux/debian/` | A Debian/Ubuntu archive, reproducible distro builds, `-dbgsym` packages |
| **PPA** | `linux/scripts/build-source-package.sh` (route B source packages) | Ubuntu users: `add-apt-repository ppa:…` |
| **Flatpak** | `linux/packaging/flatpak/*.yml`, `linux/scripts/build-flatpak.sh` | Every distribution; Flathub |
| **Snap** | `linux/packaging/snap/snapcraft.yaml.in`, `linux/scripts/build-snap.sh` | Ubuntu and other snapd systems; the Snap Store |

Every format is built for **amd64 and arm64** (see "Architectures"). The two
.deb routes give the same file layout and are lintian-clean. All formats
carry the same AppStream metadata, desktop file and icons, named after the
application ID (see "Application ID and AppStream").

All commands run from the repository root.

---

## What the package contains

```
/usr/bin/notepad-plus-plus-gtk
/usr/share/notepad-plus-plus-gtk/langs.model.xml        <- upstream, unmodified
/usr/share/notepad-plus-plus-gtk/stylers.model.xml      <- upstream, unmodified
/usr/share/notepad-plus-plus-gtk/themes/*.xml           <- upstream PowerEditor/installer/themes
/usr/share/notepad-plus-plus-gtk/{nativeLang,userDefineLangs,functionList,autoCompletion}/  <- upstream data
/usr/share/applications/org.notepad_plus_plus.NotepadPlusPlusGtk.desktop
/usr/share/metainfo/org.notepad_plus_plus.NotepadPlusPlusGtk.metainfo.xml
/usr/share/icons/hicolor/{16..256}/apps/org.notepad_plus_plus.NotepadPlusPlusGtk.png
/usr/share/man/man1/notepad-plus-plus-gtk.1.gz
/usr/share/doc/notepad-plus-plus-gtk/{copyright,changelog.Debian.gz}

# plugin SDK (route B: package notepad-plus-plus-gtk-dev)
/usr/include/notepad-plus-plus-gtk/{windows.h,tchar.h,NppGtk.h}            <- linux/sdk/include
/usr/include/notepad-plus-plus-gtk/{PluginInterface.h,Notepad_plus_msgs.h,Docking.h,dockingResource.h,menuCmdID.h}  <- upstream, unmodified
/usr/include/notepad-plus-plus-gtk/{Scintilla.h,Sci_Position.h,ScintillaWidget.h}
/usr/lib/<multiarch>/pkgconfig/notepad-plus-plus-gtk-plugin.pc
/usr/share/doc/notepad-plus-plus-gtk[-dev]/examples/HelloGtk/
```

Plugins are not part of any package. The editor loads them from
`~/.local/share/notepad-plus-plus-gtk/plugins/` and from
`/usr/lib/<multiarch>/notepad-plus-plus-gtk/plugins/` (found relative to the
executable, like the data; `pkg-config --variable=plugindir
notepad-plus-plus-gtk-plugin`), where a distribution can ship plugin
packages. See `docs/PLUGINS.md`.

`Depends:` is computed from the binary by `dpkg-shlibdeps`, so it is right
for whichever release you build on. On Ubuntu 24.04 it resolves to GTK 3,
GLib, Pango, Cairo, ATK, GDK-Pixbuf, libstdc++ and libc. Scintilla, Lexilla,
Boost.Regex, pugixml and uchardet are linked statically, as on Windows,
because Notepad++ ships modified copies of them.

`Recommends:` adds `adwaita-icon-theme`, `fonts-dejavu-core`,
`librsvg2-common` (toolbar icons and a good default monospace font) and
`libvte-2.91-0`, which the Terminal panel loads at run time (VTE is not a
build dependency; without it the panel says what to install).

---

## Route A — CPack (fast)

```bash
cmake -S linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -B build/pkg
```

Result: `build/pkg/notepad-plus-plus-gtk_8.9.8.1-1_amd64.deb`, with the
plugin SDK in the same package (route B splits it into `-dev`).
`linux/scripts/build-native.sh --deb` does all four steps and copies the
package into `dist/`. The Docker `artifact` target uses this route too:

```bash
docker build -f linux/docker/Dockerfile --target artifact --output type=local,dest=dist .
```

Important: configure with `-DCMAKE_INSTALL_PREFIX=/usr`. The CPack package
installs under the configured prefix, and the binary locates its data
relative to itself (`<prefix>/bin/../share/notepad-plus-plus-gtk`).

Customise with cache variables:

```bash
cmake -S linux -B build -DCMAKE_INSTALL_PREFIX=/usr \
      -DNPP_DEB_REVISION=2 \
      -DCPACK_PACKAGE_CONTACT="Your Name <you@example.org>"
```

CPack-specific pieces live in `linux/cmake/Packaging.cmake` and
`linux/packaging/deb/{postinst,postrm}`. The maintainer scripts refresh the
desktop database and icon cache, which debhelper does via triggers in route B.

## Route B — debhelper / dpkg-buildpackage (distribution quality)

The `debian/` directory is kept in `linux/debian/` so the port doesn't
touch upstream's top-level layout. `dpkg-buildpackage` expects it at the
package root, so copy it there first:

```bash
cp -rL linux/debian debian            # -L resolves the copyright symlink
dpkg-buildpackage -us -uc -b          # binary-only, unsigned
rm -rf debian obj-*                   # tidy up
ls ../notepad-plus-plus-gtk_*.deb
```

`linux/scripts/build-deb.sh` does exactly this. It also moves the results
(`.deb`, `-dbgsym.ddeb`, `.buildinfo`, `.changes`) into `dist/` and runs
lintian.

What `linux/debian/` does:

- `control`: `debhelper-compat (= 13)`; build-depends on cmake, ninja,
  pkg-config, libgtk-3-dev, plus xvfb/xauth for the test suite. The test
  dependencies carry the `<!nocheck>` build profile, so
  `DEB_BUILD_OPTIONS=nocheck DEB_BUILD_PROFILES=nocheck` skips them. Two
  binary packages: `notepad-plus-plus-gtk` and `notepad-plus-plus-gtk-dev`
  (the plugin SDK, `Multi-Arch: same`), split by
  `notepad-plus-plus-gtk{,-dev}.install`; `dh_missing --fail-missing`
  catches files that land in neither.
- `rules`: `dh $@ --sourcedirectory=linux --buildsystem=cmake+ninja` with
  `hardening=+all`; tests run through `dh_auto_test` (CTest, which uses
  `xvfb-run` when there is no display). debhelper installs `copyright` and
  the changelog into both packages, so CMake's copies (for route A) are
  excluded from `dh_missing`.
- `changelog`: the single source of the Debian version. CMake warns at
  configure time if it lags behind upstream's `resource.h`.
- `copyright`: DEP-5, covering Notepad++ (GPL-3), Scintilla/Lexilla, Boost,
  pugixml (MIT) and uchardet.

### Source package

A `.dsc` for Launchpad or a Debian archive needs an upstream tarball.
`linux/scripts/build-deb.sh --source` creates
`../notepad-plus-plus-gtk_<upstream>.orig.tar.xz` from the committed tree
with `git archive` (so commit first), then runs a full source + binary
build, unsigned. That is useful for checking that the source package is
complete. Only the top-level `debian/` goes into `.debian.tar.xz`;
`linux/debian/` is ordinary content of the orig tarball.

**Keep the orig tarball.** An archive accepts exactly one `.orig.tar.xz` per
upstream version. Later Debian revisions (`-2`, `~ppa2`, …) must reuse the
first one; the script does so when the file exists. Anything changed in the
tree since then, at minimum `linux/debian/changelog`, is recorded
automatically as `debian/patches/debian-changes`
(`single-debian-patch` + `auto-commit` in `linux/debian/source/options`).
The same options file keeps local `build*/`, `dist/` and `obj-*` directories
out of the source package.

### PPA

Launchpad builds from source packages only, one upload per Ubuntu series.
`linux/scripts/build-source-package.sh` makes them from the committed tree
(HEAD): one orig tarball per upstream version (kept in `dist/ppa/` and
reused afterwards), and for each series a Debian revision
`<upstream>-1~ppa<N>~<series>1` with its own changelog entry, so the uploads
for jammy, noble, … coexist and sort below an official `-1` package.

```bash
sudo apt install dpkg-dev devscripts dput
# check (unsigned) source packages:
linux/scripts/build-source-package.sh jammy noble
# signed, and uploaded:
linux/scripts/build-source-package.sh -k <GPG_KEY_ID> -u ppa:<you>/<ppa> jammy noble
# a later rebuild of the same upstream version: -r 2 (…~ppa2~noble1)
```

The script refuses uncommitted changes, because the package is HEAD. The
oldest supported series is **Ubuntu 22.04 (jammy)**: CMake 3.22, GCC 11 and
GTK 3.24 there build the port and pass its tests (checked in CI). Users
then install with:

```bash
sudo add-apt-repository ppa:<you>/<ppa>
sudo apt install notepad-plus-plus-gtk
```

---

## Versioning

| Field | Source |
|-------|--------|
| Upstream version (`8.9.8.1`) | `VERSION_PRODUCT_VALUE` in `PowerEditor/src/resource.h`, read by CMake |
| Debian revision (`-1`) | `NPP_DEB_REVISION` (CPack) / `linux/debian/changelog` (debhelper) |

After merging a new upstream release:

```bash
cd linux && dch -v <new-upstream>-1 "New upstream release" && cd ..
```

For backports or PPAs use a version that sorts below the "real" one, e.g.
`8.9.8.1-1~ppa1~noble1`.

Build a separate package for each target release, because `Depends:` is
computed from that release's libraries (Ubuntu 24.04 uses the `t64` library
names, 22.04 does not). A suffix such as `-1~jammy1` / `-1~noble1` keeps the
files distinct and upgrades ordered: `-DNPP_DEB_REVISION=1~jammy1` for CPack,
`--build-arg DEB_REVISION=1~jammy1` for Docker, or a `dch` entry for debhelper.

---

## Flatpak

`linux/packaging/flatpak/org.notepad_plus_plus.NotepadPlusPlusGtk.yml` builds
the working tree against the GNOME 51 runtime (which provides GTK 3; the
port bundles Scintilla, Lexilla, Boost.Regex, pugixml and uchardet as on
every platform).

```bash
sudo apt install flatpak flatpak-builder
linux/scripts/build-flatpak.sh --install      # dist/<app-id>.flatpak + user install
flatpak run org.notepad_plus_plus.NotepadPlusPlusGtk
```

Sandbox permissions, and why:

| Permission | Reason |
|---|---|
| `--socket=wayland`, `--socket=fallback-x11`, `--share=ipc`, `--device=dri` | the window |
| `--filesystem=host` | an editor opens files anywhere; the file dialogs use the portal, but Find in Files, Folder as Workspace, sessions, Project Panels and command-line files need real paths |
| `--talk-name=org.freedesktop.Flatpak` | Run commands and "terminal here" run programs on the host with `flatpak-spawn --host` (`src/platform/Sandbox`) |

Inside the sandbox the settings live in
`~/.var/app/<app-id>/config/notepad-plus-plus-gtk/` and user plugins in
`~/.var/app/<app-id>/data/notepad-plus-plus-gtk/plugins/` (they run against
the runtime's libraries; see `docs/PLUGINS.md` §8). Opening documents and
URLs, Trash and file monitoring work through GIO and the portals.

**Flathub.** Flathub requires an application ID under a domain the
publisher controls (e.g. `io.github.<user>.NotepadPlusPlusGtk`). Build with
`NPP_APP_ID=io.github.<user>.NotepadPlusPlusGtk linux/scripts/build-flatpak.sh`
(the script derives the manifest; CMake takes `-DNPP_APP_ID`), and in the
Flathub repository replace the `dir` source by a `git` source pinned to a
tag. Set `-DNPP_PORT_URL=<your repository>` so the AppStream data names the
port's own repository and bug tracker. `--filesystem=host` and
`--talk-name=org.freedesktop.Flatpak` must be justified in the submission
(both are accepted for code editors).

## Snap

The recipe is `linux/packaging/snap/snapcraft.yaml.in` (core24, the `gnome`
extension for GTK 3, strict confinement). snapcraft builds in LXD or
Multipass and only sees its project directory, so
`linux/scripts/build-snap.sh` assembles one in `dist/snap-project/` (the
recipe with the version from `resource.h`, the committed tree as a tarball,
the icon) and runs `snapcraft pack`:

```bash
sudo snap install snapcraft --classic && sudo snap install lxd && sudo lxd init --auto
linux/scripts/build-snap.sh
sudo snap install --dangerous dist/notepad-plus-plus-gtk_*.snap
```

Confinement: the `home` and `removable-media` plugs give access to files;
a `dbus` slot lets the running editor own its application ID for the single
instance. Strict confinement cannot start programs outside the snap, so Run
commands and "terminal here" report that. User plugins go to
`~/snap/notepad-plus-plus-gtk/current/.local/share/notepad-plus-plus-gtk/plugins/`.
For the Snap Store, register the name first and raise `grade:` to `stable`.

## Architectures

The port has no architecture-specific code. CI builds and tests every
format natively on **amd64 and arm64** (GitHub's `ubuntu-24.04-arm`
runners): `.deb` (CPack and debhelper, installed on a clean container),
Flatpak (`x86_64`, `aarch64`) and snap. arm64's unsigned `char` was also
checked on amd64 by building and testing with `-funsigned-char`. Launchpad
builds the PPA for every architecture the PPA enables (amd64 and arm64 by
default, armhf / ppc64el / riscv64 if enabled).

Locally, on an arm64 machine every script works as is. On amd64, Docker can
build for arm64 with emulation (slow):

```bash
docker run --privileged --rm tonistiigi/binfmt --install arm64   # once: register qemu
PLATFORM=linux/arm64 linux/scripts/docker-build.sh deb
```

## Application ID and AppStream

`NPP_APP_ID` (CMake, default `org.notepad_plus_plus.NotepadPlusPlusGtk`) names
the D-Bus name for the single instance, the desktop file, the icons and the
metainfo file, as Flatpak and AppStream expect. The AppStream file
(`linux/packaging/notepad-plus-plus-gtk.metainfo.xml.in`) takes the version
from `resource.h` and the release date from the newest
`linux/debian/changelog` entry; the test suite validates it with
`appstreamcli validate` and the desktop file with `desktop-file-validate`
when those tools are installed (`install-build-deps.sh` installs them).
`<developer id>` is `NPP_DEVELOPER_ID`, by default the application ID
without its last part (`io.github.<user>.NotepadPlusPlusGtk` gives
`io.github.<user>`); newer `appstreamcli` releases warn
(`developer-id-missing`) without it.

---

## Installing and verifying

```bash
sudo apt install ./dist/notepad-plus-plus-gtk_8.9.8.1-1_amd64.deb   # resolves Depends
notepad-plus-plus-gtk --version
dpkg -L notepad-plus-plus-gtk          # list installed files
lintian dist/notepad-plus-plus-gtk_8.9.8.1-1_amd64.deb
sudo apt remove notepad-plus-plus-gtk
```

Use `apt install ./file.deb` rather than `dpkg -i`: apt pulls in the
dependencies.

Expected lintian output: one warning, `initial-upload-closes-no-bugs`. It
only matters for an upload into Debian itself, which needs an ITP bug
closed in the changelog.

To check `Depends:` on a pristine system:

```bash
docker run --rm -v "$PWD/dist":/dist ubuntu:24.04 bash -c \
  'apt-get update -qq && apt-get install -y -qq /dist/*.deb && notepad-plus-plus-gtk --version'
```

## Hosting an apt repository (optional)

For a team-internal repository, `reprepro` or `aptly` work. Minimal static
repo with flat layout:

```bash
mkdir -p repo && cp dist/*.deb repo/
(cd repo && dpkg-scanpackages --multiversion . /dev/null | gzip -9c > Packages.gz)
# clients: echo "deb [trusted=yes] https://your.host/repo ./" | sudo tee /etc/apt/sources.list.d/npp-gtk.list
```

Sign the repository (`Release`/`InRelease` with GPG) before using it beyond
testing.

---

## Naming

The package is called `notepad-plus-plus-gtk` and describes itself as an
*unofficial* port, because "Notepad++" is the upstream project's name.
Before publishing packages publicly under that name (a PPA, Flathub, the
Snap Store), coordinate with the upstream maintainers; the default
application ID `org.notepad_plus_plus.*` is upstream's domain and is only
right if upstream publishes the port itself. The licence is GPL-3 (version 3 only, per
`LICENSE`), so redistributed packages must offer the corresponding source;
the `.dsc` + `.orig.tar.xz` from route B satisfy that.
