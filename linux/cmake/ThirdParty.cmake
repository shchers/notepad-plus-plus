# Static libraries built from upstream sources, unmodified.
# Warnings are silenced here: these are third-party trees we do not patch.
#
# Upstream portability bug: Notepad++'s copies of scintilla/include/Sci_Position.h
# and uchardet/nsCharSetProber.cpp use (u)intptr_t without including
# <stdint.h> (MSVC tolerates it, GCC/Clang do not). Until the one-line fixes
# are upstreamed, force-include <stdint.h> (propagated via INTERFACE).
add_library(npp_sci_compat INTERFACE)
target_compile_options(npp_sci_compat INTERFACE "SHELL:-include stdint.h")

# --- Scintilla (GTK backend) + Notepad++ Boost regex search -----------------
file(GLOB SCI_CORE_SRC CONFIGURE_DEPENDS "${SCI_DIR}/src/*.cxx")
set(SCI_GTK_SRC
  "${SCI_DIR}/gtk/PlatGTK.cxx"
  "${SCI_DIR}/gtk/ScintillaGTK.cxx"
  "${SCI_DIR}/gtk/ScintillaGTKAccessible.cxx"
  "${SCI_DIR}/gtk/scintilla-marshal.c")
# Same engine Notepad++ uses on Windows (SCI_OWNREGEX), so regex search
# semantics stay identical across platforms.
set(SCI_BREGEX_SRC
  "${BREGEX_DIR}/BoostRegExSearch.cxx"
  "${BREGEX_DIR}/UTF8DocumentIterator.cxx")

add_library(npp_scintilla STATIC ${SCI_CORE_SRC} ${SCI_GTK_SRC} ${SCI_BREGEX_SRC})
target_include_directories(npp_scintilla
  PUBLIC  "${SCI_DIR}/include"
  PRIVATE "${SCI_DIR}/src" "${BREGEX_DIR}")
target_compile_definitions(npp_scintilla
  PUBLIC  GTK
  PRIVATE SCI_OWNREGEX BOOST_REGEX_STANDALONE $<IF:$<CONFIG:Debug>,DEBUG,NDEBUG>)
target_compile_options(npp_scintilla PRIVATE -w)
target_link_libraries(npp_scintilla PUBLIC PkgConfig::GTK3 npp_sci_compat)

# --- Lexilla ----------------------------------------------------------------
file(GLOB LEX_SRC CONFIGURE_DEPENDS
  "${LEX_DIR}/lexers/*.cxx"
  "${LEX_DIR}/lexlib/*.cxx")
add_library(npp_lexilla STATIC ${LEX_SRC} "${LEX_DIR}/src/Lexilla.cxx")
target_include_directories(npp_lexilla
  PUBLIC  "${LEX_DIR}/include"
  PRIVATE "${LEX_DIR}/lexlib" "${SCI_DIR}/include")
target_compile_options(npp_lexilla PRIVATE -w)
target_link_libraries(npp_lexilla PUBLIC npp_sci_compat)
# Notepad++'s UDL lexer includes <windows.h> without using it; give that one
# file an empty header instead of patching it.
set_source_files_properties("${LEX_DIR}/lexers/LexUser.cxx" PROPERTIES
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/compat/lexuser")

# --- pugixml (Notepad++'s XML parser for langs/stylers/config) --------------
add_library(npp_pugixml STATIC "${NPP_SRC}/pugixml/pugixml.cpp")
target_include_directories(npp_pugixml PUBLIC "${NPP_SRC}/pugixml")
target_compile_options(npp_pugixml PRIVATE -w)

# --- uchardet (Notepad++'s charset detector) --------------------------------
file(GLOB UCHARDET_SRC CONFIGURE_DEPENDS "${NPP_SRC}/uchardet/*.cpp")
add_library(npp_uchardet STATIC ${UCHARDET_SRC})
target_include_directories(npp_uchardet PUBLIC "${NPP_SRC}/uchardet")
target_link_libraries(npp_uchardet PRIVATE npp_sci_compat) # same missing <stdint.h> issue
target_compile_options(npp_uchardet PRIVATE -w)
