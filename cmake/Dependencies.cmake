# Find-or-fetch dependency resolution (docs/c-making.md §5).
#
# Every dependency is declared with FetchContent's FIND_PACKAGE_ARGS, so the
# system/vcpkg/distro package wins when it exists and a pinned source tarball
# is built otherwise. That gives zero-setup builds on a bare Windows box while
# staying friendly to distro packaging, which sets
# FETCHCONTENT_FULLY_DISCONNECTED and gets an error instead of a download.

include(FetchContent)

# Several pinned upstreams still declare cmake_minimum_required(VERSION <3.5),
# which CMake 4 rejects outright. This only relaxes the floor for the fetched
# subprojects; our own tree requires 3.24.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)

# --- SDL ---------------------------------------------------------------------
# The Phase 0 bootstrap (_UQM_SDL_BOOTSTRAP2, docs/c-making.md §6) is gone:
# the tree is SDL3 now. Everything outside this block still talks to the
# version-agnostic `uqm::SDL` target.
set(SDL_TEST_LIBRARY OFF)
if(EMSCRIPTEN AND UQM_LEGACY)
	# SDL3 defaults SDL_PTHREADS off for Emscripten because SharedArrayBuffer
	# is not universally available; without it SDL_CreateSemaphore() returns
	# NULL and UQM aborts initialising the draw-command queue. We already
	# require SAB while Starcon2Main exists, so turn it back on.
	#
	# docs/unthread.md §7.6 is what removes this, along with the COOP/COEP
	# headers the host currently has to send.
	set(SDL_PTHREADS ON)
endif()
set(SDL_INSTALL OFF)
FetchContent_Declare(SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG release-3.4.12
	GIT_SHALLOW TRUE
	FIND_PACKAGE_ARGS NAMES SDL3 CONFIG
)
FetchContent_MakeAvailable(SDL3)

add_library(uqm_sdl INTERFACE)
target_link_libraries(uqm_sdl INTERFACE SDL3::SDL3)
add_library(uqm::SDL ALIAS uqm_sdl)

# --- zlib / libpng -----------------------------------------------------------
# Nothing to fetch. zlib went first: uio's zip backend inflates through the
# vendored miniz in sc2/src/libs/vendor/miniz. libpng followed once png2sdl.c
# moved to the vendored spng in sc2/src/libs/vendor/spng, which is built with
# SPNG_USE_MINIZ and so needs no zlib either (docs/plan.md Phase 5).

# --- Ogg / Vorbis ------------------------------------------------------------
# Nothing to fetch: libogg + libvorbis + libvorbisfile were replaced by the
# vendored single-file stb_vorbis in sc2/src/libs/stb (docs/plan.md Phase 5).

# --- OpenAL (optional) -------------------------------------------------------
if(UQM_OPENAL)
	set(ALSOFT_UTILS OFF)
	set(ALSOFT_EXAMPLES OFF)
	set(ALSOFT_INSTALL OFF)
	FetchContent_Declare(OpenAL
		GIT_REPOSITORY https://github.com/kcat/openal-soft.git
		GIT_TAG 1.24.3
		GIT_SHALLOW TRUE
		FIND_PACKAGE_ARGS NAMES OpenAL CONFIG
	)
	FetchContent_MakeAvailable(OpenAL)
	if(NOT TARGET OpenAL::OpenAL AND TARGET OpenAL)
		add_library(OpenAL::OpenAL ALIAS OpenAL)
	endif()
endif()
