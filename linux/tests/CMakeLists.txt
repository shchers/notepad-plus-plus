# Headless tests. GTK needs a display: when DISPLAY/WAYLAND_DISPLAY are unset
# (CI, Docker) tests run under xvfb-run if it is installed.

add_executable(nppgtk_tests smoke_test.cpp)
target_link_libraries(nppgtk_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_tests nppgtk_data)

add_executable(nppgtk_session_tests session_test.cpp)
target_link_libraries(nppgtk_session_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_session_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_session_tests nppgtk_data)

add_executable(nppgtk_phase3_tests phase3_test.cpp)
target_link_libraries(nppgtk_phase3_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_phase3_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_phase3_tests nppgtk_data)

add_executable(nppgtk_panels_tests panels_test.cpp)
target_link_libraries(nppgtk_panels_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_panels_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}"
  NPP_SOURCE_ROOT="${NPP_ROOT}")
add_dependencies(nppgtk_panels_tests nppgtk_data)

add_executable(nppgtk_prefs_tests prefs_test.cpp)
target_link_libraries(nppgtk_prefs_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_prefs_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_prefs_tests nppgtk_data)

add_executable(nppgtk_ui_tests ui_test.cpp)
target_link_libraries(nppgtk_ui_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_ui_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_ui_tests nppgtk_data)

# Plugins built like third-party ones: the SDK headers and GTK only. Each
# lands in test-plugins/<Name>/<Name>.so, the layout Notepad++ searches.
set(NPP_TEST_PLUGINS_DIR "${CMAKE_BINARY_DIR}/test-plugins")
foreach(_plugin TestPlugin NotAPlugin)
  add_library(${_plugin} MODULE plugins/${_plugin}/${_plugin}.cpp)
  set_target_properties(${_plugin} PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${NPP_TEST_PLUGINS_DIR}/${_plugin}"
    CXX_VISIBILITY_PRESET hidden)
  target_include_directories(${_plugin} PRIVATE ${NPP_SDK_INCLUDE_DIRS})
  target_link_libraries(${_plugin} PRIVATE PkgConfig::GTK3)
  target_compile_options(${_plugin} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

add_executable(nppgtk_plugins_tests plugins_test.cpp)
target_link_libraries(nppgtk_plugins_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_plugins_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}"
  NPP_TEST_PLUGINS_DIR="${NPP_TEST_PLUGINS_DIR}")
add_dependencies(nppgtk_plugins_tests nppgtk_data TestPlugin NotAPlugin)

# The bundled Remote Files plugin's protocol side: no display needed. Its
# SFTP part starts a private sshd when OpenSSH is installed.
if(TARGET nppremote_proto)
  add_executable(nppgtk_remote_tests remote_test.cpp)
  target_link_libraries(nppgtk_remote_tests PRIVATE nppremote_proto)
  target_compile_options(nppgtk_remote_tests PRIVATE -Wall -Wextra -Wpedantic)
  # The plugin itself, loaded by a live window.
  add_executable(nppgtk_remote_panel_tests remote_panel_test.cpp)
  target_link_libraries(nppgtk_remote_panel_tests PRIVATE nppgtk_core)
  target_include_directories(nppgtk_remote_panel_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/NppRemote")
  target_compile_definitions(nppgtk_remote_panel_tests PRIVATE
    NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}"
    NPP_BUNDLED_PLUGINS_DIR="${CMAKE_BINARY_DIR}/${NPP_PLUGINS_SUBDIR}")
  add_dependencies(nppgtk_remote_panel_tests nppgtk_data NppRemote)
endif()

find_program(XVFB_RUN xvfb-run)
set(_wrap "")
if(XVFB_RUN AND NOT DEFINED ENV{DISPLAY} AND NOT DEFINED ENV{WAYLAND_DISPLAY})
  set(_wrap ${XVFB_RUN} -a)
endif()

