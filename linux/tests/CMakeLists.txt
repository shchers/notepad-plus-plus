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

add_executable(nppgtk_ui_tests ui_test.cpp)
target_link_libraries(nppgtk_ui_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_ui_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_ui_tests nppgtk_data)

find_program(XVFB_RUN xvfb-run)
set(_wrap "")
if(XVFB_RUN AND NOT DEFINED ENV{DISPLAY} AND NOT DEFINED ENV{WAYLAND_DISPLAY})
  set(_wrap ${XVFB_RUN} -a)
endif()

add_test(NAME unit_and_widget COMMAND ${_wrap} $<TARGET_FILE:nppgtk_tests>)
add_test(NAME documents_and_sessions COMMAND ${_wrap} $<TARGET_FILE:nppgtk_session_tests>)
add_test(NAME search_shortcuts_i18n COMMAND ${_wrap} $<TARGET_FILE:nppgtk_phase3_tests>)
add_test(NAME dialogs_and_commands COMMAND ${_wrap} $<TARGET_FILE:nppgtk_ui_tests>)

# Launch the real application on a sample file and let it quit on its own.
# -multiInst: never hand the file to a Notepad++ already running on the desktop.
add_test(NAME app_launch
  COMMAND ${_wrap} $<TARGET_FILE:${NPP_EXE_NAME}> -multiInst --exit-after=1500 -n3 "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.cpp")
add_test(NAME single_instance
  COMMAND ${_wrap} "${CMAKE_CURRENT_SOURCE_DIR}/single_instance.sh" $<TARGET_FILE:${NPP_EXE_NAME}>)
set_tests_properties(single_instance PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "NO_AT_BRIDGE=1")
add_test(NAME app_version COMMAND $<TARGET_FILE:${NPP_EXE_NAME}> --version)
set_tests_properties(app_version PROPERTIES PASS_REGULAR_EXPRESSION "${NPP_VERSION}")

set(_env "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config" "NO_AT_BRIDGE=1")
set_tests_properties(unit_and_widget documents_and_sessions search_shortcuts_i18n dialogs_and_commands app_launch PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 60)
