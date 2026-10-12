# Install layout (FHS):
#   bin/notepad-plus-plus-gtk
#   share/notepad-plus-plus-gtk/{langs.model.xml,stylers.model.xml,themes/,
#                                nativeLang/,userDefineLangs/,functionList/}
#   share/applications/<app-id>.desktop
#   share/icons/hicolor/<size>/apps/notepad-plus-plus-gtk.png
#   share/man/man1/notepad-plus-plus-gtk.1.gz
#   share/doc/notepad-plus-plus-gtk/copyright

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
install(DIRECTORY "${NPP_ROOT}/PowerEditor/bin/userDefineLangs/"
        DESTINATION ${NPP_DATA_SUBDIR}/userDefineLangs
        FILES_MATCHING PATTERN "*.xml")

configure_file("${_pkg}/notepad-plus-plus-gtk.desktop.in"
               "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.desktop" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(DIRECTORY "${_pkg}/icons/hicolor" DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)

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
