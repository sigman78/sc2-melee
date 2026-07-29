# miniz, standing in for zlib: the vendored spng inflates through it. Built
# exactly as sc2/src/libs/vendor/miniz builds its own copy.
#
# Everything unused is compiled out: no zip reading, no gz* stdio wrappers,
# no timestamps.

add_library(uqm2_miniz STATIC)

target_sources(uqm2_miniz PRIVATE
	miniz.c
)

target_include_directories(uqm2_miniz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(uqm2_miniz PUBLIC
	MINIZ_NO_ARCHIVE_APIS
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
)
# MINIZ_NO_DEFLATE_APIS cannot join that list: spng.c is one translation unit
# holding both halves of spng, so its unused encoder still has to link against
# deflateInit2/deflate/deflateEnd/deflateBound/compress2.
