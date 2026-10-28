# NppRemote — remote files over FTP, SFTP and a shell link (see NppRemote.cpp).
#
# nppremote_proto holds everything but the plugin entry points and the
# panel, so the tests use the clients without Notepad++.

add_library(nppremote_proto STATIC
  RemoteUtil.cpp
  FtpClient.cpp
  SshProcess.cpp
  SftpClient.cpp
  ShellClient.cpp
  Profiles.cpp
  Session.cpp)
set_target_properties(nppremote_proto PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_include_directories(nppremote_proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nppremote_proto PUBLIC PkgConfig::GTK3 Threads::Threads)
target_compile_options(nppremote_proto PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${NPP_WARNINGS_AS_ERRORS}>:-Werror>)

add_library(NppRemote MODULE NppRemote.cpp Panel.cpp)
set_target_properties(NppRemote PROPERTIES
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${NPP_BUILD_PLUGINS_DIR}/NppRemote")
target_link_libraries(NppRemote PRIVATE nppgtk_sdk nppremote_proto)
target_compile_options(NppRemote PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${NPP_WARNINGS_AS_ERRORS}>:-Werror>)

install(TARGETS NppRemote LIBRARY DESTINATION "${NPP_PLUGINS_SUBDIR}/NppRemote")
