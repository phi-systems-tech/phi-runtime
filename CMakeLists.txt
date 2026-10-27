cmake_minimum_required(VERSION 3.21)

project(phi-runtime
    VERSION 0.2.0
    DESCRIPTION "Phi runtime: the event loop the daemons and their plugins share"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(PHI_RUNTIME_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/phi-runtime")

# One dynamic library on purpose (6e-a): a Loop& crosses plugin boundaries,
# and that is only sound when every side sees the same loop state - a static
# copy per plugin would duplicate Loop::current()'s thread-local and make
# every affinity assert lie. The SONAME is deliberately unstable while the
# API settles: every breaking cut bumps it, and dpkg-shlibdeps forces the
# consumers' rebuild.
add_library(phi_runtime SHARED
    src/loop.cpp
    src/epollloop.cpp
    src/thread.cpp
)
add_library(phi::runtime ALIAS phi_runtime)

set_target_properties(phi_runtime PROPERTIES
    EXPORT_NAME runtime
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_compile_features(phi_runtime PUBLIC cxx_std_20)

target_include_directories(phi_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

find_package(Threads REQUIRED)
target_link_libraries(phi_runtime PUBLIC Threads::Threads)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "phi-runtime is epoll-based and currently supports Linux only")
endif()

# The MQTT client (phi/runtime/mqtt/client.h), a library of its own: only
# what talks to a broker links libmosquitto, never the loop. Privately linked,
# so its consumers need neither libmosquitto's headers nor its CMake.
option(PHI_RUNTIME_BUILD_MQTT "Build libphi_runtime_mqtt (needs libmosquitto)" ON)
if(PHI_RUNTIME_BUILD_MQTT)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
    add_library(phi_runtime_mqtt SHARED src/mqtt.cpp)
    add_library(phi::runtime_mqtt ALIAS phi_runtime_mqtt)
    set_target_properties(phi_runtime_mqtt PROPERTIES
        EXPORT_NAME runtime_mqtt
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    # Its own include path, not only the loop's: an installed phi-runtime
    # found first may be one without the mqtt headers.
    target_include_directories(phi_runtime_mqtt
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(phi_runtime_mqtt PUBLIC phi_runtime PRIVATE PkgConfig::MOSQUITTO)
endif()

option(PHI_RUNTIME_BUILD_TESTS "Build phi-runtime tests" ${PROJECT_IS_TOP_LEVEL})
if(PHI_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(
    DIRECTORY include/phi/runtime
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/phi
    FILES_MATCHING PATTERN "*.h"
)

install(
    TARGETS phi_runtime
    EXPORT phi-runtimeTargets
)

if(PHI_RUNTIME_BUILD_MQTT)
    set(PHI_RUNTIME_MQTT_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/phi-runtime-mqtt")
    install(TARGETS phi_runtime_mqtt EXPORT phi-runtime-mqttTargets)
    install(
        EXPORT phi-runtime-mqttTargets
        FILE phi-runtime-mqttTargets.cmake
        NAMESPACE phi::
        DESTINATION ${PHI_RUNTIME_MQTT_CMAKE_INSTALL_DIR}
    )
    configure_package_config_file(
        cmake/phi-runtime-mqttConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/phi-runtime-mqttConfig.cmake
        INSTALL_DESTINATION ${PHI_RUNTIME_MQTT_CMAKE_INSTALL_DIR}
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/phi-runtime-mqttConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/phi-runtime-mqttConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/phi-runtime-mqttConfigVersion.cmake
        DESTINATION ${PHI_RUNTIME_MQTT_CMAKE_INSTALL_DIR}
    )
endif()

install(
    EXPORT phi-runtimeTargets
    FILE phi-runtimeTargets.cmake
    NAMESPACE phi::
    DESTINATION ${PHI_RUNTIME_CMAKE_INSTALL_DIR}
)

configure_package_config_file(
    cmake/phi-runtimeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/phi-runtimeConfig.cmake
    INSTALL_DESTINATION ${PHI_RUNTIME_CMAKE_INSTALL_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/phi-runtimeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/phi-runtimeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/phi-runtimeConfigVersion.cmake
    DESTINATION ${PHI_RUNTIME_CMAKE_INSTALL_DIR}
)
