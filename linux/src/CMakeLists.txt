# Port code. Split into a library (everything testable) and the thin app.
#
#   platform/  OS services that replace Win32 calls (paths, encodings,
#              file I/O and monitoring, settings)
#   core/      UI-free Notepad++ model code (languages, styles, buffers,
#              sessions, recent files, config.xml import)
#   ui/        GTK3 widgets (editor view, main window)

add_library(nppgtk_core STATIC
  platform/FileIO.cpp
  platform/FileMonitor.cpp
  platform/Paths.cpp
  platform/Settings.cpp
  platform/TextCodec.cpp
  core/FileManager.cpp
  core/LanguageCatalog.cpp
  core/LexerSetup.cpp
  core/NppConfig.cpp
  core/RecentFiles.cpp
  core/Session.cpp
  core/StyleCatalog.cpp
  ui/EditorView.cpp
  ui/FindBar.cpp
  ui/MainWindow.cpp
  ui/MainWindowDocs.cpp
)
target_include_directories(nppgtk_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${NPP_GENERATED_DIR}")
target_compile_definitions(nppgtk_core PUBLIC
  NPP_VERSION="${NPP_VERSION}"
  NPP_APP_ID="${NPP_APP_ID}"
  NPP_EXE_NAME="${NPP_EXE_NAME}"
  NPP_DATA_SUBDIR="${NPP_DATA_SUBDIR}"
  NPP_SCINTILLA_VERSION="${NPP_SCINTILLA_VERSION}"
  NPP_LEXILLA_VERSION="${NPP_LEXILLA_VERSION}")
target_link_libraries(nppgtk_core PUBLIC
  npp_scintilla npp_lexilla npp_pugixml npp_uchardet PkgConfig::GTK3)
target_compile_options(nppgtk_core PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${NPP_WARNINGS_AS_ERRORS}>:-Werror>)

add_executable(${NPP_EXE_NAME} main.cpp)
target_link_libraries(${NPP_EXE_NAME} PRIVATE nppgtk_core)
target_compile_options(${NPP_EXE_NAME} PRIVATE -Wall -Wextra -Wpedantic)

# Mirror the install layout in the build tree so build/bin/<exe> finds its
# data in build/share/<exe>/ without installing.
set(_data_files
  "${NPP_SRC}/langs.model.xml"
  "${NPP_SRC}/stylers.model.xml")
add_custom_target(nppgtk_data ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${NPP_BUILD_DATA_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_data_files} "${NPP_BUILD_DATA_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NPP_ROOT}/PowerEditor/installer/themes" "${NPP_BUILD_DATA_DIR}/themes"
  COMMENT "Staging Notepad++ language and style definitions"
  VERBATIM)
add_dependencies(${NPP_EXE_NAME} nppgtk_data)
