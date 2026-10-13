# HelloGtk — sample plugin for the Notepad++ GTK port.
#
# Standalone, against the installed SDK (package notepad-plus-plus-gtk-dev,
# or `cmake --install` of the port):
#   cmake -S . -B build
#   cmake --build build
#   cmake --install build     # -> ~/.local/share/notepad-plus-plus-gtk/plugins/HelloGtk/HelloGtk.so
#
# Inside the port's own build it is compiled against the source tree's SDK.

cmake_minimum_required(VERSION 3.16)
project(HelloGtk LANGUAGES CXX)

if(TARGET nppgtk_sdk)
  set(_sdk nppgtk_sdk)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NPP_SDK REQUIRED IMPORTED_TARGET notepad-plus-plus-gtk-plugin)
  set(_sdk PkgConfig::NPP_SDK)
endif()

add_library(HelloGtk MODULE HelloGtk.cpp)
set_target_properties(HelloGtk PROPERTIES
  PREFIX ""                       # HelloGtk.so, named after its folder
  CXX_VISIBILITY_PRESET hidden)   # export only the plugin entry points
target_compile_features(HelloGtk PRIVATE cxx_std_17)
target_link_libraries(HelloGtk PRIVATE ${_sdk})

if(NOT TARGET nppgtk_sdk)
  # Notepad++ looks for <plugins dir>/<Name>/<Name>.so; the user's directory
  # by default, or system-wide:
  #   -DPLUGINS_DIR=$(pkg-config --variable=plugindir notepad-plus-plus-gtk-plugin)
  if(DEFINED ENV{XDG_DATA_HOME})
    set(_user_plugins "$ENV{XDG_DATA_HOME}/notepad-plus-plus-gtk/plugins")
  else()
    set(_user_plugins "$ENV{HOME}/.local/share/notepad-plus-plus-gtk/plugins")
  endif()
  set(PLUGINS_DIR "${_user_plugins}" CACHE PATH "Plugins directory to install into")
  install(TARGETS HelloGtk LIBRARY DESTINATION "${PLUGINS_DIR}/HelloGtk")
endif()
