# Plugins shipped with the port. Each is built like a third-party plugin
# (the SDK headers, GTK, the C/C++ runtime) and lands where Notepad++ looks
# for system-wide plugins, in the build tree as in the install:
#   <libdir>/notepad-plus-plus-gtk/plugins/<Name>/<Name>.so

set(NPP_BUILD_PLUGINS_DIR "${CMAKE_BINARY_DIR}/${NPP_PLUGINS_SUBDIR}")

add_subdirectory(NppRemote)
add_subdirectory(SpellCheck)
