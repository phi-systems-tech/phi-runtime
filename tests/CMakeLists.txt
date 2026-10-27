add_executable(runtime_tests runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE phi::runtime)
target_include_directories(runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME runtime_tests COMMAND runtime_tests)

add_executable(str_tests str_tests.cpp)
target_link_libraries(str_tests PRIVATE phi::runtime)
target_include_directories(str_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME str_tests COMMAND str_tests)

# Against a mosquitto started on a free port; skipped (77) where there is no
# broker binary, as in a package build.
if(TARGET phi_runtime_mqtt)
    add_executable(mqtt_tests mqtt_tests.cpp)
    target_link_libraries(mqtt_tests PRIVATE phi::runtime_mqtt)
    target_include_directories(mqtt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME mqtt_tests COMMAND mqtt_tests)
    set_tests_properties(mqtt_tests PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The same client against a broker that refuses, says nothing or goes away:
# a few hundred lines of MQTT 3.1.1 in the test, no mosquitto needed.
if(TARGET phi_runtime_mqtt)
    find_package(Threads REQUIRED)
    add_executable(mqtt_fakebroker_tests mqtt_fakebroker_tests.cpp)
    target_link_libraries(mqtt_fakebroker_tests PRIVATE phi::runtime_mqtt Threads::Threads)
    target_include_directories(mqtt_fakebroker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME mqtt_fakebroker_tests COMMAND mqtt_fakebroker_tests)
    set_tests_properties(mqtt_fakebroker_tests PROPERTIES TIMEOUT 120)
endif()
