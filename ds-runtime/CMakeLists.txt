cmake_minimum_required(VERSION 3.16)
project(ds_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Library
add_library(ds_runtime
    src/ds_runtime.cpp
)

target_include_directories(ds_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(ds_runtime
    PRIVATE
        -Wall -Wextra -Wpedantic
)

# Example executable
add_executable(ds_demo
    examples/ds_demo_main.cpp
)

target_link_libraries(ds_demo
    PRIVATE
        ds_runtime
        pthread
)
