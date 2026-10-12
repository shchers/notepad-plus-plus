# Headless tests. GTK needs a display: when DISPLAY/WAYLAND_DISPLAY are unset
# (CI, Docker) tests run under xvfb-run if it is installed.

add_executable(nppgtk_tests smoke_test.cpp)
target_link_libraries(nppgtk_tests PRIVATE nppgtk_core)
target_compile_definitions(nppgtk_tests PRIVATE
  NPP_TEST_DATA_DIR="${NPP_BUILD_DATA_DIR}")
add_dependencies(nppgtk_tests nppgtk_data)

find_program(XVFB_RUN xvfb-run)
set(_wrap "")
if(XVFB_RUN AND NOT DEFINED ENV{DISPLAY} AND NOT DEFINED ENV{WAYLAND_DISPLAY})
  set(_wrap ${XVFB_RUN} -a)
endif()

add_test(NAME unit_and_widget COMMAND ${_wrap} $<TARGET_FILE:nppgtk_tests>)

# Launch the real application on a sample file and let it quit on its own.
add_test(NAME app_launch
  COMMAND ${_wrap} $<TARGET_FILE:${NPP_EXE_NAME}> --exit-after=1500 -n3 "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.cpp")
add_test(NAME app_version COMMAND $<TARGET_FILE:${NPP_EXE_NAME}> --version)
set_tests_properties(app_version PROPERTIES PASS_REGULAR_EXPRESSION "${NPP_VERSION}")

set(_env "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config" "NO_AT_BRIDGE=1")
set_tests_properties(unit_and_widget app_launch PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 60)
