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
