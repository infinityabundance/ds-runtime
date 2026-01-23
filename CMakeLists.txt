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
#
# This block is limited to GCC and Clang to avoid breaking
# other toolchains.
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


# ============================================================
# Core runtime library
#
# ds_runtime is the main library target providing the public
# API and implementation of the DirectStorage-style runtime.
# ============================================================

add_library(ds_runtime
    src/ds_runtime.cpp
)


# ------------------------------------------------------------
# Public include directory:
#
# - Marked PUBLIC so consumers automatically inherit it
# - Allows future installation/export without refactoring
# ------------------------------------------------------------


target_include_directories(ds_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ============================================================
# Example programs
#
# Example targets are intentionally optional and excluded
# when DS_BUILD_EXAMPLES=OFF.
#
# These are demonstrations and experiments, not part of the
# core runtime library.
# ============================================================

if (DS_BUILD_EXAMPLES)

    # --------------------------------------------------------
    # CPU-only demo
    #
    # Demonstrates:
    # - ds::Queue usage
    # - asynchronous request submission
    # - CPU backend behavior
    # --------------------------------------------------------
    add_executable(ds_demo
        examples/ds_demo_main.cpp
    )

    # --------------------------------------------------------
    # Link against the runtime library.
    # pthread is linked explicitly for std::thread usage on
    # POSIX platforms.
    # --------------------------------------------------------
    target_link_libraries(ds_demo
        PRIVATE
            ds_runtime
            pthread
    )

    # --------------------------------------------------------
    # Experimental Vulkan copy demo
    #
    # This target is only built if Vulkan is available.
    # It serves as groundwork for future GPU backends and
    # does not represent a full DirectStorage implementation.
    # --------------------------------------------------------
    if (Vulkan_FOUND)
        add_executable(vk_copy_test
            examples/vk-copy-test/vk_copy_test.cpp
        )

        target_link_libraries(vk_copy_test
            PRIVATE
                ds_runtime
                Vulkan::Vulkan
        )
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
# This block exists to make future test integration trivial
# without restructuring the build.
# ============================================================

if (DS_BUILD_TESTS)
    # add_executable(ds_runtime_tests tests/basic_queue_test.cpp)
    # target_link_libraries(ds_runtime_tests PRIVATE ds_runtime pthread)
endif()