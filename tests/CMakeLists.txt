# Unit tests. Deliberately tiny: this tree has no test framework and does not
# need one -- each test is a standalone program that returns non-zero on
# failure and is registered with CTest.

# The two C tests exercise sc2/src machinery, so they follow UQM_LEGACY.
if(UQM_LEGACY)
add_executable(coroutine_test coroutine_test.c)
target_link_libraries(coroutine_test PRIVATE uqm_buildsettings)
if(MSVC)
	target_compile_options(coroutine_test PRIVATE /W3)
else()
	target_compile_options(coroutine_test PRIVATE -W -Wall)
endif()
if(UQM_WERROR)
	target_compile_options(coroutine_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME coroutine COMMAND coroutine_test)

add_executable(response_test response_test.c)
target_link_libraries(response_test PRIVATE uqm_buildsettings)
if(MSVC)
	target_compile_options(response_test PRIVATE /W3)
else()
	target_compile_options(response_test PRIVATE -W -Wall)
endif()
if(UQM_WERROR)
	target_compile_options(response_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME response COMMAND response_test)
endif()

# The C++ content library. Given the content tree it sweeps every .ct and
# .ani in it, which is the point -- these formats' surprises are empirical
# (docs/content-formats.md) and curated samples would not meet them.
add_executable(content_test content_test.cpp)
target_link_libraries(content_test PRIVATE uqm2_content)
if(MSVC)
	target_compile_options(content_test PRIVATE /W4 /utf-8)
else()
	target_compile_options(content_test PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(content_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME content COMMAND content_test
	"${CMAKE_CURRENT_SOURCE_DIR}/../sc2/content")

# sim/. The RNG's golden vectors are static_asserts, so compiling this is
# most of the test -- it fails the build, not the run, if TFB_Random's uint32
# wrap is ever "corrected" into Park-Miller.
add_executable(sim_test sim_test.cpp)
target_link_libraries(sim_test PRIVATE uqm2_content)
if(MSVC)
	target_compile_options(sim_test PRIVATE /W4 /utf-8)
else()
	target_compile_options(sim_test PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(sim_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME sim COMMAND sim_test)

# The replay-similarity harness: 32 seeded battles against a checked-in
# baseline. --compare demands an exact bit-for-bit re-match; --similar (not
# wired into ctest) tolerates drift once a stage stops being bit-exact.
add_executable(replay_test replay_test.cpp)
target_link_libraries(replay_test PRIVATE uqm2_content)
if(MSVC)
	target_compile_options(replay_test PRIVATE /W4 /utf-8)
else()
	target_compile_options(replay_test PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(replay_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME replay COMMAND replay_test --compare
	"${CMAKE_CURRENT_SOURCE_DIR}/baselines/replay.base")

# engine/. Header-only so far, and deliberately checkable without a window:
# the input accumulator is the piece M1 changes behaviour with, so it needs
# tests that say what the change is.
add_executable(engine_test engine_test.cpp)
target_link_libraries(engine_test PRIVATE uqm2_content)
if(MSVC)
	target_compile_options(engine_test PRIVATE /W4 /utf-8)
else()
	target_compile_options(engine_test PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(engine_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME engine COMMAND engine_test)

# game/'s data definitions. Every resource id the ship catalog and the melee
# table name must resolve in uqm.rmp -- with browse_inventory below this
# closes the chain definition -> rmp -> file.
add_executable(game_test game_test.cpp)
target_link_libraries(game_test PRIVATE uqm2_content)
if(MSVC)
	target_compile_options(game_test PRIVATE /W4 /utf-8)
else()
	target_compile_options(game_test PRIVATE -W -Wall -Wextra)
endif()
if(UQM_WERROR)
	target_compile_options(game_test PRIVATE ${_uqm_werror})
endif()

add_test(NAME game COMMAND game_test
	"${CMAKE_CURRENT_SOURCE_DIR}/../sc2/content")

# The browser, end to end. Its inventory pass is also the resource-graph
# dangling check the plan asks for (docs/game-rewrite-plan.md, "Verification"
# 2): it fails if any resource key names a file that is not there.
add_test(NAME browse_inventory COMMAND uqm2-browse inventory
	"${CMAKE_CURRENT_SOURCE_DIR}/../sc2/content")

# Exercises the whole chain in one go -- .ani parse, PNG decode, .ct in the
# Palettes shape, colormap lookup per cel, PNG encode.
add_test(NAME browse_sheet COMMAND uqm2-browse ani
	"${CMAKE_CURRENT_SOURCE_DIR}/../sc2/content/base/comm/supox/supox.ani"
	"${CMAKE_CURRENT_BINARY_DIR}/supox-sheet.png"
	"${CMAKE_CURRENT_SOURCE_DIR}/../sc2/content/base/comm/supox/supox.ct")

add_test(NAME browse_font COMMAND uqm2-browse fon
	"${CMAKE_CURRENT_SOURCE_DIR}/../sc2/content/base/fonts/supox.fon"
	"${CMAKE_CURRENT_BINARY_DIR}/supox-font.png")
