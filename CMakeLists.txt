cmake_minimum_required(VERSION 3.24)

# The Ur-Quan Masters, rewritten in C++23. Extracted from the sc2-uqm tree;
# the original C game and the content it reads live there still, and this
# builds against neither (see README.md and third_party/README.md).

project(sc2m
	VERSION 0.1.0
	DESCRIPTION "The Ur-Quan Masters melee, rewritten"
	LANGUAGES C CXX
)

option(SC2M_TESTS "Build the test programs" ON)
option(SC2M_WERROR "Treat warnings as errors" OFF)

# The content tree the tools and content tests read. The only thing this
# project needs from outside itself, and it is optional: the content, game
# and browse_* tests register only when it exists, while sim, replay and
# engine never touch it. Defaults to a sibling sc2-uqm checkout.
set(UQM2_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sc2-uqm/sc2/content"
	CACHE PATH "UQM content tree for the content-dependent tests and tools")

if(MSVC)
	set(_sc2m_quiet /w)
	set(_sc2m_werror /WX)
else()
	set(_sc2m_quiet -w)
	set(_sc2m_werror -Werror)
endif()

include(cmake/Dependencies.cmake)

# Vendored, and compiled quietly: third-party sources are not ours to warn
# about.
add_subdirectory(third_party/miniz)
add_subdirectory(third_party/spng)
foreach(_target sc2m_miniz sc2m_spng)
	target_compile_options(${_target} PRIVATE ${_sc2m_quiet})
endforeach()

add_subdirectory(src)

# Host-runnable only; a cross build has nothing to run them on.
if(SC2M_TESTS AND NOT CMAKE_CROSSCOMPILING)
	enable_testing()
	add_subdirectory(tests)
endif()

message(STATUS "sc2m ${PROJECT_VERSION}: content=${UQM2_CONTENT_DIR}")
