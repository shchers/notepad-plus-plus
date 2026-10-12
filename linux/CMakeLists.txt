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

set(NPP_APP_ID   "org.notepad_plus_plus.NotepadPlusPlusGtk")
set(NPP_EXE_NAME "notepad-plus-plus-gtk")

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

option(NPP_BUILD_TESTS "Build the headless smoke tests" ON)
option(NPP_WARNINGS_AS_ERRORS "Treat warnings in port code as errors" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22 gmodule-no-export-2.0)

include(cmake/ThirdParty.cmake)
include(cmake/GenerateLangTable.cmake)

add_subdirectory(src)
if(NPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Install.cmake)
include(cmake/Packaging.cmake)

message(STATUS "Notepad++ GTK port ${NPP_VERSION} (upstream root: ${NPP_ROOT})")
