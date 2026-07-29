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
	engine/content/Sprite.cpp
	platform/File.cpp
	# Platform.cpp is deliberately NOT here: it pulls in SDL, and everything
	# that links this library today is a headless test.
	sim/Battle.cpp
	sim/Collision.cpp
	sim/Damage.cpp
	sim/Field.cpp
	sim/Gravity.cpp
	sim/Impulse.cpp
	sim/ShipSystems.cpp
	sim/ships/Human.cpp
	sim/ships/Ilwrath.cpp
	sim/Spawn.cpp
	sim/Targeting.cpp
	sim/Thrust.cpp
	sim/Velocity.cpp
	# game/'s headless half: pure data (the ship catalog), no SDL. Resources
	# and SpriteSet stay in uqm2_platform, which owns the window and device.
	game/Ships.cpp
)

target_include_directories(uqm2_content PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# PRIVATE: spng.h is an implementation detail of PngImage.cpp and does not
# leak into the library's own headers.
target_link_libraries(uqm2_content PRIVATE uqm2_spng)

# PUBLIC, unlike spng: EntityId IS entt::entity and the registry sits in
# sim/ headers -- the storage experiment, src/docs/review-004-entt.md.
target_link_libraries(uqm2_content PUBLIC uqm::entt)

# C++23 for std::expected and std::byteswap, both of which remove code rather
# than add it (docs/cpp-conventions.md rules 1 and 2): expected replaces the
# optional-plus-out-parameter idiom at every parser boundary, and byteswap
# replaces the hand-written shift chains with an intrinsic. Verified on GCC
# 15; the CI matrix is the first exercise of MSVC and Apple clang.
target_compile_features(uqm2_content PUBLIC cxx_std_23)

if(MSVC)
	target_compile_options(uqm2_content PRIVATE /W4 /utf-8)
else()
	# missing-field-initializers is off because a spec is a partial
	# designated-initializer literal over meaningful defaults (sim/Ship.hpp
	# ShipSpec, review-002 §3) -- the omissions are the idiom, not a bug.
	target_compile_options(uqm2_content PRIVATE -W -Wall -Wextra
		-Wno-missing-field-initializers)
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

# The platform layer, split from uqm2_content so the headless tests never link
# SDL. Single-threaded by construction: nothing in here starts a thread, and
# the audio callback (M1.5) is the only thing that will ever run off the main
# loop -- behind a ring buffer, never touching game state.
add_library(uqm2_platform STATIC platform/Audio.cpp platform/Platform.cpp game/Materialize.cpp game/Resources.cpp game/SpriteSet.cpp)
target_link_libraries(uqm2_platform PUBLIC uqm2_content uqm::SDL)
if(MSVC)
	target_compile_options(uqm2_platform PRIVATE /W4 /utf-8)
else()
	target_compile_options(uqm2_platform PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(uqm2_platform PRIVATE ${_uqm_werror})
endif()

# The M1 vertical slice. Cruiser against Avenger on a real window, drawn as
# rectangles until M1.4 wires the sprites up.
add_executable(uqm2-melee
	app/melee/Assets.cpp
	app/melee/Draw.cpp
	app/melee/Game.cpp
	app/melee/Sound.cpp
	app/melee/main.cpp
)
target_link_libraries(uqm2-melee PRIVATE uqm2_platform)
if(MSVC)
	target_compile_options(uqm2-melee PRIVATE /W4 /utf-8)
else()
	target_compile_options(uqm2-melee PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(uqm2-melee PRIVATE ${_uqm_werror})
endif()

# A headless check that the audio path actually loads content. Not a ctest --
# it needs the content tree and an audio device -- but it answers "did the
# WAVs load" without a window, which a game with its own console cannot.
add_executable(uqm2-audioprobe app/melee/audioprobe.cpp)
target_link_libraries(uqm2-audioprobe PRIVATE uqm2_platform)

if(EMSCRIPTEN)
	# The browser build of the slice.
	#
	# Note what is *not* here: -pthread. The rewrite is single-threaded and its
	# audio goes through SDL_AudioStream, which owns its own callback, so this
	# needs no SharedArrayBuffer and the page needs no COOP/COEP headers to be
	# hosted. That is the whole reason UQM_LEGACY exists -- see the top-level
	# CMakeLists.
	#
	# Content is preloaded into MEMFS, which std::filesystem can read, so
	# findContent works unchanged. Only what the melee actually opens: the
	# resource map, the global colormap, the two ships, and the shared battle
	# and planet art. About 3 MB against the 39 MB of a full base pack --
	# streaming the rest is a real feature and belongs with the ship roster in
	# M2, not here.
	set(_melee_content "${UQM2_CONTENT_DIR}")
	target_link_options(uqm2-melee PRIVATE
		-sALLOW_MEMORY_GROWTH=1
		-sINITIAL_MEMORY=134217728    # 128 MB
		-sSTACK_SIZE=1048576
		-sEXIT_RUNTIME=0
		-sASYNCIFY=0
		"SHELL:--preload-file ${_melee_content}/uqm.rmp@/content/uqm.rmp"
		"SHELL:--preload-file ${_melee_content}/base/uqm.ct@/content/base/uqm.ct"
		"SHELL:--preload-file ${_melee_content}/base/ships/human@/content/base/ships/human"
		"SHELL:--preload-file ${_melee_content}/base/ships/ilwrath@/content/base/ships/ilwrath"
		"SHELL:--preload-file ${_melee_content}/base/battle@/content/base/battle"
		"SHELL:--preload-file ${_melee_content}/base/planets@/content/base/planets"
		"SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/melee-shell.html"
	)
	set_target_properties(uqm2-melee PROPERTIES
		OUTPUT_NAME melee
		SUFFIX ".html"
	)
endif()

# MinGW keeps libstdc++, libgcc and libwinpthread in DLLs that exist only on
# the toolchain's PATH, so a perfectly good build dies at startup with a
# missing-DLL box the moment it is launched from Explorer rather than from an
# msys2 shell. SDL3 is already a static library here, so linking the runtime
# statically makes these self-contained -- which is what a thing you hand
# someone to look at has to be.
if(MINGW)
	target_link_options(uqm2-melee PRIVATE -static)
	target_link_options(uqm2-browse PRIVATE -static)
	target_link_options(uqm2-audioprobe PRIVATE -static)
endif()
