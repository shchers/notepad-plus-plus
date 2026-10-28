# Notepad++ native Linux port (GTK3) — build entry point.
#
# Lives in <repo>/linux and builds against the *unmodified* upstream tree:
#   ../scintilla   Scintilla with its GTK backend
#   ../lexilla     all lexers
#   ../boostregex  Notepad++'s Boost regex engine for Scintilla search
#   ../PowerEditor/src/{pugixml,uchardet,langs.model.xml,stylers.model.xml}
#
# Quick start:
#   cmake -S linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#   cpack --config build/CPackConfig.cmake      # -> .deb

cmake_minimum_required(VERSION 3.22)

get_filename_component(NPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(NPP_SRC   "${NPP_ROOT}/PowerEditor/src")
set(SCI_DIR   "${NPP_ROOT}/scintilla")
set(LEX_DIR   "${NPP_ROOT}/lexilla")
set(BREGEX_DIR "${NPP_ROOT}/boostregex")

# Take the version straight from upstream so packages track the source tree.
file(STRINGS "${NPP_SRC}/resource.h" _npp_ver_line REGEX "#define VERSION_PRODUCT_VALUE")
string(REGEX MATCH "([0-9]+(\\.[0-9]+)+)" NPP_VERSION "${_npp_ver_line}")
if(NOT NPP_VERSION)
  message(FATAL_ERROR "Could not read VERSION_PRODUCT_VALUE from ${NPP_SRC}/resource.h")
endif()
string(REGEX MATCHALL "[0-9]+" _ver_parts "${NPP_VERSION}")
list(SUBLIST _ver_parts 0 3 _ver_parts3)
list(JOIN _ver_parts3 "." NPP_VERSION3)

# Scintilla/Lexilla version.txt hold e.g. "566" -> "5.6.6".
foreach(_comp SCINTILLA LEXILLA)
  string(TOLOWER ${_comp} _dir)
  file(READ "${NPP_ROOT}/${_dir}/version.txt" _v)
  string(STRIP "${_v}" _v)
  string(REGEX REPLACE "^([0-9])([0-9])([0-9]+)$" "\\1.\\2.\\3" NPP_${_comp}_VERSION "${_v}")
endforeach()

project(notepad-plus-plus-gtk VERSION ${NPP_VERSION3} LANGUAGES C CXX)

# The application ID names the D-Bus name (single instance), the desktop
# file, the icons and the AppStream metainfo, as Flatpak and AppStream
# expect. A distribution publishing the port (Flathub needs an ID under a
# domain the publisher controls, e.g. io.github.<user>.NotepadPlusPlusGtk)
# sets it with -DNPP_APP_ID=...
set(NPP_APP_ID "org.notepad_plus_plus.NotepadPlusPlusGtk" CACHE STRING "Application ID (reverse DNS)")
if(NOT NPP_APP_ID MATCHES "^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_-]*)+$")
  message(FATAL_ERROR "NPP_APP_ID '${NPP_APP_ID}' is not a valid reverse-DNS application ID")
endif()
# AppStream <developer id>: reverse DNS of the publisher, by default the
# application ID without its last part (io.github.<user>.App -> io.github.<user>).
string(REGEX REPLACE "\\.[^.]+$" "" _npp_dev_id "${NPP_APP_ID}")
set(NPP_DEVELOPER_ID "${_npp_dev_id}" CACHE STRING "AppStream developer ID (reverse DNS)")
set(NPP_EXE_NAME "notepad-plus-plus-gtk")
# Where the port itself is published. Bug reports for the port must not go
# to upstream's tracker, so the metainfo names a repository only when set.
set(NPP_PORT_URL "" CACHE STRING "Repository of the port (AppStream vcs-browser / bugtracker), e.g. https://github.com/<you>/notepad-plus-plus")
set(NPP_METAINFO_PORT_URLS "")
if(NPP_PORT_URL)
  string(REGEX REPLACE "/+$" "" _port_url "${NPP_PORT_URL}")
  set(NPP_METAINFO_PORT_URLS "\n  <url type=\"vcs-browser\">${_port_url}</url>\n  <url type=\"bugtracker\">${_port_url}/issues</url>")
endif()

# Release date of this version, from the newest debian/changelog entry
# (AppStream <release date=...>).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" _chlog_sig REGEX "^ -- .*[0-9][0-9]:[0-9][0-9]:[0-9][0-9]" LIMIT_COUNT 1)
string(REGEX MATCH "([0-9]+) ([A-Z][a-z][a-z]) ([0-9][0-9][0-9][0-9])" _d "${_chlog_sig}")
set(_months Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec)
list(FIND _months "${CMAKE_MATCH_2}" _m)
math(EXPR _m "${_m} + 1")
if(_m LESS 10)
  set(_m "0${_m}")
endif()
set(_day "${CMAKE_MATCH_1}")
if(_day LESS 10)
  set(_day "0${_day}")
endif()
set(NPP_RELEASE_DATE "${CMAKE_MATCH_3}-${_m}-${_day}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Runtime data is looked up relative to the executable (../share/<name>),
# so the build tree mirrors the install layout and runs without installing.
set(NPP_DATA_SUBDIR "${CMAKE_INSTALL_DATADIR}/${NPP_EXE_NAME}")
set(NPP_BUILD_DATA_DIR "${CMAKE_BINARY_DIR}/${NPP_DATA_SUBDIR}")
# System-wide plugins: <prefix>/<libdir>/<name>/plugins/<Plugin>/<Plugin>.so
set(NPP_PLUGINS_SUBDIR "${CMAKE_INSTALL_LIBDIR}/${NPP_EXE_NAME}/plugins")

# Plugin SDK (linux/sdk): the port's <windows.h> shim first, then upstream's
# plugin headers, used unmodified.
set(NPP_SDK_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include"
  "${NPP_SRC}/MISC/PluginsManager"
  "${NPP_SRC}/WinControls/DockingWnd"
  "${NPP_SRC}"
  "${SCI_DIR}/include")

option(NPP_BUILD_TESTS "Build the headless smoke tests" ON)
option(NPP_WARNINGS_AS_ERRORS "Treat warnings in port code as errors" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22 gmodule-no-export-2.0)

include(cmake/ThirdParty.cmake)
include(cmake/GenerateLangTable.cmake)
include(cmake/GenerateCommandTables.cmake)

# What a plugin compiles against (linux/sdk/examples/HelloGtk uses it in-tree;
# installed, the same comes from pkg-config notepad-plus-plus-gtk-plugin).
add_library(nppgtk_sdk INTERFACE)
target_include_directories(nppgtk_sdk INTERFACE ${NPP_SDK_INCLUDE_DIRS})
target_link_libraries(nppgtk_sdk INTERFACE PkgConfig::GTK3)

add_subdirectory(src)
# Plugins bundled with the editor (linux/plugins).
option(NPP_BUILD_BUNDLED_PLUGINS "Build and install the bundled plugins (Remote Files)" ON)
if(NPP_BUILD_BUNDLED_PLUGINS)
  find_package(Threads REQUIRED)
  add_subdirectory(plugins)
endif()
if(NPP_BUILD_TESTS)
  enable_testing()
  # The SDK example is built (and loaded by the app_launch test) to keep it working.
  set(NPP_EXAMPLE_PLUGINS_DIR "${CMAKE_BINARY_DIR}/example-plugins")
  add_subdirectory(sdk/examples/HelloGtk)
  set_target_properties(HelloGtk PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${NPP_EXAMPLE_PLUGINS_DIR}/HelloGtk")
  target_compile_options(HelloGtk PRIVATE -Wall -Wextra -Wpedantic)
  add_subdirectory(tests)
endif()

include(cmake/Install.cmake)
include(cmake/Packaging.cmake)

message(STATUS "Notepad++ GTK port ${NPP_VERSION} (upstream root: ${NPP_ROOT})")
