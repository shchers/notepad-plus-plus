# Packaging the GTK port as a .deb

There are two routes. Both produce `notepad-plus-plus-gtk_<version>-<rev>_<arch>.deb`
with the same file layout, and both are lintian-clean.

| Route | Tooling | Use it for |
|-------|---------|------------|
| **A. CPack** | `cpack` from the CMake build | Quick local packages, CI artifacts, GitHub releases |
| **B. debhelper** | `dpkg-buildpackage` + `linux/debian/` | A PPA, a Debian/Ubuntu archive, reproducible distro builds, `-dbgsym` packages |

All commands run from the repository root.

---

## What the package contains

```
/usr/bin/notepad-plus-plus-gtk
/usr/share/notepad-plus-plus-gtk/langs.model.xml        <- upstream, unmodified
/usr/share/notepad-plus-plus-gtk/stylers.model.xml      <- upstream, unmodified
/usr/share/notepad-plus-plus-gtk/themes/*.xml           <- upstream PowerEditor/installer/themes
/usr/share/applications/org.notepad_plus_plus.NotepadPlusPlusGtk.desktop
/usr/share/icons/hicolor/{16..256}/apps/notepad-plus-plus-gtk.png
/usr/share/man/man1/notepad-plus-plus-gtk.1.gz
/usr/share/doc/notepad-plus-plus-gtk/{copyright,changelog.Debian.gz}
```

`Depends:` is computed from the binary by `dpkg-shlibdeps`, so it is right
for whichever release you build on. On Ubuntu 24.04 it resolves to GTK 3,
GLib, Pango, Cairo, ATK, GDK-Pixbuf, libstdc++ and libc. Scintilla, Lexilla,
Boost.Regex, pugixml and uchardet are linked statically, as on Windows,
because Notepad++ ships modified copies of them.

`Recommends:` adds `adwaita-icon-theme`, `fonts-dejavu-core` and
`librsvg2-common` (toolbar icons and a good default monospace font).

---

## Route A — CPack (fast)

```bash
cmake -S linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -B build/pkg
```

Result: `build/pkg/notepad-plus-plus-gtk_8.9.8.1-1_amd64.deb`.
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
  `DEB_BUILD_OPTIONS=nocheck DEB_BUILD_PROFILES=nocheck` skips them.
- `rules`: `dh $@ --sourcedirectory=linux --buildsystem=cmake+ninja` with
  `hardening=+all`; tests run through `dh_auto_test` (CTest, which uses
  `xvfb-run` when there is no display).
- `changelog`: the single source of the Debian version. CMake warns at
  configure time if it lags behind upstream's `resource.h`.
- `copyright`: DEP-5, covering Notepad++ (GPL-3), Scintilla/Lexilla, Boost,
  pugixml (MIT) and uchardet.

### Source package / PPA

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

For a PPA upload (Launchpad builds from source only):

```bash
export DEBFULLNAME="Your Name" DEBEMAIL="you@example.org"
(cd linux && dch -v 8.9.8.1-1~ppa1~noble1 -D noble "PPA build for Ubuntu 24.04")
git commit -am "debian: PPA build"

cp -rL linux/debian debian
# first upload of this upstream version only; otherwise reuse the existing file
[ -f ../notepad-plus-plus-gtk_8.9.8.1.orig.tar.xz ] || \
    git archive --format=tar --prefix=notepad-plus-plus-gtk-8.9.8.1/ HEAD \
        | xz -T0 > ../notepad-plus-plus-gtk_8.9.8.1.orig.tar.xz
dpkg-buildpackage -S -sa -k<YOUR_GPG_KEY_ID>      # signed, source-only
dput ppa:<you>/<ppa> ../notepad-plus-plus-gtk_8.9.8.1-1~ppa1~noble1_source.changes
rm -rf debian
```

Use `-sa` (include the orig) for the first upload of an upstream version and
`-sd` afterwards.

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
Before publishing packages publicly under that name, coordinate with the
upstream maintainers. The licence is GPL-3 (version 3 only, per
`LICENSE`), so redistributed packages must offer the corresponding source;
the `.dsc` + `.orig.tar.xz` from route B satisfy that.
