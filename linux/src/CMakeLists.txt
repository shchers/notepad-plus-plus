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
  platform/Sandbox.cpp
  platform/Settings.cpp
  platform/TextCodec.cpp
  platform/Zip.cpp
  core/EditOps.cpp
  core/EditorFeatures.cpp
  core/FileManager.cpp
  core/FileSearch.cpp
  core/FunctionParser.cpp
  core/Workspace.cpp
  core/KeyCombo.cpp
  core/Macro.cpp
  core/PluginFolders.cpp
  core/SearchEngine.cpp
  core/Shortcuts.cpp
  core/Translator.cpp
  core/UserLanguage.cpp
  core/LanguageCatalog.cpp
  core/LexerSetup.cpp
  core/NppConfig.cpp
  core/RecentFiles.cpp
  core/Session.cpp
  core/StyleCatalog.cpp
  ui/EditorView.cpp
  ui/CharPanel.cpp
  ui/ClipboardHistoryPanel.cpp
  ui/Commands.cpp
  ui/DocListPanel.cpp
  ui/DocumentMap.cpp
  ui/DockManager.cpp
  ui/FindBar.cpp
  ui/FindDialog.cpp
  ui/FolderPanel.cpp
  ui/FunctionListPanel.cpp
  ui/KeyMap.cpp
  ui/MainWindow.cpp
  ui/MainWindowCommands.cpp
  ui/MainWindowDialogs.cpp
  ui/MainWindowMacros.cpp
  ui/MainWindowPanels.cpp
  ui/MainWindowPlugins.cpp
  ui/MainWindowPrefs.cpp
  ui/MainWindowPrint.cpp
  ui/PluginManager.cpp
  ui/ProjectPanel.cpp
  ui/MainWindowDocs.cpp
  ui/SearchResults.cpp
  ui/ShortcutDialog.cpp
)
target_include_directories(nppgtk_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${NPP_GENERATED_DIR}"
  "${NPP_SRC}"                       # upstream ID headers: menuCmdID.h, resource.h
  "${NPP_SRC}/ScintillaComponent"   # FindReplaceDlg_rc.h
  ${NPP_SDK_INCLUDE_DIRS})          # plugin SDK: <windows.h> shim, PluginInterface.h
target_compile_definitions(nppgtk_core PUBLIC
  NPP_VERSION="${NPP_VERSION}"
  NPP_APP_ID="${NPP_APP_ID}"
  NPP_EXE_NAME="${NPP_EXE_NAME}"
  NPP_DATA_SUBDIR="${NPP_DATA_SUBDIR}"
  NPP_PLUGINS_SUBDIR="${NPP_PLUGINS_SUBDIR}"
  NPP_SCINTILLA_VERSION="${NPP_SCINTILLA_VERSION}"
  NPP_LEXILLA_VERSION="${NPP_LEXILLA_VERSION}")
target_link_libraries(nppgtk_core PUBLIC
  npp_scintilla npp_lexilla npp_pugixml npp_uchardet PkgConfig::GTK3 ${CMAKE_DL_LIBS})
# Plugins call SendMessageW, PostMessageW and MessageBoxW in the executable
# (every executable linking the core, so the tests can load plugins too).
target_link_options(nppgtk_core INTERFACE "LINKER:--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/plugin-exports.list")
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
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NPP_ROOT}/PowerEditor/installer/nativeLang" "${NPP_BUILD_DATA_DIR}/nativeLang"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NPP_ROOT}/PowerEditor/bin/userDefineLangs" "${NPP_BUILD_DATA_DIR}/userDefineLangs"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NPP_ROOT}/PowerEditor/installer/functionList" "${NPP_BUILD_DATA_DIR}/functionList"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NPP_ROOT}/PowerEditor/installer/APIs" "${NPP_BUILD_DATA_DIR}/autoCompletion"
  COMMENT "Staging Notepad++ language, style, localization, UDL, function list and auto-completion definitions"
  VERBATIM)
add_dependencies(${NPP_EXE_NAME} nppgtk_data)
