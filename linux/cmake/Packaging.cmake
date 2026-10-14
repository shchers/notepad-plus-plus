# Binary .deb via CPack (fast path; see docs/PACKAGING.md for the full
# debian/ source-package route with dpkg-buildpackage).
#
#   cmake -S linux -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build
#   cpack --config build/CPackConfig.cmake -B build/pkg

set(NPP_DEB_REVISION "1" CACHE STRING "Debian revision appended to the upstream version")

# debian/changelog is edited by hand (dch); warn when an upstream merge bumped
# resource.h but the changelog was not updated, so both .deb routes agree.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" _chlog_head LIMIT_COUNT 1)
string(REGEX MATCH "\\(([^)-]+)" _m "${_chlog_head}")
if(NOT CMAKE_MATCH_1 STREQUAL NPP_VERSION)
  message(WARNING "linux/debian/changelog is at '${CMAKE_MATCH_1}' but upstream is ${NPP_VERSION}. "
                  "Run: dch -v ${NPP_VERSION}-1 \"New upstream release\" (from the package root).")
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "${NPP_EXE_NAME}")
set(CPACK_PACKAGE_VERSION "${NPP_VERSION}")
set(CPACK_PACKAGE_VENDOR "Notepad++ Linux port")
set(CPACK_PACKAGE_CONTACT "Sergii Shcherbakov <shchers@gmail.com>"
    CACHE STRING "Maintainer field for the .deb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Notepad++ source code editor - native GTK 3 port (preview)")
set(CPACK_PACKAGE_DESCRIPTION
"Unofficial native Linux build of the Notepad++ source tree using the GTK 3
backend of Scintilla: tabbed editing in two views, syntax highlighting for
all Notepad++ languages via Lexilla, Notepad++ colour themes, encoding
detection, Boost-regex find/replace, macros, sessions, docking panels and
plugins built from source with the included SDK
(pkg-config notepad-plus-plus-gtk-plugin), auto-completion, smart highlighting
and Notepad++'s Preferences.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://notepad-plus-plus.org/")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT) # <name>_<version>-<rev>_<arch>.deb
set(CPACK_DEBIAN_PACKAGE_RELEASE "${NPP_DEB_REVISION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "editors")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)       # Depends: computed by dpkg-shlibdeps
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "adwaita-icon-theme, fonts-dejavu-core, librsvg2-common, libvte-2.91-0")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/\\\\.git/;/obj-;/debian/\\\\.debhelper/")

include(CPack)
