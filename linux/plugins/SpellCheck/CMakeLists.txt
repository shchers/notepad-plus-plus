# SpellCheck — spell checking as you type (see SpellCheck.cpp).
#
# spellcheck_core holds the word finding and the Enchant dictionaries, so the
# tests use them without Notepad++. Enchant (libenchant-2) is loaded at run
# time: building needs no Enchant headers, and without it the plugin says
# what to install.

add_library(spellcheck_core STATIC SpellText.cpp Speller.cpp)
set_target_properties(spellcheck_core PROPERTIES CXX_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
target_include_directories(spellcheck_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(spellcheck_core PUBLIC PkgConfig::GTK3 ${CMAKE_DL_LIBS})
target_compile_options(spellcheck_core PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${NPP_WARNINGS_AS_ERRORS}>:-Werror>)

add_library(SpellCheck MODULE SpellCheck.cpp)
set_target_properties(SpellCheck PROPERTIES
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${NPP_BUILD_PLUGINS_DIR}/SpellCheck")
target_link_libraries(SpellCheck PRIVATE nppgtk_sdk spellcheck_core)
target_compile_options(SpellCheck PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${NPP_WARNINGS_AS_ERRORS}>:-Werror>)

install(TARGETS SpellCheck LIBRARY DESTINATION "${NPP_PLUGINS_SUBDIR}/SpellCheck")
