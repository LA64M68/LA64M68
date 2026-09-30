add_executable(test_core test_core.c)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/host)
target_link_libraries(test_core PRIVATE la64m68_core la64m68_host)
# asserts must stay live even in Release/NDEBUG builds
target_compile_options(test_core PRIVATE -UNDEBUG)
add_test(NAME core_init COMMAND test_core)
