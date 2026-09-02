cmake_minimum_required(VERSION 3.21)

project(phi-runtime
    VERSION 0.1.0
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
