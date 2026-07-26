# Unit tests. Deliberately tiny: this tree has no test framework and does not
# need one -- each test is a standalone program that returns non-zero on
# failure and is registered with CTest.

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
