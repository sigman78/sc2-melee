# Feature checks feeding the generated config header (docs/c-making.md §4).
#
# Replaces the hand-rolled probes in build/unix/config_functions plus the
# @VAR@ substitution into config_unix.h.in / config_win.h.in. Only the HAVE_*
# macros the tree actually consults are checked; the old build also probed
# headers and types that every modern compiler provides, and those are dropped
# rather than ported.

include(CheckSymbolExists)
include(CheckTypeSize)

set(CMAKE_REQUIRED_QUIET ON)

check_symbol_exists(readdir_r "dirent.h" HAVE_READDIR_R)
check_symbol_exists(setenv "stdlib.h" HAVE_SETENV)
check_symbol_exists(strupr "string.h" HAVE_STRUPR)
# Not "HAVE_STRCASECMP": that name conflicts with SDL's own config.
check_symbol_exists(strcasecmp "strings.h" HAVE_STRCASECMP_UQM)
check_symbol_exists(stricmp "string.h" HAVE_STRICMP)
check_symbol_exists(iswgraph "wctype.h" HAVE_ISWGRAPH)
check_symbol_exists(getopt_long "getopt.h" HAVE_GETOPT_LONG)

check_type_size(wchar_t SIZEOF_WCHAR_T LANGUAGE C)
set(HAVE_WCHAR_T ${HAVE_SIZEOF_WCHAR_T})
set(CMAKE_EXTRA_INCLUDE_FILES wchar.h)
check_type_size(wint_t SIZEOF_WINT_T LANGUAGE C)
set(HAVE_WINT_T ${HAVE_SIZEOF_WINT_T})
unset(CMAKE_EXTRA_INCLUDE_FILES)
check_type_size(_Bool SIZEOF__BOOL LANGUAGE C)
set(HAVE__BOOL ${HAVE_SIZEOF__BOOL})

unset(CMAKE_REQUIRED_QUIET)

if(NOT HAVE_STRICMP AND NOT HAVE_STRCASECMP_UQM)
	message(FATAL_ERROR "neither stricmp() nor strcasecmp() is available")
endif()

# Every live target is little-endian, but endian_uqm.h consumes the macro and
# CMake already knows the answer.
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
	set(WORDS_BIGENDIAN ON)
endif()

# --- Paths -------------------------------------------------------------------
if(EMSCRIPTEN)
	# MEMFS mount point that --preload-file populates; see CMakeLists.txt.
	set(UQM_CONTENTDIR_RESOLVED "/content/")
elseif(UQM_CONTENTDIR STREQUAL "")
	# Dev default: point at the content tree in this checkout, so a binary in
	# build/<preset>/ runs with no arguments. prepareContentDir() tries this
	# first, then "./content" and the other usual places, so an installed or
	# relocated build still behaves. Packaging overrides it.
	set(UQM_CONTENTDIR_RESOLVED "${CMAKE_CURRENT_SOURCE_DIR}/sc2/content/")
else()
	set(UQM_CONTENTDIR_RESOLVED "${UQM_CONTENTDIR}")
endif()

if(EMSCRIPTEN)
	# An IDBFS mount, created by preRun in cmake/uqm-shell.html and synced
	# back to IndexedDB so saves survive a reload. Deliberately not under
	# $HOME: getHomeDir() falls back to getpwuid(), which is meaningless here.
	set(UQM_USERDIR "/uqm/")
	set(UQM_MELEEDIR "/uqm/teams/")
	set(UQM_SAVEDIR "/uqm/save/")
elseif(WIN32)
	set(UQM_USERDIR "%APPDATA%/uqm/")
	set(UQM_MELEEDIR "%UQM_CONFIG_DIR%/teams/")
	set(UQM_SAVEDIR "%UQM_CONFIG_DIR%/save/")
else()
	set(UQM_USERDIR "~/.uqm/")
	set(UQM_MELEEDIR "\${UQM_CONFIG_DIR}/teams/")
	set(UQM_SAVEDIR "\${UQM_CONFIG_DIR}/save/")
endif()

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/sc2/src/uqm_config.h.in"
	"${UQM_GENERATED_DIR}/uqm_config.h"
	@ONLY
)
