add_executable(runtime_tests runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE phi::runtime)
target_include_directories(runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME runtime_tests COMMAND runtime_tests)
