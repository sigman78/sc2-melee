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
# Everything outside this block talks to the `sc2m::SDL` target, so the pin
# lives in exactly one place.
set(SDL_TEST_LIBRARY OFF)
set(SDL_INSTALL OFF)
FetchContent_Declare(SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG release-3.4.12
	GIT_SHALLOW TRUE
	FIND_PACKAGE_ARGS NAMES SDL3 CONFIG
)
FetchContent_MakeAvailable(SDL3)

add_library(sc2m_sdl INTERFACE)
target_link_libraries(sc2m_sdl INTERFACE SDL3::SDL3)
add_library(sc2m::SDL ALIAS sc2m_sdl)

# --- EnTT --------------------------------------------------------------------
# The rewrite's entity storage (src/docs/review-004-entt.md). Header-only;
# the interface target exists so nothing else names EnTT::EnTT directly and
# the pin lives in exactly one place.
set(ENTT_INSTALL OFF)
FetchContent_Declare(EnTT
	GIT_REPOSITORY https://github.com/skypjack/entt.git
	GIT_TAG v3.16.0
	GIT_SHALLOW TRUE
	FIND_PACKAGE_ARGS NAMES EnTT CONFIG
)
FetchContent_MakeAvailable(EnTT)

add_library(sc2m_entt INTERFACE)
target_link_libraries(sc2m_entt INTERFACE EnTT::EnTT)
add_library(sc2m::entt ALIAS sc2m_entt)

# --- zlib / libpng -----------------------------------------------------------
# Nothing to fetch: PNG decoding goes through the vendored spng, built with
# SPNG_USE_MINIZ, so neither libpng nor zlib is needed (third_party/README.md).

# --- Ogg / Vorbis ------------------------------------------------------------
# Nothing to fetch: libogg + libvorbis + libvorbisfile were replaced by the
# vendored single-file stb_vorbis (docs/plan.md Phase 5).
