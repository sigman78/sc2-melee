cmake_minimum_required(VERSION 3.24)

# The version still lives in sc2/src/uqmversion.h, which the shell build also
# compiles. Parse it instead of duplicating it; once the shell build is gone
# (docs/c-making.md §8.5) the header becomes generated and this goes away.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sc2/src/uqmversion.h" _uqm_version_header)
foreach(_part MAJOR MINOR PATCH)
	if(NOT _uqm_version_header MATCHES "UQM_${_part}_VERSION[ \t]+([0-9]+)")
		message(FATAL_ERROR "cannot parse UQM_${_part}_VERSION from sc2/src/uqmversion.h")
	endif()
	set(_uqm_${_part} "${CMAKE_MATCH_1}")
endforeach()
unset(_uqm_version_header)

project(uqm
	VERSION ${_uqm_MAJOR}.${_uqm_MINOR}.${_uqm_PATCH}
	DESCRIPTION "The Ur-Quan Masters"
	HOMEPAGE_URL "https://sc2.sourceforge.net/"
	LANGUAGES C CXX
)
	# CXX for the green-field tree under src/ (docs/game-rewrite-plan.md).
	# sc2/ stays C and stays in CI for the whole project.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(WIN32)
	enable_language(RC)
endif()
if(APPLE)
	enable_language(OBJC)
endif()

# --- Options (docs/c-making.md §3) -------------------------------------------
# Everything the old build menu offered that can only have one sane value
# (graphics module, threadlib, mikmod, ovcodec, joystick, accel) is a constant
# now and lives in the generated config header instead.

option(UQM_NETPLAY "Build Network Supermelee support" ON)
option(UQM_ZIP "Read content straight out of .zip/.uqm archives" ON)
option(UQM_OPENAL "Build the OpenAL sound backend (runtime --sound=openal)" OFF)
option(UQM_WERROR "Treat compiler warnings as errors" OFF)
option(UQM_TESTS "Build the unit tests in tests/" ON)

# The original C game. On by default and stays in CI for the whole project --
# side-by-side comparison is the only oracle the "does it feel right"
# questions have.
#
# Turning it off leaves a tree containing only the rewrite, which is what the
# wasm deploy wants. The C game still needs threads (Starcon2Main), and on
# Emscripten that forces SDL_PTHREADS, which forces SharedArrayBuffer, which
# forces COOP/COEP headers on whoever hosts the page. The rewrite needs none
# of that, and there is no reason for it to inherit the constraint just
# because the two share a repository.
option(UQM_LEGACY "Build the original C game in sc2/src" ON)

# The content tree the rewrite's tools and tests read. The only thing src/ and
# tests/ need from outside their own trees, and it is optional: the content,
# game and browse_* tests are registered only when it exists, while sim,
# replay and engine never touch it.
set(UQM2_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sc2/content" CACHE PATH
	"UQM content tree for the rewrite's content-dependent tests and tools")
	# Its own option rather than BUILD_TESTING, which Dependencies.cmake turns
	# off in this scope to keep the fetched dependencies from building theirs.