add_test(NAME unit_and_widget COMMAND ${_wrap} $<TARGET_FILE:nppgtk_tests>)
add_test(NAME documents_and_sessions COMMAND ${_wrap} $<TARGET_FILE:nppgtk_session_tests>)
add_test(NAME search_shortcuts_i18n COMMAND ${_wrap} $<TARGET_FILE:nppgtk_phase3_tests>)
add_test(NAME dialogs_and_commands COMMAND ${_wrap} $<TARGET_FILE:nppgtk_ui_tests>)
add_test(NAME panels COMMAND ${_wrap} $<TARGET_FILE:nppgtk_panels_tests>)
add_test(NAME plugins COMMAND ${_wrap} $<TARGET_FILE:nppgtk_plugins_tests>)
add_test(NAME preferences COMMAND ${_wrap} $<TARGET_FILE:nppgtk_prefs_tests>)
if(TARGET nppgtk_remote_tests)
  add_test(NAME remote_files COMMAND $<TARGET_FILE:nppgtk_remote_tests>)
  set_tests_properties(remote_files PROPERTIES TIMEOUT 120)
  add_test(NAME remote_files_panel COMMAND ${_wrap} $<TARGET_FILE:nppgtk_remote_panel_tests>)
endif()

# Launch the real application on a sample file and let it quit on its own,
# with a fresh configuration each time (a session left by an earlier run
# could otherwise stop it with a question) and the test and example plugins.
# -multiInst: never hand the file to a Notepad++ already running on the desktop.
add_test(NAME app_launch_config COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/test-config-launch")
set_tests_properties(app_launch_config PROPERTIES FIXTURES_SETUP launch_config)
add_test(NAME app_launch
  COMMAND ${_wrap} $<TARGET_FILE:${NPP_EXE_NAME}> -multiInst --exit-after=1500 -n3 "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.cpp")
add_test(NAME single_instance
  COMMAND ${_wrap} "${CMAKE_CURRENT_SOURCE_DIR}/single_instance.sh" $<TARGET_FILE:${NPP_EXE_NAME}>)
set_tests_properties(single_instance PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "NO_AT_BRIDGE=1;$<$<BOOL:${_wrap}>:GDK_BACKEND=x11>")
add_test(NAME app_version COMMAND $<TARGET_FILE:${NPP_EXE_NAME}> --version)
set_tests_properties(app_version PROPERTIES PASS_REGULAR_EXPRESSION "${NPP_VERSION}")

# Packaging metadata: what software centres, Flathub and the Snap Store read.
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME appstream_metainfo COMMAND ${APPSTREAMCLI} validate --no-net "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.metainfo.xml")
endif()
find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file COMMAND ${DESKTOP_FILE_VALIDATE} "${CMAKE_BINARY_DIR}/${NPP_APP_ID}.desktop")
endif()

# NPP_GTK_PLUGINS_DIR empty: no plugins from the developer's own directories.
set(_env "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config" "NO_AT_BRIDGE=1" "NPP_GTK_PLUGINS_DIR=")
if(_wrap)
  # Under Xvfb, keep GTK on X11: with XDG_RUNTIME_DIR set it would otherwise
  # try the desktop's default Wayland socket (wayland-0) first and run the
  # tests on the developer's real session.
  list(APPEND _env "GDK_BACKEND=x11")
endif()
set_tests_properties(unit_and_widget documents_and_sessions search_shortcuts_i18n dialogs_and_commands panels plugins preferences PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 60)
if(TARGET nppgtk_remote_panel_tests)
  set_tests_properties(remote_files_panel PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 60)
endif()
set(_launch_env ${_env})
list(TRANSFORM _launch_env REPLACE "^XDG_CONFIG_HOME=.*" "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config-launch")
set(_launch_plugins "${NPP_TEST_PLUGINS_DIR}:${NPP_EXAMPLE_PLUGINS_DIR}")
if(NPP_BUILD_BUNDLED_PLUGINS)
  string(APPEND _launch_plugins ":${CMAKE_BINARY_DIR}/${NPP_PLUGINS_SUBDIR}")
endif()
list(TRANSFORM _launch_env REPLACE "^NPP_GTK_PLUGINS_DIR=.*" "NPP_GTK_PLUGINS_DIR=${_launch_plugins}")
set_tests_properties(app_launch PROPERTIES ENVIRONMENT "${_launch_env}" TIMEOUT 60 FIXTURES_REQUIRED launch_config)
