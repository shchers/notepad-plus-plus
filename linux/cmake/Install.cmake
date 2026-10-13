# Install layout (FHS):
#   bin/notepad-plus-plus-gtk
#   share/notepad-plus-plus-gtk/{langs.model.xml,stylers.model.xml,themes/,
#                                nativeLang/,userDefineLangs/,functionList/,
#                                autoCompletion/}
#   share/applications/<app-id>.desktop
#   share/metainfo/<app-id>.metainfo.xml
#   share/icons/hicolor/<size>/apps/<app-id>.png
#   share/man/man1/notepad-plus-plus-gtk.1.gz
#   share/doc/notepad-plus-plus-gtk/copyright
#   include/notepad-plus-plus-gtk/                 plugin SDK headers   } component
#   <libdir>/pkgconfig/notepad-plus-plus-gtk-plugin.pc                  } "sdk"

set(_pkg "${CMAKE_CURRENT_SOURCE_DIR}/packaging")

install(TARGETS ${NPP_EXE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES "${NPP_SRC}/langs.model.xml" "${NPP_SRC}/stylers.model.xml"
        DESTINATION ${NPP_DATA_SUBDIR})
install(DIRECTORY "${NPP_ROOT}/PowerEditor/installer/themes/"
        DESTINATION ${NPP_DATA_SUBDIR}/themes
        FILES_MATCHING PATTERN "*.xml")
install(DIRECTORY "${NPP_ROOT}/PowerEditor/installer/nativeLang/"
        DESTINATION ${NPP_DATA_SUBDIR}/nativeLang
        FILES_MATCHING PATTERN "*.xml")
install(DIRECTORY "${NPP_ROOT}/PowerEditor/installer/functionList/"
        DESTINATION ${NPP_DATA_SUBDIR}/functionList
        FILES_MATCHING PATTERN "*.xml")
install(DIRECTORY "${NPP_ROOT}/PowerEditor/installer/APIs/"
        DESTINATION ${NPP_DATA_SUBDIR}/autoCompletion
        FILES_MATCHING PATTERN "*.xml")
install(DIRECTORY "${NPP_ROOT}/PowerEditor/bin/userDefineLangs/"
        DESTINATION ${NPP_DATA_SUBDIR}/userDefineLangs
        FILES_MATCHING PATTERN "*.xml")

configure_file("${_pkg}/notepad-plus-plus-gtk.desktop.in"
               "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.desktop" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# AppStream metadata (software centres, Flathub, Snap Store).
configure_file("${_pkg}/notepad-plus-plus-gtk.metainfo.xml.in"
               "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.metainfo.xml" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.metainfo.xml"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Icons are named after the application ID, as Flatpak requires.
file(GLOB _icon_sizes RELATIVE "${_pkg}/icons/hicolor" "${_pkg}/icons/hicolor/*")
foreach(_size IN LISTS _icon_sizes)
  install(FILES "${_pkg}/icons/hicolor/${_size}/apps/${NPP_EXE_NAME}.png"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}/apps
          RENAME "${NPP_APP_ID}.png")
endforeach()

# Debian policy wants compressed man pages (lintian: manpage-not-compressed).
set(_man_gz "${CMAKE_BINARY_DIR}/${NPP_EXE_NAME}.1.gz")
add_custom_command(OUTPUT "${_man_gz}"
  COMMAND gzip -9n -c "${_pkg}/${NPP_EXE_NAME}.1" > "${_man_gz}"
  DEPENDS "${_pkg}/${NPP_EXE_NAME}.1"
  COMMENT "Compressing man page" VERBATIM)
add_custom_target(nppgtk_man ALL DEPENDS "${_man_gz}")
install(FILES "${_man_gz}" DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

install(FILES "${_pkg}/copyright" DESTINATION ${CMAKE_INSTALL_DOCDIR})

# One changelog for both packaging routes: debian/changelog. Debian policy
# requires it as changelog.Debian.gz in the doc dir of non-native packages.
set(_chlog_gz "${CMAKE_BINARY_DIR}/changelog.Debian.gz")
add_custom_command(OUTPUT "${_chlog_gz}"
  COMMAND gzip -9n -c "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" > "${_chlog_gz}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog"
  COMMENT "Compressing Debian changelog" VERBATIM)
add_custom_target(nppgtk_changelog ALL DEPENDS "${_chlog_gz}")
install(FILES "${_chlog_gz}" DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Plugin SDK: the port's shims and upstream's plugin headers side by side in
# one directory, as a plugin includes them (<windows.h>, "PluginInterface.h").
# System-wide plugins go to <libdir>/notepad-plus-plus-gtk/plugins/<Name>/
# (pkg-config --variable=plugindir); nothing is installed there by default.
set(_sdk_dir ${CMAKE_INSTALL_INCLUDEDIR}/${NPP_EXE_NAME})
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/windows.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/tchar.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/NppGtk.h"
  "${NPP_SRC}/MISC/PluginsManager/PluginInterface.h"
  "${NPP_SRC}/MISC/PluginsManager/Notepad_plus_msgs.h"
  "${NPP_SRC}/WinControls/DockingWnd/Docking.h"
  "${NPP_SRC}/WinControls/DockingWnd/dockingResource.h"
  "${NPP_SRC}/menuCmdID.h"
  "${SCI_DIR}/include/Scintilla.h"
  "${SCI_DIR}/include/Sci_Position.h"
  "${SCI_DIR}/include/ScintillaWidget.h"
  DESTINATION ${_sdk_dir} COMPONENT sdk)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/"
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples COMPONENT sdk)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sdk/notepad-plus-plus-gtk-plugin.pc.in"
               "${CMAKE_BINARY_DIR}/notepad-plus-plus-gtk-plugin.pc" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/notepad-plus-plus-gtk-plugin.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT sdk)
