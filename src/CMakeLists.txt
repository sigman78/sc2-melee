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
	engine/content/FontDir.cpp
	engine/content/PhraseFile.cpp
	engine/content/PngImage.cpp
	engine/content/ResourceMap.cpp
	platform/File.cpp
)

target_include_directories(uqm2_content PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The same vendored spng sc2/ uses, so both trees decode content identically.
# PRIVATE: spng.h is an implementation detail of PngImage.cpp and does not
# leak into the library's own headers.
target_link_libraries(uqm2_content PRIVATE uqm_spng)

# C++23 for std::expected and std::byteswap, both of which remove code rather
# than add it (docs/cpp-conventions.md rules 1 and 2): expected replaces the
# optional-plus-out-parameter idiom at every parser boundary, and byteswap
# replaces the hand-written shift chains with an intrinsic. Verified on GCC
# 15; the CI matrix is the first exercise of MSVC and Apple clang.
target_compile_features(uqm2_content PUBLIC cxx_std_23)

if(MSVC)
	target_compile_options(uqm2_content PRIVATE /W4 /utf-8)
else()
	target_compile_options(uqm2_content PRIVATE -W -Wall -Wextra)
endif()

if(UQM_WERROR)
	target_compile_options(uqm2_content PRIVATE ${_uqm_werror})
endif()

# The content browser. A CLI that writes PNG contact sheets rather than a
# window: there is no platform layer yet, it runs headless and in CI, and its
# output is a file you can attach to a bug report.
add_executable(uqm2-browse app/browse/main.cpp)
target_link_libraries(uqm2-browse PRIVATE uqm2_content)
if(MSVC)
	target_compile_options(uqm2-browse PRIVATE /W4 /utf-8)
else()
	target_compile_options(uqm2-browse PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(uqm2-browse PRIVATE ${_uqm_werror})
endif()
