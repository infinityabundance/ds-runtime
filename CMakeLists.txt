# ------------------------------------------------------------
# Minimum CMake version required to configure this project.
# 3.16+ is widely available on modern Linux distributions.
# ------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

# ------------------------------------------------------------
# Project declaration.
#
# - VERSION is used for packaging and future install/export work
# - LANGUAGES explicitly limits the toolchain to C++
# ------------------------------------------------------------

project(ds-runtime
    VERSION 0.1.0
    LANGUAGES CXX
)

# ============================================================
# Build configuration options
#
# These options allow packagers, CI, and users to disable
# non-essential targets (examples, tests) while still building
# the core runtime library.
# ============================================================

option(DS_BUILD_EXAMPLES "Build ds-runtime example programs" ON)
option(DS_BUILD_TESTS "Build ds-runtime tests" OFF)
option(DS_BUILD_SHARED "Build shared ds-runtime library" ON)
option(DS_BUILD_STATIC "Build static ds-runtime library" ON)

# ============================================================
# Global C++ configuration
#
# The runtime targets C++20 and avoids compiler-specific
# language extensions for portability.
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# Compiler warnings
#
# Apply a reasonable set of warnings globally for all targets.
# These flags are intentionally conservative and should be
# warning-clean for well-formed code.
# ============================================================

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

# ============================================================
# Optional dependencies
#
# Vulkan is only required for the experimental GPU example.
# The core runtime does not depend on Vulkan.
# ============================================================

find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBURING liburing)
endif()

# ============================================================
# Core runtime library
#
# ds_runtime is the main library target providing the public
# API and implementation of the DirectStorage-style runtime.
# ============================================================

set(DS_RUNTIME_SOURCES
    src/ds_runtime.cpp
    src/ds_runtime_c.cpp
    src/ds_runtime_logging.cpp
)

if (Vulkan_FOUND)
    list(APPEND DS_RUNTIME_SOURCES src/ds_runtime_vulkan.cpp)
endif()
if (LIBURING_FOUND)
    list(APPEND DS_RUNTIME_SOURCES src/ds_runtime_uring.cpp)
endif()

if (DS_BUILD_SHARED)
    add_library(ds_runtime SHARED ${DS_RUNTIME_SOURCES})
    set_target_properties(ds_runtime PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    target_include_directories(ds_runtime
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(ds_runtime
        PUBLIC
            pthread
    )
    if (Vulkan_FOUND)
        target_link_libraries(ds_runtime PUBLIC Vulkan::Vulkan)
        target_compile_definitions(ds_runtime PUBLIC DS_RUNTIME_HAS_VULKAN)
    endif()
    if (LIBURING_FOUND)
        target_link_libraries(ds_runtime PUBLIC ${LIBURING_LIBRARIES})
        target_include_directories(ds_runtime PUBLIC ${LIBURING_INCLUDE_DIRS})
        target_compile_definitions(ds_runtime PUBLIC DS_RUNTIME_HAS_IO_URING)
    endif()
endif()

if (DS_BUILD_STATIC)
    add_library(ds_runtime_static STATIC ${DS_RUNTIME_SOURCES})
    target_include_directories(ds_runtime_static
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(ds_runtime_static
        PUBLIC
            pthread
    )
    if (Vulkan_FOUND)
        target_link_libraries(ds_runtime_static PUBLIC Vulkan::Vulkan)
        target_compile_definitions(ds_runtime_static PUBLIC DS_RUNTIME_HAS_VULKAN)
    endif()
    if (LIBURING_FOUND)
        target_link_libraries(ds_runtime_static PUBLIC ${LIBURING_LIBRARIES})
        target_include_directories(ds_runtime_static PUBLIC ${LIBURING_INCLUDE_DIRS})
        target_compile_definitions(ds_runtime_static PUBLIC DS_RUNTIME_HAS_IO_URING)
    endif()
endif()

# ============================================================
# Example programs
#
# Example targets are intentionally optional and excluded
# when DS_BUILD_EXAMPLES=OFF.
# ============================================================

if (DS_BUILD_EXAMPLES)
    add_executable(ds_demo
        examples/ds_demo_main.cpp
    )

    if (TARGET ds_runtime)
        target_link_libraries(ds_demo PRIVATE ds_runtime)
    elseif (TARGET ds_runtime_static)
        target_link_libraries(ds_demo PRIVATE ds_runtime_static)
    endif()

    if (Vulkan_FOUND)
        add_executable(vk_copy_test
            examples/vk-copy-test/vk_copy_test.cpp
        )

        if (TARGET ds_runtime)
            target_link_libraries(vk_copy_test PRIVATE ds_runtime Vulkan::Vulkan)
        elseif (TARGET ds_runtime_static)
            target_link_libraries(vk_copy_test PRIVATE ds_runtime_static Vulkan::Vulkan)
        endif()
    else()
        message(STATUS
            "Vulkan not found – vk_copy_test example will not be built"
        )
    endif()
endif()

# ============================================================
# Tests (placeholder)
#
# Tests are currently optional and disabled by default.
# ============================================================

if (DS_BUILD_TESTS)
    # add_executable(ds_runtime_tests tests/basic_queue_test.cpp)
    # target_link_libraries(ds_runtime_tests PRIVATE ds_runtime)
endif()

# ============================================================
# Installation
# ============================================================

include(GNUInstallDirs)

if (TARGET ds_runtime)
    install(TARGETS ds_runtime
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if (TARGET ds_runtime_static)
    install(TARGETS ds_runtime_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

install(FILES
    include/ds_runtime.hpp
    include/ds_runtime_c.h
    include/ds_runtime_vulkan.hpp
    include/ds_runtime_uring.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(DS_RUNTIME_PKG_LIBS_EXTRA "")
set(DS_RUNTIME_PKG_CFLAGS_EXTRA "")
if (Vulkan_FOUND)
    set(DS_RUNTIME_PKG_LIBS_EXTRA "-lvulkan")
    set(DS_RUNTIME_PKG_CFLAGS_EXTRA "-DDS_RUNTIME_HAS_VULKAN")
endif()
if (LIBURING_FOUND)
    set(DS_RUNTIME_PKG_LIBS_EXTRA "${DS_RUNTIME_PKG_LIBS_EXTRA} -luring")
    set(DS_RUNTIME_PKG_CFLAGS_EXTRA "${DS_RUNTIME_PKG_CFLAGS_EXTRA} -DDS_RUNTIME_HAS_IO_URING")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ds-runtime.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/ds-runtime.pc
    @ONLY
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ds-runtime.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