set(UQM_CONTENTDIR "" CACHE STRING
	"Compiled-in content directory. Empty means the content tree in this checkout, so a build runs without --contentdir")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # the tree uses POSIX-isms on Unix

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(UQM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

include(Dependencies)
include(ConfigHeader)

# --- Targets -----------------------------------------------------------------
# One executable; every source directory contributes to it via target_sources().
# Vendored third-party code gets its own static library so its warnings stay out
# of the game's build log.

if(UQM_LEGACY)
	add_executable(uqm)
endif()
# A target declared without sources is a configure error, so every vendored
# library here is conditional rather than merely unused. The rewrite does not
# appear in this list: it builds its own spng and miniz from third_party/.
set(UQM_VENDORED_TARGETS)

if(UQM_LEGACY)
	add_library(uqm_miniz STATIC)
	add_library(uqm_spng STATIC)
	list(APPEND UQM_VENDORED_TARGETS uqm_miniz uqm_spng)
	add_library(uqm_uio STATIC)
	add_library(uqm_mikmod STATIC)
	add_library(uqm_stb STATIC)
	list(APPEND UQM_VENDORED_TARGETS uqm_uio uqm_mikmod uqm_stb)
	if(NOT HAVE_GETOPT_LONG)
		add_library(uqm_getopt STATIC)
		list(APPEND UQM_VENDORED_TARGETS uqm_getopt)
	endif()
endif()

# Settings shared by every target compiled from this tree.
add_library(uqm_buildsettings INTERFACE)
target_include_directories(uqm_buildsettings INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/sc2/src"
	"${UQM_GENERATED_DIR}"
)
# Build-configuration macros go on the command line, not into the generated
# config header: headers like uqm/battlecontrols.h test NETPLAY before they
# include anything at all. This is the same split the old build used between
# CCOMMONFLAGS and config_unix.h/config_win.h.
target_compile_definitions(uqm_buildsettings INTERFACE
	UQM_CMAKE_BUILD
	$<$<CONFIG:Debug>:DEBUG>
	# Constants now: the old menu's graphics/threadlib/mikmod/ovcodec/joystick
	# choices each had one sane value left (docs/c-making.md §3).
	GFXMODULE_SDL
	THREADLIB_SDL
	USE_INTERNAL_MIKMOD
	HAVE_JOYSTICK
	$<$<BOOL:${UQM_ZIP}>:HAVE_ZIP=1>
	$<$<BOOL:${UQM_OPENAL}>:HAVE_OPENAL>
	# NETPLAY_FULL comes from uqm/supermelee/netplay/netplay.h; the macro is
	# expanded lazily, exactly as the old -DNETPLAY=NETPLAY_FULL was.
	$<$<BOOL:${UQM_NETPLAY}>:NETPLAY=NETPLAY_FULL>
)
target_link_libraries(uqm_buildsettings INTERFACE uqm::SDL)

foreach(_target ${UQM_VENDORED_TARGETS})
	target_link_libraries(${_target} PRIVATE uqm_buildsettings)
endforeach()

if(UQM_LEGACY)
	target_link_libraries(uqm PRIVATE uqm_buildsettings)
	target_link_libraries(uqm PRIVATE ${UQM_VENDORED_TARGETS})
endif()
if(UQM_ZIP AND UQM_LEGACY)
	# miniz, vendored, rather than the fetched zlib: uio's whole zlib surface
	# is six decompress-only symbols and miniz is source-compatible with them
	# (sc2/src/libs/vendor/miniz/README.md). The vendored spng inflates
	# through it too, which is what left Dependencies.cmake with only SDL.
	target_link_libraries(uqm_uio PRIVATE uqm_miniz)
endif()
if(UQM_OPENAL AND UQM_LEGACY)
	target_link_libraries(uqm PRIVATE OpenAL::OpenAL)
endif()
if(WIN32 AND UQM_LEGACY)
	target_link_libraries(uqm PRIVATE user32)
	if(UQM_NETPLAY)
		target_link_libraries(uqm PRIVATE ws2_32)
	endif()
endif()
if(UNIX AND NOT EMSCRIPTEN AND UQM_LEGACY)
	target_link_libraries(uqm PRIVATE m)
endif()

if(EMSCRIPTEN AND UQM_LEGACY)
	# docs/plan.md Phase 3. The game is still threaded (Starcon2Main), so
	# pthreads are on for now; §7.6's flip is what lets them go, and with them
	# the SharedArrayBuffer requirement and the COOP/COEP headers it forces on
	# whoever hosts this.
	# -pthread itself is set in the toolchain file so the fetched
	# dependencies get it too; only link-side settings live here.
	target_link_options(uqm PRIVATE
		-pthread
		-sPTHREAD_POOL_SIZE=8
		-sALLOW_MEMORY_GROWTH=1
		-sINITIAL_MEMORY=268435456   # 256 MB; content lives in MEMFS
		-sSTACK_SIZE=1048576
		-sEXIT_RUNTIME=0
		-sASYNCIFY=0
		-sEXPORTED_RUNTIME_METHODS=FS,callMain,IDBFS
		-lidbfs.js
			# Saves and config persist to IndexedDB; see the preRun hook
			# in cmake/uqm-shell.html.
		# uio walks directories, so content is preloaded rather than fetched
		# per file. Only the base pack: the 3DO addons are another 133 MB and
		# belong behind on-demand fetching (Phase 3.3).
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/sc2/content/base@/content/base"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/sc2/content/version@/content/version"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/sc2/content/uqm.rmp@/content/uqm.rmp"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/sc2/content/uqm.key@/content/uqm.key"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/sc2/content/menu.key@/content/menu.key"
		"SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uqm-shell.html"
	)
	set_target_properties(uqm PROPERTIES
		OUTPUT_NAME uqm
		SUFFIX ".html"
	)
endif()
if(APPLE AND UQM_LEGACY)
	# log/msgbox_macosx.m (NSAlert) and darwin/SDLMain.m (NSApplication).
	target_link_libraries(uqm PRIVATE "-framework Cocoa")
endif()

# --- Warnings ----------------------------------------------------------------
# The tree is 2003-era C and is not warning-clean yet; UQM_WERROR is what CI
# turns on once it is.

if(MSVC)
	target_compile_options(uqm_buildsettings INTERFACE /utf-8)
	set(_uqm_quiet /w)
	set(_uqm_werror /WX)
	if(UQM_LEGACY)
		target_compile_options(uqm PRIVATE /W3)
	endif()
else()
	set(_uqm_quiet -w)
	set(_uqm_werror -Werror)
	if(UQM_LEGACY)
		target_compile_options(uqm PRIVATE -W -Wall)
	endif()
endif()

foreach(_target ${UQM_VENDORED_TARGETS})
	target_compile_options(${_target} PRIVATE ${_uqm_quiet})
endforeach()

if(UQM_WERROR AND UQM_LEGACY)
	target_compile_options(uqm PRIVATE ${_uqm_werror})
endif()

if(UQM_LEGACY)
	add_subdirectory(sc2/src)
endif()

# The rewrite's own PNG decode, independent of sc2/ (third_party/README.md).
add_subdirectory(third_party/miniz)
add_subdirectory(third_party/spng)
add_subdirectory(src)

# --- Tests -------------------------------------------------------------------
# Host-runnable only; a cross build has nothing to run them on.

if(UQM_TESTS AND NOT CMAKE_CROSSCOMPILING)
	enable_testing()
	add_subdirectory(tests)
endif()

message(STATUS "uqm ${PROJECT_VERSION}: netplay=${UQM_NETPLAY} zip=${UQM_ZIP} "
	"openal=${UQM_OPENAL} vendored=[${UQM_VENDORED_TARGETS}]")
