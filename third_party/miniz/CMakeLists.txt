# miniz, standing in for zlib: the vendored spng inflates through it.
#
# Everything unused is compiled out: no zip reading, no gz* stdio wrappers,
# no timestamps.

add_library(sc2m_miniz STATIC)

target_sources(sc2m_miniz PRIVATE
	miniz.c
)

target_include_directories(sc2m_miniz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(sc2m_miniz PUBLIC
	MINIZ_NO_ARCHIVE_APIS
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
)
# MINIZ_NO_DEFLATE_APIS cannot join that list: spng.c is one translation unit
# holding both halves of spng, so its unused encoder still has to link against
# deflateInit2/deflate/deflateEnd/deflateBound/compress2.
