# The green-field C++ tree (docs/game-rewrite-plan.md).
#
# Grows alongside sc2/, which keeps building and staying in CI for the whole
# project: side-by-side comparison is the only oracle the "does it feel right"
# questions have, and it costs almost nothing.
#
# The layering the plan commits to, and that CI will eventually grep for:
#
#   platform/  SDL3, filesystem, time
#   engine/    render, audio, content, input, Rng, Task<>
#   sim/       deterministic; no I/O, no wall clock, no globals
#   game/      modes, story, universe, dialogue VM, saves
#   app/       main + iterate()
#
# Only engine/content exists so far.

add_library(uqm2_content STATIC
	engine/content/AniFile.cpp
	engine/content/BinaryTable.cpp
	engine/content/ColorTable.cpp
	engine/content/ResourceMap.cpp
)

target_include_directories(uqm2_content PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_features(uqm2_content PUBLIC cxx_std_20)

if(MSVC)
	target_compile_options(uqm2_content PRIVATE /W4 /utf-8)
else()
	target_compile_options(uqm2_content PRIVATE -W -Wall -Wextra)
endif()

if(UQM_WERROR)
	target_compile_options(uqm2_content PRIVATE ${_uqm_werror})
endif()
