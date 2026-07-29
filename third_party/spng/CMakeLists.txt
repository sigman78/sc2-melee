# spng, built exactly as sc2/src/libs/vendor/spng builds its own copy, so the
# two trees decode PNG identically.

add_library(uqm2_spng STATIC)

target_sources(uqm2_spng PRIVATE
	spng.c
)

target_include_directories(uqm2_spng PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Without this spng.h decorates the API with __declspec(dllimport) on
# Windows, which nothing in a static library can satisfy.
target_compile_definitions(uqm2_spng PUBLIC SPNG_STATIC)

# Take the zlib API (inflate, crc32) from <miniz.h> rather than <zlib.h>,
# which is why no zlib is fetched. It also skips inflateValidate(), the one
# call miniz has no equivalent for.
target_compile_definitions(uqm2_spng PRIVATE SPNG_USE_MINIZ)
target_link_libraries(uqm2_spng PRIVATE uqm2_miniz)

if(EMSCRIPTEN)
	# wasm is neither x86 nor arm, so spng.c disables its SIMD defilters on
	# its own -- but noisily, with a #pragma message per build.
	target_compile_definitions(uqm2_spng PRIVATE SPNG_DISABLE_OPT)
endif()
